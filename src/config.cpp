#include "mcpp/config.hpp"
#include "mcpp/config_ini.hpp"
#include "mcpp/listing.hpp"
#include "mcpp/spelling.hpp"
#include "mcpp/syntax.hpp"
#include "mcpp/local_tools.hpp"

#include <algorithm>
#include <charconv>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <fcntl.h>
#include <sstream>
#include <map>
#include <limits>
#include <set>
#include <stdexcept>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace mcpp {
namespace {

bool parse_bool(const std::string& value, const std::string& key) {
    auto normalized = value;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](const unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    if (normalized == "true" || normalized == "1" || normalized == "yes") {
        return true;
    }
    if (normalized == "false" || normalized == "0" || normalized == "no") {
        return false;
    }
    throw std::runtime_error("invalid boolean setting: " + key + "=" + value);
}

std::size_t parse_width(
    const std::string_view value,
    const std::string_view key
) {
    std::size_t parsed = 0;
    const auto result = std::from_chars(
        value.data(),
        value.data() + value.size(),
        parsed
    );
    if (result.ec != std::errc{} ||
        result.ptr != value.data() + value.size() ||
        parsed < 1U ||
        parsed > 32U) {
        throw std::runtime_error(std::string(key) + " must be 1 through 32");
    }
    return parsed;
}

std::string sort_name(const SortField field) {
    switch (field) {
        case SortField::extension:
            return "extension";
        case SortField::size:
            return "size";
        case SortField::modified:
            return "modified";
        case SortField::name:
            return "name";
        case SortField::unsorted: return "unsorted";
        case SortField::version: return "version";
        case SortField::accessed: return "atime";
        case SortField::changed: return "ctime";
        case SortField::inode: return "inode";
    }
    return "name";
}

SortField parse_sort(const std::string& value) {
    if (value == "extension") {
        return SortField::extension;
    }
    if (value == "size") {
        return SortField::size;
    }
    if (value == "modified") {
        return SortField::modified;
    }
    if (value == "mtime") return SortField::modified;
    if (value == "name") return SortField::name;
    if (value == "unsorted") return SortField::unsorted;
    if (value == "version") return SortField::version;
    if (value == "atime") return SortField::accessed;
    if (value == "ctime") return SortField::changed;
    if (value == "inode") return SortField::inode;
    throw std::runtime_error("invalid sort field: " + value);
}

std::string listing_name(const ListingFormat format) {
    switch (format) {
        case ListingFormat::full: return "full";
        case ListingFormat::brief: return "brief";
        case ListingFormat::long_list: return "long";
        case ListingFormat::custom: return "custom";
    }
    throw std::runtime_error("invalid listing format");
}

ListingFormat parse_listing(const std::string& value) {
    if (value == "full") return ListingFormat::full;
    if (value == "brief") return ListingFormat::brief;
    if (value == "long") return ListingFormat::long_list;
    if (value == "custom") return ListingFormat::custom;
    throw std::runtime_error("invalid listing format: " + value);
}

std::string_view display_name(const PanelDisplay display) {
    switch (display) {
        case PanelDisplay::listing: return "listing";
        case PanelDisplay::quick_view: return "quickview";
        case PanelDisplay::information: return "info";
        case PanelDisplay::tree: return "tree";
    }
    throw std::runtime_error("invalid panel display");
}

PanelDisplay parse_display(const std::string& value) {
    if (value == "listing") return PanelDisplay::listing;
    if (value == "quickview") return PanelDisplay::quick_view;
    if (value == "info") return PanelDisplay::information;
    if (value == "tree") return PanelDisplay::tree;
    throw std::runtime_error("unsupported saved panel display: " + value);
}

std::string format_panel_date(const std::string& format, const std::tm& time) {
    if (format.empty()) return {};
    if (format.find_first_of("\r\n\t") != std::string::npos || format.find('\0') != std::string::npos) {
        throw std::runtime_error("panel time formats must fit a single line");
    }
    std::array<char, 4096> buffer{};
    const auto size = std::strftime(buffer.data(), buffer.size(), format.c_str(), &time);
    if (size == 0U) throw std::runtime_error("panel time format exceeds 4095 bytes");
    if (std::string_view(buffer.data(), size).find_first_of("\r\n\t") != std::string_view::npos) {
        throw std::runtime_error("panel time formats must fit a single line");
    }
    return {buffer.data(), size};
}

std::set<std::string> saved_position_paths(const Config& config) {
    std::set<std::string> paths;
    const auto include = [&](const std::string& path) {
        if (!config.max_saved_positions || paths.size() < *config.max_saved_positions) paths.insert(path);
    };
    for (const auto& path : config.editor_history) {
        if (config.editor_positions.contains(path) || config.viewer_positions.contains(path)) include(path);
    }
    for (const auto& path : config.viewer_history) {
        if (config.editor_positions.contains(path) || config.viewer_positions.contains(path)) include(path);
    }
    for (const auto& [path, position] : config.editor_positions) {
        static_cast<void>(position);
        include(path);
    }
    for (const auto& [path, position] : config.viewer_positions) {
        static_cast<void>(position);
        include(path);
    }
    return paths;
}

std::string encode_value(const std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const auto character : value) {
        if (character == '\\') {
            result += "\\\\";
        } else if (character == '\n') {
            result += "\\n";
        } else if (character == '\r') {
            result += "\\r";
        } else {
            result += character;
        }
    }
    return result;
}

