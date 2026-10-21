#include "editor_window_styles.hpp"
#include "mcpp/skin_glyphs_ui.hpp"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/terminal.hpp>

#include <array>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>

namespace {

using namespace ftxui;
using mcpp::Skin;
using mcpp::SkinPair;
using mcpp::detail::paint_skin_style;

constexpr unsigned all_attributes = SkinPair::bold | SkinPair::italic | SkinPair::underline |
    SkinPair::reverse | SkinPair::blink;
const SkinPair normal{Color::RGB(25, 35, 45), Color::RGB(5, 15, 25), all_attributes, true};
const SkinPair status{Color::RGB(225, 215, 205), Color::RGB(65, 55, 45), 0U, true};
const SkinPair active{Color::RGB(10, 230, 210), Color::RGB(90, 20, 80), 0U, true};
const SkinPair inactive{Color::RGB(220, 150, 10), Color::RGB(20, 90, 30), SkinPair::underline, true};
const SkinPair drag{Color::RGB(250, 15, 35), Color::RGB(15, 65, 95), all_attributes, true};
const SkinPair desktop{Color::RGB(120, 240, 60), Color::RGB(90, 10, 10), SkinPair::italic, true};
const std::string source = "A界é─╔ bytes";

void check(const bool condition, const std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

void pair(const Cell& cell, const SkinPair& expected) {
    const auto attributes = (cell.bold ? SkinPair::bold : 0U) |
        (cell.italic ? SkinPair::italic : 0U) |
        (cell.underlined ? SkinPair::underline : 0U) |
        (cell.inverted ? SkinPair::reverse : 0U) |
        (cell.blink ? SkinPair::blink : 0U);
    check(cell.foreground_color == expected.foreground &&
          cell.background_color == expected.background && attributes == expected.attributes &&
          !cell.dim && !cell.underlined_double && !cell.strikethrough,
          "window cells must use the exact full pair and explicit attributes");
}

void same(const Cell& before, const Cell& after) {
    check(before.character == after.character && before.automerge == after.automerge &&
          before.foreground_color == after.foreground_color &&
          before.background_color == after.background_color &&
          before.bold == after.bold && before.italic == after.italic &&
          before.underlined == after.underlined && before.inverted == after.inverted &&
          before.blink == after.blink && before.dim == after.dim &&
          before.underlined_double == after.underlined_double &&
          before.strikethrough == after.strikethrough,
          "styles and literal glyph metadata outside a role must remain unchanged");
}

Skin skin() {
    Skin result;
    result.normal = normal;
    result.menu = status;
    result.colors["editor"]["editframeactive"] = active;
    result.colors["editor"]["editframe"] = inactive;
    result.colors["editor"]["editframedrag"] = drag;
    result.colors["editor"]["editbg"] = desktop;
    return result;
}

Element body() {
    return hbox({
        paint_skin_style(mcpp::glyph_text(source), {Color::White, Color::Blue, SkinPair::italic, true}),
        paint_skin_style(mcpp::glyph_text("$"), {Color::Black, Color::Cyan, SkinPair::underline, true}),
        paint_skin_style(mcpp::glyph_text("Z") | focusCursorBlock,
                         {Color::Black, Color::White, SkinPair::bold, true}),
    });
}

Element window(
    const Skin& colors, const bool focused, const bool dragging, const bool styled,
    std::array<Box, 2>& buttons
) {
    auto border = mcpp::glyph_border(body() | borderStyled(LIGHT));
    if (styled) {
        border = mcpp::detail::editor_window_frame(std::move(border), colors, focused, dragging);
    }
    auto controls = hbox({
        mcpp::glyph_text("[*]") | reflect(buttons[0]),
        mcpp::glyph_text("[X]") | reflect(buttons[1]),
    });
    controls = styled
        ? mcpp::detail::editor_window_controls(std::move(controls), colors, focused, dragging)
        : paint_skin_style(std::move(controls), colors.menu);
    return dbox({
        std::move(border),
        vbox({
            hbox({
                filler() | size(WIDTH, EQUAL, 1),
                paint_skin_style(mcpp::glyph_text("status filename"), status) |
                    size(WIDTH, EQUAL, 22),
                std::move(controls),
                filler() | size(WIDTH, EQUAL, 1),
            }),
            filler(),
        }),
    }) | size(WIDTH, EQUAL, 30) | size(HEIGHT, EQUAL, 8);
}

Screen render_window(
    const Skin& colors, const bool focused, const bool dragging, const bool styled,
    std::array<Box, 2>& buttons
) {
    auto screen = Screen::Create(Dimension::Fixed(30), Dimension::Fixed(8));
    Render(screen, paint_skin_style(window(colors, focused, dragging, styled, buttons), normal));
    return screen;
}

void frames() {
    const auto colors = skin();
    std::array<Box, 2> original_buttons{};
    const auto original = render_window(colors, true, false, false, original_buttons);
    for (const auto& [focused, dragging] :
         {std::pair{true, false}, std::pair{false, false}, std::pair{true, true},
          std::pair{false, true}}) {
        std::array<Box, 2> buttons{};
        const auto screen = render_window(colors, focused, dragging, true, buttons);
        const auto& frame = dragging ? drag : focused ? active : inactive;
        for (int row = 0; row < 8; ++row) {
            for (int column = 0; column < 30; ++column) {
                const auto& cell = screen.CellAt(column, row);
                check(cell.character == original.CellAt(column, row).character &&
                      cell.automerge == original.CellAt(column, row).automerge,
                      "frame colors must not replace glyphs or their line metadata");
                if (row == 0 && column >= 1 && column <= 22) {
                    same(original.CellAt(column, row), cell);
                } else if (row == 0 || row == 7 || column == 0 || column == 29) {
                    pair(cell, !dragging && row == 7 && column == 29 ? drag : frame);
                } else {
                    same(original.CellAt(column, row), cell);
                }
            }
        }
        check(screen.cursor().x == original.cursor().x && screen.cursor().y == original.cursor().y,
              "frame styles must not move the document cursor");
        for (std::size_t index = 0; index < buttons.size(); ++index) {
            check(buttons[index].x_min == original_buttons[index].x_min &&
                  buttons[index].x_max == original_buttons[index].x_max &&
                  buttons[index].y_min == original_buttons[index].y_min &&
                  buttons[index].y_max == original_buttons[index].y_max,
                  "existing window button hit boxes must remain unchanged");
        }
        check(screen.CellAt(2, 1).character == "界" && screen.CellAt(4, 1).character == "é" &&
              source == "A界é─╔ bytes", "wide, combining and literal frame bytes must survive");
    }
}

void fallbacks() {
    auto colors = skin();
    colors.colors.clear();
    colors.colors["editor"]["_default_"] = desktop;
    colors.colors["core"]["editframeactive"] = active;
    std::array<Box, 2> buttons{};
    const auto original = render_window(colors, true, false, false, buttons);
    const auto unchanged = render_window(colors, true, false, true, buttons);
    for (int row = 0; row < 8; ++row) {
        for (int column = 0; column < 30; ++column) {
            same(original.CellAt(column, row), unchanged.CellAt(column, row));
        }
    }
    colors.colors["editor"]["editframe"] = inactive;
    colors.colors["editor"]["editframeactive"] = active;
    const auto missing_drag = render_window(colors, true, true, true, buttons);
    for (int row = 0; row < 8; ++row) {
        for (int column = 0; column < 30; ++column) {
            same(original.CellAt(column, row), missing_drag.CellAt(column, row));
        }
    }
    colors.colors["editor"].erase("editframeactive");
    colors.colors["editor"]["editframedrag"] = drag;
    const auto corner_only = render_window(colors, true, false, true, buttons);
    for (int row = 0; row < 8; ++row) {
        for (int column = 0; column < 30; ++column) {
            if (row == 7 && column == 29) pair(corner_only.CellAt(column, row), drag);
            else same(original.CellAt(column, row), corner_only.CellAt(column, row));
        }
    }
}

void blank_desktop() {
    auto colors = skin();
    const auto render = [&](const bool styled) {
        const auto blank = [&](Element element) {
            return styled ? mcpp::detail::editor_desktop(std::move(element), colors) :
                            std::move(element);
        };
        std::array<Box, 2> buttons{};
        auto screen = Screen::Create(Dimension::Fixed(40), Dimension::Fixed(14));
        Render(screen, paint_skin_style(vbox({
            paint_skin_style(mcpp::glyph_text("MENUBAR"), status),
            vbox({
                blank(filler() | size(HEIGHT, EQUAL, 1)),
                hbox({
                    blank(filler() | size(WIDTH, EQUAL, 3)),
                    window(colors, true, false, true, buttons),
                    blank(filler()),
                }),
                blank(filler()),
            }) | flex,
            paint_skin_style(mcpp::glyph_text("KEYBAR"), status),
        }), normal));
        return screen;
    };
    const auto original = render(false);
    const auto styled = render(true);
    for (int row = 0; row < 14; ++row) {
        for (int column = 0; column < 40; ++column) {
            const auto blank = row > 0 && row < 13 &&
                (row < 2 || row > 9 || column < 3 || column > 32);
            if (blank) {
                pair(styled.CellAt(column, row), desktop);
                check(styled.CellAt(column, row).character ==
                      original.CellAt(column, row).character, "desktop paint must not draw text");
            } else {
                same(original.CellAt(column, row), styled.CellAt(column, row));
            }
        }
    }
    colors.colors["editor"].erase("editbg");
    const auto fallback = render(true);
    const auto plain = render(false);
    for (int row = 0; row < 14; ++row) {
        for (int column = 0; column < 40; ++column) {
            same(plain.CellAt(column, row), fallback.CellAt(column, row));
        }
    }
}

void clipping() {
    const auto colors = skin();
    const auto render = [&](const bool styled, const Box box) {
        auto screen = Screen::Create(Dimension::Fixed(12), Dimension::Fixed(8));
        Render(screen, paint_skin_style(filler(), normal));
        screen.stencil = {1, 7, 1, 5};
        auto border = mcpp::glyph_border(mcpp::glyph_text("λ界é─") | borderStyled(LIGHT));
        if (styled) {
            border = mcpp::detail::editor_window_frame(std::move(border), colors, true, false);
        }
        border->ComputeRequirement();
        border->SetBox(box);
        border->Render(screen);
        return screen;
    };
    const auto original = render(false, {-1, 8, -1, 5});
    const auto styled = render(true, {-1, 8, -1, 5});
    for (int row = 0; row < 8; ++row) {
        for (int column = 0; column < 12; ++column) {
            if (row == 5 && column >= 1 && column <= 7) {
                pair(styled.CellAt(column, row), active);
            } else {
                same(original.CellAt(column, row), styled.CellAt(column, row));
            }
        }
    }
    for (const auto box : {Box{2, 2, 2, 5}, Box{2, 5, 2, 2}}) {
        const auto before = render(false, box);
        const auto after = render(true, box);
        for (int row = 0; row < 8; ++row) {
            for (int column = 0; column < 12; ++column) {
                same(before.CellAt(column, row), after.CellAt(column, row));
            }
        }
    }
}

}  // namespace

int main() {
    try {
        check(::geteuid() == 1001U && std::filesystem::exists("/.dockerenv") &&
              std::filesystem::is_regular_file("/review/.operation-regressions-disposable"),
              "window style tests require UID1001 in the owned review container");
        Terminal::SetColorSupport(Terminal::TrueColor);
        frames();
        fallbacks();
        blank_desktop();
        clipping();
        std::cout << "editor frame pairs, focus/drag/corner, desktop, fallbacks and protected cells: passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
