// SPDX-FileCopyrightText: 2021-2023 Robin Lindén <dev@robinlinden.eu>
// SPDX-FileCopyrightText: 2022 Mikael Larsson <c.mikael.larsson@gmail.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include "layout/layout.h"

#include "util/from_chars.h"
#include "util/overloaded.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <map>
#include <optional>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>

using namespace std::literals;

namespace layout {
namespace {

bool last_node_was_anonymous(LayoutBox const &box) {
    return !box.children.empty() && box.children.back().type == LayoutType::AnonymousBlock;
}

// https://www.w3.org/TR/CSS2/visuren.html#box-gen
std::optional<LayoutBox> create_tree(style::StyledNode const &node) {
    auto visitor = util::Overloaded{
            [&node](dom::Element const &) -> std::optional<LayoutBox> {
                auto display = node.get_property<css::PropertyId::Display>();
                if (display == style::DisplayValue::None) {
                    return std::nullopt;
                }

                LayoutBox box{&node, display == style::DisplayValue::Inline ? LayoutType::Inline : LayoutType::Block};

                for (auto const &child : node.children) {
                    auto child_box = create_tree(child);
                    if (!child_box) {
                        continue;
                    }

                    if (child_box->type == LayoutType::Inline && box.type != LayoutType::Inline) {
                        if (!last_node_was_anonymous(box)) {
                            box.children.push_back(LayoutBox{nullptr, LayoutType::AnonymousBlock});
                        }

                        box.children.back().children.push_back(std::move(*child_box));
                    } else {
                        box.children.push_back(std::move(*child_box));
                    }
                }

                return box;
            },
            [&node](dom::Text const &) -> std::optional<LayoutBox> {
                return LayoutBox{&node, LayoutType::Inline};
            },
    };

    return std::visit(visitor, node.node);
}

// TODO(robinlinden):
// * margin, border, etc.
// * Not all measurements have to be in pixels.
// * %, rem
int to_px(std::string_view property, int const font_size) {
    // Special case for 0 since it won't ever have a unit that needs to be handled.
    if (property == "0") {
        return 0;
    }

    float res{};
    auto parse_result = util::from_chars(property.data(), property.data() + property.size(), res);
    if (parse_result.ec != std::errc{}) {
        spdlog::warn("Unable to parse property '{}' in to_px", property);
        return 0;
    }

    auto const parsed_length = std::distance(property.data(), parse_result.ptr);
    auto const unit = property.substr(parsed_length);

    if (unit == "px") {
        return static_cast<int>(res);
    }

    if (unit == "em") {
        res *= static_cast<float>(font_size);
        return static_cast<int>(res);
    }

    spdlog::warn("Bad property '{}' w/ unit '{}' in to_px", property, unit);
    return static_cast<int>(res);
}

void calculate_left_and_right_margin(LayoutBox &box,
        geom::Rect const &parent,
        std::string_view margin_left,
        std::string_view margin_right,
        int const font_size) {
    if (margin_left == "auto" && margin_right == "auto") {
        int margin_px = (parent.width - box.dimensions.border_box().width) / 2;
        box.dimensions.margin.left = box.dimensions.margin.right = margin_px;
    } else if (margin_left == "auto" && margin_right != "auto") {
        box.dimensions.margin.right = to_px(margin_right, font_size);
        box.dimensions.margin.left = parent.width - box.dimensions.margin_box().width;
    } else if (margin_left != "auto" && margin_right == "auto") {
        box.dimensions.margin.left = to_px(margin_left, font_size);
        box.dimensions.margin.right = parent.width - box.dimensions.margin_box().width;
    } else {
        // TODO(mkiael): Compute margin depending on direction property
    }
}

// https://www.w3.org/TR/CSS2/visudet.html#blockwidth
void calculate_width_and_margin(LayoutBox &box, geom::Rect const &parent, int const font_size) {
    assert(box.node != nullptr);

    if (std::holds_alternative<dom::Text>(box.node->node)) {
        // TODO(robinlinden): Measure the text for real.
        auto text_node = std::get<dom::Text>(box.node->node);
        box.dimensions.content.width = std::min(parent.width, static_cast<int>(text_node.text.size()) * font_size / 2);
        return;
    }

    if (auto margin_top = box.get_property<css::PropertyId::MarginTop>()) {
        box.dimensions.margin.top = to_px(*margin_top, font_size);
    }

    if (auto margin_bottom = box.get_property<css::PropertyId::MarginBottom>()) {
        box.dimensions.margin.bottom = to_px(*margin_bottom, font_size);
    }

    auto width = box.get_property<css::PropertyId::Width>().value_or("auto");
    auto margin_left = box.get_property<css::PropertyId::MarginLeft>().value_or("0");
    auto margin_right = box.get_property<css::PropertyId::MarginRight>().value_or("0");
    if (width == "auto") {
        if (margin_left != "auto") {
            box.dimensions.margin.left = to_px(margin_left, font_size);
        }
        if (margin_right != "auto") {
            box.dimensions.margin.right = to_px(margin_right, font_size);
        }
        box.dimensions.content.width = parent.width - box.dimensions.margin_box().width;
    } else {
        box.dimensions.content.width = to_px(width, font_size);
        calculate_left_and_right_margin(box, parent, margin_left, margin_right, font_size);
    }

    if (auto min = box.get_property<css::PropertyId::MinWidth>(); min && min != "auto") {
        int min_width_px = to_px(*min, font_size);
        if (box.dimensions.content.width < min_width_px) {
            box.dimensions.content.width = min_width_px;
            calculate_left_and_right_margin(box, parent, margin_left, margin_right, font_size);
        }
    }

    if (auto max = box.get_property<css::PropertyId::MaxWidth>(); max && max != "none") {
        int max_width_px = to_px(*max, font_size);
        if (box.dimensions.content.width > max_width_px) {
            box.dimensions.content.width = max_width_px;
            calculate_left_and_right_margin(box, parent, margin_left, margin_right, font_size);
        }
    }
}

void calculate_position(LayoutBox &box, geom::Rect const &parent) {
    auto const &d = box.dimensions;
    box.dimensions.content.x = parent.x + d.padding.left + d.border.left + d.margin.left;
    // Position below previous content in parent.
    box.dimensions.content.y = parent.y + parent.height + d.border.top + d.padding.top + d.margin.top;
}

void calculate_height(LayoutBox &box, int const font_size) {
    assert(box.node != nullptr);
    if (auto const *text = std::get_if<dom::Text>(&box.node->node)) {
        int lines = static_cast<int>(std::ranges::count(text->text, '\n')) + 1;
        box.dimensions.content.height = lines * font_size;
    }

    if (auto height = box.get_property<css::PropertyId::Height>(); height && height != "auto") {
        box.dimensions.content.height = to_px(*height, font_size);
    }

    if (auto min = box.get_property<css::PropertyId::MinHeight>(); min && min != "auto") {
        box.dimensions.content.height = std::max(box.dimensions.content.height, to_px(*min, font_size));
    }

    if (auto max = box.get_property<css::PropertyId::MaxHeight>(); max && max != "none") {
        box.dimensions.content.height = std::min(box.dimensions.content.height, to_px(*max, font_size));
    }
}

void calculate_padding(LayoutBox &box, int const font_size) {
    if (auto padding_left = box.get_property<css::PropertyId::PaddingLeft>()) {
        box.dimensions.padding.left = to_px(*padding_left, font_size);
    }

    if (auto padding_right = box.get_property<css::PropertyId::PaddingRight>()) {
        box.dimensions.padding.right = to_px(*padding_right, font_size);
    }

    if (auto padding_top = box.get_property<css::PropertyId::PaddingTop>()) {
        box.dimensions.padding.top = to_px(*padding_top, font_size);
    }

    if (auto padding_bottom = box.get_property<css::PropertyId::PaddingBottom>()) {
        box.dimensions.padding.bottom = to_px(*padding_bottom, font_size);
    }
}

// https://w3c.github.io/csswg-drafts/css-backgrounds/#the-border-width
std::map<std::string_view, int> const kBorderWidthKeywords{
        {"thin", 3},
        {"medium", 5},
        {"thick", 7},
};

void calculate_border(LayoutBox &box, int const font_size) {
    std::string_view default_style = "none";
    std::string_view default_width = "medium";

    auto as_px = [&](std::string_view border_width_property) {
        if (kBorderWidthKeywords.contains(border_width_property)) {
            return kBorderWidthKeywords.at(border_width_property);
        }

        return to_px(border_width_property, font_size);
    };

    if (box.get_property<css::PropertyId::BorderLeftStyle>().value_or(default_style) != default_style) {
        auto border_width = box.get_property<css::PropertyId::BorderLeftWidth>().value_or(default_width);
        box.dimensions.border.left = as_px(border_width);
    }

    if (box.get_property<css::PropertyId::BorderRightStyle>().value_or(default_style) != default_style) {
        auto border_width = box.get_property<css::PropertyId::BorderRightWidth>().value_or(default_width);
        box.dimensions.border.right = as_px(border_width);
    }

    if (box.get_property<css::PropertyId::BorderTopStyle>().value_or(default_style) != default_style) {
        auto border_width = box.get_property<css::PropertyId::BorderTopWidth>().value_or(default_width);
        box.dimensions.border.top = as_px(border_width);
    }

    if (box.get_property<css::PropertyId::BorderBottomStyle>().value_or(default_style) != default_style) {
        auto border_width = box.get_property<css::PropertyId::BorderBottomWidth>().value_or(default_width);
        box.dimensions.border.bottom = as_px(border_width);
    }
}

void layout(LayoutBox &box, geom::Rect const &bounds) {
    switch (box.type) {
        case LayoutType::Inline:
        case LayoutType::Block: {
            assert(box.node);
            auto font_size = box.get_property<css::PropertyId::FontSize>().value();
            calculate_padding(box, font_size);
            calculate_border(box, font_size);
            calculate_width_and_margin(box, bounds, font_size);
            calculate_position(box, bounds);
            for (auto &child : box.children) {
                layout(child, box.dimensions.content);
                box.dimensions.content.height += child.dimensions.margin_box().height;
            }
            calculate_height(box, font_size);
            return;
        }
        // TODO(robinlinden): This needs to place its children side-by-side.
        // TODO(robinlinden): Children wider than the available area need to be split across multiple lines.
        case LayoutType::AnonymousBlock: {
            box.dimensions.content.width = bounds.width;
            calculate_position(box, bounds);
            for (auto &child : box.children) {
                layout(child, box.dimensions.content);
                box.dimensions.content.height += child.dimensions.margin_box().height;
            }
            return;
        }
    }
}

std::string_view to_str(LayoutType type) {
    switch (type) {
        case LayoutType::Inline:
            return "inline";
        case LayoutType::Block:
            return "block";
        case LayoutType::AnonymousBlock:
            return "ablock";
    }
    assert(false);
    std::abort();
}

std::string_view to_str(dom::Node const &node) {
    return std::visit(util::Overloaded{
                              [](dom::Element const &element) -> std::string_view { return element.name; },
                              [](dom::Text const &text) -> std::string_view { return text.text; },
                      },
            node);
}

std::string to_str(geom::Rect const &rect) {
    std::stringstream ss;
    ss << "{" << rect.x << "," << rect.y << "," << rect.width << "," << rect.height << "}";
    return std::move(ss).str();
}

std::string to_str(geom::EdgeSize const &edge) {
    std::stringstream ss;
    ss << "{" << edge.top << "," << edge.right << "," << edge.bottom << "," << edge.left << "}";
    return std::move(ss).str();
}

void print_box(LayoutBox const &box, std::ostream &os, uint8_t depth = 0) {
    for (std::uint8_t i = 0; i < depth; ++i) {
        os << "  ";
    }

    if (box.node != nullptr) {
        os << to_str(box.node->node) << '\n';
        for (std::uint8_t i = 0; i < depth; ++i) {
            os << "  ";
        }
    }

    auto const &d = box.dimensions;
    os << to_str(box.type) << " " << to_str(d.content) << " " << to_str(d.padding) << " " << to_str(d.margin) << '\n';
    for (auto const &child : box.children) {
        print_box(child, os, depth + 1);
    }
}

} // namespace

std::optional<LayoutBox> create_layout(style::StyledNode const &node, int width) {
    auto tree = create_tree(node);
    if (!tree) {
        return {};
    }

    layout(*tree, {0, 0, width, 0});
    return *tree;
}

LayoutBox const *box_at_position(LayoutBox const &box, geom::Position p) {
    if (!box.dimensions.contains(p)) {
        return nullptr;
    }

    for (auto const &child : box.children) {
        if (auto maybe = box_at_position(child, p)) {
            return maybe;
        }
    }

    if (box.type == LayoutType::AnonymousBlock) {
        return nullptr;
    }

    return &box;
}

std::string to_string(LayoutBox const &box) {
    std::stringstream ss;
    print_box(box, ss);
    return std::move(ss).str();
}

} // namespace layout
