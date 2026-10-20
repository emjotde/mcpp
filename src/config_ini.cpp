#include "mcpp/config_ini.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace mcpp {
namespace {

enum class Conversion { plain, string, inverted, save_mode, listing, sort, typewriter, paragraph, codepage };

struct NativeOption {
    std::string_view section;
    std::string_view key;
    std::string_view flat;
    Conversion conversion{Conversion::plain};
};

constexpr NativeOption native_options[]{
    {"Midnight-Commander", "verbose", "verbose_operation"},
    {"Midnight-Commander", "shell_patterns", "shell_patterns"},
    {"Midnight-Commander", "auto_save_setup", "auto_save_setup"},
    {"Midnight-Commander", "preallocate_space", "preallocate_space"},
    {"Midnight-Commander", "copymove_persistent_attr", "copymove_persistent_attr"},
    {"Midnight-Commander", "auto_menu", "auto_menus"},
    {"Midnight-Commander", "use_internal_view", "use_internal_view"},
    {"Midnight-Commander", "use_file_to_guess_type", "use_file_to_guess_type"},
    {"Midnight-Commander", "use_internal_edit", "use_internal_edit"},
    {"Midnight-Commander", "wrap_mode", "viewer_wrap"},
    {"Midnight-Commander", "mouse_move_pages_viewer", "viewer_mouse_page_scrolling"},
    {"Midnight-Commander", "mcview_remember_file_position", "viewer_remember_file_position"},
    {"Midnight-Commander", "filepos_max_saved_entries", "max_saved_positions"},
    {"Midnight-Commander", "confirm_delete", "confirm_delete"},
    {"Midnight-Commander", "confirm_overwrite", "confirm_overwrite"},
    {"Midnight-Commander", "confirm_execute", "confirm_execute"},
    {"Midnight-Commander", "confirm_history_cleanup", "confirm_history_cleanup"},
    {"Midnight-Commander", "confirm_exit", "confirm_exit"},
    {"Midnight-Commander", "confirm_view_dir", "confirm_view_dir"},
    {"Midnight-Commander", "double_click_speed", "double_click_speed"},
    {"Midnight-Commander", "confirm_directory_hotlist_delete", "confirm_hotlist_delete"},
    {"Midnight-Commander", "safe_delete", "safe_delete"},
    {"Midnight-Commander", "safe_overwrite", "safe_overwrite"},
    {"Midnight-Commander", "drop_menus", "drop_menus"},
    {"Midnight-Commander", "old_esc_mode", "single_escape"},
    {"Midnight-Commander", "old_esc_mode_timeout", "escape_timeout"},
    {"Midnight-Commander", "cd_symlinks", "cd_follows_links"},
    {"Midnight-Commander", "show_all_if_ambiguous", "completion_show_all"},
    {"Midnight-Commander", "file_op_compute_totals", "compute_totals"},
    {"Midnight-Commander", "classic_progressbar", "classic_progressbar"},
    {"Midnight-Commander", "editor_fill_tabs_with_spaces", "editor_fill_tabs_with_spaces"},
    {"Midnight-Commander", "editor_return_does_auto_indent", "editor_auto_indent"},
    {"Midnight-Commander", "editor_backspace_through_tabs", "editor_backspace_through_tabs"},
    {"Midnight-Commander", "editor_fake_half_tabs", "editor_fake_half_tabs"},
    {"Midnight-Commander", "editor_option_save_position", "editor_save_position"},
    {"Midnight-Commander", "editor_option_auto_para_formatting", "editor_wrap_mode", Conversion::paragraph},
    {"Midnight-Commander", "editor_option_typewriter_wrap", "editor_wrap_mode", Conversion::typewriter},
    {"Midnight-Commander", "editor_edit_confirm_save", "editor_confirm_save"},
    {"Midnight-Commander", "editor_syntax_highlighting", "editor_syntax"},
    {"Midnight-Commander", "editor_persistent_selections", "editor_persistent_selection"},
    {"Midnight-Commander", "editor_drop_selection_on_copy", "editor_drop_selection_on_copy"},
    {"Midnight-Commander", "editor_state_full_filename", "editor_state_full_filename"},
    {"Midnight-Commander", "editor_stop_format_chars", "editor_stop_format_chars", Conversion::string},
    {"Midnight-Commander", "editor_cursor_beyond_eol", "editor_cursor_beyond_eol"},
    {"Midnight-Commander", "editor_cursor_after_inserted_block", "editor_cursor_after_inserted_block"},
    {"Midnight-Commander", "editor_visible_tabs", "editor_visible_tabs"},
    {"Midnight-Commander", "editor_visible_spaces", "editor_visible_trailing_spaces"},
    {"Midnight-Commander", "editor_line_state", "editor_line_numbers"},
    {"Midnight-Commander", "editor_check_new_line", "editor_check_newline"},
    {"Midnight-Commander", "editor_group_undo", "editor_group_undo"},
    {"Midnight-Commander", "editor_ask_filename_before_edit", "ask_new_filename"},
    {"Midnight-Commander", "nice_rotating_dash", "rotating_dash"},
    {"Midnight-Commander", "shadows", "shadows"},
    {"Midnight-Commander", "auto_fill_mkdir_name", "mkdir_autoname"},
    {"Midnight-Commander", "pause_after_run", "pause_after_run"},
    {"Midnight-Commander", "vfs_timeout", "vfs_timeout"},
    {"Midnight-Commander", "editor_tab_spacing", "editor_tab_width"},
    {"Midnight-Commander", "editor_word_wrap_line_length", "editor_wrap_length"},
    {"Midnight-Commander", "editor_option_save_mode", "editor_save_mode", Conversion::save_mode},
    {"Midnight-Commander", "editor_backup_extension", "editor_backup_extension", Conversion::string},
    {"Midnight-Commander", "skin", "theme", Conversion::string},
    {"Midnight-Commander", "keymap", "keymap", Conversion::string},
    {"Layout", "left_panel_size", "panel_left_size"},
    {"Layout", "top_panel_size", "panel_top_size"},
    {"Layout", "message_visible", "hintbar_visible"},
    {"Layout", "keybar_visible", "keybar_visible"},
    {"Layout", "xterm_title", "xterm_title"},
    {"Layout", "command_prompt", "command_prompt"},
    {"Layout", "menubar_visible", "menubar_visible"},
    {"Layout", "free_space", "show_free_space"},
    {"Layout", "horizontal_split", "layout_horizontal"},
    {"Layout", "vertical_equal", "layout_vertical_equal"},
    {"Layout", "horizontal_equal", "layout_horizontal_equal"},
    {"Misc", "spell_language", "editor_spelling_language", Conversion::string},
    {"Misc", "source_codepage", "source_codepage", Conversion::codepage},
    {"Misc", "timeformat_recent", "timeformat_recent", Conversion::string},
    {"Misc", "timeformat_old", "timeformat_old", Conversion::string},
    {"Panels", "show_mini_info", "show_mini_status"},
    {"Panels", "kilobyte_si", "use_si_units"},
    {"Panels", "mix_all_files", "directories_first", Conversion::inverted},
    {"Panels", "show_backups", "show_backups"},
    {"Panels", "show_dot_files", "show_hidden"},
    {"Panels", "fast_reload", "fast_reload"},
    {"Panels", "mark_moves_down", "mark_moves_down"},
    {"Panels", "reverse_files_only", "reverse_files_only"},
    {"Panels", "simple_swap", "simple_swap"},
    {"Panels", "auto_save_setup_panels", "auto_save_panels_setup"},
    {"Panels", "navigate_with_arrows", "navigate_with_arrows"},
    {"Panels", "panel_scroll_pages", "scroll_pages"},
    {"Panels", "panel_scroll_center", "scroll_center"},
    {"Panels", "mouse_move_pages", "mouse_page_scrolling"},
    {"Panels", "filetype_mode", "highlight_file_types"},
    {"Panels", "permission_mode", "highlight_permissions"},
    {"Panels", "quick_search_mode", "quick_search_mode"},
    {"Dirs", "current_is_left", "current_is_left"},
    {"Dirs", "other_dir", "other_directory", Conversion::string},
    {"DiffView", "diff_quality", "diff_quality"},
    {"DiffView", "tab_size", "diff_tab_size"},
    {"DiffView", "diff_ignore_case", "diff_ignore_case"},
    {"DiffView", "diff_tab_expansion", "diff_ignore_tab_expansion"},
    {"DiffView", "diff_ignore_space_change", "diff_ignore_space_change"},
    {"DiffView", "diff_ignore_all_space", "diff_ignore_all_space"},
    {"DiffView", "diff_ignore_tws", "diff_strip_trailing_cr"},
    {"DiffView", "show_numbers", "diff_show_numbers"},
    {"DiffView", "show_symbols", "diff_show_symbols"},
};

constexpr NativeOption panel_options[]{
    {"", "display", "display"},
    {"", "reverse", "descending"},
    {"", "case_sensitive", "case_sensitive"},
    {"", "exec_first", "executable_first"},
    {"", "sort_order", "sort", Conversion::sort},
    {"", "list_mode", "listing", Conversion::listing},
    {"", "brief_cols", "brief_columns"},
    {"", "user_format", "columns", Conversion::string},
    {"", "user_mini_status", "user_mini_status"},
    {"", "user_status0", "status_0", Conversion::string},
    {"", "user_status1", "status_1", Conversion::string},
    {"", "user_status2", "status_2", Conversion::string},
    {"", "user_status3", "status_3", Conversion::string},
};

std::string_view trim(std::string_view value) {
    const auto begin = value.find_first_not_of(" \t\r");
    if (begin == std::string_view::npos) return {};
    return value.substr(begin, value.find_last_not_of(" \t\r") - begin + 1U);
}

bool boolean(std::string value) {
    std::ranges::transform(value, value.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    if (value == "true" || value == "1" || value == "yes") return true;
    if (value == "false" || value == "0" || value == "no") return false;
    throw std::runtime_error("expected true/false or 1/0, got " + value);
}

std::string encode_flat(const std::string_view value) {
    std::string result;
    for (const auto character : value) {
        if (character == '\\') result += "\\\\";
        else if (character == '\n') result += "\\n";
        else if (character == '\r') result += "\\r";
        else result += character;
    }
    return result;
}

std::string decode_string(const std::string_view value, const bool native) {
    std::string result;
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] != '\\') {
            result += value[index];
            continue;
        }
        if (++index == value.size()) {
            if (native) throw std::runtime_error("incomplete string escape");
            result += '\\';
            break;
        }
        switch (value[index]) {
            case 'n': result += '\n'; break;
            case 'r': result += '\r'; break;
            case '\\': result += '\\'; break;
            case 's': if (native) result += ' '; else result += 's'; break;
            case 't': if (native) result += '\t'; else result += 't'; break;
            default:
                if (native) throw std::runtime_error("unsupported string escape: \\" + std::string(1, value[index]));
                result += value[index];
                break;
        }
    }
    return result;
}

