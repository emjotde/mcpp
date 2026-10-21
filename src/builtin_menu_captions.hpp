#pragma once

#include "mcpp/translation.hpp"

#include <array>
#include <cstddef>
#include <span>
#include <string_view>

namespace mcpp::detail {

struct BuiltinCaptionMessage {
    UiCaption id;
    std::string_view key;

    [[nodiscard]] constexpr std::string_view english() const noexcept {
        const auto separator = key.find('\004');
        return separator == std::string_view::npos ? std::string_view{} : key.substr(separator + 1U);
    }
};

inline constexpr auto builtin_caption_messages = std::to_array<BuiltinCaptionMessage>({
    {UiCaption::delete_confirmation, "delete-dialog\004Delete"},
    {UiCaption::manager_panel_file_listing, "manager-panel-menu\004File listing"},
    {UiCaption::manager_panel_quick_view, "manager-panel-menu\004Quick view"},
    {UiCaption::manager_panel_info, "manager-panel-menu\004Info"},
    {UiCaption::manager_panel_tree, "manager-panel-menu\004Tree"},
    {UiCaption::manager_panel_listing_format, "manager-panel-menu\004Listing format..."},
    {UiCaption::manager_panel_sort_order, "manager-panel-menu\004Sort order..."},
    {UiCaption::manager_panel_filter, "manager-panel-menu\004Filter..."},
    {UiCaption::manager_panel_encoding, "manager-panel-menu\004Encoding..."},
    {UiCaption::manager_panel_ftp_link, "manager-panel-menu\004FTP link..."},
    {UiCaption::manager_panel_shell_link, "manager-panel-menu\004Shell link..."},
    {UiCaption::manager_panel_sftp_link, "manager-panel-menu\004SFTP link..."},
    {UiCaption::manager_panel_panelize, "manager-panel-menu\004Panelize"},
    {UiCaption::manager_panel_rescan, "manager-panel-menu\004Rescan"},
    {UiCaption::manager_file_view, "manager-file-menu\004View"},
    {UiCaption::manager_file_view_file, "manager-file-menu\004View file..."},
    {UiCaption::manager_file_filtered_view, "manager-file-menu\004Filtered view"},
    {UiCaption::manager_file_edit, "manager-file-menu\004Edit"},
    {UiCaption::manager_file_copy, "manager-file-menu\004Copy"},
    {UiCaption::manager_file_chmod, "manager-file-menu\004Chmod"},
    {UiCaption::manager_file_link, "manager-file-menu\004Link"},
    {UiCaption::manager_file_symlink, "manager-file-menu\004Symlink"},
    {UiCaption::manager_file_relative_symlink, "manager-file-menu\004Relative symlink"},
    {UiCaption::manager_file_edit_symlink, "manager-file-menu\004Edit symlink"},
    {UiCaption::manager_file_chown, "manager-file-menu\004Chown"},
    {UiCaption::manager_file_advanced_chown, "manager-file-menu\004Advanced chown"},
    {UiCaption::manager_file_chattr, "manager-file-menu\004Chattr"},
    {UiCaption::manager_file_rename_move, "manager-file-menu\004Rename/Move"},
    {UiCaption::manager_file_mkdir, "manager-file-menu\004Mkdir"},
    {UiCaption::manager_file_delete, "manager-file-menu\004Delete"},
    {UiCaption::manager_file_quick_cd, "manager-file-menu\004Quick cd"},
    {UiCaption::manager_file_select_group, "manager-file-menu\004Select group"},
    {UiCaption::manager_file_unselect_group, "manager-file-menu\004Unselect group"},
    {UiCaption::manager_file_invert_selection, "manager-file-menu\004Invert selection"},
    {UiCaption::manager_file_exit, "manager-file-menu\004Exit"},
    {UiCaption::manager_command_user_menu, "manager-command-menu\004User menu"},
    {UiCaption::manager_command_directory_tree, "manager-command-menu\004Directory tree"},
    {UiCaption::manager_command_find_file, "manager-command-menu\004Find file"},
    {UiCaption::manager_command_swap_panels, "manager-command-menu\004Swap panels"},
    {UiCaption::manager_command_switch_panels, "manager-command-menu\004Switch panels on/off"},
    {UiCaption::manager_command_compare_directories, "manager-command-menu\004Compare directories"},
    {UiCaption::manager_command_compare_files, "manager-command-menu\004Compare files"},
    {UiCaption::manager_command_external_panelize, "manager-command-menu\004External panelize"},
    {UiCaption::manager_command_show_directory_sizes, "manager-command-menu\004Show directory sizes"},
    {UiCaption::manager_command_history, "manager-command-menu\004Command history"},
    {UiCaption::manager_command_file_history, "manager-command-menu\004Viewed/edited files history"},
    {UiCaption::manager_command_directory_hotlist, "manager-command-menu\004Directory hotlist"},
    {UiCaption::manager_command_vfs_list, "manager-command-menu\004Active VFS list"},
    {UiCaption::manager_command_background_jobs, "manager-command-menu\004Background jobs"},
    {UiCaption::manager_command_screen_list, "manager-command-menu\004Screen list"},
    {UiCaption::manager_command_undelete_files, "manager-command-menu\004Undelete files (ext2fs only)"},
    {UiCaption::manager_command_edit_extension_file, "manager-command-menu\004Edit extension file"},
    {UiCaption::manager_command_edit_menu_file, "manager-command-menu\004Edit menu file"},
    {UiCaption::manager_command_edit_highlighting_file, "manager-command-menu\004Edit highlighting group file"},
    {UiCaption::manager_options_configuration, "manager-options-menu\004Configuration..."},
    {UiCaption::manager_options_layout, "manager-options-menu\004Layout..."},
    {UiCaption::manager_options_panel_options, "manager-options-menu\004Panel options..."},
    {UiCaption::manager_options_confirmation, "manager-options-menu\004Confirmation..."},
    {UiCaption::manager_options_appearance, "manager-options-menu\004Appearance..."},
    {UiCaption::manager_options_display_bits, "manager-options-menu\004Display bits..."},
    {UiCaption::manager_options_learn_keys, "manager-options-menu\004Learn keys..."},
    {UiCaption::manager_options_virtual_fs, "manager-options-menu\004Virtual FS..."},
    {UiCaption::manager_options_save_setup, "manager-options-menu\004Save setup"},
    {UiCaption::editor_menu_file, "editor-menubar\004File"},
    {UiCaption::editor_menu_edit, "editor-menubar\004Edit"},
    {UiCaption::editor_menu_search, "editor-menubar\004Search"},
    {UiCaption::editor_menu_command, "editor-menubar\004Command"},
    {UiCaption::editor_menu_format, "editor-menubar\004Format"},
    {UiCaption::editor_menu_window, "editor-menubar\004Window"},
    {UiCaption::editor_menu_options, "editor-menubar\004Options"},
    {UiCaption::editor_file_open_file, "editor-file-menu\004Open file..."},
    {UiCaption::editor_file_new, "editor-file-menu\004New"},
    {UiCaption::editor_file_close, "editor-file-menu\004Close"},
    {UiCaption::editor_file_history, "editor-file-menu\004History..."},
    {UiCaption::editor_file_save, "editor-file-menu\004Save"},
    {UiCaption::editor_file_save_as, "editor-file-menu\004Save as..."},
    {UiCaption::editor_file_insert_file, "editor-file-menu\004Insert file..."},
    {UiCaption::editor_file_copy_to_file, "editor-file-menu\004Copy to file..."},
    {UiCaption::editor_file_user_menu, "editor-file-menu\004User menu..."},
    {UiCaption::editor_file_about, "editor-file-menu\004About..."},
    {UiCaption::editor_file_quit, "editor-file-menu\004Quit"},
    {UiCaption::editor_edit_undo, "editor-edit-menu\004Undo"},
    {UiCaption::editor_edit_redo, "editor-edit-menu\004Redo"},
    {UiCaption::editor_edit_toggle_ins_overw, "editor-edit-menu\004Toggle ins/overw"},
    {UiCaption::editor_edit_toggle_mark, "editor-edit-menu\004Toggle mark"},
    {UiCaption::editor_edit_mark_columns, "editor-edit-menu\004Mark columns"},
    {UiCaption::editor_edit_mark_all, "editor-edit-menu\004Mark all"},
    {UiCaption::editor_edit_unmark, "editor-edit-menu\004Unmark"},
    {UiCaption::editor_edit_copy, "editor-edit-menu\004Copy"},
    {UiCaption::editor_edit_move, "editor-edit-menu\004Move"},
    {UiCaption::editor_edit_delete, "editor-edit-menu\004Delete"},
    {UiCaption::editor_edit_copy_to_clipfile, "editor-edit-menu\004Copy to clipfile"},
    {UiCaption::editor_edit_cut_to_clipfile, "editor-edit-menu\004Cut to clipfile"},
    {UiCaption::editor_edit_paste_from_clipfile, "editor-edit-menu\004Paste from clipfile"},
    {UiCaption::editor_edit_beginning, "editor-edit-menu\004Beginning"},
    {UiCaption::editor_edit_end, "editor-edit-menu\004End"},
    {UiCaption::editor_search_search, "editor-search-menu\004Search..."},
    {UiCaption::editor_search_search_again, "editor-search-menu\004Search again"},
    {UiCaption::editor_search_replace, "editor-search-menu\004Replace..."},
    {UiCaption::editor_search_toggle_bookmark, "editor-search-menu\004Toggle bookmark"},
    {UiCaption::editor_search_next_bookmark, "editor-search-menu\004Next bookmark"},
    {UiCaption::editor_search_prev_bookmark, "editor-search-menu\004Prev bookmark"},
    {UiCaption::editor_search_flush_bookmarks, "editor-search-menu\004Flush bookmarks"},
    {UiCaption::editor_command_go_to_line, "editor-command-menu\004Go to line..."},
    {UiCaption::editor_command_toggle_line_state, "editor-command-menu\004Toggle line state"},
    {UiCaption::editor_command_go_to_matching_bracket, "editor-command-menu\004Go to matching bracket"},
    {UiCaption::editor_command_toggle_syntax_highlighting, "editor-command-menu\004Toggle syntax highlighting"},
    {UiCaption::editor_command_find_declaration, "editor-command-menu\004Find declaration"},
    {UiCaption::editor_command_back_from_declaration, "editor-command-menu\004Back from declaration"},
    {UiCaption::editor_command_forward_to_declaration, "editor-command-menu\004Forward to declaration"},
    {UiCaption::editor_command_encoding, "editor-command-menu\004Encoding..."},
    {UiCaption::editor_command_refresh_screen, "editor-command-menu\004Refresh screen"},
    {UiCaption::editor_command_start_stop_record_macro, "editor-command-menu\004Start/Stop record macro"},
    {UiCaption::editor_command_delete_macro, "editor-command-menu\004Delete macro..."},
    {UiCaption::editor_command_record_repeat_actions, "editor-command-menu\004Record/Repeat actions"},
    {UiCaption::editor_command_spell_check, "editor-command-menu\004Spell check"},
    {UiCaption::editor_command_check_word, "editor-command-menu\004Check word"},
    {UiCaption::editor_command_change_spelling_language, "editor-command-menu\004Change spelling language..."},
    {UiCaption::editor_command_mail, "editor-command-menu\004Mail..."},
    {UiCaption::editor_format_insert_literal, "editor-format-menu\004Insert literal..."},
    {UiCaption::editor_format_insert_date_time, "editor-format-menu\004Insert date/time"},
    {UiCaption::editor_format_format_paragraph, "editor-format-menu\004Format paragraph"},
    {UiCaption::editor_format_sort, "editor-format-menu\004Sort..."},
    {UiCaption::editor_format_paste_output_of, "editor-format-menu\004Paste output of..."},
    {UiCaption::editor_format_external_formatter, "editor-format-menu\004External formatter"},
    {UiCaption::editor_window_move, "editor-window-menu\004Move"},
    {UiCaption::editor_window_resize, "editor-window-menu\004Resize"},
    {UiCaption::editor_window_toggle_fullscreen, "editor-window-menu\004Toggle fullscreen"},
    {UiCaption::editor_window_next, "editor-window-menu\004Next"},
    {UiCaption::editor_window_previous, "editor-window-menu\004Previous"},
    {UiCaption::editor_window_list, "editor-window-menu\004List..."},
    {UiCaption::editor_options_general, "editor-options-menu\004General..."},
    {UiCaption::editor_options_save_mode, "editor-options-menu\004Save mode..."},
    {UiCaption::editor_options_learn_keys, "editor-options-menu\004Learn keys..."},
    {UiCaption::editor_options_syntax_highlighting, "editor-options-menu\004Syntax highlighting..."},
    {UiCaption::editor_options_syntax_file, "editor-options-menu\004Syntax file"},
    {UiCaption::editor_options_menu_file, "editor-options-menu\004Menu file"},
    {UiCaption::editor_options_save_setup, "editor-options-menu\004Save setup"},
});

consteval bool valid_builtin_caption_messages() {
    if (builtin_caption_messages.size() != ui_caption_count) return false;
    for (std::size_t index = 0U; index < builtin_caption_messages.size(); ++index) {
        const auto& message = builtin_caption_messages[index];
        const auto separator = message.key.find('\004');
        if (static_cast<std::size_t>(message.id) != index || separator == 0U ||
            separator == std::string_view::npos || separator != message.key.rfind('\004') ||
            message.english().empty() || message.key.find('\0') != std::string_view::npos) return false;
        for (std::size_t earlier = 0U; earlier < index; ++earlier) {
            if (message.key == builtin_caption_messages[earlier].key) return false;
        }
    }
    return true;
}

static_assert(valid_builtin_caption_messages());

[[nodiscard]] constexpr std::string_view builtin_english_caption(const UiCaption caption) noexcept {
    const auto index = static_cast<std::size_t>(caption);
    return index < builtin_caption_messages.size()
        ? builtin_caption_messages[index].english() : std::string_view{};
}

struct BuiltinMenuCaption {
    UiCaption id{UiCaption::count};
    // The original English spacing and shortcut are display-only, never catalog inputs.
    std::string_view shortcut;
};

inline constexpr auto builtin_panel_menu_captions = std::to_array<BuiltinMenuCaption>({
    {UiCaption::manager_panel_file_listing, ""},
    {UiCaption::manager_panel_quick_view, "         C-x q"},
    {UiCaption::manager_panel_info, "               C-x i"},
    {UiCaption::manager_panel_tree, ""},
    {},
    {UiCaption::manager_panel_listing_format, ""},
    {UiCaption::manager_panel_sort_order, ""},
    {UiCaption::manager_panel_filter, ""},
    {UiCaption::manager_panel_encoding, "        M-e"},
    {},
    {UiCaption::manager_panel_ftp_link, ""},
    {UiCaption::manager_panel_shell_link, ""},
    {UiCaption::manager_panel_sftp_link, ""},
    {UiCaption::manager_panel_panelize, ""},
    {},
    {UiCaption::manager_panel_rescan, "             C-r"},
});

inline constexpr auto builtin_file_menu_captions = std::to_array<BuiltinMenuCaption>({
    {UiCaption::manager_file_view, "              F3"},
    {UiCaption::manager_file_view_file, ""},
    {UiCaption::manager_file_filtered_view, "     M-!"},
    {UiCaption::manager_file_edit, "              F4"},
    {UiCaption::manager_file_copy, "              F5"},
    {UiCaption::manager_file_chmod, "             C-x c"},
    {UiCaption::manager_file_link, "              C-x l"},
    {UiCaption::manager_file_symlink, "           C-x s"},
    {UiCaption::manager_file_relative_symlink, "  C-x v"},
    {UiCaption::manager_file_edit_symlink, "      C-x C-s"},
    {UiCaption::manager_file_chown, "             C-x o"},
    {UiCaption::manager_file_advanced_chown, ""},
    {UiCaption::manager_file_chattr, "            C-x e"},
    {UiCaption::manager_file_rename_move, "       F6"},
    {UiCaption::manager_file_mkdir, "             F7"},
    {UiCaption::manager_file_delete, "            F8"},
    {UiCaption::manager_file_quick_cd, "          M-c"},
    {},
    {UiCaption::manager_file_select_group, "      +"},
    {UiCaption::manager_file_unselect_group, "    -"},
    {UiCaption::manager_file_invert_selection, "  *"},
    {},
    {UiCaption::manager_file_exit, "              F10"},
});

inline constexpr auto builtin_command_menu_captions = std::to_array<BuiltinMenuCaption>({
    {UiCaption::manager_command_user_menu, "                     F2"},
    {UiCaption::manager_command_directory_tree, ""},
    {UiCaption::manager_command_find_file, "                     M-?"},
    {UiCaption::manager_command_swap_panels, "                   C-u"},
    {UiCaption::manager_command_switch_panels, "          C-o"},
    {UiCaption::manager_command_compare_directories, "           C-x d"},
    {UiCaption::manager_command_compare_files, "                 C-x C-d"},
    {UiCaption::manager_command_external_panelize, "             C-x !"},
    {UiCaption::manager_command_show_directory_sizes, "          C-Space"},
    {},
    {UiCaption::manager_command_history, "               M-h"},
    {UiCaption::manager_command_file_history, "   M-E"},
    {UiCaption::manager_command_directory_hotlist, "             C-\\"},
    {UiCaption::manager_command_vfs_list, "               C-x a"},
    {UiCaption::manager_command_background_jobs, "               C-x j"},
    {UiCaption::manager_command_screen_list, "                   M-`"},
    {},
    {UiCaption::manager_command_undelete_files, ""},
    {},
    {UiCaption::manager_command_edit_extension_file, ""},
    {UiCaption::manager_command_edit_menu_file, ""},
    {UiCaption::manager_command_edit_highlighting_file, ""},
});

inline constexpr auto builtin_options_menu_captions = std::to_array<BuiltinMenuCaption>({
    {UiCaption::manager_options_configuration, ""},
    {UiCaption::manager_options_layout, ""},
    {UiCaption::manager_options_panel_options, ""},
    {UiCaption::manager_options_confirmation, ""},
    {UiCaption::manager_options_appearance, ""},
    {UiCaption::manager_options_display_bits, ""},
    {UiCaption::manager_options_learn_keys, ""},
    {UiCaption::manager_options_virtual_fs, ""},
    {},
    {UiCaption::manager_options_save_setup, ""},
});

inline constexpr std::array<std::span<const BuiltinMenuCaption>, 5> builtin_manager_menu_captions{
    builtin_panel_menu_captions, builtin_file_menu_captions, builtin_command_menu_captions,
    builtin_options_menu_captions, builtin_panel_menu_captions,
};

inline constexpr std::array builtin_editor_menu_caption_ids{
    UiCaption::editor_menu_file, UiCaption::editor_menu_edit, UiCaption::editor_menu_search,
    UiCaption::editor_menu_command, UiCaption::editor_menu_format, UiCaption::editor_menu_window,
    UiCaption::editor_menu_options,
};

inline constexpr auto builtin_editor_file_menu_captions = std::to_array<BuiltinMenuCaption>({
    {UiCaption::editor_file_open_file, ""},
    {UiCaption::editor_file_new, "              C-n"},
    {UiCaption::editor_file_close, ""},
    {UiCaption::editor_file_history, "       M-E"},
    {},
    {UiCaption::editor_file_save, "             F2"},
    {UiCaption::editor_file_save_as, "       F12"},
    {},
    {UiCaption::editor_file_insert_file, "   F15"},
    {UiCaption::editor_file_copy_to_file, "  C-f"},
    {},
    {UiCaption::editor_file_user_menu, "     F11"},
    {},
    {UiCaption::editor_file_about, ""},
    {},
    {UiCaption::editor_file_quit, "             F10"},
});

inline constexpr auto builtin_editor_edit_menu_captions = std::to_array<BuiltinMenuCaption>({
    {UiCaption::editor_edit_undo, "                 C-u"},
    {UiCaption::editor_edit_redo, "                 M-r"},
    {},
    {UiCaption::editor_edit_toggle_ins_overw, "     Ins"},
    {},
    {UiCaption::editor_edit_toggle_mark, "          F3"},
    {UiCaption::editor_edit_mark_columns, "         F13"},
    {UiCaption::editor_edit_mark_all, ""},
    {UiCaption::editor_edit_unmark, ""},
    {},
    {UiCaption::editor_edit_copy, "                 F5"},
    {UiCaption::editor_edit_move, "                 F6"},
    {UiCaption::editor_edit_delete, "               F8"},
    {},
    {UiCaption::editor_edit_copy_to_clipfile, "     C-Ins"},
    {UiCaption::editor_edit_cut_to_clipfile, "      S-Del"},
    {UiCaption::editor_edit_paste_from_clipfile, "  S-Ins"},
    {},
    {UiCaption::editor_edit_beginning, "            C-Home"},
    {UiCaption::editor_edit_end, "                  C-End"},
});

inline constexpr auto builtin_editor_search_menu_captions = std::to_array<BuiltinMenuCaption>({
    {UiCaption::editor_search_search, "        F7"},
    {UiCaption::editor_search_search_again, "     F17"},
    {UiCaption::editor_search_replace, "       F4"},
    {},
    {UiCaption::editor_search_toggle_bookmark, "  M-k"},
    {UiCaption::editor_search_next_bookmark, "    M-j"},
    {UiCaption::editor_search_prev_bookmark, "    M-i"},
    {UiCaption::editor_search_flush_bookmarks, "  M-o"},
});

inline constexpr auto builtin_editor_command_menu_captions = std::to_array<BuiltinMenuCaption>({
    {UiCaption::editor_command_go_to_line, "                M-l"},
    {UiCaption::editor_command_toggle_line_state, "            M-n"},
    {UiCaption::editor_command_go_to_matching_bracket, "       M-b"},
    {UiCaption::editor_command_toggle_syntax_highlighting, "   C-s"},
    {},
    {UiCaption::editor_command_find_declaration, "             M-Enter"},
    {UiCaption::editor_command_back_from_declaration, "        M--"},
    {UiCaption::editor_command_forward_to_declaration, "       M-+"},
    {},
    {UiCaption::editor_command_encoding, "                  M-e"},
    {},
    {UiCaption::editor_command_refresh_screen, "               C-l"},
    {},
    {UiCaption::editor_command_start_stop_record_macro, "      C-r"},
    {UiCaption::editor_command_delete_macro, ""},
    {UiCaption::editor_command_record_repeat_actions, ""},
    {},
    {UiCaption::editor_command_spell_check, ""},
    {UiCaption::editor_command_check_word, "                   C-p"},
    {UiCaption::editor_command_change_spelling_language, ""},
    {},
    {UiCaption::editor_command_mail, ""},
});

inline constexpr auto builtin_editor_format_menu_captions = std::to_array<BuiltinMenuCaption>({
    {UiCaption::editor_format_insert_literal, "   C-q"},
    {UiCaption::editor_format_insert_date_time, ""},
    {},
    {UiCaption::editor_format_format_paragraph, "    M-p"},
    {UiCaption::editor_format_sort, "             M-t"},
    {UiCaption::editor_format_paste_output_of, "  M-u"},
    {UiCaption::editor_format_external_formatter, ""},
});

inline constexpr auto builtin_editor_window_menu_captions = std::to_array<BuiltinMenuCaption>({
    {UiCaption::editor_window_move, ""},
    {UiCaption::editor_window_resize, ""},
    {UiCaption::editor_window_toggle_fullscreen, ""},
    {},
    {UiCaption::editor_window_next, ""},
    {UiCaption::editor_window_previous, ""},
    {UiCaption::editor_window_list, ""},
});

inline constexpr auto builtin_editor_options_menu_captions = std::to_array<BuiltinMenuCaption>({
    {UiCaption::editor_options_general, ""},
    {UiCaption::editor_options_save_mode, ""},
    {UiCaption::editor_options_learn_keys, ""},
    {UiCaption::editor_options_syntax_highlighting, ""},
    {},
    {UiCaption::editor_options_syntax_file, ""},
    {UiCaption::editor_options_menu_file, ""},
    {},
    {UiCaption::editor_options_save_setup, ""},
});

inline constexpr std::array<std::span<const BuiltinMenuCaption>, 7> builtin_editor_dropdown_captions{
    builtin_editor_file_menu_captions, builtin_editor_edit_menu_captions,
    builtin_editor_search_menu_captions, builtin_editor_command_menu_captions,
    builtin_editor_format_menu_captions, builtin_editor_window_menu_captions,
    builtin_editor_options_menu_captions,
};

consteval bool valid_builtin_menu_caption_map() {
    std::array<bool, ui_caption_count> seen{};
    seen[static_cast<std::size_t>(UiCaption::delete_confirmation)] = true;
    const std::array<std::span<const BuiltinMenuCaption>, 4> distinct_menus{
        builtin_panel_menu_captions, builtin_file_menu_captions,
        builtin_command_menu_captions, builtin_options_menu_captions,
    };
    for (const auto menu : distinct_menus) {
        for (const auto& caption : menu) {
            if (caption.id == UiCaption::count) {
                if (!caption.shortcut.empty()) return false;
                continue;
            }
            const auto index = static_cast<std::size_t>(caption.id);
            if (index == 0U || index >= static_cast<std::size_t>(UiCaption::editor_menu_file) ||
                seen[index]) return false;
            seen[index] = true;
            for (const auto value : caption.shortcut) {
                if (value < ' ' || value > '~') return false;
            }
        }
    }
    for (const auto id : builtin_editor_menu_caption_ids) {
        const auto index = static_cast<std::size_t>(id);
        if (index < static_cast<std::size_t>(UiCaption::editor_menu_file) ||
            index >= static_cast<std::size_t>(UiCaption::editor_file_open_file) ||
            seen[index]) return false;
        seen[index] = true;
    }
    for (const auto menu : builtin_editor_dropdown_captions) {
        for (const auto& caption : menu) {
            if (caption.id == UiCaption::count) {
                if (!caption.shortcut.empty()) return false;
                continue;
            }
            const auto index = static_cast<std::size_t>(caption.id);
            if (index < static_cast<std::size_t>(UiCaption::editor_file_open_file) ||
                index >= seen.size() || seen[index]) return false;
            seen[index] = true;
            for (const auto value : caption.shortcut) {
                if (value < ' ' || value > '~') return false;
            }
        }
    }
    for (const auto value : seen) {
        if (!value) return false;
    }
    return true;
}

static_assert(builtin_panel_menu_captions.size() == 16U);
static_assert(builtin_file_menu_captions.size() == 23U);
static_assert(builtin_command_menu_captions.size() == 22U);
static_assert(builtin_options_menu_captions.size() == 10U);
static_assert(static_cast<std::size_t>(UiCaption::editor_file_open_file) == 70U);
static_assert(builtin_editor_file_menu_captions.size() == 16U);
static_assert(builtin_editor_edit_menu_captions.size() == 20U);
static_assert(builtin_editor_search_menu_captions.size() == 8U);
static_assert(builtin_editor_command_menu_captions.size() == 22U);
static_assert(builtin_editor_format_menu_captions.size() == 7U);
static_assert(builtin_editor_window_menu_captions.size() == 7U);
static_assert(builtin_editor_options_menu_captions.size() == 9U);
static_assert(valid_builtin_menu_caption_map());

}  // namespace mcpp::detail
