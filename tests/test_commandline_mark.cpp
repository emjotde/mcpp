#include "mcpp/cli_display.hpp"
#include "mcpp/command_line.hpp"
#include "mcpp/config.hpp"
#include "mcpp/skin.hpp"
#include "input_styles.hpp"
#include "viewer_help_styles.hpp"

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
#include <vector>

namespace {

using mcpp::SkinPair;
constexpr unsigned all_attributes =
    SkinPair::bold | SkinPair::italic | SkinPair::underline | SkinPair::reverse | SkinPair::blink;

void check(const bool condition, const std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

std::string read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    check(static_cast<bool>(input), "read the original command-line fixture");
    return {std::istreambuf_iterator<char>(input), {}};
}

void write(const std::filesystem::path& path, const std::string_view bytes) {
    std::ofstream output(path, std::ios::binary);
    output << bytes;
    check(static_cast<bool>(output), "write the original command-line fixture");
}

bool same_pair(const SkinPair& first, const SkinPair& second) {
    return first.foreground == second.foreground && first.background == second.background &&
        first.attributes == second.attributes && first.explicit_style == second.explicit_style;
}

unsigned attributes(const ftxui::Cell& cell) {
    return (cell.bold ? SkinPair::bold : 0U) | (cell.italic ? SkinPair::italic : 0U) |
        (cell.underlined ? SkinPair::underline : 0U) | (cell.inverted ? SkinPair::reverse : 0U) |
        (cell.blink ? SkinPair::blink : 0U);
}

void exact_pair(const ftxui::Cell& cell, const SkinPair& expected) {
    check(cell.foreground_color == expected.foreground && cell.background_color == expected.background &&
          attributes(cell) == expected.attributes && !cell.dim && !cell.underlined_double &&
          !cell.strikethrough, "command selection must use its exact pair after the outer focus inversion");
}

void same_cell(const ftxui::Cell& first, const ftxui::Cell& second) {
    check(first.character == second.character && first.foreground_color == second.foreground_color &&
          first.background_color == second.background_color && attributes(first) == attributes(second) &&
          first.dim == second.dim && first.underlined_double == second.underlined_double &&
          first.strikethrough == second.strikethrough,
          "command-line styling must leave unselected glyphs and their normal styles unchanged");
}

ftxui::Screen draw(const mcpp::Skin& skin, const bool active, const bool legacy = false) {
    using namespace ftxui;
    const auto selection = [&](Element element) {
        return legacy ? std::move(element) | bold | underlined
                      : mcpp::detail::command_input_selection_style(std::move(element), skin, active);
    };
    auto field = hbox({
        text("$ "),
        hbox({
            text("A"),
            selection(text("é")),
            selection(text("中")),
            text("_$[x] 'literal draft' "),
        }) | xflex | xframe,
    });
    if (active) {
        auto reversed = skin.dialog_focus;
        std::swap(reversed.foreground, reversed.background);
        field = mcpp::detail::paint_skin_style(std::move(field), reversed) | inverted;
    }
    auto screen = Screen::Create(Dimension::Fixed(40), Dimension::Fixed(1));
    Render(screen, mcpp::detail::paint_skin_style(std::move(field), skin.normal));
    check(screen.CellAt(2, 0).character == "A" && screen.CellAt(3, 0).character == "é" &&
          screen.CellAt(4, 0).character == "中" && screen.CellAt(6, 0).character == "_" &&
          screen.CellAt(7, 0).character == "$" && screen.CellAt(12, 0).character == "'",
          "command-line rendering must preserve literal UTF-8, shell punctuation and quotes");
    return screen;
}

void preserved_roles(const mcpp::Skin& skin, const mcpp::Skin& source) {
    check(same_pair(skin.normal, source.normal) && same_pair(skin.dialog, source.dialog) &&
          same_pair(skin.dialog_focus, source.dialog_focus),
          "a command selection override must not replace command normal or dialog colors");
    for (const auto role : {"input", "inputmark", "inputunchanged"}) {
        check(same_pair(skin.colors.at("core").at(role), source.colors.at("core").at(role)),
              "all three dialog-input roles must remain independent of commandlinemark");
    }
}

void rendered(const mcpp::Skin& skin, const mcpp::Skin& baseline) {
    const auto& expected = skin.colors.at("core").at("commandlinemark");
    for (const bool active : {false, true}) {
        const auto actual = draw(skin, active);
        const auto original = draw(baseline, active, true);
        exact_pair(actual.CellAt(3, 0), expected);
        exact_pair(actual.CellAt(4, 0), expected);
        exact_pair(actual.CellAt(5, 0), expected);
        for (int column = 0; column < actual.dimx(); ++column) {
            if (column < 3 || column > 5) same_cell(actual.CellAt(column, 0), original.CellAt(column, 0));
        }
    }
}

void exercise(const std::filesystem::path& root) {
    const auto resource = root / "original-commandline.ini";
    const auto preferences = root / "original-colors.ini";
    write(resource, R"([skin]
description=Original independent command-line mark fixture
[core]
_default_=lightgray;black;bold+italic+underline+reverse+blink
commandlinemark=#102030;#d8c070;none
input=cyan;red;italic
inputmark=yellow;green;underline
inputunchanged=white;magenta;bold
[dialog]
_default_=white;blue;bold+italic+underline+reverse+blink
dfocus=black;lightgray;bold+italic+underline+reverse+blink
)");
    write(preferences,
        "# original saved colors, never rewritten\n[Colors]\n"
        "base_color=commandlinemark=#123456,#654321,bold+blink\n"
        "xterm-256color=commandlinemark=,color17,italic+underline\n"
        "inactive-terminal=commandlinemark=not-a-color\n");
    const auto resource_bytes = read(resource);
    const auto preferences_bytes = read(preferences);
    const auto source = mcpp::load_skin(resource.string(), root);
    rendered(source, source);
    for (const auto flags : {SkinPair::bold, SkinPair::italic, SkinPair::underline,
                             SkinPair::reverse, SkinPair::blink, all_attributes}) {
        auto skin = source;
        skin.colors["core"]["commandlinemark"].attributes = flags;
        rendered(skin, source);
        preserved_roles(skin, source);
    }

    auto missing = source;
    missing.colors["core"].erase("commandlinemark");
    for (const bool active : {false, true}) {
        const auto actual = draw(missing, active);
        const auto original = draw(missing, active, true);
        for (int column = 0; column < actual.dimx(); ++column) {
            same_cell(actual.CellAt(column, 0), original.CellAt(column, 0));
        }
    }
    const auto partial = mcpp::parse_cli_colors(std::array<std::string, 1>{"commandlinemark=red"});
    const auto expected_fallback = mcpp::skin_color(missing, "core", "commandlinemark", source.dialog_focus);
    mcpp::apply_cli_colors(missing, partial, mcpp::CliPaletteContext::panels);
    const auto& fallback = missing.colors.at("core").at("commandlinemark");
    check(fallback.foreground == ftxui::Color::Red && fallback.background == expected_fallback.background &&
          fallback.attributes == (expected_fallback.attributes | SkinPair::bold | SkinPair::underline),
          "a partial command role follows the existing core-default and focus fallback precedence");
    rendered(missing, source);
    preserved_roles(missing, source);

    const auto options = mcpp::parse_cli(std::array<std::string_view, 4>{
        "-C", "commandlinemark=#abc,,none", "--colors=commandlinemark=,magenta",
        "literal:$[x]-é中 'quoted'"
    });
    check(options.colors.size() == 2U &&
          options.paths == std::vector<std::string>{"literal:$[x]-é中 'quoted'"},
          "short and long color options must retain literal Unicode and quoted operands");
    const auto overrides = mcpp::parse_cli_colors(options.colors);
    auto cli = source;
    mcpp::apply_cli_colors(cli, overrides, mcpp::CliPaletteContext::panels);
    const auto& command = cli.colors.at("core").at("commandlinemark");
    check(command.foreground == ftxui::Color::RGB(170, 187, 204) &&
          command.background == ftxui::Color::Magenta && command.attributes == 0U,
          "repeated CLI overrides merge fields and preserve an explicit none");
    preserved_roles(cli, source);
    rendered(cli, source);

    const auto config = mcpp::Config::load(preferences);
    const auto layers = mcpp::parse_config_colors(config, "xterm-256color", "commandlinemark=#abc,,reverse");
    check(layers.size() == 3U, "native base, exact TERM and environment tables accept commandlinemark");
    auto configured = source;
    for (const auto& layer : layers) {
        mcpp::apply_cli_colors(configured, layer, mcpp::CliPaletteContext::panels);
        rendered(configured, source);
        preserved_roles(configured, source);
    }
    const auto& configured_pair = configured.colors.at("core").at("commandlinemark");
    check(configured_pair.foreground == ftxui::Color::RGB(170, 187, 204) &&
          configured_pair.background == ftxui::Color(static_cast<ftxui::Color::Palette256>(17)) &&
          configured_pair.attributes == SkinPair::reverse,
          "native Colors and the environment preserve omitted fields in precedence order");
    mcpp::apply_cli_colors(configured, overrides, mcpp::CliPaletteContext::panels);
    check(same_pair(configured.colors.at("core").at("commandlinemark"), command),
          "launch-only CLI colors override native and environment layers without changing other fields");
    rendered(configured, source);
    preserved_roles(configured, source);
    for (const auto specification : {"statusbar=red", "commandlinemark=", "commandlinemark=not-a-color",
                                      "commandlinemark=red,,,none", "commandlinemark=,,unknown"}) {
        bool rejected = false;
        try {
            static_cast<void>(mcpp::parse_cli_colors(std::array<std::string, 1>{specification}));
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        check(rejected, "unrendered roles and malformed command selection colors must remain rejected");
    }
    check(read(resource) == resource_bytes && read(preferences) == preferences_bytes,
          "rendering and launch color layers must not rewrite original skins or saved Colors");
}

}  // namespace

int main(const int argc, char** argv) {
    try {
        check(argc == 2 && ::geteuid() == 1001U && std::filesystem::exists("/.dockerenv") &&
              std::filesystem::exists("/review/.operation-regressions-disposable"),
              "commandline mark requires UID1001 in the owned disposable review container");
        const auto root = std::filesystem::absolute(argv[1]).lexically_normal();
        check(root.string().starts_with("/review/cases/commandline-mark/model/"),
              "use an owned commandline-mark/model case directory");
        ::umask(0077);
        std::filesystem::create_directories(root);
        ftxui::Terminal::SetColorSupport(ftxui::Terminal::TrueColor);
        exercise(root);
        std::cout << "commandline mark model, CLI and native Colors: passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
