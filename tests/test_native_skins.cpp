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

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2 || ::geteuid() != 1001U || !std::filesystem::exists("/.dockerenv") ||
            !std::filesystem::exists("/review/.operation-regressions-disposable")) {
            throw std::runtime_error("native skin check requires the existing UID1001 isolated review container and a case directory");
        }
        const auto root = std::filesystem::absolute(argv[1]).lexically_normal();
        check(root.string().starts_with("/review/cases/native-skins/"), "use the owned native-skins case directory");
        std::filesystem::create_directories(root);
        native_colors_and_rules(root);
        std::cout << "native skin colors and filename rules passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
