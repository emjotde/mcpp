#include "mcpp/cli_display.hpp"
#include "mcpp/skin.hpp"
#include "mcpp/skin_glyphs_ui.hpp"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/string.hpp>
#include <ftxui/screen/terminal.hpp>

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>

namespace {

void check(const bool condition, const std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

void write(const std::filesystem::path& path, const std::string_view contents) {
    std::ofstream output(path);
    output << contents;
    check(static_cast<bool>(output), "write original glyph fixture");
}

void native_roles(const std::filesystem::path& root) {
    const auto path = root / "independent.ini";
    write(path, R"([core]
_default_=brightcyan;blue;bold
[Lines]
horiz=-
vert=!
lefttop=a
righttop=b
leftbottom=c
rightbottom=d
topmiddle=e
bottommiddle=f
leftmiddle=g
rightmiddle=h
cross=i
dhoriz==
dvert=:
dlefttop=A
drighttop=B
dleftbottom=C
drightbottom=D
dtopmiddle=E
dbottommiddle=F
dleftmiddle=G
drightmiddle=H
[widget-panel]
sort-up-char=U
sort-down-char=D
[widget-common]
sort-sign-up=^
sort-sign-down=v
[widget-editor]
window-state-char=S
window-close-char=Q
[editor]
window-state-char=*
window-close-char=x
[widget-scrollbar]
current-char=#
[widget-scollbar]
background-char=.
)");
    const auto skin = mcpp::load_skin(path.string(), root);
    check(skin.normal.foreground == ftxui::Color::CyanLight &&
          skin.normal.background == ftxui::Color::Blue &&
          skin.normal.attributes == mcpp::SkinPair::bold,
          "glyph definitions preserve native color attributes");
    check(skin.glyphs.has_lines() && skin.glyphs.has_double_lines() &&
          mcpp::skin_glyph(skin.glyphs, mcpp::SkinGlyph::lefttop, "?") == "a" &&
          mcpp::skin_glyph(skin.glyphs, mcpp::SkinGlyph::drightmiddle, "?") == "H",
          "single and double native line roles load");
    check(mcpp::skin_glyph(skin.glyphs, mcpp::SkinGlyph::sort_up, "?") == "U" &&
          mcpp::skin_glyph(skin.glyphs, mcpp::SkinGlyph::window_close, "?") == "Q",
          "current widget keys win over legacy aliases independent of section order");
    check(skin.warning.find("[widget-scrollbar] current-char") != std::string::npos &&
          skin.warning.find("[widget-scollbar] background-char") != std::string::npos &&
          !skin.colors.contains("widget-panel"),
          "unrendered roles have exact diagnostics and never become colors");
    write(path, "[core]\n_default_=white;black\n[widget-common]\nsort-sign-up=↑\n"
                "[editor]\nwindow-close-char=×\n");
    const auto legacy = mcpp::load_skin(path.string(), root);
    check(legacy.warning.empty() && !legacy.glyphs.has_lines() &&
          mcpp::skin_glyph(legacy.glyphs, mcpp::SkinGlyph::sort_up, "^") == "↑" &&
          mcpp::skin_glyph(legacy.glyphs, mcpp::SkinGlyph::window_close, "X") == "×",
          "legacy roles and reload use fresh glyph values");
    check(&mcpp::find_skin("classic") == &mcpp::find_skin("grayscale") &&
          &mcpp::find_skin("gray") == &mcpp::find_skin("grayscale") &&
          !mcpp::find_skin("classic").glyphs.has_lines(),
          "legacy builtin aliases keep their original fallback glyphs");
}

void validation() {
    mcpp::SkinGlyphs glyphs;
    for (const auto invalid : std::array<std::string_view, 10>{
             "\x1b", "\t", "\xc0\xaf", "\xed\xa0\x80", "界", "\xcc\x81", "\xe2\x80\xae",
             "\xd8\x80", "\xef\xbf\xbf", "ab"}) {
        bool rejected = false;
        try {
            static_cast<void>(mcpp::set_skin_glyph(glyphs, "Lines", "vert", invalid));
        } catch (const std::invalid_argument& error) {
            rejected = std::string_view(error.what()).find("single-cell UTF-8") != std::string_view::npos;
        }
        check(rejected, "unsafe, malformed, combining, wide and multi-character glyphs are rejected");
    }
    check(mcpp::set_skin_glyph(glyphs, "Lines", "vert", "") == mcpp::SkinGlyphSetting::supported &&
          mcpp::skin_glyph(glyphs, mcpp::SkinGlyph::vert, "│") == "│",
          "empty or omitted glyphs preserve fallback");
    check(mcpp::set_skin_glyph(glyphs, "core", "selected", "black;white") ==
              mcpp::SkinGlyphSetting::color &&
          mcpp::set_skin_glyph(glyphs, "Lines", "unknown", "?") ==
              mcpp::SkinGlyphSetting::unsupported,
          "colors and unmapped glyph roles remain distinguishable");
    static_cast<void>(mcpp::set_skin_glyph(glyphs, "widget-common", "sort-sign-up", "↑"));
    static_cast<void>(mcpp::set_skin_glyph(glyphs, "widget-editor", "window-close-char", "×"));
    check(mcpp::skin_glyph(glyphs, mcpp::SkinGlyph::sort_up, "↑", true) == "^" &&
          mcpp::skin_glyph(glyphs, mcpp::SkinGlyph::window_close, "×", true) == "X",
          "stickchars overrides custom widget characters");
}

void generated_lines_and_text(const std::filesystem::path& root) {
    using namespace ftxui;
    const auto path = root / "render.ini";
    write(path, "[core]\n_default_=brightcyan;blue;bold\n[Lines]\n"
                "horiz=-\nvert=!\nlefttop=a\nrighttop=b\nleftbottom=c\nrightbottom=d\n"
                "leftmiddle=g\nrightmiddle=h\ndhoriz==\ndvert=:\ndlefttop=A\n"
                "drighttop=B\ndleftbottom=C\ndrightbottom=D\n");
    const auto skin = mcpp::load_skin(path.string(), root);
    const std::array<const mcpp::Skin*, 1> palettes{&skin};
    constexpr std::string_view content = "┌┐└┘│─╔╗╚╝═║↑↓";
    const auto frame = [&](const BorderStyle style) {
        return mcpp::glyph_border(window(mcpp::glyph_text("┌"), vbox({
            mcpp::glyph_text(content), separator(), mcpp::glyph_text("café"),
        }), style)) | color(skin.normal.foreground) | bgcolor(skin.normal.background) | bold;
    };
    auto screen = Screen::Create(Dimension::Fixed(24), Dimension::Fixed(5));
    Render(screen, mcpp::cli_display(frame(LIGHT), false, false, palettes));
    check(screen.at(0, 0) == "a" && screen.at(23, 0) == "b" &&
          screen.at(0, 4) == "c" && screen.at(23, 4) == "d" &&
          screen.at(1, 0) == "┌" && screen.at(2, 0) == "-" &&
          screen.at(0, 2) == "g" && screen.at(23, 2) == "h",
          "titled corners, edges and merged separator junctions use UI glyph roles");
    const auto original = Utf8ToGlyphs(content);
    for (std::size_t index = 0; index < original.size(); ++index) {
        check(screen.at(static_cast<int>(index) + 1, 1) == original[index],
              "file text containing identical border and arrow characters is unchanged");
    }
    check(screen.CellAt(0, 0).foreground_color == skin.normal.foreground &&
          screen.CellAt(0, 0).background_color == skin.normal.background &&
          screen.CellAt(0, 0).bold, "line substitution preserves colors and attributes");
    screen.Clear();
    Render(screen, mcpp::cli_display(frame(DOUBLE), false, false, palettes));
    check(screen.at(0, 0) == "A" && screen.at(23, 0) == "B" &&
          screen.at(0, 4) == "C" && screen.at(23, 4) == "D" &&
          screen.at(2, 0) == "=" && screen.at(0, 1) == ":",
          "double frames use their distinct native roles");
    screen.Clear();
    Render(screen, mcpp::cli_display(frame(LIGHT), false, true, palettes));
    check(screen.at(0, 0) == "+" && screen.at(2, 0) == "-" && screen.at(0, 1) == "|" &&
          screen.at(1, 1) == "┌" && screen.at(14, 1) == "↓",
          "stickchars wins without modifying content");
    screen.Clear();
    Render(screen, mcpp::cli_display(dbox({
        frame(LIGHT), mcpp::glyph_text("┌│─↑↓"),
    }), false, true, palettes));
    check(screen.at(0, 0) == "┌" && screen.at(1, 0) == "│" &&
          screen.at(2, 0) == "─" && screen.at(3, 0) == "↑" && screen.at(4, 0) == "↓",
          "ordinary text is not guessed to be a UI frame");
}

}  // namespace

int main(const int argc, char** argv) {
    try {
        if (argc != 2 || ::geteuid() != 1001U || !std::filesystem::exists("/.dockerenv") ||
            !std::filesystem::exists("/review/.operation-regressions-disposable")) {
            throw std::runtime_error("glyph checks require the existing UID1001 review container and an owned case path");
        }
        const auto root = std::filesystem::absolute(argv[1]).lexically_normal();
        check(root.string().starts_with("/review/cases/native-skin-glyphs-"), "use an owned glyph case directory");
        std::filesystem::create_directories(root);
        ftxui::Terminal::SetColorSupport(ftxui::Terminal::TrueColor);
        native_roles(root);
        validation();
        generated_lines_and_text(root);
        std::cout << "native glyph roles, safety and UI-only rendering: passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
