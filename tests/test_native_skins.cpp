#include "mcpp/file_highlight.hpp"
#include "mcpp/skin.hpp"

#include <ftxui/screen/terminal.hpp>

#include <algorithm>
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

void write(const std::filesystem::path& path, const std::string_view value) {
    std::ofstream output(path);
    output << value;
    check(static_cast<bool>(output), "write native skin fixture");
}

void native_colors_and_rules(const std::filesystem::path& root) {
    ftxui::Terminal::SetColorSupport(ftxui::Terminal::TrueColor);
    const auto& builtin = mcpp::builtin_skins();
    const auto* stable = &mcpp::find_skin("forest");
    check(builtin.size() == 16U, "all sixteen original skins remain available");
    check(&mcpp::find_skin("classic") == &mcpp::find_skin("grayscale") &&
          &mcpp::find_skin("gray") == &mcpp::find_skin("grayscale") &&
          &mcpp::find_skin("blue") == &mcpp::find_skin("default"), "legacy aliases are unchanged");
    std::filesystem::create_directories(root / "skins");
    const auto skin_path = root / "skins/independent.ini";
    write(skin_path, R"([skin]
description=Independent L23 sample
truecolors=true
[aliases]
ink=brightcyan
focus=yellow
emphasis=bold+underline
accent=ink
[core]
_default_=lightgray;#102030;italic
selected=black;accent;none
marked=focus;;emphasis
shadow=black;black;none
[dialog]
_default_=white;#304050;none
dfocus=black;focus;none
[menu]
_default_=white;#405060;none
menusel=black;accent;none
menuhot=focus;;underline
[filehighlight]
notes=#aabbcc;;none
tools=brightgreen;;bold
folders=brightblue;;none
)");
    write(root / "filehighlight.ini", R"([tools]
type=FILE_EXE
regexp=[
[folders]
type=DIR
[notes]
extensions=l23;txt;
extensions_case=false
)");
    const auto skin = mcpp::load_skin("independent", root);
    check(skin.label == "Independent L23 sample" && skin.resource == skin_path,
          "a discovered skin loads metadata and its resource path");
    check(skin.selected.foreground == ftxui::Color::Black &&
          skin.selected.background == ftxui::Color::CyanLight && skin.selected.attributes == 0U,
          "color aliases and explicit none override inherited attributes");
    check(skin.dialog_focus.background == ftxui::Color::YellowLight &&
          skin.dialog_focus.attributes == 0U, "native dialog focus uses dfocus");
    const auto& marked = mcpp::skin_color(skin, "core", "marked", skin.normal);
    check(marked.background == ftxui::Color::RGB(16, 32, 48) &&
          marked.attributes == (mcpp::SkinPair::bold | mcpp::SkinPair::underline),
          "omitted color fields inherit the section default and attribute aliases expand");
    const auto rules = mcpp::FileHighlightRules::load(root);
    mcpp::Entry note;
    note.name = "notes.TXT";
    note.kind = mcpp::EntryKind::file;
    note.permissions = std::filesystem::perms::owner_read;
    const auto note_color = rules.color_for(note, skin);
    check(note_color && note_color->foreground == ftxui::Color::RGB(170, 187, 204) &&
          note_color->background == skin.normal.background && note_color->attributes == 0U,
          "native extension rules actually select the skin group color");
    auto tool = note;
    tool.name = "run.l23";
    tool.permissions |= std::filesystem::perms::owner_exec;
    const auto tool_color = rules.color_for(tool, skin);
    check(tool_color && tool_color->foreground == ftxui::Color::GreenLight &&
          tool_color->attributes == mcpp::SkinPair::bold,
          "type rules have native priority over regexp and extension selectors");
    auto folder = note;
    folder.name = "notes.l23";
    folder.kind = mcpp::EntryKind::directory;
    check(rules.color_for(folder, skin)->foreground == ftxui::Color::BlueLight,
          "directory rules affect entry types, not merely extensions");
    check(!rules.color_for(folder, mcpp::find_skin("forest")),
          "groups without user-defined colors leave the builtin type palette unchanged");
    const auto choices = mcpp::available_skins(root);
    check(choices.size() >= 17U &&
          std::ranges::find(choices, skin_path.string(), &mcpp::SkinChoice::id) != choices.end(),
          "Appearance discovers the user-provided skin alongside every builtin");
    check(mcpp::find_skin("paper").syntax[1] != mcpp::find_skin("forest").syntax[1],
          "builtin syntax colors retain their light/dark adaptation");
    ftxui::Terminal::SetColorSupport(ftxui::Terminal::Palette16);
    static_cast<void>(mcpp::builtin_skins());
    ftxui::Terminal::SetColorSupport(ftxui::Terminal::TrueColor);
    check(&mcpp::find_skin("forest") == stable, "terminal palette caches retain stable references");
    const auto legacy = root / "legacy";
    std::filesystem::create_directories(legacy);
    write(legacy / "highlighting", "*.TXT=16,32,48\n");
    check(mcpp::FileHighlightRules::load(legacy).color_for(note, mcpp::find_skin("paper"))->foreground ==
          ftxui::Color::RGB(16, 32, 48), "explicit legacy filename colors work with every skin");
}

void native_widget_metadata_and_syntax(const std::filesystem::path& root) {
    const auto path = root / "skins/widget-metadata.ini";
    write(path, R"([core]
_default_=white;#172330;none
selected=black;#a0c040;inverse
[menu]
_default_=white;#1a3344;none
menusel=black;#a0c040;none
[popupmenu]
_default_=white;#354050;none
menusel=#ffdd44;#620052;bold
menutitle=#9fe0c2;#182f2a;underline
[editor]
_default_=black;#f0f0dd;none
window-state-char=*
window-close-char=x
[Lines]
vert=|
[widget-panel]
sort-up-char=^
sort-down-char=v
[widget-common]
sort-sign-up=^
[widget-scrollbar]
current-char=#
[widget-scollbar]
first-vert-char=^
[widget-editor]
window-close-char=x
)");
    const auto skin = mcpp::load_skin(path.string(), root);
    check(skin.normal.background == ftxui::Color::RGB(23, 35, 48) &&
          skin.selected.attributes == mcpp::SkinPair::reverse,
          "native inverse is accepted without treating widget glyphs as colors");
    check(skin.popup_selected.foreground == ftxui::Color::RGB(255, 221, 68) &&
          skin.popup_selected.background == ftxui::Color::RGB(98, 0, 82) &&
          skin.popup_selected.attributes == mcpp::SkinPair::bold,
          "native popup selection is independent of the main menu selection");
    check(skin.glyphs.has(mcpp::SkinGlyph::vert) && skin.glyphs.has(mcpp::SkinGlyph::sort_up) &&
          skin.glyphs.has(mcpp::SkinGlyph::window_close) &&
          skin.warning.find("[widget-scrollbar] current-char") != std::string::npos &&
          !skin.colors.contains("widget-panel"),
          "supported glyphs load and unrendered roles are reported instead of becoming colors");
    check(skin.light && skin.syntax[1] == mcpp::find_skin("paper").syntax[1] &&
          skin.syntax[2] == mcpp::find_skin("paper").syntax[2] &&
          skin.syntax[5] == mcpp::find_skin("paper").syntax[5],
          "syntax fallback follows the editor background, not the dark panel background");
    const auto rejected = [&](const std::string_view contents, const std::string_view message) {
        const auto invalid = root / "skins/invalid-widget-probe.ini";
        write(invalid, contents);
        try {
            static_cast<void>(mcpp::load_skin(invalid.string(), root));
        } catch (const std::exception& error) {
            check(std::string_view(error.what()).find(message) != std::string_view::npos,
                  "malformed color settings retain their diagnostic");
            return;
        }
        throw std::runtime_error("invalid native skin was silently accepted");
    };
    rejected("[core]\n_default_=not-a-color;black\n[widget-panel]\nsort-up-char=^\n", "unknown color");
    rejected("[widget-panel]\nsort-up-char=^\n", "no color definitions");
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if ((argc != 2 && argc != 3) || ::geteuid() != 1001U || !std::filesystem::exists("/.dockerenv") ||
            !std::filesystem::exists("/review/.operation-regressions-disposable")) {
            throw std::runtime_error("native skin check requires the existing UID1001 isolated review container and a case directory");
        }
        const auto root = std::filesystem::absolute(argv[1]).lexically_normal();
        check(root.string().starts_with("/review/cases/native-skins/"), "use the owned native-skins case directory");
        std::filesystem::create_directories(root);
        if (argc == 3) {
            ftxui::Terminal::SetColorSupport(ftxui::Terminal::TrueColor);
            std::size_t count = 0;
            for (const auto& entry : std::filesystem::directory_iterator(argv[2])) {
                if (!entry.is_regular_file() || entry.path().extension() != ".ini") continue;
                const auto skin = mcpp::load_skin(entry.path().string(), root);
                check(!skin.resource.empty(), "an explicit external path must not resolve to a builtin");
                ++count;
            }
            check(count != 0U, "external skin directory contains no skin files");
            std::cout << "external skin loading passed: " << count << '\n';
            return 0;
        }
        native_colors_and_rules(root);
        native_widget_metadata_and_syntax(root);
        std::cout << "native skin colors and filename rules passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
