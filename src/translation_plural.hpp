#pragma once

#include "mcpp/translation.hpp"

#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace mcpp::detail {

enum class PluralError {
    none,
    syntax,
    limit,
    overflow,
    division_by_zero,
    index,
};

struct PluralValue {
    std::uint64_t value{};
    PluralError error{};
};

class PluralExpression {
    enum class Op {
        number, count, logical_not, multiply, divide, remainder, add, subtract,
        less, less_equal, greater, greater_equal, equal, not_equal,
        logical_and, logical_or, conditional,
    };

    struct Node {
        Op op;
        std::size_t left{};
        std::size_t right{};
        std::size_t third{};
        std::uint64_t number{};
        std::size_t height{1U};
    };

    class Parser {
    public:
        explicit Parser(const std::string_view expression) : remaining_(expression) {}

        bool parse(PluralExpression& result) {
            expression_ = &result;
            const auto root = conditional(1U);
            whitespace();
            if (!root || !remaining_.empty()) {
                if (error_ == PluralError::none) error_ = PluralError::syntax;
                return false;
            }
            result.root_ = *root;
            return true;
        }

        [[nodiscard]] PluralError error() const { return error_; }

    private:
        using Index = std::optional<std::size_t>;

        void whitespace() {
            while (!remaining_.empty() &&
                   (remaining_.front() == ' ' || remaining_.front() == '\t' ||
                    remaining_.front() == '\r')) remaining_.remove_prefix(1U);
        }

        bool take(const std::string_view token) {
            whitespace();
            if (!remaining_.starts_with(token)) return false;
            remaining_.remove_prefix(token.size());
            return true;
        }

        Index fail(const PluralError error) {
            if (error_ == PluralError::none) error_ = error;
            return std::nullopt;
        }

        Index node(
            const Op op, const Index left = {}, const Index right = {},
            const Index third = {}, const std::uint64_t number = 0U
        ) {
            if (error_ != PluralError::none) return std::nullopt;
            if (expression_->nodes_.size() == UiTranslations::max_plural_expression_nodes) {
                return fail(PluralError::limit);
            }
            std::size_t height = 1U;
            for (const auto child : {left, right, third}) {
                if (child) {
                    const auto child_height = expression_->nodes_[*child].height + 1U;
                    if (child_height > height) height = child_height;
                }
            }
            if (height > UiTranslations::max_plural_expression_depth) {
                return fail(PluralError::limit);
            }
            expression_->nodes_.push_back({
                op, left.value_or(0U), right.value_or(0U), third.value_or(0U), number, height,
            });
            return expression_->nodes_.size() - 1U;
        }

        Index conditional(const std::size_t depth) {
            if (depth > UiTranslations::max_plural_expression_depth) return fail(PluralError::limit);
            const auto condition = logical_or(depth);
            if (!condition || !take("?")) return condition;
            const auto yes = conditional(depth + 1U);
            if (!yes || !take(":")) return fail(PluralError::syntax);
            const auto no = conditional(depth + 1U);
            if (!no) return std::nullopt;
            return node(Op::conditional, condition, yes, no);
        }

        Index logical_or(const std::size_t depth) {
            auto left = logical_and(depth);
            while (left && take("||")) {
                const auto right = logical_and(depth);
                if (!right) return std::nullopt;
                left = node(Op::logical_or, left, right);
            }
            return left;
        }

        Index logical_and(const std::size_t depth) {
            auto left = equality(depth);
            while (left && take("&&")) {
                const auto right = equality(depth);
                if (!right) return std::nullopt;
                left = node(Op::logical_and, left, right);
            }
            return left;
        }

        Index equality(const std::size_t depth) {
            auto left = relation(depth);
            while (left) {
                Op op;
                if (take("==")) op = Op::equal;
                else if (take("!=")) op = Op::not_equal;
                else break;
                const auto right = relation(depth);
                if (!right) return std::nullopt;
                left = node(op, left, right);
            }
            return left;
        }

        Index relation(const std::size_t depth) {
            auto left = sum(depth);
            while (left) {
                Op op;
                if (take("<=")) op = Op::less_equal;
                else if (take(">=")) op = Op::greater_equal;
                else if (take("<")) op = Op::less;
                else if (take(">")) op = Op::greater;
                else break;
                const auto right = sum(depth);
                if (!right) return std::nullopt;
                left = node(op, left, right);
            }
            return left;
        }

        Index sum(const std::size_t depth) {
            auto left = product(depth);
            while (left) {
                Op op;
                if (take("+")) op = Op::add;
                else if (take("-")) op = Op::subtract;
                else break;
                const auto right = product(depth);
                if (!right) return std::nullopt;
                left = node(op, left, right);
            }
            return left;
        }

