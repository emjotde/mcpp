#include "mcpp/app.hpp"

#include "mcpp/compare.hpp"
#include "mcpp/editor_clipboard.hpp"
#include "mcpp/local_provider.hpp"
#include "mcpp/location.hpp"
#include "mcpp/search.hpp"
#include "mcpp/syntax.hpp"
#include "mcpp/user_menu.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <fnmatch.h>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <span>
#include <stdexcept>
#include <sys/wait.h>
#include <thread>
#include <unordered_map>
#include <utility>
#include <unistd.h>

#include <ftxui/component/app.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/string.hpp>
#include <ftxui/screen/terminal.hpp>

namespace mcpp {
namespace {

struct EditorSearchState {
    std::string needle;
    EditorSearchOptions options;
    bool in_selection{false};
    std::optional<std::pair<EditorPosition, EditorPosition>> scope;
    std::uint64_t change_id{0};
};

struct EditorBuffer : EditorModel {
    EditorSearchState search;
    std::size_t first_row{0};
    std::size_t first_column{0};
};

enum class Mode {
    panels,
    viewer,
    viewer_message,
    viewer_close_dialog,
    viewer_wrap_dialog,
    editor,
    editor_menu,
    editor_help,
    editor_error,
    editor_search_dialog,
    editor_replace_dialog,
    editor_replace_confirm,
    editor_save_dialog,
    editor_close_dialog,
    editor_link_dialog,
    editor_overwrite_dialog,
    editor_options_dialog,
    editor_history,
    editor_buffer_list,
    prompt,
    help,
    user_menu,
    pulldown,
    search_results,
    bookmarks,
    information,
    operation_dialog,
    delete_dialog,
    operation_progress,
    conflict_dialog,
    operation_error_dialog,
    jobs,
    compare_dialog,
};

enum class ButtonInput {
    ignored,
    changed,
    activate,
    cancel,
};

enum class EditorSavePurpose {
    save,
    close,
    quit,
    save_as,
};

enum class OperationChoice {
    overwrite,
    skip,
    overwrite_all,
    skip_all,
    older,
    retry,
    ignore_all,
    cancel,
};

struct OperationPolicy {
    std::optional<CollisionDecision> collisions;
    bool older{false};
    bool ignore_errors{false};
};

struct OperationJob {
    std::uint64_t id{0};
    std::string label;
    std::string destination;
    std::string status;
};

enum class PromptAction {
    none,
    rename,
    mkdir,
    view_file,
    viewer_find,
    viewer_goto,
    viewer_path,
    quick_viewer_find,
    editor_open,
    editor_new,
    editor_save_as,
    editor_goto,
    hardlink,
    symlink,
    relative_symlink,
    select_pattern,
    unselect_pattern,
    quick_cd,
    chmod,
    chown,
    recursive_search,
    panel_filter,
    panelize_search,
};

std::string format_size(const std::uint64_t size) {
    static constexpr std::array<const char*, 5> suffixes{
        "B", "KiB", "MiB", "GiB", "TiB"
    };
    auto value = static_cast<double>(size);
    std::size_t suffix = 0;
    while (value >= 1024.0 && suffix + 1U < suffixes.size()) {
        value /= 1024.0;
        ++suffix;
    }
    std::ostringstream output;
    if (suffix == 0U) {
        output << size << ' ' << suffixes[suffix];
    } else {
        output << std::fixed << std::setprecision(1)
               << value << ' ' << suffixes[suffix];
    }
    return output.str();
}

std::string format_panel_size(const std::uint64_t size) {
    const auto decimal = std::to_string(size);
    if (decimal.size() <= 8U) {
        return decimal;
    }
    static constexpr std::array<char, 6> suffixes{
        'B', 'K', 'M', 'G', 'T', 'P'
    };
    auto value = static_cast<double>(size);
    std::size_t suffix = 0;
    while (value >= 1024.0 && suffix + 1U < suffixes.size()) {
        value /= 1024.0;
        ++suffix;
    }
    std::ostringstream output;
    output << std::fixed
           << std::setprecision(value < 10.0 ? 1 : 0)
           << value << suffixes[suffix];
    return output.str();
}

std::string display_text(const std::string_view value) {
    std::string result;
    result.reserve(value.size());
    constexpr char hex[] = "0123456789ABCDEF";
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (character == '\n') {
            result += "\\n";
        } else if (character == '\r') {
            result += "\\r";
        } else if (character == '\t') {
            result += "\\t";
        } else if (byte < 0x20U || byte == 0x7FU) {
            result += "\\x";
            result += hex[(byte >> 4U) & 0x0FU];
            result += hex[byte & 0x0FU];
        } else {
            result += character;
        }
    }
    return result;
}

std::string lowercase_text(std::string value) {
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](const unsigned char character) {
            return static_cast<char>(std::tolower(character));
        }
    );
    return value;
}

std::size_t previous_utf8(const std::string_view value, std::size_t offset) {
    offset = std::min(offset, value.size());
    if (offset == 0U) {
        return 0U;
    }
    auto begin = offset - 1U;
    while (begin > 0U &&
           (
               static_cast<unsigned char>(value[begin]) &
               0xC0U
           ) == 0x80U) {
        --begin;
    }
    return begin;
}

void erase_last_utf8(std::string& value) {
    value.erase(previous_utf8(value, value.size()));
}

std::size_t next_utf8(
    const std::string_view value,
    std::size_t offset
) {
    if (offset >= value.size()) {
        return value.size();
    }
    ++offset;
    while (offset < value.size() &&
           (
               static_cast<unsigned char>(value[offset]) &
               0xC0U
           ) == 0x80U) {
        ++offset;
    }
    return offset;
}

std::uint32_t utf8_codepoint(
    const std::string_view value,
    const std::size_t offset
) {
    if (offset >= value.size()) {
        return 0U;
    }
    const auto first = static_cast<unsigned char>(value[offset]);
    if (first < 0x80U) {
        return first;
    }
    std::uint32_t result = 0;
    std::size_t length = 0;
    if ((first & 0xE0U) == 0xC0U) {
        result = first & 0x1FU;
        length = 2;
    } else if ((first & 0xF0U) == 0xE0U) {
        result = first & 0x0FU;
        length = 3;
    } else if ((first & 0xF8U) == 0xF0U) {
        result = first & 0x07U;
        length = 4;
    } else {
        return first;
    }
    if (offset + length > value.size()) {
        return first;
    }
    for (std::size_t index = 1; index < length; ++index) {
        const auto next =
            static_cast<unsigned char>(value[offset + index]);
        if ((next & 0xC0U) != 0x80U) {
            return first;
        }
        result = (result << 6U) | (next & 0x3FU);
    }
    return result;
}

std::size_t editor_offset_for_width(
    const std::string_view value,
    const std::size_t target_width,
    const std::size_t tab_width
) {
    std::size_t offset = 0;
    std::size_t width = 0;
    while (offset < value.size()) {
        if (value[offset] == '\t') {
            const auto glyph_width =
                tab_width - width % tab_width;
            if (width + glyph_width > target_width) {
                break;
            }
            width += glyph_width;
            ++offset;
            continue;
        }
        const auto next = next_utf8(value, offset);
        const auto glyph_width = static_cast<std::size_t>(
            std::max(
                0,
                ftxui::string_width(value.substr(offset, next - offset))
            )
        );
        if (width + glyph_width > target_width) {
            break;
        }
        width += glyph_width;
        offset = next;
    }
    return offset;
}

std::size_t editor_display_width(
    const std::string_view value,
    const std::size_t byte_offset,
    const std::size_t tab_width
) {
    std::size_t offset = 0;
    std::size_t width = 0;
    const auto end = std::min(byte_offset, value.size());
    while (offset < end) {
        if (value[offset] == '\t') {
            width += tab_width - width % tab_width;
            ++offset;
            continue;
        }
        const auto next = std::min(next_utf8(value, offset), end);
        width += static_cast<std::size_t>(
            std::max(
                0,
                ftxui::string_width(
                    value.substr(offset, next - offset)
                )
            )
        );
        offset = next;
    }
    return width;
}

std::string editor_display_text(
    const std::string_view value,
    std::size_t display_column,
    const std::size_t tab_width,
    const bool show_tabs,
    const bool show_spaces
) {
    std::string result;
    for (std::size_t offset = 0; offset < value.size();) {
        if (value[offset] == '\t') {
            const auto spaces =
                tab_width - display_column % tab_width;
            if (show_tabs) {
                if (spaces == 1U) {
                    result += '>';
                } else {
                    result += '<';
                    result.append(spaces - 2U, '-');
                    result += '>';
                }
            } else {
                result.append(spaces, ' ');
            }
            display_column += spaces;
            ++offset;
            continue;
        }
        if (value[offset] == ' ' && show_spaces) {
            result += '.';
            ++display_column;
            ++offset;
            continue;
        }
        const auto next = next_utf8(value, offset);
        const auto glyph = value.substr(offset, next - offset);
        result += glyph;
        display_column += static_cast<std::size_t>(
            std::max(0, ftxui::string_width(glyph))
        );
        offset = next;
    }
    return result;
}

std::string format_time(
    const std::chrono::system_clock::time_point point
) {
    const auto value = std::chrono::system_clock::to_time_t(point);
    std::tm local{};
    if (localtime_r(&value, &local) == nullptr) {
        return "---- -- --:--";
    }
    std::array<char, 32> buffer{};
    if (std::strftime(
            buffer.data(),
            buffer.size(),
            "%Y-%m-%d %H:%M",
            &local
        ) == 0U) {
        return "---- -- --:--";
    }
    return buffer.data();
}

std::string format_panel_time(
    const std::chrono::system_clock::time_point point
) {
    const auto value = std::chrono::system_clock::to_time_t(point);
    std::tm local{};
    if (localtime_r(&value, &local) == nullptr) {
        return "--- -- --:--";
    }
    std::array<char, 24> buffer{};
    if (std::strftime(
            buffer.data(),
            buffer.size(),
            "%b %d %H:%M",
            &local
        ) == 0U) {
        return "--- -- --:--";
    }
    return buffer.data();
}

std::string truncate_text(
    const std::string& value,
    const std::size_t width
) {
    if (static_cast<std::size_t>(
            std::max(0, ftxui::string_width(value))
        ) <= width) {
        return value;
    }
    const auto budget = width > 3U ? width - 3U : width;
    std::size_t end = 0;
    std::size_t columns = 0;
    while (end < value.size()) {
        const auto next = next_utf8(value, end);
        const auto glyph_width = static_cast<std::size_t>(
            std::max(0, ftxui::string_width(
                std::string_view(value).substr(end, next - end)
            ))
        );
        if (columns + glyph_width > budget) {
            break;
        }
        columns += glyph_width;
        end = next;
    }
    return value.substr(0, end) + (width > 3U ? "..." : "");
}

std::string command_text(
    const std::string& value,
    const std::size_t width
) {
    if (static_cast<std::size_t>(
            std::max(0, ftxui::string_width(value))
        ) <= width) {
        return value;
    }
    if (width == 0U) {
        return {};
    }
    std::size_t offset = 0;
    while (offset < value.size() &&
           static_cast<std::size_t>(
               std::max(
                   0,
                   ftxui::string_width(value.substr(offset))
               )
           ) >= width) {
        offset = next_utf8(value, offset);
    }
    return "<" + value.substr(offset);
}

std::string middle_truncate(
    const std::string& value,
    const std::size_t width
) {
    if (static_cast<std::size_t>(
            std::max(0, ftxui::string_width(value))
        ) <= width) {
        return value;
    }
    if (width == 0U) {
        return {};
    }
    const auto glyphs = ftxui::Utf8ToGlyphs(value);
    const auto left_budget = (width - 1U) / 2U;
    std::size_t left_bytes = 0;
    std::size_t left_width = 0;
    for (const auto& glyph : glyphs) {
        const auto glyph_width = static_cast<std::size_t>(
            std::max(0, ftxui::string_width(glyph))
        );
        if (left_width + glyph_width > left_budget) {
            break;
        }
        left_bytes += glyph.size();
        left_width += glyph_width;
    }
    if (const auto separator = value.rfind('/', left_bytes);
        separator != std::string::npos && separator > 0U) {
        if (separator < left_bytes) {
            left_bytes = separator + 1U;
            left_width = static_cast<std::size_t>(
                std::max(
                    0,
                    ftxui::string_width(value.substr(0, left_bytes))
                )
            );
        }
    }
    const auto right_budget = width - left_width - 1U;
    std::size_t right_bytes = 0;
    std::size_t right_width = 0;
    for (auto glyph = glyphs.rbegin(); glyph != glyphs.rend(); ++glyph) {
        const auto glyph_width = static_cast<std::size_t>(
            std::max(0, ftxui::string_width(*glyph))
        );
        if (right_width + glyph_width > right_budget) {
            break;
        }
        right_bytes += glyph->size();
        right_width += glyph_width;
    }
    return value.substr(0, left_bytes) + "~" +
           value.substr(value.size() - right_bytes);
}

std::string path_label(const std::string& path, const std::size_t width) {
    if (static_cast<std::size_t>(
            std::max(0, ftxui::string_width(path))
        ) <= width) {
        return path;
    }
    const auto separator = path.find_last_of('/');
    const auto name = separator == std::string::npos
                          ? path
                          : path.substr(separator + 1U);
    const auto name_width = static_cast<std::size_t>(
        std::max(0, ftxui::string_width(name))
    );
    if (!name.empty() && name_width + 2U <= width) {
        const auto prefix_width = width - name_width - 2U;
        auto prefix = path.substr(0, separator);
        while (!prefix.empty() && static_cast<std::size_t>(
                   std::max(0, ftxui::string_width(prefix))
               ) > prefix_width) {
            prefix.erase(previous_utf8(prefix, prefix.size()));
        }
        return prefix + "~/" + name;
    }
    return command_text(path, width);
}

bool modal_cancel(const ftxui::Event& event) {
    return event == ftxui::Event::Escape ||
           event == ftxui::Event::F10 ||
           event == ftxui::Event::CtrlG;
}

std::optional<std::vector<std::string>> plain_command_words(
    const std::string_view command
) {
    std::vector<std::string> words;
    std::string word;
    char quote = '\0';
    bool started = false;
    for (std::size_t index = 0; index < command.size(); ++index) {
        const auto character = command[index];
        if (character == '\\' && quote != '\'') {
            if (++index == command.size()) {
                return std::nullopt;
            }
            word += command[index];
            started = true;
        } else if ((character == '\'' || character == '"') &&
                   (quote == '\0' || quote == character)) {
            quote = quote == '\0' ? character : '\0';
            started = true;
        } else if (quote == '\0' &&
                   std::isspace(static_cast<unsigned char>(character)) != 0) {
            if (started) {
                words.push_back(std::move(word));
                word.clear();
                started = false;
            }
        } else {
            if ((quote != '\'' &&
                 (character == '$' || character == '`')) ||
                (quote == '\0' &&
                 std::string_view(";&|<>()*?[]{}").find(character) !=
                     std::string_view::npos)) {
                return std::nullopt;
            }
            word += character;
            started = true;
        }
    }
    if (quote != '\0') {
        return std::nullopt;
    }
    if (started) {
        words.push_back(std::move(word));
    }
    return words;
}

std::string decode_hex_query(const std::string_view query) {
    std::string bytes;
    std::optional<unsigned int> high;
    for (const auto character : query) {
        const auto byte = static_cast<unsigned char>(character);
        if (std::isspace(byte) != 0) {
            continue;
        }
        const auto lowered = static_cast<char>(std::tolower(byte));
        if (!((lowered >= '0' && lowered <= '9') ||
              (lowered >= 'a' && lowered <= 'f'))) {
            throw std::invalid_argument("enter hexadecimal byte pairs, for example 4d 43");
        }
        const auto digit = static_cast<unsigned int>(
            lowered <= '9' ? lowered - '0' : lowered - 'a' + 10
        );
        if (high) {
            bytes += static_cast<char>((*high << 4U) | digit);
            high.reset();
        } else {
            high = digit;
        }
    }
    if (high || bytes.empty()) {
        throw std::invalid_argument("enter complete hexadecimal byte pairs");
    }
    return bytes;
}

ButtonInput handle_button_input(
    const ftxui::Event& event,
    std::size_t& choice,
    const std::span<const ftxui::Box> boxes,
    const std::string_view shortcuts = {}
) {
    using ftxui::Event;
    if (modal_cancel(event)) {
        return ButtonInput::cancel;
    }
    if (boxes.empty()) {
        return ButtonInput::ignored;
    }
    choice = std::min(choice, boxes.size() - 1U);
    if (event == Event::ArrowLeft || event == Event::TabReverse) {
        choice = (choice + boxes.size() - 1U) % boxes.size();
        return ButtonInput::changed;
    }
    if (event == Event::ArrowRight || event == Event::Tab) {
        choice = (choice + 1U) % boxes.size();
        return ButtonInput::changed;
    }
    if (event.is_mouse()) {
        auto copy = event;
        const auto& mouse = copy.mouse();
        if (mouse.button == ftxui::Mouse::Left &&
            mouse.motion == ftxui::Mouse::Pressed) {
            for (std::size_t index = 0; index < boxes.size(); ++index) {
                if (boxes[index].Contain(mouse.x, mouse.y)) {
                    choice = index;
                    return ButtonInput::activate;
                }
            }
        }
        return ButtonInput::changed;
    }
    if (event.is_character() && event.character().size() == 1U) {
        const auto key = static_cast<char>(std::tolower(
            static_cast<unsigned char>(event.character().front())
        ));
        const auto index = shortcuts.find(key);
        if (index < boxes.size()) {
            choice = index;
            return ButtonInput::activate;
        }
    }
    if (event == Event::Return || event == Event::Character(' ')) {
        return ButtonInput::activate;
    }
    return ButtonInput::ignored;
}

ftxui::Element dialog_button(
    const std::string& label,
    const bool focused,
    ftxui::Box& box
) {
    auto element = ftxui::text("[ " + label + " ]");
    if (focused) {
        element = element | ftxui::inverted | ftxui::focus;
    }
    return element | ftxui::reflect(box);
}

std::string type_prefix(const Entry& entry) {
    switch (entry.kind) {
        case EntryKind::directory:
            return "/";
        case EntryKind::symlink:
            return "@";
        case EntryKind::other:
            return "?";
        case EntryKind::file:
            return " ";
    }
    return " ";
}

bool can_open_file(const Entry& entry) {
    return entry.kind == EntryKind::file ||
           (
               entry.kind == EntryKind::symlink &&
               !entry.target_is_directory
           );
}

ftxui::Element gray_dialog(ftxui::Element element) {
    using namespace ftxui;
    return std::move(element) |
           color(Color::Grey11) |
           bgcolor(Color::Grey82);
}

ftxui::Element mnemonic_label(
    const std::string& label,
    const std::size_t index
) {
    using namespace ftxui;
    if (index >= label.size()) {
        return text(label);
    }
    return hbox({
        text(label.substr(0, index)),
        text(label.substr(index, 1U)) | underlined | bold,
        text(label.substr(index + 1U)),
    });
}

ftxui::Element gray_menu(ftxui::Element element) {
    using namespace ftxui;
    return std::move(element) |
           color(Color::Grey11) |
           bgcolor(Color::Grey78);
}

ftxui::Element gray_selected(
    ftxui::Element element,
    const bool marked,
    const bool active
) {
    using namespace ftxui;
    const auto foreground =
        active ? Color::Grey0 : Color::Grey93;
    const auto background =
        active ? Color::Grey85 : Color::Grey27;
    auto result = std::move(element) |
                  color(foreground) |
                  bgcolor(background);
    return marked ? result | bold : result;
}

ftxui::Element gray_entry(
    ftxui::Element element,
    const Entry& entry
) {
    using namespace ftxui;
    if (entry.kind == EntryKind::directory) {
        return std::move(element) | color(Color::Grey100);
    }
    if (entry.kind == EntryKind::symlink) {
        return std::move(element) | color(Color::Grey66);
    }
    if (entry.kind == EntryKind::other) {
        return std::move(element) | color(Color::Grey46);
    }
    const auto executable =
        (
            entry.permissions &
            (
                std::filesystem::perms::owner_exec |
                std::filesystem::perms::group_exec |
                std::filesystem::perms::others_exec
            )
        ) != std::filesystem::perms::none;
    return executable
               ? std::move(element) | color(Color::Grey100) | underlined
               : std::move(element) | color(Color::Grey85);
}

std::string permission_text(const Entry& entry) {
    std::string result =
        entry.kind == EntryKind::directory
            ? "d"
            : entry.kind == EntryKind::symlink ? "l" : "-";
    if (entry.permissions == std::filesystem::perms::unknown) {
        return result + "?????????";
    }
    const std::array<std::pair<
        std::filesystem::perms,
        char
    >, 9> bits{{
        {std::filesystem::perms::owner_read, 'r'},
        {std::filesystem::perms::owner_write, 'w'},
        {std::filesystem::perms::owner_exec, 'x'},
        {std::filesystem::perms::group_read, 'r'},
        {std::filesystem::perms::group_write, 'w'},
        {std::filesystem::perms::group_exec, 'x'},
        {std::filesystem::perms::others_read, 'r'},
        {std::filesystem::perms::others_write, 'w'},
        {std::filesystem::perms::others_exec, 'x'},
    }};
    for (const auto& [permission, character] : bits) {
        result +=
            (entry.permissions & permission) !=
                    std::filesystem::perms::none
                ? character
                : '-';
    }
    const auto special = [&](const std::filesystem::perms permission) {
        return (entry.permissions & permission) !=
               std::filesystem::perms::none;
    };
    if (special(std::filesystem::perms::set_uid)) {
        result[3] = result[3] == 'x' ? 's' : 'S';
    }
    if (special(std::filesystem::perms::set_gid)) {
        result[6] = result[6] == 'x' ? 's' : 'S';
    }
    if (special(std::filesystem::perms::sticky_bit)) {
        result[9] = result[9] == 'x' ? 't' : 'T';
    }
    return result;
}

struct MenuRow {
    std::string label;
    int action{-1};
};

struct TreeNode {
    std::string path;
    std::size_t depth{0};
    bool expanded{false};
    bool loaded{false};
    bool inaccessible{false};
    bool has_children{false};
    bool loading{false};
};

enum class ViewerGotoMode { line, percent, decimal, hexadecimal };

struct ViewerRow {
    ftxui::Box box;
    std::uint64_t offset{0};
    std::size_t hex_column{0};
    std::size_t first_column{0};
    std::size_t bytes{0};
};

struct TextInputState {
    std::size_t cursor{0};
    std::optional<std::size_t> anchor;
    std::size_t rendered_cursor{0};
    ftxui::Box cursor_box;
    ftxui::Box box;
    bool dragging{false};
    std::vector<std::string> history;
    std::size_t history_index{0};
    std::string draft;
    std::vector<std::string> completions;
    std::size_t completion_index{0};
    std::string completion_prefix;
    std::string completion_suffix;
    std::string completed_value;
    std::string message;

    void reset(const std::string_view value, const bool select_initial = false) {
        cursor = value.size();
        anchor = select_initial && !value.empty()
                     ? std::optional<std::size_t>(0U)
                     : std::nullopt;
        dragging = false;
        history_index = history.size();
        draft.clear();
        completions.clear();
        message.clear();
    }
};

}  // namespace

struct App::Impl {
    Config config;
    ProviderPtr local;
    Location left_location;
    Location right_location;
    Pane left;
    Pane right;
    int active_index{0};
    std::array<int, 2> pane_ids{0, 1};
    Mode mode{Mode::panels};
    Mode mode_before_prompt{Mode::panels};
    PromptAction prompt_action{PromptAction::none};
    std::string prompt_title;
    std::string prompt_value;
    std::string prompt_error;
    std::size_t prompt_focus{0};
    std::array<ftxui::Box, 4> prompt_boxes{};
    std::vector<Entry> prompt_targets;
    ProviderPtr prompt_provider;
    EditorLineEnding editor_save_line_ending{EditorLineEnding::unchanged};
    std::map<PromptAction, TextInputState> prompt_inputs;
    TextInputState destination_input;
    TextInputState mask_input;
    TextInputState command_input;
    std::string input_clipboard;
    std::array<ftxui::Box, 7> operation_dialog_boxes{};
    ftxui::Box operation_dialog_viewport;
    std::string status{"Ready"};
    std::optional<std::string> pending_operation_notice;
    OperationEngine operations;
    Cancellation operation_cancellation;
    TransferProgress progress;
    ViewerModel viewer;
    std::string viewer_search_needle;
    std::string viewer_search_pattern;
    bool viewer_search_hex{false};
    std::string viewer_message_title{"Viewer"};
    std::string viewer_message;
    ftxui::Box message_button_box;
    Mode viewer_return_mode{Mode::panels};
    bool exit_after_viewer{false};
    std::size_t viewer_choice{0};
    std::array<ftxui::Box, 3> viewer_choice_boxes{};
    std::array<ftxui::Box, 2> viewer_wrap_boxes{};
    bool viewer_search_quick{false};
    std::size_t viewer_hex_first_column{0};
    ViewerGotoMode viewer_goto_mode{ViewerGotoMode::line};
    std::vector<ViewerRow> viewer_rows;
    std::string viewer_rendered_path;
    EditorBuffer editor;
    std::vector<EditorBuffer> editor_buffers;
    std::vector<std::string> editor_history;
    std::size_t editor_list_cursor{0};
    std::vector<ftxui::Box> editor_list_boxes;
    ftxui::Box editor_list_box;
    Mode editor_return_mode{Mode::panels};
    std::size_t editor_session_base{0};
    bool exit_after_editor{false};
    SyntaxHighlighter syntax_highlighter;
    bool editor_syntax{true};
    bool editor_line_numbers{true};
    std::size_t editor_indent_width{4};
    bool editor_auto_indent{true};
    bool editor_show_whitespace{false};
    std::uint64_t syntax_change_id{
        std::numeric_limits<std::uint64_t>::max()
    };
    std::string syntax_path;
    std::vector<std::vector<SyntaxSpan>> syntax_spans;
    std::vector<ftxui::Box> editor_line_boxes;
    std::size_t editor_rendered_first_row{0};
    std::vector<std::size_t> editor_line_first_columns;
    bool editor_mouse_dragging{false};
    bool editor_mouse_marking{false};
    bool editor_mouse_column{false};
    std::string editor_clipboard;
    bool editor_clipboard_column{false};
    EditorSearchOptions editor_dialog_options;
    bool editor_dialog_in_selection{false};
    std::optional<std::pair<
        EditorPosition,
        EditorPosition
    >> editor_dialog_scope;
    std::string editor_dialog_query;
    std::string editor_dialog_replacement;
    TextInputState editor_query_input;
    TextInputState editor_replacement_input;
    std::array<ftxui::Box, 10> editor_search_boxes{};
    ftxui::Box editor_search_viewport;
    std::size_t editor_dialog_focus{0};
    std::string editor_replace_needle;
    std::string editor_replace_text;
    std::size_t editor_replace_choice{0};
    std::size_t editor_replacements{0};
    std::size_t editor_menu_category{0};
    std::size_t editor_menu_item{0};
    bool editor_menu_dropped{false};
    std::array<ftxui::Box, 7> editor_menu_category_boxes{};
    std::vector<ftxui::Box> editor_menu_item_boxes;
    std::size_t editor_save_choice{0};
    std::array<ftxui::Box, 2> editor_save_boxes{};
    std::size_t editor_close_choice{0};
    std::array<ftxui::Box, 3> editor_close_boxes{};
    std::array<ftxui::Box, 5> editor_replace_boxes{};
    std::array<ftxui::Box, 3> editor_link_boxes{};
    std::array<ftxui::Box, 2> editor_overwrite_boxes{};
    std::size_t editor_link_choice{0};
    std::size_t editor_overwrite_choice{1};
    EditorSavePurpose editor_save_purpose{EditorSavePurpose::save};
    EditorBuffer* editor_save_buffer{nullptr};
    std::string editor_save_path;
    std::string editor_overwrite_revision;
    bool editor_save_overwrite{false};
    bool editor_quit_requested{false};
    std::size_t editor_quit_index{0};
    std::size_t editor_options_focus{0};
    std::string editor_options_tab_width;
    std::string editor_options_indent_width;
    TextInputState editor_tab_input;
    TextInputState editor_indent_input;
    std::array<ftxui::Box, 6> editor_options_boxes{};
    bool editor_options_auto_indent{true};
    bool editor_options_show_whitespace{false};
    std::string editor_options_error;
    bool editor_shift_marking{false};
    bool editor_column_marking{false};
    std::string editor_message_title{"Editor error"};
    std::string editor_error_message;
    ftxui::App* terminal{nullptr};
    std::jthread operation_thread;
    std::atomic_bool operation_running{false};
    std::chrono::steady_clock::time_point operation_started{};
    std::mutex operation_mutex;
    std::string command_line;
    bool command_mode{false};
    bool panel_ctrl_x{false};
    bool panel_search_mode{false};
    std::string panel_search;
    std::vector<std::string> command_history;
    std::array<std::string, 2> command_previous_directories;
    std::vector<UserMenuEntry> user_menu;
    ftxui::Box left_box;
    ftxui::Box right_box;
    std::vector<ftxui::Box> left_entry_boxes;
    std::vector<ftxui::Box> right_entry_boxes;
    std::vector<Entry> search_results;
    ProviderPtr search_provider;
    std::string search_warning;
    std::size_t search_cursor{0};
    std::vector<ftxui::Box> search_result_boxes;
    ftxui::Box search_list_box;
    std::size_t bookmark_cursor{0};
    std::vector<ftxui::Box> bookmark_boxes;
    ftxui::Box bookmark_list_box;
    std::array<ftxui::Box, 10> function_key_boxes{};
    std::array<bool, 10> function_key_enabled{};
    bool detailed_view{true};
    std::optional<int> quick_view_panel;
    bool quick_view_focused{false};
    std::optional<int> information_panel;
    bool information_panel_focused{false};
    std::optional<int> tree_panel;
    bool tree_panel_focused{false};
    std::vector<TreeNode> tree_nodes;
    std::size_t tree_cursor{0};
    bool tree_search_mode{false};
    std::string tree_search;
    bool tree_dynamic{false};
    ViewerModel quick_viewer;
    std::string quick_view_search_needle;
    std::string quick_view_search_pattern;
    bool quick_view_search_hex{false};
    std::string quick_view_path;
    ftxui::Box quick_view_box;
    ftxui::Box information_panel_box;
    ftxui::Box tree_panel_box;
    std::vector<ftxui::Box> tree_row_boxes;
    std::vector<std::string> tree_row_paths;
    std::chrono::steady_clock::time_point last_click{};
    int last_click_pane{-1};
    std::size_t last_click_index{
        std::numeric_limits<std::size_t>::max()
    };
    std::string last_click_path;
    std::size_t pulldown_category{0};
    std::size_t pulldown_item{0};
    bool pulldown_dropped{false};
    std::array<ftxui::Box, 5> top_menu_boxes{};
    std::vector<ftxui::Box> pulldown_item_boxes;
    ftxui::Box pulldown_viewport;
    bool dialog_move{false};
    bool dialog_exact{false};
    std::string dialog_destination;
    std::string dialog_source_mask{"*"};
    bool dialog_preserve_attributes{true};
    bool dialog_preserve_symlinks{true};
    bool dialog_background{false};
    std::size_t dialog_focus{1};
    std::array<ftxui::Box, 2> delete_button_boxes{};
    std::size_t compare_choice{0};
    std::array<ftxui::Box, 4> compare_boxes{};
    std::atomic_bool operation_paused{false};
    std::atomic_bool operation_skip{false};
    std::mutex operation_pause_mutex;
    std::condition_variable operation_pause_condition;
    std::mutex operation_prompt_mutex;
    std::condition_variable operation_prompt_condition;
    std::optional<OperationChoice> operation_answer;
    std::atomic_bool operation_prompt_pending{false};
    std::optional<Entry> operation_prompt_source;
    std::optional<Entry> operation_prompt_destination;
    std::string operation_prompt_error;
    Mode operation_return_mode{Mode::panels};
    std::size_t operation_return_focus{0};
    std::array<ftxui::Box, 6> operation_choice_boxes{};
    std::vector<OperationJob> jobs;
    std::optional<std::size_t> active_job;
    std::size_t job_cursor{0};
    std::uint64_t next_job_id{1};
    std::array<ftxui::Box, 4> job_button_boxes{};
    std::vector<ftxui::Box> job_row_boxes;
    std::jthread traversal_thread;
    std::atomic_bool traversal_running{false};
    Cancellation traversal_cancellation;
    std::jthread tree_thread;
    std::atomic_bool tree_expansion_running{false};
    Cancellation tree_expansion_cancellation;
    std::uint64_t tree_generation{0};
    std::optional<std::string> tree_refresh_pending;
    bool tree_refresh_notify_pending{false};
    bool tree_surroundings_pending{false};
    std::string operation_label{"File operation"};
    int rendered_width{0};
    int rendered_height{0};
    std::optional<Entry> tree_operation_source;
    ProviderPtr tree_operation_source_provider;
    ProviderPtr tree_operation_destination_provider;
    int tree_operation_source_index{0};
    bool exiting{false};

    Impl(
        std::string left_path,
        std::string right_path,
        Config configured
    )
        : config(std::move(configured)),
          local(std::make_shared<LocalProvider>()),
          left_location(open_location(left_path, local)),
          right_location(open_location(right_path, local)),
          left(left_location.provider, left_location.path),
          right(right_location.provider, right_location.path) {
        user_menu = load_user_menu(default_user_menu_path());
        left.set_show_hidden(config.show_hidden);
        right.set_show_hidden(config.show_hidden);
        left.set_directories_first(config.directories_first);
        right.set_directories_first(config.directories_first);
        left.set_sort(config.sort, config.sort_descending);
        right.set_sort(config.sort, config.sort_descending);
        editor_syntax = config.editor_syntax;
        editor_line_numbers = config.editor_line_numbers;
        editor.set_tab_width(config.editor_tab_width);
        editor_indent_width = config.editor_indent_width;
        editor_auto_indent = config.editor_auto_indent;
        editor_show_whitespace = config.editor_show_whitespace;
        editor_history = config.editor_history;
    }

    Pane& active() {
        return active_index == 0 ? left : right;
    }

    Pane& passive() {
        return active_index == 0 ? right : left;
    }

    static std::size_t panel_visible_rows() {
        return static_cast<std::size_t>(
            std::max(3, ftxui::Terminal::Size().dimy - 9)
        );
    }

    static int panel_width(const int index) {
        const auto width = std::max(2, ftxui::Terminal::Size().dimx);
        const auto left_width = width / 2;
        return index == 0
                   ? left_width
                   : width - left_width;
    }

    std::pair<std::size_t, std::size_t>
    quick_view_dimensions() const {
        const auto width = quick_view_panel
                               ? panel_width(*quick_view_panel)
                               : panel_width(1 - active_index);
        return {
            static_cast<std::size_t>(std::max(1, width - 2)),
            panel_visible_rows() + 2U,
        };
    }

    static std::pair<std::size_t, std::size_t>
    viewer_dimensions() {
        const auto size = ftxui::Terminal::Size();
        return {
            static_cast<std::size_t>(std::max(1, size.dimx)),
            static_cast<std::size_t>(std::max(1, size.dimy - 2)),
        };
    }

    static std::size_t editor_visible_rows() {
        return static_cast<std::size_t>(
            std::max(1, ftxui::Terminal::Size().dimy - 2)
        );
    }

    static std::size_t editor_page_rows() {
        return std::max<std::size_t>(
            1U,
            editor_visible_rows() - 1U
        );
    }

    ftxui::Element render_function_key(
        const std::size_t index,
        const std::string& label,
        const bool enabled = true
    ) {
        using namespace ftxui;
        const auto available = std::max(10, Terminal::Size().dimx - 1);
        const auto key_width =
            available / 10 +
            (static_cast<int>(index) < available % 10 ? 1 : 0);
        const auto number = std::to_string(index + 1U);
        const auto label_width = static_cast<std::size_t>(std::max(
            0, key_width - static_cast<int>(number.size())
        ));
        function_key_enabled[index] = enabled && !label.empty();
        auto description = text(truncate_text(label, label_width)) |
                           color(enabled ? Color::Grey85 : Color::Grey50) |
                           bgcolor(Color::Grey11);
        if (!enabled) {
            description = description | dim;
        }
        return hbox({
                   text(number) |
                       color(enabled ? Color::Grey0 : Color::Grey50) |
                       bgcolor(Color::Grey85),
                   std::move(description) | flex,
               }) |
               size(WIDTH, EQUAL, key_width) |
               reflect(function_key_boxes[index]);
    }

    ButtonInput handle_list_input(
        const ftxui::Event& event,
        std::size_t& cursor,
        const std::size_t count,
        const std::vector<ftxui::Box>& boxes,
        const ftxui::Box& viewport
    ) {
        using ftxui::Event;
        if (!event.is_mouse()) {
            last_click_pane = -1;
        }
        if (modal_cancel(event)) {
            return ButtonInput::cancel;
        }
        if (count == 0U) {
            return ButtonInput::ignored;
        }
        cursor = std::min(cursor, count - 1U);
        const auto move = [&](const int amount) {
            cursor = static_cast<std::size_t>(std::clamp(
                static_cast<long long>(cursor) + amount,
                0LL, static_cast<long long>(count - 1U)
            ));
        };
        if (event.is_mouse()) {
            auto copy = event;
            const auto& mouse = copy.mouse();
            if (!viewport.Contain(mouse.x, mouse.y)) {
                return ButtonInput::ignored;
            }
            if (mouse.button == ftxui::Mouse::WheelUp ||
                mouse.button == ftxui::Mouse::WheelDown) {
                move(mouse.button == ftxui::Mouse::WheelUp ? -3 : 3);
                return ButtonInput::changed;
            }
            if (mouse.button == ftxui::Mouse::Left &&
                mouse.motion == ftxui::Mouse::Pressed) {
                for (std::size_t index = 0; index < boxes.size(); ++index) {
                    if (!boxes[index].Contain(mouse.x, mouse.y)) {
                        continue;
                    }
                    cursor = index;
                    const auto now = std::chrono::steady_clock::now();
                    const auto context = 100 + static_cast<int>(mode);
                    const bool activate =
                        last_click_pane == context &&
                        last_click_index == index &&
                        now - last_click < std::chrono::milliseconds(400);
                    last_click_pane = context;
                    last_click_index = index;
                    last_click = now;
                    if (activate) {
                        last_click_pane = -1;
                    }
                    return activate ? ButtonInput::activate : ButtonInput::changed;
                }
            }
            return ButtonInput::changed;
        }
        const auto page = std::max(1, viewport.y_max - viewport.y_min);
        if (event == Event::ArrowUp) {
            move(-1);
        } else if (event == Event::ArrowDown) {
            move(1);
        } else if (event == Event::PageUp) {
            move(-page);
        } else if (event == Event::PageDown) {
            move(page);
        } else if (event == Event::Home) {
            cursor = 0;
        } else if (event == Event::End) {
            cursor = count - 1U;
        } else if (event == Event::Return) {
            return ButtonInput::activate;
        } else {
            return ButtonInput::ignored;
        }
        return ButtonInput::changed;
    }

    void update_editor_view() {
        if (!editor.open()) {
            return;
        }
        const auto visible = editor_visible_rows();
        const auto maximum =
            editor.lines().size() > visible ? editor.lines().size() - visible : 0U;
        editor.first_row = std::min(editor.first_row, maximum);
        if (editor.row() < editor.first_row) {
            editor.first_row = editor.row();
        } else if (editor.row() - editor.first_row >= visible) {
            editor.first_row = editor.row() - visible + 1U;
        }
        const auto width = static_cast<std::size_t>(std::max(
            1, ftxui::Terminal::Size().dimx - (editor_line_numbers ? 6 : 0)
        ));
        const auto column = editor_display_width(
            editor.lines()[editor.row()], editor.column(), editor.tab_width()
        );
        if (column < editor.first_column) {
            editor.first_column = column;
        } else if (column - editor.first_column >= width) {
            editor.first_column = column - width + 1U;
        }
    }

    void scroll_editor(const int rows) {
        const auto visible = editor_visible_rows();
        const auto maximum =
            editor.lines().size() > visible ? editor.lines().size() - visible : 0U;
        if (rows < 0) {
            editor.first_row -= std::min(
                editor.first_row, static_cast<std::size_t>(-rows)
            );
        } else {
            editor.first_row = std::min(
                maximum, editor.first_row + static_cast<std::size_t>(rows)
            );
        }
        editor.page(rows);
    }

    void center_editor() {
        const auto half = editor_visible_rows() / 2U;
        editor.first_row = editor.row() > half ? editor.row() - half : 0U;
    }

    void refresh(const bool notify_tree = false) {
        left.refresh();
        right.refresh();
        quick_viewer.close();
        quick_view_path.clear();
        if (tree_panel) {
            refresh_tree(notify_tree);
        }
    }

    void close_quick_view(const bool restore_focus = true) {
        if (restore_focus &&
            quick_view_panel &&
            quick_view_focused) {
            active_index = *quick_view_panel;
        }
        quick_view_panel.reset();
        quick_view_focused = false;
        quick_viewer.close();
        quick_view_path.clear();
    }

    void show_quick_view(const int panel_index) {
        if (information_panel) {
            information_panel.reset();
            information_panel_focused = false;
        }
        tree_panel.reset();
        tree_panel_focused = false;
        tree_nodes.clear();
        quick_view_panel = panel_index;
        if (active_index == panel_index) {
            active_index = 1 - panel_index;
        }
        quick_view_focused = false;
        quick_viewer.close();
        quick_view_path.clear();
    }

    void toggle_quick_view() {
        if (quick_view_panel) {
            close_quick_view();
        } else {
            show_quick_view(1 - active_index);
        }
    }

    void close_information_panel(
        const bool restore_focus = true
    ) {
        if (restore_focus &&
            information_panel &&
            information_panel_focused) {
            active_index = *information_panel;
        }
        information_panel.reset();
        information_panel_focused = false;
    }

    void show_information_panel(const int panel_index) {
        if (quick_view_panel) {
            close_quick_view(false);
        }
        tree_panel.reset();
        tree_panel_focused = false;
        tree_nodes.clear();
        information_panel = panel_index;
        if (active_index == panel_index) {
            active_index = 1 - panel_index;
        }
        information_panel_focused = false;
    }

    void toggle_information_panel() {
        if (information_panel) {
            close_information_panel();
        } else {
            show_information_panel(1 - active_index);
        }
    }

    void close_tree_panel(const bool restore_focus = true) {
        invalidate_tree_loading();
        tree_refresh_pending.reset();
        tree_refresh_notify_pending = false;
        if (tree_expansion_running.load(
                std::memory_order_acquire
            )) {
            tree_expansion_cancellation.request();
        }
        if (restore_focus && tree_panel && tree_panel_focused) {
            active_index = *tree_panel;
            auto& pane =
                active_index == 0 ? left : right;
            pane.move_cursor(
                -static_cast<int>(pane.cursor())
            );
        }
        tree_panel.reset();
        tree_panel_focused = false;
        tree_nodes.clear();
        tree_row_boxes.clear();
        tree_cursor = 0;
        tree_search_mode = false;
        tree_search.clear();
    }

    void invalidate_tree_loading() {
        ++tree_generation;
        tree_surroundings_pending = false;
        if (tree_expansion_running.load(std::memory_order_acquire)) {
            tree_expansion_cancellation.request();
        }
        for (auto& node : tree_nodes) {
            node.loading = false;
        }
    }

    void restore_tree_selection(std::string path) {
        if (!tree_panel || tree_nodes.empty()) {
            tree_cursor = 0;
            return;
        }
        const auto provider =
            (*tree_panel == 0 ? left : right).provider();
        while (!path.empty()) {
            const auto match = std::find_if(
                tree_nodes.begin(),
                tree_nodes.end(),
                [&](const TreeNode& node) {
                    return node.path == path;
                }
            );
            if (match != tree_nodes.end()) {
                tree_cursor = static_cast<std::size_t>(
                    std::distance(tree_nodes.begin(), match)
                );
                return;
            }
            const auto parent = provider->parent(path);
            if (parent == path) {
                break;
            }
            path = parent;
        }
        tree_cursor = 0;
    }

    void refresh_tree(
        const bool notify = false,
        std::optional<std::string> requested_path = std::nullopt
    ) {
        if (!tree_panel || tree_nodes.empty()) {
            return;
        }
        const auto selected_path = tree_nodes[tree_cursor].path;
        auto path = requested_path.value_or(selected_path);
        if (tree_expansion_running.load(std::memory_order_acquire)) {
            invalidate_tree_loading();
            if (!tree_refresh_pending || notify ||
                !tree_refresh_notify_pending) {
                tree_refresh_pending = std::move(path);
                tree_refresh_notify_pending = notify;
            }
            return;
        }
        const auto original_path = path;
        const auto provider =
            (*tree_panel == 0 ? left : right).provider();
        while (!provider->exists(path)) {
            const auto parent = provider->parent(path);
            if (parent == path) {
                break;
            }
            path = parent;
        }
        const auto match = std::find_if(
            tree_nodes.begin(),
            tree_nodes.end(),
            [&](const TreeNode& node) { return node.path == path; }
        );
        if (match == tree_nodes.end()) {
            status = "Tree branch is no longer visible";
            return;
        }
        if (selected_path == original_path && path != original_path) {
            restore_tree_selection(path);
        }
        expand_tree_node(
            static_cast<std::size_t>(
                std::distance(tree_nodes.begin(), match)
            ),
            true,
            notify
        );
    }

    void load_tree_surroundings() {
        tree_surroundings_pending = false;
        if (!tree_panel || tree_cursor >= tree_nodes.size()) {
            return;
        }
        const auto& node = tree_nodes[tree_cursor];
        if (!node.expanded || node.loaded || node.loading) {
            return;
        }
        if (tree_expansion_running.load(std::memory_order_acquire)) {
            tree_surroundings_pending = true;
            return;
        }
        expand_tree_node(tree_cursor, true, false);
    }

    void finish_tree_loading() {
        tree_expansion_running.store(false, std::memory_order_release);
        if (tree_refresh_pending) {
            auto path = std::move(*tree_refresh_pending);
            const bool notify = tree_refresh_notify_pending;
            tree_refresh_pending.reset();
            tree_refresh_notify_pending = false;
            refresh_tree(notify, std::move(path));
        } else if (tree_surroundings_pending) {
            load_tree_surroundings();
        }
    }

    std::vector<TreeNode> tree_children(
        Pane& pane,
        const std::string& path,
        const std::size_t depth
    ) {
        Cancellation traversal;
        std::vector<TreeNode> children;
        for (const auto& entry : pane.provider()->list(
                 path,
                 {
                     .show_hidden = true,
                     .directories_first = true,
                 },
                 traversal
             )) {
            if (entry.kind == EntryKind::directory) {
                children.push_back({
                    .path = entry.path,
                    .depth = depth,
                });
            }
        }
        return children;
    }

    std::vector<TreeNode> build_tree_nodes(
        Pane& pane,
        std::size_t& selected
    ) {
        auto root = pane.path();
        for (std::size_t guard = 0; guard < 1024U; ++guard) {
            const auto parent = pane.provider()->parent(root);
            if (parent == root) {
                break;
            }
            root = parent;
        }
        std::vector<std::string> ancestry;
        for (auto path = pane.path(); path != root;) {
            ancestry.push_back(path);
            const auto parent = pane.provider()->parent(path);
            if (parent == path) {
                break;
            }
            path = parent;
        }
        std::reverse(ancestry.begin(), ancestry.end());
        // Visible ancestor branches have not had their other children listed.
        std::vector<TreeNode> nodes{{
            .path = root,
            .depth = 0,
            .expanded = !ancestry.empty(),
            .has_children = !ancestry.empty(),
        }};
        std::size_t depth = 1U;
        for (const auto& path : ancestry) {
            const bool current = path == pane.path();
            nodes.push_back({
                .path = path,
                .depth = depth++,
                .expanded = !current,
                .has_children = !current,
            });
        }
        selected = nodes.size() - 1U;
        return nodes;
    }

    void collapse_tree_node(const std::size_t index) {
        invalidate_tree_loading();
        tree_refresh_pending.reset();
        tree_refresh_notify_pending = false;
        const auto depth = tree_nodes[index].depth;
        auto end = index + 1U;
        while (end < tree_nodes.size() &&
               tree_nodes[end].depth > depth) {
            ++end;
        }
        tree_nodes.erase(
            tree_nodes.begin() +
                static_cast<std::ptrdiff_t>(index + 1U),
            tree_nodes.begin() +
                static_cast<std::ptrdiff_t>(end)
        );
        tree_nodes[index].expanded = false;
        status = "Tree branch collapsed";
    }

    void expand_tree_node(
        const std::size_t index,
        const bool rescan = false,
        const bool notify = true
    ) {
        if (!tree_panel || index >= tree_nodes.size()) {
            return;
        }
        if (tree_nodes[index].expanded && !rescan) {
            collapse_tree_node(index);
            return;
        }
        if (!rescan && tree_nodes[index].loaded &&
            !tree_nodes[index].has_children &&
            !tree_nodes[index].inaccessible) {
            status = "Tree branch has no children";
            return;
        }
        if (tree_expansion_running.load(
                std::memory_order_acquire
            )) {
            status = "Tree expansion already running";
            return;
        }
        if (tree_thread.joinable()) {
            tree_thread.join();
        }
        const auto panel_index = *tree_panel;
        const auto provider =
            (panel_index == 0 ? left : right).provider();
        const auto path = tree_nodes[index].path;
        const auto depth = tree_nodes[index].depth + 1U;
        const auto generation = tree_generation;
        tree_nodes[index].loading = true;
        tree_nodes[index].inaccessible = false;
        tree_expansion_cancellation = Cancellation{};
        tree_expansion_running.store(
            true,
            std::memory_order_release
        );
        if (notify) {
            status = rescan ? "Rescanning tree..." : "Expanding tree...";
        }
        try {
            tree_thread = std::jthread([
                this,
                provider,
                path,
                depth,
                panel_index,
                generation,
                rescan,
                notify
            ] {
                try {
                    std::vector<TreeNode> children;
                    for (const auto& entry : provider->list(
                             path,
                             {
                                 .show_hidden = true,
                                 .directories_first = true,
                             },
                             tree_expansion_cancellation
                         )) {
                        if (entry.kind == EntryKind::directory) {
                            children.push_back({
                                .path = entry.path,
                                .depth = depth,
                            });
                        }
                    }
                    post_ui([
                        this,
                        children = std::move(children),
                        path,
                        panel_index,
                        generation,
                        rescan,
                        notify
                    ]() mutable {
                        if (!tree_panel ||
                            *tree_panel != panel_index ||
                            tree_generation != generation) {
                            finish_tree_loading();
                            return;
                        }
                        const auto match = std::find_if(
                            tree_nodes.begin(),
                            tree_nodes.end(),
                            [&](const TreeNode& node) {
                                return node.path == path;
                            }
                        );
                        if (match == tree_nodes.end() ||
                            !match->loading) {
                            finish_tree_loading();
                            return;
                        }
                        const auto node_index =
                            static_cast<std::size_t>(
                                std::distance(
                                    tree_nodes.begin(),
                                    match
                                )
                            );
                        const auto selected_path =
                            tree_cursor < tree_nodes.size()
                                ? tree_nodes[tree_cursor].path
                                : path;
                        const auto old_depth = match->depth;
                        auto old_end = node_index + 1U;
                        while (old_end < tree_nodes.size() &&
                               tree_nodes[old_end].depth > old_depth) {
                            ++old_end;
                        }
                        std::unordered_map<
                            std::string,
                            std::pair<std::size_t, std::size_t>
                        > cached_children;
                        cached_children.reserve(children.size());
                        for (auto begin = node_index + 1U; begin < old_end;) {
                            auto end = begin + 1U;
                            while (end < old_end &&
                                   tree_nodes[end].depth >
                                       tree_nodes[begin].depth) {
                                ++end;
                            }
                            cached_children.emplace(
                                tree_nodes[begin].path,
                                std::pair{begin, end}
                            );
                            begin = end;
                        }
                        std::vector<TreeNode> replacement;
                        replacement.reserve(children.size());
                        for (auto& child : children) {
                            const auto previous =
                                cached_children.find(child.path);
                            if (previous == cached_children.end()) {
                                replacement.push_back(std::move(child));
                                continue;
                            }
                            replacement.insert(
                                replacement.end(),
                                tree_nodes.begin() +
                                    static_cast<std::ptrdiff_t>(
                                        previous->second.first
                                    ),
                                tree_nodes.begin() +
                                    static_cast<std::ptrdiff_t>(
                                        previous->second.second
                                    )
                            );
                        }
                        auto& node = tree_nodes[node_index];
                        node.loaded = true;
                        node.inaccessible = false;
                        node.loading = false;
                        node.has_children = !children.empty();
                        node.expanded = !children.empty();
                        tree_nodes.erase(
                            tree_nodes.begin() +
                                static_cast<std::ptrdiff_t>(
                                    node_index + 1U
                                ),
                            tree_nodes.begin() +
                                static_cast<std::ptrdiff_t>(old_end)
                        );
                        tree_nodes.insert(
                            tree_nodes.begin() +
                                static_cast<std::ptrdiff_t>(
                                    node_index + 1U
                                ),
                            std::make_move_iterator(replacement.begin()),
                            std::make_move_iterator(replacement.end())
                        );
                        restore_tree_selection(selected_path);
                        if (notify) {
                            status = rescan ? "Tree refreshed" : "Ready";
                        }
                        finish_tree_loading();
                    });
                } catch (const OperationCancelled&) {
                    post_ui([this] {
                        finish_tree_loading();
                    });
                } catch (const std::exception& error) {
                    const auto message = std::string(error.what());
                    post_ui([this, path, panel_index, generation, notify, message] {
                        if (!tree_panel ||
                            *tree_panel != panel_index ||
                            tree_generation != generation) {
                            finish_tree_loading();
                            return;
                        }
                        const auto match = std::find_if(
                            tree_nodes.begin(),
                            tree_nodes.end(),
                            [&](const TreeNode& node) {
                                return node.path == path;
                            }
                        );
                        if (match != tree_nodes.end()) {
                            match->loaded = true;
                            match->inaccessible = true;
                            match->loading = false;
                        }
                        status =
                            notify
                                ? "Tree: " + message
                                : status + "; Tree refresh failed: " + message;
                        finish_tree_loading();
                    });
                }
            });
        } catch (...) {
            tree_expansion_running.store(
                false,
                std::memory_order_release
            );
            tree_nodes[index].loading = false;
            throw;
        }
    }

    void show_tree_panel(const int panel_index) {
        auto& pane = panel_index == 0 ? left : right;
        std::size_t selected = 0;
        auto nodes = build_tree_nodes(pane, selected);
        invalidate_tree_loading();
        tree_refresh_pending.reset();
        tree_refresh_notify_pending = false;
        const bool was_active = active_index == panel_index;
        if (quick_view_panel) {
            close_quick_view(false);
        }
        if (information_panel) {
            close_information_panel(false);
        }
        tree_nodes = std::move(nodes);
        tree_cursor = selected;
        tree_panel = panel_index;
        if (was_active) {
            active_index = 1 - panel_index;
        }
        tree_panel_focused = false;
    }

    std::vector<Entry> current_operation_sources() const {
        if (tree_operation_source) {
            return {*tree_operation_source};
        }
        return active_index == 0
                   ? left.selected_entries()
                   : right.selected_entries();
    }

    std::vector<Entry> matching_operation_sources() const {
        auto sources = current_operation_sources();
        std::erase_if(
            sources,
            [&](const Entry& entry) {
                return fnmatch(
                           dialog_source_mask.c_str(),
                           entry.name.c_str(),
                           0
                       ) != 0;
            }
        );
        return sources;
    }

    void clear_tree_operation_context() {
        tree_operation_source.reset();
        tree_operation_source_provider.reset();
        tree_operation_destination_provider.reset();
    }

    bool prepare_tree_operation(
        const bool move_operation,
        const bool remove_operation
    ) {
        if (!tree_panel || tree_cursor >= tree_nodes.size()) {
            return false;
        }
        if (tree_cursor == 0U) {
            status = "Cannot operate on the tree root";
            return false;
        }
        const auto panel_index = *tree_panel;
        auto& pane = panel_index == 0 ? left : right;
        tree_operation_source = pane.provider()->stat(
            tree_nodes[tree_cursor].path,
            {}
        );
        tree_operation_source_provider = pane.provider();
        tree_operation_destination_provider = active().provider();
        tree_operation_source_index = panel_index;
        if (remove_operation) {
            dialog_focus = 0;
            mode = Mode::delete_dialog;
            return true;
        }
        dialog_move = move_operation;
        dialog_exact = move_operation;
        dialog_destination = active().path();
        dialog_source_mask = "*";
        destination_input.reset(dialog_destination, true);
        mask_input.reset(dialog_source_mask, true);
        dialog_preserve_attributes = true;
        dialog_preserve_symlinks = true;
        dialog_background = false;
        dialog_focus = 0;
        mode = Mode::operation_dialog;
        return true;
    }

    void sync_dynamic_tree() {
        if (!tree_dynamic ||
            tree_cursor >= tree_nodes.size()) {
            return;
        }
        try {
            active().change_directory(
                tree_nodes[tree_cursor].path
            );
        } catch (const std::exception& error) {
            set_error(error);
        }
    }

    void update_tree_selection() {
        sync_dynamic_tree();
        load_tree_surroundings();
    }

    void swap_panels() {
        std::swap(left, right);
        std::swap(pane_ids[0], pane_ids[1]);
        active_index = 1 - active_index;
        if (quick_view_panel) {
            quick_view_panel = 1 - *quick_view_panel;
        }
        if (information_panel) {
            information_panel = 1 - *information_panel;
        }
        if (tree_panel) {
            tree_panel = 1 - *tree_panel;
            if (tree_expansion_running.load(std::memory_order_acquire)) {
                refresh_tree();
            }
        }
    }

    void sort_active(const SortField field) {
        const auto descending =
            active().sort_field() == field
                ? !active().descending()
                : false;
        active().set_sort(field, descending);
        status = descending ? "Sort descending" : "Sort ascending";
    }

    void store_panel_options_in_config() {
        config.show_hidden = left.show_hidden();
        config.directories_first = left.directories_first();
        config.sort = left.sort_field();
        config.sort_descending = left.descending();
    }

    void store_editor_options_in_config() {
        config.editor_syntax = editor_syntax;
        config.editor_line_numbers = editor_line_numbers;
        config.editor_tab_width = editor.tab_width();
        config.editor_indent_width = editor_indent_width;
        config.editor_auto_indent = editor_auto_indent;
        config.editor_show_whitespace = editor_show_whitespace;
        config.editor_history = editor_history;
    }

    void set_error(const std::exception& error) {
        status = std::string("Error: ") + error.what();
    }

    void post_ui(std::function<void()> task) {
        if (terminal == nullptr) {
            return;
        }
        auto* app = terminal;
        app->Post([
            app,
            task = std::move(task)
        ]() mutable {
            task();
            app->RequestAnimationFrame();
        });
    }

    void start_traversal(
        std::string pattern,
        const bool panelize
    ) {
        if (traversal_running.load(std::memory_order_acquire)) {
            throw std::runtime_error(
                "a recursive search is already running"
            );
        }
        if (operation_running.load(std::memory_order_acquire)) {
            throw std::runtime_error(
                "cannot search during a file operation"
            );
        }
        if (traversal_thread.joinable()) {
            traversal_thread.join();
        }
        const auto panel_index = active_index;
        const auto provider = active().provider();
        const auto root = active().path();
        const auto include_hidden = active().show_hidden();
        traversal_cancellation = Cancellation{};
        traversal_running.store(true, std::memory_order_release);
        status = panelize ? "Panelizing..." : "Searching...";
        try {
            traversal_thread = std::jthread([
                this,
                pattern = std::move(pattern),
                panelize,
                panel_index,
                provider,
                root,
                include_hidden
            ] {
                try {
                    const SearchEngine search;
                    std::size_t error_count = 0;
                    std::string first_error;
                    auto entries = search.search(
                        *provider,
                        root,
                        {
                            .name_pattern =
                                pattern.empty() ? "*" : pattern,
                            .content = {},
                            .case_sensitive = panelize,
                            .recursive = true,
                            .include_hidden = include_hidden,
                        },
                        traversal_cancellation,
                        [this, panelize](
                            const TransferProgress& value
                        ) {
                            if (value.files_done % 128U != 0U) {
                                return;
                            }
                            post_ui([this, panelize, value] {
                                status =
                                    (
                                        panelize
                                            ? "Panelizing "
                                            : "Searching "
                                    ) +
                                    std::to_string(value.files_done) +
                                    " entries";
                            });
                        },
                        [&](const std::string& path, const std::string& message) {
                            ++error_count;
                            if (first_error.empty()) {
                                first_error = path + ": " + message;
                            }
                        }
                    );
                    post_ui([
                        this,
                        entries = std::move(entries),
                        panelize,
                        panel_index,
                        provider,
                        root,
                        error_count,
                        first_error = std::move(first_error)
                    ]() mutable {
                        traversal_running.store(
                            false,
                            std::memory_order_release
                        );
                        if (panelize) {
                            auto& pane =
                                panel_index == 0 ? left : right;
                            if (pane.path() != root) {
                                status =
                                    "Panelize results discarded: "
                                    "panel directory changed";
                                return;
                            }
                            const auto count = entries.size();
                            pane.set_panelized(std::move(entries));
                            status =
                                std::to_string(count) +
                                " panelized result(s)";
                        } else {
                            search_results = std::move(entries);
                            search_provider = provider;
                            search_warning.clear();
                            if (error_count != 0U) {
                                search_warning =
                                    "Warning: " + std::to_string(error_count) +
                                    " unreadable path(s): " + first_error;
                            }
                            search_cursor = 0;
                            if (mode == Mode::panels) {
                                mode = Mode::search_results;
                            }
                            status =
                                std::to_string(
                                    search_results.size()
                                ) +
                                (
                                    mode == Mode::search_results
                                        ? " search result(s)"
                                        : " search result(s) ready"
                                );
                        }
                        if (error_count != 0U) {
                            status += "; " + std::to_string(error_count) +
                                      " unreadable path(s): " + first_error;
                        }
                    });
                } catch (const OperationCancelled&) {
                    post_ui([this] {
                        traversal_running.store(
                            false,
                            std::memory_order_release
                        );
                        status = "Search cancelled";
                    });
                } catch (const std::exception& error) {
                    const auto message = std::string(error.what());
                    post_ui([this, message] {
                        traversal_running.store(
                            false,
                            std::memory_order_release
                        );
                        status = "Search error: " + message;
                    });
                }
            });
        } catch (...) {
            traversal_running.store(
                false,
                std::memory_order_release
            );
            throw;
        }
    }

    void request_operation_cancel() {
        {
            std::lock_guard lock(operation_pause_mutex);
            operation_cancellation.request();
            operation_paused.store(
                false,
                std::memory_order_release
            );
        }
        operation_pause_condition.notify_all();
        operation_prompt_condition.notify_all();
    }

    OperationChoice ask_operation(
        Entry source,
        std::optional<Entry> destination,
        std::string error = {}
    ) {
        {
            std::lock_guard lock(operation_prompt_mutex);
            operation_answer.reset();
            operation_prompt_pending.store(true, std::memory_order_release);
        }
        post_ui([
            this, source = std::move(source),
            destination = std::move(destination), error = std::move(error)
        ]() mutable {
            if (!operation_prompt_pending.load(std::memory_order_acquire) ||
                operation_cancellation.requested()) {
                return;
            }
            operation_return_mode = mode;
            operation_return_focus = dialog_focus;
            operation_prompt_source = std::move(source);
            operation_prompt_destination = std::move(destination);
            operation_prompt_error = std::move(error);
            dialog_focus = 0;
            mode = operation_prompt_destination
                       ? Mode::conflict_dialog
                       : Mode::operation_error_dialog;
        });
        std::unique_lock lock(operation_prompt_mutex);
        while (!operation_answer && !operation_cancellation.requested()) {
            operation_prompt_condition.wait_for(lock, std::chrono::milliseconds(100));
        }
        operation_prompt_pending.store(false, std::memory_order_release);
        operation_cancellation.throw_if_requested();
        return *operation_answer;
    }

    void answer_operation(const OperationChoice answer) {
        mode = operation_return_mode;
        dialog_focus = operation_return_focus;
        {
            std::lock_guard lock(operation_prompt_mutex);
            operation_answer = answer;
        }
        operation_prompt_condition.notify_all();
    }

    OperationErrorResolver error_handler(
        const std::shared_ptr<OperationPolicy>& policy
    ) {
        return [this, policy](const Entry& source, const std::string& message) {
            if (policy->ignore_errors) {
                return OperationErrorDecision::skip;
            }
            switch (ask_operation(source, std::nullopt, message)) {
                case OperationChoice::retry:
                    return OperationErrorDecision::retry;
                case OperationChoice::ignore_all:
                    policy->ignore_errors = true;
                    return OperationErrorDecision::skip;
                case OperationChoice::skip:
                    return OperationErrorDecision::skip;
                default:
                    return OperationErrorDecision::cancel;
            }
        };
    }

    void start_job(const std::string& destination) {
        if (jobs.size() >= 20U) {
            jobs.erase(jobs.begin());
        }
        jobs.push_back({next_job_id++, operation_label, destination, "Running"});
        active_job = jobs.size() - 1U;
        job_cursor = *active_job;
    }

    void exit_terminal() {
        if (!exiting) {
            if (editor.dirty() ||
                std::ranges::any_of(
                    editor_buffers,
                    [](const auto& buffer) {
                        return buffer.dirty();
                    }
                )) {
                request_editor_quit();
                return;
            }
            exiting = true;
            const auto size = ftxui::Terminal::Size();
            std::string blank;
            blank.reserve(
                static_cast<std::size_t>(
                    std::max(0, size.dimx * size.dimy)
                ) +
                static_cast<std::size_t>(
                    std::max(0, size.dimy * 12)
                )
            );
            const std::string row(
                static_cast<std::size_t>(std::max(0, size.dimx)),
                ' '
            );
            for (int line = 1; line <= size.dimy; ++line) {
                blank += "\033[" + std::to_string(line) + ";1H";
                blank += row;
            }
            blank += "\033[H";
            std::cout << blank << std::flush;
            terminal->Exit();
        }
    }

    void finish_operation(
        std::string message,
        const std::optional<int> source_id = std::nullopt,
        const std::vector<std::string>& completed_sources = {},
        bool notify = false
    ) {
        if (mode == Mode::conflict_dialog || mode == Mode::operation_error_dialog) {
            mode = operation_return_mode;
            dialog_focus = operation_return_focus;
        }
        const bool background =
            mode != Mode::panels && mode != Mode::operation_progress;
        if (mode == Mode::operation_progress) {
            mode = Mode::panels;
        }
        operation_running.store(false, std::memory_order_release);
        try {
            if (source_id == pane_ids[0]) {
                left.unmark_paths(completed_sources);
            } else if (source_id == pane_ids[1]) {
                right.unmark_paths(completed_sources);
            }
            refresh();
            status = std::move(message);
        } catch (const std::exception& error) {
            // A refresh error must not dismiss the foreground editor or dialog.
            status =
                std::move(message) +
                "; refresh failed: " + error.what();
            notify = true;
        }
        if (background && notify) {
            pending_operation_notice = status;
        }
        if (active_job) {
            jobs[*active_job].status = status;
        }
    }

    void start_prompt(
        const PromptAction action,
        std::string title,
        std::string value = {}
    ) {
        mode_before_prompt =
            mode == Mode::pulldown ? Mode::panels : mode;
        mode = Mode::prompt;
        prompt_action = action;
        prompt_title = std::move(title);
        prompt_value = std::move(value);
        prompt_targets.clear();
        prompt_provider.reset();
        if (action == PromptAction::chmod || action == PromptAction::chown) {
            prompt_targets = active().selected_entries();
            std::erase_if(prompt_targets, [](const Entry& entry) {
                return entry.revision == "mcpp:parent" || entry.name == "..";
            });
            if (prompt_targets.empty()) {
                mode = mode_before_prompt;
                prompt_action = PromptAction::none;
                throw std::runtime_error("no files selected");
            }
            prompt_provider = active().provider();
            const auto& first = prompt_targets.front();
            if (action == PromptAction::chmod) {
                std::ostringstream permissions;
                permissions << std::oct << std::setfill('0') << std::setw(4)
                            << (static_cast<unsigned int>(first.permissions) & 07777U);
                prompt_value = permissions.str();
            } else {
                prompt_value = first.owner + ":" + first.group;
            }
        }
        if (action == PromptAction::editor_save_as) {
            editor_save_line_ending = EditorLineEnding::unchanged;
        }
        prompt_inputs[action].reset(prompt_value, true);
        prompt_error.clear();
        prompt_focus = 0;
        prompt_boxes = {};
    }

    void remember_input(TextInputState& input, const std::string& value) {
        if (!value.empty() &&
            (input.history.empty() || input.history.back() != value)) {
            input.history.push_back(value);
            if (input.history.size() > 100U) {
                input.history.erase(input.history.begin());
            }
        }
        input.history_index = input.history.size();
    }

    bool edit_input(
        std::string& value,
        TextInputState& input,
        const ftxui::Event& event
    ) {
        using ftxui::Event;
        input.completions.clear();
        input.message.clear();
        input.cursor = std::min(input.cursor, value.size());
        if (input.anchor) {
            *input.anchor = std::min(*input.anchor, value.size());
        }
        const auto selected = [&]() -> std::pair<std::size_t, std::size_t> {
            const auto anchor = input.anchor.value_or(input.cursor);
            return std::minmax(anchor, input.cursor);
        };
        const auto erase_selection = [&] {
            const auto [begin, end] = selected();
            if (begin == end) {
                return false;
            }
            value.erase(begin, end - begin);
            input.cursor = begin;
            input.anchor.reset();
            return true;
        };
        const auto move = [&](const std::size_t position, const bool extend) {
            if (extend) {
                if (!input.anchor) {
                    input.anchor = input.cursor;
                }
            } else {
                input.anchor.reset();
            }
            input.cursor = position;
        };
        const auto word = [&](const std::size_t position) {
            const auto character = static_cast<unsigned char>(value[position]);
            return character >= 0x80U || std::isalnum(character) != 0 ||
                   character == '_';
        };
        const auto word_left = [&] {
            auto position = input.cursor;
            while (position > 0U && !word(previous_utf8(value, position))) {
                position = previous_utf8(value, position);
            }
            while (position > 0U && word(previous_utf8(value, position))) {
                position = previous_utf8(value, position);
            }
            return position;
        };
        const auto word_right = [&] {
            auto position = input.cursor;
            while (position < value.size() && !word(position)) {
                position = next_utf8(value, position);
            }
            while (position < value.size() && word(position)) {
                position = next_utf8(value, position);
            }
            return position;
        };
        const bool select_left =
            event == Event::Special("\x1B[1;2D");
        const bool select_right =
            event == Event::Special("\x1B[1;2C");
        const bool select_word_left =
            event == Event::Special("\x1B[1;6D");
        const bool select_word_right =
            event == Event::Special("\x1B[1;6C");
        if (event == Event::Home || event == Event::CtrlA ||
            event == Event::Special("\x1B[1;2H")) {
            move(0U, event == Event::Special("\x1B[1;2H"));
        } else if (event == Event::End || event == Event::CtrlE ||
                   event == Event::Special("\x1B[1;2F")) {
            move(value.size(), event == Event::Special("\x1B[1;2F"));
        } else if (event == Event::ArrowLeftCtrl || select_word_left) {
            move(word_left(), select_word_left);
        } else if (event == Event::ArrowRightCtrl || select_word_right) {
            move(word_right(), select_word_right);
        } else if (event == Event::ArrowLeft || event == Event::CtrlB ||
                   select_left) {
            const auto range = selected();
            move(!select_left && range.first != range.second
                     ? range.first
                     : previous_utf8(value, input.cursor),
                 select_left);
        } else if (event == Event::ArrowRight || event == Event::CtrlF ||
                   select_right) {
            const auto range = selected();
            move(!select_right && range.first != range.second
                     ? range.second
                     : next_utf8(value, input.cursor),
                 select_right);
        } else if (event == Event::CtrlU) {
            input_clipboard = value;
            value.clear();
            move(0U, false);
        } else if (event == Event::CtrlK || event == Event::CtrlW ||
                   event == Event::Special("\x1B[3;5~")) {
            auto [begin, end] = selected();
            if (begin == end) {
                begin = event == Event::CtrlW ? word_left() : input.cursor;
                end = event == Event::CtrlK
                          ? value.size()
                          : event == Event::CtrlW ? input.cursor : word_right();
            }
            input_clipboard = value.substr(begin, end - begin);
            value.erase(begin, end - begin);
            move(begin, false);
        } else if (event == Event::Backspace || event == Event::Delete) {
            if (!erase_selection()) {
                const auto begin = event == Event::Backspace
                                       ? previous_utf8(value, input.cursor)
                                       : input.cursor;
                const auto end = event == Event::Backspace
                                     ? input.cursor
                                     : next_utf8(value, input.cursor);
                value.erase(begin, end - begin);
                move(begin, false);
            }
        } else if (event == Event::CtrlY ||
                   event == Event::Special("\x1B[2;2~")) {
            erase_selection();
            value.insert(input.cursor, input_clipboard);
            move(input.cursor + input_clipboard.size(), false);
        } else if (event == Event::Special("\x1B[2;5~") ||
                   event == Event::Special("\x1B[3;2~")) {
            const auto [begin, end] = selected();
            input_clipboard = value.substr(begin, end - begin);
            if (event == Event::Special("\x1B[3;2~")) {
                erase_selection();
            }
        } else if (event == Event::AltP || event == Event::ArrowUp ||
                   event == Event::AltN || event == Event::ArrowDown) {
            const bool backward = event == Event::AltP || event == Event::ArrowUp;
            if (input.history_index == input.history.size()) {
                input.draft = value;
            }
            if (backward && input.history_index > 0U) {
                --input.history_index;
            } else if (!backward && input.history_index < input.history.size()) {
                ++input.history_index;
            }
            value = input.history_index < input.history.size()
                        ? input.history[input.history_index]
                        : input.draft;
            move(value.size(), false);
        } else if (event.is_character()) {
            erase_selection();
            value.insert(input.cursor, event.character());
            move(input.cursor + event.character().size(), false);
        } else {
            return false;
        }
        return true;
    }

    ftxui::Element render_input(
        const std::string& value,
        TextInputState& input,
        const bool active,
        const std::string_view prefix = "> "
    ) {
        using namespace ftxui;
        input.cursor = std::min(input.cursor, value.size());
        input.rendered_cursor = input.cursor;
        const auto anchor = std::min(input.anchor.value_or(input.cursor), value.size());
        const auto selected = std::minmax(anchor, input.cursor);
        Elements cells;
        for (std::size_t offset = 0; offset <= value.size();) {
            const auto next = next_utf8(value, offset);
            auto cell = text(
                offset == value.size()
                    ? " "
                    : display_text(std::string_view(value).substr(offset, next - offset))
            );
            if (offset >= selected.first && offset < selected.second) {
                cell = cell | bold | underlined;
            }
            if (offset == input.cursor) {
                cell = std::move(cell) | reflect(input.cursor_box);
                if (active) {
                    cell = std::move(cell) | focusCursorBarBlinking;
                }
            }
            cells.push_back(std::move(cell));
            if (offset == value.size()) {
                break;
            }
            offset = next;
        }
        auto field = hbox({
            text(std::string(prefix)),
            hbox(std::move(cells)) | xflex | xframe,
        }) | reflect(input.box);
        return active ? field | inverted : field;
    }

    bool input_mouse(
        const std::string& value,
        TextInputState& input,
        const ftxui::Mouse& mouse
    ) {
        if (mouse.motion == ftxui::Mouse::Released) {
            input.dragging = false;
            if (input.anchor == input.cursor) {
                input.anchor.reset();
            }
            return true;
        }
        if (mouse.button != ftxui::Mouse::Left ||
            (!input.dragging && !input.box.Contain(mouse.x, mouse.y))) {
            return false;
        }
        const auto target = std::max(
            0,
            ftxui::string_width(display_text(
                std::string_view(value).substr(0, input.rendered_cursor)
            )) + mouse.x - input.cursor_box.x_min
        );
        std::size_t position = 0;
        int width = 0;
        while (position < value.size()) {
            const auto next = next_utf8(value, position);
            const auto columns = ftxui::string_width(display_text(
                std::string_view(value).substr(position, next - position)
            ));
            if (width + columns > target) {
                break;
            }
            width += columns;
            position = next;
        }
        if (mouse.motion == ftxui::Mouse::Pressed) {
            input.anchor = mouse.shift ? input.cursor : position;
            input.dragging = true;
        }
        if (input.dragging) {
            input.cursor = position;
            input.completions.clear();
            input.message.clear();
            return true;
        }
        return false;
    }

    void complete_path(
        std::string& value,
        TextInputState& input,
        const std::string& base
    ) {
        const auto apply = [&](const std::string& completion) {
            const auto suffix =
                completion.ends_with('/') && input.completion_suffix.starts_with('/')
                    ? input.completion_suffix.substr(1U)
                    : input.completion_suffix;
            value = input.completion_prefix + completion + suffix;
            input.cursor = input.completion_prefix.size() + completion.size();
            input.anchor.reset();
            input.completed_value = value;
        };
        if (!input.completions.empty() && value == input.completed_value) {
            input.completion_index =
                (input.completion_index + 1U) % input.completions.size();
            apply(input.completions[input.completion_index]);
            return;
        }
        input.cursor = std::min(input.cursor, value.size());
        const auto before = std::string_view(value).substr(0, input.cursor);
        const auto slash = before.rfind('/');
        auto directory_text =
            slash == std::string_view::npos
                ? std::string{}
                : std::string(before.substr(0, slash + 1U));
        const auto prefix = std::string(
            before.substr(slash == std::string_view::npos ? 0U : slash + 1U)
        );
        const auto end = value.find('/', input.cursor);
        auto suffix = end == std::string::npos ? std::string{} : value.substr(end);
        const bool directories_only = !suffix.empty();
        if (directory_text.starts_with("~/")) {
            const auto* home = std::getenv("HOME");
            if (home == nullptr) {
                throw std::runtime_error("HOME is not set");
            }
            directory_text = std::string(home) + directory_text.substr(1U);
        }
        auto directory = std::filesystem::path(directory_text);
        if (directory.empty() || directory.is_relative()) {
            directory = std::filesystem::path(base) / directory;
        }
        std::vector<std::string> matches;
        for (const auto& entry : std::filesystem::directory_iterator(directory)) {
            const auto name = entry.path().filename().string();
            if (!name.starts_with(prefix) ||
                (name.starts_with('.') && !prefix.starts_with('.'))) {
                continue;
            }
            std::error_code error;
            const bool is_directory = entry.is_directory(error);
            if (error && error != std::errc::no_such_file_or_directory) {
                throw std::filesystem::filesystem_error(
                    "cannot inspect completion", entry.path(), error
                );
            }
            if (!directories_only || is_directory) {
                matches.push_back(name + (is_directory ? "/" : ""));
            }
        }
        std::sort(matches.begin(), matches.end());
        input.completions = matches;
        input.completion_prefix = directory_text;
        input.completion_suffix = std::move(suffix);
        if (matches.empty()) {
            input.message = "No matching paths";
            return;
        }
        if (matches.size() == 1U) {
            apply(matches.front());
            input.completions.clear();
            input.message.clear();
            return;
        }
        auto common = matches.front();
        for (const auto& match : matches) {
            std::size_t count = 0;
            while (count < common.size() && count < match.size() &&
                   common[count] == match[count]) {
                ++count;
            }
            while (count > 0U && count < common.size() &&
                   (static_cast<unsigned char>(common[count]) & 0xC0U) == 0x80U) {
                --count;
            }
            common.resize(count);
        }
        apply(common);
        input.completion_index = matches.size() - 1U;
        input.message =
            std::to_string(matches.size()) + " paths; Alt-Tab cycles matches";
    }

    void cancel_prompt() {
        mode = mode_before_prompt;
        prompt_action = PromptAction::none;
        prompt_value.clear();
        prompt_error.clear();
    }

    void start_operation_dialog(
        const bool move_operation,
        const bool exact
    ) {
        clear_tree_operation_context();
        const auto sources = active().selected_entries();
        if (sources.empty()) {
            throw std::runtime_error("no source selected");
        }
        dialog_move = move_operation;
        dialog_exact = exact && sources.size() == 1U;
        dialog_destination = passive().path();
        dialog_preserve_attributes = true;
        dialog_preserve_symlinks = true;
        dialog_background = false;
        dialog_source_mask = "*";
        destination_input.reset(dialog_destination, true);
        mask_input.reset(dialog_source_mask, true);
        dialog_focus = 0;
        mode = Mode::operation_dialog;
    }

    void confirm_operation_dialog() {
        if (dialog_destination.empty()) {
            throw std::runtime_error("destination is empty");
        }
        const auto sources = matching_operation_sources();
        if (sources.empty()) {
            throw std::runtime_error(
                "source mask matches no selected entries"
            );
        }
        const auto destination_provider =
            tree_operation_source
                ? tree_operation_destination_provider
                : passive().provider();
        bool destination_directory = dialog_destination.ends_with('/');
        if (std::filesystem::path(dialog_destination).is_relative()) {
            dialog_destination = destination_provider->join(
                active().path(),
                dialog_destination
            );
        }
        if (destination_provider->exists(dialog_destination)) {
            const auto destination = destination_provider->stat(
                dialog_destination,
                {}
            );
            const bool is_directory =
                destination.kind == EntryKind::directory ||
                destination.target_is_directory;
            if (destination_directory && !is_directory) {
                throw std::runtime_error(
                    "destination is not a directory"
                );
            }
            destination_directory = is_directory;
            if (sources.size() > 1U && !is_directory) {
                throw std::runtime_error(
                    "multiple sources require a directory destination"
                );
            }
        }
        dialog_exact = sources.size() == 1U &&
                       !destination_directory;
        run_operation(
            dialog_move,
            dialog_destination,
            !config.confirm_overwrite,
            dialog_exact
        );
        if (dialog_background) {
            mode = Mode::panels;
            status = operation_label + " running in background";
        }
    }

    void start_delete_dialog() {
        clear_tree_operation_context();
        if (active().selected_entries().empty()) {
            throw std::runtime_error("no source selected");
        }
        dialog_focus = 0;
        mode = Mode::delete_dialog;
    }

    void run_operation(
        const bool move_operation,
        const std::string& destination,
        const bool overwrite,
        const bool exact = false
    ) {
        if (operation_running.load(std::memory_order_acquire)) {
            throw std::runtime_error("an operation is already running");
        }
        if (traversal_running.load(std::memory_order_acquire)) {
            throw std::runtime_error(
                "cannot start an operation while search is running"
            );
        }
        const auto sources = matching_operation_sources();
        if (sources.empty()) {
            throw std::runtime_error(
                "source mask matches no selected entries"
            );
        }
        if (operation_thread.joinable()) {
            operation_thread.join();
        }
        operation_cancellation = Cancellation{};
        progress = {};
        const auto source_provider =
            tree_operation_source
                ? tree_operation_source_provider
                : active().provider();
        const auto destination_provider =
            tree_operation_source
                ? tree_operation_destination_provider
                : passive().provider();
        const auto source_id = pane_ids[
            tree_operation_source
                ? tree_operation_source_index
                : active_index
        ];
        const auto policy = std::make_shared<OperationPolicy>();
        const OperationOptions options{
            .overwrite = overwrite,
            .preserve_permissions = dialog_preserve_attributes,
            .preserve_symlinks = dialog_preserve_symlinks,
            .collision =
                [this, policy](const Entry& source, const Entry& target) {
                    if (policy->collisions) {
                        return *policy->collisions;
                    }
                    if (policy->older) {
                        return source.kind == EntryKind::directory ||
                                       source.modified > target.modified
                                   ? CollisionDecision::overwrite
                                   : CollisionDecision::skip;
                    }
                    switch (ask_operation(source, target)) {
                        case OperationChoice::overwrite:
                            return CollisionDecision::overwrite;
                        case OperationChoice::skip:
                            return CollisionDecision::skip;
                        case OperationChoice::overwrite_all:
                            policy->collisions = CollisionDecision::overwrite;
                            return CollisionDecision::overwrite;
                        case OperationChoice::skip_all:
                            policy->collisions = CollisionDecision::skip;
                            return CollisionDecision::skip;
                        case OperationChoice::older:
                            policy->older = true;
                            return source.kind == EntryKind::directory ||
                                           source.modified > target.modified
                                       ? CollisionDecision::overwrite
                                       : CollisionDecision::skip;
                        default:
                            return CollisionDecision::cancel;
                    }
                },
            .error = error_handler(policy),
        };
        clear_tree_operation_context();
        operation_running.store(true, std::memory_order_release);
        operation_started = std::chrono::steady_clock::now();
        operation_paused.store(false, std::memory_order_release);
        operation_skip.store(false, std::memory_order_release);
        mode = Mode::operation_progress;
        operation_label = move_operation ? "Move" : "Copy";
        start_job(destination);
        status = move_operation ? "Starting move" : "Starting copy";
        try {
            operation_thread = std::jthread(
            [
                this,
                move_operation,
                sources,
                source_provider,
                destination_provider,
                destination,
                options,
                source_id,
                exact
            ]() {
                OperationResult result;
                try {
                    const auto callback =
                        [this, move_operation](
                            const TransferProgress& value
                        ) {
                            {
                                std::unique_lock lock(
                                    operation_pause_mutex
                                );
                                operation_pause_condition.wait(
                                    lock,
                                    [&] {
                                        return !operation_paused.load(
                                                   std::memory_order_acquire
                                               ) ||
                                               Cancellation{}
                                                   .requested();
                                    }
                                );
                            }
                            operation_cancellation.throw_if_requested();
                            if (operation_skip.exchange(
                                    false,
                                    std::memory_order_acq_rel
                                )) {
                                throw SkipCurrent{};
                            }
                            if (terminal == nullptr) {
                                return;
                            }
                            post_ui([
                                this,
                                move_operation,
                                value
                            ] {
                                std::lock_guard lock(operation_mutex);
                                progress = value;
                                status =
                                    (move_operation
                                         ? "Moving "
                                         : "Copying ") +
                                    display_text(value.current_path) +
                                    " (" +
                                    std::to_string(value.files_done) +
                                    "/" +
                                    std::to_string(value.files_total) +
                                    ")";
                            });
                        };
                    if (move_operation && exact) {
                        if (sources.size() != 1U) {
                            throw std::runtime_error(
                                "exact move requires one source"
                            );
                        }
                        operations.move_exact(
                            *source_provider,
                            sources.front(),
                            *destination_provider,
                            destination,
                            options,
                            operation_cancellation,
                            callback,
                            &result
                        );
                    } else if (move_operation) {
                        operations.move(
                            *source_provider,
                            sources,
                            *destination_provider,
                            destination,
                            options,
                            operation_cancellation,
                            callback,
                            &result
                        );
                    } else if (exact) {
                        operations.copy_exact(
                            *source_provider,
                            sources.front(),
                            *destination_provider,
                            destination,
                            options,
                            operation_cancellation,
                            callback,
                            &result
                        );
                    } else {
                        operations.copy(
                            *source_provider,
                            sources,
                            *destination_provider,
                            destination,
                            options,
                            operation_cancellation,
                            callback,
                            &result
                        );
                    }
                    post_ui([
                        this,
                        move_operation,
                        source_id,
                        result = std::move(result)
                    ] {
                        const std::string label =
                            move_operation ? "Move" : "Copy";
                        auto message = label + " complete";
                        if (!result.skipped.empty()) {
                            message =
                                label + " incomplete: " +
                                std::to_string(result.skipped.size()) +
                                " skipped (" +
                                display_text(result.skipped.front().reason) +
                                "; " +
                                display_text(result.skipped.front().path) +
                                ")";
                        }
                        finish_operation(
                            std::move(message),
                            source_id,
                            result.completed_sources,
                            !result.skipped.empty()
                        );
                    });
                } catch (const OperationCancelled&) {
                    post_ui([
                        this,
                        source_id,
                        completed = std::move(result.completed_sources)
                    ] {
                        finish_operation(
                            "Operation cancelled",
                            source_id,
                            completed,
                            true
                        );
                    });
                } catch (const std::exception& error) {
                    const auto message = std::string(error.what());
                    post_ui([
                        this,
                        message,
                        source_id,
                        completed = std::move(result.completed_sources)
                    ] {
                        finish_operation(
                            "Error: " + message,
                            source_id,
                            completed,
                            true
                        );
                    });
                }
            });
        } catch (const std::exception& error) {
            operation_running.store(
                false,
                std::memory_order_release
            );
            if (active_job) {
                jobs[*active_job].status = "Error: " + std::string(error.what());
            }
            mode = Mode::panels;
            throw;
        }
    }

    void run_remove_operation() {
        if (operation_running.load(std::memory_order_acquire)) {
            throw std::runtime_error("an operation is already running");
        }
        if (traversal_running.load(std::memory_order_acquire)) {
            throw std::runtime_error(
                "cannot delete while search is running"
            );
        }
        const auto entries = current_operation_sources();
        if (entries.empty()) {
            throw std::runtime_error("no source selected");
        }
        if (operation_thread.joinable()) {
            operation_thread.join();
        }
        const auto provider =
            tree_operation_source
                ? tree_operation_source_provider
                : active().provider();
        const auto source_id = pane_ids[
            tree_operation_source
                ? tree_operation_source_index
                : active_index
        ];
        clear_tree_operation_context();
        operation_cancellation = Cancellation{};
        progress = {};
        operation_running.store(true, std::memory_order_release);
        operation_started = std::chrono::steady_clock::now();
        operation_paused.store(false, std::memory_order_release);
        operation_skip.store(false, std::memory_order_release);
        mode = Mode::operation_progress;
        operation_label = "Delete";
        start_job({});
        const auto resolve_error = error_handler(std::make_shared<OperationPolicy>());
        status = "Starting delete";
        try {
            operation_thread = std::jthread(
            [this, provider, entries, source_id, resolve_error] {
                OperationResult result;
                try {
                    operations.remove(
                        *provider,
                        entries,
                        operation_cancellation,
                        [this](const TransferProgress& value) {
                            {
                                std::unique_lock lock(
                                    operation_pause_mutex
                                );
                                operation_pause_condition.wait(
                                    lock,
                                    [&] {
                                        return !operation_paused.load(
                                                   std::memory_order_acquire
                                               ) ||
                                               Cancellation{}
                                                   .requested();
                                    }
                                );
                            }
                            operation_cancellation.throw_if_requested();
                            post_ui([this, value] {
                                std::lock_guard lock(operation_mutex);
                                progress = value;
                                status =
                                    "Deleting " +
                                    display_text(value.current_path) +
                                    " (" +
                                    std::to_string(value.files_done) +
                                    "/" +
                                    std::to_string(value.files_total) +
                                    ")";
                            });
                        },
                        &result,
                        resolve_error
                    );
                    post_ui([
                        this,
                        source_id,
                        result = std::move(result)
                    ] {
                        finish_operation(
                            result.skipped.empty()
                                ? "Delete complete"
                                : "Delete incomplete: " +
                                      std::to_string(result.skipped.size()) +
                                      " skipped",
                            source_id,
                            result.completed_sources,
                            !result.skipped.empty()
                        );
                    });
                } catch (const OperationCancelled&) {
                    post_ui([
                        this,
                        source_id,
                        completed = std::move(result.completed_sources)
                    ] {
                        finish_operation(
                            "Delete cancelled",
                            source_id,
                            completed,
                            true
                        );
                    });
                } catch (const std::exception& error) {
                    const auto message = std::string(error.what());
                    post_ui([
                        this,
                        message,
                        source_id,
                        completed = std::move(result.completed_sources)
                    ] {
                        finish_operation(
                            "Error: " + message,
                            source_id,
                            completed,
                            true
                        );
                    });
                }
            });
        } catch (const std::exception& error) {
            operation_running.store(
                false,
                std::memory_order_release
            );
            if (active_job) {
                jobs[*active_job].status = "Error: " + std::string(error.what());
            }
            mode = Mode::panels;
            throw;
        }
    }

    int run_process(
        const std::string& command,
        const bool interactive
    ) {
        if (operation_running.load(std::memory_order_acquire)) {
            throw std::runtime_error(
                "cannot launch a command while a file operation is running"
            );
        }
        if (traversal_running.load(std::memory_order_acquire)) {
            throw std::runtime_error(
                "cannot launch a command while search is running"
            );
        }
        int child_status = 0;
        terminal->WithRestoredIO([&] {
            if (interactive) {
                std::cout << "\033[H\033[2J" << std::flush;
            }
            const auto child = fork();
            if (child < 0) {
                throw std::runtime_error("failed to fork shell");
            }
            if (child == 0) {
                if (chdir(active().path().c_str()) != 0) {
                    _exit(126);
                }
                const auto* shell = std::getenv("SHELL");
                if (shell == nullptr || *shell == '\0') {
                    shell = "/bin/sh";
                }
                if (interactive) {
                    execl(shell, shell, static_cast<char*>(nullptr));
                } else {
                    execl(
                        shell,
                        shell,
                        "-c",
                        command.c_str(),
                        static_cast<char*>(nullptr)
                    );
                }
                _exit(127);
            }
            while (waitpid(child, &child_status, 0) < 0) {
                if (errno != EINTR) {
                    throw std::runtime_error(
                        "failed to wait for shell"
                    );
                }
            }
        })();
        refresh();
        if (WIFEXITED(child_status)) {
            return WEXITSTATUS(child_status);
        }
        return WIFSIGNALED(child_status)
                   ? 128 + WTERMSIG(child_status)
                   : 1;
    }

    void execute_command() {
        if (command_line.empty()) {
            command_mode = false;
            return;
        }
        const auto command = command_line;
        command_history.push_back(command);
        command_input.history = command_history;
        command_line.clear();
        command_input.reset(command_line);
        command_mode = false;
        try {
            const auto words = plain_command_words(command);
            if (words && !words->empty() && words->front() == "exit" &&
                words->size() == 1U) {
                handle_panels(ftxui::Event::F10);
                return;
            }
            if (words && !words->empty() && words->front() == "cd") {
                if (words->size() > 2U) {
                    throw std::runtime_error("cd: too many arguments");
                }
                auto destination = words->size() == 1U
                                       ? std::string("~")
                                       : (*words)[1];
                auto& previous = command_previous_directories[
                    static_cast<std::size_t>(pane_ids[active_index])
                ];
                if (destination == "-") {
                    if (previous.empty()) {
                        throw std::runtime_error("cd: no previous directory");
                    }
                    destination = previous;
                } else if (destination == "~" || destination.starts_with("~/")) {
                    const auto* home = std::getenv("HOME");
                    if (home == nullptr || *home == '\0') {
                        throw std::runtime_error("cd: HOME is not set");
                    }
                    destination = std::string(home) + destination.substr(1U);
                }
                const auto old_path = active().path();
                active().change_directory(active().provider()->join(
                    old_path, destination
                ));
                previous = old_path;
                status = "Directory changed: " + active().path();
                return;
            }
            const auto result = run_process(command, false);
            status = "Command exited " + std::to_string(result);
        } catch (const std::exception& error) {
            set_error(error);
        }
    }

    void execute_prompt() {
        try {
            switch (prompt_action) {
                case PromptAction::view_file: {
                    if (prompt_value.empty()) {
                        throw std::invalid_argument(
                            "filename is empty"
                        );
                    }
                    const auto provider = active().provider();
                    const auto path = provider->join(
                        active().path(),
                        prompt_value
                    );
                    if (!can_open_file(provider->stat(path, {}))) {
                        throw std::runtime_error(
                            "not a regular file: " + path
                        );
                    }
                    viewer.open(provider, path);
                    viewer_search_needle.clear();
                    viewer_return_mode = Mode::panels;
                    prompt_action = PromptAction::none;
                    prompt_value.clear();
                    prompt_error.clear();
                    mode = Mode::viewer;
                    return;
                }
                case PromptAction::rename: {
                    const auto* entry = active().current();
                    if (entry == nullptr || prompt_value.empty()) {
                        break;
                    }
                    active().provider()->rename(
                        entry->path,
                        active().provider()->join(
                            active().path(),
                            prompt_value
                        ),
                        false
                    );
                    active().refresh();
                    status = "Renamed";
                    break;
                }
                case PromptAction::mkdir:
                    active().provider()->create_directory(
                        active().provider()->join(
                            active().path(),
                            prompt_value
                        )
                    );
                    active().refresh();
                    status = "Directory created";
                    break;
                case PromptAction::recursive_search: {
                    start_traversal(prompt_value, false);
                    break;
                }
                case PromptAction::panel_filter:
                    active().set_filter(prompt_value);
                    status = prompt_value.empty()
                                 ? "Panel filter cleared"
                                 : "Panel filter: " + prompt_value;
                    break;
                case PromptAction::panelize_search: {
                    start_traversal(prompt_value, true);
                    break;
                }
                case PromptAction::viewer_find:
                    viewer_search_pattern = viewer.hex()
                                                ? decode_hex_query(prompt_value)
                                                : prompt_value;
                    viewer_search_needle = prompt_value;
                    viewer_search_hex = viewer.hex();
                    prompt_action = PromptAction::none;
                    prompt_value.clear();
                    run_viewer_search(false, false);
                    return;
                case PromptAction::viewer_goto: {
                    auto input = std::string_view(prompt_value);
                    const bool hexadecimal = viewer_goto_mode == ViewerGotoMode::hexadecimal;
                    if (hexadecimal && (input.starts_with("0x") || input.starts_with("0X"))) {
                        input.remove_prefix(2);
                    }
                    std::uint64_t value = 0;
                    const auto parsed = std::from_chars(
                        input.data(), input.data() + input.size(), value, hexadecimal ? 16 : 10
                    );
                    if (parsed.ec != std::errc{} || parsed.ptr != input.data() + input.size()) {
                        throw std::invalid_argument("enter a valid nonnegative number");
                    }
                    if (viewer_goto_mode == ViewerGotoMode::line) {
                        viewer.goto_line(value);
                    } else if (viewer_goto_mode == ViewerGotoMode::percent) {
                        if (value > 100U) {
                            throw std::out_of_range("percentage must be between 0 and 100");
                        }
                        viewer.goto_percent(static_cast<unsigned int>(value));
                        if (value == 100U) {
                            const auto [width, height] = viewer_dimensions();
                            viewer.go_to_end(width, height);
                        }
                    } else {
                        if (value > viewer.size()) {
                            throw std::out_of_range("offset is outside the file");
                        }
                        viewer.goto_offset(value);
                    }
                    break;
                }
                case PromptAction::viewer_path:
                    break;
                case PromptAction::quick_viewer_find:
                    quick_view_search_pattern = quick_viewer.hex()
                                                    ? decode_hex_query(prompt_value)
                                                    : prompt_value;
                    quick_view_search_needle = prompt_value;
                    quick_view_search_hex = quick_viewer.hex();
                    prompt_action = PromptAction::none;
                    prompt_value.clear();
                    run_viewer_search(true, false);
                    return;
                case PromptAction::select_pattern:
                    active().select_pattern(prompt_value, true);
                    status = "Selection updated";
                    break;
                case PromptAction::unselect_pattern:
                    active().select_pattern(prompt_value, false);
                    status = "Selection updated";
                    break;
                case PromptAction::editor_open:
                case PromptAction::editor_new: {
                    auto path = std::filesystem::path(prompt_value);
                    if (path.is_relative()) {
                        path =
                            std::filesystem::path(editor.path())
                                .parent_path() /
                            path;
                    }
                    open_editor_buffer(
                        active().provider(),
                        path.string(),
                        prompt_action ==
                            PromptAction::editor_new
                    );
                    status =
                        prompt_action == PromptAction::editor_open
                            ? "File opened"
                            : "New file";
                    break;
                }
                case PromptAction::editor_save_as:
                    if (!prompt_value.empty()) {
                        request_editor_save_as();
                        return;
                    }
                    break;
                case PromptAction::editor_goto: {
                    std::size_t parsed = 0;
                    const auto line = std::stoull(
                        prompt_value,
                        &parsed
                    );
                    if (parsed != prompt_value.size() || line == 0U) {
                        throw std::runtime_error(
                            "line number must be positive"
                        );
                    }
                    editor.goto_line(line);
                    center_editor();
                    status = "Line " + std::to_string(line);
                    break;
                }
                case PromptAction::hardlink:
                case PromptAction::symlink:
                case PromptAction::relative_symlink: {
                    const auto* entry = active().current();
                    if (entry == nullptr || prompt_value.empty()) {
                        break;
                    }
                    const auto destination =
                        active().provider()->join(
                            active().path(),
                            prompt_value
                        );
                    if (prompt_action == PromptAction::hardlink) {
                        active().provider()->create_hardlink(
                            entry->path,
                            destination
                        );
                    } else {
                        auto target = entry->path;
                        if (prompt_action ==
                            PromptAction::relative_symlink) {
                            const auto relative =
                                std::filesystem::path(entry->path)
                                    .lexically_relative(
                                        std::filesystem::path(
                                            destination
                                        ).parent_path()
                                    );
                            if (relative.empty()) {
                                throw std::runtime_error(
                                    "cannot form relative link target"
                                );
                            }
                            target = relative.string();
                        }
                        active().provider()->create_symlink(
                            target,
                            destination
                        );
                    }
                    active().refresh();
                    status = "Link created";
                    break;
                }
                case PromptAction::quick_cd:
                    active().change_directory(prompt_value);
                    status = "Directory changed";
                    break;
                case PromptAction::chmod: {
                    std::size_t parsed = 0;
                    const auto parsed_mode = std::stoul(
                        prompt_value,
                        &parsed,
                        8
                    );
                    if (parsed != prompt_value.size() ||
                        parsed_mode > 07777U) {
                        throw std::runtime_error("invalid octal mode");
                    }
                    for (const auto& entry : prompt_targets) {
                        prompt_provider->set_permissions(
                            entry.path,
                            static_cast<std::filesystem::perms>(parsed_mode)
                        );
                    }
                    active().refresh();
                    status = "Permissions changed";
                    break;
                }
                case PromptAction::chown: {
                    const auto colon = prompt_value.find(':');
                    const auto owner =
                        colon == std::string::npos
                            ? prompt_value
                            : prompt_value.substr(0, colon);
                    const auto group =
                        colon == std::string::npos
                            ? std::string{}
                            : prompt_value.substr(colon + 1U);
                    for (const auto& entry : prompt_targets) {
                        prompt_provider->set_owner(
                        entry.path,
                        owner.empty()
                            ? std::nullopt
                            : std::optional<std::string>(owner),
                        group.empty()
                            ? std::nullopt
                            : std::optional<std::string>(group)
                        );
                    }
                    active().refresh();
                    status = "Owner changed";
                    break;
                }
                case PromptAction::none:
                    break;
            }
        } catch (const std::exception& error) {
            if (prompt_action == PromptAction::view_file ||
                prompt_action == PromptAction::viewer_find ||
                prompt_action == PromptAction::viewer_goto ||
                prompt_action == PromptAction::quick_viewer_find) {
                prompt_error = error.what();
                prompt_focus = 0;
                return;
            }
            if (mode_before_prompt == Mode::editor) {
                editor_replace_needle.clear();
                editor_replace_text.clear();
                prompt_action = PromptAction::none;
                prompt_value.clear();
                show_editor_error(error);
                return;
            }
            if (mode_before_prompt == Mode::viewer) {
                viewer_message_title = "Viewer error";
                viewer_message = error.what();
                prompt_action = PromptAction::none;
                prompt_value.clear();
                mode = Mode::viewer_message;
                return;
            }
            set_error(error);
        }
        mode = mode_before_prompt;
        prompt_action = PromptAction::none;
        prompt_value.clear();
    }

    ftxui::Element render_pane(
        Pane& pane,
        const bool focused,
        const int panel_width_value,
        ftxui::Box& pane_box,
        std::vector<ftxui::Box>& entry_boxes
    ) {
        using namespace ftxui;
        const auto inner_width = std::max(1, panel_width_value - 2);
        const bool show_details =
            detailed_view && inner_width >= 28;
        const auto name_width = std::max(1, inner_width - 22);
        const auto visible_rows = panel_visible_rows();
        pane.set_viewport_rows(visible_rows);
        Elements rows;
        const auto& entries = pane.entries();
        rows.reserve(visible_rows);
        entry_boxes.clear();
        entry_boxes.resize(entries.size());
        const auto end = std::min(entries.size(), pane.first_row() + visible_rows);
        for (std::size_t index = pane.first_row(); index < end; ++index) {
            const auto& entry = entries[index];
            auto name = type_prefix(entry) + display_text(entry.name);
            auto row =
                show_details
                    ? hbox({
                          text(
                              truncate_text(
                                  (
                                      pane.is_selected(entry)
                                          ? "*"
                                          : " "
                                  ) + name,
                                  static_cast<std::size_t>(name_width)
                              )
                          ) | size(WIDTH, EQUAL, name_width),
                          separator(),
                          text(
                              entry.revision == "mcpp:parent"
                                  ? "UP--DIR"
                                  : format_panel_size(entry.size)
                          ) | size(WIDTH, EQUAL, 8),
                          separator(),
                          text(format_panel_time(entry.modified)) |
                              size(WIDTH, EQUAL, 12),
                      })
                    : hbox({
                          text(pane.is_selected(entry) ? "*" : " "),
                          text(name) | flex,
                      });
            if (index == pane.cursor()) {
                row = gray_selected(
                    std::move(row),
                    pane.is_selected(entry),
                    focused
                );
            } else {
                row = gray_entry(std::move(row), entry);
                if (pane.is_selected(entry)) {
                    row = row | bold;
                }
            }
            rows.push_back(
                std::move(row) | reflect(entry_boxes[index])
            );
        }
        while (rows.size() < visible_rows) {
            rows.push_back(
                show_details
                    ? hbox({
                          text("") | size(WIDTH, EQUAL, name_width),
                          separator(),
                          text("") | size(WIDTH, EQUAL, 8),
                          separator(),
                          text("") | size(WIDTH, EQUAL, 12),
                      })
                    : text("")
            );
        }
        const auto* current = pane.current();
        auto info =
            current == nullptr
                ? std::string{}
                : inner_width < 50
                      ? display_text(current->name) + "  " +
                            format_size(current->size)
                      : permission_text(*current) + " " +
                            display_text(current->name) + "  " +
                            format_size(current->size) +
                            "  " + current->owner + ":" +
                            current->group;
        if (pane.marked_count() > 0U) {
            info =
                std::to_string(pane.marked_count()) +
                " selected, " +
                format_size(pane.marked_size());
        }
        std::string available;
        try {
            const auto disk = pane.provider()->space(pane.path());
            const auto percent =
                disk.capacity == 0U
                    ? 0U
                    : static_cast<unsigned int>(
                          disk.available * 100U / disk.capacity
                      );
            available =
                format_size(disk.available) + " / " +
                format_size(disk.capacity) + " (" +
                std::to_string(percent) + "%)";
        } catch (...) {
        }
        auto title = text(" " + path_label(
            display_text(pane.path()) +
                (pane.panelized() ? " [panelized]" : ""),
            static_cast<std::size_t>(
                std::max(1, panel_width_value - 8)
            )
        ) + " ");
        if (focused) {
            title = title | color(Color::Grey100) | bold;
        } else {
            title = title | color(Color::Grey58);
        }
        auto header =
            show_details
                ? hbox({
                      text(" Name") |
                          size(WIDTH, EQUAL, name_width),
                      separator(),
                      text(" Size") |
                          size(WIDTH, EQUAL, 8),
                      separator(),
                      text("Modify time") |
                          size(WIDTH, EQUAL, 12),
                  })
                : text(" Name");
        header = header | color(Color::Grey93) | bold;
        const auto minimum_info_width = std::min(
            static_cast<int>(info.size()),
            std::max(12, inner_width / 2)
        );
        if (static_cast<int>(available.size()) +
                minimum_info_width + 1 >
            inner_width) {
            available.clear();
        }
        const auto available_width =
            static_cast<int>(available.size());
        const auto info_width =
            std::max(0, inner_width - available_width - 1);
        auto content = vbox({
                   header,
                   vbox(std::move(rows)) | flex,
                   separator(),
                   hbox({
                       text(truncate_text(
                           info,
                           static_cast<std::size_t>(info_width)
                       )) | size(WIDTH, EQUAL, info_width),
                       filler(),
                       text(available) | color(Color::Grey50),
                   }),
               });
        return window(title, content, LIGHT) |
               size(WIDTH, EQUAL, panel_width_value) |
               yflex |
               reflect(pane_box);
    }

    ftxui::Element render_viewer() {
        using namespace ftxui;
        const auto [width, height] = viewer_dimensions();
        const auto rows = viewer.lines(width, height);
        Elements elements;
        viewer_rows.assign(rows.size(), {});
        viewer_rendered_path = viewer.path();
        const auto low_nibble =
            viewer.hex_editing() && viewer.hex_low_nibble() ? 1U : 0U;
        const auto cursor_width =
            viewer.hex_ascii() || viewer.hex_editing() ? 1U : 2U;
        if (viewer.hex() && !rows.empty() && viewer.hex_cursor() >= viewer.offset()) {
            const auto cursor_row = (viewer.hex_cursor() - viewer.offset()) / 16U;
            if (cursor_row < rows.size()) {
                const auto prefix = rows[static_cast<std::size_t>(cursor_row)].find("  ") + 2U;
                const auto byte = static_cast<std::size_t>(
                    (viewer.hex_cursor() - viewer.offset()) % 16U
                );
                const auto cursor = prefix +
                    (viewer.hex_ascii() ? 49U + byte : byte * 3U + low_nibble);
                if (cursor < viewer_hex_first_column) {
                    viewer_hex_first_column = cursor;
                } else if (cursor + cursor_width > viewer_hex_first_column + width) {
                    viewer_hex_first_column = cursor + cursor_width - width;
                }
            }
        }
        for (std::size_t index = 0; index < rows.size(); ++index) {
            const auto& row = rows[index];
            const auto row_offset = viewer.offset() + index * 16U;
            auto& rendered = viewer_rows[index];
            rendered.offset = row_offset;
            rendered.first_column = viewer_hex_first_column;
            if (viewer.hex()) {
                rendered.hex_column = row.find("  ") + 2U;
                rendered.bytes = static_cast<std::size_t>(
                    std::min<std::uint64_t>(16U, viewer.size() - row_offset)
                );
            }
            Element line;
            if (viewer.hex() && viewer.hex_cursor() >= row_offset &&
                viewer.hex_cursor() - row_offset < 16U) {
                const auto byte = static_cast<std::size_t>(viewer.hex_cursor() - row_offset);
                const auto prefix = row.find("  ") + 2U;
                const auto cursor = prefix +
                    (viewer.hex_ascii() ? 49U + byte : byte * 3U + low_nibble);
                const auto first = std::min(viewer_hex_first_column, row.size());
                if (cursor < row.size() && cursor >= first) {
                    const auto end = std::min(row.size(), first + width);
                    line = hbox({
                        text(row.substr(first, cursor - first)),
                        text(row.substr(cursor, cursor_width)) | inverted | bold |
                            focusCursorBlock,
                        text(row.substr(cursor + cursor_width, end - cursor - cursor_width)),
                    });
                }
            }
            if (!line) {
                line = text(
                    viewer.hex()
                        ? row.substr(std::min(viewer_hex_first_column, row.size()), width)
                        : row
                );
            }
            elements.push_back(std::move(line) | reflect(rendered.box));
        }
        const auto key = [this](
                             const std::string& number,
                             const std::string& label,
                             const bool enabled
                         ) {
            return render_function_key(
                static_cast<std::size_t>(std::stoul(number) - 1U),
                label, enabled && mode == Mode::viewer
            );
        };
        const auto percent = viewer.position_percent();
        const auto screen_width =
            static_cast<std::size_t>(
                std::max(1, Terminal::Size().dimx)
            );
        Element header;
        if (screen_width > 40U) {
            const auto status_column = screen_width - 32U;
            std::ostringstream position;
            if (viewer.hex()) {
                position << (viewer.hex_editing() ? "Edit" : "Hex")
                         << (viewer.dirty() ? "*" : " ") << " @"
                         << viewer.hex_cursor();
            } else {
                position << std::setw(9) << viewer.view_end();
            }
            position
                     << "/" << viewer.size() << " ";
            std::ostringstream percentage;
            percentage << std::setw(3) << percent << "%";
            header = hbox({
                text(path_label(
                    display_text(viewer.path()),
                    screen_width - 34U
                )) |
                    size(
                        WIDTH,
                        EQUAL,
                        static_cast<int>(status_column)
                    ),
                text(position.str()) | flex,
                text(percentage.str()) |
                    size(WIDTH, EQUAL, 4),
            });
        } else {
            std::ostringstream percentage;
            percentage << std::setw(3) << percent << "%";
            header = hbox({
                text(path_label(
                    display_text(viewer.path()),
                    screen_width > 5U ? screen_width - 5U : 1U
                )) |
                    flex,
                screen_width > 26U
                    ? text(percentage.str())
                    : text(""),
            });
        }
        return vbox({
            std::move(header) |
                color(Color::Grey11) |
                bgcolor(Color::Grey62),
            vbox(std::move(elements)) | flex,
            hbox({
                text(" "),
                key("1", "Help", true),
                key(
                    "2",
                    viewer.hex()
                        ? viewer.hex_editing() ? "View" : "Edit"
                        : viewer.wrap() ? "UnWrap" : "Wrap",
                    true
                ),
                key("3", "Quit", true),
                key("4", viewer.hex() ? "Text" : "Hex", true),
                key("5", "Goto", true),
                key("6", viewer.hex() ? "Save" : "", viewer.hex() && viewer.dirty()),
                key("7", viewer.hex() ? "HxSrch" : "Search", true),
                key("8", "Raw", false),
                key("9", "Format", false),
                key("10", "Quit", true),
            }),
        });
    }

    ftxui::Element render_viewer_message() {
        using namespace ftxui;
        return vbox({
                   text(viewer_message_title) | bold | center,
                   separator(),
                   paragraph(display_text(viewer_message)),
                   separator(),
                   dialog_button("OK", true, message_button_box) | center,
               }) |
               borderStyled(LIGHT) |
               size(WIDTH, LESS_THAN, 72);
    }

    ftxui::Element render_viewer_close_dialog() {
        using namespace ftxui;
        return vbox({
                   text("Save modified bytes?") | bold | center,
                   separator(),
                   text(path_label(
                       display_text(viewer.path()),
                       static_cast<std::size_t>(std::max(1, Terminal::Size().dimx - 6))
                   )),
                   separator(),
                   hbox({
                       dialog_button("Save", viewer_choice == 0U, viewer_choice_boxes[0]),
                       text(" "),
                       dialog_button("Discard", viewer_choice == 1U, viewer_choice_boxes[1]),
                       text(" "),
                       dialog_button("Cancel", viewer_choice == 2U, viewer_choice_boxes[2]),
                   }) | center,
               }) |
               borderStyled(LIGHT) |
               size(WIDTH, LESS_THAN, 72);
    }

    ftxui::Element render_viewer_wrap_dialog() {
        using namespace ftxui;
        return vbox({
                   text("Search reached the end") | bold | center,
                   separator(),
                   text("Continue from the beginning?"),
                   separator(),
                   hbox({
                       dialog_button("Continue", viewer_choice == 0U, viewer_wrap_boxes[0]),
                       text("  "),
                       dialog_button("Cancel", viewer_choice == 1U, viewer_wrap_boxes[1]),
                   }) | center,
               }) |
               borderStyled(LIGHT) |
               size(WIDTH, LESS_THAN, 52);
    }
    static ftxui::Element style_syntax(
        ftxui::Element element,
        const SyntaxStyle style
    ) {
        using namespace ftxui;
        switch (style) {
            case SyntaxStyle::keyword:
                return std::move(element) |
                       color(Color::RGB(86, 156, 214)) | bold;
            case SyntaxStyle::type:
                return std::move(element) |
                       color(Color::RGB(78, 201, 176));
            case SyntaxStyle::string_literal:
                return std::move(element) |
                       color(Color::RGB(206, 145, 120));
            case SyntaxStyle::comment:
                return std::move(element) |
                       color(Color::RGB(106, 153, 85)) | italic;
            case SyntaxStyle::number:
                return std::move(element) |
                       color(Color::RGB(181, 206, 168));
            case SyntaxStyle::preprocessor:
                return std::move(element) |
                       color(Color::RGB(197, 134, 192)) | bold;
            case SyntaxStyle::normal:
                return element;
        }
        return element;
    }

    ftxui::Element render_editor_line(
        const std::size_t index,
        const std::size_t first_column,
        const std::size_t viewport_width
    ) {
        using namespace ftxui;
        const auto& line = editor.lines()[index];
        auto selected_begin = line.size();
        auto selected_end = std::size_t{0};
        if (const auto selected = editor.selection_on_line(index)) {
            selected_begin = selected->first;
            selected_end = selected->second;
        }
        auto trailing_whitespace = line.size();
        while (trailing_whitespace > 0U &&
               (
                   line[trailing_whitespace - 1U] == ' ' ||
                   line[trailing_whitespace - 1U] == '\t'
               )) {
            --trailing_whitespace;
        }

        const auto visible_begin =
            editor_offset_for_width(
                line,
                first_column,
                editor.tab_width()
            );
        auto visible_end = editor_offset_for_width(
            line,
            first_column + viewport_width,
            editor.tab_width()
        );
        if (visible_end == visible_begin &&
            visible_end < line.size()) {
            visible_end = next_utf8(line, visible_end);
        }
        std::vector<std::size_t> boundaries{
            visible_begin,
            visible_end,
            std::clamp(
                selected_begin,
                visible_begin,
                visible_end
            ),
            std::clamp(
                selected_end,
                visible_begin,
                visible_end
            ),
            std::clamp(
                trailing_whitespace,
                visible_begin,
                visible_end
            ),
        };
        if (index == editor.row() &&
            editor.column() >= visible_begin &&
            editor.column() <= visible_end) {
            boundaries.push_back(
                std::min(editor.column(), visible_end)
            );
            boundaries.push_back(
                std::min(
                    next_utf8(line, editor.column()),
                    visible_end
                )
            );
        }
        if (editor_syntax && index < syntax_spans.size()) {
            for (const auto& span : syntax_spans[index]) {
                if (span.end <= visible_begin) {
                    continue;
                }
                if (span.begin >= visible_end) {
                    break;
                }
                boundaries.push_back(
                    std::max(span.begin, visible_begin)
                );
                boundaries.push_back(
                    std::min(span.end, visible_end)
                );
            }
        }
        std::sort(boundaries.begin(), boundaries.end());
        boundaries.erase(
            std::unique(boundaries.begin(), boundaries.end()),
            boundaries.end()
        );

        Elements segments;
        auto display_begin =
            editor_display_width(
                line,
                visible_begin,
                editor.tab_width()
            );
        std::size_t syntax_index = 0;
        if (editor_syntax && index < syntax_spans.size()) {
            while (syntax_index < syntax_spans[index].size() &&
                   syntax_spans[index][syntax_index].end <=
                       visible_begin) {
                ++syntax_index;
            }
        }
        for (std::size_t boundary = 0;
             boundary + 1U < boundaries.size();
             ++boundary) {
            const auto begin = boundaries[boundary];
            const auto end = boundaries[boundary + 1U];
            if (begin < visible_begin || begin == end) {
                continue;
            }
            auto displayed = editor_display_text(
                std::string_view(line).substr(begin, end - begin),
                display_begin,
                editor.tab_width(),
                editor_show_whitespace,
                editor_show_whitespace &&
                    begin >= trailing_whitespace
            );
            const auto display_width = static_cast<std::size_t>(
                std::max(0, string_width(displayed))
            );
            if (begin == visible_begin &&
                line[begin] == '\t' &&
                display_begin < first_column) {
                displayed.erase(
                    0,
                    std::min(
                        first_column - display_begin,
                        displayed.size()
                    )
                );
            }
            const bool cursor =
                index == editor.row() &&
                begin == editor.column();
            const bool selected =
                begin >= selected_begin && begin < selected_end;
            if (cursor && line[begin] == '\t') {
                segments.push_back(
                    focusCursorBlock(
                        text(
                            editor_show_whitespace &&
                                    !displayed.empty()
                                ? displayed.substr(0U, 1U)
                                : " "
                        ) |
                        color(Color::Grey0) |
                        bgcolor(Color::Grey85) |
                        bold
                    )
                );
                if (displayed.size() > 1U) {
                    auto remainder = text(displayed.substr(1U));
                    if (selected) {
                        remainder = remainder |
                                    color(Color::Grey0) |
                                    bgcolor(Color::Grey85);
                    }
                    segments.push_back(std::move(remainder));
                }
                display_begin += display_width;
                continue;
            }
            auto segment = text(displayed);
            if (cursor) {
                segment = focusCursorBlock(
                    segment |
                    color(Color::Grey0) |
                    bgcolor(Color::Grey85) |
                    bold
                );
            } else if (selected) {
                segment = segment |
                          color(Color::Grey0) |
                          bgcolor(Color::Grey85);
            } else if (editor_syntax && index < syntax_spans.size()) {
                while (syntax_index < syntax_spans[index].size() &&
                       syntax_spans[index][syntax_index].end <= begin) {
                    ++syntax_index;
                }
                if (syntax_index < syntax_spans[index].size() &&
                    begin >=
                        syntax_spans[index][syntax_index].begin &&
                    begin <
                        syntax_spans[index][syntax_index].end) {
                    segment = style_syntax(
                        std::move(segment),
                        syntax_spans[index][syntax_index].style
                    );
                }
            }
            segments.push_back(std::move(segment));
            display_begin += display_width;
        }
        if (editor.column_selection()) {
            const auto selected = editor.selection();
            if (selected &&
                index >= selected->first.row &&
                index <= selected->second.row) {
                const auto first_display = editor_display_width(
                    editor.lines()[selected->first.row],
                    selected->first.column,
                    editor.tab_width()
                );
                const auto second_display = editor_display_width(
                    editor.lines()[selected->second.row],
                    selected->second.column,
                    editor.tab_width()
                );
                const auto selection_left =
                    std::min(first_display, second_display);
                const auto selection_right =
                    std::max(first_display, second_display);
                const auto line_width =
                    editor_display_width(
                        line,
                        line.size(),
                        editor.tab_width()
                    );
                if (line_width < selection_left) {
                    segments.push_back(
                        text(std::string(
                            selection_left - line_width,
                            ' '
                        ))
                    );
                }
                const auto virtual_begin =
                    std::max(line_width, selection_left);
                if (selection_right > virtual_begin) {
                    segments.push_back(
                        text(std::string(
                            selection_right - virtual_begin,
                            ' '
                        )) |
                        color(Color::Grey0) |
                        bgcolor(Color::Grey85)
                    );
                }
            }
        }
        if (index == editor.row() &&
            editor.column() == line.size() &&
            visible_end == line.size()) {
            segments.push_back(
                focusCursorBlock(
                    text(" ") |
                    color(Color::Grey0) |
                    bgcolor(Color::Grey85) |
                    bold
                )
            );
        }
        if (segments.empty()) {
            segments.push_back(text(""));
        }
        auto content = hbox(std::move(segments));
        if (!editor_line_numbers) {
            return content;
        }
        return hbox({
            text(std::to_string(index + 1U)) |
                color(Color::Grey50) |
                size(WIDTH, EQUAL, 6),
            std::move(content),
        });
    }

    ftxui::Element render_editor() {
        using namespace ftxui;
        update_editor_view();
        const auto terminal_size = Terminal::Size();
        const auto visible_rows = editor_visible_rows();
        const auto& lines = editor.lines();
        if (editor_syntax &&
            (
                syntax_change_id != editor.change_id() ||
                syntax_path != editor.path() ||
                syntax_spans.size() != lines.size()
            )) {
            syntax_spans = syntax_highlighter.highlight(
                editor.path(),
                lines
            );
            syntax_change_id = editor.change_id();
            syntax_path = editor.path();
        } else if (!editor_syntax && !syntax_spans.empty()) {
            syntax_spans.clear();
        }
        const auto first = editor.first_row;
        const auto end = std::min(lines.size(), first + visible_rows);
        const auto& current_line = lines[editor.row()];
        const auto cursor_width = editor_display_width(
            current_line,
            editor.column(),
            editor.tab_width()
        );
        const auto content_width = static_cast<std::size_t>(
            std::max(
                1,
                terminal_size.dimx -
                    (editor_line_numbers ? 6 : 0)
            )
        );
        const auto first_column = editor.first_column;
        editor_rendered_first_row = first;
        editor_line_boxes.clear();
        editor_line_boxes.resize(end - first);
        editor_line_first_columns.clear();
        editor_line_first_columns.reserve(end - first);
        Elements elements;
        for (std::size_t index = first; index < end; ++index) {
            const auto visible_begin = editor_offset_for_width(
                lines[index],
                first_column,
                editor.tab_width()
            );
            const auto aligned_column = editor_display_width(
                lines[index],
                visible_begin,
                editor.tab_width()
            );
            editor_line_first_columns.push_back(
                visible_begin < lines[index].size() &&
                        lines[index][visible_begin] == '\t' &&
                        aligned_column < first_column
                    ? first_column
                    : aligned_column
            );
            elements.push_back(
                render_editor_line(
                    index,
                    first_column,
                    content_width
                ) |
                reflect(editor_line_boxes[index - first])
            );
        }

        static const std::array<std::string, 7> menu_names{
            "File     ", "Edit     ", "Search     ", "Command     ",
            "Format     ", "Window     ", "Options"
        };
        static constexpr std::array<std::size_t, 7> mnemonic_indices{
            0, 0, 0, 0, 3, 0, 0
        };
        Elements menu;
        for (std::size_t index = 0; index < menu_names.size(); ++index) {
            auto item = mnemonic_label(
                menu_names[index],
                mnemonic_indices[index]
            );
            if (index == editor_menu_category) {
                item = item |
                       color(Color::Grey100) |
                       bgcolor(Color::Grey23) |
                       bold;
            }
            menu.push_back(
                std::move(item) | reflect(editor_menu_category_boxes[index])
            );
        }
        const bool at_eof = editor.cursor_offset() == editor.byte_size();
        const auto character =
            editor.column() == current_line.size() && !at_eof
                ? 10U
                : utf8_codepoint(current_line, editor.column());
        std::ostringstream editor_status;
        editor_status
            << " L:[" << editor.row() + 1U
            << " C:" << cursor_width + 1U
            << "/" << editor.lines().size() << "]"
            << " *(" << editor.selection_size()
            << "/" << editor.cursor_offset()
            << "/" << editor.byte_size() << "b)";
        if (at_eof) {
            editor_status << " <EOF>";
        } else {
            editor_status
                << " " << std::setfill('0') << std::setw(4)
                << character
                << " U+" << std::uppercase << std::hex
                << std::setw(4) << character << std::dec;
        }
        const auto block_flag =
            editor.selection().has_value()
               ? editor.column_selection() ? 'C' : 'B'
               : '-';
        const std::string editor_flags{
            ' ',
            '[',
            block_flag,
            editor.dirty() ? 'M' : '-',
            '-',
            editor.overwrite() ? 'O' : '-',
            ']',
            ' ',
        };
        auto position_status = editor_status.str();
        if (static_cast<int>(editor_flags.size() + position_status.size()) + 18 >
            terminal_size.dimx) {
            position_status =
                " " + std::to_string(editor.row() + 1U) + ":" +
                std::to_string(cursor_width + 1U) + "/" +
                std::to_string(editor.lines().size());
        }
        const auto filename_width = static_cast<std::size_t>(std::max(
            1, terminal_size.dimx -
                   static_cast<int>(editor_flags.size() + position_status.size())
        ));
        auto header =
            mode == Mode::editor_menu
                ? hbox(std::move(menu))
                : hbox({
                      text(path_label(
                          display_text(editor.path()), filename_width
                      )) | bold | flex,
                      text(editor_flags),
                      text(position_status),
                  });
        header = gray_menu(std::move(header));
        const auto key = [this](
                             const std::string& number,
                             const std::string& label
                         ) {
            return render_function_key(
                static_cast<std::size_t>(std::stoul(number) - 1U),
                label, mode == Mode::editor
            );
        };
        return vbox({
            header,
            vbox(std::move(elements)) | flex,
            hbox({
                text(" "),
                key("1", "Help"),
                key("2", "Save"),
                key("3", "Mark"),
                key("4", "Replac"),
                key("5", "Copy"),
                key("6", "Move"),
                key("7", "Search"),
                key("8", "Delete"),
                key("9", "PullDn"),
                key("10", "Quit"),
            }),
        });
    }

    static const std::array<
        std::vector<MenuRow>,
        7
    >& editor_menu_items() {
        static const std::array<std::vector<MenuRow>, 7> menus{{
            {
                {"Open file...", 0},
                {"New  C-n", 1},
                {"Close", 2},
                {"History...  M-E", 5},
                {"", -2},
                {"Save  F2", 3},
                {"Save as...  F12", 4},
                {"", -2},
                {"Insert file...  F15", -1},
                {"Copy to file...  C-f", -1},
                {"", -2},
                {"User menu...  F11", -1},
                {"", -2},
                {"About...", -1},
                {"", -2},
                {"Quit  F10", 6},
            },
            {
                {"Undo  C-u", 0},
                {"Redo  M-r", 1},
                {"", -2},
                {"Toggle insert/overwrite  Ins", 2},
                {"", -2},
                {"Mark  F3", 3},
                {"Mark columns  F13", 4},
                {"Mark all", 5},
                {"Unmark", 6},
                {"", -2},
                {"Copy  F5", 7},
                {"Move  F6", 8},
                {"Delete  F8", 9},
                {"", -2},
                {"Copy to clipfile  C-Ins", 10},
                {"Cut to clipfile  S-Del", 11},
                {"Paste from clipfile  S-Ins", 12},
                {"", -2},
                {"Beginning  C-Home", 13},
                {"End  C-End", 14},
            },
            {
                {"Search...  F7", 0},
                {"Search again  F17", 1},
                {"Replace...  F4", 2},
                {"", -2},
                {"Toggle bookmark  M-k", -1},
                {"Next bookmark  M-j", -1},
                {"Previous bookmark  M-i", -1},
                {"Flush bookmarks  M-o", -1},
            },
            {
                {"Go to line...  M-l", 0},
                {"Toggle line numbers  M-n", 2},
                {"Go to matching bracket  M-b", 1},
                {"Toggle syntax highlighting  C-s", 3},
                {"", -2},
                {"Find declaration", -1},
                {"Back from declaration", -1},
                {"Forward to declaration", -1},
                {"", -2},
                {"Encoding...", -1},
                {"", -2},
                {"Refresh screen", -1},
                {"", -2},
                {"Start/Stop record macro", -1},
                {"Delete macro...", -1},
                {"Record/Repeat actions", -1},
                {"", -2},
                {"Spell check", -1},
                {"Check word", -1},
                {"Change spelling language...", -1},
                {"", -2},
                {"Mail...", -1},
            },
            {
                {"Insert literal...  C-q", -1},
                {"Insert date/time", -1},
                {"", -2},
                {"Format paragraph  M-p", -1},
                {"Sort...  M-t", -1},
                {"Paste output of...  M-u", -1},
                {"External formatter", -1},
            },
            {
                {"Move window", -1},
                {"Resize window", -1},
                {"Toggle fullscreen", -1},
                {"", -2},
                {"Next buffer", 0},
                {"Previous buffer", 1},
                {"Buffer list...", 2},
            },
            {
                {"General...", 0},
                {"Save mode...", -1},
                {"Learn keys...", -1},
                {"Syntax highlighting...", -1},
                {"", -2},
                {"Syntax file", -1},
                {"Menu file", -1},
                {"", -2},
                {"Save setup", 1},
            },
        }};
        return menus;
    }

    ftxui::Element render_editor_menu() {
        using namespace ftxui;
        Elements rows;
        const auto& items = editor_menu_items()[editor_menu_category];
        editor_menu_item_boxes.assign(items.size(), {});
        for (std::size_t index = 0; index < items.size(); ++index) {
            if (items[index].action == -2) {
                rows.push_back(
                    separator() | reflect(editor_menu_item_boxes[index])
                );
                continue;
            }
            auto row = text(" " + items[index].label);
            if (items[index].action < 0) {
                row = row | color(Color::Grey66) | dim;
            }
            if (index == editor_menu_item) {
                row = row | inverted | focus;
            }
            rows.push_back(
                std::move(row) | reflect(editor_menu_item_boxes[index])
            );
        }
        return gray_menu(
            (
                vbox(std::move(rows)) |
                yframe |
                size(
                    HEIGHT,
                    LESS_THAN,
                    std::max(1, Terminal::Size().dimy - 3)
                )
            ) |
            borderStyled(LIGHT)
        );
    }

    ftxui::Element render_editor_save_dialog() {
        using namespace ftxui;
        const auto button = [&](const std::string& label, const std::size_t index) {
            auto element = text("[ " + label + " ]");
            if (index == editor_save_choice) {
                element = element | inverted | focus;
            }
            return element | reflect(editor_save_boxes[index]);
        };
        return vbox({
                   text("Save file") | bold | center,
                   separator(),
                   text("\"" + path_label(
                       display_text(editor.path()),
                       static_cast<std::size_t>(std::max(
                           1, std::min(72, Terminal::Size().dimx) - 6
                       ))
                   ) + "\""),
                   separator(),
                   hbox({
                       filler(),
                       button("Save", 0),
                       text("  "),
                       button("Cancel", 1),
                       filler(),
                   }),
               }) |
               borderStyled(LIGHT) |
               size(WIDTH, LESS_THAN, 72);
    }

    ftxui::Element render_editor_close_dialog() {
        using namespace ftxui;
        const auto button = [&](const std::string& label,
                                const std::size_t index) {
            return dialog_button(
                label, index == editor_close_choice, editor_close_boxes[index]
            );
        };
        return vbox({
                   text("Close file") | bold | center,
                   separator(),
                   text("File " + path_label(
                       display_text(editor_quit_requested
                                        ? editor_quit_buffer().path()
                                        : editor.path()),
                       static_cast<std::size_t>(std::max(
                           1, std::min(72, Terminal::Size().dimx) - 9
                       ))
                   )),
                   text("was modified. Save before close?") | center,
                   separator(),
                   hbox({
                       filler(),
                       button("Yes", 0),
                       text("  "),
                       button("No", 1),
                       text("  "),
                       button("Cancel", 2),
                       filler(),
                   }),
               }) |
               borderStyled(LIGHT) |
               size(WIDTH, LESS_THAN, 72);
    }

    ftxui::Element render_editor_link_dialog() {
        using namespace ftxui;
        const auto& path = editor_save_purpose == EditorSavePurpose::save_as
                               ? editor_save_path
                               : editor_save_buffer->path();
        return vbox({
                   text("File has multiple hardlinks") | bold | center,
                   text(path_label(
                       display_text(path),
                       static_cast<std::size_t>(std::max(
                           1, std::min(66, Terminal::Size().dimx) - 4
                       ))
                   )),
                   separator(),
                   paragraph(
                       "Detach saves this name atomically. Preserve links "
                       "writes every linked name in place, not atomically."
                   ),
                   separator(),
                   hbox({
                       dialog_button("Detach", editor_link_choice == 0U, editor_link_boxes[0]),
                       text(" "),
                       dialog_button("Preserve links", editor_link_choice == 1U, editor_link_boxes[1]),
                   }) | center,
                   dialog_button("Cancel", editor_link_choice == 2U, editor_link_boxes[2]) | center,
               }) |
               borderStyled(LIGHT) |
               size(WIDTH, LESS_THAN, std::min(66, Terminal::Size().dimx));
    }

    ftxui::Element render_editor_overwrite_dialog() {
        using namespace ftxui;
        return vbox({
                   text("Replace existing file?") | bold | center,
                   separator(),
                   text(path_label(
                       display_text(editor_save_path),
                       static_cast<std::size_t>(std::max(
                           1, std::min(72, Terminal::Size().dimx) - 4
                       ))
                   )),
                   separator(),
                   hbox({
                       dialog_button("Replace", editor_overwrite_choice == 0U, editor_overwrite_boxes[0]),
                       text("  "),
                       dialog_button("Cancel", editor_overwrite_choice == 1U, editor_overwrite_boxes[1]),
                   }) | center,
               }) |
               borderStyled(LIGHT) |
               size(WIDTH, LESS_THAN, 72);
    }

    ftxui::Element render_editor_help() const {
        using namespace ftxui;
        return vbox({
                   text("mcpp editor") | bold | center,
                   separator(),
                   text("F2        Save"),
                   text("F3        Start or finish marking"),
                   text("F4        Replace next match"),
                   text("F5/F6     Copy/move marked block"),
                   text("F7        Search"),
                   text("F8        Delete marked block or current line"),
                   text("F9        Pull-down menu"),
                   text("F10/Esc   Quit editor (all buffers)"),
                   text("F12       Save as"),
                   text("Ctrl-U/M-R Undo/redo"),
                   text("Alt-L      Go to line"),
                   text("Insert     Insert/overwrite"),
                   text("Shift-arrow Mark text"),
                   text("C-Ins/S-Del Copy/cut block"),
                   text("S-Ins      Paste clipboard"),
                   text("Ctrl-S     Syntax highlighting"),
                   text("Alt-N      Line numbers"),
                   separator(),
                   text("Press F1 or Esc") | center,
               }) |
               borderStyled(LIGHT);
    }

    ftxui::Element render_editor_error() {
        using namespace ftxui;
        return vbox({
                   text(editor_message_title) | bold | center,
                   separator(),
                   paragraph(display_text(editor_error_message)),
                   separator(),
                   dialog_button("OK", true, message_button_box) | center,
               }) |
               borderStyled(LIGHT) |
               size(WIDTH, LESS_THAN, 72) |
               color(Color::Grey100) |
               bgcolor(Color::Grey19);
    }

    ftxui::Element render_editor_search_dialog() {
        using namespace ftxui;
        const bool replace = mode == Mode::editor_replace_dialog;
        const auto focused = [&](Element element,
                                 const std::size_t index) {
            if (editor_dialog_focus == index) {
                element = element | inverted | focus;
            }
            return element | reflect(editor_search_boxes[index]);
        };
        const auto checked = [](const bool value) {
            return std::string(value ? "[x] " : "[ ] ");
        };
        static const std::array<std::string, 4> mode_names{
            "Normal", "Regular expression", "Wildcard", "Hexadecimal"
        };
        editor_search_boxes = {};
        Elements rows{
            text("Search for:"),
            render_input(
                editor_dialog_query, editor_query_input, editor_dialog_focus == 0U
            ) | reflect(editor_search_boxes[0]),
        };
        if (replace) {
            rows.push_back(text("Replace with:"));
            rows.push_back(
                render_input(
                    editor_dialog_replacement,
                    editor_replacement_input,
                    editor_dialog_focus == 1U
                ) | reflect(editor_search_boxes[1])
            );
        }
        rows.push_back(separator());
        rows.push_back(
            focused(
                text(
                    "Mode: " +
                    mode_names[static_cast<std::size_t>(
                        editor_dialog_options.mode
                    )] +
                    "  (Left/Right changes)"
                ),
                2
            )
        );
        rows.push_back(
            focused(
                text(
                    checked(editor_dialog_options.case_sensitive) +
                    "Case sensitive"
                ),
                3
            )
        );
        rows.push_back(
            focused(
                text(
                    checked(editor_dialog_options.backwards) +
                    "Backwards"
                ),
                4
            )
        );
        rows.push_back(
            focused(
                text(
                    checked(editor_dialog_in_selection) +
                    "In selection"
                ),
                5
            )
        );
        rows.push_back(
            focused(
                text(
                    checked(editor_dialog_options.whole_words) +
                    "Whole words"
                ),
                6
            )
        );
        Element buttons;
        if (replace) {
            buttons = hbox({
                    focused(text(" [ OK ] "), 7),
                    text("  "),
                    focused(text(" [ Cancel ] "), 8),
                }) | center;
        } else {
            buttons = hbox({
                    focused(text(" [ OK ] "), 7),
                    text("  "),
                    focused(text(" [ Find all ] "), 8),
                    text("  "),
                    focused(text(" [ Cancel ] "), 9),
                }) | center;
        }
        Elements form{
            text(replace ? "Replace" : "Search") | bold | center,
            separator(),
            vbox(std::move(rows)) | yframe | flex | reflect(editor_search_viewport),
            separator(),
            std::move(buttons),
        };
        if (Terminal::Size().dimy < 20) {
            form.push_back(text("Wheel/PgUp/PgDn: more options") | dim | center);
        }
        return vbox(std::move(form)) |
               size(HEIGHT, LESS_THAN, std::max(5, Terminal::Size().dimy - 2)) |
               borderStyled(LIGHT) |
               size(WIDTH, EQUAL, std::min(72, Terminal::Size().dimx));
    }

    ftxui::Element render_editor_options_dialog() {
        using namespace ftxui;
        const auto focused = [&](Element element,
                                 const std::size_t index) {
            if (editor_options_focus == index) {
                element = element | inverted | focus;
            }
            return element | reflect(editor_options_boxes[index]);
        };
        const auto checked = [](const bool value) {
            return std::string(value ? "[x] " : "[ ] ");
        };
        Elements rows{
            text("Editor options") | bold | center,
            separator(),
            text("Tabulation") | bold,
            render_input(
                editor_options_tab_width, editor_tab_input,
                editor_options_focus == 0U, "Tab spacing:        "
            ) | reflect(editor_options_boxes[0]),
            render_input(
                editor_options_indent_width, editor_indent_input,
                editor_options_focus == 1U, "Indent spacing:     "
            ) | reflect(editor_options_boxes[1]),
            separator(),
            text("Other options") | bold,
            focused(
                text(
                    checked(editor_options_auto_indent) +
                    "Return does autoindent"
                ),
                2
            ),
            focused(
                text(
                    checked(editor_options_show_whitespace) +
                    "Show whitespace"
                ),
                3
            ),
        };
        if (!editor_options_error.empty()) {
            rows.push_back(
                text(editor_options_error) |
                color(Color::Grey100) |
                bgcolor(Color::Grey23)
            );
        }
        rows.push_back(separator());
        rows.push_back(
            hbox({
                focused(text(" [ OK ] "), 4),
                text("  "),
                focused(text(" [ Cancel ] "), 5),
            }) | center
        );
        return vbox(std::move(rows)) |
               borderStyled(LIGHT) |
               size(WIDTH, EQUAL, 52);
    }

    ftxui::Element render_editor_list(const bool history) {
        using namespace ftxui;
        Elements rows;
        editor_list_boxes.assign(
            history ? editor_history.size() : editor_buffers.size() - editor_session_base + 1U, {}
        );
        if (history) {
            for (std::size_t index = 0;
                 index < editor_history.size();
                 ++index) {
                auto row = text(
                    std::to_string(index + 1U) + " " +
                    path_label(
                        display_text(editor_history[index]),
                        static_cast<std::size_t>(std::max(
                            1, std::min(72, Terminal::Size().dimx) - 8
                        ))
                    )
                );
                if (index == editor_list_cursor) {
                    row = row | inverted | focus;
                }
                rows.push_back(std::move(row) | reflect(editor_list_boxes[index]));
            }
        } else {
            std::vector<std::string> paths{editor.path()};
            for (auto index = editor_session_base; index < editor_buffers.size(); ++index) {
                paths.push_back(editor_buffers[index].path());
            }
            for (std::size_t index = 0;
                 index < paths.size();
                 ++index) {
                auto row = text(
                    (
                        index == 0U ? "* " : "  "
                    ) +
                    path_label(
                        display_text(paths[index]),
                        static_cast<std::size_t>(std::max(
                            1, std::min(72, Terminal::Size().dimx) - 6
                        ))
                    )
                );
                if (index == editor_list_cursor) {
                    row = row | inverted | focus;
                }
                rows.push_back(std::move(row) | reflect(editor_list_boxes[index]));
            }
        }
        if (rows.empty()) {
            rows.push_back(text("<empty>") | dim);
        }
        return vbox({
                   text(
                       history ? "Editor history" : "Buffer list"
                   ) |
                       bold |
                       center,
                   separator(),
                   vbox(std::move(rows)) | yframe | flex | reflect(editor_list_box),
                   separator(),
                   text("Enter selects; Esc closes") | center,
               }) |
               borderStyled(LIGHT) |
               size(WIDTH, EQUAL, 72) |
               size(HEIGHT, LESS_THAN, std::max(5, std::min(20, Terminal::Size().dimy - 2)));
    }

    ftxui::Element render_editor_replace_confirm() {
        using namespace ftxui;
        const auto button = [&](const std::string& label,
                                const std::size_t index) {
            return dialog_button(
                label, index == editor_replace_choice, editor_replace_boxes[index]
            );
        };
        auto buttons = Terminal::Size().dimx < 72
                           ? vbox({
                                 hbox({button("Replace", 0), text(" "),
                                       button("Skip", 1), text(" "),
                                       button("All", 2)}) | center,
                                 hbox({button("One", 3), text(" "),
                                       button("Cancel", 4)}) | center,
                             })
                           : hbox({
                                 button("Replace", 0), text(" "),
                                 button("Skip", 1), text(" "),
                                 button("All", 2), text(" "),
                                 button("One", 3), text(" "),
                                 button("Cancel", 4),
                             }) | center;
        return vbox({
                   text("Replace") | bold | center,
                   separator(),
                   paragraph(
                       "\"" + display_text(editor_replace_needle) +
                       "\" with \"" +
                       display_text(editor_replace_text) + "\"?"
                   ),
                   separator(),
                   std::move(buttons),
               }) |
               borderStyled(LIGHT) |
               size(WIDTH, LESS_THAN, 78);
    }

    ftxui::Element render_help() const {
        using namespace ftxui;
        return vbox({
                   text("mcpp help") | bold | center,
                   separator(),
                   text("Tab       Switch pane"),
                   text("Arrows    Move selection"),
                   text("Enter     Enter directory"),
                   text("Backspace Parent directory"),
                   text("Insert    Toggle selection"),
                   text("F2        User menu"),
                   text("F3        View"),
                   text("F4        Edit"),
                   text("F5/F6     Copy/Rename or move"),
                   text("F7/F8     Mkdir/Delete"),
                   text("F9/F10    Pull-down menu/Quit"),
                   separator(),
                   text("Press Esc or F1"),
               }) |
               borderStyled(LIGHT) | size(WIDTH, EQUAL, 50);
    }

    ftxui::Element render_menu() const {
        using namespace ftxui;
        Elements rows{
            text("User menu") | bold | center,
            separator(),
        };
        for (const auto& item : user_menu) {
            rows.push_back(
                text(
                    std::string(1, item.key) + "  " + item.label
                )
            );
        }
        if (user_menu.empty()) {
            rows.push_back(text("<no menu entries>") | dim);
        }
        rows.push_back(separator());
        rows.push_back(text("Esc closes menu"));
        return vbox(std::move(rows)) |
               borderStyled(LIGHT) | size(WIDTH, EQUAL, 50);
    }

    static const std::array<
        std::vector<MenuRow>,
        5
    >& pulldown_items() {
        static const std::array<std::vector<MenuRow>, 5> items{{
            {
                {"File listing", 12},
                {"Quick view         C-x q", 10},
                {"Info               C-x i", 11},
                {"Tree", 14},
                {"", -2},
                {"Listing format...", -1},
                {"Sort order...", -1},
                {"Filter...", 13},
                {"Encoding...        M-e", -1},
                {"", -2},
                {"FTP link...", -1},
                {"Shell link...", -1},
                {"SFTP link...", -1},
                {"Panelize by name...", 15},
                {"", -2},
                {"Rescan             C-r", 7},
            },
            {
                {"View              F3", 0},
                {"View file...", 18},
                {"Filtered view     M-!", -1},
                {"Edit              F4", 1},
                {"Copy              F5", 2},
                {"Chmod             C-x c", 8},
                {"Link              C-x l", 11},
                {"Symlink           C-x s", 12},
                {"Relative symlink  C-x v", 13},
                {"Edit symlink      C-x C-s", -1},
                {"Chown             C-x o", 9},
                {"Advanced chown", -1},
                {"Chattr            C-x e", -1},
                {"Rename/Move       F6", 4},
                {"Mkdir             F7", 5},
                {"Delete            F8", 6},
                {"Quick cd          M-c", 14},
                {"", -2},
                {"Select group      +", 15},
                {"Unselect group    -", 16},
                {"Invert selection  *", 17},
                {"", -2},
                {"Exit              F10", 10},
            },
            {
                {"User menu                     F2", 8},
                {"Directory tree", -1},
                {"Find file                     M-?", 0},
                {"Swap panels                   C-u", 5},
                {"Switch panels on/off          C-o", 3},
                {"Compare directories           C-x d", 1},
                {"Compare files                 C-x C-d", -1},
                {"External panelize             C-x !", -1},
                {"Show directory sizes          C-Space", -1},
                {"", -2},
                {"Command history               M-h", -1},
                {"Viewed/edited files history   M-E", -1},
                {"Directory hotlist             C-\\", 7},
                {"Active VFS list               C-x a", -1},
                {"Background jobs               C-x j", 10},
                {"Screen list                   M-`", -1},
                {"", -2},
                {"Undelete files (ext2fs only)", -1},
                {"", -2},
                {"Edit extension file", -1},
                {"Edit menu file", -1},
                {"Edit highlighting group file", -1},
            },
            {
                {"Configuration...", -1},
                {"Layout...", -1},
                {"Panel options...", -1},
                {"Confirmation...", -1},
                {"Appearance...", -1},
                {"Display bits...", -1},
                {"Learn keys...", -1},
                {"Virtual FS...", -1},
                {"", -2},
                {"Save setup", 3},
            },
            {
                {"File listing", 12},
                {"Quick view         C-x q", 10},
                {"Info               C-x i", 11},
                {"Tree", 14},
                {"", -2},
                {"Listing format...", -1},
                {"Sort order...", -1},
                {"Filter...", 13},
                {"Encoding...        M-e", -1},
                {"", -2},
                {"FTP link...", -1},
                {"Shell link...", -1},
                {"SFTP link...", -1},
                {"Panelize by name...", 15},
                {"", -2},
                {"Rescan             C-r", 7},
            },
        }};
        return items;
    }

    ftxui::Element render_pulldown() {
        using namespace ftxui;
        static constexpr std::array<int, 5> widths{
            27, 28, 40, 21, 27
        };
        Elements rows;
        const auto& items = pulldown_items()[pulldown_category];
        pulldown_item_boxes.clear();
        pulldown_item_boxes.resize(items.size());
        for (std::size_t index = 0; index < items.size(); ++index) {
            if (items[index].action == -2) {
                rows.push_back(separator());
                continue;
            }
            auto row = text(" " + items[index].label + " ");
            if (items[index].action < 0) {
                row = row | color(Color::Grey66) | dim;
            }
            if (index == pulldown_item) {
                row = row | inverted | focus;
            }
            rows.push_back(
                std::move(row) |
                reflect(pulldown_item_boxes[index])
            );
        }
        return gray_menu(
            (
                vbox(std::move(rows)) |
                yframe |
                size(HEIGHT, LESS_THAN, std::max(1, Terminal::Size().dimy - 3)) |
                reflect(pulldown_viewport) |
                borderStyled(LIGHT)
            ) |
            size(
                WIDTH,
                EQUAL,
                widths[pulldown_category]
            )
        );
    }

    void execute_pulldown() {
        const auto action =
            pulldown_items()[pulldown_category][pulldown_item]
                .action;
        if (action < 0) {
            return;
        }
        if (pulldown_category == 0U ||
            pulldown_category == 4U) {
            const auto panel_index =
                pulldown_category == 0U ? 0 : 1;
            if (action == 10) {
                show_quick_view(panel_index);
                mode = Mode::panels;
                return;
            }
            if (action == 11) {
                show_information_panel(panel_index);
                mode = Mode::panels;
                return;
            }
            if (action == 14) {
                show_tree_panel(panel_index);
                mode = Mode::panels;
                return;
            }
            if (action == 15) {
                if (quick_view_panel == panel_index) {
                    close_quick_view(false);
                }
                if (information_panel == panel_index) {
                    close_information_panel(false);
                }
                if (tree_panel == panel_index) {
                    close_tree_panel(false);
                }
                active_index = panel_index;
                start_prompt(
                    PromptAction::panelize_search,
                    "Panelize name pattern",
                    "*"
                );
                return;
            }
            if (action == 12) {
                if (quick_view_panel == panel_index) {
                    close_quick_view();
                }
                if (information_panel == panel_index) {
                    close_information_panel();
                }
                if (tree_panel == panel_index) {
                    close_tree_panel();
                }
                mode = Mode::panels;
                return;
            }
            if (quick_view_panel == panel_index) {
                close_quick_view();
            }
            if (information_panel == panel_index) {
                close_information_panel();
            }
            if (tree_panel == panel_index) {
                close_tree_panel();
            }
            active_index = panel_index;
            switch (action) {
                case 0:
                    detailed_view = false;
                    break;
                case 1:
                    detailed_view = true;
                    break;
                case 2:
                    active().set_show_hidden(!active().show_hidden());
                    break;
                case 3:
                    sort_active(SortField::name);
                    break;
                case 4:
                    sort_active(SortField::size);
                    break;
                case 5:
                    sort_active(SortField::modified);
                    break;
                case 6:
                    active().set_directories_first(
                        !active().directories_first()
                    );
                    break;
                case 7:
                    active().refresh();
                    break;
                case 8:
                    bookmark_cursor = 0;
                    mode = Mode::bookmarks;
                    return;
                case 9:
                    sort_active(SortField::extension);
                    break;
                case 13:
                    start_prompt(
                        PromptAction::panel_filter,
                        "Panel filter",
                        active().filter()
                    );
                    return;
                default:
                    break;
            }
            mode = Mode::panels;
            return;
        }
        if (pulldown_category == 1U) {
            const auto* entry = active().current();
            switch (action) {
                case 0:
                    if (entry != nullptr &&
                        can_open_file(*entry)) {
                        viewer.open(active().provider(), entry->path);
                        viewer_search_needle.clear();
                        viewer_return_mode = Mode::panels;
                        mode = Mode::viewer;
                        return;
                    }
                    break;
                case 1:
                    if (entry != nullptr &&
                        can_open_file(*entry)) {
                        open_editor_buffer(
                            active().provider(),
                            entry->path,
                            false
                        );
                        mode = Mode::editor;
                        return;
                    }
                    break;
                case 2:
                    start_operation_dialog(false, false);
                    return;
                case 3:
                    start_prompt(
                        PromptAction::rename,
                        "Rename",
                        entry == nullptr ? "" : entry->name
                    );
                    return;
                case 4: {
                    const auto sources = active().selected_entries();
                    start_operation_dialog(
                        true,
                        sources.size() == 1U
                    );
                    return;
                }
                case 5:
                    start_prompt(PromptAction::mkdir, "New directory");
                    return;
                case 6:
                    if (config.confirm_delete) {
                        start_delete_dialog();
                    } else {
                        run_remove_operation();
                    }
                    return;
                case 7:
                    mode = Mode::information;
                    return;
                case 8: {
                    std::ostringstream permissions;
                    permissions
                        << std::oct
                        << std::setfill('0')
                        << std::setw(4)
                        << (
                               entry == nullptr
                                   ? 0644U
                                   : static_cast<unsigned int>(
                                         entry->permissions
                                     ) &
                                         07777U
                           );
                    start_prompt(
                        PromptAction::chmod,
                        "Octal permissions",
                        permissions.str()
                    );
                    return;
                }
                case 9:
                    start_prompt(
                        PromptAction::chown,
                        "Owner:group (blank keeps current)",
                        entry == nullptr
                            ? ""
                            : entry->owner + ":" + entry->group
                    );
                    return;
                case 10:
                    exit_terminal();
                    return;
                case 11:
                    start_prompt(
                        PromptAction::hardlink,
                        "Hard link name"
                    );
                    return;
                case 12:
                    start_prompt(
                        PromptAction::symlink,
                        "Symbolic link name"
                    );
                    return;
                case 13:
                    start_prompt(
                        PromptAction::relative_symlink,
                        "Relative symbolic link name"
                    );
                    return;
                case 14:
                    start_prompt(
                        PromptAction::quick_cd,
                        "Change directory",
                        active().path()
                    );
                    return;
                case 15:
                    start_prompt(
                        PromptAction::select_pattern,
                        "Select group",
                        "*"
                    );
                    return;
                case 16:
                    start_prompt(
                        PromptAction::unselect_pattern,
                        "Unselect group",
                        "*"
                    );
                    return;
                case 17:
                    active().invert_selection();
                    break;
                case 18: {
                    auto filename =
                        entry == nullptr ? std::string{} : entry->name;
                    if (entry != nullptr &&
                        active().provider()->join(
                            active().path(),
                            filename
                        ) != entry->path) {
                        filename = entry->path;
                    }
                    start_prompt(
                        PromptAction::view_file,
                        "View file",
                        std::move(filename)
                    );
                    return;
                }
                default:
                    break;
            }
            mode = Mode::panels;
            return;
        }
        if (pulldown_category == 2U) {
            switch (action) {
                case 0:
                    start_prompt(
                        PromptAction::recursive_search,
                        "Recursive name pattern",
                        "*"
                    );
                    return;
                case 1: {
                    compare_choice = 0;
                    mode = Mode::compare_dialog;
                    return;
                }
                case 2:
                    start_prompt(
                        PromptAction::quick_cd,
                        "Change directory",
                        active().path()
                    );
                    return;
                case 3:
                    status = "Subshell exited " +
                             std::to_string(run_process("", true));
                    break;
                case 4:
                    refresh();
                    break;
                case 5:
                    swap_panels();
                    break;
                case 6:
                    passive().change_directory(active().path());
                    break;
                case 7:
                    bookmark_cursor = 0;
                    mode = Mode::bookmarks;
                    return;
                case 8:
                    mode = Mode::user_menu;
                    return;
                case 10:
                    open_jobs();
                    return;
                default:
                    break;
            }
            mode = Mode::panels;
            return;
        }
        if (pulldown_category == 3U) {
            switch (action) {
                case 0:
                    toggle_quick_view();
                    break;
                case 1:
                    config.confirm_delete = !config.confirm_delete;
                    break;
                case 2:
                    config.confirm_overwrite =
                        !config.confirm_overwrite;
                    break;
                case 3:
                    store_panel_options_in_config();
                    store_editor_options_in_config();
                    config.save(default_config_path());
                    status = "Setup saved";
                    break;
                default:
                    break;
            }
            mode = Mode::panels;
        }
    }

    void execute_pulldown_action(
        const std::size_t category,
        const int action
    ) {
        const auto& items = pulldown_items()[category];
        const auto item = std::ranges::find(
            items,
            action,
            &MenuRow::action
        );
        if (item == items.end()) {
            return;
        }
        pulldown_category = category;
        pulldown_item = static_cast<std::size_t>(
            std::distance(items.begin(), item)
        );
        execute_pulldown();
    }

    ftxui::Element render_bookmarks() {
        using namespace ftxui;
        Elements rows;
        bookmark_boxes.assign(config.bookmarks.size(), {});
        for (std::size_t index = 0;
             index < config.bookmarks.size();
             ++index) {
            auto row = text(path_label(
                display_text(config.bookmarks[index]),
                static_cast<std::size_t>(std::max(
                    1, std::min(62, Terminal::Size().dimx) - 4
                ))
            ));
            if (index == bookmark_cursor) {
                row = row | inverted | focus;
            }
            rows.push_back(std::move(row) | reflect(bookmark_boxes[index]));
        }
        if (rows.empty()) {
            rows.push_back(text("<hotlist is empty>") | dim);
        }
        return vbox({
                   text("Directory hotlist") | bold,
                   separator(),
                   vbox(std::move(rows)) | yframe | flex | reflect(bookmark_list_box),
                   separator(),
                   text("Enter opens; Delete removes; Esc closes"),
               }) |
               borderStyled(LIGHT) | size(WIDTH, EQUAL, 62) |
               size(HEIGHT, LESS_THAN, std::max(5, Terminal::Size().dimy - 2));
    }

    ftxui::Element render_information() {
        using namespace ftxui;
        const auto* entry = active().current();
        if (entry == nullptr) {
            return text("No selected entry") | border | center;
        }
        const auto kind =
            entry->kind == EntryKind::directory
                ? "directory"
                : entry->kind == EntryKind::file
                      ? "file"
                      : entry->kind == EntryKind::symlink
                            ? "symbolic link"
                            : "other";
        return vbox({
                   text("File information") | bold | center,
                   separator(),
                   text("Name: " + display_text(entry->name)),
                   text("Path: " + display_text(entry->path)),
                   text("Type: " + std::string(kind)),
                   text("Size: " + std::to_string(entry->size)),
                   text("Modified: " + format_time(entry->modified)),
                   text("Owner: " + entry->owner),
                   text("Group: " + entry->group),
                   text(
                       entry->target.empty()
                           ? ""
                           : "Target: " + display_text(entry->target)
                   ),
                   separator(),
                   text("Esc closes"),
               }) |
               borderStyled(LIGHT) | size(WIDTH, EQUAL, 72);
    }

    ftxui::Element render_operation_dialog() {
        using namespace ftxui;
        const auto sources = current_operation_sources();
        operation_dialog_boxes = {};
        const auto focused = [&](Element element, const std::size_t index) {
            if (dialog_focus == index) {
                element = element | inverted | focus;
            }
            return element | reflect(operation_dialog_boxes[index]);
        };
        Elements rows{
            text("To:"),
            render_input(
                dialog_destination, destination_input, dialog_focus == 0U
            ) | reflect(operation_dialog_boxes[0]),
            text("Source mask:"),
            render_input(
                dialog_source_mask, mask_input, dialog_focus == 1U
            ) | reflect(operation_dialog_boxes[1]),
            separator(),
            focused(
                text(std::string(dialog_preserve_attributes ? "[x] " : "[ ] ") +
                     "Preserve permissions"),
                2
            ),
            focused(
                text(std::string(dialog_preserve_symlinks ? "[x] " : "[ ] ") +
                     "Preserve symbolic links"),
                3
            ),
            focused(
                text(std::string(dialog_background ? "[x] " : "[ ] ") +
                     "Run in background"),
                4
            ),
        };
        const auto width = std::min(68, Terminal::Size().dimx);
        const auto title = std::string(dialog_move ? "Move / Rename: " : "Copy: ") +
                           (sources.size() == 1U
                                ? display_text(sources.front().name)
                                : std::to_string(sources.size()) + " selected item(s)");
        Elements form{
            text(path_label(title, static_cast<std::size_t>(std::max(1, width - 2)))) |
                bold | center,
            separator(),
            vbox(std::move(rows)) | yframe | flex | reflect(operation_dialog_viewport),
            separator(),
            hbox({
                focused(text(" [ OK ] "), 5),
                text(" "),
                focused(text(" [ Cancel ] "), 6),
            }) | center,
        };
        if (Terminal::Size().dimy < 20) {
            form.push_back(text("Wheel/PgUp/PgDn: more options") | dim | center);
        }
        return vbox(std::move(form)) |
               size(HEIGHT, LESS_THAN, std::max(5, Terminal::Size().dimy - 2)) |
               borderStyled(LIGHT) |
               size(WIDTH, EQUAL, width);
    }

    ftxui::Element render_delete_dialog() {
        using namespace ftxui;
        const auto entries = current_operation_sources();
        const auto focused = [&](Element element, const std::size_t index) {
            if (dialog_focus == index) {
                element = element | inverted | focus;
            }
            return element | reflect(delete_button_boxes[index]);
        };
        return vbox({
                   text("Delete") | bold | center,
                   separator(),
                   text(
                       "Delete " + std::to_string(entries.size()) +
                       " selected item(s)?"
                   ),
                   text(
                       entries.size() == 1U
                           ? display_text(entries.front().name)
                           : ""
                   ),
                   separator(),
                   hbox({
                       focused(text(" [ Yes ] "), 0),
                       text("  "),
                       focused(text(" [ No ] "), 1),
                   }) | center,
               }) |
               borderStyled(LIGHT) | size(WIDTH, EQUAL, 54);
    }

    ftxui::Element render_operation_progress() {
        using namespace ftxui;
        TransferProgress current;
        {
            std::lock_guard lock(operation_mutex);
            current = progress;
        }
        const auto byte_ratio =
            current.bytes_total == 0U
                ? 0.0F
                : static_cast<float>(current.bytes_done) /
                      static_cast<float>(current.bytes_total);
        const auto file_ratio =
            current.files_total == 0U
                ? 0.0F
                : static_cast<float>(current.files_done) /
                      static_cast<float>(current.files_total);
        const auto elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - operation_started
        ).count();
        const auto bytes_per_second =
            elapsed > 0.0
                ? static_cast<std::uint64_t>(
                      static_cast<double>(current.bytes_done) /
                      elapsed
                  )
                : 0U;
        std::string eta{"calculating"};
        if (bytes_per_second > 0U &&
            current.bytes_total >= current.bytes_done) {
            const auto seconds =
                (current.bytes_total - current.bytes_done) /
                bytes_per_second;
            eta =
                std::to_string(seconds / 60U) + "m " +
                std::to_string(seconds % 60U) + "s";
        }
        return vbox({
                   text(
                       operation_label + " progress"
                   ) | bold | center,
                   separator(),
                   text(display_text(current.current_path)),
                   text(
                       "File bytes: " +
                       format_size(current.bytes_done) + " / " +
                       format_size(current.bytes_total)
                   ),
                   gauge(std::clamp(byte_ratio, 0.0F, 1.0F)) |
                       color(Color::Grey11) |
                       bgcolor(Color::Grey70),
                   text(
                       "Files: " +
                       std::to_string(current.files_done) + " / " +
                       std::to_string(current.files_total)
                   ),
                   gauge(std::clamp(file_ratio, 0.0F, 1.0F)) |
                       color(Color::Grey11) |
                       bgcolor(Color::Grey70),
                   text(
                       "Speed: " +
                       format_size(bytes_per_second) +
                       "/s   ETA: " + eta
                   ),
                   separator(),
                   hbox({
                       text(
                           operation_paused.load(
                               std::memory_order_acquire
                           )
                               ? " [ Continue ] "
                               : " [ Pause ] "
                       ) |
                           inverted,
                       text("  [ Skip ] ") | inverted,
                       text("  [ Abort ] ") | inverted,
                   }) | center,
               }) |
               borderStyled(LIGHT) | size(WIDTH, EQUAL, 72);
    }

    ftxui::Element render_conflict_dialog() {
        using namespace ftxui;
        const bool error = mode == Mode::operation_error_dialog;
        const std::vector<std::string> labels =
            error ? std::vector<std::string>{"Retry", "Skip", "Ignore all", "Abort"}
                  : std::vector<std::string>{
                        "Overwrite", "Skip", "All", "None", "Older", "Cancel"
                    };
        Elements rows{
            text(error ? "File operation error" : "Destination exists") | bold | center,
            separator(),
        };
        if (operation_prompt_source) {
            rows.push_back(paragraph(
                "Source: " + display_text(operation_prompt_source->path)
            ));
            rows.push_back(text(
                "Size: " + format_size(operation_prompt_source->size) +
                "  Modified: " + format_time(operation_prompt_source->modified)
            ));
        }
        if (operation_prompt_destination) {
            rows.push_back(paragraph(
                "Destination: " + display_text(operation_prompt_destination->path)
            ));
            rows.push_back(text(
                "Size: " + format_size(operation_prompt_destination->size) +
                "  Modified: " + format_time(operation_prompt_destination->modified)
            ));
        }
        if (error) {
            rows.push_back(paragraph(display_text(operation_prompt_error)));
        }
        rows.push_back(separator());
        Elements buttons;
        for (std::size_t index = 0; index < labels.size(); ++index) {
            auto button = text(" [ " + labels[index] + " ] ");
            if (dialog_focus == index) {
                button = button | inverted | focus;
            }
            buttons.push_back(
                std::move(button) | reflect(operation_choice_boxes[index])
            );
        }
        if (error) {
            rows.push_back(hbox(std::move(buttons)) | center);
            rows.push_back(text("R retry  S skip  I ignore all  Esc abort") | center);
        } else {
            Elements first, second;
            for (std::size_t index = 0; index < buttons.size(); ++index) {
                (index < 3U ? first : second).push_back(std::move(buttons[index]));
            }
            rows.push_back(hbox(std::move(first)) | center);
            rows.push_back(hbox(std::move(second)) | center);
            rows.push_back(text("Y overwrite  N skip  A all  S none  O older  Esc abort") | center);
        }
        return vbox(std::move(rows)) | borderStyled(LIGHT) |
               size(WIDTH, LESS_THAN, std::min(90, Terminal::Size().dimx));
    }

    void open_jobs() {
        if (active_job) {
            job_cursor = *active_job;
        } else if (!jobs.empty()) {
            job_cursor = jobs.size() - 1U;
        }
        mode = Mode::jobs;
    }

    ftxui::Element render_compare_dialog() {
        using namespace ftxui;
        Elements rows{
            text("Compare directories") | bold | center,
            separator(),
        };
        const std::array<std::string, 4> labels{
            "Quick", "Size only", "Thorough", "Cancel"
        };
        for (std::size_t index = 0; index < labels.size(); ++index) {
            rows.push_back(dialog_button(
                labels[index], compare_choice == index, compare_boxes[index]
            ) | center);
        }
        return vbox(std::move(rows)) | borderStyled(LIGHT) |
               size(WIDTH, LESS_THAN, 44);
    }

    bool handle_compare_dialog(const ftxui::Event& event) {
        auto input = event;
        if (event == ftxui::Event::ArrowUp) {
            input = ftxui::Event::ArrowLeft;
        } else if (event == ftxui::Event::ArrowDown) {
            input = ftxui::Event::ArrowRight;
        }
        const auto result = handle_button_input(input, compare_choice, compare_boxes, "qstc");
        if (result == ButtonInput::cancel ||
            (result == ButtonInput::activate && compare_choice == 3U)) {
            mode = Mode::panels;
        } else if (result == ButtonInput::activate) {
            try {
                const DirectoryComparator comparator;
                const auto differences = comparator.compare(
                    *left.provider(), left.path(), *right.provider(), right.path(),
                    false, {}, static_cast<CompareMode>(compare_choice)
                );
                std::vector<std::string> left_marks, right_marks;
                for (const auto& difference : differences) {
                    if (difference.mark_left) {
                        left_marks.push_back(left.provider()->join(
                            left.path(), difference.relative_path
                        ));
                    }
                    if (difference.mark_right) {
                        right_marks.push_back(right.provider()->join(
                            right.path(), difference.relative_path
                        ));
                    }
                }
                left.select_paths(left_marks);
                right.select_paths(right_marks);
                status = std::to_string(differences.size()) + " difference(s)";
            } catch (const std::exception& error) {
                set_error(error);
            }
            mode = Mode::panels;
        }
        return result != ButtonInput::ignored;
    }

    ftxui::Element render_jobs() {
        using namespace ftxui;
        Elements rows;
        job_row_boxes.assign(jobs.size(), {});
        for (std::size_t index = 0; index < jobs.size(); ++index) {
            auto state = jobs[index].status;
            if (active_job == index &&
                operation_running.load(std::memory_order_acquire)) {
                state = operation_prompt_pending.load(std::memory_order_acquire)
                            ? "Waiting for decision"
                            : operation_paused.load(std::memory_order_acquire)
                                  ? "Paused"
                                  : "Running";
            }
            auto row = text(
                std::to_string(jobs[index].id) + "  " +
                jobs[index].label + "  " +
                middle_truncate(
                    display_text(state),
                    std::max(1, std::min(68, Terminal::Size().dimx - 12))
                )
            );
            if (job_cursor == index) {
                row = row | inverted | focus;
            }
            rows.push_back(std::move(row) | reflect(job_row_boxes[index]));
        }
        if (rows.empty()) {
            rows.push_back(text("No file-operation jobs"));
        }
        Elements buttons;
        const std::array<std::string, 4> labels{
            "Pause/Resume F2", "Show F3", "Abort F8", "Close"
        };
        for (std::size_t index = 0; index < labels.size(); ++index) {
            buttons.push_back(
                text(" [ " + labels[index] + " ] ") |
                reflect(job_button_boxes[index])
            );
        }
        return vbox({
                   text("Background jobs") | bold | center,
                   separator(),
                   vbox(std::move(rows)) | yframe |
                       size(HEIGHT, LESS_THAN, std::max(1, Terminal::Size().dimy - 9)),
                   separator(),
                   text(
                       job_cursor < jobs.size()
                           ? middle_truncate(
                                 display_text(jobs[job_cursor].destination),
                                 std::max(1, Terminal::Size().dimx - 8)
                             )
                           : ""
                   ),
                   hbox({buttons[0], buttons[1]}) | center,
                   hbox({buttons[2], buttons[3]}) | center,
               }) |
               borderStyled(LIGHT) |
               size(WIDTH, LESS_THAN, std::min(90, Terminal::Size().dimx));
    }

    ftxui::Element render_search_results() {
        using namespace ftxui;
        Elements rows;
        search_result_boxes.assign(search_results.size(), {});
        rows.reserve(search_results.size());
        for (std::size_t index = 0;
             index < search_results.size();
             ++index) {
            auto row = text(path_label(
                display_text(search_results[index].path),
                static_cast<std::size_t>(std::max(1, Terminal::Size().dimx - 4))
            ));
            if (index == search_cursor) {
                row = row | inverted | focus;
            }
            rows.push_back(std::move(row) | reflect(search_result_boxes[index]));
        }
        if (rows.empty()) {
            rows.push_back(text("<no matches>") | dim);
        }
        return vbox({
                   text("Search results") | bold,
                   separator(),
                   vbox(std::move(rows)) | yframe | flex | reflect(search_list_box),
                   separator(),
                   search_warning.empty()
                       ? emptyElement()
                       : text(truncate_text(
                             display_text(search_warning),
                             static_cast<std::size_t>(std::max(1, Terminal::Size().dimx - 4))
                         )) | bold,
                   text("F3 View  F4 Edit  Enter location  Esc closes"),
               }) |
               border;
    }

    ftxui::Element render_quick_view(const int width) {
        using namespace ftxui;
        const auto [content_width, content_height] =
            quick_view_dimensions();
        const auto frame = [&](Element title, Element content) {
            return window(
                       std::move(title),
                       std::move(content),
                       LIGHT
                   ) |
                   size(WIDTH, EQUAL, width) |
                   yflex |
                   reflect(quick_view_box);
        };
        const auto* entry = active().current();
        if (entry == nullptr || !can_open_file(*entry)) {
            quick_viewer.close();
            quick_view_path.clear();
            return frame(
                text(" Quick view ") | bold,
                text("<select a regular file>") | dim | center
            );
        }
        if (entry->path != quick_view_path) {
            try {
                quick_viewer.open(active().provider(), entry->path);
                quick_view_search_needle.clear();
                quick_view_path = entry->path;
            } catch (const std::exception& error) {
                set_error(error);
                quick_view_path.clear();
                return frame(
                    text(" Quick view ") | bold,
                    text("<preview unavailable>") | center
                );
            }
        }
        Elements rows;
        for (const auto& line : quick_viewer.lines(
                 content_width,
                 content_height
             )) {
            rows.push_back(text(line));
        }
        auto title =
            text("Quick view: " + path_label(
                display_text(entry->name),
                static_cast<std::size_t>(std::max(1, width - 16))
            )) |
            bold;
        if (quick_view_focused) {
            title = title | color(Color::Grey100);
        } else {
            title = title | color(Color::Grey58);
        }
        const auto percent =
            quick_viewer.size() == 0U
                ? 100U
                : static_cast<unsigned int>(
                      std::min<std::uint64_t>(
                          100U,
                          quick_viewer.view_end() * 100U /
                              quick_viewer.size()
                      )
                  );
        return frame(
            hbox({
                text(" "),
                std::move(title),
                text(" "),
            }),
            vbox({
                vbox(std::move(rows)) | yframe | flex,
                hbox({
                    filler(),
                    text(
                        std::to_string(quick_viewer.view_end()) +
                        "/" +
                        std::to_string(quick_viewer.size()) +
                        "  " +
                        std::to_string(percent) + "%"
                    ),
                }) |
                    color(Color::Grey50),
            })
        );
    }

    ftxui::Element render_information_panel(const int width) {
        using namespace ftxui;
        const auto* entry = active().current();
        Elements rows;
        rows.push_back(
            text("Location: " + display_text(active().path()))
        );
        rows.push_back(separator());
        if (entry == nullptr) {
            rows.push_back(text("<no selected entry>") | dim);
        } else {
            const auto kind =
                entry->kind == EntryKind::directory
                    ? "directory"
                    : entry->kind == EntryKind::file
                          ? "file"
                          : entry->kind == EntryKind::symlink
                                ? "symbolic link"
                                : "other";
            rows.push_back(
                text("Name: " + display_text(entry->name))
            );
            rows.push_back(
                text("Type: " + std::string(kind))
            );
            rows.push_back(
                text("Size: " + format_size(entry->size))
            );
            rows.push_back(
                text("Mode: " + permission_text(*entry))
            );
            rows.push_back(
                text(
                    "Owner: " + entry->owner + ":" +
                    entry->group
                )
            );
            rows.push_back(
                text(
                    "Modified: " +
                    format_time(entry->modified)
                )
            );
            if (!entry->target.empty()) {
                rows.push_back(
                    text(
                        "Target: " +
                        display_text(entry->target)
                    )
                );
            }
        }
        rows.push_back(separator());
        try {
            const auto space =
                active().provider()->space(active().path());
            rows.push_back(
                text(
                    "Filesystem: " +
                    format_size(space.available) + " free / " +
                    format_size(space.capacity)
                )
            );
        } catch (const std::exception&) {
            rows.push_back(text("Filesystem: unavailable") | dim);
        }
        auto title = text(" Info ");
        title = information_panel_focused
                    ? title | color(Color::Grey100) | bold
                    : title | color(Color::Grey58);
        return window(
                   std::move(title),
                   vbox(std::move(rows)),
                   LIGHT
               ) |
               size(WIDTH, EQUAL, width) |
               yflex |
               reflect(information_panel_box);
    }

    ftxui::Element render_tree_panel(const int width) {
        using namespace ftxui;
        Elements rows;
        tree_row_boxes.clear();
        tree_row_boxes.resize(tree_nodes.size());
        tree_row_paths.assign(tree_nodes.size(), {});
        const auto visible_rows = panel_visible_rows();
        const auto first =
            tree_nodes.size() <= visible_rows
                ? 0U
                : std::min(
                      tree_nodes.size() - visible_rows,
                      tree_cursor > visible_rows / 2U
                          ? tree_cursor - visible_rows / 2U
                          : 0U
                  );
        const auto end = std::min(
            tree_nodes.size(),
            first + visible_rows
        );
        const auto is_last_sibling = [&](const std::size_t index) {
            const auto depth = tree_nodes[index].depth;
            for (auto next = index + 1U;
                 next < tree_nodes.size();
                 ++next) {
                if (tree_nodes[next].depth < depth) {
                    break;
                }
                if (tree_nodes[next].depth == depth) {
                    return false;
                }
            }
            return true;
        };
        for (std::size_t index = first;
             index < end;
             ++index) {
            const auto& node = tree_nodes[index];
            tree_row_paths[index] = node.path;
            const auto name =
                node.depth == 0U
                    ? node.path
                    : std::filesystem::path(node.path)
                          .filename()
                          .string();
            std::string connector =
                node.depth == 0U ? std::string{} : " ";
            for (std::size_t depth = 1U;
                 depth < node.depth;
                 ++depth) {
                auto ancestor = index;
                while (ancestor > 0U) {
                    --ancestor;
                    if (tree_nodes[ancestor].depth == depth) {
                        break;
                    }
                }
                connector += is_last_sibling(ancestor)
                                 ? "   "
                                 : "│  ";
            }
            if (node.depth > 0U) {
                connector +=
                    is_last_sibling(index) ? "└─ " : "├─ ";
            }
            if (node.inaccessible) {
                connector += "! ";
            } else if (node.loading) {
                connector += "… ";
            }
            const auto minimum_name_width =
                std::min(12, std::max(1, string_width(name)));
            const auto maximum_connector_width =
                std::max(
                    0,
                    width - 2 - minimum_name_width
                );
            if (string_width(connector) >
                maximum_connector_width) {
                if (maximum_connector_width <= 1) {
                    connector.clear();
                } else {
                    std::size_t offset = 0;
                    while (offset < connector.size() &&
                           string_width(
                               connector.substr(offset)
                           ) >= maximum_connector_width) {
                        offset = next_utf8(connector, offset);
                    }
                    connector =
                        "~" + connector.substr(offset);
                }
            }
            const auto available_name_width =
                static_cast<std::size_t>(
                    std::max(
                        1,
                        width - 2 -
                            std::max(
                                0,
                                string_width(connector)
                            )
                    )
                );
            auto row = text(
                connector +
                middle_truncate(
                    display_text(name),
                    available_name_width
                )
            );
            if (index == tree_cursor) {
                row = row | focus;
                row = tree_panel_focused
                          ? row | inverted
                          : row | color(Color::Grey70);
            }
            rows.push_back(
                std::move(row) | reflect(tree_row_boxes[index])
            );
        }
        if (rows.empty()) {
            rows.push_back(text("<empty tree>") | dim);
        }
        while (rows.size() < visible_rows) {
            rows.push_back(text(""));
        }
        auto title_text = text(" Directory tree ");
        title_text = tree_panel_focused
                         ? title_text |
                               color(Color::Grey100) |
                               bold
                         : title_text | color(Color::Grey58);
        auto title = hbox({
                         filler(),
                         std::move(title_text),
                         filler(),
                     }) |
                     size(WIDTH, EQUAL, std::max(1, width - 2));
        return window(
                   std::move(title),
                   vbox({
                       vbox(std::move(rows)) | flex,
                       separator(),
                       text(
                           tree_cursor < tree_nodes.size()
                               ? middle_truncate(
                                     tree_nodes[tree_cursor].path,
                                     static_cast<std::size_t>(
                                         std::max(1, width - 2)
                                     )
                                 )
                               : std::string{}
                       ) |
                           color(Color::Grey50),
                   }),
                   LIGHT
               ) |
               size(WIDTH, EQUAL, width) |
               yflex |
               reflect(tree_panel_box);
    }

    ftxui::Element render() {
        using namespace ftxui;
        if (exiting) {
            return filler();
        }
        const auto terminal_dimensions = Terminal::Size();
        if (rendered_width != 0 &&
            (
                rendered_width != terminal_dimensions.dimx ||
                rendered_height != terminal_dimensions.dimy
            )) {
            std::cout << "\033[H\033[2J" << std::flush;
        }
        rendered_width = terminal_dimensions.dimx;
        rendered_height = terminal_dimensions.dimy;
        const auto display_mode =
            mode == Mode::conflict_dialog || mode == Mode::operation_error_dialog
                ? operation_return_mode
                : mode;
        const bool editor_context =
            display_mode == Mode::editor ||
            display_mode == Mode::editor_menu ||
            display_mode == Mode::editor_help ||
            display_mode == Mode::editor_error ||
            display_mode == Mode::editor_search_dialog ||
            display_mode == Mode::editor_replace_dialog ||
            display_mode == Mode::editor_replace_confirm ||
            display_mode == Mode::editor_save_dialog ||
            display_mode == Mode::editor_close_dialog ||
            display_mode == Mode::editor_link_dialog ||
            display_mode == Mode::editor_overwrite_dialog ||
            display_mode == Mode::editor_options_dialog ||
            display_mode == Mode::editor_history ||
            display_mode == Mode::editor_buffer_list ||
            (
                display_mode == Mode::prompt &&
                mode_before_prompt == Mode::editor
            );
        const bool viewer_context =
            display_mode == Mode::viewer ||
            display_mode == Mode::viewer_message ||
            display_mode == Mode::viewer_close_dialog ||
            (display_mode == Mode::viewer_wrap_dialog && !viewer_search_quick) ||
            (
                display_mode == Mode::prompt &&
                mode_before_prompt == Mode::viewer
            );
        Element body;
        const auto left_width = panel_width(0);
        const auto right_width = panel_width(1);
        if (editor_context || viewer_context) {
            body = emptyElement();
        } else if (tree_panel) {
            if (*tree_panel == 1) {
                right_entry_boxes.clear();
                body = hbox({
                    render_pane(
                        left,
                        !tree_panel_focused,
                        left_width,
                        left_box,
                        left_entry_boxes
                    ),
                    render_tree_panel(right_width),
                });
            } else {
                left_entry_boxes.clear();
                body = hbox({
                    render_tree_panel(left_width),
                    render_pane(
                        right,
                        !tree_panel_focused,
                        right_width,
                        right_box,
                        right_entry_boxes
                    ),
                });
            }
        } else if (information_panel) {
            if (*information_panel == 1) {
                right_entry_boxes.clear();
                body = hbox({
                    render_pane(
                        left,
                        !information_panel_focused,
                        left_width,
                        left_box,
                        left_entry_boxes
                    ),
                    render_information_panel(right_width),
                });
            } else {
                left_entry_boxes.clear();
                body = hbox({
                    render_information_panel(left_width),
                    render_pane(
                        right,
                        !information_panel_focused,
                        right_width,
                        right_box,
                        right_entry_boxes
                    ),
                });
            }
        } else if (quick_view_panel) {
            if (*quick_view_panel == 1) {
                right_entry_boxes.clear();
                body = hbox({
                    render_pane(
                        left,
                        !quick_view_focused,
                        left_width,
                        left_box,
                        left_entry_boxes
                    ),
                    render_quick_view(right_width),
                });
            } else {
                left_entry_boxes.clear();
                body = hbox({
                    render_quick_view(left_width),
                    render_pane(
                        right,
                        !quick_view_focused,
                        right_width,
                        right_box,
                        right_entry_boxes
                    ),
                });
            }
        } else {
            body = hbox({
                render_pane(
                    left,
                    active_index == 0,
                    left_width,
                    left_box,
                    left_entry_boxes
                ),
                render_pane(
                    right,
                    active_index == 1,
                    right_width,
                    right_box,
                    right_entry_boxes
                ),
            });
        }
        if (mode == Mode::search_results) {
            body = render_search_results();
        }
        std::string progress_text;
        {
            std::lock_guard lock(operation_mutex);
            progress_text =
                progress.files_total == 0U
                    ? std::string{}
                    : " [" + std::to_string(progress.files_done) +
                          "/" +
                          std::to_string(progress.files_total) + "]";
        }
        const auto key = [this](
                             const std::string& number,
                             const std::string& label,
                             const bool enabled = true
                         ) {
            return render_function_key(
                static_cast<std::size_t>(std::stoul(number) - 1U),
                label,
                enabled &&
                    (mode == Mode::panels || mode == Mode::search_results)
            );
        };
        std::array<char, 256> hostname{};
        if (gethostname(hostname.data(), hostname.size()) != 0) {
            std::strncpy(
                hostname.data(),
                "localhost",
                hostname.size() - 1U
            );
        }
        const auto* user = std::getenv("USER");
        const auto prompt =
            std::string(user == nullptr ? "user" : user) + "@" +
            hostname.data() + ":" + active().path() + "$";
        const auto hint =
            pending_operation_notice
                ? "Background [Esc]: " + *pending_operation_notice
                : status == "Ready"
                      ? tree_panel && tree_panel_focused
                            ? "Tree: Enter open/expand  Space toggle  "
                              "+/- branch  Tab panel"
                            : "Hint: Tab changes your current panel."
                      : status + progress_text;
        static const std::array<std::string, 5> menu_names{
            "  Left     ",
            "File     ",
            "Command     ",
            "Options     ",
            "Right "
        };
        static constexpr std::array<std::size_t, 5> mnemonic_indices{
            2, 0, 0, 0, 0
        };
        Elements menu_elements;
        for (std::size_t index = 0; index < menu_names.size(); ++index) {
            auto item = mnemonic_label(
                menu_names[index],
                mnemonic_indices[index]
            );
            if (mode == Mode::pulldown &&
                index == pulldown_category) {
                item = item |
                       color(Color::Grey100) |
                       bgcolor(Color::Grey23) |
                       bold;
            }
            menu_elements.push_back(
                std::move(item) | reflect(top_menu_boxes[index])
            );
        }
        const auto* selected_file = active().current();
        const bool file_action = selected_file != nullptr && can_open_file(*selected_file);
        const bool source_action = !active().selected_entries().empty();
        auto keybar = hbox({
            text(" "),
            key("1", "Help"),
            key("2", "Menu"),
            key("3", "View", file_action),
            key("4", "Edit", file_action),
            key("5", "Copy", source_action),
            key("6", "RenMov", source_action),
            key("7", "Mkdir"),
            key("8", "Delete", source_action),
            key("9", "PullDn"),
            key("10", "Quit"),
        });
        if (mode == Mode::search_results) {
            const bool file_selected =
                search_cursor < search_results.size() &&
                can_open_file(search_results[search_cursor]);
            keybar = hbox({
                text(" "),
                key("1", "", false),
                key("2", "", false),
                key("3", "View", file_selected),
                key("4", "Edit", file_selected),
                key("5", "", false),
                key("6", "", false),
                key("7", "", false),
                key("8", "", false),
                key("9", "", false),
                key("10", "Quit"),
            });
        } else if (information_panel && information_panel_focused) {
            keybar = hbox({
                text(" "),
                key("1", "Help"),
                key("2", "", false),
                key("3", "", false),
                key("4", "", false),
                key("5", "", false),
                key("6", "", false),
                key("7", "", false),
                key("8", "", false),
                key("9", "PullDn"),
                key("10", "Quit"),
            });
        } else if (quick_view_panel && quick_view_focused) {
            keybar = hbox({
                text(" "),
                key("1", "Help", false),
                key(
                    "2",
                    quick_viewer.hex()
                        ? "Edit"
                        : quick_viewer.wrap()
                              ? "UnWrap"
                              : "Wrap",
                    !quick_viewer.hex()
                ),
                key("3", "View", quick_viewer.open()),
                key(
                    "4",
                    quick_viewer.hex() ? "Ascii" : "Hex",
                    quick_viewer.open()
                ),
                key("5", "Goto", false),
                key(
                    "6",
                    quick_viewer.hex() ? "Save" : "",
                    false
                ),
                key(
                    "7",
                    quick_viewer.hex() ? "HxSrch" : "Search",
                    quick_viewer.open()
                ),
                key("8", "Raw", false),
                key("9", "PullDn"),
                key("10", "Quit"),
            });
        } else if (tree_panel && tree_panel_focused) {
            const bool tree_source = tree_cursor > 0U && tree_cursor < tree_nodes.size();
            Elements tree_keys{
                text(" "),
                key("1", "Help", false),
                key("2", "Rescan"),
                key("3", "Forget", tree_source),
                key("4", tree_dynamic ? "Static" : "Dynamc"),
                key("5", "Copy", tree_source),
                key("6", "RenMov", tree_source),
                key("7", "", false),
                key("8", "Rmdir", tree_source),
                key("9", "PullDn"),
            };
            tree_keys.push_back(key("10", "Quit"));
            keybar = hbox(std::move(tree_keys));
        }
        auto document = vbox({
            gray_menu(hbox(std::move(menu_elements))),
            body | flex,
            text(hint) |
                color(Color::Grey11) |
                bgcolor(Color::Grey62),
            render_input(
                      command_line,
                      command_input,
                      command_mode &&
                          !(tree_panel && tree_panel_focused) &&
                          !(quick_view_panel && quick_view_focused) &&
                          !(information_panel && information_panel_focused),
                      command_text(
                          prompt,
                          static_cast<std::size_t>(
                              std::max(1, Terminal::Size().dimx / 2)
                          )
                      ) + " "
                  ),
            std::move(keybar),
        });
        if (viewer_context) {
            document = render_viewer();
        } else if (editor_context) {
            document = render_editor();
        }
        const auto centered_modal = [](Element element) {
            return vbox({
                filler(),
                hbox({
                    filler(),
                    gray_dialog(std::move(element)) | clear_under,
                    filler(),
                }),
                filler(),
            });
        };
        if (mode == Mode::prompt) {
            const auto focused = [&](Element element,
                                     const std::size_t index) {
                if (prompt_focus == index) {
                    element = element | inverted;
                }
                return element | reflect(prompt_boxes[index]);
            };
            Elements prompt_rows{
                text(prompt_title) | bold | center,
                separator(),
            };
            const auto* prompt_entry = prompt_targets.empty()
                                           ? active().current()
                                           : &prompt_targets.front();
            if (
                prompt_action == PromptAction::chmod ||
                prompt_action == PromptAction::chown ||
                prompt_action == PromptAction::hardlink ||
                prompt_action == PromptAction::symlink ||
                prompt_action == PromptAction::relative_symlink ||
                prompt_action == PromptAction::rename
            ) {
                prompt_rows.push_back(
                    text(
                        (prompt_targets.size() > 1U
                             ? std::to_string(prompt_targets.size()) +
                                   " marked targets; first: "
                             : "Source: ") +
                        (
                           prompt_entry == nullptr
                               ? std::string{"<none>"}
                               : display_text(prompt_entry->path)
                        )
                    )
                );
            }
            if (prompt_action == PromptAction::chmod &&
                prompt_entry != nullptr) {
                prompt_rows.push_back(
                    text(
                        "Current: " +
                        permission_text(*prompt_entry)
                    )
                );
            }
            const auto field_label =
                prompt_action == PromptAction::chmod
                    ? "Octal mode:"
                    : prompt_action == PromptAction::chown
                          ? "Owner:group:"
                          : prompt_action == PromptAction::hardlink
                               ? "Hard link name:"
                               : (
                                     prompt_action ==
                                             PromptAction::symlink ||
                                     prompt_action ==
                                         PromptAction::relative_symlink
                                 )
                                     ? "Symbolic link name:"
                                     : prompt_action ==
                                               PromptAction::rename
                                           ? "New name:"
                                           : prompt_action ==
                                                     PromptAction::mkdir
                                                 ? "Directory name:"
                                                 : "Value:";
            prompt_rows.push_back(
                text(
                    prompt_action == PromptAction::view_file
                        ? "Filename:"
                        : field_label
                )
            );
            prompt_rows.push_back(
                render_input(
                    prompt_value, prompt_inputs[prompt_action], prompt_focus == 0U
                ) | reflect(prompt_boxes[0])
            );
            if (!prompt_inputs[prompt_action].message.empty()) {
                prompt_rows.push_back(text(prompt_inputs[prompt_action].message));
            }
            if (!prompt_error.empty()) {
                prompt_rows.push_back(
                    text("Error: " + display_text(prompt_error)) |
                    bold
                );
            }
            if (prompt_action == PromptAction::editor_save_as) {
                static const std::array<std::string, 4> endings{
                    "Unchanged", "LF", "CRLF", "CR"
                };
                prompt_rows.push_back(focused(
                    text("Line endings: " + endings[
                        static_cast<std::size_t>(editor_save_line_ending)
                    ] + "  (Left/Right)"),
                    3
                ));
            } else if (prompt_action == PromptAction::viewer_goto) {
                static const std::array<std::string, 4> names{
                    "Line number", "Percentage (0-100)", "Decimal offset", "Hexadecimal offset"
                };
                prompt_rows.push_back(focused(
                    text("Mode: " + names[static_cast<std::size_t>(viewer_goto_mode)] +
                         "  (Left/Right)"),
                    3
                ));
            }
            prompt_rows.push_back(separator());
            prompt_rows.push_back(
                hbox({
                    focused(text(" [ OK ] "), 1),
                    focused(text("  [ Cancel ] "), 2),
                }) | center
            );
            auto prompt_dialog = vbox(std::move(prompt_rows)) |
                          borderStyled(LIGHT) |
                          size(WIDTH, EQUAL, std::min(72, Terminal::Size().dimx));
            document = dbox({
                document,
                centered_modal(std::move(prompt_dialog)),
            });
        } else if (mode == Mode::editor_help) {
            document = dbox({
                document,
                centered_modal(render_editor_help()),
            });
        } else if (mode == Mode::editor_error) {
            document = dbox({
                document,
                centered_modal(render_editor_error()),
            });
        } else if (
            mode == Mode::editor_search_dialog ||
            mode == Mode::editor_replace_dialog
        ) {
            document = dbox({
                document,
                centered_modal(render_editor_search_dialog()),
            });
        } else if (mode == Mode::editor_replace_confirm) {
            document = dbox({
                document,
                centered_modal(render_editor_replace_confirm()),
            });
        } else if (mode == Mode::editor_save_dialog) {
            document = dbox({
                document,
                centered_modal(render_editor_save_dialog()),
            });
        } else if (mode == Mode::editor_close_dialog) {
            document = dbox({
                document,
                centered_modal(render_editor_close_dialog()),
            });
        } else if (mode == Mode::editor_link_dialog) {
            document = dbox({
                document,
                centered_modal(render_editor_link_dialog()),
            });
        } else if (mode == Mode::editor_overwrite_dialog) {
            document = dbox({
                document,
                centered_modal(render_editor_overwrite_dialog()),
            });
        } else if (mode == Mode::editor_options_dialog) {
            document = dbox({
                document,
                centered_modal(render_editor_options_dialog()),
            });
        } else if (mode == Mode::viewer_message) {
            document = dbox({
                document,
                centered_modal(render_viewer_message()),
            });
        } else if (mode == Mode::viewer_close_dialog) {
            document = dbox({
                document,
                centered_modal(render_viewer_close_dialog()),
            });
        } else if (mode == Mode::viewer_wrap_dialog) {
            document = dbox({
                document,
                centered_modal(render_viewer_wrap_dialog()),
            });
        } else if (mode == Mode::editor_history) {
            document = dbox({
                document,
                centered_modal(render_editor_list(true)),
            });
        } else if (mode == Mode::editor_buffer_list) {
            document = dbox({
                document,
                centered_modal(render_editor_list(false)),
            });
        } else if (
            mode == Mode::editor_menu &&
            editor_menu_dropped
        ) {
            static constexpr std::array<int, 7> offsets{
                0, 9, 18, 29, 41, 52, 63
            };
            int menu_width = 2;
            for (const auto& item : editor_menu_items()[editor_menu_category]) {
                menu_width = std::max(
                    menu_width,
                    ftxui::string_width(item.label) + 3
                );
            }
            const auto popup_left = std::min(
                offsets[editor_menu_category],
                std::max(0, Terminal::Size().dimx - menu_width)
            );
            auto positioned_menu = vbox({
                filler() | size(HEIGHT, EQUAL, 1),
                hbox({
                    filler() |
                        size(
                            WIDTH,
                            EQUAL,
                            popup_left
                        ),
                    render_editor_menu() |
                        size(WIDTH, LESS_THAN, Terminal::Size().dimx) |
                        clear_under,
                    filler(),
                }),
                filler(),
            });
            document = dbox({
                document,
                positioned_menu,
            });
        } else if (mode == Mode::help) {
            document = dbox({
                document,
                centered_modal(render_help()),
            });
        } else if (mode == Mode::user_menu) {
            document = dbox({
                document,
                centered_modal(render_menu()),
            });
        } else if (mode == Mode::bookmarks) {
            document = dbox({
                document,
                centered_modal(render_bookmarks()),
            });
        } else if (mode == Mode::information) {
            document = dbox({
                document,
                centered_modal(render_information()),
            });
        } else if (mode == Mode::operation_dialog) {
            document = dbox({
                document,
                centered_modal(render_operation_dialog()),
            });
        } else if (mode == Mode::delete_dialog) {
            document = dbox({
                document,
                centered_modal(render_delete_dialog()),
            });
        } else if (mode == Mode::operation_progress) {
            document = dbox({
                document,
                centered_modal(render_operation_progress()),
            });
        } else if (mode == Mode::conflict_dialog ||
                   mode == Mode::operation_error_dialog) {
            document = dbox({
                document,
                centered_modal(render_conflict_dialog()),
            });
        } else if (mode == Mode::jobs) {
            document = dbox({
                document,
                centered_modal(render_jobs()),
            });
        } else if (mode == Mode::compare_dialog) {
            document = dbox({
                document,
                centered_modal(render_compare_dialog()),
            });
        } else if (
            mode == Mode::pulldown &&
            pulldown_dropped
        ) {
            static constexpr std::array<int, 5> offsets{
                0, 9, 18, 30, 42
            };
            auto positioned_menu = vbox({
                filler() | size(HEIGHT, EQUAL, 1),
                hbox({
                    filler() |
                        size(
                            WIDTH,
                            EQUAL,
                            offsets[pulldown_category]
                        ),
                    render_pulldown() | clear_under,
                    filler(),
                }),
                filler(),
            });
            document = dbox({
                document,
                positioned_menu,
            });
        }
        return document |
               color(Color::Grey85) |
               bgcolor(Color::Grey3);
    }

    bool handle_prompt(const ftxui::Event& event) {
        const bool save_as = prompt_action == PromptAction::editor_save_as;
        const bool viewer_goto = prompt_action == PromptAction::viewer_goto;
        const bool options = save_as || viewer_goto;
        if (modal_cancel(event)) {
            cancel_prompt();
            return true;
        }
        if (event == ftxui::Event::Tab) {
            prompt_focus = options
                               ? std::array<std::size_t, 4>{3U, 2U, 0U, 1U}[prompt_focus]
                               : (prompt_focus + 1U) % 3U;
            return true;
        }
        if (event == ftxui::Event::TabReverse) {
            prompt_focus = options
                               ? std::array<std::size_t, 4>{2U, 3U, 1U, 0U}[prompt_focus]
                               : (prompt_focus + 2U) % 3U;
            return true;
        }
        if (event.is_mouse()) {
            auto mouse_event = event;
            const auto& mouse = mouse_event.mouse();
            auto& input = prompt_inputs[prompt_action];
            if (input.dragging ||
                (prompt_focus == 0U && mouse.motion != ftxui::Mouse::Pressed)) {
                return input_mouse(prompt_value, input, mouse);
            }
            if (mouse.button == ftxui::Mouse::Left &&
                mouse.motion == ftxui::Mouse::Pressed) {
                for (std::size_t index = 0;
                     index < (options ? 4U : 3U);
                     ++index) {
                    if (!prompt_boxes[index].Contain(
                            mouse.x,
                            mouse.y
                        )) {
                        continue;
                    }
                    prompt_focus = index;
                    if (index == 0U) {
                        input_mouse(prompt_value, input, mouse);
                    } else if (index == 1U) {
                        remember_input(input, prompt_value);
                        execute_prompt();
                    } else if (index == 2U) {
                        cancel_prompt();
                    } else {
                        return handle_prompt(ftxui::Event::ArrowRight);
                    }
                    break;
                }
            }
            return true;
        }
        if (options && prompt_focus == 3U &&
            (event == ftxui::Event::ArrowLeft ||
             event == ftxui::Event::ArrowRight ||
             event == ftxui::Event::Character(' '))) {
            const auto current = save_as
                                     ? static_cast<std::size_t>(editor_save_line_ending)
                                     : static_cast<std::size_t>(viewer_goto_mode);
            const auto next = (current + (event == ftxui::Event::ArrowLeft ? 3U : 1U)) % 4U;
            if (save_as) {
                editor_save_line_ending = static_cast<EditorLineEnding>(next);
            } else {
                viewer_goto_mode = static_cast<ViewerGotoMode>(next);
                prompt_error.clear();
            }
            return true;
        }
        if (event == ftxui::Event::Return ||
            (prompt_focus != 0U &&
             event == ftxui::Event::Character(' '))) {
            if (prompt_focus == 2U) {
                cancel_prompt();
            } else {
                remember_input(prompt_inputs[prompt_action], prompt_value);
                execute_prompt();
            }
            return true;
        }
        if (prompt_focus != 0U) {
            return false;
        }
        if (event == ftxui::Event::Special("\x1B\t")) {
            const bool path =
                prompt_action == PromptAction::view_file ||
                prompt_action == PromptAction::rename ||
                prompt_action == PromptAction::mkdir ||
                prompt_action == PromptAction::quick_cd ||
                prompt_action == PromptAction::editor_open ||
                prompt_action == PromptAction::editor_new ||
                prompt_action == PromptAction::editor_save_as ||
                prompt_action == PromptAction::hardlink ||
                prompt_action == PromptAction::symlink ||
                prompt_action == PromptAction::relative_symlink;
            if (path) {
                try {
                    complete_path(
                        prompt_value,
                        prompt_inputs[prompt_action],
                        mode_before_prompt == Mode::editor
                            ? std::filesystem::path(editor.path()).parent_path().string()
                            : active().path()
                    );
                    prompt_error.clear();
                } catch (const std::exception& error) {
                    prompt_error = error.what();
                }
            } else {
                prompt_inputs[prompt_action].message =
                    "Path completion is not available for this field";
            }
            return true;
        }
        const auto previous = prompt_value;
        const auto handled = edit_input(
            prompt_value, prompt_inputs[prompt_action], event
        );
        if (prompt_value != previous) {
            prompt_error.clear();
        }
        return handled;
    }

    void close_viewer() {
        viewer.close();
        mode = viewer_return_mode;
        viewer_return_mode = Mode::panels;
        viewer_hex_first_column = 0;
        if (exit_after_viewer) {
            exit_terminal();
        }
    }

    void request_viewer_close() {
        if (viewer.dirty()) {
            viewer_choice = 0;
            mode = Mode::viewer_close_dialog;
        } else {
            close_viewer();
        }
    }

    bool handle_viewer_close_dialog(const ftxui::Event& event) {
        const auto input = handle_button_input(
            event, viewer_choice, viewer_choice_boxes, "sdc"
        );
        if (input == ButtonInput::cancel ||
            (input == ButtonInput::activate && viewer_choice == 2U)) {
            mode = Mode::viewer;
        } else if (input == ButtonInput::activate) {
            try {
                if (viewer_choice == 0U) {
                    viewer.save_changes();
                } else {
                    viewer.discard_changes();
                }
                close_viewer();
            } catch (const std::exception& error) {
                viewer_message_title = "Viewer error";
                viewer_message = error.what();
                mode = Mode::viewer_message;
            }
        }
        return input != ButtonInput::ignored;
    }

    void run_viewer_search(const bool quick, const bool wrap) {
        auto& target = quick ? quick_viewer : viewer;
        const auto& needle = quick ? quick_view_search_pattern : viewer_search_pattern;
        const bool hexadecimal = quick ? quick_view_search_hex : viewer_search_hex;
        viewer_search_quick = quick;
        const auto return_mode = quick ? Mode::panels : Mode::viewer;
        if (wrap) {
            target.go_to_start();
        }
        if (target.find(needle, hexadecimal, {}, false)) {
            status = "Match found";
            mode = return_mode;
        } else if (!wrap && target.search_can_wrap()) {
            viewer_choice = 0;
            mode = Mode::viewer_wrap_dialog;
        } else if (quick) {
            status = "Search string not found";
            mode = return_mode;
        } else {
            viewer_message_title = "Search";
            viewer_message = "Search string not found";
            mode = Mode::viewer_message;
        }
    }

    bool handle_viewer_wrap_dialog(const ftxui::Event& event) {
        const auto input = handle_button_input(
            event, viewer_choice, viewer_wrap_boxes, "yc"
        );
        if (input == ButtonInput::cancel ||
            (input == ButtonInput::activate && viewer_choice == 1U)) {
            mode = viewer_search_quick ? Mode::panels : Mode::viewer;
        } else if (input == ButtonInput::activate) {
            try {
                run_viewer_search(viewer_search_quick, true);
            } catch (const std::exception& error) {
                if (viewer_search_quick) {
                    set_error(error);
                    mode = Mode::panels;
                } else {
                    viewer_message_title = "Viewer error";
                    viewer_message = error.what();
                    mode = Mode::viewer_message;
                }
            }
        }
        return input != ButtonInput::ignored;
    }

    void move_viewer_hex(const long long delta) {
        viewer.move_hex_cursor(delta);
        const auto cursor = viewer.hex_cursor();
        const auto offset = viewer.offset();
        const auto height = viewer_dimensions().second;
        if (cursor < offset) {
            const auto rows = std::min<std::uint64_t>(
                (offset - cursor + 15U) / 16U,
                static_cast<std::uint64_t>(std::numeric_limits<int>::max())
            );
            viewer.scroll_lines(-static_cast<int>(rows), viewer_dimensions().first);
        } else if ((cursor - offset) / 16U >= height) {
            const auto rows = std::min<std::uint64_t>(
                (cursor - offset) / 16U - height + 1U,
                static_cast<std::uint64_t>(std::numeric_limits<int>::max())
            );
            viewer.scroll_lines(static_cast<int>(rows), viewer_dimensions().first);
        }
    }

    bool handle_viewer(const ftxui::Event& event) {
        using ftxui::Event;
        const auto [width, height] = viewer_dimensions();
        const auto character = [&](const char value) {
            return event == Event::Character(value);
        };
        try {
            if (event == Event::Escape || event == Event::F3 || event == Event::F10) {
                request_viewer_close();
                return true;
            }
            if (event.is_mouse()) {
                auto copy = event;
                const auto& mouse = copy.mouse();
                if (mouse.button == ftxui::Mouse::Left &&
                    mouse.motion == ftxui::Mouse::Pressed && mouse.y == 0) {
                    start_prompt(PromptAction::viewer_path, "Full filename", viewer.path());
                    return true;
                }
                if (mouse.button == ftxui::Mouse::WheelUp ||
                    mouse.button == ftxui::Mouse::WheelDown) {
                    const auto amount = mouse.button == ftxui::Mouse::WheelUp ? -3 : 3;
                    if (viewer.hex()) {
                        viewer.scroll_hex_lines(amount);
                    } else {
                        viewer.scroll_lines(amount, width);
                    }
                    return true;
                }
                if (viewer.hex() && viewer_rendered_path == viewer.path() &&
                    mouse.button == ftxui::Mouse::Left &&
                    mouse.motion == ftxui::Mouse::Pressed) {
                    for (const auto& row : viewer_rows) {
                        if (!row.box.Contain(mouse.x, mouse.y)) {
                            continue;
                        }
                        const auto column = static_cast<std::size_t>(
                            mouse.x - row.box.x_min
                        ) + row.first_column;
                        const auto ascii_column = row.hex_column + 49U;
                        if (column >= ascii_column && column - ascii_column < row.bytes) {
                            viewer.set_hex_cursor(row.offset + column - ascii_column, true);
                        } else if (column >= row.hex_column &&
                                   column - row.hex_column < row.bytes * 3U &&
                                   (column - row.hex_column) % 3U < 2U) {
                            const auto cell = column - row.hex_column;
                            viewer.set_hex_cursor(row.offset + cell / 3U, false, cell % 3U == 1U);
                        }
                        return true;
                    }
                }
                return false;
            }
            if (event == Event::F1) {
                viewer_message_title = "Viewer";
                viewer_message =
                    "File: " + viewer.path() +
                    ". F2 wrap/hex edit, F4 text/hex, F5 line/percent/offset, F6 save hex edits, "
                    "F7 search, n repeat, q quit. Wheel and paging scroll. "
                    "Click a hex/ASCII byte to place the cursor, or the header for the full filename.";
                mode = Mode::viewer_message;
                return true;
            }
            if (event == Event::F2) {
                if (viewer.hex()) {
                    viewer.toggle_hex_editing();
                } else {
                    viewer.set_wrap(!viewer.wrap());
                }
                return true;
            }
            if (event == Event::F4) {
                viewer.toggle_hex();
                viewer_hex_first_column = 0;
                return true;
            }
            if (event == Event::F5) {
                viewer_goto_mode = viewer.hex() ? ViewerGotoMode::hexadecimal : ViewerGotoMode::line;
                std::ostringstream position;
                if (viewer.hex()) {
                    position << "0x" << std::hex << viewer.hex_cursor();
                } else {
                    position << viewer.line_number();
                }
                start_prompt(
                    PromptAction::viewer_goto, "Go to", position.str()
                );
                return true;
            }
            if (event == Event::F6 && viewer.hex()) {
                viewer.save_changes();
                status = "Saved";
                return true;
            }
            if (viewer.hex() && event == Event::Tab) {
                viewer.toggle_hex_navigation();
                return true;
            }
            if (event == Event::F7 ||
                (!viewer.hex_editing() && character('/'))) {
                start_prompt(
                    PromptAction::viewer_find,
                    viewer.hex() ? "Find hexadecimal bytes" : "Find in file",
                    viewer_search_hex == viewer.hex() ? viewer_search_needle : ""
                );
                return true;
            }
            if (viewer.hex() && viewer.hex_editing() && event.is_character()) {
                if (viewer.hex_ascii()) {
                    for (const auto byte : event.character()) {
                        viewer.edit_ascii_byte(byte);
                    }
                } else if (event.character().size() == 1U &&
                           std::isxdigit(static_cast<unsigned char>(
                               event.character().front()
                           )) != 0) {
                    viewer.edit_hex_digit(event.character().front());
                }
                return true;
            }
            if (character('q')) {
                request_viewer_close();
                return true;
            }
            if (event == Event::Special("\x1B[31~") ||
                event == Event::Special("\x1B[18;2~") ||
                character('n') || event == Event::CtrlS) {
                if (viewer_search_needle.empty()) {
                    start_prompt(
                        PromptAction::viewer_find,
                        viewer.hex() ? "Find hexadecimal bytes" : "Find in file"
                    );
                } else {
                    run_viewer_search(false, false);
                }
                return true;
            }
            const bool top =
                event == Event::Special("\x1B[1;5H") ||
                event == Event::Special("\x1B[5;5~") ||
                event == Event::Special("\x1B<") || character('g');
            const bool bottom =
                event == Event::Special("\x1B[1;5F") ||
                event == Event::Special("\x1B[6;5~") ||
                event == Event::Special("\x1B>") || character('G');
            const bool page_down = event == Event::PageDown || event == Event::CtrlV;
            const bool page_up = event == Event::PageUp || event == Event::AltV;
            if (viewer.hex()) {
                const auto page = static_cast<long long>(std::max<std::size_t>(1U, height - 1U)) * 16LL;
                if (top) {
                    viewer.goto_offset(0);
                } else if (bottom) {
                    viewer.goto_offset(viewer.size() == 0U ? 0U : viewer.size() - 1U);
                    viewer.go_to_end(width, height);
                } else if (event == Event::Home || event == Event::CtrlA) {
                    move_viewer_hex(-static_cast<long long>(viewer.hex_cursor() % 16U));
                } else if (event == Event::End || event == Event::CtrlE) {
                    move_viewer_hex(15LL - static_cast<long long>(viewer.hex_cursor() % 16U));
                } else if (event == Event::ArrowLeft || character('b')) {
                    move_viewer_hex(-1);
                } else if (event == Event::ArrowRight || character('f')) {
                    move_viewer_hex(1);
                } else if (event == Event::ArrowUp || character('k') || character('y')) {
                    move_viewer_hex(-16);
                } else if (event == Event::ArrowDown || character('j') || event == Event::Delete) {
                    move_viewer_hex(16);
                } else if (page_up) {
                    move_viewer_hex(-page);
                } else if (page_down) {
                    move_viewer_hex(page);
                } else {
                    return false;
                }
                return true;
            }
            if (top || event == Event::Home) {
                viewer.go_to_start();
            } else if (bottom || event == Event::End) {
                viewer.go_to_end(width, height);
            } else if (event == Event::ArrowDown || character('j') || character('e') ||
                       event == Event::Delete || event == Event::Return || event == Event::CtrlN) {
                viewer.scroll_lines(1, width);
            } else if (event == Event::ArrowUp || character('k') || character('y') ||
                       event == Event::Insert || event == Event::CtrlP) {
                viewer.scroll_lines(-1, width);
            } else if (event == Event::ArrowLeft || character('h')) {
                viewer.scroll_columns(-1);
            } else if (event == Event::ArrowRight || character('l')) {
                viewer.scroll_columns(1);
            } else if (event == Event::ArrowLeftCtrl) {
                viewer.scroll_columns(-10);
            } else if (event == Event::ArrowRightCtrl) {
                viewer.scroll_columns(10);
            } else if (page_down || character('f') || character(' ')) {
                viewer.page(1, width, height);
            } else if (page_up || character('b') || event == Event::Backspace) {
                viewer.page(-1, width, height);
            } else if (character('d') || character('u')) {
                viewer.scroll_lines(
                    (character('u') ? -1 : 1) *
                        static_cast<int>(std::max<std::size_t>(1U, height / 2U)),
                    width
                );
            } else if (event == Event::CtrlA) {
                viewer.scroll_columns(-std::numeric_limits<int>::max());
            } else {
                return false;
            }
            return true;
        } catch (const std::exception& error) {
            viewer_message_title = "Viewer error";
            viewer_message = error.what();
            mode = Mode::viewer_message;
            return true;
        }
    }

    void show_editor_message(
        std::string title,
        std::string message
    ) {
        editor_message_title = std::move(title);
        editor_error_message = std::move(message);
        mode = Mode::editor_error;
    }

    void show_editor_error(const std::exception& error) {
        status = "Error: " + std::string(error.what());
        show_editor_message("Editor error", error.what());
    }

    void start_editor_options_dialog() {
        editor_options_tab_width =
            std::to_string(editor.tab_width());
        editor_options_indent_width =
            std::to_string(editor_indent_width);
        editor_options_auto_indent = editor_auto_indent;
        editor_options_show_whitespace = editor_show_whitespace;
        editor_options_error.clear();
        editor_options_focus = 0;
        editor_tab_input.reset(editor_options_tab_width);
        editor_tab_input.anchor = 0U;
        editor_indent_input.reset(editor_options_indent_width);
        editor_indent_input.anchor = 0U;
        mode = Mode::editor_options_dialog;
    }

    void start_editor_search_dialog(const bool replace) {
        editor_dialog_query = editor.search.needle;
        editor_dialog_replacement =
            replace ? editor_replace_text : std::string{};
        editor_query_input.reset(editor_dialog_query, true);
        editor_replacement_input.reset(editor_dialog_replacement, true);
        editor_dialog_scope = editor.selection();
        editor_dialog_options = editor.search.options;
        editor_dialog_in_selection =
            editor.search.in_selection && editor_dialog_scope.has_value();
        if (!editor_dialog_scope) {
            editor_dialog_in_selection = false;
        }
        editor_dialog_focus = 0;
        mode = replace
                   ? Mode::editor_replace_dialog
                   : Mode::editor_search_dialog;
    }

    void commit_editor_search(
        const std::string& query,
        const EditorSearchOptions& options,
        const bool in_selection,
        const std::optional<std::pair<
            EditorPosition,
            EditorPosition
        >> scope
    ) {
        editor.search.needle = query;
        editor.search.options = options;
        editor.search.in_selection = in_selection;
        editor.search.scope = scope;
        editor.search.change_id = editor.change_id();
    }

    void reset_editor_view_state() {
        editor_line_boxes.clear();
        editor_line_first_columns.clear();
        editor_mouse_dragging = false;
        editor_mouse_marking = false;
        editor_shift_marking = false;
        editor_column_marking = false;
        syntax_change_id =
            std::numeric_limits<std::uint64_t>::max();
        syntax_path.clear();
        syntax_spans.clear();
    }

    void remember_editor_path(const std::string& path) {
        if (path.empty()) {
            return;
        }
        std::erase(editor_history, path);
        editor_history.insert(editor_history.begin(), path);
        if (editor_history.size() > 20U) {
            editor_history.resize(20U);
        }
    }

    void open_editor_buffer(
        const ProviderPtr& provider,
        const std::string& path,
        const bool create
    ) {
        const bool same_session =
            mode == Mode::editor || mode == Mode::editor_menu ||
            mode == Mode::editor_history ||
            (mode == Mode::prompt && mode_before_prompt == Mode::editor);
        if (!create && same_session && editor.open()) {
            const auto normalized = provider->normalize(path);
            if (provider->normalize(editor.path()) == normalized) {
                return;
            }
            for (auto index = editor_session_base; index < editor_buffers.size(); ++index) {
                if (provider->normalize(editor_buffers[index].path()) == normalized) {
                    std::swap(editor, editor_buffers[index]);
                    remember_editor_path(editor.path());
                    reset_editor_view_state();
                    return;
                }
            }
        }
        EditorBuffer next;
        next.set_tab_width(editor.tab_width());
        if (create || !provider->exists(path)) {
            next.create(provider, path);
        } else {
            next.open(provider, path);
        }
        next.search.change_id = next.change_id();
        if (editor.open()) {
            editor_buffers.push_back(std::move(editor));
        }
        editor = std::move(next);
        remember_editor_path(editor.path());
        reset_editor_view_state();
    }

    bool switch_editor_buffer(const bool forward) {
        if (editor_buffers.size() <= editor_session_base) {
            status = "No other editor buffer";
            return false;
        }
        if (forward) {
            editor_buffers.push_back(std::move(editor));
            editor = std::move(editor_buffers[editor_session_base]);
            editor_buffers.erase(
                editor_buffers.begin() + static_cast<std::ptrdiff_t>(editor_session_base)
            );
        } else {
            editor_buffers.insert(
                editor_buffers.begin() + static_cast<std::ptrdiff_t>(editor_session_base),
                std::move(editor)
            );
            editor = std::move(editor_buffers.back());
            editor_buffers.pop_back();
        }
        remember_editor_path(editor.path());
        reset_editor_view_state();
        status = "Buffer: " + editor.path();
        return true;
    }

    void save_editor() {
        editor_save_choice = 0;
        mode = Mode::editor_save_dialog;
    }

    void confirm_editor_save() {
        begin_editor_buffer_save(editor, EditorSavePurpose::save);
    }

    EditorBuffer& editor_quit_buffer() {
        return editor_quit_index == 0U
                   ? editor
                   : editor_buffers[
                         editor_session_base + editor_quit_index - 1U
                     ];
    }

    void accept_editor_close() {
        if (editor_quit_requested) {
            ++editor_quit_index;
            continue_editor_quit();
        } else {
            close_editor();
        }
    }

    void complete_editor_buffer_save(const bool preserve_links) {
        const auto purpose = editor_save_purpose;
        try {
            if (editor_save_buffer == nullptr) {
                throw std::runtime_error("no buffer to save");
            }
            if (purpose == EditorSavePurpose::save_as && editor_save_overwrite &&
                local->stat_follow(editor_save_path, {}).revision != editor_overwrite_revision) {
                throw std::runtime_error("Save As destination changed while awaiting confirmation");
            }
            if (preserve_links) {
                if (purpose == EditorSavePurpose::save_as) {
                    editor_save_buffer->save_as_preserving_links(
                        editor_save_path, editor_save_overwrite, editor_save_line_ending
                    );
                } else {
                    editor_save_buffer->save_preserving_links(false);
                }
            } else if (purpose == EditorSavePurpose::save_as) {
                editor_save_buffer->save_as(
                    editor_save_path, editor_save_overwrite, editor_save_line_ending
                );
            } else {
                editor_save_buffer->save(false);
            }
            remember_editor_path(editor_save_buffer->path());
            editor_save_buffer = nullptr;
            prompt_action = PromptAction::none;
            prompt_value.clear();
            mode = Mode::editor;
            status = "Saved";
            if (purpose == EditorSavePurpose::close || purpose == EditorSavePurpose::quit) {
                accept_editor_close();
            }
        } catch (const std::exception& error) {
            editor_save_buffer = nullptr;
            editor_quit_requested = false;
            show_editor_error(error);
        }
    }

    void begin_editor_buffer_save(
        EditorBuffer& buffer,
        const EditorSavePurpose purpose
    ) {
        editor_save_buffer = &buffer;
        editor_save_purpose = purpose;
        try {
            if (buffer.hardlink_count() > 1U) {
                editor_link_choice = 0;
                mode = Mode::editor_link_dialog;
            } else {
                complete_editor_buffer_save(false);
            }
        } catch (const std::exception& error) {
            editor_save_buffer = nullptr;
            editor_quit_requested = false;
            show_editor_error(error);
        }
    }

    void request_editor_save_as() {
        auto path = std::filesystem::path(prompt_value);
        if (path.is_relative()) {
            path = std::filesystem::path(editor.path()).parent_path() / path;
        }
        editor_save_path = local->normalize(path.string());
        editor_save_overwrite = false;
        editor_save_buffer = &editor;
        editor_save_purpose = EditorSavePurpose::save_as;
        if (editor_save_path == local->normalize(editor.path())) {
            begin_editor_buffer_save(editor, EditorSavePurpose::save_as);
        } else if (local->exists(editor_save_path)) {
            const auto target = local->stat_follow(editor_save_path, {});
            if (target.kind != EntryKind::file) {
                throw std::runtime_error(
                    "Save As target is not a regular file: " + editor_save_path
                );
            }
            editor_overwrite_revision = target.revision;
            editor_overwrite_choice = 1;
            mode = Mode::editor_overwrite_dialog;
        } else {
            complete_editor_buffer_save(false);
        }
    }

    bool handle_editor_link_dialog(const ftxui::Event& event) {
        auto key = event;
        if (event == ftxui::Event::ArrowUp) {
            key = ftxui::Event::ArrowLeft;
        } else if (event == ftxui::Event::ArrowDown) {
            key = ftxui::Event::ArrowRight;
        }
        const auto input = handle_button_input(key, editor_link_choice, editor_link_boxes, "dpc");
        if (input == ButtonInput::cancel ||
            (input == ButtonInput::activate && editor_link_choice == 2U)) {
            editor_quit_requested = false;
            editor_save_buffer = nullptr;
            mode = editor_save_purpose == EditorSavePurpose::save_as
                       ? Mode::prompt
                       : Mode::editor;
        } else if (input == ButtonInput::activate) {
            complete_editor_buffer_save(editor_link_choice == 1U);
        }
        return input != ButtonInput::ignored;
    }

    bool handle_editor_overwrite_dialog(const ftxui::Event& event) {
        const auto input = handle_button_input(
            event, editor_overwrite_choice, editor_overwrite_boxes, "rc"
        );
        if (input == ButtonInput::cancel ||
            (input == ButtonInput::activate && editor_overwrite_choice == 1U)) {
            editor_save_buffer = nullptr;
            mode = Mode::prompt;
        } else if (input == ButtonInput::activate) {
            try {
                if (local->stat_follow(editor_save_path, {}).revision != editor_overwrite_revision) {
                    throw std::runtime_error("Save As destination changed while awaiting confirmation");
                }
                editor_save_overwrite = true;
                if (std::filesystem::hard_link_count(editor_save_path) > 1U) {
                    editor_link_choice = 0;
                    mode = Mode::editor_link_dialog;
                } else {
                    complete_editor_buffer_save(false);
                }
            } catch (const std::exception& error) {
                editor_save_buffer = nullptr;
                show_editor_error(error);
            }
        }
        return input != ButtonInput::ignored;
    }

    void close_editor() {
        reset_editor_view_state();
        editor.close();
        editor.search = {};
        if (editor_buffers.size() > editor_session_base) {
            editor = std::move(editor_buffers.back());
            editor_buffers.pop_back();
            remember_editor_path(editor.path());
            mode = Mode::editor;
            status = "Buffer: " + editor.path();
            return;
        }
        const auto return_mode = editor_return_mode;
        editor_return_mode = Mode::panels;
        editor_session_base = 0;
        if (!editor_buffers.empty()) {
            editor = std::move(editor_buffers.back());
            editor_buffers.pop_back();
        }
        mode = return_mode;
        if (exit_after_editor && return_mode == Mode::panels) {
            exit_terminal();
            return;
        }
        if (return_mode != Mode::panels) {
            return;
        }
        try {
            refresh();
            status = "Editor closed";
        } catch (const std::exception& error) {
            set_error(error);
        }
    }

    void request_editor_close() {
        editor_quit_requested = false;
        if (editor.dirty()) {
            editor_close_choice = 0;
            mode = Mode::editor_close_dialog;
        } else {
            close_editor();
        }
    }

    void continue_editor_quit() {
        while (editor_quit_index <= editor_buffers.size() - editor_session_base) {
            const auto& buffer = editor_quit_buffer();
            if (buffer.dirty()) {
                editor_close_choice = 0;
                mode = Mode::editor_close_dialog;
                return;
            }
            ++editor_quit_index;
        }
        editor_quit_requested = false;
        editor_buffers.erase(
            editor_buffers.begin() + static_cast<std::ptrdiff_t>(editor_session_base),
            editor_buffers.end()
        );
        close_editor();
    }

    void request_editor_quit() {
        editor_quit_requested = true;
        editor_quit_index = 0;
        continue_editor_quit();
    }

    void store_editor_clipboard(const bool cut) {
        if (const auto selected = editor.selected_text()) {
            editor_clipboard = *selected;
            editor_clipboard_column = editor.column_selection();
            try {
                save_editor_clipboard(
                    editor_clipboard_path(),
                    editor_clipboard,
                    editor_clipboard_column
                );
                if (cut) {
                    static_cast<void>(editor.delete_selection());
                }
                status = cut
                             ? "Block cut to clipboard"
                             : "Block copied to clipboard";
            } catch (const std::exception& error) {
                show_editor_error(error);
            }
        } else {
            status = "No marked block";
        }
    }

    void paste_editor_clipboard() {
        try {
            const auto path = editor_clipboard_path();
            bool persisted_column = false;
            const auto persisted = load_editor_clipboard(
                path,
                &persisted_column
            );
            if (std::filesystem::exists(path)) {
                editor_clipboard_column = persisted_column;
                editor_clipboard = persisted;
            }
            if (editor_clipboard.empty()) {
                status = "Clipboard is empty";
                return;
            }
            if (editor_clipboard_column) {
                editor.paste_column(editor_clipboard);
            } else {
                editor.paste(editor_clipboard);
            }
            status = "Clipboard pasted";
        } catch (const std::exception& error) {
            show_editor_error(error);
        }
    }

    void repeat_editor_search() {
        if (editor.search.needle.empty()) {
            start_editor_search_dialog(false);
            return;
        }
        if (editor.search.change_id != editor.change_id()) {
            editor.search.scope.reset();
            editor.search.in_selection = false;
            editor.search.change_id = editor.change_id();
        }
        try {
            if (!editor.find(
                    editor.search.needle,
                    editor.search.options,
                    false,
                    false,
                    editor.search.scope
                )) {
                show_editor_message(
                    "Search",
                    "Search string not found"
                );
            } else {
                center_editor();
            }
        } catch (const std::exception& error) {
            show_editor_error(error);
        }
    }

    void execute_editor_menu_item() {
        const auto category = editor_menu_category;
        const auto item =
            editor_menu_items()[category][editor_menu_item].action;
        if (item < 0) {
            return;
        }
        mode = Mode::editor;
        if (category == 0U) {
            if (item == 0U) {
                start_prompt(
                    PromptAction::editor_open,
                    "Open file",
                    editor.path()
                );
            } else if (item == 1U) {
                start_prompt(
                    PromptAction::editor_new,
                    "New file",
                    (
                        std::filesystem::path(editor.path())
                            .parent_path() /
                        "new-file"
                    ).string()
                );
            } else if (item == 2U) {
                request_editor_close();
            } else if (item == 3U) {
                save_editor();
            } else if (item == 4U) {
                start_prompt(
                    PromptAction::editor_save_as,
                    "Save as",
                    editor.path()
                );
            } else if (item == 6U) {
                request_editor_quit();
            } else {
                editor_list_cursor = 0;
                mode = Mode::editor_history;
            }
            return;
        }
        if (category == 1U) {
            switch (item) {
                case 0:
                    status =
                        editor.undo() ? "Undo" : "Nothing to undo";
                    break;
                case 1:
                    status =
                        editor.redo() ? "Redo" : "Nothing to redo";
                    break;
                case 2:
                    editor.toggle_overwrite();
                    status = editor.overwrite()
                                 ? "Overwrite mode"
                                 : "Insert mode";
                    break;
                case 3:
                    editor.toggle_mark();
                    status = editor.marking()
                                 ? "Marking block"
                                 : "Block marked";
                    break;
                case 4:
                    editor.toggle_column_mark();
                    status = editor.marking()
                                 ? "Marking columns"
                                 : "Column block marked";
                    break;
                case 5:
                    editor.select_all();
                    status = "All text marked";
                    break;
                case 6:
                    editor.clear_selection();
                    status = "Block unmarked";
                    break;
                case 7:
                    status = editor.copy_selection()
                                 ? "Block copied"
                                 : "No marked block";
                    break;
                case 8:
                    status = editor.move_selection()
                                 ? "Block moved"
                                 : "No movable block";
                    break;
                case 9:
                    editor.delete_block_or_line();
                    status = "Deleted";
                    break;
                case 10:
                    store_editor_clipboard(false);
                    break;
                case 11:
                    store_editor_clipboard(true);
                    break;
                case 12:
                    paste_editor_clipboard();
                    break;
                case 13:
                    editor.goto_line(1);
                    break;
                case 14:
                    editor.goto_line(editor.lines().size());
                    break;
                default:
                    break;
            }
            return;
        }
        if (category == 2U) {
            if (item == 0U) {
                start_editor_search_dialog(false);
            } else if (item == 1U) {
                repeat_editor_search();
            } else {
                start_editor_search_dialog(true);
            }
            return;
        }
        if (category == 3U) {
            if (item == 0U) {
                start_prompt(
                    PromptAction::editor_goto,
                    "Go to line",
                    std::to_string(editor.row() + 1U)
                );
            } else if (item == 1U) {
                status = editor.match_bracket()
                             ? "Matching bracket"
                             : "No matching bracket";
            } else if (item == 2U) {
                editor_line_numbers = !editor_line_numbers;
                status = editor_line_numbers
                             ? "Line numbers enabled"
                             : "Line numbers disabled";
            } else {
                editor_syntax = !editor_syntax;
                status = editor_syntax
                             ? "Syntax highlighting enabled"
                             : "Syntax highlighting disabled";
            }
            return;
        }
        if (category == 5U) {
            if (item == 0U) {
                static_cast<void>(switch_editor_buffer(true));
            } else if (item == 1U) {
                static_cast<void>(switch_editor_buffer(false));
            } else {
                editor_list_cursor = 0;
                mode = Mode::editor_buffer_list;
            }
            return;
        }
        if (category == 6U && item == 0) {
            start_editor_options_dialog();
            return;
        }
        if (category == 6U && item == 1) {
            try {
                store_panel_options_in_config();
                store_editor_options_in_config();
                config.save(default_config_path());
                status = "Setup saved";
            } catch (const std::exception& error) {
                show_editor_error(error);
            }
            return;
        }
        status = "No command in this menu";
    }

    void reset_editor_menu_item() {
        const auto& items = editor_menu_items()[editor_menu_category];
        const auto match = std::find_if(
            items.begin(),
            items.end(),
            [](const MenuRow& item) {
                return item.action >= 0;
            }
        );
        editor_menu_item =
            match == items.end()
                ? 0U
                : static_cast<std::size_t>(
                      std::distance(items.begin(), match)
                  );
    }

    void move_editor_menu_item(const int direction) {
        const auto& items = editor_menu_items()[editor_menu_category];
        if (items.empty()) {
            editor_menu_item = 0;
            return;
        }
        for (std::size_t count = 0; count < items.size(); ++count) {
            editor_menu_item =
                direction > 0
                    ? (editor_menu_item + 1U) % items.size()
                    : (editor_menu_item + items.size() - 1U) %
                          items.size();
            if (items[editor_menu_item].action >= 0) {
                return;
            }
        }
    }

    bool handle_editor_menu(const ftxui::Event& event) {
        if (event.is_mouse()) {
            auto mouse_event = event;
            const auto& mouse = mouse_event.mouse();
            if (mouse.motion == ftxui::Mouse::Released) {
                return true;
            }
            for (std::size_t index = 0;
                 index < editor_menu_category_boxes.size();
                 ++index) {
                if (editor_menu_category_boxes[index].Contain(mouse.x, mouse.y) &&
                    (mouse.motion == ftxui::Mouse::Moved ||
                     mouse.button == ftxui::Mouse::Left)) {
                    if (editor_menu_category != index || !editor_menu_dropped) {
                        editor_menu_category = index;
                        reset_editor_menu_item();
                    }
                    editor_menu_dropped = true;
                    return true;
                }
            }
            if (mouse.button == ftxui::Mouse::WheelUp ||
                mouse.button == ftxui::Mouse::WheelDown) {
                move_editor_menu_item(
                    mouse.button == ftxui::Mouse::WheelUp ? -1 : 1
                );
                return true;
            }
            if (editor_menu_dropped) {
                const auto& items = editor_menu_items()[editor_menu_category];
                for (std::size_t index = 0;
                     index < editor_menu_item_boxes.size();
                     ++index) {
                    if (!editor_menu_item_boxes[index].Contain(mouse.x, mouse.y)) {
                        continue;
                    }
                    if (items[index].action == -2) {
                        return true;
                    }
                    editor_menu_item = index;
                    if (mouse.button == ftxui::Mouse::Left &&
                        mouse.motion == ftxui::Mouse::Pressed &&
                        items[index].action >= 0) {
                        execute_editor_menu_item();
                    }
                    return true;
                }
            }
            if (mouse.button == ftxui::Mouse::Left &&
                mouse.motion == ftxui::Mouse::Pressed) {
                mode = Mode::editor;
            }
            return true;
        }
        if (modal_cancel(event) || event == ftxui::Event::F9) {
            mode = Mode::editor;
            return true;
        }
        if (event == ftxui::Event::ArrowLeft) {
            editor_menu_category =
                (
                    editor_menu_category +
                    editor_menu_items().size() - 1U
                ) %
                editor_menu_items().size();
            reset_editor_menu_item();
            return true;
        }
        if (event == ftxui::Event::ArrowRight ||
            event == ftxui::Event::Tab) {
            editor_menu_category =
                (editor_menu_category + 1U) %
                editor_menu_items().size();
            reset_editor_menu_item();
            return true;
        }
        if (event == ftxui::Event::ArrowUp) {
            if (!editor_menu_dropped) {
                editor_menu_dropped = true;
                reset_editor_menu_item();
            }
            move_editor_menu_item(-1);
            return true;
        }
        if (event == ftxui::Event::ArrowDown) {
            if (!editor_menu_dropped) {
                editor_menu_dropped = true;
                reset_editor_menu_item();
                return true;
            }
            move_editor_menu_item(1);
            return true;
        }
        if (event == ftxui::Event::Home) {
            editor_menu_dropped = true;
            editor_menu_item = 0;
            reset_editor_menu_item();
            return true;
        }
        if (event == ftxui::Event::End) {
            editor_menu_dropped = true;
            const auto& items =
                editor_menu_items()[editor_menu_category];
            editor_menu_item = items.empty() ? 0U : items.size() - 1U;
            while (editor_menu_item > 0U &&
                   items[editor_menu_item].action < 0) {
                --editor_menu_item;
            }
            return true;
        }
        if (event == ftxui::Event::Return) {
            if (!editor_menu_dropped) {
                editor_menu_dropped = true;
                reset_editor_menu_item();
                return true;
            }
            execute_editor_menu_item();
            return true;
        }
        return false;
    }

    bool handle_editor_options_dialog(const ftxui::Event& event) {
        if (event.is_mouse()) {
            auto mouse_event = event;
            const auto& mouse = mouse_event.mouse();
            if (editor_tab_input.dragging) {
                return input_mouse(editor_options_tab_width, editor_tab_input, mouse);
            }
            if (editor_indent_input.dragging) {
                return input_mouse(
                    editor_options_indent_width, editor_indent_input, mouse
                );
            }
            if (mouse.button == ftxui::Mouse::Left &&
                mouse.motion == ftxui::Mouse::Pressed) {
                for (std::size_t index = 0; index < editor_options_boxes.size(); ++index) {
                    if (!editor_options_boxes[index].Contain(mouse.x, mouse.y)) {
                        continue;
                    }
                    editor_options_focus = index;
                    if (index == 0U) {
                        input_mouse(editor_options_tab_width, editor_tab_input, mouse);
                    } else if (index == 1U) {
                        input_mouse(
                            editor_options_indent_width, editor_indent_input, mouse
                        );
                    } else {
                        return handle_editor_options_dialog(ftxui::Event::Return);
                    }
                    break;
                }
            }
            return true;
        }
        const auto move_focus = [&](const int direction) {
            editor_options_focus =
                direction > 0
                    ? (editor_options_focus + 1U) % 6U
                    : (editor_options_focus + 5U) % 6U;
        };
        if (modal_cancel(event)) {
            mode = Mode::editor;
            return true;
        }
        if (event == ftxui::Event::Tab ||
            event == ftxui::Event::ArrowDown) {
            move_focus(1);
            return true;
        }
        if (event == ftxui::Event::TabReverse ||
            event == ftxui::Event::ArrowUp) {
            move_focus(-1);
            return true;
        }
        if ((event == ftxui::Event::ArrowLeft ||
             event == ftxui::Event::ArrowRight) &&
            editor_options_focus >= 4U) {
            editor_options_focus =
                editor_options_focus == 4U ? 5U : 4U;
            return true;
        }
        if (editor_options_focus <= 1U) {
            auto& value =
                editor_options_focus == 0U
                    ? editor_options_tab_width
                    : editor_options_indent_width;
            if (event == ftxui::Event::Return) {
                move_focus(1);
                return true;
            }
            const auto previous = value;
            const auto handled = edit_input(
                value,
                editor_options_focus == 0U ? editor_tab_input : editor_indent_input,
                event
            );
            if (value != previous) {
                editor_options_error.clear();
            }
            return handled;
        }
        if (editor_options_focus == 2U &&
            (
                event == ftxui::Event::Character(' ') ||
                event == ftxui::Event::Return
            )) {
            editor_options_auto_indent =
                !editor_options_auto_indent;
            return true;
        }
        if (editor_options_focus == 3U &&
            (
                event == ftxui::Event::Character(' ') ||
                event == ftxui::Event::Return
            )) {
            editor_options_show_whitespace =
                !editor_options_show_whitespace;
            return true;
        }
        if (event != ftxui::Event::Return) {
            return false;
        }
        if (editor_options_focus == 5U) {
            mode = Mode::editor;
            return true;
        }
        if (editor_options_focus != 4U) {
            return false;
        }
        const auto parse_width = [](const std::string& value) {
            std::size_t parsed = 0;
            const auto result = std::from_chars(
                value.data(),
                value.data() + value.size(),
                parsed
            );
            return result.ec == std::errc{} &&
                           result.ptr == value.data() + value.size() &&
                           parsed >= 1U &&
                           parsed <= 32U
                       ? std::optional<std::size_t>{parsed}
                       : std::nullopt;
        };
        const auto tab_width =
            parse_width(editor_options_tab_width);
        if (!tab_width) {
            editor_options_error =
                "Tab spacing must be between 1 and 32";
            editor_options_focus = 0;
            return true;
        }
        const auto indent_width =
            parse_width(editor_options_indent_width);
        if (!indent_width) {
            editor_options_error =
                "Indent spacing must be between 1 and 32";
            editor_options_focus = 1;
            return true;
        }
        remember_input(editor_tab_input, editor_options_tab_width);
        remember_input(editor_indent_input, editor_options_indent_width);
        editor.set_tab_width(*tab_width);
        editor_indent_width = *indent_width;
        editor_auto_indent = editor_options_auto_indent;
        editor_show_whitespace = editor_options_show_whitespace;
        status = "Editor options updated";
        mode = Mode::editor;
        return true;
    }

    bool handle_editor_list(const ftxui::Event& event) {
        const bool history = mode == Mode::editor_history;
        const auto count =
            history
                ? editor_history.size()
                : editor_buffers.size() - editor_session_base + 1U;
        const auto input = handle_list_input(
            event, editor_list_cursor, count, editor_list_boxes, editor_list_box
        );
        if (input == ButtonInput::cancel) {
            mode = Mode::editor;
            return true;
        }
        if (input == ButtonInput::changed) {
            return true;
        }
        if (history &&
            event == ftxui::Event::Delete &&
            editor_list_cursor < editor_history.size()) {
            editor_history.erase(
                editor_history.begin() +
                static_cast<std::ptrdiff_t>(
                    editor_list_cursor
                )
            );
            if (editor_list_cursor >= editor_history.size() &&
                editor_list_cursor > 0U) {
                --editor_list_cursor;
            }
            return true;
        }
        if (input != ButtonInput::activate || count == 0U) {
            return false;
        }
        if (history) {
            if (editor_list_cursor >= editor_history.size()) {
                return true;
            }
            const auto path = editor_history[editor_list_cursor];
            if (path == editor.path()) {
                mode = Mode::editor;
                return true;
            }
            try {
                open_editor_buffer(
                    active().provider(),
                    path,
                    false
                );
                mode = Mode::editor;
                status = "File opened";
            } catch (const std::exception& error) {
                show_editor_error(error);
            }
            return true;
        }
        if (editor_list_cursor == 0U) {
            mode = Mode::editor;
            return true;
        }
        std::swap(
            editor,
            editor_buffers[editor_session_base + editor_list_cursor - 1U]
        );
        remember_editor_path(editor.path());
        reset_editor_view_state();
        mode = Mode::editor;
        status = "Buffer: " + editor.path();
        return true;
    }

    bool handle_editor_save_dialog(const ftxui::Event& event) {
        const auto input = handle_button_input(
            event, editor_save_choice, editor_save_boxes, "sc"
        );
        if (input == ButtonInput::cancel) {
            mode = Mode::editor;
            return true;
        }
        if (input != ButtonInput::activate) {
            return input != ButtonInput::ignored;
        }
        if (editor_save_choice == 0U) {
            confirm_editor_save();
        } else {
            mode = Mode::editor;
        }
        return true;
    }

    bool handle_editor_close_dialog(const ftxui::Event& event) {
        const auto input = handle_button_input(
            event, editor_close_choice, editor_close_boxes, "ync"
        );
        if (input == ButtonInput::cancel) {
            editor_quit_requested = false;
            mode = Mode::editor;
            return true;
        }
        if (input != ButtonInput::activate) {
            return input != ButtonInput::ignored;
        }
        if (editor_close_choice == 2U) {
            editor_quit_requested = false;
            mode = Mode::editor;
            return true;
        }
        if (editor_close_choice == 0U) {
            begin_editor_buffer_save(
                editor_quit_requested ? editor_quit_buffer() : editor,
                editor_quit_requested ? EditorSavePurpose::quit : EditorSavePurpose::close
            );
        } else {
            accept_editor_close();
        }
        return true;
    }

    void finish_editor_replace(const std::string& message) {
        status = message;
        editor_replace_needle.clear();
        editor_replace_text.clear();
        editor_replace_choice = 0;
        mode = Mode::editor;
    }

    bool handle_editor_search_dialog(const ftxui::Event& event) {
        const bool replace = mode == Mode::editor_replace_dialog;
        static const std::array<std::size_t, 9> search_focus{
            0, 2, 3, 4, 5, 6, 7, 8, 9
        };
        static const std::array<std::size_t, 9> replace_focus{
            0, 1, 2, 3, 4, 5, 6, 7, 8
        };
        const auto& order = replace ? replace_focus : search_focus;
        const auto move_focus = [&](const int amount, const bool body_only = false) {
            const auto current = std::ranges::find(order, editor_dialog_focus);
            const auto index = current == order.end()
                                   ? std::size_t{0}
                                   : static_cast<std::size_t>(current - order.begin());
            if (body_only) {
                const auto last = replace ? 6U : 5U;
                const auto next = std::clamp(
                    static_cast<long long>(std::min<std::size_t>(index, last)) + amount,
                    0LL, static_cast<long long>(last)
                );
                editor_dialog_focus = order[static_cast<std::size_t>(next)];
            } else {
                editor_dialog_focus = order[
                    (index + order.size() +
                     (amount < 0 ? order.size() - 1U : 1U)) % order.size()
                ];
            }
        };
        if (event.is_mouse()) {
            auto mouse_event = event;
            const auto& mouse = mouse_event.mouse();
            if (mouse.button == ftxui::Mouse::WheelUp ||
                mouse.button == ftxui::Mouse::WheelDown) {
                editor_query_input.dragging = false;
                editor_replacement_input.dragging = false;
                move_focus(mouse.button == ftxui::Mouse::WheelUp ? -2 : 2, true);
                return true;
            }
            if (editor_query_input.dragging) {
                return input_mouse(editor_dialog_query, editor_query_input, mouse);
            }
            if (editor_replacement_input.dragging) {
                return input_mouse(
                    editor_dialog_replacement, editor_replacement_input, mouse
                );
            }
            if (mouse.button == ftxui::Mouse::Left &&
                mouse.motion == ftxui::Mouse::Pressed) {
                for (std::size_t index = 0; index < editor_search_boxes.size(); ++index) {
                    if ((!replace && index == 1U) || (replace && index == 9U) ||
                        (index < 7U &&
                         !editor_search_viewport.Contain(mouse.x, mouse.y)) ||
                        !editor_search_boxes[index].Contain(mouse.x, mouse.y)) {
                        continue;
                    }
                    editor_dialog_focus = index;
                    if (index == 0U) {
                        input_mouse(editor_dialog_query, editor_query_input, mouse);
                    } else if (index == 1U) {
                        input_mouse(
                            editor_dialog_replacement, editor_replacement_input, mouse
                        );
                    } else if (index < 7U) {
                        return handle_editor_search_dialog(
                            index == 2U ? ftxui::Event::ArrowRight
                                        : ftxui::Event::Character(' ')
                        );
                    } else {
                        return handle_editor_search_dialog(ftxui::Event::Return);
                    }
                    break;
                }
            }
            return true;
        }
        if (modal_cancel(event)) {
            mode = Mode::editor;
            return true;
        }
        if (event == ftxui::Event::Tab ||
            event == ftxui::Event::ArrowDown) {
            move_focus(1);
            return true;
        }
        if (event == ftxui::Event::TabReverse ||
            event == ftxui::Event::ArrowUp) {
            move_focus(-1);
            return true;
        }
        if (event == ftxui::Event::PageUp ||
            event == ftxui::Event::PageDown) {
            for (int step = 0; step < 3; ++step) {
                move_focus(event == ftxui::Event::PageUp ? -1 : 1);
            }
            return true;
        }
        if (editor_dialog_focus == 2U &&
            (
                event == ftxui::Event::ArrowLeft ||
                event == ftxui::Event::ArrowRight ||
                event == ftxui::Event::Character(' ')
            )) {
            constexpr auto count = 4U;
            auto value = static_cast<unsigned int>(
                editor_dialog_options.mode
            );
            value =
                event == ftxui::Event::ArrowLeft
                    ? (value + count - 1U) % count
                    : (value + 1U) % count;
            editor_dialog_options.mode =
                static_cast<EditorSearchMode>(value);
            return true;
        }
        if (editor_dialog_focus >= 3U &&
            editor_dialog_focus <= 6U &&
            (
                event == ftxui::Event::Character(' ') ||
                event == ftxui::Event::Return
            )) {
            switch (editor_dialog_focus) {
                case 3:
                    editor_dialog_options.case_sensitive =
                        !editor_dialog_options.case_sensitive;
                    break;
                case 4:
                    editor_dialog_options.backwards =
                        !editor_dialog_options.backwards;
                    break;
                case 5:
                    editor_dialog_in_selection =
                        !editor_dialog_in_selection;
                    break;
                case 6:
                    editor_dialog_options.whole_words =
                        !editor_dialog_options.whole_words;
                    break;
                default:
                    break;
            }
            return true;
        }
        if (event != ftxui::Event::Return &&
            (editor_dialog_focus == 0U ||
             editor_dialog_focus == 1U)) {
            return edit_input(
                editor_dialog_focus == 0U ? editor_dialog_query
                                           : editor_dialog_replacement,
                editor_dialog_focus == 0U ? editor_query_input
                                           : editor_replacement_input,
                event
            );
        }
        if (replace && event == ftxui::Event::Return &&
            editor_dialog_focus == 0U) {
            editor_dialog_focus = 1U;
            return true;
        }
        if (event != ftxui::Event::Return) {
            return false;
        }
        if ((editor_dialog_focus == 8U && replace) ||
            editor_dialog_focus == 9U) {
            mode = Mode::editor;
            return true;
        }
        if (editor_dialog_query.empty()) {
            show_editor_message("Search", "Search string is empty");
            return true;
        }
        if (editor_dialog_in_selection && !editor_dialog_scope) {
            show_editor_message(
                "Search",
                "No marked block to search"
            );
            return true;
        }
        const auto scope =
            editor_dialog_in_selection
                ? editor_dialog_scope
                : std::nullopt;
        remember_input(editor_query_input, editor_dialog_query);
        if (replace) {
            remember_input(editor_replacement_input, editor_dialog_replacement);
        }
        try {
            if (!replace && editor_dialog_focus == 8U) {
                const auto count = editor.count_matches(
                    editor_dialog_query,
                    editor_dialog_options,
                    scope
                );
                commit_editor_search(
                    editor_dialog_query,
                    editor_dialog_options,
                    editor_dialog_in_selection,
                    scope
                );
                show_editor_message(
                    "Search",
                    std::to_string(count) + " match(es)"
                );
                return true;
            }
            if (!editor.find(
                    editor_dialog_query,
                    editor_dialog_options,
                    true,
                    false,
                    scope
                )) {
                commit_editor_search(
                    editor_dialog_query,
                    editor_dialog_options,
                    editor_dialog_in_selection,
                    scope
                );
                show_editor_message(
                    replace ? "Replace" : "Search",
                    "Search string not found"
                );
                return true;
            }
            center_editor();
            commit_editor_search(
                editor_dialog_query,
                editor_dialog_options,
                editor_dialog_in_selection,
                scope
            );
            if (replace) {
                editor_replace_needle = editor.search.needle;
                editor_replace_text = editor_dialog_replacement;
                editor_replace_choice = 0;
                editor_replacements = 0;
                mode = Mode::editor_replace_confirm;
            } else {
                status = "Match found";
                mode = Mode::editor;
            }
        } catch (const std::exception& error) {
            show_editor_error(error);
        }
        return true;
    }

    bool handle_editor_replace_confirm(const ftxui::Event& event) {
        const auto input = handle_button_input(
            event, editor_replace_choice, editor_replace_boxes, "rsaoc"
        );
        if (input == ButtonInput::cancel) {
            finish_editor_replace("Replace cancelled");
            return true;
        }
        if (input != ButtonInput::activate) {
            return input != ButtonInput::ignored;
        }

        if (editor_replace_choice == 4U) {
            finish_editor_replace("Replace cancelled");
            return true;
        }
        if (editor_replace_choice == 1U) {
            if (!editor.find(
                    editor_replace_needle,
                    editor.search.options,
                    false,
                    true,
                    editor.search.scope
                )) {
                finish_editor_replace(
                    std::to_string(editor_replacements) +
                    " replacement(s)"
                );
            }
            center_editor();
            return true;
        }
        if (editor_replace_choice == 2U) {
            try {
                editor_replacements += editor.replace_all(
                    editor_replace_needle,
                    editor_replace_text,
                    editor.search.options,
                    editor.search.scope
                );
                editor.search.change_id = editor.change_id();
            } catch (const std::exception& error) {
                show_editor_error(error);
                return true;
            }
            finish_editor_replace(
                std::to_string(editor_replacements) +
                " replacement(s)"
            );
            return true;
        }

        std::string replacement;
        try {
            replacement = editor.replacement_for_selection(
                editor_replace_needle,
                editor_replace_text,
                editor.search.options
            );
        } catch (const std::exception& error) {
            show_editor_error(error);
            return true;
        }
        const auto match = editor.selection();
        if (editor.replace_selection(
                replacement,
                &editor.search.scope
            )) {
            ++editor_replacements;
            editor.search.change_id = editor.change_id();
        }
        if (editor.search.options.backwards && match) {
            editor.set_position(match->first);
        }
        if (editor_replace_choice == 3U ||
            !editor.find(
                editor_replace_needle,
                editor.search.options,
                false,
                false,
                editor.search.scope
            )) {
            finish_editor_replace(
                std::to_string(editor_replacements) +
                " replacement(s)"
            );
        }
        center_editor();
        return true;
    }

    std::optional<EditorPosition> editor_mouse_position(
        const ftxui::Mouse& mouse,
        const bool clamp
    ) const {
        if (editor_line_boxes.empty()) {
            return std::nullopt;
        }
        std::size_t index = 0;
        while (index + 1U < editor_line_boxes.size() &&
               mouse.y > editor_line_boxes[index].y_max) {
            ++index;
        }
        const auto& box = editor_line_boxes[index];
        if (!clamp && !box.Contain(mouse.x, mouse.y)) {
            return std::nullopt;
        }
        const auto row = std::min(
            editor.lines().size() - 1U, editor_rendered_first_row + index
        );
        const auto cell = static_cast<std::size_t>(std::max(
            0, mouse.x - box.x_min - (editor_line_numbers ? 6 : 0)
        ));
        return EditorPosition{
            row,
            editor_offset_for_width(
                editor.lines()[row],
                editor_line_first_columns[index] + cell,
                editor.tab_width()
            ),
        };
    }

    bool handle_editor(const ftxui::Event& event) {
        if (event.is_mouse()) {
            auto mouse_event = event;
            const auto& mouse = mouse_event.mouse();
            if (mouse.button == ftxui::Mouse::WheelUp) {
                scroll_editor(-2);
                return true;
            }
            if (mouse.button == ftxui::Mouse::WheelDown) {
                scroll_editor(2);
                return true;
            }
            if (editor_mouse_dragging &&
                (mouse.motion == ftxui::Mouse::Moved ||
                 mouse.motion == ftxui::Mouse::Released)) {
                if (mouse.motion == ftxui::Mouse::Released) {
                    editor.finish_mark();
                    editor_mouse_dragging = false;
                    return true;
                }
                if (const auto position = editor_mouse_position(mouse, true)) {
                    if (!editor_mouse_marking &&
                        (position->row != editor.row() ||
                         position->column != editor.column())) {
                        if (editor_mouse_column) {
                            editor.begin_column_mark();
                        } else {
                            editor.begin_mark();
                        }
                        editor_mouse_marking = true;
                    }
                    editor.set_position(*position);
                }
                return true;
            }
            if (mouse.button == ftxui::Mouse::Left &&
                mouse.motion == ftxui::Mouse::Pressed) {
                if (mouse.y == 0) {
                    editor_menu_category = 0;
                    reset_editor_menu_item();
                    editor_menu_dropped = true;
                    mode = Mode::editor_menu;
                    return true;
                }
                if (const auto position = editor_mouse_position(mouse, false)) {
                    editor_shift_marking = false;
                    editor_column_marking = false;
                    editor.finish_mark();
                    if (mouse.shift) {
                        editor.begin_mark();
                    } else {
                        editor.clear_selection();
                    }
                    editor.set_position(*position);
                    editor_mouse_dragging = true;
                    editor_mouse_marking = mouse.shift;
                    editor_mouse_column = mouse.meta;
                    return true;
                }
            }
            return false;
        }
        if (editor_mouse_dragging) {
            editor.finish_mark();
            editor_mouse_dragging = false;
        }
        const auto column_move = [&](const ftxui::Event& key,
                                     const int rows,
                                     const int columns) {
            if (event != key) {
                return false;
            }
            if (editor_shift_marking) {
                editor.finish_mark();
                editor_shift_marking = false;
            }
            if (!editor_column_marking) {
                editor.begin_column_mark();
                editor_column_marking = true;
            }
            editor.move(rows, columns);
            return true;
        };
        if (column_move(
                ftxui::Event::Special("\x1B[1;3A"),
                -1,
                0
            ) ||
            column_move(
                ftxui::Event::Special("\x1B[1;3B"),
                1,
                0
            ) ||
            column_move(
                ftxui::Event::Special("\x1B[1;3D"),
                0,
                -1
            ) ||
            column_move(
                ftxui::Event::Special("\x1B[1;3C"),
                0,
                1
            )) {
            return true;
        }
        if (event == ftxui::Event::Special("\x1B[5;3~") ||
            event == ftxui::Event::Special("\x1B[6;3~")) {
            if (editor_shift_marking) {
                editor.finish_mark();
                editor_shift_marking = false;
            }
            if (!editor_column_marking) {
                editor.begin_column_mark();
                editor_column_marking = true;
            }
            scroll_editor(
                (event == ftxui::Event::Special("\x1B[5;3~") ? -1 : 1) *
                static_cast<int>(editor_page_rows())
            );
            return true;
        }
        if (editor_column_marking) {
            editor.finish_mark();
            editor_column_marking = false;
        }
        const bool clipboard_event =
            event == ftxui::Event::Special("\x1B[2;5~") ||
            event == ftxui::Event::Special("\x1B[3;2~") ||
            event == ftxui::Event::Special("\x1B[2;2~");
        if (clipboard_event && editor_shift_marking) {
            editor.finish_mark();
            editor_shift_marking = false;
        }
        if (event == ftxui::Event::Special("\x1B[2;5~")) {
            store_editor_clipboard(false);
            return true;
        }
        if (event == ftxui::Event::Special("\x1B[3;2~")) {
            store_editor_clipboard(true);
            return true;
        }
        if (event == ftxui::Event::Special("\x1B[2;2~")) {
            paste_editor_clipboard();
            return true;
        }
        const auto shifted_move = [&](const ftxui::Event& shifted,
                                      const int rows,
                                      const int columns) {
            if (event != shifted) {
                return false;
            }
            if (!editor_shift_marking) {
                editor.begin_mark();
                editor_shift_marking = true;
            }
            editor.move(rows, columns);
            return true;
        };
        if (shifted_move(
                ftxui::Event::Special("\x1B[1;2A"),
                -1,
                0
            ) ||
            shifted_move(
                ftxui::Event::Special("\x1B[1;2B"),
                1,
                0
            ) ||
            shifted_move(
                ftxui::Event::Special("\x1B[1;2D"),
                0,
                -1
            ) ||
            shifted_move(
                ftxui::Event::Special("\x1B[1;2C"),
                0,
                1
            )) {
            return true;
        }
        const auto control_shift = std::array<ftxui::Event, 8>{
            ftxui::Event::Special("\x1B[1;6D"),
            ftxui::Event::Special("\x1B[1;6C"),
            ftxui::Event::Special("\x1B[1;6H"),
            ftxui::Event::Special("\x1B[1;6F"),
            ftxui::Event::Special("\x1B[5;6~"),
            ftxui::Event::Special("\x1B[6;6~"),
            ftxui::Event::Special("\x1B[1;6A"),
            ftxui::Event::Special("\x1B[1;6B"),
        };
        const auto selected_motion = std::find(
            control_shift.begin(), control_shift.end(), event
        );
        if (selected_motion != control_shift.end()) {
            if (!editor_shift_marking) {
                editor.begin_mark();
                editor_shift_marking = true;
            }
            switch (std::distance(control_shift.begin(), selected_motion)) {
                case 0: editor.move_word(-1); break;
                case 1: editor.move_word(1); break;
                case 2: editor.goto_line(1U); editor.home(); break;
                case 3: editor.goto_line(editor.lines().size()); editor.end(); break;
                case 4: editor.goto_line(editor.first_row + 1U); break;
                case 5: editor.goto_line(editor.first_row + editor_visible_rows()); break;
                case 6: scroll_editor(-1); break;
                case 7: scroll_editor(1); break;
                default: break;
            }
            return true;
        }
        if (event == ftxui::Event::Special("\x1B[1;2H") ||
            event == ftxui::Event::Special("\x1B[1;2F") ||
            event == ftxui::Event::Special("\x1B[5;2~") ||
            event == ftxui::Event::Special("\x1B[6;2~")) {
            if (!editor_shift_marking) {
                editor.begin_mark();
                editor_shift_marking = true;
            }
            if (event == ftxui::Event::Special("\x1B[1;2H")) {
                editor.home();
            } else if (event ==
                       ftxui::Event::Special("\x1B[1;2F")) {
                editor.end();
            } else {
                scroll_editor(
                    event == ftxui::Event::Special("\x1B[5;2~")
                        ? -static_cast<int>(editor_page_rows())
                        : static_cast<int>(editor_page_rows())
                );
            }
            return true;
        }
        if (editor_shift_marking) {
            editor.finish_mark();
            editor_shift_marking = false;
        }
        if (event == ftxui::Event::Special("\x1B[25~") ||
            event == ftxui::Event::Special("\x1B[1;2P")) {
            editor.toggle_column_mark();
            status = editor.marking()
                         ? "Marking columns"
                         : "Column block marked";
            return true;
        }
        if (event == ftxui::Event::F1) {
            mode = Mode::editor_help;
            return true;
        }
        if (event == ftxui::Event::F2) {
            save_editor();
            return true;
        }
        if (event == ftxui::Event::F3) {
            editor.toggle_mark();
            status = editor.marking()
                         ? "Marking block"
                         : "Block marked";
            return true;
        }
        if (event == ftxui::Event::F4) {
            start_editor_search_dialog(true);
            return true;
        }
        if (event == ftxui::Event::F5) {
            status = editor.copy_selection()
                         ? "Block copied"
                         : "No marked block";
            return true;
        }
        if (event == ftxui::Event::F6) {
            status = editor.move_selection()
                         ? "Block moved"
                         : "No movable block";
            return true;
        }
        if (event == ftxui::Event::F7) {
            start_editor_search_dialog(false);
            return true;
        }
        if (event == ftxui::Event::Special("\x1B[31~") ||
            event == ftxui::Event::Special("\x1B[18;2~")) {
            repeat_editor_search();
            return true;
        }
        if (event == ftxui::Event::F8) {
            editor.delete_block_or_line();
            return true;
        }
        if (event == ftxui::Event::F9) {
            editor_menu_category = 0;
            editor_menu_item = 0;
            editor_menu_dropped = false;
            mode = Mode::editor_menu;
            return true;
        }
        if (event == ftxui::Event::F10 || event == ftxui::Event::Escape) {
            request_editor_quit();
            return true;
        }
        if (event == ftxui::Event::F12) {
            start_prompt(
                PromptAction::editor_save_as,
                "Save as",
                editor.path()
            );
            return true;
        }
        if (event == ftxui::Event::AltL) {
            start_prompt(
                PromptAction::editor_goto,
                "Go to line",
                std::to_string(editor.row() + 1U)
            );
            return true;
        }
        if (event == ftxui::Event::Special("\x1B" "E")) {
            editor_list_cursor = 0;
            mode = Mode::editor_history;
            return true;
        }
        if (event == ftxui::Event::CtrlN) {
            start_prompt(
                PromptAction::editor_new,
                "New file",
                (
                    std::filesystem::path(editor.path())
                        .parent_path() /
                    "new-file"
                ).string()
            );
            return true;
        }
        if (event == ftxui::Event::CtrlU) {
            status = editor.undo() ? "Undo" : "Nothing to undo";
            return true;
        }
        if (event == ftxui::Event::AltR) {
            status = editor.redo() ? "Redo" : "Nothing to redo";
            return true;
        }
        if (event == ftxui::Event::Insert) {
            editor.toggle_overwrite();
            return true;
        }
        if (event == ftxui::Event::CtrlZ ||
            event == ftxui::Event::ArrowLeftCtrl) {
            editor.move_word(-1);
            return true;
        }
        if (event == ftxui::Event::CtrlX ||
            event == ftxui::Event::ArrowRightCtrl) {
            editor.move_word(1);
            return true;
        }
        if (event == ftxui::Event::CtrlY) {
            editor.delete_line();
            return true;
        }
        if (event == ftxui::Event::CtrlK) {
            editor.delete_to_end();
            return true;
        }
        if (event == ftxui::Event::CtrlD) {
            editor.erase();
            return true;
        }
        if (event == ftxui::Event::CtrlS) {
            editor_syntax = !editor_syntax;
            status = editor_syntax
                         ? "Syntax highlighting enabled"
                         : "Syntax highlighting disabled";
            return true;
        }
        if (event == ftxui::Event::AltN) {
            editor_line_numbers = !editor_line_numbers;
            status = editor_line_numbers
                         ? "Line numbers enabled"
                         : "Line numbers disabled";
            return true;
        }
        if (event == ftxui::Event::AltB) {
            status = editor.match_bracket()
                         ? "Matching bracket"
                         : "No matching bracket";
            return true;
        }
        if (event == ftxui::Event::CtrlF) {
            show_editor_message(
                "Save block",
                "Saving a block to a file is not implemented"
            );
            return true;
        }
        if (event == ftxui::Event::Special("\x1B[1;5H")) {
            editor.goto_line(1);
            return true;
        }
        if (event == ftxui::Event::Special("\x1B[1;5F")) {
            editor.goto_line(editor.lines().size());
            return true;
        }
        if (event == ftxui::Event::Special("\x1B[5;5~")) {
            editor.goto_line(editor.first_row + 1U);
            return true;
        }
        if (event == ftxui::Event::Special("\x1B[6;5~")) {
            editor.goto_line(editor.first_row + editor_visible_rows());
            return true;
        }
        if (event == ftxui::Event::Special("\x1B[1;5A")) {
            scroll_editor(-1);
            return true;
        }
        if (event == ftxui::Event::Special("\x1B[1;5B")) {
            scroll_editor(1);
            return true;
        }
        if (event == ftxui::Event::ArrowUp) {
            editor.move(-1, 0);
        } else if (event == ftxui::Event::ArrowDown) {
            editor.move(1, 0);
        } else if (event == ftxui::Event::ArrowLeft) {
            editor.move(0, -1);
        } else if (event == ftxui::Event::ArrowRight) {
            editor.move(0, 1);
        } else if (event == ftxui::Event::Home) {
            editor.home();
        } else if (event == ftxui::Event::End) {
            editor.end();
        } else if (event == ftxui::Event::PageUp) {
            scroll_editor(
                -static_cast<int>(editor_page_rows())
            );
        } else if (event == ftxui::Event::PageDown) {
            scroll_editor(
                static_cast<int>(editor_page_rows())
            );
        } else if (event == ftxui::Event::Tab ||
                   event == ftxui::Event::TabReverse ||
                   event == ftxui::Event::Special("\x1B[9;5u") ||
                   event == ftxui::Event::Special("\x1B[9;6u") ||
                   event == ftxui::Event::Special("\x1B[27;5;9~") ||
                   event == ftxui::Event::Special("\x1B[27;6;9~")) {
            editor.tab(editor_indent_width);
        } else if (event == ftxui::Event::Special("\x1B[13;2u") ||
                   event == ftxui::Event::Special("\x1B[13;5u") ||
                   event == ftxui::Event::Special("\x1B[13;6u") ||
                   event == ftxui::Event::Special("\x1B[27;2;13~") ||
                   event == ftxui::Event::Special("\x1B[27;5;13~") ||
                   event == ftxui::Event::Special("\x1B[27;6;13~")) {
            editor.newline(false);
        } else if (event == ftxui::Event::Return) {
            editor.newline(editor_auto_indent);
        } else if (event == ftxui::Event::Backspace) {
            editor.backspace(editor_indent_width);
        } else if (event == ftxui::Event::Delete) {
            editor.erase();
        } else if (event.is_character()) {
            editor.insert(event.character());
        } else {
            return false;
        }
        return true;
    }

    bool handle_menu(const ftxui::Event& event) {
        if (modal_cancel(event) || event == ftxui::Event::F2) {
            mode = Mode::panels;
            return true;
        }
        if (!event.is_character()) {
            return false;
        }
        const auto key = event.character();
        if (key.size() != 1U) {
            return false;
        }
        const auto match = std::find_if(
            user_menu.begin(),
            user_menu.end(),
            [&](const UserMenuEntry& item) {
                return item.key == key.front();
            }
        );
        if (match == user_menu.end()) {
            return false;
        }
        const auto* selected = active().current();
        mode = Mode::panels;
        try {
            const auto command = expand_user_menu_command(
                match->command,
                selected == nullptr ? "" : selected->path,
                active().path(),
                passive().path()
            );
            const auto result = run_process(command, false);
            status =
                match->label + " exited " +
                std::to_string(result);
        } catch (const std::exception& error) {
            set_error(error);
        }
        return true;
    }

    void reset_pulldown_item() {
        const auto& items = pulldown_items()[pulldown_category];
        if (pulldown_category == 0U ||
            pulldown_category == 4U) {
            const auto panel_index =
                pulldown_category == 0U ? 0 : 1;
            const auto action =
                tree_panel == panel_index
                    ? 14
                    : information_panel == panel_index
                          ? 11
                          : quick_view_panel == panel_index
                                ? 10
                                : 12;
            const auto current = std::ranges::find(
                items.begin(),
                items.end(),
                action,
                &MenuRow::action
            );
            if (current != items.end()) {
                pulldown_item = static_cast<std::size_t>(
                    std::distance(items.begin(), current)
                );
                return;
            }
        }
        const auto match = std::find_if(
            items.begin(),
            items.end(),
            [](const MenuRow& item) {
                return item.action >= 0;
            }
        );
        pulldown_item =
            match == items.end()
                ? 0U
                : static_cast<std::size_t>(
                      std::distance(items.begin(), match)
                  );
    }

    void move_pulldown_item(const int direction) {
        const auto& items = pulldown_items()[pulldown_category];
        if (items.empty()) {
            pulldown_item = 0;
            return;
        }
        for (std::size_t count = 0; count < items.size(); ++count) {
            pulldown_item =
                direction > 0
                    ? (pulldown_item + 1U) % items.size()
                    : (pulldown_item + items.size() - 1U) %
                          items.size();
            if (items[pulldown_item].action >= 0) {
                return;
            }
        }
    }

    bool handle_pulldown(const ftxui::Event& event) {
        if (modal_cancel(event) || event == ftxui::Event::F9) {
            mode = Mode::panels;
            return true;
        }
        if (event.is_mouse()) {
            auto mouse_event = event;
            const auto& mouse = mouse_event.mouse();
            if (pulldown_dropped && pulldown_viewport.Contain(mouse.x, mouse.y) &&
                (mouse.button == ftxui::Mouse::WheelUp ||
                 mouse.button == ftxui::Mouse::WheelDown)) {
                for (int step = 0; step < 3; ++step) {
                    move_pulldown_item(mouse.button == ftxui::Mouse::WheelUp ? -1 : 1);
                }
                return true;
            }
            if (mouse.button == ftxui::Mouse::Left &&
                mouse.motion == ftxui::Mouse::Pressed) {
                for (std::size_t index = 0;
                     index < top_menu_boxes.size();
                     ++index) {
                    if (top_menu_boxes[index].Contain(
                            mouse.x,
                            mouse.y
                        )) {
                        pulldown_category = index;
                        reset_pulldown_item();
                        pulldown_dropped = true;
                        return true;
                    }
                }
                for (std::size_t index = 0;
                     index < pulldown_item_boxes.size();
                     ++index) {
                    if (!pulldown_dropped ||
                        !pulldown_viewport.Contain(mouse.x, mouse.y) ||
                        !pulldown_item_boxes[index].Contain(
                            mouse.x,
                            mouse.y
                        )) {
                        continue;
                    }
                    pulldown_item = index;
                    try {
                        execute_pulldown();
                    } catch (const std::exception& error) {
                        set_error(error);
                        mode = Mode::panels;
                        clear_tree_operation_context();
                        clear_tree_operation_context();
                    }
                    return true;
                }
            }
        }
        if (event == ftxui::Event::ArrowLeft) {
            pulldown_category = (pulldown_category + 4U) % 5U;
            reset_pulldown_item();
            return true;
        }
        if (event == ftxui::Event::ArrowRight) {
            pulldown_category = (pulldown_category + 1U) % 5U;
            reset_pulldown_item();
            return true;
        }
        if (event == ftxui::Event::ArrowDown) {
            if (!pulldown_dropped) {
                pulldown_dropped = true;
                reset_pulldown_item();
                return true;
            }
            move_pulldown_item(1);
            return true;
        }
        if (event == ftxui::Event::ArrowUp) {
            if (!pulldown_dropped) {
                pulldown_dropped = true;
                reset_pulldown_item();
            }
            move_pulldown_item(-1);
            return true;
        }
        if (event == ftxui::Event::Return) {
            if (!pulldown_dropped) {
                pulldown_dropped = true;
                reset_pulldown_item();
                return true;
            }
            try {
                execute_pulldown();
            } catch (const std::exception& error) {
                set_error(error);
                mode = Mode::panels;
                clear_tree_operation_context();
            }
            return true;
        }
        return false;
    }

    bool handle_bookmarks(const ftxui::Event& event) {
        const auto input = handle_list_input(
            event, bookmark_cursor, config.bookmarks.size(),
            bookmark_boxes, bookmark_list_box
        );
        if (input == ButtonInput::cancel) {
            mode = Mode::panels;
            return true;
        }
        if (input == ButtonInput::changed) {
            return true;
        }
        if (event == ftxui::Event::Delete &&
            bookmark_cursor < config.bookmarks.size()) {
            config.bookmarks.erase(
                config.bookmarks.begin() +
                    static_cast<std::ptrdiff_t>(bookmark_cursor)
            );
            if (bookmark_cursor >= config.bookmarks.size() &&
                bookmark_cursor > 0U) {
                --bookmark_cursor;
            }
            return true;
        }
        if (input == ButtonInput::activate &&
            bookmark_cursor < config.bookmarks.size()) {
            try {
                active().change_directory(
                    config.bookmarks[bookmark_cursor]
                );
                mode = Mode::panels;
            } catch (const std::exception& error) {
                set_error(error);
            }
            return true;
        }
        return false;
    }

    bool handle_search_results(const ftxui::Event& event) {
        const auto input = handle_list_input(
            event, search_cursor, search_results.size(),
            search_result_boxes, search_list_box
        );
        if (input == ButtonInput::cancel) {
            mode = Mode::panels;
            return true;
        }
        if (input == ButtonInput::changed) {
            return true;
        }
        if ((event == ftxui::Event::F3 || event == ftxui::Event::F4 ||
             input == ButtonInput::activate) &&
            search_cursor < search_results.size()) {
            const auto& result = search_results[search_cursor];
            const auto provider = search_provider ? search_provider : active().provider();
            try {
                if (event == ftxui::Event::F3 || event == ftxui::Event::F4) {
                    if (!can_open_file(result)) {
                        status = "Select a regular file to view or edit";
                        return true;
                    }
                    if (event == ftxui::Event::F3) {
                        viewer.open(provider, result.path);
                        viewer_search_needle.clear();
                        viewer_return_mode = Mode::search_results;
                        mode = Mode::viewer;
                    } else {
                        const auto retained =
                            editor_buffers.size() + (editor.open() ? 1U : 0U);
                        open_editor_buffer(provider, result.path, false);
                        editor_session_base = retained;
                        editor_return_mode = Mode::search_results;
                        mode = Mode::editor;
                    }
                } else {
                    active().change_directory(provider->parent(result.path));
                    active().find(result.name, true);
                    mode = Mode::panels;
                }
            } catch (const std::exception& error) {
                set_error(error);
            }
            return true;
        }
        return false;
    }

    bool handle_operation_dialog(const ftxui::Event& event) {
        const auto scroll_body = [&](const int amount) {
            dialog_focus = static_cast<std::size_t>(std::clamp(
                static_cast<int>(std::min<std::size_t>(dialog_focus, 4U)) + amount,
                0, 4
            ));
        };
        if (event == ftxui::Event::PageUp || event == ftxui::Event::PageDown) {
            scroll_body(event == ftxui::Event::PageUp ? -3 : 3);
            return true;
        }
        if (event.is_mouse()) {
            auto mouse_event = event;
            const auto& mouse = mouse_event.mouse();
            if (operation_dialog_viewport.Contain(mouse.x, mouse.y) &&
                (mouse.button == ftxui::Mouse::WheelUp ||
                 mouse.button == ftxui::Mouse::WheelDown)) {
                destination_input.dragging = false;
                mask_input.dragging = false;
                scroll_body(mouse.button == ftxui::Mouse::WheelUp ? -2 : 2);
                return true;
            }
            if (destination_input.dragging) {
                return input_mouse(dialog_destination, destination_input, mouse);
            }
            if (mask_input.dragging) {
                return input_mouse(dialog_source_mask, mask_input, mouse);
            }
            if (mouse.button == ftxui::Mouse::Left &&
                mouse.motion == ftxui::Mouse::Pressed) {
                for (std::size_t index = 0; index < operation_dialog_boxes.size(); ++index) {
                    if ((index < 5U && !operation_dialog_viewport.Contain(mouse.x, mouse.y)) ||
                        !operation_dialog_boxes[index].Contain(mouse.x, mouse.y)) {
                        continue;
                    }
                    dialog_focus = index;
                    if (index == 0U) {
                        input_mouse(dialog_destination, destination_input, mouse);
                    } else if (index == 1U) {
                        input_mouse(dialog_source_mask, mask_input, mouse);
                    } else if (index < 5U) {
                        return handle_operation_dialog(ftxui::Event::Character(' '));
                    } else {
                        return handle_operation_dialog(ftxui::Event::Return);
                    }
                    break;
                }
            }
            return true;
        }
        if (modal_cancel(event) ||
            ((event == ftxui::Event::Return ||
              event == ftxui::Event::Character(' ')) && dialog_focus == 6U)) {
            mode = Mode::panels;
            status = "Operation cancelled";
            clear_tree_operation_context();
            return true;
        }
        if (event == ftxui::Event::Tab) {
            dialog_focus = (dialog_focus + 1U) % 7U;
            return true;
        }
        if (event == ftxui::Event::TabReverse) {
            dialog_focus = (dialog_focus + 6U) % 7U;
            return true;
        }
        if (event == ftxui::Event::ArrowDown) {
            dialog_focus = std::min<std::size_t>(
                6U,
                dialog_focus + 1U
            );
            return true;
        }
        if (event == ftxui::Event::ArrowUp) {
            dialog_focus =
                dialog_focus == 0U ? 0U : dialog_focus - 1U;
            return true;
        }
        if (event == ftxui::Event::Character(' ') &&
            (
                dialog_focus == 2U ||
                dialog_focus == 3U ||
                dialog_focus == 4U
            )) {
            if (dialog_focus == 2U) {
                dialog_preserve_attributes =
                    !dialog_preserve_attributes;
            } else if (dialog_focus == 3U) {
                dialog_preserve_symlinks =
                    !dialog_preserve_symlinks;
            } else {
                dialog_background = !dialog_background;
            }
            return true;
        }
        if (event == ftxui::Event::Return ||
            (event == ftxui::Event::Character(' ') && dialog_focus == 5U)) {
            try {
                remember_input(destination_input, dialog_destination);
                remember_input(mask_input, dialog_source_mask);
                confirm_operation_dialog();
            } catch (const std::exception& error) {
                set_error(error);
                mode = Mode::panels;
                clear_tree_operation_context();
            }
            return true;
        }
        if (dialog_focus == 0U || dialog_focus == 1U) {
            if (event == ftxui::Event::Special("\x1B\t")) {
                try {
                    if (dialog_focus == 0U) {
                        complete_path(dialog_destination, destination_input, active().path());
                        status = destination_input.message.empty()
                                     ? "Path completed"
                                     : destination_input.message;
                    } else {
                        status = "Path completion is only available for the destination";
                    }
                } catch (const std::exception& error) {
                    status = "Completion: " + std::string(error.what());
                }
                return true;
            }
            return edit_input(
                dialog_focus == 0U ? dialog_destination : dialog_source_mask,
                dialog_focus == 0U ? destination_input : mask_input,
                event
            );
        }
        return false;
    }

    bool handle_delete_dialog(const ftxui::Event& event) {
        const auto input = handle_button_input(
            event, dialog_focus, delete_button_boxes, "yn"
        );
        if (input == ButtonInput::cancel) {
            mode = Mode::panels;
            status = "Delete cancelled";
            clear_tree_operation_context();
            return true;
        }
        if (input == ButtonInput::activate) {
            if (dialog_focus == 0U) {
                try {
                    run_remove_operation();
                } catch (const std::exception& error) {
                    set_error(error);
                    mode = Mode::panels;
                    clear_tree_operation_context();
                }
            } else {
                mode = Mode::panels;
                status = "Delete cancelled";
                clear_tree_operation_context();
            }
            return true;
        }
        return input != ButtonInput::ignored;
    }

    bool handle_operation_progress(const ftxui::Event& event) {
        if (modal_cancel(event) ||
            event == ftxui::Event::CtrlC ||
            event == ftxui::Event::Character('a')) {
            request_operation_cancel();
            status = "Cancelling operation";
            return true;
        }
        if (event == ftxui::Event::Character(' ') ||
            event == ftxui::Event::Character('p')) {
            bool paused = false;
            {
                std::lock_guard lock(operation_pause_mutex);
                paused = !operation_paused.load(
                    std::memory_order_acquire
                );
                operation_paused.store(
                    paused,
                    std::memory_order_release
                );
            }
            if (!paused) {
                operation_pause_condition.notify_all();
            }
            status = paused ? "Operation paused" : "Operation resumed";
            return true;
        }
        if (event == ftxui::Event::Character('s') &&
            operation_label != "Delete") {
            {
                std::lock_guard lock(operation_pause_mutex);
                operation_skip.store(
                    true,
                    std::memory_order_release
                );
                operation_paused.store(
                    false,
                    std::memory_order_release
                );
            }
            operation_pause_condition.notify_all();
            status = "Skipping current entry";
            return true;
        }
        return false;
    }

    bool handle_conflict_dialog(const ftxui::Event& event) {
        const bool error = mode == Mode::operation_error_dialog;
        const std::vector<OperationChoice> choices =
            error ? std::vector<OperationChoice>{
                        OperationChoice::retry, OperationChoice::skip,
                        OperationChoice::ignore_all, OperationChoice::cancel
                    }
                  : std::vector<OperationChoice>{
                        OperationChoice::overwrite, OperationChoice::skip,
                        OperationChoice::overwrite_all, OperationChoice::skip_all,
                        OperationChoice::older, OperationChoice::cancel
                    };
        const auto input = handle_button_input(
            event, dialog_focus,
            std::span<const ftxui::Box>(operation_choice_boxes).first(choices.size()),
            error ? "rsia" : "ynasoc"
        );
        if (input == ButtonInput::cancel) {
            answer_operation(OperationChoice::cancel);
            return true;
        }
        if (input == ButtonInput::activate) {
            answer_operation(choices[dialog_focus]);
            return true;
        }
        return input != ButtonInput::ignored;
    }

    bool handle_jobs(const ftxui::Event& event) {
        if (modal_cancel(event)) {
            mode = Mode::panels;
            return true;
        }
        if (event.is_mouse()) {
            auto mouse_event = event;
            const auto& mouse = mouse_event.mouse();
            if (mouse.button == ftxui::Mouse::Left &&
                mouse.motion == ftxui::Mouse::Pressed) {
                for (std::size_t index = 0; index < job_row_boxes.size(); ++index) {
                    if (job_row_boxes[index].Contain(mouse.x, mouse.y)) {
                        job_cursor = index;
                        return true;
                    }
                }
                const std::array<ftxui::Event, 4> actions{
                    ftxui::Event::F2, ftxui::Event::F3,
                    ftxui::Event::F8, ftxui::Event::Escape
                };
                for (std::size_t index = 0; index < job_button_boxes.size(); ++index) {
                    if (job_button_boxes[index].Contain(mouse.x, mouse.y)) {
                        return handle_jobs(actions[index]);
                    }
                }
            }
            return true;
        }
        if (event == ftxui::Event::ArrowUp && job_cursor > 0U) {
            --job_cursor;
        } else if (event == ftxui::Event::ArrowDown && job_cursor + 1U < jobs.size()) {
            ++job_cursor;
        } else if (event == ftxui::Event::F2 || event == ftxui::Event::F3 ||
                   event == ftxui::Event::F8 || event == ftxui::Event::Return) {
            if (active_job != job_cursor ||
                !operation_running.load(std::memory_order_acquire)) {
                status = "No running job selected";
                return true;
            }
            if (event == ftxui::Event::F2) {
                operation_paused.store(
                    !operation_paused.load(std::memory_order_acquire),
                    std::memory_order_release
                );
                operation_pause_condition.notify_all();
            } else if (event == ftxui::Event::F8) {
                request_operation_cancel();
            } else {
                mode = Mode::operation_progress;
            }
        } else {
            return false;
        }
        return true;
    }

    bool handle_panels(ftxui::Event event) {
        if (!event.is_mouse()) {
            last_click_pane = -1;
        }
        if (event == ftxui::Event::Escape && pending_operation_notice) {
            pending_operation_notice.reset();
            status = "Ready";
        }
        if (event.is_mouse()) {
            const auto& mouse = event.mouse();
            if (command_input.dragging ||
                (mouse.button == ftxui::Mouse::Left &&
                 mouse.motion == ftxui::Mouse::Pressed &&
                 command_input.box.Contain(mouse.x, mouse.y))) {
                command_mode = true;
                quick_view_focused = false;
                information_panel_focused = false;
                tree_panel_focused = false;
                return input_mouse(command_line, command_input, mouse);
            }
        }
        if (event.is_mouse() &&
            (
                event.mouse().motion == ftxui::Mouse::Moved ||
                event.mouse().motion == ftxui::Mouse::Released
            )) {
            return false;
        }
        if (traversal_running.load(std::memory_order_acquire)) {
            if (event == ftxui::Event::CtrlC) {
                traversal_cancellation.request();
                status = "Cancelling search";
                return true;
            }
            if (event.is_mouse() ||
                event == ftxui::Event::CtrlX ||
                event == ftxui::Event::CtrlU ||
                event == ftxui::Event::CtrlO ||
                event == ftxui::Event::AltC ||
                event == ftxui::Event::Return ||
                event == ftxui::Event::Insert ||
                event == ftxui::Event::F1 ||
                event == ftxui::Event::F2 ||
                event == ftxui::Event::F3 ||
                event == ftxui::Event::F4 ||
                event == ftxui::Event::F5 ||
                event == ftxui::Event::F6 ||
                event == ftxui::Event::F7 ||
                event == ftxui::Event::F8 ||
                event == ftxui::Event::F9) {
                status = "Search running; Ctrl-C cancels";
                return true;
            }
        }
        const auto find_tree = [&](const bool include_current) {
            if (tree_search.empty() || tree_nodes.empty()) {
                return false;
            }
            const auto sought = lowercase_text(tree_search);
            const auto first = include_current ? 0U : 1U;
            for (std::size_t offset = first;
                 offset < tree_nodes.size() + first;
                 ++offset) {
                const auto index =
                    (tree_cursor + offset) % tree_nodes.size();
                const auto name = lowercase_text(
                    std::filesystem::path(tree_nodes[index].path)
                        .filename()
                        .string()
                );
                if (name.starts_with(sought)) {
                    tree_cursor = index;
                    update_tree_selection();
                    return true;
                }
            }
            return false;
        };
        if (tree_search_mode) {
            bool continue_event = false;
            if (event == ftxui::Event::Escape ||
                event == ftxui::Event::Return) {
                tree_search_mode = false;
                status = "Ready";
                return true;
            }
            if (event == ftxui::Event::Backspace) {
                erase_last_utf8(tree_search);
                static_cast<void>(find_tree(true));
            } else if (event == ftxui::Event::CtrlS ||
                       event == ftxui::Event::AltS) {
                static_cast<void>(find_tree(false));
            } else if (event.is_character()) {
                tree_search += event.character();
                static_cast<void>(find_tree(true));
            } else {
                tree_search_mode = false;
                status = "Ready";
                tree_search.clear();
                continue_event = true;
            }
            if (!continue_event) {
                status = "Tree search: " + tree_search;
                return true;
            }
        }
        bool panel_search_confirmed = false;
        if (panel_search_mode) {
            if (event == ftxui::Event::Escape) {
                panel_search_mode = false;
                status = "Ready";
                return true;
            } else if (event == ftxui::Event::Return) {
                panel_search_mode = false;
                panel_search_confirmed = true;
                status = "Ready";
            } else if (event == ftxui::Event::Backspace) {
                erase_last_utf8(panel_search);
                if (!panel_search.empty()) {
                    static_cast<void>(
                        active().find(
                            panel_search,
                            false,
                            true,
                            true
                        )
                    );
                }
                status = "Quick search: " + panel_search;
                return true;
            } else if (event == ftxui::Event::CtrlS ||
                       event == ftxui::Event::AltS) {
                if (!panel_search.empty()) {
                    static_cast<void>(
                        active().find(
                            panel_search,
                            false,
                            false,
                            true
                        )
                    );
                }
                return true;
            } else if (event.is_character()) {
                panel_search += event.character();
                const auto found =
                    active().find(
                        panel_search,
                        false,
                        true,
                        true
                    );
                status = found
                             ? "Quick search: " + panel_search
                             : "Quick search: " + panel_search +
                                   " (not found)";
                return true;
            } else {
                panel_search_mode = false;
            }
        }
        if (panel_ctrl_x) {
            panel_ctrl_x = false;
            status = "Ready";
            try {
                if (event == ftxui::Event::q) {
                    toggle_quick_view();
                } else if (event == ftxui::Event::i) {
                    toggle_information_panel();
                } else if (event == ftxui::Event::c) {
                    execute_pulldown_action(1U, 8);
                } else if (event == ftxui::Event::l) {
                    execute_pulldown_action(1U, 11);
                } else if (event == ftxui::Event::s) {
                    execute_pulldown_action(1U, 12);
                } else if (event == ftxui::Event::v) {
                    execute_pulldown_action(1U, 13);
                } else if (event == ftxui::Event::o) {
                    execute_pulldown_action(1U, 9);
                } else if (event == ftxui::Event::d) {
                    execute_pulldown_action(2U, 1);
                } else if (event == ftxui::Event::j) {
                    open_jobs();
                } else if (
                    event == ftxui::Event::Character('!')
                ) {
                    status = "External panelize is not implemented";
                } else if (event != ftxui::Event::Escape) {
                    status = "Unknown C-x command";
                } else {
                    status = "Ready";
                }
            } catch (const std::exception& error) {
                set_error(error);
            }
            return true;
        }
        if (event == ftxui::Event::CtrlX) {
            panel_ctrl_x = true;
            status = "C-x";
            return true;
        }
        const bool selection_shortcut =
            command_line.empty() &&
            (
                event == ftxui::Event::Character('+') ||
                event == ftxui::Event::Character('-') ||
                event == ftxui::Event::Character('*')
            );
        const bool panel_shortcut =
            event.is_mouse() ||
            selection_shortcut ||
            event == ftxui::Event::Tab ||
            event == ftxui::Event::ArrowUp ||
            event == ftxui::Event::ArrowDown ||
            event == ftxui::Event::ArrowLeft ||
            event == ftxui::Event::ArrowRight ||
            event == ftxui::Event::PageUp ||
            event == ftxui::Event::PageDown ||
            event == ftxui::Event::Home ||
            event == ftxui::Event::End ||
            event == ftxui::Event::Insert ||
            event == ftxui::Event::CtrlA ||
            event ==
                ftxui::Event::Special("\x1B[1;2A") ||
            event ==
                ftxui::Event::Special("\x1B[1;2B") ||
            event == ftxui::Event::Special("\x1B[1;2A") ||
            event == ftxui::Event::Special("\x1B[1;2B") ||
            event == ftxui::Event::Special("\x1B[5;5~") ||
            event == ftxui::Event::Special("\x1B[6;5~") ||
            event == ftxui::Event::CtrlS ||
            event == ftxui::Event::AltS ||
            event == ftxui::Event::CtrlC ||
            event == ftxui::Event::CtrlO ||
            event == ftxui::Event::CtrlU ||
            event == ftxui::Event::Special("\x1C") ||
            event == ftxui::Event::Special("\x1B?") ||
            event == ftxui::Event::AltC ||
            event == ftxui::Event::Special("\x1B.") ||
            event == ftxui::Event::F1 ||
            event == ftxui::Event::F2 ||
            event == ftxui::Event::F3 ||
            event == ftxui::Event::F4 ||
            event == ftxui::Event::F5 ||
            event == ftxui::Event::F6 ||
            event == ftxui::Event::F7 ||
            event == ftxui::Event::F8 ||
            event == ftxui::Event::F9 ||
            event == ftxui::Event::F10;
        const bool listing_focused =
            !(tree_panel && tree_panel_focused) &&
            !(quick_view_panel && quick_view_focused) &&
            !(information_panel && information_panel_focused);
        const bool command_navigation =
            event == ftxui::Event::ArrowLeft ||
            event == ftxui::Event::ArrowRight ||
            event == ftxui::Event::Home ||
            event == ftxui::Event::End ||
            event == ftxui::Event::CtrlA;
        if (!command_mode && listing_focused &&
            (event == ftxui::Event::AltP || event == ftxui::Event::AltN) &&
            !command_input.history.empty()) {
            command_mode = true;
            command_input.reset(command_line);
        }
        if (command_mode && listing_focused &&
            (!panel_shortcut || command_navigation) &&
            !panel_search_confirmed) {
            if (event == ftxui::Event::Escape) {
                command_mode = false;
                command_line.clear();
                command_input.reset(command_line);
            } else if (event == ftxui::Event::Return) {
                execute_command();
            } else {
                return edit_input(command_line, command_input, event);
            }
            return true;
        }
        if (traversal_running.load(std::memory_order_acquire) &&
            (
                event == ftxui::Event::Return ||
                event == ftxui::Event::CtrlO ||
                event == ftxui::Event::F1 ||
                event == ftxui::Event::F2 ||
                event == ftxui::Event::F3 ||
                event == ftxui::Event::F4 ||
                event == ftxui::Event::F5 ||
                event == ftxui::Event::F6 ||
                event == ftxui::Event::F7 ||
                event == ftxui::Event::F8 ||
                event == ftxui::Event::F9
            )) {
            status = "Search running; Ctrl-C cancels";
            return true;
        }
        try {
            if (event.is_mouse()) {
                const auto& mouse = event.mouse();
                if (mouse.button == ftxui::Mouse::Left &&
                    mouse.motion == ftxui::Mouse::Pressed) {
                    for (std::size_t index = 0;
                         index < top_menu_boxes.size();
                         ++index) {
                        if (top_menu_boxes[index].Contain(
                                mouse.x,
                                mouse.y
                            )) {
                            pulldown_category = index;
                            reset_pulldown_item();
                            pulldown_dropped = true;
                            mode = Mode::pulldown;
                            return true;
                        }
                    }
                }
                if (quick_view_panel &&
                    quick_view_box.Contain(mouse.x, mouse.y)) {
                    const auto viewport_width =
                        quick_view_dimensions().first;
                    if (mouse.button == ftxui::Mouse::WheelUp) {
                        quick_viewer.scroll_lines(
                            -3,
                            viewport_width
                        );
                    } else if (
                        mouse.button == ftxui::Mouse::WheelDown
                    ) {
                        quick_viewer.scroll_lines(
                            3,
                            viewport_width
                        );
                    } else if (
                        mouse.button == ftxui::Mouse::Left &&
                        mouse.motion == ftxui::Mouse::Pressed
                    ) {
                        quick_view_focused = true;
                    } else {
                        return false;
                    }
                    return true;
                }
                if (information_panel &&
                    information_panel_box.Contain(
                        mouse.x,
                        mouse.y
                    ) &&
                    mouse.button == ftxui::Mouse::Left &&
                    mouse.motion == ftxui::Mouse::Pressed) {
                    information_panel_focused = true;
                    return true;
                }
                if (tree_panel &&
                    tree_panel_box.Contain(mouse.x, mouse.y)) {
                    tree_panel_focused = true;
                    if (mouse.button == ftxui::Mouse::WheelUp ||
                        mouse.button == ftxui::Mouse::WheelDown) {
                        tree_cursor = static_cast<std::size_t>(std::clamp(
                            static_cast<long long>(tree_cursor) +
                                (mouse.button == ftxui::Mouse::WheelUp ? -3 : 3),
                            0LL,
                            static_cast<long long>(
                                tree_nodes.empty() ? 0U : tree_nodes.size() - 1U
                            )
                        ));
                        last_click_pane = -1;
                        update_tree_selection();
                        return true;
                    }
                    if (mouse.button != ftxui::Mouse::Left ||
                        mouse.motion != ftxui::Mouse::Pressed) {
                        return false;
                    }
                    for (std::size_t index = 0;
                         index < tree_row_boxes.size();
                         ++index) {
                        if (tree_row_boxes[index].y_min <=
                                tree_panel_box.y_min ||
                            tree_row_boxes[index].y_max >=
                                tree_panel_box.y_max ||
                            !tree_row_boxes[index].Contain(
                                mouse.x,
                                mouse.y
                            )) {
                            continue;
                        }
                        if (index >= tree_row_paths.size()) {
                            return true;
                        }
                        const auto& path = tree_row_paths[index];
                        const auto found = std::find_if(
                            tree_nodes.begin(), tree_nodes.end(),
                            [&](const TreeNode& node) { return node.path == path; }
                        );
                        if (found == tree_nodes.end()) {
                            return true;
                        }
                        const auto selected_index = static_cast<std::size_t>(
                            std::distance(tree_nodes.begin(), found)
                        );
                        const auto now =
                            std::chrono::steady_clock::now();
                        const bool double_click =
                            last_click_pane ==
                                10 + *tree_panel &&
                            last_click_path == path &&
                            now - last_click <
                                std::chrono::milliseconds(400);
                        last_click = now;
                        last_click_pane = 10 + *tree_panel;
                        last_click_path = path;
                        tree_cursor = selected_index;
                        update_tree_selection();
                        if (double_click) {
                            active().change_directory(
                                tree_nodes[tree_cursor].path
                            );
                            if (!tree_nodes[tree_cursor].expanded &&
                                !tree_nodes[tree_cursor].loading) {
                                expand_tree_node(tree_cursor);
                            }
                        }
                        return true;
                    }
                    return true;
                }
                Pane* pane = nullptr;
                int pane_index = active_index;
                std::vector<ftxui::Box>* boxes = nullptr;
                if (
                    quick_view_panel != 0 &&
                    information_panel != 0 &&
                    tree_panel != 0 &&
                    left_box.Contain(mouse.x, mouse.y)
                ) {
                    pane_index = 0;
                    pane = &left;
                    boxes = &left_entry_boxes;
                } else if (
                    quick_view_panel != 1 &&
                    information_panel != 1 &&
                    tree_panel != 1 &&
                    right_box.Contain(mouse.x, mouse.y)
                ) {
                    pane_index = 1;
                    pane = &right;
                    boxes = &right_entry_boxes;
                }
                if (pane != nullptr &&
                    mouse.button == ftxui::Mouse::WheelUp) {
                    active_index = pane_index;
                    quick_view_focused = false;
                    information_panel_focused = false;
                    tree_panel_focused = false;
                    pane->move_cursor(-3);
                    return true;
                }
                if (pane != nullptr &&
                    mouse.button == ftxui::Mouse::WheelDown) {
                    active_index = pane_index;
                    quick_view_focused = false;
                    information_panel_focused = false;
                    tree_panel_focused = false;
                    pane->move_cursor(3);
                    return true;
                }
                if (mouse.button != ftxui::Mouse::Left ||
                    mouse.motion != ftxui::Mouse::Pressed) {
                    return false;
                }
                if (pane != nullptr && boxes != nullptr) {
                    active_index = pane_index;
                    quick_view_focused = false;
                    information_panel_focused = false;
                    tree_panel_focused = false;
                    for (std::size_t index = 0;
                         index < boxes->size();
                         ++index) {
                        if ((*boxes)[index].Contain(mouse.x, mouse.y)) {
                            pane->move_cursor(
                                static_cast<int>(index) -
                                static_cast<int>(pane->cursor())
                            );
                            if (mouse.control) {
                                pane->toggle_selected();
                            }
                            const auto now =
                                std::chrono::steady_clock::now();
                            if (last_click_pane == active_index &&
                                last_click_index == index &&
                                now - last_click <
                                    std::chrono::milliseconds(400)) {
                                static_cast<void>(pane->enter());
                            }
                            last_click = now;
                            last_click_pane = active_index;
                            last_click_index = index;
                            return true;
                        }
                    }
                }
                return pane != nullptr;
            }
            if (information_panel &&
                information_panel_focused &&
                (
                    event == ftxui::Event::F2 ||
                    event == ftxui::Event::F3 ||
                    event == ftxui::Event::F4 ||
                    event == ftxui::Event::F5 ||
                    event == ftxui::Event::F6 ||
                    event == ftxui::Event::F7 ||
                    event == ftxui::Event::F8 ||
                    event == ftxui::Event::Insert ||
                    event == ftxui::Event::CtrlA ||
                    event ==
                        ftxui::Event::Special("\x1B[1;2A") ||
                    event ==
                        ftxui::Event::Special("\x1B[1;2B") ||
                    event == ftxui::Event::ArrowUp ||
                    event == ftxui::Event::ArrowDown ||
                    event == ftxui::Event::ArrowLeft ||
                    event == ftxui::Event::ArrowRight ||
                    event == ftxui::Event::PageUp ||
                    event == ftxui::Event::PageDown ||
                    event == ftxui::Event::Home ||
                    event == ftxui::Event::End ||
                    event == ftxui::Event::Return ||
                    event == ftxui::Event::Character('+') ||
                    event == ftxui::Event::Character('-') ||
                    event == ftxui::Event::Character('*')
                )) {
                status = "No command for this Info-panel key";
                return true;
            }
            if (quick_view_panel && quick_view_focused) {
                const auto [viewport_width, viewport_lines] =
                    quick_view_dimensions();
                if (event == ftxui::Event::F1 ||
                    event == ftxui::Event::F5 ||
                    event == ftxui::Event::F6 ||
                    event == ftxui::Event::F8 ||
                    event == ftxui::Event::Insert ||
                    event == ftxui::Event::CtrlA ||
                    event ==
                        ftxui::Event::Special("\x1B[1;2A") ||
                    event ==
                        ftxui::Event::Special("\x1B[1;2B") ||
                    event == ftxui::Event::Character('+') ||
                    event == ftxui::Event::Character('-') ||
                    event == ftxui::Event::Character('*')) {
                    status = "No command for this quick-view key";
                    return true;
                }
                if (event == ftxui::Event::F2) {
                    if (!quick_viewer.hex()) {
                        quick_viewer.set_wrap(!quick_viewer.wrap());
                    }
                    return true;
                }
                if (event == ftxui::Event::F4) {
                    quick_viewer.toggle_hex();
                    return true;
                }
                if (event == ftxui::Event::F7) {
                    start_prompt(
                        PromptAction::quick_viewer_find,
                        quick_viewer.hex() ? "Find hexadecimal bytes" : "Find in file",
                        quick_view_search_hex == quick_viewer.hex()
                            ? quick_view_search_needle
                            : ""
                    );
                    return true;
                }
                if (event == ftxui::Event::ArrowDown) {
                    quick_viewer.scroll_lines(1, viewport_width);
                    return true;
                }
                if (event == ftxui::Event::ArrowUp) {
                    quick_viewer.scroll_lines(-1, viewport_width);
                    return true;
                }
                if (event == ftxui::Event::Home) {
                    quick_viewer.go_to_start();
                    return true;
                }
                if (event == ftxui::Event::End) {
                    quick_viewer.go_to_end(viewport_width, viewport_lines);
                    return true;
                }
                if (event == ftxui::Event::ArrowLeft) {
                    quick_viewer.scroll_columns(-1);
                    return true;
                }
                if (event == ftxui::Event::ArrowRight) {
                    quick_viewer.scroll_columns(1);
                    return true;
                }
                if (event == ftxui::Event::PageDown) {
                    quick_viewer.page(
                        1,
                        viewport_width,
                        viewport_lines
                    );
                    return true;
                }
                if (event == ftxui::Event::PageUp) {
                    quick_viewer.page(
                        -1,
                        viewport_width,
                        viewport_lines
                    );
                    return true;
                }
            }
            if (tree_panel && tree_panel_focused) {
                if (event == ftxui::Event::CtrlS ||
                    event == ftxui::Event::AltS) {
                    tree_search_mode = true;
                    tree_search.clear();
                    status = "Tree search: ";
                    return true;
                }
                if (event == ftxui::Event::Character(' ') ||
                    event == ftxui::Event::Character('+')) {
                    if (tree_cursor < tree_nodes.size() &&
                        (tree_nodes[tree_cursor].loading ||
                         tree_refresh_pending ==
                             tree_nodes[tree_cursor].path)) {
                        if (event == ftxui::Event::Character(' ')) {
                            collapse_tree_node(tree_cursor);
                        } else {
                            status = "Tree branch is loading";
                        }
                        return true;
                    }
                    if (tree_cursor < tree_nodes.size()) {
                        if (event == ftxui::Event::Character('+') &&
                            tree_nodes[tree_cursor].expanded) {
                            status = "Tree branch already expanded";
                        } else {
                            expand_tree_node(tree_cursor);
                        }
                    }
                    return true;
                }
                if (event == ftxui::Event::Character('-')) {
                    if (tree_cursor < tree_nodes.size() &&
                        (tree_nodes[tree_cursor].loading ||
                         tree_refresh_pending ==
                             tree_nodes[tree_cursor].path)) {
                        collapse_tree_node(tree_cursor);
                        return true;
                    }
                    if (tree_cursor < tree_nodes.size() &&
                        tree_nodes[tree_cursor].expanded) {
                        expand_tree_node(tree_cursor);
                    } else {
                        status = "Tree branch already collapsed";
                    }
                    return true;
                }
                if (event == ftxui::Event::F1 ||
                    event == ftxui::Event::F7 ||
                    event == ftxui::Event::Insert ||
                    event == ftxui::Event::CtrlA ||
                    event ==
                        ftxui::Event::Special("\x1B[1;2A") ||
                    event ==
                        ftxui::Event::Special("\x1B[1;2B") ||
                    event == ftxui::Event::Character('*')) {
                    status =
                        "This tree command is not implemented";
                    return true;
                }
                if (event == ftxui::Event::F2) {
                    refresh(true);
                    return true;
                }
                if (event == ftxui::Event::F3) {
                    invalidate_tree_loading();
                    tree_refresh_pending.reset();
                    tree_refresh_notify_pending = false;
                    if (tree_cursor > 0U &&
                        tree_cursor < tree_nodes.size()) {
                        const auto depth =
                            tree_nodes[tree_cursor].depth;
                        auto end = tree_cursor + 1U;
                        while (end < tree_nodes.size() &&
                               tree_nodes[end].depth > depth) {
                            ++end;
                        }
                        tree_nodes.erase(
                            tree_nodes.begin() +
                                static_cast<std::ptrdiff_t>(
                                    tree_cursor
                                ),
                            tree_nodes.begin() +
                                static_cast<std::ptrdiff_t>(end)
                        );
                        if (tree_cursor >= tree_nodes.size()) {
                            --tree_cursor;
                        }
                        sync_dynamic_tree();
                    }
                    return true;
                }
                if (event == ftxui::Event::F4) {
                    tree_dynamic = !tree_dynamic;
                    status = tree_dynamic
                                 ? "Dynamic tree navigation"
                                 : "Static tree navigation";
                    return true;
                }
                if (event == ftxui::Event::F5) {
                    static_cast<void>(
                        prepare_tree_operation(false, false)
                    );
                    return true;
                }
                if (event == ftxui::Event::F6) {
                    static_cast<void>(
                        prepare_tree_operation(true, false)
                    );
                    return true;
                }
                if (event == ftxui::Event::F8) {
                    if (prepare_tree_operation(false, true) &&
                        !config.confirm_delete) {
                        run_remove_operation();
                    }
                    return true;
                }
                if (event == ftxui::Event::ArrowUp) {
                    if (tree_cursor > 0U) {
                        --tree_cursor;
                    }
                    update_tree_selection();
                    return true;
                }
                if (event == ftxui::Event::ArrowDown) {
                    if (tree_cursor + 1U < tree_nodes.size()) {
                        ++tree_cursor;
                    }
                    update_tree_selection();
                    return true;
                }
                if (event == ftxui::Event::Home) {
                    tree_cursor = 0;
                    update_tree_selection();
                    return true;
                }
                if (event == ftxui::Event::End) {
                    tree_cursor =
                        tree_nodes.empty()
                            ? 0U
                            : tree_nodes.size() - 1U;
                    update_tree_selection();
                    return true;
                }
                if (event == ftxui::Event::ArrowRight) {
                    if (tree_cursor < tree_nodes.size() &&
                        !tree_nodes[tree_cursor].expanded) {
                        expand_tree_node(tree_cursor);
                    } else if (
                        tree_cursor + 1U < tree_nodes.size() &&
                        tree_nodes[tree_cursor + 1U].depth >
                            tree_nodes[tree_cursor].depth
                    ) {
                        ++tree_cursor;
                        update_tree_selection();
                    }
                    return true;
                }
                if (event == ftxui::Event::ArrowLeft) {
                    if (tree_cursor < tree_nodes.size() &&
                        tree_nodes[tree_cursor].depth > 0U) {
                        const auto depth =
                            tree_nodes[tree_cursor].depth;
                        while (tree_cursor > 0U) {
                            --tree_cursor;
                            if (tree_nodes[tree_cursor].depth <
                                depth) {
                                break;
                            }
                        }
                        update_tree_selection();
                    }
                    return true;
                }
                if (event == ftxui::Event::PageUp) {
                    tree_cursor =
                        tree_cursor > panel_visible_rows()
                            ? tree_cursor - panel_visible_rows()
                            : 0U;
                    update_tree_selection();
                    return true;
                }
                if (event == ftxui::Event::PageDown) {
                    tree_cursor = std::min(
                        tree_nodes.empty()
                            ? 0U
                            : tree_nodes.size() - 1U,
                        tree_cursor + panel_visible_rows()
                    );
                    update_tree_selection();
                    return true;
                }
                if (event == ftxui::Event::Return &&
                    tree_cursor < tree_nodes.size()) {
                    active().change_directory(
                        tree_nodes[tree_cursor].path
                    );
                    if (!tree_nodes[tree_cursor].expanded &&
                        !tree_nodes[tree_cursor].loading) {
                        expand_tree_node(tree_cursor);
                    }
                    status =
                        "Directory changed: " +
                        tree_nodes[tree_cursor].path;
                    return true;
                }
            }
            if (event == ftxui::Event::Tab) {
                if (quick_view_panel) {
                    quick_view_focused = !quick_view_focused;
                    status = quick_view_focused
                                 ? "Quick view focused"
                                 : "File listing focused";
                } else if (information_panel) {
                    information_panel_focused =
                        !information_panel_focused;
                    status = information_panel_focused
                                 ? "Info panel focused"
                                 : "File listing focused";
                } else if (tree_panel) {
                    tree_panel_focused = !tree_panel_focused;
                    status = "Ready";
                } else {
                    active_index = 1 - active_index;
                }
            } else if (
                event == ftxui::Event::Special("\x1B[1;2A")
            ) {
                active().toggle_selected(-1);
            } else if (
                event == ftxui::Event::Special("\x1B[1;2B")
            ) {
                active().toggle_selected(1);
            } else if (event == ftxui::Event::ArrowDown) {
                active().move_cursor(1);
            } else if (event == ftxui::Event::ArrowUp) {
                active().move_cursor(-1);
            } else if (event == ftxui::Event::ArrowLeft ||
                       event == ftxui::Event::ArrowRight) {
                return true;
            } else if (
                event == ftxui::Event::Special("\x1B[5;5~")
            ) {
                static_cast<void>(active().up());
            } else if (
                event == ftxui::Event::Special("\x1B[6;5~")
            ) {
                static_cast<void>(active().enter());
            } else if (event == ftxui::Event::PageDown) {
                active().page_cursor(
                    static_cast<int>(panel_visible_rows())
                );
            } else if (event == ftxui::Event::PageUp) {
                active().page_cursor(
                    -static_cast<int>(panel_visible_rows())
                );
            } else if (event == ftxui::Event::Home) {
                active().move_cursor(
                    -static_cast<int>(active().cursor())
                );
            } else if (event == ftxui::Event::End) {
                active().move_cursor(
                    static_cast<int>(active().entries().size())
                );
            } else if (event == ftxui::Event::Return) {
                static_cast<void>(active().enter());
            } else if (event == ftxui::Event::AltY) {
                active().history_back();
            } else if (event == ftxui::Event::AltU) {
                active().history_forward();
            } else if (event == ftxui::Event::Insert) {
                active().toggle_selected();
            } else if (event == ftxui::Event::CtrlS ||
                       event == ftxui::Event::AltS) {
                panel_search_mode = true;
                panel_search.clear();
                status = "Quick search: ";
            } else if (event == ftxui::Event::CtrlA) {
                active().select_all();
            } else if (event == ftxui::Event::CtrlR) {
                refresh(true);
                if (!tree_panel ||
                    !tree_expansion_running.load(std::memory_order_acquire)) {
                    status = "Refreshed";
                }
            } else if (event == ftxui::Event::CtrlU) {
                swap_panels();
            } else if (
                event == ftxui::Event::Special("\x1C")
            ) {
                execute_pulldown_action(2U, 7);
            } else if (
                event == ftxui::Event::Special("\x1B?")
            ) {
                execute_pulldown_action(2U, 0);
            } else if (event == ftxui::Event::AltC) {
                start_prompt(
                    PromptAction::quick_cd,
                    "Change directory",
                    active().path()
                );
            } else if (
                event == ftxui::Event::Special("\x1B.")
            ) {
                active().set_show_hidden(
                    !active().show_hidden()
                );
            } else if (event == ftxui::Event::CtrlC &&
                       operation_running.load(
                           std::memory_order_acquire
                       )) {
                request_operation_cancel();
                status = "Cancelling operation";
            } else if (event == ftxui::Event::CtrlC &&
                       traversal_running.load(
                           std::memory_order_acquire
                       )) {
                traversal_cancellation.request();
                status = "Cancelling search";
            } else if (event == ftxui::Event::CtrlO) {
                const auto result = run_process("", true);
                status = "Subshell exited " +
                         std::to_string(result);
            } else if (event == ftxui::Event::F1) {
                mode = Mode::help;
            } else if (event == ftxui::Event::F2) {
                mode = Mode::user_menu;
            } else if (event == ftxui::Event::F3) {
                const auto* entry = active().current();
                if (entry != nullptr && can_open_file(*entry)) {
                    viewer.open(active().provider(), entry->path);
                    viewer_search_needle.clear();
                    mode = Mode::viewer;
                }
            } else if (event == ftxui::Event::F4) {
                const auto* entry = active().current();
                if (entry != nullptr && can_open_file(*entry)) {
                    open_editor_buffer(
                        active().provider(),
                        entry->path,
                        false
                    );
                    mode = Mode::editor;
                }
            } else if (event == ftxui::Event::F5) {
                start_operation_dialog(false, false);
            } else if (event == ftxui::Event::F6) {
                const auto sources = active().selected_entries();
                start_operation_dialog(
                    true,
                    sources.size() == 1U
                );
            } else if (event == ftxui::Event::F7) {
                start_prompt(
                    PromptAction::mkdir,
                    "New directory"
                );
            } else if (event == ftxui::Event::F8) {
                if (config.confirm_delete) {
                    start_delete_dialog();
                } else {
                    run_remove_operation();
                }
            } else if (event == ftxui::Event::F9) {
                const auto menu_panel =
                    quick_view_panel && quick_view_focused
                        ? *quick_view_panel
                        : information_panel &&
                                  information_panel_focused
                              ? *information_panel
                              : tree_panel && tree_panel_focused
                                    ? *tree_panel
                        : active_index;
                pulldown_category =
                    menu_panel == 0 ? 0U : 4U;
                reset_pulldown_item();
                pulldown_dropped = false;
                mode = Mode::pulldown;
            } else if (event == ftxui::Event::F10) {
                if (traversal_running.load(
                        std::memory_order_acquire
                    )) {
                    traversal_cancellation.request();
                    status = "Cancel search before quitting";
                    return true;
                }
                if (operation_running.load(
                        std::memory_order_acquire
                    )) {
                    request_operation_cancel();
                    status = "Cancel operation before quitting";
                    return true;
                }
                exit_terminal();
            } else if (
                selection_shortcut &&
                event == ftxui::Event::Character('+')
            ) {
                command_mode = false;
                execute_pulldown_action(1U, 15);
            } else if (
                selection_shortcut &&
                event == ftxui::Event::Character('-')
            ) {
                command_mode = false;
                execute_pulldown_action(1U, 16);
            } else if (
                selection_shortcut &&
                event == ftxui::Event::Character('*')
            ) {
                command_mode = false;
                execute_pulldown_action(1U, 17);
            } else if (event.is_character()) {
                command_mode = true;
                command_line = event.character();
                command_input.reset(command_line);
            } else {
                return false;
            }
        } catch (const std::exception& error) {
            set_error(error);
        }
        return true;
    }

    bool handle(const ftxui::Event& event) {
        if (event.is_mouse() &&
            (mode == Mode::panels || mode == Mode::viewer ||
             mode == Mode::editor || mode == Mode::search_results)) {
            auto copy = event;
            const auto& mouse = copy.mouse();
            if (mouse.button == ftxui::Mouse::Left &&
                mouse.motion == ftxui::Mouse::Pressed) {
                static const std::array<ftxui::Event, 10> keys{
                    ftxui::Event::F1, ftxui::Event::F2, ftxui::Event::F3,
                    ftxui::Event::F4, ftxui::Event::F5, ftxui::Event::F6,
                    ftxui::Event::F7, ftxui::Event::F8, ftxui::Event::F9,
                    ftxui::Event::F10
                };
                for (std::size_t index = 0; index < keys.size(); ++index) {
                    if (function_key_boxes[index].Contain(mouse.x, mouse.y)) {
                        return !function_key_enabled[index] || handle(keys[index]);
                    }
                }
            }
        }
        if (mode == Mode::prompt) {
            return handle_prompt(event);
        }
        if (mode == Mode::viewer) {
            return handle_viewer(event);
        }
        if (mode == Mode::viewer_close_dialog) {
            return handle_viewer_close_dialog(event);
        }
        if (mode == Mode::viewer_wrap_dialog) {
            return handle_viewer_wrap_dialog(event);
        }
        if (mode == Mode::viewer_message) {
            std::size_t choice = 0;
            const auto input = handle_button_input(
                event, choice, std::span<const ftxui::Box>(&message_button_box, 1U)
            );
            if (input == ButtonInput::cancel || input == ButtonInput::activate) {
                mode = Mode::viewer;
                return true;
            }
            return input != ButtonInput::ignored;
        }
        if (mode == Mode::editor) {
            return handle_editor(event);
        }
        if (mode == Mode::editor_menu) {
            return handle_editor_menu(event);
        }
        if (mode == Mode::editor_help) {
            if (modal_cancel(event) || event == ftxui::Event::F1) {
                mode = Mode::editor;
                return true;
            }
            return false;
        }
        if (mode == Mode::editor_error) {
            std::size_t choice = 0;
            const auto input = handle_button_input(
                event, choice, std::span<const ftxui::Box>(&message_button_box, 1U)
            );
            if (input == ButtonInput::cancel || input == ButtonInput::activate) {
                mode = Mode::editor;
                return true;
            }
            return input != ButtonInput::ignored;
        }
        if (mode == Mode::editor_replace_confirm) {
            return handle_editor_replace_confirm(event);
        }
        if (mode == Mode::editor_search_dialog ||
            mode == Mode::editor_replace_dialog) {
            return handle_editor_search_dialog(event);
        }
        if (mode == Mode::editor_close_dialog) {
            return handle_editor_close_dialog(event);
        }
        if (mode == Mode::editor_save_dialog) {
            return handle_editor_save_dialog(event);
        }
        if (mode == Mode::editor_link_dialog) {
            return handle_editor_link_dialog(event);
        }
        if (mode == Mode::editor_overwrite_dialog) {
            return handle_editor_overwrite_dialog(event);
        }
        if (mode == Mode::editor_options_dialog) {
            return handle_editor_options_dialog(event);
        }
        if (mode == Mode::editor_history ||
            mode == Mode::editor_buffer_list) {
            return handle_editor_list(event);
        }
        if (mode == Mode::help) {
            if (modal_cancel(event) || event == ftxui::Event::F1) {
                mode = Mode::panels;
                return true;
            }
            return false;
        }
        if (mode == Mode::user_menu) {
            return handle_menu(event);
        }
        if (mode == Mode::pulldown) {
            return handle_pulldown(event);
        }
        if (mode == Mode::search_results) {
            return handle_search_results(event);
        }
        if (mode == Mode::bookmarks) {
            return handle_bookmarks(event);
        }
        if (mode == Mode::information) {
            if (modal_cancel(event)) {
                mode = Mode::panels;
                return true;
            }
            return false;
        }
        if (mode == Mode::operation_dialog) {
            return handle_operation_dialog(event);
        }
        if (mode == Mode::delete_dialog) {
            return handle_delete_dialog(event);
        }
        if (mode == Mode::operation_progress) {
            return handle_operation_progress(event);
        }
        if (mode == Mode::conflict_dialog || mode == Mode::operation_error_dialog) {
            return handle_conflict_dialog(event);
        }
        if (mode == Mode::jobs) {
            return handle_jobs(event);
        }
        if (mode == Mode::compare_dialog) {
            return handle_compare_dialog(event);
        }
        return handle_panels(event);
    }
};

App::App(
    std::string left,
    std::string right,
    Config config
)
    : impl_(std::make_unique<Impl>(
          std::move(left),
          std::move(right),
          std::move(config)
      )) {}

App::~App() = default;

void App::open_viewer(const std::string& path) {
    const auto resolved =
        std::filesystem::path(path).is_absolute()
            ? path
            : impl_->active().provider()->join(
                  impl_->active().path(),
                  path
              );
    impl_->viewer.open(impl_->active().provider(), resolved);
    impl_->viewer_search_needle.clear();
    impl_->exit_after_viewer = true;
    impl_->mode = Mode::viewer;
}

void App::open_editor(const std::string& path) {
    const auto resolved =
        std::filesystem::path(path).is_absolute()
            ? path
            : impl_->active().provider()->join(
                  impl_->active().path(),
                  path
              );
    impl_->open_editor_buffer(
        impl_->active().provider(),
        resolved,
        false
    );
    impl_->exit_after_editor = true;
    impl_->mode = Mode::editor;
}

std::string App::active_path() const {
    return impl_->active_index == 0
               ? impl_->left.path()
               : impl_->right.path();
}

int App::run() {
    struct TerminalCleanup {
        ~TerminalCleanup() {
            std::cout << "\033[0m\n" << std::flush;
        }
    } terminal_cleanup;
    const auto run_terminal = [&] {
        auto terminal = ftxui::App::FullscreenAlternateScreen();
        terminal.ForceHandleCtrlC(false);
        terminal.ForceHandleCtrlZ(false);
        terminal.TrackMouse(true);
        impl_->terminal = &terminal;
        struct OperationJoinGuard {
            Impl* implementation;
            bool finished{false};

            void finish() noexcept {
                if (finished) {
                    return;
                }
                finished = true;
                if (implementation->operation_running.load(
                        std::memory_order_acquire
                    )) {
                    implementation->request_operation_cancel();
                }
                if (implementation->operation_thread.joinable()) {
                    implementation->operation_thread.join();
                }
                if (implementation->traversal_running.load(
                        std::memory_order_acquire
                    )) {
                    implementation->traversal_cancellation.request();
                }
                if (implementation->traversal_thread.joinable()) {
                    implementation->traversal_thread.join();
                }
                if (implementation->tree_expansion_running.load(
                        std::memory_order_acquire
                    )) {
                    implementation->tree_expansion_cancellation.request();
                }
                if (implementation->tree_thread.joinable()) {
                    implementation->tree_thread.join();
                }
                implementation->terminal = nullptr;
            }

            ~OperationJoinGuard() {
                finish();
            }
        } operation_guard{impl_.get()};
        bool first_frame = true;
        auto component = ftxui::Renderer([&] {
            if (first_frame) {
                first_frame = false;
                std::cout << "\033[H\033[2J" << std::flush;
            }
            return impl_->render();
        });
        component = ftxui::CatchEvent(
            std::move(component),
            [&](const ftxui::Event& event) {
                const auto handled = impl_->handle(event);
                impl_->update_editor_view();
                return handled;
            }
        );
        terminal.Loop(std::move(component));
        operation_guard.finish();
    };
    run_terminal();
    impl_->store_panel_options_in_config();
    impl_->store_editor_options_in_config();
    try {
        impl_->config.save(default_config_path());
    } catch (const std::exception& error) {
        std::cerr << "mcpp: cannot save setup: " << error.what() << '\n';
        return 1;
    }
    return 0;
}

}  // namespace mcpp
