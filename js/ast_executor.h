// SPDX-FileCopyrightText: 2022-2023 Robin Lindén <dev@robinlinden.eu>
//
// SPDX-License-Identifier: BSD-2-Clause

#ifndef JS_AST_EXECUTOR_H_
#define JS_AST_EXECUTOR_H_

#include "js/ast.h"

#include <cassert>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace js::ast {

class AstExecutor {
public:
    Value execute(auto const &ast) { return (*this)(ast); }

    Value operator()(Literal const &v) { return std::visit(*this, v); }
    Value operator()(NumericLiteral const &v) { return Value{v.value}; }
    Value operator()(StringLiteral const &v) { return Value{v.value}; }
    Value operator()(Expression const &v) { return std::visit(*this, v); }
    Value operator()(Identifier const &v) { return Value{v.name}; }
    Value operator()(Pattern const &v) { return std::visit(*this, v); }
    Value operator()(Declaration const &v) { return std::visit(*this, v); }
    Value operator()(Statement const &v) { return std::visit(*this, v); }

    Value operator()(ExpressionStatement const &v) { return execute(v.expression); }

    Value operator()(BinaryExpression const &v) {
        // TODO(robinlinden): This should be done in a more generic fashion.
        auto get_value_resolving_variables = [this](Expression const &expr) {
            if (std::holds_alternative<Identifier>(expr)) {
                return variables.at(execute(expr).as_string());
            }

            return execute(expr);
        };

        auto lhs = get_value_resolving_variables(*v.lhs);
        auto rhs = get_value_resolving_variables(*v.rhs);

        switch (v.op) {
            case BinaryOperator::Plus:
                return Value{lhs.as_number() + rhs.as_number()};
            case BinaryOperator::Minus:
                return Value{lhs.as_number() - rhs.as_number()};
        }
        std::abort();
    }

    Value operator()(VariableDeclaration const &v) {
        for (auto const &declaration : v.declarations) {
            execute(declaration);
        }

        return Value{};
    }

    Value operator()(VariableDeclarator const &v) {
        auto name = execute(v.id).as_string();
        variables[name] = v.init ? execute(*v.init) : Value{};
        return Value{};
    }

    Value operator()(FunctionDeclaration const &v) {
        variables[v.id.name] = Value{v.function};
        return Value{};
    }

    Value operator()(CallExpression const &v) {
        AstExecutor scope{*this};

        auto const &fn = variables.at(execute(*v.callee).as_string());
        assert(fn.is_function() || fn.is_native_function());

        std::vector<Value> args;
        args.reserve(v.arguments.size());
        for (auto const &arg : v.arguments) {
            args.push_back(execute(*arg));
        }

        // https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Functions/arguments
        scope.variables["arguments"] = Value{std::move(args)};
        if (fn.is_function()) {
            return scope.execute(*fn.as_function());
        }

        return scope.execute(fn.as_native_function());
    }

    Value operator()(Function const &v) {
        auto const &args = variables.at("arguments").as_vector();
        for (std::size_t i = 0; i < v.params.size(); ++i) {
            auto id = execute(v.params[i]).as_string();
            variables[std::move(id)] = i < args.size() ? args[i] : Value{};
        }

        return execute(v.body);
    }

    Value operator()(BlockStatement const &) {
        // TODO(robinlinden): Implement.
        return Value{};
    }

    Value operator()(FunctionBody const &v) {
        for (auto const &statement : v.body) {
            execute(statement);
            if (returning) {
                return *std::exchange(returning, std::nullopt);
            }
        }

        return Value{};
    }

    Value operator()(ReturnStatement const &v) {
        returning = v.argument ? execute(*v.argument) : Value{};
        return Value{};
    }

    Value operator()(IfStatement const &v) {
        if (execute(v.test).as_bool()) {
            return execute(*v.if_branch);
        }

        return v.else_branch ? execute(**v.else_branch) : Value{};
    }

    Value operator()(NativeFunction const &v) { return v.f(variables.at("arguments").as_vector()); }

    Value operator()(EmptyStatement const &) { return Value{}; }

    Value operator()(WhileStatement const &v) {
        while (execute(v.test).as_bool()) {
            execute(*v.body);
        }

        return Value{};
    }

    std::map<std::string, Value, std::less<>> variables;
    std::optional<Value> returning;
};

} // namespace js::ast

#endif
