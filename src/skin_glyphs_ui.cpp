#include "mcpp/skin_glyphs_ui.hpp"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/string.hpp>

#include <array>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace mcpp {
namespace {

std::optional<SkinGlyph> line_role(const std::string_view value) {
    if (value == "─" || value == "━" || value == "╍") return SkinGlyph::horiz;
    if (value == "│" || value == "┃" || value == "╏") return SkinGlyph::vert;
    if (value == "┌" || value == "┏" || value == "╭") return SkinGlyph::lefttop;
    if (value == "┐" || value == "┓" || value == "╮") return SkinGlyph::righttop;
    if (value == "└" || value == "┗" || value == "╰") return SkinGlyph::leftbottom;
    if (value == "┘" || value == "┛" || value == "╯") return SkinGlyph::rightbottom;
    if (value == "┬" || value == "┳") return SkinGlyph::topmiddle;
    if (value == "┴" || value == "┻") return SkinGlyph::bottommiddle;
    if (value == "├" || value == "┣") return SkinGlyph::leftmiddle;
    if (value == "┤" || value == "┫") return SkinGlyph::rightmiddle;
    if (value == "┼" || value == "╋" || value == "╬") return SkinGlyph::cross;
    if (value == "═") return SkinGlyph::dhoriz;
    if (value == "║") return SkinGlyph::dvert;
    if (value == "╔") return SkinGlyph::dlefttop;
    if (value == "╗") return SkinGlyph::drighttop;
    if (value == "╚") return SkinGlyph::dleftbottom;
    if (value == "╝") return SkinGlyph::drightbottom;
    if (value == "╤" || value == "╦") return SkinGlyph::dtopmiddle;
    if (value == "╧" || value == "╩") return SkinGlyph::dbottommiddle;
    if (value == "╟" || value == "╠") return SkinGlyph::dleftmiddle;
    if (value == "╢" || value == "╣") return SkinGlyph::drightmiddle;
    return {};
}

class GlyphBorder final : public ftxui::Node {
public:
    explicit GlyphBorder(ftxui::Element child) : Node({std::move(child)}) {}

    void SetBox(const ftxui::Box box) override {
        Node::SetBox(box);
        children_.front()->SetBox(box);
    }

    void Render(ftxui::Screen& screen) override {
        Node::Render(screen);
        if (box_.x_min >= box_.x_max || box_.y_min >= box_.y_max) return;
        const auto visible = ftxui::Box::Intersection(
            screen.stencil, {0, screen.dimx() - 1, 0, screen.dimy() - 1});
        const std::array corners{
            std::pair{box_.x_min, box_.y_min}, std::pair{box_.x_max, box_.y_min},
            std::pair{box_.x_min, box_.y_max}, std::pair{box_.x_max, box_.y_max},
        };
        constexpr std::array roles{
            SkinGlyph::lefttop, SkinGlyph::righttop, SkinGlyph::leftbottom, SkinGlyph::rightbottom,
        };
        constexpr std::array double_roles{
            SkinGlyph::dlefttop, SkinGlyph::drighttop, SkinGlyph::dleftbottom, SkinGlyph::drightbottom,
        };
        for (std::size_t index = 0; index < corners.size(); ++index) {
            const auto [x, y] = corners[index];
            if (!visible.Contain(x, y)) continue;
            auto& cell = screen.CellAt(x, y);
            const auto role = line_role(cell.character);
            // Unlike edges, FTXUI corners lack automerge metadata. Their known
            // positions remain valid even when a title replaces a nearby edge.
            if (role == roles[index] || role == double_roles[index]) cell.automerge = true;
        }
    }
};

class GlyphText final : public ftxui::Node {
public:
    explicit GlyphText(const std::string_view value) : Node({ftxui::text(value)}), widths_{0U} {
        for (const auto& glyph : ftxui::Utf8ToGlyphs(value)) {
            if (glyph == "\n") widths_.push_back(0U);
            else ++widths_.back();
        }
    }

    void SetBox(const ftxui::Box box) override {
        Node::SetBox(box);
        children_.front()->SetBox(box);
    }

    void Render(ftxui::Screen& screen) override {
        const auto visible = ftxui::Box::Intersection(
            ftxui::Box::Intersection(screen.stencil, box_),
            {0, screen.dimx() - 1, 0, screen.dimy() - 1});
        if (!visible.IsEmpty()) {
            for (int y = visible.y_min; y <= visible.y_max; ++y) {
                const auto row = static_cast<std::size_t>(y - box_.y_min);
                if (row >= widths_.size()) break;
                for (int x = visible.x_min; x <= visible.x_max; ++x) {
                    if (static_cast<std::size_t>(x - box_.x_min) >= widths_[row]) break;
                    // Text may cover an underlying frame in a title or overlay.
                    screen.CellAt(x, y).automerge = false;
                }
            }
        }
        Node::Render(screen);
    }

private:
    std::vector<std::size_t> widths_;
};

}  // namespace

ftxui::Element glyph_border(ftxui::Element border) {
    return std::make_shared<GlyphBorder>(std::move(border));
}

ftxui::Element glyph_text(const std::string_view text) {
    return std::make_shared<GlyphText>(text);
}

void apply_skin_lines(
    ftxui::Screen& screen, const SkinGlyphs& glyphs, const bool stickchars
) {
    if (!stickchars && !glyphs.has_lines()) return;
    screen.ApplyShader();
    for (int y = 0; y < screen.dimy(); ++y) {
        for (int x = 0; x < screen.dimx(); ++x) {
            auto& cell = screen.CellAt(x, y);
            if (!cell.automerge) continue;
            if (const auto role = line_role(cell.character)) {
                cell.character = skin_glyph(glyphs, *role, cell.character, stickchars);
                cell.automerge = false;
            }
        }
    }
}

}  // namespace mcpp
