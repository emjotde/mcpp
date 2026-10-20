#include "mcpp/config.hpp"
#include "mcpp/extension.hpp"

#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>

namespace {

namespace fs = std::filesystem;

void require(const bool condition, const std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

void write(const fs::path& path, const std::string_view value) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path);
    output << value;
    require(static_cast<bool>(output), "write fixture");
}

std::string read(const fs::path& path) {
    std::ifstream input(path);
    require(static_cast<bool>(input), "read fixture");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void native_roundtrip(const fs::path& root) {
    const auto path = root / "ini";
    write(path,
        "# retained comment\n[Midnight-Commander]\nverbose=0\nconfirm_delete=false\n"
        "editor_tab_spacing=4\neditor_return_does_auto_indent=true\neditor_option_save_mode=1\n"
        "editor_option_auto_para_formatting=true\neditor_option_typewriter_wrap=false\n"
        "editor_drop_selection_on_copy=true\neditor_state_full_filename=false\n"
        "editor_stop_format_chars=-+*\\\\>\nmcview_remember_file_position=true\nfilepos_max_saved_entries=2\n"
        "skin=default\nkeymap=keys\nwrap_mode=false\nmouse_move_pages_viewer=true\nftpfs_retry_seconds=17\n"
        "[Panels]\nshow_dot_files=false\nmix_all_files=true\n"
        "[Layout]\nhorizontal_split=false\nvertical_equal=false\nhorizontal_equal=true\n"
        "left_panel_size=31\n[DiffView]\ndiff_quality=2\ndiff_ignore_case=true\ntab_size=4\ndiff_ignore_tws=true\n"
        "[Misc]\nspell_language=NONE\nsource_codepage=CP1251\n"
        "timeformat_recent=RECENT-%Y\ntimeformat_old=OLD-%Y\n[Future]\nkeep=some\\q=value\n");
    write(root / "panels.ini",
        "[New Left Panel]\ndisplay=tree\nlist_mode=user\nuser_format=half type name | size\n"
        "sort_order=mtime\nreverse=true\nbrief_cols=3\n"
        "[Dirs]\ncurrent_is_left=false\nother_dir=/retained\\sdir\n"
        "[Future panel]\nuntouched=literal\\\\value\n");
    auto config = mcpp::Config::load(path);
    require(!config.verbose_operation && !config.confirm_delete && !config.show_hidden &&
        !config.directories_first, "native general and panel settings are applied");
    require(config.editor_tab_width == 4U && config.editor_auto_indent &&
        config.editor_save_mode == "atomic" && config.editor_wrap_mode == 1U,
        "native editor settings use the existing editor options");
    require(!config.split_is_equal() && config.panel_left_size == 31 &&
        config.layout_horizontal_equal == true, "independent native split settings are retained");
    require(config.panel_display[0] == mcpp::PanelDisplay::tree &&
        config.panel_listing[0] == mcpp::ListingFormat::custom &&
        config.panel_sort[0] == mcpp::SortField::modified && config.panel_descending[0] == true,
        "panels.ini applies native listing and sorting");
    require(config.current_is_left == false && config.other_directory == "/retained dir" &&
        !config.viewer_wrap && config.viewer_mouse_page_scrolling == true &&
        config.diff_quality == 2U && config.diff_ignore_case && config.diff_tab_size == 4U &&
        config.editor_spelling_language == "NONE", "native directory, viewer, diff and spelling settings are applied");
    require(config.editor_drop_selection_on_copy && !config.editor_state_full_filename &&
        config.editor_stop_format_chars == "-+*\\>" && config.viewer_remember_file_position &&
        config.source_codepage == "CP1251" && config.diff_strip_trailing_cr &&
        config.max_saved_positions == 2U, "additional native editor, viewer, encoding and diff settings are applied");
    const auto reference = std::chrono::system_clock::from_time_t(1700000000);
    require(config.format_panel_time(reference - std::chrono::hours(1), reference).starts_with("RECENT-") &&
        config.format_panel_time(reference - std::chrono::hours(24 * 200), reference).starts_with("OLD-") &&
        config.format_panel_time(reference + std::chrono::hours(1), reference).starts_with("OLD-"),
        "native recent, old and future panel timestamps use their configured formats");
    require(config.diagnostics.size() == 3U, "unsupported settings have explicit diagnostics");
    config.theme_override = "paper";
    config.keymap_override = "launch.keymap";
    config.keymap_disabled = true;
    config.subshell_override = false;
    config.editor_positions["/document"] = {2U, 3U, {1U}};
    const std::string archive_location = "/example.zip/uzip://nested/document";
    config.viewer_positions[archive_location] = 123U;
    config.viewer_positions["/older"] = 9U;
    config.viewer_history = {archive_location};
    config.editor_history = {"/saved\nname"};
    config.editor_macros["example"] = "kept\nmacro";
    config.panel_display = {mcpp::PanelDisplay::listing, mcpp::PanelDisplay::quick_view};
    config.save();
    const auto restored = mcpp::Config::load(path);
    require(restored.theme == "default" && !restored.theme_override && restored.keymap_file == "keys" &&
        !restored.keymap_override && !restored.keymap_disabled && !restored.subshell_override,
        "launch-only overrides do not replace saved preferences");
    require(restored.editor_positions.at("/document").row == 2U &&
        restored.editor_positions.at("/document").bookmarks == std::vector<std::size_t>{1U} &&
        restored.editor_history == config.editor_history && restored.editor_macros == config.editor_macros,
        "private editor state survives a native round-trip");
    require(restored.panel_display[1] == mcpp::PanelDisplay::quick_view &&
        restored.viewer_positions.at(archive_location) == 123U && !restored.viewer_positions.contains("/older") &&
        restored.editor_drop_selection_on_copy && !restored.editor_state_full_filename &&
        restored.source_codepage == "CP1251" && restored.diff_strip_trailing_cr,
        "native displays and settings round-trip, keeping namespace-aware positions within the configured bound");
    require(read(path).find("keep=some\\q=value") != std::string::npos &&
        read(path).find("ftpfs_retry_seconds=17") != std::string::npos &&
        read(path).find("# retained comment") != std::string::npos &&
        read(root / "panels.ini").find("untouched=literal\\\\value") != std::string::npos,
        "unsupported sections, keys and their undecoded values are retained");
    struct stat status {};
    require(::stat(path.c_str(), &status) == 0 && (status.st_mode & 0777) == 0600 &&
        ::stat((root / "panels.ini").c_str(), &status) == 0 && (status.st_mode & 0777) == 0600,
        "native files are saved privately");
}

void legacy_coexistence(const fs::path& root) {
    const auto directory = root / "mcpp";
    const auto legacy = directory / "config";
    const std::string original =
        "theme=forest\neditor_tab_width=6\nleft_sort=extension\nleft_hidden=true\n"
        "saved_left_path=/retained\neditor_position=4,2:/saved\neditor_history=/retained-history\n"
        "future_flat=literal\\q\n";
    write(legacy, original);
    write(root / "mc" / "ini", "[Midnight-Commander]\nskin=must-not-be-imported\n");
    require(::setenv("XDG_CONFIG_HOME", root.c_str(), 1) == 0, "set isolated configuration root");
    require(mcpp::default_config_path() == legacy, "existing legacy configuration keeps its path");
    auto legacy_config = mcpp::Config::load(mcpp::default_config_path());
    legacy_config.save();
    require(read(legacy).find("future_flat=literal\\q") != std::string::npos,
        "flat configuration retains unknown options");
    write(legacy, original);
    write(directory / "ini", "[Midnight-Commander]\nconfirm_exit=true\n[Panels]\nshow_dot_files=false\n"
        "[New Right Panel]\ndisplay=info\n");
    require(mcpp::default_config_path() == directory / "ini", "native ini is discovered before legacy config");
    auto native = mcpp::Config::load(mcpp::default_config_path());
    require(native.confirm_exit && native.theme == "forest" && native.editor_tab_width == 6U &&
        native.panel_display[1] == mcpp::PanelDisplay::information &&
        native.panel_sort[0] == mcpp::SortField::extension && !native.panel_hidden[0].value_or(native.show_hidden) &&
        native.saved_left_path == "/retained" && native.editor_positions.at("/saved").row == 4U,
        "native overrides keep other existing pane and editor preferences");
    native.save();
    const auto restored = mcpp::Config::load(directory / "ini");
    require(restored.theme == "forest" && restored.editor_positions.at("/saved").column == 2U &&
        restored.saved_left_path == "/retained" && restored.editor_history == native.editor_history,
        "legacy preferences persist in native private settings without duplicating histories");
    require(read(legacy) == original &&
        read(root / "mc" / "ini") == "[Midnight-Commander]\nskin=must-not-be-imported\n",
        "native saving does not import or rewrite MC files or the legacy baseline");
    legacy_config.save();
    require(read(legacy).find("theme=forest") != std::string::npos,
        "an already loaded legacy configuration saves to its original resource");
}

void invalid_transaction(const fs::path& root) {
    write(root / "good", "theme=forest\neditor_tab_width=6\n");
    auto retained = mcpp::Config::load(root / "good");
    const std::string invalid = "[Midnight-Commander]\neditor_tab_spacing=4\n";
    write(root / "ini", invalid);
    write(root / "panels.ini", "[New Left Panel]\ndisplay=information\n");
    const auto panels = read(root / "panels.ini");
    std::string diagnostic;
    try {
        retained = mcpp::Config::load(root / "ini");
    } catch (const std::exception& error) {
        diagnostic = error.what();
    }
    require(diagnostic.find("panels.ini:2: [New Left Panel] display") != std::string::npos &&
        diagnostic.find("unsupported saved panel display: information") != std::string::npos,
        "invalid values identify the resource, line and option without guessing display aliases");
    require(retained.theme == "forest" && retained.editor_tab_width == 6U &&
        read(root / "ini") == invalid && read(root / "panels.ini") == panels,
        "failed loading changes neither accepted preferences nor source files");
}

void remaining_four_native(const fs::path& root) {
    const auto path = root / "ini";
    write(path, "# keep four-key comment\n[Midnight-Commander]\n"
        "double_click_speed=50\nconfirm_view_dir=false\nuse_file_to_guess_type=false\n"
        "copymove_persistent_attr=false\n[Future]\nkeep=literal\\q\n");
    auto config = mcpp::Config::load(path);
    require(config.double_click_timeout() == std::chrono::milliseconds(50) &&
        config.confirm_view_dir.has_value() && !*config.confirm_view_dir &&
        !config.use_file_to_guess_type && config.copymove_persistent_attr.has_value() &&
        !*config.copymove_persistent_attr, "four native values are applied, including explicit false");
    auto rules = mcpp::ExtensionRules::parse(
        "[mc.ext.ini]\nVersion=4.0\n[typed]\nType=.*\nView=typed-command\n"
        "[named]\nRegex=[.]txt$\nView=named-command\n");
    mcpp::Entry entry;
    entry.kind = mcpp::EntryKind::file;
    entry.name = "note.txt";
    entry.path = "/note.txt";
    std::size_t probes = 0;
    const auto probe = [&](mcpp::ExtensionType) {
        ++probes;
        return std::string("ASCII text");
    };
    const auto named = rules.match(entry, mcpp::ExtensionAction::view, true, probe,
                                   config.use_file_to_guess_type);
    require(probes == 0U && named && named->command == "named-command",
        "disabled guessing skips Type matching without disabling filename rules or probing");
    const auto typed = rules.match(entry, mcpp::ExtensionAction::view, true, probe, true);
    require(probes == 1U && typed && typed->command == "typed-command",
        "enabled guessing still probes and applies the Type rule");
    config.save();
    const auto restored = mcpp::Config::load(path);
    require(restored.double_click_timeout() == std::chrono::milliseconds(50) &&
        restored.confirm_view_dir == false && !restored.use_file_to_guess_type &&
        restored.copymove_persistent_attr == false, "four native values survive saving");
    require(read(path).find("# keep four-key comment") != std::string::npos &&
        read(path).find("keep=literal\\q") != std::string::npos,
        "four-key saves preserve unknown content");
    std::cout << "remaining-four native: passed\n";
}

void remaining_four_legacy(const fs::path& root) {
    const auto path = root / "config";
    write(path, "theme=forest\ncommand_history=retained\nfuture_flat=literal\\q\n");
    auto config = mcpp::Config::load(path);
    require(!config.double_click_speed && !config.confirm_view_dir &&
        config.use_file_to_guess_type && !config.copymove_persistent_attr &&
        config.double_click_timeout() == std::chrono::milliseconds(400),
        "absent fields retain legacy timing, F3, probe and attribute defaults");
    config.save();
    const auto raw = read(path);
    require(raw.find("double_click_speed=") == std::string::npos &&
        raw.find("confirm_view_dir=") == std::string::npos &&
        raw.find("copymove_persistent_attr=") == std::string::npos &&
        raw.find("future_flat=literal\\q") != std::string::npos,
        "legacy saves retain absence-dependent behavior and unknown data");
    const auto restored = mcpp::Config::load(path);
    require(restored.command_history == config.command_history &&
        restored.use_file_to_guess_type && !restored.confirm_view_dir &&
        restored.double_click_timeout() == std::chrono::milliseconds(400),
        "legacy preferences survive the four-key extension");
    std::cout << "remaining-four legacy: passed\n";
}

void remaining_four_invalid(const fs::path& root) {
    write(root / "good", "theme=forest\ndouble_click_speed=200\nconfirm_view_dir=true\n"
        "use_file_to_guess_type=true\ncopymove_persistent_attr=true\n");
    auto retained = mcpp::Config::load(root / "good");
    for (const auto& [key, value] : {
             std::pair{"double_click_speed", "-1"}, std::pair{"confirm_view_dir", "maybe"},
             std::pair{"use_file_to_guess_type", "maybe"}, std::pair{"copymove_persistent_attr", "maybe"}}) {
        const auto invalid = std::string("[Midnight-Commander]\n") + key + '=' + value + '\n';
        write(root / "ini", invalid);
        std::string diagnostic;
        try {
            retained = mcpp::Config::load(root / "ini");
        } catch (const std::exception& error) {
            diagnostic = error.what();
        }
        require(diagnostic.find(std::string("ini:2: [Midnight-Commander] ") + key) != std::string::npos,
            "invalid four-key values identify their source and setting");
        require(retained.theme == "forest" && retained.double_click_timeout() == std::chrono::milliseconds(200) &&
            retained.confirm_view_dir == true && retained.use_file_to_guess_type &&
            retained.copymove_persistent_attr == true && read(root / "ini") == invalid,
            "invalid values retain accepted preferences and original bytes");
    }
    std::cout << "remaining-four invalid: passed\n";
}

}  // namespace

int main(const int argc, char** argv) {
    try {
        if (argc != 3) throw std::runtime_error("usage: mcpp_native_config_tests CASE WORKDIR");
        const fs::path root(argv[2]);
        const std::string_view selected(argv[1]);
        if (selected == "native") native_roundtrip(root);
        else if (selected == "legacy") legacy_coexistence(root);
        else if (selected == "invalid") invalid_transaction(root);
        else if (selected == "remaining-four") {
            remaining_four_native(root / "native");
            remaining_four_legacy(root / "legacy");
            remaining_four_invalid(root / "invalid");
        }
        else throw std::runtime_error("unknown native configuration case");
        std::cout << selected << ": passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