        Index product(const std::size_t depth) {
            auto left = unary(depth);
            while (left) {
                Op op;
                if (take("*")) op = Op::multiply;
                else if (take("/")) op = Op::divide;
                else if (take("%")) op = Op::remainder;
                else break;
                const auto right = unary(depth);
                if (!right) return std::nullopt;
                left = node(op, left, right);
            }
            return left;
        }

        Index unary(const std::size_t depth) {
            if (depth > UiTranslations::max_plural_expression_depth) return fail(PluralError::limit);
            if (take("!")) {
                const auto operand = unary(depth + 1U);
                return operand ? node(Op::logical_not, operand) : std::nullopt;
            }
            if (take("(")) {
                const auto value = conditional(depth + 1U);
                if (!value || !take(")")) return fail(PluralError::syntax);
                return value;
            }
            if (take("n")) {
                expression_->uses_count_ = true;
                return node(Op::count);
            }
            whitespace();
            if (remaining_.empty() || remaining_.front() < '0' || remaining_.front() > '9') {
                return fail(PluralError::syntax);
            }
            std::uint64_t number = 0U;
            while (!remaining_.empty() && remaining_.front() >= '0' && remaining_.front() <= '9') {
                const auto digit = static_cast<std::uint64_t>(remaining_.front() - '0');
                if (number > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
                    return fail(PluralError::overflow);
                }
                number = number * 10U + digit;
                remaining_.remove_prefix(1U);
            }
            return node(Op::number, {}, {}, {}, number);
        }

        std::string_view remaining_;
        PluralExpression* expression_{};
        PluralError error_{};
    };

public:
    [[nodiscard]] static std::optional<PluralExpression> parse(
        const std::string_view expression, PluralError& error
    ) {
        error = PluralError::none;
        if (expression.size() > UiTranslations::max_plural_expression_bytes) {
            error = PluralError::limit;
            return std::nullopt;
        }
        PluralExpression result;
        Parser parser(expression);
        if (!parser.parse(result)) {
            error = parser.error();
            return std::nullopt;
        }
        return result;
    }

    [[nodiscard]] PluralValue evaluate(const std::uint64_t count, const std::size_t forms) const {
        std::size_t steps = 0U;
        auto value = evaluate_node(root_, count, 1U, steps);
        if (value.error == PluralError::none && value.value >= forms) value.error = PluralError::index;
        return value;
    }

    [[nodiscard]] bool uses_count() const { return uses_count_; }

private:
    [[nodiscard]] PluralValue evaluate_node(
        const std::size_t index, const std::uint64_t count, const std::size_t depth,
        std::size_t& steps
    ) const {
        if (index >= nodes_.size() || depth > UiTranslations::max_plural_expression_depth ||
            ++steps > UiTranslations::max_plural_expression_nodes) return {0U, PluralError::limit};
        const auto& current = nodes_[index];
        if (current.op == Op::number) return {current.number, PluralError::none};
        if (current.op == Op::count) return {count, PluralError::none};
        const auto left = evaluate_node(current.left, count, depth + 1U, steps);
        if (left.error != PluralError::none) return left;
        if (current.op == Op::logical_not) return {left.value == 0U, PluralError::none};
        if (current.op == Op::logical_and && left.value == 0U) return {0U, PluralError::none};
        if (current.op == Op::logical_or && left.value != 0U) return {1U, PluralError::none};
        if (current.op == Op::conditional) {
            return evaluate_node(
                left.value != 0U ? current.right : current.third, count, depth + 1U, steps
            );
        }
        const auto right = evaluate_node(current.right, count, depth + 1U, steps);
        if (right.error != PluralError::none) return right;
        constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
        switch (current.op) {
            case Op::multiply:
                if (right.value != 0U && left.value > maximum / right.value) {
                    return {0U, PluralError::overflow};
                }
                return {left.value * right.value, PluralError::none};
            case Op::divide:
            case Op::remainder:
                if (right.value == 0U) return {0U, PluralError::division_by_zero};
                return {current.op == Op::divide ? left.value / right.value : left.value % right.value,
                        PluralError::none};
            case Op::add:
                if (left.value > maximum - right.value) return {0U, PluralError::overflow};
                return {left.value + right.value, PluralError::none};
            case Op::subtract:
                if (left.value < right.value) return {0U, PluralError::overflow};
                return {left.value - right.value, PluralError::none};
            case Op::less: return {left.value < right.value, PluralError::none};
            case Op::less_equal: return {left.value <= right.value, PluralError::none};
            case Op::greater: return {left.value > right.value, PluralError::none};
            case Op::greater_equal: return {left.value >= right.value, PluralError::none};
            case Op::equal: return {left.value == right.value, PluralError::none};
            case Op::not_equal: return {left.value != right.value, PluralError::none};
            case Op::logical_and:
            case Op::logical_or: return {right.value != 0U, PluralError::none};
            default: return {0U, PluralError::syntax};
        }
    }

    std::vector<Node> nodes_;
    std::size_t root_{};
    bool uses_count_{};
};

}  // namespace mcpp::detail