std::string encode_native(const std::string_view value) {
    std::string result;
    bool leading = true;
    const auto last = value.find_last_not_of(' ');
    for (std::size_t index = 0; index < value.size(); ++index) {
        const auto character = value[index];
        if (character == '\\') result += "\\\\";
        else if (character == '\n') result += "\\n";
        else if (character == '\r') result += "\\r";
        else if (character == '\t') result += "\\t";
        else if (character == ' ' && (leading || last == std::string_view::npos || index > last)) result += "\\s";
        else result += character;
        if (character != ' ') leading = false;
    }
    return result;
}

std::string convert_input(const NativeOption& option, const std::string& value) {
    switch (option.conversion) {
        case Conversion::string: return encode_flat(decode_string(value, true));
        case Conversion::codepage: {
            const auto decoded = decode_string(value, true);
            return decoded == "~" || decoded == "Other_8_bit" ? "" : encode_flat(decoded);
        }
        case Conversion::inverted: return boolean(value) ? "false" : "true";
        case Conversion::save_mode:
            if (value == "0") return "in-place";
            if (value == "1") return "atomic";
            if (value == "2") return "backup";
            throw std::runtime_error("editor save mode must be 0 (quick), 1 (safe), or 2 (backup)");
        case Conversion::listing:
            if (value == "user") return "custom";
            if (value == "full" || value == "brief" || value == "long") return value;
            throw std::runtime_error("unsupported panel listing mode: " + value);
        case Conversion::sort: return value == "mtime" ? "modified" : value;
        case Conversion::typewriter:
        case Conversion::paragraph: return boolean(value) ? "true" : "false";
        case Conversion::plain: return value;
    }
    throw std::runtime_error("invalid configuration conversion");
}

