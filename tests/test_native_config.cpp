#include "mcpp/config.hpp"

#include <cstdlib>
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
        "skin=default\nkeymap=keys\nwrap_mode=false\nmouse_move_pages_viewer=true\nftpfs_retry_seconds=17\n"
        "[Panels]\nshow_dot_files=false\nmix_all_files=true\n"
        "[Layout]\nhorizontal_split=false\nvertical_equal=false\nhorizontal_equal=true\n"
        "left_panel_size=31\n[DiffView]\ndiff_quality=2\ndiff_ignore_case=true\ntab_size=4\n"
        "[Misc]\nspell_language=NONE\n[Future]\nkeep=some\\q=value\n");
    write(root / "panels.ini",
        "[New Left Panel]\ndisplay=listing\nlist_mode=user\nuser_format=half type name | size\n"
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
    require(config.panel_listing[0] == mcpp::ListingFormat::custom &&
        config.panel_sort[0] == mcpp::SortField::modified && config.panel_descending[0] == true,
        "panels.ini applies native listing and sorting");
    require(config.current_is_left == false && config.other_directory == "/retained dir" &&
        !config.viewer_wrap && config.viewer_mouse_page_scrolling == true &&
        config.diff_quality == 2U && config.diff_ignore_case && config.diff_tab_size == 4U &&
        config.editor_spelling_language == "NONE", "native directory, viewer, diff and spelling settings are applied");
    require(config.diagnostics.size() == 3U, "unsupported settings have explicit diagnostics");
    config.theme_override = "paper";
    config.keymap_override = "launch.keymap";
    config.keymap_disabled = true;
    config.subshell_override = false;
    config.editor_positions["/document"] = {2U, 3U, {1U}};
    config.editor_history = {"/saved\nname"};
    config.editor_macros["example"] = "kept\nmacro";
    config.save();
    const auto restored = mcpp::Config::load(path);
    require(restored.theme == "default" && !restored.theme_override && restored.keymap_file == "keys" &&
        !restored.keymap_override && !restored.keymap_disabled && !restored.subshell_override,
        "launch-only overrides do not replace saved preferences");
    require(restored.editor_positions.at("/document").row == 2U &&
        restored.editor_positions.at("/document").bookmarks == std::vector<std::size_t>{1U} &&
        restored.editor_history == config.editor_history && restored.editor_macros == config.editor_macros,
        "private editor state survives a native round-trip");
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
    write(directory / "ini", "[Midnight-Commander]\nconfirm_exit=true\n[Panels]\nshow_dot_files=false\n");
    require(mcpp::default_config_path() == directory / "ini", "native ini is discovered before legacy config");
    auto native = mcpp::Config::load(mcpp::default_config_path());
    require(native.confirm_exit && native.theme == "forest" && native.editor_tab_width == 6U &&
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
    const std::string invalid = "[Midnight-Commander]\neditor_tab_spacing=0\n";
    write(root / "ini", invalid);
    write(root / "panels.ini", "[New Left Panel]\nlist_mode=brief\n");
    const auto panels = read(root / "panels.ini");
    std::string diagnostic;
    try {
        retained = mcpp::Config::load(root / "ini");
    } catch (const std::exception& error) {
        diagnostic = error.what();
    }
    require(diagnostic.find("ini:2: [Midnight-Commander] editor_tab_spacing") != std::string::npos &&
        diagnostic.find("1 through 32") != std::string::npos, "invalid values identify the resource, line and option");
    require(retained.theme == "forest" && retained.editor_tab_width == 6U &&
        read(root / "ini") == invalid && read(root / "panels.ini") == panels,
        "failed loading changes neither accepted preferences nor source files");
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
        else throw std::runtime_error("unknown native configuration case");
        std::cout << selected << ": passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
