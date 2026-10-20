#include "mcpp/cli_display.hpp"
#include "mcpp/command_line.hpp"
#include "mcpp/skin.hpp"
#include "mcpp/skin_glyphs_ui.hpp"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/terminal.hpp>

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {

void require(const bool condition, const std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

template <typename Function>
void rejects(Function function, const std::string_view message) {
    try {
        function();
    } catch (const std::invalid_argument& error) {
        require(std::string_view(error.what()).find(message) != std::string_view::npos, error.what());
        return;
    }
    throw std::runtime_error("expected a command-line diagnostic");
}

void options_and_display() {
    using namespace std::literals;
    for (const auto frontend : {mcpp::CliFrontend::manager, mcpp::CliFrontend::editor,
                               mcpp::CliFrontend::viewer, mcpp::CliFrontend::diff}) {
        std::vector<std::string_view> arguments{
            "-dab"sv, "-c"sv, "-Sforest"sv, "--nokeymap"sv, "-Klaunch.keys"sv,
            "-Uu"sv, "--printwd=result"sv
        };
        if (frontend == mcpp::CliFrontend::viewer || frontend == mcpp::CliFrontend::diff) arguments.push_back("one");
        if (frontend == mcpp::CliFrontend::diff) arguments.push_back("two");
        const auto parsed = mcpp::parse_cli(arguments, frontend);
        require(!parsed.mouse && parsed.stickchars && parsed.color == true &&
                parsed.skin == "forest" && parsed.keymap == "launch.keys" &&
                !parsed.keymap_disabled && parsed.subshell == false && parsed.print_directory == "result",
                "common flags and last-option precedence work in every frontend");
    }
    ftxui::Terminal::SetColorSupport(ftxui::Terminal::TrueColor);
    const std::vector<std::string> specifications{
        "normal=brightcyan,blue,bold+underline:selected=black,yellow,none",
        "normal=,color17:editnormal=white,rgb123"
    };
    const auto colors = mcpp::parse_cli_colors(specifications);
    auto skin = mcpp::find_skin("default");
    mcpp::apply_cli_colors(skin, colors, mcpp::CliPaletteContext::panels);
    require(skin.normal.foreground == ftxui::Color::CyanLight &&
            skin.normal.background == ftxui::Color(static_cast<ftxui::Color::Palette256>(17)) &&
            skin.normal.attributes == (mcpp::SkinPair::bold | mcpp::SkinPair::underline) &&
            skin.selected.background == ftxui::Color::YellowLight,
            "repeated color specifications preserve omitted fields and attributes");
    auto editor = skin;
    mcpp::apply_cli_colors(editor, colors, mcpp::CliPaletteContext::editor);
    require(editor.normal.foreground == ftxui::Color::White &&
            editor.normal.background == ftxui::Color(static_cast<ftxui::Color::Palette256>(67)),
            "editor-specific colors override only the editor palette");
    auto menu = mcpp::find_skin("default");
    mcpp::apply_cli_colors(menu, mcpp::parse_cli_colors(
        std::array<std::string, 2>{"menuinactive=gray", "pmenusel=white,blue"}),
        mcpp::CliPaletteContext::panels);
    require(menu.colors.at("menu").at("menuinactive").background == menu.menu.background,
            "foreground-only inactive menu colors retain the menu background");
    require(menu.colors.at("popupmenu").at("menusel").background == ftxui::Color::Blue &&
            menu.popup_selected.foreground == ftxui::Color::White,
            "CLI popup selection overrides use the native popup menu key");
    const std::array<const mcpp::Skin*, 1> palettes{&skin};
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(12), ftxui::Dimension::Fixed(3));
    const auto content = mcpp::glyph_border(mcpp::glyph_text("┌ café") | ftxui::color(skin.normal.foreground) |
        ftxui::bgcolor(skin.normal.background) | ftxui::borderStyled(ftxui::LIGHT));
    ftxui::Render(screen, mcpp::cli_display(content, true, true, palettes));
    require(screen.at(0, 0) == "+" && screen.at(1, 0) == "-" && screen.at(0, 1) == "|" &&
            screen.at(1, 1) == "┌" && screen.at(6, 1) == "é",
            "ASCII frames do not rewrite Unicode file text");
    for (int y = 0; y < screen.dimy(); ++y) {
        for (int x = 0; x < screen.dimx(); ++x) {
            require(screen.CellAt(x, y).foreground_color == ftxui::Color{} &&
                    screen.CellAt(x, y).background_color == ftxui::Color{},
                    "monochrome output contains no color settings");
        }
    }
    auto titled = ftxui::Screen::Create(ftxui::Dimension::Fixed(32), ftxui::Dimension::Fixed(4));
    const auto help = mcpp::glyph_border(ftxui::window(mcpp::glyph_text(" mcpp help: Text editor "),
                                    mcpp::glyph_text("╭ file │"), ftxui::ROUNDED));
    ftxui::Render(titled, mcpp::cli_display(help, false, true, palettes));
    require(titled.at(0, 0) == "+" && titled.at(31, 0) == "+" &&
            titled.at(0, 3) == "+" && titled.at(31, 3) == "+" &&
            titled.at(0, 1) == "|" && titled.at(1, 1) == "╭" && titled.at(8, 1) == "│",
            "titled help frames use ASCII corners without changing ordinary box-glyph text");
}

void information_and_errors() {
    using namespace std::literals;
    for (const auto frontend : {mcpp::CliFrontend::manager, mcpp::CliFrontend::editor,
                               mcpp::CliFrontend::viewer, mcpp::CliFrontend::diff}) {
        for (const auto flag : {"--help-all"sv, "--help-terminal"sv, "--help-color"sv,
                               "-V"sv, "-f"sv, "-F"sv, "--configure-options"sv}) {
            const auto parsed = mcpp::parse_cli(std::array{flag}, frontend);
            std::ostringstream output;
            mcpp::print_cli_action(parsed, output);
            require(parsed.action != mcpp::CliAction::run && !output.str().empty(),
                    "information selectors do not require files or configuration");
        }
    }
    rejects([] { static_cast<void>(mcpp::parse_cli(std::array{"--skin"sv})); }, "requires a value");
    rejects([] { static_cast<void>(mcpp::parse_cli(std::array{"--termcap"sv})); }, "does not use termcap");
    rejects([] { static_cast<void>(mcpp::parse_cli(std::array{"--ftplog=log"sv})); }, "remote providers");
    rejects([] { static_cast<void>(mcpp::parse_cli_colors(std::vector<std::string>{"commandlinemark=red"})); },
            "unsupported color role");
    rejects([] { static_cast<void>(mcpp::parse_cli_colors(std::vector<std::string>{"header=red,blue"})); },
            "only a foreground");
    rejects([] { static_cast<void>(mcpp::parse_cli_colors(std::vector<std::string>{"normal=color256"})); },
            "invalid color index");
}

void editor_operands(const std::filesystem::path& root) {
    using namespace std::literals;
    std::filesystem::create_directories(root);
    std::filesystem::current_path(root);
    {
        std::ofstream literal("literal:12");
        literal << "kept\n";
        require(static_cast<bool>(literal), "create literal colon fixture");
    }
    const auto parsed = mcpp::parse_cli(std::array{
        "--edit=first:9"sv, "+7"sv, "--"sv, "-draft"sv, "+notes"sv, "literal:12"sv
    });
    require(parsed.edit && parsed.first_line == 7U &&
            parsed.paths == std::vector<std::string>{"first:9", "-draft", "+notes", "literal:12"},
            "editor operands are never limited to two panel directories");
    require(mcpp::parse_editor_file(parsed.paths.front()).line == 9U &&
            mcpp::parse_editor_file(parsed.paths.back()).path == "literal:12" &&
            mcpp::parse_editor_file(parsed.paths.back()).line == 0U,
            "existing literal colon names take precedence over line suffixes");
    const std::string recovered = "/captured.ext2/undel://12:1";
    const auto literal_member = mcpp::parse_editor_file(recovered);
    require(literal_member.path == recovered && literal_member.line == 0U,
            "the undelete member colon is not an editor line suffix");
    for (const auto& suffix : {std::string(":9"), std::string(":9:")}) {
        const auto positioned = mcpp::parse_editor_file(recovered + suffix);
        require(positioned.path == recovered && positioned.line == 9U,
                "an explicit line suffix follows the complete undelete member");
    }
    const auto standalone = mcpp::parse_cli(std::array{"+4"sv, "--"sv, "-draft"sv},
                                           mcpp::CliFrontend::editor);
    require(standalone.first_line == 4U && standalone.paths == std::vector<std::string>{"-draft"},
            "standalone editor shares line and separator handling");
}

}  // namespace

int main(const int argc, char** argv) {
    try {
        if (argc != 3 || ::geteuid() != 1001U || !std::filesystem::exists("/.dockerenv") ||
            !std::filesystem::exists("/review/.operation-regressions-disposable")) {
            throw std::runtime_error("CLI checks require the existing UID1001 review container, a selector and an owned case path");
        }
        const auto root = std::filesystem::absolute(argv[2]).lexically_normal();
        require(root.string().starts_with("/review/cases/cli-remaining-"), "use an owned cli-remaining case directory");
        const std::string_view selected(argv[1]);
        if (selected == "options") options_and_display();
        else if (selected == "information") information_and_errors();
        else if (selected == "operands") editor_operands(root);
        else throw std::runtime_error("unknown CLI check selector");
        std::cout << selected << ": passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
