#pragma once

#include "mcpp/entry.hpp"

#include <cstddef>
#include <array>
#include <filesystem>
#include <optional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace mcpp {

struct SavedEditorPosition {
    std::size_t row{0};
    std::size_t column{0};
    std::vector<std::size_t> bookmarks;
};

struct Config {
    bool show_hidden{true};
    bool confirm_delete{true};
    bool confirm_overwrite{true};
    bool confirm_execute{false};
    bool confirm_exit{false};
    bool confirm_hotlist_delete{false};
    bool confirm_history_cleanup{true};
    bool directories_first{true};
    SortField sort{SortField::name};
    bool sort_descending{false};
    std::array<ListingFormat, 2> panel_listing{ListingFormat::full, ListingFormat::full};
    std::array<std::string, 2> panel_columns{"half type name | size | perm", "half type name | size | perm"};
    std::array<std::size_t, 2> panel_brief_columns{2, 2};
    std::array<bool, 2> panel_user_mini_status{false, false};
    std::array<std::array<std::string, 4>, 2> panel_status_formats{{
        {"half type name | size | perm", "half type name | size | perm",
         "half type name | size | perm", "half type name | size | perm"},
        {"half type name | size | perm", "half type name | size | perm",
         "half type name | size | perm", "half type name | size | perm"}
    }};
    std::array<std::string, 2> panel_encoding;
    std::array<std::optional<SortField>, 2> panel_sort;
    std::array<std::optional<bool>, 2> panel_descending;
    std::array<std::optional<bool>, 2> panel_hidden;
    std::array<std::optional<bool>, 2> panel_directories_first;
    std::array<std::optional<bool>, 2> panel_case_sensitive;
    std::array<std::optional<bool>, 2> panel_executable_first;
    bool reverse_files_only{true};
    bool layout_horizontal{false};
    bool equal_split{true};
    int panel_left_size{0};
    int panel_top_size{0};
    bool menubar_visible{true};
    bool command_prompt{true};
    bool keybar_visible{true};
    bool hintbar_visible{true};
    bool xterm_title{true};
    bool show_free_space{true};
    bool show_mini_status{true};
    bool use_si_units{false};
    bool show_backups{true};
    bool fast_reload{false};
    bool mark_moves_down{true};
    bool simple_swap{false};
    bool auto_save_panels_setup{false};
    bool navigate_with_arrows{false};
    bool scroll_pages{true};
    bool scroll_center{false};
    bool mouse_page_scrolling{true};
    bool highlight_file_types{true};
    bool highlight_permissions{false};
    std::size_t quick_search_mode{2};
    std::string saved_left_path;
    std::string saved_right_path;
    bool verbose_operation{true};
    bool compute_totals{true};
    bool classic_progressbar{true};
    bool mkdir_autoname{true};
    bool preallocate_space{false};
    bool single_escape{true};
    int escape_timeout{1000000};
    std::size_t pause_after_run{1};
    bool use_internal_edit{true};
    bool use_internal_view{true};
    bool ask_new_filename{false};
    bool auto_menus{false};
    bool drop_menus{false};
    bool shell_patterns{true};
    bool completion_show_all{false};
    bool rotating_dash{true};
    bool cd_follows_links{true};
    bool safe_delete{false};
    bool safe_overwrite{false};
    bool auto_save_setup{true};
    std::string theme{"grayscale"};
    bool shadows{true};
    bool editor_syntax{true};
    bool editor_line_numbers{false};
    std::size_t editor_tab_width{8};
    std::size_t editor_indent_width{4};
    std::size_t editor_wrap_length{72};
    bool editor_auto_indent{false};
    bool editor_show_whitespace{false};
    bool editor_cursor_after_inserted_block{false};
    bool editor_fake_half_tabs{true};
    bool editor_backspace_through_tabs{false};
    bool editor_fill_tabs_with_spaces{false};
    bool editor_confirm_save{true};
    bool editor_save_position{true};
    bool editor_visible_tabs{true};
    bool editor_visible_trailing_spaces{true};
    bool editor_persistent_selection{true};
    bool editor_cursor_beyond_eol{false};
    bool editor_group_undo{true};
    std::size_t editor_wrap_mode{0};
    std::size_t diff_quality{0};
    bool diff_ignore_case{false};
    bool diff_ignore_tab_expansion{false};
    bool diff_ignore_space_change{false};
    bool diff_ignore_all_space{false};
    bool diff_strip_trailing_cr{false};
    bool diff_show_numbers{false};
    bool diff_show_symbols{false};
    std::string editor_save_mode{"atomic"};
    std::string editor_backup_extension{"~"};
    bool editor_check_newline{false};
    std::string editor_syntax_language{"auto"};
    std::vector<std::string> bookmarks;
    std::vector<std::string> editor_history;
    std::vector<std::string> viewer_history;
    std::vector<std::string> command_history;
    std::vector<std::pair<std::string, std::string>> panelize_commands{
        {"Find rejects after patching", "find . -name '*.rej' -print"},
        {"Find SUID and SGID programs", "find . \\( \\( -perm -04000 -a -perm /011 \\) -o \\( -perm -02000 -a -perm /01 \\) \\) -print"},
        {"Find *.orig after patching", "find . -name '*.orig' -print"},
        {"Modified git files", "git ls-files --modified"}
    };
    std::map<std::string, SavedEditorPosition> editor_positions;
    std::map<std::string, std::string> learned_keys;
    std::map<std::string, std::string> editor_macros;

    static Config load(const std::filesystem::path& path);
    void save(const std::filesystem::path& path) const;
};

[[nodiscard]] std::filesystem::path default_config_path();

}  // namespace mcpp
