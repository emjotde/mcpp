#include "mcpp/cli_display.hpp"
#include "mcpp/command_line.hpp"
#include "mcpp/skin.hpp"
#include "mnemonic_styles.hpp"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/terminal.hpp>

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace {

using mcpp::SkinPair;

void check(const bool condition, const std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

std::string read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}

bool same_pair(const SkinPair& left, const SkinPair& right) {
    return left.foreground == right.foreground && left.background == right.background &&
           left.attributes == right.attributes && left.explicit_style == right.explicit_style;
}

void pair(const ftxui::Cell& cell, const SkinPair& expected) {
    const auto attributes = (cell.bold ? SkinPair::bold : 0U) |
        (cell.italic ? SkinPair::italic : 0U) | (cell.underlined ? SkinPair::underline : 0U) |
        (cell.inverted ? SkinPair::reverse : 0U) | (cell.blink ? SkinPair::blink : 0U);
    check(cell.foreground_color == expected.foreground && cell.background_color == expected.background &&
          attributes == expected.attributes && !cell.dim && !cell.underlined_double && !cell.strikethrough,
          "only the hot glyph receives its complete pair and exact attributes");
}

ftxui::Screen draw(
    const SkinPair& body, const SkinPair* hot, const std::size_t index = 0,
    const bool emphasize_missing = true, const bool outer_inverted = false
) {
    using namespace ftxui;
    Box box;
    auto label = hbox({text("[ "),
        mcpp::detail::render_mnemonic_label("Aé中_$[]", index, hot, emphasize_missing,
            std::nullopt, outer_inverted), text(" ]")}) |
        reflect(box);
    auto screen = Screen::Create(Dimension::Fixed(12), Dimension::Fixed(1));
    auto element = mcpp::detail::paint_skin_style(std::move(label), body);
    if (outer_inverted) element = element | inverted;
    Render(screen, std::move(element));
    check(box.x_min == 0 && box.x_max == 11 && box.y_min == 0 && box.y_max == 0,
          "mnemonic styling must retain the complete button hitbox");
    check(screen.CellAt(2, 0).character == "A" && screen.CellAt(3, 0).character == "é" &&
          screen.CellAt(4, 0).character == "中" && screen.CellAt(6, 0).character == "_" &&
          screen.CellAt(7, 0).character == "$" && screen.CellAt(8, 0).character == "[" &&
          screen.CellAt(9, 0).character == "]",
          "mnemonic painting must retain literal Unicode and punctuation");
    auto ordinary = body;
    if (outer_inverted) ordinary.attributes ^= SkinPair::reverse;
    pair(screen.CellAt(0, 0), ordinary);
    pair(screen.CellAt(10, 0), ordinary);
    return screen;
}

void exercise(const std::filesystem::path& root) {
    const auto resource = root / "original-mnemonics.ini";
    {
        std::ofstream output(resource);
        output << R"([skin]
description=Original mnemonic role sample
[core]
_default_=white;black;none
[dialog]
_default_=white;#102030;bold+italic+underline+reverse+blink
dfocus=black;#708090;none
dhotnormal=#ddeeff;#401020;none
dhotfocus=#ccaa88;#123456;italic+underline
[error]
_default_=white;red;none
errdfocus=black;lightgray;none
errdhotnormal=#abcdef;#432154;blink
errdhotfocus=yellow;magenta;bold
[menu]
_default_=white;#203040;none
menusel=black;cyan;none
menuhot=#ffddaa;#512345;underline
menuhotsel=#aabbcc;#615243;none
)";
        check(static_cast<bool>(output), "write the single original mnemonic resource");
    }
    const auto original = read(resource);
    const auto source = mcpp::load_skin(resource.string(), root);
    const std::array<const SkinPair*, 5> styles{
        mcpp::detail::dialog_mnemonic_style(source, false),
        mcpp::detail::dialog_mnemonic_style(source, true),
        mcpp::detail::dialog_mnemonic_style(source, false, true),
        mcpp::detail::menu_mnemonic_style(source, false),
        mcpp::detail::menu_mnemonic_style(source, true),
    };
    for (const auto* style : styles) {
        check(style != nullptr, "each supported native mnemonic has an exact raw lookup");
        const auto screen = draw(source.dialog, style);
        pair(screen.CellAt(2, 0), *style);
        pair(screen.CellAt(3, 0), source.dialog);
    }
    pair(draw(source.dialog, styles[0], 1).CellAt(3, 0), *styles[0]);
    const auto continuation = draw(source.dialog, styles[0], 2, false);
    pair(continuation.CellAt(3, 0), source.dialog);
    auto all = *styles[0];
    all.attributes = SkinPair::bold | SkinPair::italic | SkinPair::underline |
                     SkinPair::reverse | SkinPair::blink;
    pair(draw(source.dialog, &all).CellAt(2, 0), all);
    pair(draw(source.dialog, styles[1], 0, true, true).CellAt(2, 0), *styles[1]);
    pair(draw(source.menu_selected, styles[4], 0, true, true).CellAt(2, 0), *styles[4]);
    pair(draw(source.dialog, &all, 0, true, true).CellAt(2, 0), all);

    const auto legacy = mcpp::find_skin("gray");
    check(!mcpp::detail::dialog_mnemonic_style(legacy, false) &&
          !mcpp::detail::dialog_mnemonic_style(legacy, true) &&
          !mcpp::detail::dialog_mnemonic_style(legacy, false, true) &&
          !mcpp::detail::menu_mnemonic_style(legacy, true),
          "missing roles must retain the existing button and mnemonic fallbacks");
    auto emphasized = legacy.dialog;
    emphasized.attributes |= SkinPair::bold | SkinPair::underline;
    pair(draw(legacy.dialog, nullptr).CellAt(2, 0), emphasized);
    pair(draw(legacy.dialog, nullptr, 0, false).CellAt(2, 0), legacy.dialog);
    emphasized.attributes ^= SkinPair::reverse;
    pair(draw(legacy.dialog, nullptr, 0, true, true).CellAt(2, 0), emphasized);

    const auto options = mcpp::parse_cli(std::array<std::string_view, 4>{
        "-C", "dhotnormal=#abc,,none:dhotfocus=,color17,italic+underline",
        "--colors=errdhotnormal=black,red,blink:menuhotsel=white,#abc,none:dhotfocus=brightgreen",
        "literal:$[x]-é中",
    });
    check(options.paths == std::vector<std::string>{"literal:$[x]-é中"},
          "short and long color flags must not reinterpret literal operands");
    const auto overrides = mcpp::parse_cli_colors(options.colors);
    for (const auto context : {mcpp::CliPaletteContext::panels, mcpp::CliPaletteContext::editor,
                              mcpp::CliPaletteContext::viewer, mcpp::CliPaletteContext::help,
                              mcpp::CliPaletteContext::diff}) {
        auto skin = source;
        mcpp::apply_cli_colors(skin, overrides, context);
        const auto* normal = mcpp::detail::dialog_mnemonic_style(skin, false);
        const auto* focused = mcpp::detail::dialog_mnemonic_style(skin, true);
        const auto* error = mcpp::detail::dialog_mnemonic_style(skin, false, true);
        const auto* selected = mcpp::detail::menu_mnemonic_style(skin, true);
        check(normal->foreground == ftxui::Color::RGB(170, 187, 204) &&
              normal->background == styles[0]->background && normal->attributes == 0U &&
              focused->foreground == ftxui::Color::GreenLight &&
              focused->background == ftxui::Color(static_cast<ftxui::Color::Palette256>(17)) &&
              focused->attributes == (SkinPair::italic | SkinPair::underline) &&
              error->foreground == ftxui::Color::Black && error->background == ftxui::Color::Red &&
              error->attributes == SkinPair::blink &&
              selected->foreground == ftxui::Color::White &&
              selected->background == ftxui::Color::RGB(170, 187, 204) && selected->attributes == 0U,
              "four CLI roles must merge supplied fields in order into their canonical raw keys");
        check(same_pair(skin.dialog, source.dialog) &&
              same_pair(skin.dialog_focus, source.dialog_focus) &&
              same_pair(skin.error, source.error) && same_pair(skin.error_focus, source.error_focus) &&
              same_pair(skin.menu, source.menu) && same_pair(skin.menu_selected, source.menu_selected),
              "mnemonic-only overrides must not change ordinary button, dialog or menu pairs");
        for (const auto* style : {normal, focused, error, selected}) {
            pair(draw(skin.dialog, style).CellAt(2, 0), *style);
        }
        mcpp::apply_cli_colors(skin, mcpp::parse_cli_colors(std::array<std::string, 1>{
            "errdhotfocus=brightgreen,blue,none",
        }), context);
        check(skin.error_focus.foreground == ftxui::Color::GreenLight &&
              skin.error_focus.background == ftxui::Color::Blue &&
              skin.error_focus.attributes == 0U &&
              same_pair(skin.colors.at("error").at("errdhotfocus"),
                        source.colors.at("error").at("errdhotfocus")) &&
              !mcpp::detail::dialog_mnemonic_style(skin, true, true),
              "errdhotfocus must remain the whole-focus alias, not silently become a glyph-only role");
    }
    for (const auto role : {"dhotnormal", "dhotfocus", "errdhotnormal", "menuhotsel"}) {
        check(mcpp::cli_color_roles().find(role) != std::string_view::npos,
              "CLI help must advertise the four independently consumed roles");
    }
    check(read(resource) == original, "painting and launch overrides must not rewrite the original skin");
}

}  // namespace

int main(const int argc, char** argv) {
    try {
        check(argc == 2 && ::geteuid() == 1001U && std::filesystem::exists("/.dockerenv") &&
              std::filesystem::exists("/review/.operation-regressions-disposable"),
              "mnemonic styles require UID1001 in the owned review container");
        const auto root = std::filesystem::absolute(argv[1]).lexically_normal();
        check(root.string().starts_with("/review/cases/mnemonic-roles-96b2/model/"),
              "use the owned mnemonic model directory");
        ::umask(0077);
        std::filesystem::create_directories(root);
        ftxui::Terminal::SetColorSupport(ftxui::Terminal::TrueColor);
        exercise(root);
        std::cout << "mnemonic styles and CLI: passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
