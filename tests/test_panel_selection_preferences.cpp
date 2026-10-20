#include "mcpp/config.hpp"
#include "mcpp/keymap.hpp"
#include "mcpp/keymap_input.hpp"
#include "panel_selection_keys.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
using mcpp::detail::PanelGroupSelection;
using mcpp::detail::panel_group_selection_action;
using mcpp::detail::panel_group_selection_key;

void check(const bool condition, const std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

void write(const fs::path& path, const std::string_view contents) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path);
    output << contents;
    check(static_cast<bool>(output), "write original selection preference fixture");
}

std::string read(const fs::path& path) {
    std::ifstream input(path);
    check(static_cast<bool>(input), "read original selection preference fixture");
    return {std::istreambuf_iterator<char>(input), {}};
}

void configuration(const fs::path& root) {
    const auto native = root / "native/ini";
    write(native,
        "# original selection settings\n[Midnight-Commander]\n"
        "alternate_plus_minus=YeS\nonly_leading_plus_minus=0\n"
        "[Future]\nkeep=literal\\q\n");
    auto config = mcpp::Config::load(native);
    check(config.alternate_plus_minus == true && config.only_leading_plus_minus == false &&
          config.diagnostics.size() == 1U,
          "native selection settings must apply both boolean states");
    config.save();
    auto restored = mcpp::Config::load(native);
    check(restored.alternate_plus_minus == true && restored.only_leading_plus_minus == false &&
          read(native).find("# original selection settings") != std::string::npos &&
          read(native).find("[Future]\nkeep=literal\\q") != std::string::npos,
          "native saves must retain false, comments and unknown settings");

    const auto flat = root / "flat/config";
    write(flat, "alternate_plus_minus=false\nonly_leading_plus_minus=true\n");
    config = mcpp::Config::load(flat);
    config.save();
    restored = mcpp::Config::load(flat);
    check(restored.alternate_plus_minus == false && restored.only_leading_plus_minus == true,
          "flat saves must retain explicit selection booleans");

    const auto absent = root / "absent/ini";
    write(absent, "# legacy profile\n[Midnight-Commander]\nconfirm_exit=false\n");
    config = mcpp::Config::load(absent);
    check(!config.alternate_plus_minus && !config.only_leading_plus_minus,
          "absent selection settings must remain disengaged");
    config.save();
    const auto saved = read(absent);
    check(saved.find("alternate_plus_minus=") == std::string::npos &&
          saved.find("only_leading_plus_minus=") == std::string::npos,
          "saving an old profile must not opt into selection settings");

    for (const auto& contents : {
             std::string("[Midnight-Commander]\nalternate_plus_minus=maybe\n"),
             std::string("[Midnight-Commander]\nonly_leading_plus_minus=2\n"),
             std::string("alternate_plus_minus=2\n"),
             std::string("only_leading_plus_minus=maybe\n")}) {
        const auto invalid = root / "invalid/config";
        write(invalid, contents);
        bool rejected = false;
        try {
            static_cast<void>(mcpp::Config::load(invalid));
        } catch (const std::exception& error) {
            rejected = std::string_view(error.what()).find("invalid boolean") != std::string_view::npos;
        }
        check(rejected && read(invalid) == contents,
              "invalid selection booleans must fail without modifying the profile");
    }
}