std::string decode_value(const std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] != '\\' || index + 1U >= value.size()) {
            result += value[index];
            continue;
        }
        const auto escaped = value[++index];
        if (escaped == 'n') {
            result += '\n';
        } else if (escaped == 'r') {
            result += '\r';
        } else {
            result += escaped;
        }
    }
    return result;
}

}  // namespace

template <typename Configuration>
auto panel_bool_settings(Configuration& config) {
    using Pointer = decltype(&config.show_mini_status);
    return std::array<std::pair<std::string_view, Pointer>, 13>{{
        {"show_mini_status", &config.show_mini_status}, {"use_si_units", &config.use_si_units},
        {"show_backups", &config.show_backups}, {"fast_reload", &config.fast_reload},
        {"mark_moves_down", &config.mark_moves_down}, {"simple_swap", &config.simple_swap},
        {"auto_save_panels_setup", &config.auto_save_panels_setup},
        {"navigate_with_arrows", &config.navigate_with_arrows},
        {"scroll_pages", &config.scroll_pages}, {"scroll_center", &config.scroll_center},
        {"mouse_page_scrolling", &config.mouse_page_scrolling},
        {"highlight_file_types", &config.highlight_file_types}, {"highlight_permissions", &config.highlight_permissions}
    }};
}

template <typename Configuration>
auto general_bool_settings(Configuration& config) {
    using Pointer = decltype(&config.verbose_operation);
    return std::array<std::pair<std::string_view, Pointer>, 19>{{
        {"verbose_operation", &config.verbose_operation}, {"compute_totals", &config.compute_totals},
        {"classic_progressbar", &config.classic_progressbar}, {"mkdir_autoname", &config.mkdir_autoname},
        {"preallocate_space", &config.preallocate_space}, {"single_escape", &config.single_escape},
        {"use_internal_edit", &config.use_internal_edit}, {"use_internal_view", &config.use_internal_view},
        {"ask_new_filename", &config.ask_new_filename}, {"auto_menus", &config.auto_menus},
        {"drop_menus", &config.drop_menus}, {"shell_patterns", &config.shell_patterns},
        {"completion_show_all", &config.completion_show_all}, {"rotating_dash", &config.rotating_dash},
        {"cd_follows_links", &config.cd_follows_links}, {"safe_delete", &config.safe_delete},
        {"safe_overwrite", &config.safe_overwrite}, {"auto_save_setup", &config.auto_save_setup},
        {"use_subshell", &config.use_subshell}
    }};
}

template <typename Configuration>
auto editor_bool_settings(Configuration& config) {
    using Pointer = decltype(&config.editor_fake_half_tabs);
    return std::array<std::pair<std::string_view, Pointer>, 12>{{
        {"editor_fake_half_tabs", &config.editor_fake_half_tabs},
        {"editor_backspace_through_tabs", &config.editor_backspace_through_tabs},
        {"editor_fill_tabs_with_spaces", &config.editor_fill_tabs_with_spaces},
        {"editor_confirm_save", &config.editor_confirm_save},
        {"editor_save_position", &config.editor_save_position},
        {"editor_visible_tabs", &config.editor_visible_tabs},
        {"editor_visible_trailing_spaces", &config.editor_visible_trailing_spaces},
        {"editor_persistent_selection", &config.editor_persistent_selection},
        {"editor_drop_selection_on_copy", &config.editor_drop_selection_on_copy},
        {"editor_state_full_filename", &config.editor_state_full_filename},
        {"editor_cursor_beyond_eol", &config.editor_cursor_beyond_eol},
        {"editor_group_undo", &config.editor_group_undo}
    }};
}

template <typename Configuration>
auto diff_bool_settings(Configuration& config) {
    using Pointer = decltype(&config.diff_ignore_case);
    return std::array<std::pair<std::string_view, Pointer>, 7>{{
        {"diff_ignore_case", &config.diff_ignore_case}, {"diff_ignore_tab_expansion", &config.diff_ignore_tab_expansion},
        {"diff_ignore_space_change", &config.diff_ignore_space_change}, {"diff_ignore_all_space", &config.diff_ignore_all_space},
        {"diff_strip_trailing_cr", &config.diff_strip_trailing_cr}, {"diff_show_numbers", &config.diff_show_numbers},
        {"diff_show_symbols", &config.diff_show_symbols}
    }};
}