std::string convert_output(const NativeOption& option, const std::string& value) {
    switch (option.conversion) {
        case Conversion::string: return encode_native(decode_string(value, false));
        case Conversion::codepage: return value.empty() ? "Other_8_bit" : encode_native(decode_string(value, false));
        case Conversion::inverted: return boolean(value) ? "false" : "true";
        case Conversion::save_mode:
            if (value == "in-place") return "0";
            if (value == "atomic") return "1";
            if (value == "backup") return "2";
            throw std::runtime_error("invalid editor save mode: " + value);
        case Conversion::listing: return value == "custom" ? "user" : value;
        case Conversion::sort: return value == "modified" ? "mtime" : value;
        case Conversion::typewriter: return value == "typewriter" ? "true" : "false";
        case Conversion::paragraph: return value == "dynamic" ? "true" : "false";
        case Conversion::plain: return value;
    }
    throw std::runtime_error("invalid configuration conversion");
}

struct OutputSetting {
    std::string section;
    std::string key;
    std::string value;
    bool written{false};
};

bool panel_section(const std::string_view section) {
    return section == "New Left Panel" || section == "New Right Panel";
}

}  // namespace

std::string config_setting_location(const ConfigIniDocument& document, const ConfigIniLine& line) {
    return document.path.string() + ":" + std::to_string(line.number) + ": " +
        (line.section.empty() ? "" : "[" + line.section + "] ") + line.key;
}

