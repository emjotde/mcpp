#pragma once

#include "mcpp/skin.hpp"

#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace mcpp::detail {

[[nodiscard]] inline std::string editor_simple_position(
    const std::size_t column, const std::size_t row, const std::size_t rows,
    const std::uint64_t offset, const std::uint64_t bytes,
    const unsigned int character, const bool eof, const std::size_t width
) {
    const auto percent = bytes == 0U ? 0U : static_cast<unsigned int>(
        static_cast<long double>(std::min(offset, bytes)) * 100.0L /
        static_cast<long double>(bytes));
    std::ostringstream full;
    full << ' ' << std::setw(3) << column << ' ' << std::setw(5) << row << '/' << rows
         << ' ' << std::setw(6) << offset << '/' << bytes << ' ';
    if (eof) full << "<EOF>";
    else full << std::setfill('0') << std::setw(4) << character << std::setfill(' ');
    full << ' ' << std::setw(3) << percent << '%';
    if (full.str().size() <= width) return full.str();
    auto compact = " " + std::to_string(column) + " " + std::to_string(row) + "/" +
        std::to_string(rows) + " " + std::to_string(percent) + "%";
    if (compact.size() <= width) return compact;
    compact = " " + std::to_string(column) + " " + std::to_string(percent) + "%";
    if (compact.size() <= width) return compact;
    return " " + std::to_string(percent) + "%";
}

class EditorRightMargin final : public ftxui::Node {
public:
    EditorRightMargin(
        ftxui::Element child, const std::size_t first_column, const std::size_t width,
        const std::size_t margin, const SkinPair style,
        std::vector<std::pair<std::size_t, std::size_t>> protected_columns
    )
        : Node({std::move(child)}), first_column_(first_column), width_(width),
          margin_(margin), style_(style), protected_columns_(std::move(protected_columns)) {}

    void SetBox(const ftxui::Box box) override {
        Node::SetBox(box);
        children_.front()->SetBox(box);
    }

    void Render(ftxui::Screen& screen) override {
        Node::Render(screen);
        const auto begin = margin_ > first_column_ ? margin_ - first_column_ : 0U;
        const auto width = std::min(width_, static_cast<std::size_t>(
            std::max(0, box_.x_max - box_.x_min + 1)));
        for (auto column = begin; column < width; ++column) {
            const auto document_column = first_column_ + column;
            if (std::ranges::any_of(protected_columns_, [document_column](const auto& range) {
                    return document_column >= range.first && document_column < range.second;
                })) continue;
            for (int row = box_.y_min; row <= box_.y_max; ++row) {
                auto& cell = screen.CellAt(box_.x_min + static_cast<int>(column), row);
                cell.foreground_color = style_.foreground;
                cell.background_color = style_.background;
                if (!style_.explicit_style) continue;
                cell.bold = (style_.attributes & SkinPair::bold) != 0U;
                cell.italic = (style_.attributes & SkinPair::italic) != 0U;
                cell.underlined = (style_.attributes & SkinPair::underline) != 0U;
                cell.inverted = (style_.attributes & SkinPair::reverse) != 0U;
                cell.blink = (style_.attributes & SkinPair::blink) != 0U;
                cell.dim = false;
                cell.underlined_double = false;
                cell.strikethrough = false;
            }
        }
    }

private:
    std::size_t first_column_;
    std::size_t width_;
    std::size_t margin_;
    SkinPair style_;
    std::vector<std::pair<std::size_t, std::size_t>> protected_columns_;
};

[[nodiscard]] inline ftxui::Element editor_right_margin(
    ftxui::Element element, const std::size_t first_column, const std::size_t width,
    const std::size_t margin, const SkinPair& style,
    std::vector<std::pair<std::size_t, std::size_t>> protected_columns = {}
) {
    const auto display_width = static_cast<int>(std::min(
        width, static_cast<std::size_t>(std::numeric_limits<int>::max())));
    return std::make_shared<EditorRightMargin>(
        std::move(element) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, display_width),
        first_column, width, margin, style, std::move(protected_columns));
}

}  // namespace mcpp::detail