Config Config::load(const std::filesystem::path& path) {
    Config result;
    auto loaded = std::make_shared<ConfigSource>();
    loaded->primary = read_config_document(path);
    if (loaded->primary.native && path.filename() == "ini") {
        bool preferences_saved = false;
        for (const auto& line : loaded->primary.lines) {
            if (line.section == "mcpp" && line.key == "preferences_saved") {
                try {
                    preferences_saved = parse_bool(line.value, line.key);
                } catch (const std::exception& error) {
                    throw std::runtime_error(config_setting_location(loaded->primary, line) + ": " + error.what());
                }
            }
        }
        if (!preferences_saved) {
            const auto legacy = path.parent_path() / "config";
            const auto legacy_document = read_config_document(legacy);
            if (legacy_document.present) result = Config::load(legacy);
        }
        loaded->panels = read_config_document(path.parent_path() / "panels.ini");
    }
    bool visible_tabs_seen = false;
    bool visible_trailing_seen = false;
    bool native_wrap_seen = false;
    bool typewriter_wrap = false;
    bool paragraph_wrap = false;
    std::string wrap_location;
    const auto apply_document = [&](ConfigIniDocument& document, const bool panels_only) {
      bool panelize_seen = false;
      for (auto& line : document.lines) {
        if (line.key.empty()) continue;
        try {
        const auto translated = panels_only && line.section != "New Left Panel" && line.section != "New Right Panel" &&
            line.section != "Dirs"
            ? std::nullopt : translate_config_setting(line, document.native);
        if (!translated) {
            result.diagnostics.push_back(config_setting_location(document, line) +
                ": not supported by mcpp; preserved without applying it");
            continue;
        }
        const auto& [key, value] = *translated;
        line.applied = true;
        if (document.native && line.section == "Panels") {
            if (key == "show_hidden") result.panel_hidden = {};
            if (key == "directories_first") result.panel_directories_first = {};
        }
        if (line.section == "Panelize" && !panelize_seen) {
            result.panelize_commands.clear();
            panelize_seen = true;
        }
        const auto panel_settings = panel_bool_settings(result);
        const auto panel_boolean = std::ranges::find(panel_settings, key,
            &std::pair<std::string_view, bool*>::first);
        if (panel_boolean != panel_settings.end()) {
            *panel_boolean->second = parse_bool(value, key);
            continue;
        }
        const auto general_settings = general_bool_settings(result);
        const auto general_boolean = std::ranges::find(general_settings, key,
            &std::pair<std::string_view, bool*>::first);
        if (general_boolean != general_settings.end()) {
            *general_boolean->second = parse_bool(value, key);
            continue;
        }
        const auto editor_settings = editor_bool_settings(result);
        const auto editor_boolean = std::ranges::find(editor_settings, key,
            &std::pair<std::string_view, bool*>::first);
        if (editor_boolean != editor_settings.end()) {
            *editor_boolean->second = parse_bool(value, key);
            if (key == "editor_visible_tabs") visible_tabs_seen = true;
            if (key == "editor_visible_trailing_spaces") visible_trailing_seen = true;
            continue;
        }
        const auto diff_settings = diff_bool_settings(result);
        const auto diff_boolean = std::ranges::find(diff_settings, key, &std::pair<std::string_view, bool*>::first);
        if (diff_boolean != diff_settings.end()) {
            *diff_boolean->second = parse_bool(value, key);
            continue;
        }
        if (key == "preferences_saved" && line.section == "mcpp") {
            static_cast<void>(parse_bool(value, key));
        } else if (key == "show_hidden") {
            result.show_hidden = parse_bool(value, key);
        } else if (key == "confirm_delete") {
            result.confirm_delete = parse_bool(value, key);
        } else if (key == "confirm_overwrite") {
            result.confirm_overwrite = parse_bool(value, key);
        } else if (key == "confirm_execute") {
            result.confirm_execute = parse_bool(value, key);
        } else if (key == "confirm_exit") {
            result.confirm_exit = parse_bool(value, key);
        } else if (key == "confirm_hotlist_delete") {
            result.confirm_hotlist_delete = parse_bool(value, key);
        } else if (key == "confirm_history_cleanup") {
            result.confirm_history_cleanup = parse_bool(value, key);
        } else if (key == "directories_first") {
            result.directories_first = parse_bool(value, key);
        } else if (key == "sort") {
            result.sort = parse_sort(value);
        } else if (key == "sort_descending") {
            result.sort_descending = parse_bool(value, key);
        } else if (key == "reverse_files_only") {
            result.reverse_files_only = parse_bool(value, key);
        } else if (key == "layout_horizontal") {
            result.layout_horizontal = parse_bool(value, key);
        } else if (key == "equal_split") {
            result.equal_split = parse_bool(value, key);
        } else if (key == "layout_vertical_equal") {
            result.layout_vertical_equal = parse_bool(value, key);
        } else if (key == "layout_horizontal_equal") {
            result.layout_horizontal_equal = parse_bool(value, key);
        } else if (key == "menubar_visible") {
            result.menubar_visible = parse_bool(value, key);
        } else if (key == "command_prompt") {
            result.command_prompt = parse_bool(value, key);
        } else if (key == "keybar_visible") {
            result.keybar_visible = parse_bool(value, key);
        } else if (key == "hintbar_visible") {
            result.hintbar_visible = parse_bool(value, key);
        } else if (key == "xterm_title") {
            result.xterm_title = parse_bool(value, key);
        } else if (key == "show_free_space") {
            result.show_free_space = parse_bool(value, key);
        } else if (key == "quick_search_mode") {
            std::size_t mode = 0;
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), mode);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || mode > 2U) {
                throw std::runtime_error("invalid quick search mode");
            }
            result.quick_search_mode = mode;
        } else if (key == "saved_left_path") {
            result.saved_left_path = decode_value(value);
        } else if (key == "saved_right_path") {
            result.saved_right_path = decode_value(value);
        } else if (key == "current_is_left") {
            result.current_is_left = parse_bool(value, key);
        } else if (key == "other_directory") {
            result.other_directory = decode_value(value);
            if (result.other_directory->empty()) throw std::runtime_error("other_dir needs a directory path");
        } else if (key == "viewer_wrap") {
            result.viewer_wrap = parse_bool(value, key);
        } else if (key == "viewer_mouse_page_scrolling") {
            result.viewer_mouse_page_scrolling = parse_bool(value, key);
        } else if (key == "viewer_remember_file_position") {
            result.viewer_remember_file_position = parse_bool(value, key);
        } else if (key == "source_codepage") {
            auto encoding = decode_value(value);
            if (encoding == "~" || encoding == "Other_8_bit") encoding.clear();
            if (!encoding.empty() &&
                std::ranges::find(supported_codepages(), encoding, &Codepage::id) == supported_codepages().end()) {
                throw std::runtime_error("unsupported file encoding: " + encoding);
            }
            result.source_codepage = std::move(encoding);
        } else if (key == "timeformat_recent" || key == "timeformat_old") {
            auto format = decode_value(value);
            std::tm time{};
            time.tm_year = 126;
            time.tm_mday = 1;
            static_cast<void>(format_panel_date(format, time));
            (key == "timeformat_recent" ? result.timeformat_recent : result.timeformat_old) = std::move(format);
        } else if (key == "max_saved_positions") {
            std::size_t limit = 0;
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), limit);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
                limit > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
                throw std::runtime_error("filepos_max_saved_entries must be a nonnegative integer");
            }
            result.max_saved_positions = limit;
        } else if (key == "escape_timeout" || key == "pause_after_run") {
            int number = 0;
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
                number < 0 || (key == "pause_after_run" && number > 2)) {
                throw std::runtime_error("invalid general setting: " + key);
            }
            if (key == "escape_timeout") result.escape_timeout = number;
            else result.pause_after_run = static_cast<std::size_t>(number);
        } else if (key == "panel_left_size" || key == "panel_top_size") {
            int size = 0;
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), size);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || size < 0) {
                throw std::runtime_error("invalid panel split size: " + value);
            }
            (key == "panel_left_size" ? result.panel_left_size : result.panel_top_size) = size;
        } else if (key.starts_with("left_") || key.starts_with("right_")) {
            const auto index = key.starts_with("left_") ? 0U : 1U;
            const auto field = key.substr(index == 0U ? 5U : 6U);
            if (field == "display") {
                result.panel_display[index] = parse_display(value);
            } else if (field == "listing") {
                result.panel_listing[index] = parse_listing(value);
            } else if (field == "columns") {
                result.panel_columns[index] = decode_value(value);
                static_cast<void>(parse_listing_format(result.panel_columns[index]));
            } else if (field == "brief_columns") {
                std::size_t columns = 0;
                const auto parsed = std::from_chars(value.data(), value.data() + value.size(), columns);
                if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
                    columns < 1U || columns > 9U) throw std::runtime_error("brief columns must be 1 through 9");
                result.panel_brief_columns[index] = columns;
            } else if (field == "user_mini_status") {
                result.panel_user_mini_status[index] = parse_bool(value, key);
            } else if (field.starts_with("status_") && field.size() == 8U &&
                       field.back() >= '0' && field.back() <= '3') {
                result.panel_status_formats[index][static_cast<std::size_t>(field.back() - '0')] = decode_value(value);
                static_cast<void>(parse_listing_format(
                    result.panel_status_formats[index][static_cast<std::size_t>(field.back() - '0')]));
            } else if (field == "encoding") {
                if (!value.empty() &&
                    std::ranges::find(supported_codepages(), value, &Codepage::id) == supported_codepages().end()) {
                    throw std::runtime_error("unsupported filename encoding: " + value);
                }
                result.panel_encoding[index] = value;
            } else if (field == "sort") {
                result.panel_sort[index] = parse_sort(value);
            } else if (field == "descending") {
                result.panel_descending[index] = parse_bool(value, key);
            } else if (field == "hidden") {
                result.panel_hidden[index] = parse_bool(value, key);
            } else if (field == "directories_first") {
                result.panel_directories_first[index] = parse_bool(value, key);
            } else if (field == "case_sensitive") {
                result.panel_case_sensitive[index] = parse_bool(value, key);
            } else if (field == "executable_first") {
                result.panel_executable_first[index] = parse_bool(value, key);
            } else {
                line.applied = false;
            }
        } else if (key == "theme") {
            result.theme = decode_value(value);
        } else if (key == "keymap") {
            result.keymap_file = decode_value(value);
        } else if (key == "vfs_timeout") {
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result.vfs_timeout);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
                result.vfs_timeout > 86400U) {
                throw std::runtime_error("invalid VFS cache timeout: " + value);
            }
        } else if (key == "shadows") {
            result.shadows = parse_bool(value, key);
        } else if (key == "editor_syntax") {
            result.editor_syntax = parse_bool(value, key);
        } else if (key == "editor_line_numbers") {
            result.editor_line_numbers = parse_bool(value, key);
        } else if (key == "editor_tab_width") {
            result.editor_tab_width = parse_width(
                value,
                key
            );
        } else if (key == "editor_indent_width") {
            result.editor_indent_width = parse_width(
                value,
                key
            );
        } else if (key == "editor_wrap_length") {
            std::size_t width = 0;
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), width);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || width < 1U || width > 4096U) {
                throw std::runtime_error("invalid editor word wrap length");
            }
            result.editor_wrap_length = width;
        } else if (key == "editor_auto_indent") {
            result.editor_auto_indent = parse_bool(value, key);
        } else if (key == "editor_show_whitespace") {
            result.editor_show_whitespace = parse_bool(value, key);
            if (!visible_tabs_seen) result.editor_visible_tabs = result.editor_show_whitespace;
            if (!visible_trailing_seen) result.editor_visible_trailing_spaces = result.editor_show_whitespace;
        } else if (key == "editor_wrap_mode") {
            if (value == "none") result.editor_wrap_mode = 0;
            else if (value == "dynamic") result.editor_wrap_mode = 1;
            else if (value == "typewriter") result.editor_wrap_mode = 2;
            else throw std::runtime_error("invalid editor wrap mode");
            if (native_wrap_seen) {
                typewriter_wrap = result.editor_wrap_mode == 2U;
                paragraph_wrap = result.editor_wrap_mode == 1U;
            }
        } else if (key == "native_typewriter_wrap" || key == "native_paragraph_wrap") {
            native_wrap_seen = true;
            (key == "native_typewriter_wrap" ? typewriter_wrap : paragraph_wrap) = parse_bool(value, key);
            wrap_location = config_setting_location(document, line);
        } else if (key == "diff_tab_size") {
            result.diff_tab_size = parse_width(value, key);
        } else if (key == "diff_quality") {
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result.diff_quality);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || result.diff_quality > 2U) {
                throw std::runtime_error("invalid diff algorithm");
            }
        } else if (key == "editor_cursor_after_inserted_block") {
            result.editor_cursor_after_inserted_block = parse_bool(value, key);
        } else if (key == "editor_save_mode") {
            if (value != "atomic" && value != "in-place" && value != "backup") {
                throw std::runtime_error("unknown editor save mode: " + value);
            }
            result.editor_save_mode = value;
        } else if (key == "editor_backup_extension") {
            result.editor_backup_extension = decode_value(value);
        } else if (key == "editor_check_newline") {
            result.editor_check_newline = parse_bool(value, key);
        } else if (key == "editor_syntax_language") {
            const auto& languages = SyntaxHighlighter::languages();
            if (std::ranges::find(languages, value) == languages.end() &&
                !(value.starts_with("external:") && value.size() > 9U)) {
                throw std::runtime_error("unknown syntax language: " + value);
            }
            result.editor_syntax_language = value;
        } else if (key == "editor_spelling_language") {
            result.editor_spelling_language = decode_value(value);
            validate_spelling_language(result.editor_spelling_language);
        } else if (key == "editor_stop_format_chars") {
            result.editor_stop_format_chars = decode_value(value);
            if (result.editor_stop_format_chars->find('\0') != std::string::npos ||
                result.editor_stop_format_chars->find_first_of("\r\n") != std::string::npos) {
                throw std::runtime_error("editor_stop_format_chars must fit a single line");
            }
        } else if (key == "bookmark") {
            result.bookmarks.push_back(decode_value(value));
        } else if (key == "editor_history") {
            result.editor_history.push_back(decode_value(value));
        } else if (key == "viewer_history") {
            result.viewer_history.push_back(decode_value(value));
        } else if (key == "command_history") {
            result.command_history.push_back(decode_value(value));
        } else if (key == "panelize_commands_saved") {
            if (parse_bool(value, key)) result.panelize_commands.clear();
        } else if (key == "panelize_command") {
            std::istringstream fields(value);
            std::string label, command;
            if (!(fields >> std::quoted(label) >> std::quoted(command))) {
                throw std::runtime_error("invalid saved panelize command");
            }
            fields >> std::ws;
            if (!fields.eof() || label.empty() || command.empty()) throw std::runtime_error("invalid saved panelize command");
            result.panelize_commands.emplace_back(decode_value(label), decode_value(command));
        } else if (key == "learned_key" || key == "editor_macro") {
            std::istringstream fields(value);
            std::string name, data;
            if (!(fields >> std::quoted(name) >> std::quoted(data))) {
                throw std::runtime_error("invalid named setting: " + key);
            }
            fields >> std::ws;
            if (!fields.eof() || name.empty()) throw std::runtime_error("invalid named setting: " + key);
            (key == "learned_key" ? result.learned_keys : result.editor_macros)[decode_value(name)] =
                decode_value(data);
        } else if (key == "viewer_position") {
            const auto colon = value.find(':');
            std::uint64_t offset = 0;
            if (colon == std::string::npos || colon + 1U == value.size()) {
                throw std::runtime_error("invalid saved viewer position");
            }
            const auto parsed = std::from_chars(value.data(), value.data() + colon, offset);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + colon) {
                throw std::runtime_error("invalid saved viewer byte offset");
            }
            result.viewer_positions[decode_value(value.substr(colon + 1U))] = offset;
        } else if (key == "editor_position") {
            const auto colon = value.find(':');
            if (colon == std::string::npos || colon + 1U == value.size()) {
                throw std::runtime_error("invalid saved editor position");
            }
            std::vector<std::size_t> positions;
            std::istringstream fields(value.substr(0, colon));
            std::string field;
            while (std::getline(fields, field, ',')) {
                std::size_t position = 0;
                const auto parsed = std::from_chars(field.data(), field.data() + field.size(), position);
                if (parsed.ec != std::errc{} || parsed.ptr != field.data() + field.size()) {
                    throw std::runtime_error("invalid saved editor position");
                }
                positions.push_back(position);
            }
            if (positions.size() < 2U) throw std::runtime_error("saved position needs a row and column");
            result.editor_positions[decode_value(value.substr(colon + 1U))] = {
                positions[0], positions[1], {positions.begin() + 2, positions.end()}
            };
        } else {
            line.applied = false;
        }
        if (!line.applied) result.diagnostics.push_back(config_setting_location(document, line) +
            ": not supported by mcpp; preserved without applying it");
        } catch (const std::exception& error) {
            throw std::runtime_error(config_setting_location(document, line) + ": " + error.what());
        }
      }
    };
    apply_document(loaded->primary, false);
    if (loaded->panels) apply_document(*loaded->panels, true);
    if (result.panel_display[0] != PanelDisplay::listing && result.panel_display[1] != PanelDisplay::listing) {
        throw std::runtime_error(path.string() +
            ": mcpp supports one saved auxiliary panel; one panel must use display=listing");
    }
    if (native_wrap_seen) {
        if (typewriter_wrap && paragraph_wrap) {
            throw std::runtime_error(wrap_location + ": typewriter wrap and automatic paragraph formatting cannot both be enabled");
        }
        result.editor_wrap_mode = typewriter_wrap ? 2U : paragraph_wrap ? 1U : 0U;
    }
    result.source = std::move(loaded);
    return result;
}