void key_decisions() {
    using ftxui::Event;
    const std::vector<std::pair<Event, std::string_view>> keys{
        {Event::Character('+'), "plus"}, {Event::Character('-'), "minus"},
        {Event::Special("\x1bOk"), "kpplus"}, {Event::Special("\x1bOm"), "kpminus"},
        {Event::Special("\x1b+"), "alt-plus"}, {Event::Special("\x1b-"), "alt-minus"},
    };
    for (const auto& [event, key] : keys) {
        check(mcpp::keymap_event_name(event) == key,
              "plain, keypad and Alt +/- must retain their distinct key identities");
    }

    for (const bool alternate : {false, true}) {
        for (const bool leading : {false, true}) {
            for (const std::string_view draft : {"", "ab cd", "ab '界'\\ suffix"}) {
                for (const auto& [event, key] : keys) {
                    const bool plain = key == "plus" || key == "minus";
                    const bool selects = !plain || (!alternate && (!leading || draft.empty()));
                    const auto action = key.ends_with("plus")
                        ? PanelGroupSelection::select : PanelGroupSelection::unselect;
                    check(panel_group_selection_key(
                              *mcpp::keymap_event_name(event), draft, alternate, leading) ==
                              (selects ? action : PanelGroupSelection::none),
                          "only plain +/- depend on the two settings and whole-draft emptiness");
                }
            }
        }
    }
    check(panel_group_selection_key("plus", "", std::nullopt, std::nullopt) ==
              PanelGroupSelection::select &&
          panel_group_selection_key("minus", "", std::nullopt, std::nullopt) ==
              PanelGroupSelection::unselect &&
          panel_group_selection_key("plus", "draft", std::nullopt, std::nullopt) ==
              PanelGroupSelection::none &&
          panel_group_selection_key("minus", "draft", std::nullopt, std::nullopt) ==
              PanelGroupSelection::none,
          "absent settings must preserve the legacy empty-draft plain +/- behavior");
    for (const auto key : {"kpplus", "kpminus", "alt-plus", "alt-minus"}) {
        check(panel_group_selection_key(key, "", std::nullopt, std::nullopt) ==
                  PanelGroupSelection::none,
              "an old profile must not gain unbound keypad or Alt selection shortcuts");
        check(panel_group_selection_key(key, "draft", false, std::nullopt) !=
                  PanelGroupSelection::none &&
              panel_group_selection_key(key, "draft", std::nullopt, true) !=
                  PanelGroupSelection::none,
              "either explicit preference must enable draft-independent keypad and Alt selection");
    }
    for (const auto key : {"asterisk", "alt-asterisk", "slash", "ctrl-plus"}) {
        check(panel_group_selection_key(key, "", false, false) == PanelGroupSelection::none,
              "selection preferences must not alias unrelated actions or key modifiers");
    }
}

void explicit_actions() {
    const auto keymap = mcpp::Keymap::parse(
        "[filemanager]\nUnselect=plus;alt-plus;kpplus\nSelect=minus;alt-minus;kpminus\n");
    for (const auto key : {"plus", "alt-plus", "kpplus", "minus", "alt-minus", "kpminus"}) {
        const auto action = keymap.lookup(mcpp::KeymapContext::filemanager, key);
        check(action.has_value(), "original swapped user bindings must resolve");
        const auto expected = std::string_view(key).ends_with("plus")
            ? PanelGroupSelection::unselect : PanelGroupSelection::select;
        check(panel_group_selection_action(*action) == expected,
              "explicit user Select/Unselect actions must bypass physical-key preferences");
    }
    check(panel_group_selection_action("SelectInvert") == PanelGroupSelection::none &&
          panel_group_selection_action("MarkAll") == PanelGroupSelection::none,
          "direct group dispatch must not alias other selection actions");
}

}  // namespace

int main(const int argc, char** argv) {
    try {
        check(argc == 2 && ::geteuid() == 1001U && fs::exists("/.dockerenv") &&
              fs::exists("/review/.operation-regressions-disposable"),
              "selection preference tests require UID1001 in the owned review container");
        const auto root = fs::absolute(argv[1]).lexically_normal();
        check(root.string().starts_with("/review/cases/selection-0d2d/model/"),
              "use an owned selection preference model directory");
        ::umask(0077);
        fs::create_directories(root);
        check(!fs::is_symlink(root), "model directory must not be a symlink");
        configuration(root);
        key_decisions();
        explicit_actions();
        std::cout << "selection native/flat/absent/invalid settings and narrow key dispatch: passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
