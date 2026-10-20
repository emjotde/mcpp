#pragma once

#include "mcpp/entry.hpp"

#include <cstddef>
#include <cstdint>
#include <chrono>
#include <array>
#include <filesystem>
#include <memory>
#include <optional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mcpp {

struct ConfigSource;

enum class PanelDisplay { listing, quick_view, information, tree };

struct SavedEditorPosition {
    std::size_t row{0};
    std::size_t column{0};
    std::vector<std::size_t> bookmarks;
};

struct ConfigColorTable {
    std::string value;
    std::string location;
};

struct Config {
    bool show_hidden{true};
    bool confirm_delete{true};
    bool confirm_overwrite{true};
    bool confirm_execute{false};
    bool confirm_exit{false};
    std::optional<bool> confirm_view_dir;
    bool confirm_hotlist_delete{false};
    bool confirm_history_cleanup{true};
    bool directories_first{true};
    SortField sort{SortField::name};
    bool sort_descending{false};
    std::array<PanelDisplay, 2> panel_display{PanelDisplay::listing, PanelDisplay::listing};
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
    std::optional<bool> layout_vertical_equal;
    std::optional<bool> layout_horizontal_equal;
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
    std::optional<unsigned> select_flags;
    std::string saved_left_path;
    std::string saved_right_path;
    std::optional<bool> current_is_left;
    std::optional<std::string> other_directory;
    bool verbose_operation{true};
    bool compute_totals{true};
    bool classic_progressbar{true};
    bool mkdir_autoname{true};
    bool preallocate_space{false};
    std::optional<bool> copymove_persistent_attr;
    bool single_escape{true};
    int escape_timeout{1000000};
    std::size_t pause_after_run{1};
    std::optional<std::size_t> double_click_speed;
    std::optional<std::size_t> num_history_items_recorded;
    bool use_subshell{true};
    std::optional<bool> subshell_override;
    std::string keymap_file;
    std::optional<std::string> keymap_override;
    bool keymap_disabled{false};
    std::size_t vfs_timeout{60};
    bool use_internal_edit{true};
    bool use_internal_view{true};
    bool use_file_to_guess_type{true};
    bool viewer_wrap{true};
    std::optional<bool> viewer_mouse_page_scrolling;
    bool viewer_remember_file_position{false};
    std::optional<std::string> source_codepage;
    std::optional<std::string> clipboard_store;
    std::optional<std::string> clipboard_paste;
    std::optional<std::string> timeformat_recent;
    std::optional<std::string> timeformat_old;
    std::optional<std::size_t> max_saved_positions;
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
    std::map<std::string, ConfigColorTable> color_tables;
    std::optional<std::string> color_terminals;
    // Launch-only; the saved theme is unchanged.
    std::optional<std::string> theme_override;
    bool shadows{true};
    bool editor_syntax{true};
    bool editor_line_numbers{false};
    std::optional<bool> editor_simple_statusbar;
    std::optional<bool> editor_show_right_margin;
    std::size_t editor_tab_width{8};
    std::size_t editor_indent_width{4};
    std::size_t editor_wrap_length{72};
    std::optional<std::uint64_t> editor_filesize_threshold;
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
    bool editor_drop_selection_on_copy{false};
    bool editor_state_full_filename{true};
    bool editor_cursor_beyond_eol{false};
    bool editor_group_undo{true};
    std::size_t editor_wrap_mode{0};
    std::size_t diff_quality{0};
    std::size_t diff_tab_size{8};
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
    std::string editor_spelling_language;
    std::optional<std::string> editor_stop_format_chars;
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
    std::map<std::string, std::uint64_t> viewer_positions;
    std::map<std::string, std::string> learned_keys;
    std::map<std::string, std::string> editor_macros;
    std::vector<std::string> diagnostics;
    std::shared_ptr<const ConfigSource> source;

    static Config load(const std::filesystem::path& path);
    void save(const std::filesystem::path& path) const;
    void save() const;
    [[nodiscard]] bool split_is_equal() const;
    void set_split_equal(bool value);
    [[nodiscard]] std::chrono::milliseconds double_click_timeout() const;
    [[nodiscard]] std::size_t history_limit(std::size_t fallback) const noexcept;
    void limit_histories();
    [[nodiscard]] bool color_terminal_listed(std::string_view terminal) const;
    [[nodiscard]] bool selection_matches(const Entry& entry, std::string_view pattern) const;
    [[nodiscard]] std::string format_panel_time(
        std::chrono::system_clock::time_point point,
        std::chrono::system_clock::time_point reference = std::chrono::system_clock::now()) const;
};

[[nodiscard]] std::filesystem::path default_config_path();

}  // namespace mcpp