void Config::save(const std::filesystem::path& path) const {
    if (panel_display[0] != PanelDisplay::listing && panel_display[1] != PanelDisplay::listing) {
        throw std::runtime_error("mcpp supports one saved auxiliary panel; one panel must use display=listing");
    }
    std::ostringstream output;
    output << "show_hidden=" << (show_hidden ? "true" : "false")
           << '\n'
           << "confirm_delete=" << (confirm_delete ? "true" : "false")
           << '\n'
           << "confirm_overwrite="
           << (confirm_overwrite ? "true" : "false") << '\n'
           << "confirm_execute=" << (confirm_execute ? "true" : "false") << '\n'
           << "confirm_exit=" << (confirm_exit ? "true" : "false") << '\n'
           << "confirm_hotlist_delete=" << (confirm_hotlist_delete ? "true" : "false") << '\n'
           << "confirm_history_cleanup=" << (confirm_history_cleanup ? "true" : "false") << '\n'
           << "directories_first="
           << (directories_first ? "true" : "false") << '\n'
           << "sort=" << sort_name(sort) << '\n'
           << "sort_descending="
           << (sort_descending ? "true" : "false") << '\n'
           << "theme=" << encode_value(theme) << '\n'
           << "keymap=" << encode_value(keymap_file) << '\n'
           << "vfs_timeout=" << vfs_timeout << '\n'
           << "shadows=" << (shadows ? "true" : "false") << '\n'
           << "editor_syntax="
           << (editor_syntax ? "true" : "false") << '\n'
           << "editor_line_numbers="
           << (editor_line_numbers ? "true" : "false") << '\n'
           << "editor_tab_width=" << editor_tab_width << '\n'
           << "editor_indent_width=" << editor_indent_width << '\n'
           << "editor_wrap_length=" << editor_wrap_length << '\n'
           << "editor_auto_indent="
           << (editor_auto_indent ? "true" : "false") << '\n'
           << "editor_show_whitespace="
           << (editor_show_whitespace ? "true" : "false") << '\n';
    output << "editor_save_mode=" << editor_save_mode << '\n';
    for (const auto& [key, value] : diff_bool_settings(*this)) {
        output << key << '=' << (*value ? "true" : "false") << '\n';
    }
    output << "diff_quality=" << diff_quality << '\n';
    output << "diff_tab_size=" << diff_tab_size << '\n';
    for (const auto& [key, value] : editor_bool_settings(*this)) {
        output << key << '=' << (*value ? "true" : "false") << '\n';
    }
    if (editor_wrap_mode > 2U) throw std::runtime_error("invalid editor wrap mode");
    output << "editor_wrap_mode=" << std::array{"none", "dynamic", "typewriter"}[editor_wrap_mode] << '\n';
    output << "editor_cursor_after_inserted_block=" << (editor_cursor_after_inserted_block ? "true" : "false") << '\n';
    for (const auto& [key, value] : panel_bool_settings(*this)) {
        output << key << '=' << (*value ? "true" : "false") << '\n';
    }
    for (const auto& [key, value] : general_bool_settings(*this)) {
        output << key << '=' << (*value ? "true" : "false") << '\n';
    }
    output << "escape_timeout=" << escape_timeout << '\n'
           << "pause_after_run=" << pause_after_run << '\n';
    output << "quick_search_mode=" << quick_search_mode << '\n'
           << "saved_left_path=" << encode_value(saved_left_path) << '\n'
           << "saved_right_path=" << encode_value(saved_right_path) << '\n';
    if (current_is_left) output << "current_is_left=" << (*current_is_left ? "true" : "false") << '\n';
    if (other_directory) output << "other_directory=" << encode_value(*other_directory) << '\n';
    output << "viewer_wrap=" << (viewer_wrap ? "true" : "false") << '\n';
    if (viewer_mouse_page_scrolling) {
        output << "viewer_mouse_page_scrolling=" << (*viewer_mouse_page_scrolling ? "true" : "false") << '\n';
    }
    output << "viewer_remember_file_position=" << (viewer_remember_file_position ? "true" : "false") << '\n';
    if (source_codepage) output << "source_codepage=" << encode_value(*source_codepage) << '\n';
    if (timeformat_recent) output << "timeformat_recent=" << encode_value(*timeformat_recent) << '\n';
    if (timeformat_old) output << "timeformat_old=" << encode_value(*timeformat_old) << '\n';
    if (max_saved_positions) output << "max_saved_positions=" << *max_saved_positions << '\n';
    output << "editor_backup_extension=" << encode_value(editor_backup_extension) << '\n'
           << "editor_check_newline=" << (editor_check_newline ? "true" : "false") << '\n';
    output << "reverse_files_only=" << (reverse_files_only ? "true" : "false") << '\n';
    output << "editor_syntax_language=" << editor_syntax_language << '\n';
    output << "editor_spelling_language=" << encode_value(editor_spelling_language) << '\n';
    if (editor_stop_format_chars) output << "editor_stop_format_chars=" << encode_value(*editor_stop_format_chars) << '\n';
    output << "layout_horizontal=" << (layout_horizontal ? "true" : "false") << '\n'
           << "equal_split=" << (equal_split ? "true" : "false") << '\n'
           << "panel_left_size=" << panel_left_size << '\n'
           << "panel_top_size=" << panel_top_size << '\n'
           << "menubar_visible=" << (menubar_visible ? "true" : "false") << '\n'
           << "command_prompt=" << (command_prompt ? "true" : "false") << '\n'
           << "keybar_visible=" << (keybar_visible ? "true" : "false") << '\n'
           << "hintbar_visible=" << (hintbar_visible ? "true" : "false") << '\n'
           << "xterm_title=" << (xterm_title ? "true" : "false") << '\n'
           << "show_free_space=" << (show_free_space ? "true" : "false") << '\n';
    if (layout_vertical_equal) output << "layout_vertical_equal=" << (*layout_vertical_equal ? "true" : "false") << '\n';
    if (layout_horizontal_equal) output << "layout_horizontal_equal=" << (*layout_horizontal_equal ? "true" : "false") << '\n';
    for (std::size_t index = 0; index < 2U; ++index) {
        const auto prefix = index == 0U ? "left_" : "right_";
        output << prefix << "display=" << display_name(panel_display[index]) << '\n'
               << prefix << "listing=" << listing_name(panel_listing[index]) << '\n'
               << prefix << "columns=" << encode_value(panel_columns[index]) << '\n'
               << prefix << "brief_columns=" << panel_brief_columns[index] << '\n'
               << prefix << "user_mini_status=" << (panel_user_mini_status[index] ? "true" : "false") << '\n'
               << prefix << "encoding=" << panel_encoding[index] << '\n';
        for (std::size_t mode = 0; mode < 4U; ++mode) {
            output << prefix << "status_" << mode << '=' << encode_value(panel_status_formats[index][mode]) << '\n';
        }
        if (panel_sort[index]) output << prefix << "sort=" << sort_name(*panel_sort[index]) << '\n';
        if (panel_descending[index]) {
            output << prefix << "descending=" << (*panel_descending[index] ? "true" : "false") << '\n';
        }
        if (panel_hidden[index]) {
            output << prefix << "hidden=" << (*panel_hidden[index] ? "true" : "false") << '\n';
        }
        if (panel_directories_first[index]) {
            output << prefix << "directories_first=" << (*panel_directories_first[index] ? "true" : "false") << '\n';
        }
        if (panel_case_sensitive[index]) {
            output << prefix << "case_sensitive=" << (*panel_case_sensitive[index] ? "true" : "false") << '\n';
        }
        if (panel_executable_first[index]) {
            output << prefix << "executable_first=" << (*panel_executable_first[index] ? "true" : "false") << '\n';
        }
    }
    for (const auto& bookmark : bookmarks) {
        output << "bookmark=" << encode_value(bookmark) << '\n';
    }
    for (const auto& history_path : editor_history) {
        output << "editor_history="
               << encode_value(history_path) << '\n';
    }
    for (const auto& file : viewer_history) output << "viewer_history=" << encode_value(file) << '\n';
    for (const auto& command : command_history) output << "command_history=" << encode_value(command) << '\n';
    output << "panelize_commands_saved=true\n";
    for (const auto& [label, command] : panelize_commands) {
        output << "panelize_command=" << std::quoted(encode_value(label)) << ' '
               << std::quoted(encode_value(command)) << '\n';
    }
    const auto position_paths = saved_position_paths(*this);
    for (const auto& [file, position] : editor_positions) {
        if (!position_paths.contains(file)) continue;
        output << "editor_position=" << position.row << ',' << position.column;
        for (const auto row : position.bookmarks) output << ',' << row;
        output << ':' << encode_value(file) << '\n';
    }
    for (const auto& [file, position] : viewer_positions) {
        if (position_paths.contains(file)) output << "viewer_position=" << position << ':' << encode_value(file) << '\n';
    }
    for (const auto& [key, action] : learned_keys) {
        output << "learned_key=" << std::quoted(encode_value(key)) << ' '
               << std::quoted(encode_value(action)) << '\n';
    }
    for (const auto& [name, data] : editor_macros) {
        output << "editor_macro=" << std::quoted(encode_value(name)) << ' '
               << std::quoted(encode_value(data)) << '\n';
    }
    if (!output) {
        throw std::runtime_error("cannot serialize configuration");
    }
    const auto flat = std::move(output).str();
    ConfigIniDocument primary;
    if (source && source->primary.path == path) primary = source->primary;
    else primary.native = path.filename() == "ini" || path.extension() == ".ini";
    primary.path = path;
    const bool separate_panels = primary.native && path.filename() == "ini";
    std::vector<std::pair<std::filesystem::path, std::string>> files;
    if (separate_panels) {
        ConfigIniDocument panels;
        if (source && source->panels && source->panels->path == path.parent_path() / "panels.ini") {
            panels = *source->panels;
        }
        panels.path = path.parent_path() / "panels.ini";
        panels.native = true;
        files.emplace_back(panels.path, serialize_config_document(panels, flat, true));
    }
    files.emplace_back(path, serialize_config_document(primary, flat, false, separate_panels));
    for (const auto& file : files) {
        std::error_code error;
        const auto status = std::filesystem::symlink_status(file.first, error);
        if (error == std::errc::no_such_file_or_directory) continue;
        if (error) throw std::filesystem::filesystem_error("inspect configuration destination", file.first, error);
        if (!std::filesystem::is_regular_file(status) && !std::filesystem::is_symlink(status)) {
            throw std::runtime_error("configuration destination is not a regular file: " + file.first.string());
        }
    }
    const auto directory = path.has_parent_path() ? path.parent_path() : std::filesystem::path(".");
    if (std::filesystem::create_directories(directory) && chmod(directory.c_str(), 0700) != 0) {
        throw std::runtime_error("cannot protect configuration directory: " + directory.string());
    }
    const auto stage = [](const std::filesystem::path& destination, const std::string& content) {
    auto pattern =
        (destination.parent_path() / ".mcpp-config-XXXXXX").string();
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    int descriptor = mkostemp(buffer.data(), O_CLOEXEC);
    if (descriptor < 0) {
        throw std::runtime_error(
            "cannot create configuration temporary file: " +
            std::string(std::strerror(errno))
        );
    }
    const std::filesystem::path temporary(buffer.data());
    try {
        if (fchmod(descriptor, 0600) != 0) {
            throw std::runtime_error(
                "cannot protect configuration temporary file"
            );
        }
        std::size_t offset = 0;
        while (offset < content.size()) {
            const auto count = ::write(
                descriptor,
                content.data() + offset,
                content.size() - offset
            );
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count <= 0) {
                throw std::runtime_error(
                    "cannot write configuration"
                );
            }
            offset += static_cast<std::size_t>(count);
        }
        if (fsync(descriptor) != 0) {
            throw std::runtime_error(
                "cannot commit configuration"
            );
        }
        if (close(descriptor) != 0) {
            descriptor = -1;
            throw std::runtime_error(
                "cannot close configuration"
            );
        }
        descriptor = -1;
        return temporary;
    } catch (...) {
        if (descriptor >= 0) {
            static_cast<void>(close(descriptor));
        }
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
    };
    std::vector<std::pair<std::filesystem::path, std::filesystem::path>> staged;
    try {
        for (const auto& [destination, content] : files) staged.emplace_back(stage(destination, content), destination);
        for (const auto& [temporary, destination] : staged) std::filesystem::rename(temporary, destination);
    } catch (...) {
        for (const auto& [temporary, destination] : staged) {
            static_cast<void>(destination);
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
        }
        throw;
    }
}

