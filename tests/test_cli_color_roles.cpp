#include "mcpp/cli_display.hpp"
#include "mcpp/skin.hpp"

#include <ftxui/screen/terminal.hpp>

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>

namespace {

void require(const bool condition, const std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

void rendered_roles(const std::filesystem::path& root) {
    std::filesystem::create_directories(root);
    const auto resource = root / "roles.ini";
    {
        std::ofstream output(resource);
        output << R"([aliases]
ink=#abc
emphasis=bold+underline
[core]
_default_=ink;#102030;italic
[dialog]
_default_=white;#445566;none
[error]
_default_=white;red;none
[menu]
_default_=lightgray;#233344;none
menuinactive=gray;;emphasis
[popupmenu]
_default_=white;#354050;none
menusel=yellow;#620052;bold
[editor]
_default_=black;#f0f0dd;none
editlinestate=white;cyan;bold
)";
        require(static_cast<bool>(output), "write the single native skin fixture");
    }
    const auto source = mcpp::load_skin(resource.string(), root);
    require(source.normal.foreground == ftxui::Color::RGB(170, 187, 204) &&
            source.colors.at("menu").at("menuinactive").attributes ==
                (mcpp::SkinPair::bold | mcpp::SkinPair::underline),
            "native aliases still use the shared color and attribute parser");
    const auto overrides = mcpp::parse_cli_colors(std::array<std::string, 1>{
        "pmenunormal=#abc,color17,bold:pmenunormal=,#123456:"
        "pmenusel=black,rgb123,inverse:pmenutitle=yellow,,none:"
        "menuinactive=,gray7,italic+underline:"
        "dtitle=brightgreen,,underline:errdtitle=yellow,,bold:"
        "errdfocus=white,red,blink:"
        "editlinestate=brightmagenta,,underline:editlinestate=,color18"
    });
    auto panels = source;
    mcpp::apply_cli_colors(panels, overrides, mcpp::CliPaletteContext::panels);
    const auto& popup = panels.colors.at("popupmenu");
    const auto& normal = popup.at("_default_");
    require(normal.foreground == ftxui::Color::RGB(170, 187, 204) &&
            normal.background == ftxui::Color::RGB(18, 52, 86) &&
            normal.attributes == mcpp::SkinPair::bold,
            "popup normal overrides merge omitted fields in launch order");
    require(panels.popup_selected.foreground == ftxui::Color::Black &&
            panels.popup_selected.background ==
                ftxui::Color(static_cast<ftxui::Color::Palette256>(67)) &&
            panels.popup_selected.attributes == mcpp::SkinPair::reverse,
            "popup selection updates its actual pair using the inverse alias");
    require(popup.at("menutitle").foreground == ftxui::Color::YellowLight &&
            popup.at("menutitle").background == normal.background &&
            popup.at("menutitle").attributes == 0U &&
            !popup.contains("pmenunormal") && !popup.contains("pmenusel") &&
            !popup.contains("pmenutitle"),
            "popup titles inherit the popup default and use native canonical keys");
    require(panels.menu.foreground == source.menu.foreground &&
            panels.menu.background == source.menu.background &&
            panels.menu_selected.background == source.menu_selected.background,
            "popup colors do not replace main menu colors");
    const auto& inactive = panels.colors.at("menu").at("menuinactive");
    require(inactive.foreground == source.disabled &&
            inactive.background == ftxui::Color(static_cast<ftxui::Color::Palette256>(239)) &&
            inactive.attributes == (mcpp::SkinPair::italic | mcpp::SkinPair::underline),
            "disabled menu labels accept a background and attributes without losing their foreground");
    require(panels.colors.at("dialog").at("dtitle").foreground == ftxui::Color::GreenLight &&
            panels.colors.at("dialog").at("dtitle").background == source.dialog.background &&
            panels.colors.at("dialog").at("dtitle").attributes == mcpp::SkinPair::underline &&
            panels.colors.at("error").at("errdtitle").foreground == ftxui::Color::YellowLight &&
            panels.error_focus.attributes == mcpp::SkinPair::blink,
            "rendered dialog titles and canonical error focus keep their own pairs");
    require(panels.colors.at("editor").at("editlinestate").background == ftxui::Color::Cyan,
            "editor gutter overrides do not leak into the panel palette");
    auto editor = source;
    editor.normal = mcpp::skin_color(editor, "editor", "_default_", editor.normal);
    const auto editor_normal = editor.normal;
    mcpp::apply_cli_colors(editor, overrides, mcpp::CliPaletteContext::editor);
    const auto& gutter = editor.colors.at("editor").at("editlinestate");
    require(gutter.foreground == ftxui::Color::MagentaLight &&
            gutter.background == ftxui::Color(static_cast<ftxui::Color::Palette256>(18)) &&
            gutter.attributes == mcpp::SkinPair::underline &&
            editor.normal.foreground == editor_normal.foreground &&
            editor.normal.background == editor_normal.background,
            "editor gutter colors merge separately from the editor body");
    const auto reloaded = mcpp::load_skin(resource.string(), root);
    require(reloaded.colors.at("popupmenu").at("_default_").background ==
                source.colors.at("popupmenu").at("_default_").background &&
            !reloaded.colors.at("popupmenu").contains("menutitle"),
            "launch color overrides never rewrite the native skin");
}

void preservation() {
    const auto overrides = mcpp::parse_cli_colors(std::array<std::string, 1>{
        "menuinactive=,blue,underline"
    });
    for (const auto id : {"default", "gray"}) {
        auto skin = mcpp::find_skin(id);
        const auto disabled = skin.disabled;
        const auto menu = skin.menu;
        mcpp::apply_cli_colors(skin, overrides, mcpp::CliPaletteContext::panels);
        require(skin.disabled == disabled &&
                skin.colors.at("menu").at("menuinactive").foreground == disabled &&
                skin.menu.foreground == menu.foreground && skin.menu.background == menu.background,
                "default and gray disabled text keep their contrast and main menu pair");
    }
    auto help = mcpp::find_skin("gray");
    const auto normal = help.normal;
    mcpp::apply_cli_colors(help, mcpp::parse_cli_colors(std::array<std::string, 1>{
        "helplink=#abc,default,inverse+blink:errdhotfocus=yellow,green,italic"
    }), mcpp::CliPaletteContext::help);
    require(help.normal.foreground == normal.foreground &&
            help.normal.background == normal.background &&
            help.colors.at("help").at("helplink").foreground == ftxui::Color::RGB(170, 187, 204) &&
            help.colors.at("help").at("helplink").background == ftxui::Color{} &&
            help.colors.at("help").at("helplink").attributes ==
                (mcpp::SkinPair::reverse | mcpp::SkinPair::blink),
            "map-only roles do not overwrite body colors");
    require(help.error_focus.foreground == ftxui::Color::YellowLight &&
            help.error_focus.background == ftxui::Color::Green &&
            help.error_focus.attributes == mcpp::SkinPair::italic,
            "the existing error-hot-focus spelling remains a whole-focus alias");
    for (const auto& [specification, diagnostic] : std::array{
            std::pair{"inputhistory=red", "unsupported color role"},
            std::pair{"header=red,blue", "only a foreground"},
            std::pair{"normal=color256", "invalid color index"}}) {
        bool rejected = false;
        try {
            static_cast<void>(mcpp::parse_cli_colors(std::array<std::string, 1>{specification}));
        } catch (const std::invalid_argument& error) {
            rejected = std::string_view(error.what()).find(diagnostic) != std::string_view::npos;
        }
        require(rejected, "unrendered roles and invalid fields retain explicit diagnostics");
    }
}

}  // namespace

int main(const int argc, char** argv) {
    try {
        if (argc != 3 || ::geteuid() != 1001U || !std::filesystem::exists("/.dockerenv") ||
            !std::filesystem::exists("/review/.operation-regressions-disposable")) {
            throw std::runtime_error("CLI color models require the UID1001 review container, selector and case path");
        }
        const auto root = std::filesystem::absolute(argv[2]).lexically_normal();
        require(root.string().starts_with("/review/cases/cli-color-roles/"),
                "use an owned cli-color-roles case directory");
        ftxui::Terminal::SetColorSupport(ftxui::Terminal::TrueColor);
        const std::string_view selected(argv[1]);
        if (selected == "roles") rendered_roles(root);
        else if (selected == "preservation") preservation();
        else throw std::runtime_error("unknown CLI color model selector");
        std::cout << selected << ": passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