ConfigIniDocument read_config_document(const std::filesystem::path& path) {
    ConfigIniDocument document;
    document.path = path;
    document.native = path.filename() == "ini" || path.extension() == ".ini";
    std::error_code error;
    const auto status = std::filesystem::status(path, error);
    if (error == std::errc::no_such_file_or_directory) {
        const auto link = std::filesystem::symlink_status(path, error);
        if (!error && std::filesystem::is_symlink(link)) {
            throw std::runtime_error("configuration symlink has no target: " + path.string());
        }
        if (!error || error == std::errc::no_such_file_or_directory) return document;
    }
    if (error) throw std::filesystem::filesystem_error("inspect configuration", path, error);
    if (!std::filesystem::is_regular_file(status)) {
        throw std::runtime_error("configuration is not a regular file: " + path.string());
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot read configuration: " + path.string());
    document.present = true;
    std::string section;
    std::size_t number = 0;
    for (std::string raw; std::getline(input, raw);) {
        auto text = trim(raw);
        if (number == 0U && text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
        ConfigIniLine line{raw, section, {}, {}, ++number, false};
        if (text.empty() || text.front() == '#' || text.front() == ';') {
            document.lines.push_back(std::move(line));
            continue;
        }
        if (text.front() == '[') {
            if (text.size() < 3U || text.back() != ']' || trim(text.substr(1, text.size() - 2U)).empty()) {
                throw std::runtime_error(config_setting_location(document, line) + ": malformed section header");
            }
            document.native = true;
            section = std::string(trim(text.substr(1, text.size() - 2U)));
            line.section = section;
        } else {
            const auto equals = raw.find('=');
            if (equals == std::string::npos || trim(std::string_view(raw).substr(0, equals)).empty()) {
                throw std::runtime_error(config_setting_location(document, line) + ": expected key=value");
            }
            line.key = std::string(trim(std::string_view(raw).substr(0, equals)));
            line.value = raw.substr(equals + 1U);
            if (!section.empty() && section != "mcpp") line.value = std::string(trim(line.value));
            else if (!line.value.empty() && line.value.back() == '\r') line.value.pop_back();
        }
        document.lines.push_back(std::move(line));
    }
    if (input.bad()) throw std::runtime_error("failed while reading configuration: " + path.string());
    return document;
}

std::optional<std::pair<std::string, std::string>> translate_config_setting(
    const ConfigIniLine& line, const bool native
) {
    if (line.key.empty()) return std::nullopt;
    if (!native || line.section.empty() || line.section == "mcpp") return std::pair{line.key, line.value};
    if (panel_section(line.section)) {
        for (const auto& option : panel_options) {
            if (option.key == line.key) {
                return std::pair{std::string(line.section == "New Left Panel" ? "left_" : "right_") +
                    std::string(option.flat), convert_input(option, line.value)};
            }
        }
    }
    if (line.section == "Panelize") {
        std::ostringstream value;
        value << std::quoted(encode_flat(line.key)) << ' ' << std::quoted(encode_flat(decode_string(line.value, true)));
        return std::pair{std::string("panelize_command"), std::move(value).str()};
    }
    for (const auto& option : native_options) {
        if (option.section == line.section && option.key == line.key) {
            const auto key = option.conversion == Conversion::typewriter ? "native_typewriter_wrap" :
                option.conversion == Conversion::paragraph ? "native_paragraph_wrap" : option.flat;
            return std::pair{std::string(key), convert_input(option, line.value)};
        }
    }
    return std::nullopt;
}

std::string serialize_config_document(
    const ConfigIniDocument& document, const std::string_view flat,
    const bool panels_only, const bool separate_panels
) {
    std::vector<OutputSetting> settings;
    std::map<std::string, std::vector<std::string>> values;
    std::istringstream input{std::string(flat)};
    for (std::string line; std::getline(input, line);) {
        const auto equals = line.find('=');
        if (equals != std::string::npos) values[line.substr(0, equals)].push_back(line.substr(equals + 1U));
    }
    std::set<std::string> native_fields;
    const auto add = [&](const std::string_view section, const std::string_view key, const std::string& value) {
        settings.push_back({std::string(section), std::string(key), value, false});
    };
    if (document.native) {
        {
            for (const auto& option : native_options) {
                auto found = values.find(std::string(option.flat));
                if (found == values.end() && (option.flat == "layout_vertical_equal" || option.flat == "layout_horizontal_equal")) {
                    found = values.find("equal_split");
                }
                const bool directory = option.section == "Dirs";
                if (found != values.end() && (panels_only ? directory : !directory || !separate_panels)) {
                    add(option.section, option.key, convert_output(option, found->second.back()));
                }
                native_fields.emplace(option.flat);
            }
            native_fields.emplace("equal_split");
        }
        for (const auto* section : {"New Left Panel", "New Right Panel"}) {
            const std::string prefix = std::string_view(section) == "New Left Panel" ? "left_" : "right_";
            for (const auto& option : panel_options) {
                const auto field = prefix + std::string(option.flat);
                auto found = values.find(field);
                if (found == values.end() && option.flat == "sort") found = values.find("sort");
                if (found == values.end() && option.flat == "descending") found = values.find("sort_descending");
                if (found != values.end() && (panels_only || !separate_panels)) {
                    add(section, option.key, convert_output(option, found->second.back()));
                }
                native_fields.insert(field);
            }
        }
        if (!panels_only) {
            const auto commands = values.find("panelize_command");
            bool native_commands = true;
            std::set<std::string> names;
            std::vector<std::pair<std::string, std::string>> parsed_commands;
            if (commands != values.end()) {
                for (const auto& value : commands->second) {
                    std::istringstream fields(value);
                    std::string label, command;
                    if (!(fields >> std::quoted(label) >> std::quoted(command))) throw std::runtime_error("invalid saved panelize command");
                    label = decode_string(label, false);
                    command = decode_string(command, false);
                    if (label.empty() || trim(label) != label || label.find_first_of("=\r\n") != std::string::npos ||
                        label.front() == '[' || label.front() == '#' || label.front() == ';' || !names.insert(label).second) {
                        native_commands = false;
                    }
                    parsed_commands.emplace_back(std::move(label), std::move(command));
                }
            } else {
                native_commands = false;
            }
            if (native_commands) {
                for (const auto& [label, command] : parsed_commands) add("Panelize", label, encode_native(command));
                native_fields.emplace("panelize_commands_saved");
                native_fields.emplace("panelize_command");
            }
        }
    }
    if (!panels_only) {
        if (document.native) add("mcpp", "preferences_saved", "true");
        for (const auto& [key, entries] : values) {
            if (native_fields.contains(key)) continue;
            for (const auto& value : entries) add(document.native ? "mcpp" : "", key, value);
        }
    }
    std::ostringstream output;
    for (const auto& line : document.lines) {
        if (!line.key.empty() && line.applied) {
            for (auto& setting : settings) {
                if (!setting.written && setting.section == line.section && setting.key == line.key) {
                    output << setting.key << '=' << setting.value << '\n';
                    setting.written = true;
                }
            }
        } else {
            output << line.raw << '\n';
        }
    }
    std::string section;
    bool first = true;
    for (auto& setting : settings) {
        if (setting.written) continue;
        if (first || section != setting.section) {
            section = setting.section;
            if (!section.empty()) output << "\n[" << section << "]\n";
            first = false;
        }
        output << setting.key << '=' << setting.value << '\n';
    }
    if (!output) throw std::runtime_error("cannot serialize configuration");
    return std::move(output).str();
}

}  // namespace mcpp