void Config::save() const {
    save(source ? source->primary.path : default_config_path());
}

bool Config::split_is_equal() const {
    return (layout_horizontal ? layout_horizontal_equal : layout_vertical_equal).value_or(equal_split);
}

void Config::set_split_equal(const bool value) {
    equal_split = value;
    (layout_horizontal ? layout_horizontal_equal : layout_vertical_equal) = value;
}

std::string Config::format_panel_time(
    const std::chrono::system_clock::time_point point,
    const std::chrono::system_clock::time_point reference
) const {
    const auto value = std::chrono::system_clock::to_time_t(point);
    std::tm local{};
    if (localtime_r(&value, &local) == nullptr) return "--- -- --:--";
    const bool recent = point <= reference && point >= reference - std::chrono::hours(24 * 183);
    const auto& format = recent ? timeformat_recent : timeformat_old;
    return format_panel_date(format.value_or("%b %d %H:%M"), local);
}

std::filesystem::path default_config_path() {
    std::filesystem::path directory;
    if (const auto* value = std::getenv("XDG_CONFIG_HOME");
        value != nullptr && *value != '\0') {
        directory = std::filesystem::path(value) / "mcpp";
    } else if (const auto* home = std::getenv("HOME");
        home != nullptr && *home != '\0') {
        directory = std::filesystem::path(home) / ".config" / "mcpp";
    } else {
        return std::filesystem::path(".mcpp-config");
    }
    const auto present = [](const std::filesystem::path& candidate) {
        std::error_code error;
        const auto status = std::filesystem::symlink_status(candidate, error);
        if (error == std::errc::no_such_file_or_directory) return false;
        if (error) throw std::filesystem::filesystem_error("inspect configuration", candidate, error);
        return std::filesystem::exists(status);
    };
    const auto native = directory / "ini";
    const auto legacy = directory / "config";
    if (present(native)) return native;
    if (present(legacy)) return legacy;
    return native;
}

}  // namespace mcpp
