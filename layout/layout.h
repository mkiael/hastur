// SPDX-FileCopyrightText: 2021-2023 Robin Lindén <dev@robinlinden.eu>
//
// SPDX-License-Identifier: BSD-2-Clause

#ifndef LAYOUT_LAYOUT_H_
#define LAYOUT_LAYOUT_H_

#include "layout/box_model.h"

#include "css/property_id.h"
#include "geom/geom.h"
#include "style/styled_node.h"

#include <cassert>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace layout {

enum class LayoutType {
    Inline,
    Block,
    AnonymousBlock, // Holds groups of sequential inline boxes.
};

struct LayoutBox {
    style::StyledNode const *node;
    LayoutType type;
    BoxModel dimensions;
    std::vector<LayoutBox> children;
    [[nodiscard]] bool operator==(LayoutBox const &) const = default;

    template<css::PropertyId T>
    auto get_property() const {
        using RetT = decltype(node->get_property<T>());
        if (!node) {
            return std::optional<RetT>{};
        }

        return std::optional<RetT>{node->get_property<T>()};
    }
};

std::optional<LayoutBox> create_layout(style::StyledNode const &node, int width);

LayoutBox const *box_at_position(LayoutBox const &, geom::Position);

std::string to_string(LayoutBox const &box);

inline std::string_view dom_name(LayoutBox const &node) {
    assert(node.node);
    return std::get<dom::Element>(node.node->node).name;
}

inline std::vector<LayoutBox const *> dom_children(LayoutBox const &node) {
    assert(node.node);
    std::vector<LayoutBox const *> children{};
    for (auto const &child : node.children) {
        if (child.type == LayoutType::AnonymousBlock) {
            for (auto const &inline_child : child.children) {
                assert(inline_child.node);
                if (!std::holds_alternative<dom::Element>(inline_child.node->node)) {
                    continue;
                }

                children.push_back(&inline_child);
            }
            continue;
        }

        assert(child.node);
        if (!std::holds_alternative<dom::Element>(child.node->node)) {
            continue;
        }

        children.push_back(&child);
    }
    return children;
}

} // namespace layout

#endif
