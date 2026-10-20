#include "viewer_help_styles.hpp"

#include "mcpp/skin_glyphs_ui.hpp"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/string.hpp>

#include <algorithm>
#include <memory>

namespace mcpp::detail {
namespace {

class SkinPaint final : public ftxui::Node {
public:
    SkinPaint(ftxui::Element child, const SkinPair colors)
        : Node({std::move(child)}), colors_(colors) {}

    void SetBox(const ftxui::Box box) override {
        Node::SetBox(box);
        children_.front()->SetBox(box);
    }

    void Render(ftxui::Screen& screen) override {
        for (int row = box_.y_min; row <= box_.y_max; ++row) {
            for (int column = box_.x_min; column <= box_.x_max; ++column) {
                auto& cell = screen.CellAt(column, row);
                cell.foreground_color = colors_.foreground;
                cell.background_color = colors_.background;
                cell.bold = (colors_.attributes & SkinPair::bold) != 0U;
                cell.italic = (colors_.attributes & SkinPair::italic) != 0U;
                cell.underlined = (colors_.attributes & SkinPair::underline) != 0U;
                cell.inverted = (colors_.attributes & SkinPair::reverse) != 0U;
                cell.blink = (colors_.attributes & SkinPair::blink) != 0U;
                cell.dim = false;
                cell.underlined_double = false;
                cell.strikethrough = false;
            }
        }
        Node::Render(screen);
    }

private:
    SkinPair colors_;
};

}  // namespace

ftxui::Element paint_skin_style(ftxui::Element element, const SkinPair& colors) {
    if (colors.explicit_style) return std::make_shared<SkinPaint>(std::move(element), colors);
    return std::move(element) | ftxui::color(colors.foreground) | ftxui::bgcolor(colors.background);
}

ftxui::Element render_viewer_text(
    const Skin& skin, const std::string& row,
    const std::optional<std::pair<std::size_t, std::size_t>> highlight,
    const std::vector<ViewerTextStyle>& styles
) {
    using namespace ftxui;
    if (!highlight && styles.empty()) return glyph_text(row);
    auto aligned = highlight;
    std::size_t offset = 0;
    for (const auto& glyph : Utf8ToGlyphs(row)) {
        const auto next = offset + glyph.size();
        if (highlight && offset <= highlight->first && highlight->first < next) {
            aligned->first = offset;
        }
        if (highlight && offset < highlight->second && highlight->second <= next) {
            aligned->second = next;
            break;
        }
        offset = next;
    }
    std::vector<std::size_t> boundaries{0, row.size()};
    if (aligned) {
        boundaries.push_back(std::min(aligned->first, row.size()));
        boundaries.push_back(std::min(aligned->second, row.size()));
    }
    for (const auto& style : styles) {
        boundaries.push_back(std::min(style.begin, row.size()));
        boundaries.push_back(std::min(style.end, row.size()));
    }
    std::ranges::sort(boundaries);
    Elements segments;
    for (std::size_t index = 1; index < boundaries.size(); ++index) {
        const auto begin = boundaries[index - 1U], end = boundaries[index];
        if (end <= begin) continue;
        auto segment = glyph_text(row.substr(begin, end - begin));
        bool is_bold = false;
        bool is_underlined = false;
        for (const auto& style : styles) {
            if (begin < style.begin || begin >= style.end) continue;
            is_bold = is_bold || style.bold;
            is_underlined = is_underlined || style.underline;
        }
        const bool matched = aligned && begin >= aligned->first && begin < aligned->second;
        // MC 4.8.30 gives underline priority when both overstrike styles apply.
        const auto* colors = is_underlined ? find_skin_color(skin, "viewer", "viewunderline") :
                             is_bold ? find_skin_color(skin, "viewer", "viewbold") : nullptr;
        if (colors && !matched) {
            segment = paint_skin_style(std::move(segment), *colors);
        } else {
            if (is_bold) segment = segment | bold;
            if (is_underlined) segment = segment | underlined;
        }
        if (matched) segment = segment | inverted | bold;
        segments.push_back(std::move(segment));
    }
    return hbox(std::move(segments));
}

void append_viewer_eof_rows(
    std::vector<ftxui::Element>& rows, const std::string_view display_marker,
    const ViewerModel& viewer, const std::size_t height
) {
    if (display_marker.empty() || !viewer.open() || viewer.hex() ||
        viewer.view_end() != viewer.size() || !viewer.source_error().empty()) return;
    while (rows.size() < height) rows.push_back(glyph_text(display_marker));
}

ftxui::Element help_text_style(
    ftxui::Element element, const Skin& skin, const HelpTextStyle style
) {
    const auto* colors = find_skin_color(
        skin, "help", style == HelpTextStyle::bold ? "helpbold" : "helpitalic");
    if (colors) return paint_skin_style(std::move(element), *colors);
    return style == HelpTextStyle::bold ? std::move(element) | ftxui::bold :
                                         std::move(element) | ftxui::italic;
}

}  // namespace mcpp::detail
