#include "mcpp/cli_display.hpp"
#include "mcpp/command_line.hpp"
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

namespace {

using mcpp::SkinPair;
constexpr unsigned all_attributes =
    SkinPair::bold | SkinPair::italic | SkinPair::underline | SkinPair::reverse | SkinPair::blink;
constexpr std::string_view literal = "Aé中_$[]";

void check(const bool condition, const std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

std::string read(const std::filesystem::path& path) {
    std::ifstream source(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(source), {}};
}

void pair(const ftxui::Cell& cell, const SkinPair& expected) {
    const auto attributes = (cell.bold ? SkinPair::bold : 0U) |
        (cell.italic ? SkinPair::italic : 0U) | (cell.underlined ? SkinPair::underline : 0U) |
        (cell.inverted ? SkinPair::reverse : 0U) | (cell.blink ? SkinPair::blink : 0U);
    check(cell.foreground_color == expected.foreground && cell.background_color == expected.background &&
          attributes == expected.attributes && !cell.dim && !cell.underlined_double && !cell.strikethrough,
          "input text, selection and fill must use their complete pair and exact attributes");
}

ftxui::Screen draw(
    const mcpp::Skin& skin, const bool pristine, const bool active, const bool empty = false
) {
    using namespace ftxui;
    auto contents = empty ? text(" ") : hbox({
        text("Aé中_"),
        mcpp::detail::input_selection_style(text("$"), skin, pristine),
        text("[]"),
    });
    auto field = mcpp::detail::input_field_style(std::move(contents) | xflex, skin, active, pristine);
    auto screen = Screen::Create(Dimension::Fixed(24), Dimension::Fixed(1));
    Render(screen, mcpp::detail::paint_skin_style(std::move(field), skin.dialog));
    if (!empty) {
        check(screen.CellAt(0, 0).character == "A" && screen.CellAt(1, 0).character == "é" &&
              screen.CellAt(2, 0).character == "中" && screen.CellAt(4, 0).character == "_" &&
              screen.CellAt(5, 0).character == "$" && screen.CellAt(6, 0).character == "[" &&
              screen.CellAt(7, 0).character == "]", "styling must preserve literal Unicode input glyphs");
    }
    return screen;
}

void exercise(const std::filesystem::path& root) {
    const auto resource = root / "original-input.ini";
    {
        std::ofstream output(resource);
        output << R"([skin]
description=Original input role fixture
[core]
_default_=lightgray;black;bold+italic+underline+reverse+blink
input=#ddeeff;#431627;italic
inputmark=#102030;#d8c070;none
inputunchanged=#4fdd90;#12395a;none
[dialog]
_default_=white;blue;bold+italic+underline+reverse+blink
dfocus=black;lightgray;none
)";
        check(static_cast<bool>(output), "write the single original input resource");
    }
    const auto original_bytes = read(resource);
    const auto source = mcpp::load_skin(resource.string(), root);
    const auto& core = source.colors.at("core");
    for (const bool active : {false, true}) {
        auto screen = draw(source, true, active);
        for (const int column : {0, 5, 23}) pair(screen.CellAt(column, 0), core.at("inputunchanged"));
        screen = draw(source, true, active, true);
        pair(screen.CellAt(0, 0), core.at("inputunchanged"));
        pair(screen.CellAt(23, 0), core.at("inputunchanged"));
        screen = draw(source, false, active);
        pair(screen.CellAt(0, 0), core.at("input"));
        pair(screen.CellAt(23, 0), core.at("input"));
        pair(screen.CellAt(5, 0), core.at("inputmark"));
    }
    for (const auto role : {"input", "inputmark", "inputunchanged"}) {
        auto skin = source;
        skin.colors["core"][role].attributes = all_attributes;
        const bool pristine = std::string_view(role) == "inputunchanged";
        const auto screen = draw(skin, pristine, true);
        pair(screen.CellAt(std::string_view(role) == "inputmark" ? 5 : 0, 0),
             skin.colors.at("core").at(role));
    }

    auto legacy = mcpp::find_skin("gray");
    for (const bool active : {false, true}) {
        auto expected = active ? legacy.dialog_focus : legacy.dialog;
        if (active) {
            std::swap(expected.foreground, expected.background);
            expected.attributes ^= SkinPair::reverse;
        }
        const auto screen = draw(legacy, true, active);
        pair(screen.CellAt(0, 0), expected);
        pair(screen.CellAt(23, 0), expected);
        expected.attributes |= SkinPair::bold | SkinPair::underline;
        pair(screen.CellAt(5, 0), expected);
    }
    legacy.colors["core"]["inputmark"] = core.at("inputmark");
    const auto marked = draw(legacy, false, true);
    pair(marked.CellAt(5, 0), core.at("inputmark"));
    auto partial = source;
    partial.colors["core"].erase("inputunchanged");
    const auto initial = draw(partial, true, true);
    pair(initial.CellAt(0, 0), core.at("input"));
    auto selected_fallback = core.at("input");
    selected_fallback.attributes |= SkinPair::bold | SkinPair::underline;
    pair(initial.CellAt(5, 0), selected_fallback);
    partial.colors["core"].erase("inputmark");
    const auto unmarked = draw(partial, false, false);
    pair(unmarked.CellAt(5, 0), selected_fallback);

    const auto options = mcpp::parse_cli(std::array<std::string_view, 4>{
        "-C", "input=#abc,,none:inputmark=black,#abc,blink",
        "--colors=inputunchanged=brightgreen,color17,italic+underline:inputmark=,red,none",
        "literal:$[x]-é中"
    });
    check(options.colors.size() == 2U && options.paths == std::vector<std::string>{"literal:$[x]-é中"},
          "the short and long CLI options must preserve literal operands");
    const auto overrides = mcpp::parse_cli_colors(options.colors);
    for (const auto context : {mcpp::CliPaletteContext::panels, mcpp::CliPaletteContext::editor,
                              mcpp::CliPaletteContext::viewer, mcpp::CliPaletteContext::help}) {
        auto skin = source;
        mcpp::apply_cli_colors(skin, overrides, context);
        const auto& colors = skin.colors.at("core");
        check(colors.at("input").foreground == ftxui::Color::RGB(170, 187, 204) &&
              colors.at("input").background == core.at("input").background &&
              colors.at("input").attributes == 0U &&
              colors.at("inputmark").foreground == ftxui::Color::Black &&
              colors.at("inputmark").background == ftxui::Color::Red &&
              colors.at("inputmark").attributes == 0U &&
              colors.at("inputunchanged").foreground == ftxui::Color::GreenLight &&
              colors.at("inputunchanged").background ==
                  ftxui::Color(static_cast<ftxui::Color::Palette256>(17)) &&
              colors.at("inputunchanged").attributes == (SkinPair::italic | SkinPair::underline),
              "all three canonical raw roles must merge fields in launch order in each dialog palette");
        check(skin.normal.foreground == source.normal.foreground &&
              skin.normal.background == source.normal.background &&
              skin.normal.attributes == source.normal.attributes &&
              skin.dialog_focus.foreground == source.dialog_focus.foreground &&
              skin.dialog_focus.background == source.dialog_focus.background &&
              skin.dialog_focus.attributes == source.dialog_focus.attributes,
              "input-only CLI roles must not overwrite body or dialog-focus colors");
        const auto edited = draw(skin, false, true);
        pair(edited.CellAt(0, 0), colors.at("input"));
        pair(edited.CellAt(5, 0), colors.at("inputmark"));
        pair(edited.CellAt(23, 0), colors.at("input"));
        pair(draw(skin, true, false).CellAt(0, 0), colors.at("inputunchanged"));
    }
    for (const auto role : {"input", "inputmark", "inputunchanged"}) {
        check(mcpp::cli_color_roles().find(role) != std::string_view::npos,
              "CLI color help must advertise the three consumed input roles");
    }
    for (const auto role : {"dhotnormal", "dhotfocus"}) {
        bool rejected = false;
        try {
            static_cast<void>(mcpp::parse_cli_colors(std::array<std::string, 1>{
                std::string(role) + "=red"
            }));
        } catch (const std::invalid_argument& error) {
            rejected = std::string_view(error.what()).find("unsupported color role") != std::string_view::npos;
        }
        check(rejected, "separate unimplemented widget roles must remain explicitly rejected");
    }
    check(read(resource) == original_bytes && literal == "Aé中_$[]",
          "rendering and launch overrides must not rewrite original resources or literal input");
}

}  // namespace

int main(const int argc, char** argv) {
    try {
        if (argc != 2 || ::geteuid() != 1001U || !std::filesystem::exists("/.dockerenv") ||
            !std::filesystem::exists("/review/.operation-regressions-disposable")) {
            throw std::runtime_error("input styles require UID1001 in the owned review container");
        }
        const auto root = std::filesystem::absolute(argv[1]).lexically_normal();
        check(root.string().starts_with("/review/cases/input-styles/model/"),
              "use an owned input-styles/model case directory");
        ::umask(0077);
        std::filesystem::create_directories(root);
        ftxui::Terminal::SetColorSupport(ftxui::Terminal::TrueColor);
        exercise(root);
        std::cout << "input styles and CLI: passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
