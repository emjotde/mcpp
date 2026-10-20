#include "mcpp/app.hpp"

#include "mcpp/archive_provider.hpp"
#include "mcpp/compare.hpp"
#include "mcpp/completion.hpp"
#include "mcpp/editor_clipboard.hpp"
#include "mcpp/editor_lock.hpp"
#include "mcpp/extension.hpp"
#include "mcpp/file_highlight.hpp"
#include "mcpp/help.hpp"
#include "mcpp/hotlist.hpp"
#include "mcpp/input_history.hpp"
#include "mcpp/keymap.hpp"
#include "mcpp/keymap_input.hpp"
#include "mcpp/local_provider.hpp"
#include "mcpp/local_tools.hpp"
#include "mcpp/location.hpp"
#include "mcpp/listing.hpp"
#include "mcpp/operation_job.hpp"
#include "mcpp/search.hpp"
#include "mcpp/shell.hpp"
#include "mcpp/skin.hpp"
#include "mcpp/spelling.hpp"
#include "mcpp/syntax.hpp"
#include "mcpp/terminal_input.hpp"
#include "mcpp/tree_cache.hpp"
#include "mcpp/diff.hpp"
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
#include <exception>
#include <fcntl.h>
#include <functional>
#include <fnmatch.h>
#include <iomanip>
#include <fstream>
#include <iostream>
#include <iterator>
#include <linux/fs.h>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <span>
#include <set>
#include <stdexcept>
#include <sys/wait.h>
#include <sys/stat.h>
#include <thread>
#include <unordered_map>
#include <utility>
#include <unistd.h>
#include <pwd.h>
#include <grp.h>
#include <termios.h>

#include <ftxui/component/app.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/string.hpp>
#include <ftxui/screen/terminal.hpp>
#include <ftxui/screen/screen.hpp>

namespace mcpp {
namespace {

bool viewer_stdin_path(const std::string_view path) {
    return path == "/dev/stdin" || path == "/dev/fd/0" || path == "/proc/self/fd/0" ||
           path == "/proc/" + std::to_string(::getpid()) + "/fd/0";
}

class ViewerStdin {
public:
    ViewerStdin() {
        if (::isatty(STDIN_FILENO) != 0) {
            throw std::runtime_error("viewer standard input is the terminal; redirect data or name a file");
        }
        original_flags_ = ::fcntl(STDIN_FILENO, F_GETFD);
        data_ = ::fcntl(STDIN_FILENO, F_DUPFD_CLOEXEC, 3);
        if (data_ < 0) throw std::system_error(errno, std::generic_category(), "retain viewer standard input");
        const auto keyboard = ::open("/dev/tty", O_RDONLY | O_NOCTTY | O_CLOEXEC);
        if (keyboard < 0) {
            const auto failure = errno;
            ::close(data_);
            data_ = -1;
            throw std::system_error(failure, std::generic_category(), "open viewer terminal keyboard");
        }
        const auto attached = ::dup2(keyboard, STDIN_FILENO);
        const auto failure = errno;
        ::close(keyboard);
        if (attached < 0) {
            ::close(data_);
            data_ = -1;
            throw std::system_error(failure, std::generic_category(), "attach viewer terminal keyboard");
        }
    }
    ~ViewerStdin() {
        if (data_ < 0) return;
        if (::dup2(data_, STDIN_FILENO) < 0 ||
            (original_flags_ >= 0 && ::fcntl(STDIN_FILENO, F_SETFD, original_flags_) < 0)) {
            std::cerr << "mcpp: cannot restore viewer standard input: " << std::strerror(errno) << '\n';
        }
        ::close(data_);
    }
    ViewerStdin(const ViewerStdin&) = delete;
    ViewerStdin& operator=(const ViewerStdin&) = delete;
    [[nodiscard]] int data() const noexcept { return data_; }
private:
    int data_{-1};
    int original_flags_{0};
};

struct EditorSearchState {
    std::string needle;
    EditorSearchOptions options;
    bool in_selection{false};
    std::optional<std::pair<EditorPosition, EditorPosition>> scope;
    std::uint64_t change_id{0};
};

struct EditorBuffer;
struct EditorEditBlocked {
    EditorBuffer* buffer;
    EditorLockConflict conflict;
};

struct EditorBuffer : EditorModel {
    std::uint64_t screen_id{0};
    std::optional<std::pair<std::uint64_t, std::size_t>> diff_screen_return;
    EditorLock file_lock;
    std::function<void(EditorBuffer&)> before_change;
    bool changed_once{false};
    EditorSearchState search;
    std::string base_directory;
    std::size_t first_row{0};
    std::size_t first_column{0};
    bool fullscreen{true};
    int window_x{0};
    int window_y{0};
    int window_width{80};
    int window_height{24};
    bool window_geometry_saved{false};

    void guard_change() {
        try {
            if (before_change) before_change(*this);
        } catch (const EditorLockConflict& conflict) {
            throw EditorEditBlocked{this, conflict};
        }
        changed_once = true;
    }

    void close() {
        file_lock.reset();
        before_change = {};
        changed_once = false;
        EditorModel::close();
    }

    void replace_text(const EditorPosition begin, const EditorPosition end, const std::string_view value) {
        guard_change();
        EditorModel::replace_text(begin, end, value);
    }
    void replace_block(const std::string_view value) {
        guard_change();
        EditorModel::replace_block(value);
    }
    void set_text(const std::string_view value) {
        guard_change();
        EditorModel::set_text(value);
    }
    void insert(const char value) {
        guard_change();
        EditorModel::insert(value);
    }
    void insert(const std::string_view value) {
        if (!value.empty()) guard_change();
        EditorModel::insert(value);
    }
    void paste(const std::string_view value) {
        if (!value.empty()) guard_change();
        EditorModel::paste(value);
    }
    void paste_bytes(const std::string_view value) {
        if (!value.empty()) guard_change();
        EditorModel::paste_bytes(value);
    }
    void paste_column(const std::string_view value) {
        if (!value.empty()) guard_change();
        EditorModel::paste_column(value);
    }
    void newline(const bool indent = false) {
        guard_change();
        EditorModel::newline(indent);
    }
    void tab() {
        guard_change();
        EditorModel::tab();
    }
    void tab(const std::size_t width, const bool spaces = false) {
        guard_change();
        EditorModel::tab(width, spaces);
    }
    void backspace(const std::size_t width = 0) {
        if (selection() || row() != 0U || column() != 0U) guard_change();
        EditorModel::backspace(width);
    }
    void erase() {
        if (selection() || virtual_column() != 0U || column() < lines().at(row()).size() ||
            row() + 1U < lines().size()) guard_change();
        EditorModel::erase();
    }
    void delete_block_or_line() {
        guard_change();
        EditorModel::delete_block_or_line();
    }
    void delete_line() {
        guard_change();
        EditorModel::delete_line();
    }
    void delete_to_end() {
        if (column() < lines().at(row()).size()) guard_change();
        EditorModel::delete_to_end();
    }
    bool copy_selection() {
        if (selection()) guard_change();
        return EditorModel::copy_selection();
    }
    bool move_selection() {
        if (selection()) guard_change();
        return EditorModel::move_selection();
    }
    bool delete_selection() {
        if (selection()) guard_change();
        return EditorModel::delete_selection();
    }
    bool replace_selection(
        const std::string_view value,
        std::optional<std::pair<EditorPosition, EditorPosition>>* scope = nullptr
    ) {
        if (selection() || search_selection()) guard_change();
        return EditorModel::replace_selection(value, scope);
    }
    std::size_t replace_all(
        const std::string_view needle, const std::string_view value, const bool sensitive
    ) {
        guard_change();
        return EditorModel::replace_all(needle, value, sensitive);
    }
    std::size_t replace_all(
        const std::string_view needle, const std::string_view value, const EditorSearchOptions& options,
        const std::optional<std::pair<EditorPosition, EditorPosition>> scope = std::nullopt
    ) {
        guard_change();
        return EditorModel::replace_all(needle, value, options, scope);
    }
    bool replace(const std::string_view needle, const std::string_view value, const bool sensitive) {
        guard_change();
        return EditorModel::replace(needle, value, sensitive);
    }
    bool undo() {
        if (changed_once) guard_change();
        return EditorModel::undo();
    }
    bool redo() {
        if (changed_once) guard_change();
        return EditorModel::redo();
    }
};

struct EditorLocation {
    std::string path;
    EditorPosition position;
};

enum class Mode {
    panels,
    viewer,
    viewer_message,
    viewer_close_dialog,
    viewer_link_dialog,
    viewer_wrap_dialog,
    editor,
    editor_menu,
    editor_error,
    editor_search_dialog,
    editor_replace_dialog,
    editor_replace_confirm,
    editor_save_dialog,
    editor_save_mode_dialog,
    editor_newline_warning,
    editor_close_dialog,
    editor_link_dialog,
    editor_overwrite_dialog,
    editor_options_dialog,
    editor_history,
    editor_buffer_list,
    prompt,
    listing_dialog,
    sort_dialog,
    confirmation_dialog,
    layout_dialog,
    panel_options_dialog,
    appearance_dialog,
    display_bits_dialog,
    configure_dialog,
    external_panelize_dialog,
    chattr_dialog,
    advanced_chown_dialog,
    directory_tree_dialog,
    diff,
    diff_options,
    file_search_dialog,
    editor_mail_dialog,
    editor_spelling_dialog,
    native_input,
    native_key_query,
    native_message,
    native_query,
    local_list,
    local_command,
    learn_key,
    learn_keys_dialog,
    user_menu,
    pulldown,
    search_results,
    bookmarks,
    information,
    operation_dialog,
    delete_dialog,
    recursive_delete_dialog,
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
    diff_save,
};

struct OperationChoiceButton {
    OperationChoice choice;
    std::string_view label;
    char shortcut;
};

struct OperationPolicy {
    std::optional<CollisionDecision> collisions;
    bool older{false};
    bool ignore_errors{false};
};

struct OperationJob {
    enum class Kind { copy, move, remove };
    std::uint64_t id{0};
    Kind kind{Kind::copy};
    std::string label;
    std::string destination;
    std::string status;
    ProviderPtr source_provider;
    ProviderPtr destination_provider;
    std::vector<Entry> sources;
    OperationOptions options;
    int source_pane_id{0};
    int source_side{0};
    std::string source_directory;
    std::string source_base_directory;
    bool exact{false};
    bool compute_totals{true};
    bool background{false};
    bool finished{false};
    std::chrono::steady_clock::time_point started{std::chrono::steady_clock::now()};
    OperationJobControl control;
    std::jthread worker;
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

std::string format_size(const std::uint64_t size, const bool si = false) {
    static constexpr std::array<const char*, 5> suffixes{
        "B", "KiB", "MiB", "GiB", "TiB"
    };
    static constexpr std::array<const char*, 5> decimal_suffixes{"B", "kB", "MB", "GB", "TB"};
    const auto& units = si ? decimal_suffixes : suffixes;
    const auto base = si ? 1000.0 : 1024.0;
    auto value = static_cast<double>(size);
    std::size_t suffix = 0;
    while (value >= base && suffix + 1U < units.size()) {
        value /= base;
        ++suffix;
    }
    std::ostringstream output;
    if (suffix == 0U) {
        output << size << ' ' << units[suffix];
    } else {
        output << std::fixed << std::setprecision(1)
               << value << ' ' << units[suffix];
    }
    return output.str();
}

std::string format_panel_size(const std::uint64_t size, const bool si = false) {
    const auto decimal = std::to_string(size);
    if (decimal.size() <= 7U) {
        return decimal;
    }
    static constexpr std::array<char, 6> suffixes{
        'B', 'K', 'M', 'G', 'T', 'P'
    };
    auto value = static_cast<double>(size);
    const auto base = si ? 1000.0 : 1024.0;
    std::size_t suffix = 0;
    while (value >= base && suffix + 1U < suffixes.size()) {
        value /= base;
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

std::size_t editor_first_visible_column(
    const std::string_view line,
    const std::size_t first_column,
    const std::size_t tab_width
) {
    const auto begin = editor_offset_for_width(line, first_column, tab_width);
    const auto aligned = editor_display_width(line, begin, tab_width);
    return begin < line.size() && line[begin] == '\t' && aligned < first_column
               ? first_column
               : aligned;
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

std::string encode_hex_bytes(const std::string_view value) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(value.size() * 2U);
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        result += hex[byte >> 4U];
        result += hex[byte & 15U];
    }
    return result;
}

ftxui::Event named_event(const std::string& name) {
    using namespace ftxui;
    static const std::array<Event, 12> functions{
        Event::F1, Event::F2, Event::F3, Event::F4, Event::F5, Event::F6,
        Event::F7, Event::F8, Event::F9, Event::F10, Event::F11, Event::F12
    };
    if (name.starts_with("F")) {
        unsigned int number = 0;
        const auto parsed = std::from_chars(name.data() + 1, name.data() + name.size(), number);
        if (parsed.ec == std::errc{} && parsed.ptr == name.data() + name.size() &&
            number >= 1U && number <= functions.size()) return functions[number - 1U];
        static const std::array<std::string_view, 8> extended{
            "\x1B[25~", "\x1B[26~", "\x1B[28~", "\x1B[29~",
            "\x1B[31~", "\x1B[32~", "\x1B[33~", "\x1B[34~"
        };
        if (parsed.ec == std::errc{} && parsed.ptr == name.data() + name.size() &&
            number >= 13U && number <= 20U) return Event::Special(extended[number - 13U]);
    }
    static const std::vector<std::pair<std::string, Event>> navigation{
        {"Up", Event::ArrowUp}, {"Down", Event::ArrowDown}, {"Left", Event::ArrowLeft},
        {"Right", Event::ArrowRight}, {"Home", Event::Home}, {"End", Event::End},
        {"PageUp", Event::PageUp}, {"PageDown", Event::PageDown}, {"Tab", Event::Tab},
        {"Enter", Event::Return}, {"Insert", Event::Insert}, {"Delete", Event::Delete},
        {"Backspace", Event::Backspace}, {"Escape", Event::Escape},
        {"Completion", Event::Special("\x1B\t")}, {"BackTab", Event::TabReverse},
        {"Keypad /", Event::Character('/')}, {"Keypad *", Event::Character('*')},
        {"Keypad -", Event::Character('-')}, {"Keypad +", Event::Character('+')}
    };
    for (const auto& [label, event] : navigation) if (label == name) return event;
    throw std::runtime_error("unknown learned key action: " + name);
}

std::optional<Keymap> load_configured_keymap(const Config& config) {
    if (config.keymap_disabled) return std::nullopt;
    auto requested = config.keymap_override.value_or(config.keymap_file);
    if (!config.keymap_override) {
        for (const auto* name : {"MCPP_KEYMAP", "MC_KEYMAP"}) {
            if (const auto* environment = std::getenv(name); environment && *environment) {
                requested = environment;
                break;
            }
        }
    }
    const auto configuration = default_config_path().parent_path();
    const auto present = [](const std::filesystem::path& path) {
        std::error_code error;
        const auto status = std::filesystem::symlink_status(path, error);
        if (error == std::errc::no_such_file_or_directory) return false;
        if (error) throw std::filesystem::filesystem_error("inspect keymap", path, error);
        return std::filesystem::exists(status);
    };
    if (!requested.empty()) {
        const std::filesystem::path path(requested);
        if (config.keymap_override && present(path)) return Keymap::load(path);
        const std::array candidates{path, std::filesystem::path(requested + ".keymap")};
        if (!path.has_parent_path()) {
            for (const auto& directory : {configuration, std::filesystem::path("/etc/mcpp"),
                                           std::filesystem::path("/usr/share/mcpp")}) {
                for (const auto& candidate : candidates) {
                    if (present(directory / candidate)) return Keymap::load(directory / candidate);
                }
            }
        }
        for (const auto& candidate : candidates) if (present(candidate)) return Keymap::load(candidate);
        return Keymap::load(path);
    }
    for (const auto& path : {configuration / "mcpp.keymap",
                             configuration / "mc.keymap",
                             std::filesystem::path("/etc/mcpp/mcpp.keymap"),
                             std::filesystem::path("/etc/mcpp/mc.keymap"),
                             std::filesystem::path("/usr/share/mcpp/mcpp.keymap"),
                             std::filesystem::path("/usr/share/mcpp/mc.keymap")}) {
        if (present(path)) return Keymap::load(path);
    }
    return std::nullopt;
}

struct OriginalKeyBinding {
    KeymapContext context;
    std::string_view action;
    std::string_view keys;
};

std::span<const OriginalKeyBinding> original_keymap_bindings() {
    using enum KeymapContext;
    static constexpr OriginalKeyBinding bindings[]{
        {filemanager, "Help", "f1"}, {filemanager, "UserMenu", "f2"},
        {filemanager, "View", "f3"}, {filemanager, "Edit", "f4"},
        {filemanager, "Copy", "f5"}, {filemanager, "Move", "f6"},
        {filemanager, "MakeDir", "f7"}, {filemanager, "Delete", "f8"},
        {filemanager, "Menu", "f9"}, {filemanager, "Quit", "f10"},
        {filemanager, "ChangePanel", "tab"}, {filemanager, "Reread", "ctrl-r"},
        {filemanager, "Swap", "ctrl-u"}, {filemanager, "Shell", "ctrl-o"},
        {filemanager, "HotList", "ctrl-backslash"}, {filemanager, "Find", "alt-question"},
        {filemanager, "CdQuick", "alt-c"}, {filemanager, "ShowHidden", "alt-dot"},
        {filemanager, "ExtendedKeyMap", "ctrl-x"}, {filemanager, "History", "alt-h"},
        {filemanager, "SelectCodepage", "alt-e"}, {filemanager, "EditorViewerHistory", "alt-E"},
        {filemanager, "ViewFiltered", "alt-exclamation"}, {filemanager, "ScreenList", "alt-`"},
        {filemanager, "ScreenNext", "alt-rbrace"}, {filemanager, "ScreenPrev", "alt-lbrace"},
        {filemanager_xmap, "PanelQuickView", "q"}, {filemanager_xmap, "PanelInfo", "i"},
        {filemanager_xmap, "ChangeMode", "c"}, {filemanager_xmap, "Link", "l"},
        {filemanager_xmap, "LinkSymbolic", "s"}, {filemanager_xmap, "LinkSymbolicRelative", "v"},
        {filemanager_xmap, "ChangeOwn", "o"}, {filemanager_xmap, "CompareDirs", "d"},
        {filemanager_xmap, "CompareFiles", "ctrl-d"}, {filemanager_xmap, "LinkSymbolicEdit", "ctrl-s"},
        {filemanager_xmap, "ChangeAttributes", "e"}, {filemanager_xmap, "Jobs", "j"},
        {filemanager_xmap, "VfsList", "a"},
        {filemanager_xmap, "HotListAdd", "h"},
        {filemanager_xmap, "ExternalPanelize", "exclamation"},
        {panel, "Up", "up"}, {panel, "Down", "down"},
        {panel, "Left", "left"}, {panel, "Right", "right"},
        {panel, "PageUp", "pgup"}, {panel, "PageDown", "pgdn"},
        {panel, "Top", "home"}, {panel, "Bottom", "end"},
        {panel, "Enter", "enter"}, {panel, "CdParent", "ctrl-pgup"},
        {panel, "CdChild", "ctrl-pgdn"}, {panel, "HistoryPrev", "alt-y"},
        {panel, "HistoryNext", "alt-u"}, {panel, "Mark", "insert"},
        {panel, "MarkAll", "ctrl-a"}, {panel, "MarkUp", "shift-up"},
        {panel, "MarkDown", "shift-down"}, {panel, "Search", "ctrl-s;alt-s"},
        {panel, "Select", "plus"}, {panel, "Unselect", "minus"},
        {panel, "SelectInvert", "asterisk"},
        {tree, "Up", "up"}, {tree, "Down", "down"},
        {tree, "Left", "left"}, {tree, "Right", "right"},
        {tree, "PageUp", "pgup"}, {tree, "PageDown", "pgdn"},
        {tree, "Top", "home"}, {tree, "Bottom", "end"},
        {tree, "Enter", "enter"}, {tree, "Help", "f1"},
        {tree, "Reread", "f2;ctrl-r"}, {tree, "Forget", "f3"},
        {tree, "ToggleNavigation", "f4"}, {tree, "Copy", "f5"},
        {tree, "Move", "f6"}, {tree, "Remove", "f8"},
        {tree, "Search", "ctrl-s;alt-s"},
        {editor, "Help", "f1"}, {editor, "Save", "f2"},
        {editor, "Mark", "f3"}, {editor, "Replace", "f4"},
        {editor, "Copy", "f5"}, {editor, "Move", "f6"},
        {editor, "Search", "f7"}, {editor, "Delete", "f8"},
        {editor, "Menu", "f9"}, {editor, "Quit", "f10;esc"},
        {editor, "UserMenu", "f11"}, {editor, "SaveAs", "f12"},
        {editor, "MarkColumn", "f13;shift-f1"},
        {editor, "InsertFile", "f15"}, {editor, "SearchContinue", "f17;shift-f7"},
        {editor, "Up", "up"}, {editor, "Down", "down"},
        {editor, "Left", "left"}, {editor, "Right", "right"},
        {editor, "Home", "home"}, {editor, "End", "end"},
        {editor, "PageUp", "pgup"}, {editor, "PageDown", "pgdn"},
        {editor, "WordLeft", "ctrl-left;ctrl-z"}, {editor, "WordRight", "ctrl-right;ctrl-x"},
        {editor, "Top", "ctrl-home"}, {editor, "Bottom", "ctrl-end"},
        {editor, "TopOnScreen", "ctrl-pgup"}, {editor, "BottomOnScreen", "ctrl-pgdn"},
        {editor, "ScrollUp", "ctrl-up"}, {editor, "ScrollDown", "ctrl-down"},
        {editor, "MarkUp", "shift-up"}, {editor, "MarkDown", "shift-down"},
        {editor, "MarkLeft", "shift-left"}, {editor, "MarkRight", "shift-right"},
        {editor, "MarkToHome", "shift-home"}, {editor, "MarkToEnd", "shift-end"},
        {editor, "MarkPageUp", "shift-pgup"}, {editor, "MarkPageDown", "shift-pgdn"},
        {editor, "MarkToWordBegin", "ctrl-shift-left"}, {editor, "MarkToWordEnd", "ctrl-shift-right"},
        {editor, "MarkToFileBegin", "ctrl-shift-home"}, {editor, "MarkToFileEnd", "ctrl-shift-end"},
        {editor, "MarkToPageBegin", "ctrl-shift-pgup"}, {editor, "MarkToPageEnd", "ctrl-shift-pgdn"},
        {editor, "MarkScrollUp", "ctrl-shift-up"}, {editor, "MarkScrollDown", "ctrl-shift-down"},
        {editor, "MarkColumnUp", "alt-up"}, {editor, "MarkColumnDown", "alt-down"},
        {editor, "MarkColumnLeft", "alt-left"}, {editor, "MarkColumnRight", "alt-right"},
        {editor, "MarkColumnPageUp", "alt-pgup"}, {editor, "MarkColumnPageDown", "alt-pgdn"},
        {editor, "Store", "ctrl-insert"}, {editor, "Cut", "shift-delete"},
        {editor, "Paste", "shift-insert"}, {editor, "Undo", "ctrl-u"},
        {editor, "Redo", "alt-r"}, {editor, "EditNew", "ctrl-n"},
        {editor, "InsertOverwrite", "insert"}, {editor, "BackSpace", "backspace"},
        {editor, "Remove", "delete;ctrl-d"}, {editor, "DeleteLine", "ctrl-y"},
        {editor, "DeleteToEnd", "ctrl-k"}, {editor, "Enter", "enter"},
        {editor, "Return", "shift-enter;ctrl-enter;ctrl-shift-enter"},
        {editor, "Tab", "tab;shift-tab;ctrl-tab;ctrl-shift-tab"},
        {editor, "Goto", "alt-l"}, {editor, "History", "alt-E"},
        {editor, "SyntaxOnOff", "ctrl-s"}, {editor, "ShowNumbers", "alt-n"},
        {editor, "MatchBracket", "alt-b"}, {editor, "BlockSave", "ctrl-f"},
        {editor, "Bookmark", "alt-k"}, {editor, "BookmarkNext", "alt-j"},
        {editor, "BookmarkPrev", "alt-i"}, {editor, "BookmarkFlush", "alt-o"},
        {editor, "SelectCodepage", "alt-e"}, {editor, "Mail", "alt-m"},
        {editor, "SpellCheckCurrentWord", "ctrl-p"}, {editor, "MacroStartStopRecord", "ctrl-r"},
        {editor, "InsertLiteral", "ctrl-q"}, {editor, "ParagraphFormat", "alt-p"},
        {editor, "ExternalCommand", "alt-u"}, {editor, "ScreenList", "alt-`"},
        {editor, "ScreenNext", "alt-rbrace"}, {editor, "ScreenPrev", "alt-lbrace"},
        {viewer, "Quit", "f10;f3;esc;q"}, {viewer, "Help", "f1"},
        {viewer, "ScreenList", "alt-`"}, {viewer, "ScreenNext", "alt-rbrace"},
        {viewer, "ScreenPrev", "alt-lbrace"},
        {viewer, "WrapMode", "f2"}, {viewer, "HexMode", "f4"},
        {viewer, "Goto", "f5"}, {viewer, "Search", "f7"},
        {viewer, "SearchForward", "slash"}, {viewer, "SearchBackward", "question"},
        {viewer, "SearchContinue", "f17;shift-f7;n"},
        {viewer, "SearchOppositeContinue", "N"},
        {viewer, "SearchForwardContinue", "ctrl-s"}, {viewer, "SearchBackwardContinue", "ctrl-r"},
        {viewer, "MagicMode", "f8"}, {viewer, "NroffMode", "f9"},
        {viewer, "SelectCodepage", "alt-e"}, {viewer, "FileNext", "ctrl-f"},
        {viewer, "FilePrev", "ctrl-b"}, {viewer, "History", "alt-E"},
        {viewer, "Ruler", "alt-r"}, {viewer, "Bookmark", "m"},
        {viewer, "BookmarkGoto", "r"}, {viewer, "Up", "up;k;y;insert;ctrl-p"},
        {viewer, "Down", "down;j;e;delete;enter;ctrl-n"},
        {viewer, "Left", "left;h"}, {viewer, "Right", "right;l"},
        {viewer, "ScrollLeft", "ctrl-left"}, {viewer, "ScrollRight", "ctrl-right"},
        {viewer, "Top", "ctrl-home;ctrl-pgup;alt-lt;g;home"},
        {viewer, "Bottom", "ctrl-end;ctrl-pgdn;alt-gt;G;end"},
        {viewer, "PageUp", "pgup;alt-v;b;backspace"},
        {viewer, "PageDown", "pgdn;ctrl-v;f;space"},
        {viewer, "HalfPageUp", "u"}, {viewer, "HalfPageDown", "d"},
        {viewer_hex, "HexEditMode", "f2"}, {viewer_hex, "Save", "f6"},
        {viewer_hex, "Tab", "tab"}, {viewer_hex, "Home", "home;ctrl-a"},
        {viewer_hex, "End", "end;ctrl-e"}, {viewer_hex, "Left", "left;b"},
        {viewer_hex, "Right", "right;f"}, {viewer_hex, "PageUp", "pgup;alt-v"},
        {viewer_hex, "PageDown", "pgdn;ctrl-v"},
        {diffviewer, "Help", "f1"}, {diffviewer, "Save", "f2"},
        {diffviewer, "ScreenList", "alt-`"}, {diffviewer, "ScreenNext", "alt-rbrace"},
        {diffviewer, "ScreenPrev", "alt-lbrace"},
        {diffviewer, "Edit", "f4"}, {diffviewer, "EditOther", "f14"},
        {diffviewer, "Merge", "f5"}, {diffviewer, "MergeOther", "f15"},
        {diffviewer, "Search", "f7"}, {diffviewer, "SearchContinue", "f17"},
        {diffviewer, "Options", "f9"}, {diffviewer, "Quit", "f10;esc;q;Q"},
        {diffviewer, "Undo", "ctrl-z"}, {diffviewer, "Redo", "alt-r"},
        {diffviewer, "Reread", "ctrl-r"}, {diffviewer, "Swap", "ctrl-u"},
        {diffviewer, "SelectCodepage", "alt-e"}, {diffviewer, "HunkNext", "n;enter;space"},
        {diffviewer, "HunkPrev", "p;backspace"}, {diffviewer, "Up", "up"},
        {diffviewer, "Down", "down"}, {diffviewer, "Left", "left"},
        {diffviewer, "Right", "right"}, {diffviewer, "Top", "home;ctrl-home"},
        {diffviewer, "Bottom", "end;ctrl-end"}, {diffviewer, "PageUp", "pgup"},
        {diffviewer, "PageDown", "pgdn"}, {diffviewer, "SplitFull", "f"},
        {diffviewer, "SplitEqual", "equal"}, {diffviewer, "SplitLess", "lt"},
        {diffviewer, "SplitMore", "gt"}, {diffviewer, "ShowSymbols", "s;alt-s"},
        {diffviewer, "ShowNumbers", "l;alt-n"}, {diffviewer, "Goto", "g;G"},
        {diffviewer, "Tab2", "2"}, {diffviewer, "Tab3", "3"},
        {diffviewer, "Tab4", "4"}, {diffviewer, "Tab8", "8"},
        {input, "Home", "home;ctrl-a"}, {input, "End", "end;ctrl-e"},
        {input, "Left", "left;ctrl-b"}, {input, "Right", "right;ctrl-f"},
        {input, "WordLeft", "ctrl-left"}, {input, "WordRight", "ctrl-right"},
        {input, "MarkLeft", "shift-left"}, {input, "MarkRight", "shift-right"},
        {input, "MarkToHome", "shift-home"}, {input, "MarkToEnd", "shift-end"},
        {input, "MarkToWordBegin", "ctrl-shift-left"}, {input, "MarkToWordEnd", "ctrl-shift-right"},
        {input, "Clear", "ctrl-u"}, {input, "DeleteToEnd", "ctrl-k"},
        {input, "DeleteToWordBegin", "ctrl-w"}, {input, "DeleteToWordEnd", "ctrl-delete"},
        {input, "BackSpace", "backspace"}, {input, "Delete", "delete"},
        {input, "Yank", "ctrl-y;shift-insert"}, {input, "Store", "ctrl-insert"},
        {input, "Cut", "shift-delete"}, {input, "HistoryPrev", "alt-p;up"},
        {input, "HistoryNext", "alt-n;down"}, {input, "Complete", "alt-tab"},
        {dialog, "Help", "f1"}, {dialog, "Ok", "enter"}, {dialog, "Cancel", "esc"},
        {dialog, "Left", "left"}, {dialog, "Right", "right"},
        {dialog, "Up", "up"}, {dialog, "Down", "down"},
        {dialog, "Select", "space"}, {dialog, "Tab", "tab"},
        {menu, "Left", "left"}, {menu, "Right", "right"},
        {menu, "Up", "up"}, {menu, "Down", "down"},
        {menu, "Home", "home"}, {menu, "End", "end"},
        {menu, "Enter", "enter"}, {menu, "Quit", "esc"},
        {listbox, "Up", "up"}, {listbox, "Down", "down"},
        {listbox, "Top", "home"}, {listbox, "Bottom", "end"},
        {listbox, "PageUp", "pgup"}, {listbox, "PageDown", "pgdn"},
        {listbox, "Enter", "enter"}, {listbox, "Delete", "delete"},
        {radio, "Up", "up"}, {radio, "Down", "down"},
        {radio, "Select", "space"},
        {help, "Help", "f1"}, {help, "Index", "f2"}, {help, "Back", "f3;left"},
        {help, "Quit", "f10;esc"}, {help, "Up", "up"}, {help, "Down", "down"},
        {help, "PageUp", "pgup;b"}, {help, "PageDown", "pgdn;space"},
        {help, "Top", "home;ctrl-home"}, {help, "Bottom", "end;ctrl-end"},
        {help, "LinkNext", "tab"}, {help, "LinkPrev", "shift-tab;alt-tab"},
        {help, "Enter", "enter;right"},
        {chattr, "Up", "up"}, {chattr, "Down", "down"},
        {chattr, "PageUp", "pgup"}, {chattr, "PageDown", "pgdn"},
        {chattr, "Mark", "space"},
    };
    return bindings;
}

bool binding_has_key(std::string_view keys, const std::string_view key) {
    while (!keys.empty()) {
        const auto separator = keys.find(';');
        if (keys.substr(0, separator) == key) return true;
        if (separator == std::string_view::npos) break;
        keys.remove_prefix(separator + 1U);
    }
    return false;
}

struct MacroKey {
    bool character;
    std::string input;
};

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

ftxui::Element native_button(
    const std::string& label, const bool primary, const bool focused, ftxui::Box& box
) {
    auto element = ftxui::text(primary ? "[< " + label + " >]" : "[ " + label + " ]");
    if (focused) element = element | ftxui::inverted | ftxui::focus;
    return element | ftxui::reflect(box);
}

ftxui::Element native_window(const std::string& title, ftxui::Element body, const int width) {
    using namespace ftxui;
    const auto border_width = std::min(width, std::max(1, Terminal::Size().dimx - 2));
    auto framed = dbox({
        std::move(body) | borderStyled(LIGHT),
        vbox({hbox({filler(), text(" " + title + " ") | bold, filler()}), filler()}),
    }) | size(WIDTH, EQUAL, border_width);
    return vbox({
        text(" "),
        hbox({text(" "), std::move(framed), text(" ")}),
        text(" "),
    }) | size(WIDTH, EQUAL, border_width + 2);
}

class DialogShadow final : public ftxui::Node {
public:
    DialogShadow(ftxui::Element child, ftxui::Color color) : Node({std::move(child)}), color_(color) {}
    void SetBox(ftxui::Box box) override {
        Node::SetBox(box);
        children_.front()->SetBox(box);
    }
    void Render(ftxui::Screen& screen) override {
        for (int row = box_.y_min + 1; row <= box_.y_max + 1; ++row) {
            for (int column = box_.x_min + 2; column <= box_.x_max + 2; ++column) {
                auto& cell = screen.CellAt(column, row);
                cell.character = " ";
                cell.foreground_color = color_;
                cell.background_color = color_;
                cell.automerge = false;
            }
        }
        Node::Render(screen);
    }
private:
    ftxui::Color color_;
};

class SkinPaint final : public ftxui::Node {
public:
    SkinPaint(ftxui::Element child, SkinPair colors) : Node({std::move(child)}), colors_(colors) {}
    void SetBox(ftxui::Box box) override {
        Node::SetBox(box);
        children_.front()->SetBox(box);
    }
    void Render(ftxui::Screen& screen) override {
        for (int row = box_.y_min; row <= box_.y_max; ++row) {
            for (int column = box_.x_min; column <= box_.x_max; ++column) {
                auto& cell = screen.CellAt(column, row);
                cell.foreground_color = colors_.foreground;
                cell.background_color = colors_.background;
                cell.bold = (colors_.attributes & SkinPair::bold) != 0U;
                cell.italic = (colors_.attributes & SkinPair::italic) != 0U;
                cell.underlined = (colors_.attributes & SkinPair::underline) != 0U;
                cell.inverted = (colors_.attributes & SkinPair::reverse) != 0U;
                cell.blink = (colors_.attributes & SkinPair::blink) != 0U;
                cell.dim = false;
                cell.underlined_double = false;
                cell.strikethrough = false;
            }
        }
        Node::Render(screen);
    }
private:
    SkinPair colors_;
};

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

ftxui::Element mnemonic_label(
    const std::string& label,
    const std::size_t index,
    const std::optional<ftxui::Color> foreground = std::nullopt
) {
    using namespace ftxui;
    if (index >= label.size()) {
        return text(label);
    }
    auto mnemonic = text(label.substr(index, 1U)) | underlined | bold;
    if (foreground) mnemonic = mnemonic | color(*foreground);
    return hbox({
        text(label.substr(0, index)),
        std::move(mnemonic),
        text(label.substr(index + 1U)),
    });
}

const std::array<std::string, 5> top_menu_names{
    "  Left     ", "File     ", "Command     ", "Options     ", "Right "
};
constexpr std::array<std::size_t, 5> top_menu_mnemonics{2, 0, 0, 0, 0};
const std::array<std::string, 7> editor_menu_names{
    "File     ", "Edit     ", "Search     ", "Command     ",
    "Format     ", "Window     ", "Options"
};
constexpr std::array<std::size_t, 7> editor_menu_mnemonics{0, 0, 0, 0, 3, 0, 0};

ftxui::Element render_viewer_text(
    const std::string& row,
    const std::optional<std::pair<std::size_t, std::size_t>> highlight,
    const std::vector<ViewerTextStyle>& styles = {}
) {
    using namespace ftxui;
    if (!highlight && styles.empty()) {
        return text(row);
    }
    auto aligned = highlight;
    std::size_t offset = 0;
    for (const auto& glyph : Utf8ToGlyphs(row)) {
        const auto next = offset + glyph.size();
        if (highlight && offset <= highlight->first && highlight->first < next) {
            aligned->first = offset;
        }
        if (highlight && offset < highlight->second && highlight->second <= next) {
            aligned->second = next;
            break;
        }
        offset = next;
    }
    std::vector<std::size_t> boundaries{0, row.size()};
    if (aligned) {
        boundaries.push_back(std::min(aligned->first, row.size()));
        boundaries.push_back(std::min(aligned->second, row.size()));
    }
    for (const auto& style : styles) {
        boundaries.push_back(std::min(style.begin, row.size()));
        boundaries.push_back(std::min(style.end, row.size()));
    }
    std::ranges::sort(boundaries);
    Elements segments;
    for (std::size_t index = 1; index < boundaries.size(); ++index) {
        const auto begin = boundaries[index - 1U], end = boundaries[index];
        if (end <= begin) continue;
        auto segment = text(row.substr(begin, end - begin));
        if (aligned && begin >= aligned->first && begin < aligned->second) segment = segment | inverted | bold;
        for (const auto& style : styles) {
            if (begin < style.begin || begin >= style.end) continue;
            if (style.bold) segment = segment | bold;
            if (style.underline) segment = segment | underlined;
        }
        segments.push_back(std::move(segment));
    }
    return hbox(std::move(segments));
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
    char mnemonic{0};
};

ftxui::Element menu_row_label(const MenuRow& row, const std::optional<ftxui::Color> foreground = std::nullopt) {
    std::size_t index = std::string::npos;
    if (row.mnemonic != 0) {
        for (std::size_t offset = 0; offset < row.label.size(); ++offset) {
            if (std::tolower(static_cast<unsigned char>(row.label[offset])) == row.mnemonic) {
                index = offset;
                break;
            }
        }
    }
    return ftxui::hbox({ftxui::text(" "), mnemonic_label(row.label, index, foreground)});
}

struct TreeNode {
    std::string path;
    std::size_t depth{0};
    bool expanded{false};
    bool loaded{false};
    bool inaccessible{false};
    bool has_children{false};
    bool loading{false};
};

struct TreeSnapshot {
    std::vector<TreeNode> nodes;
    std::size_t cursor;
    bool searching;
    std::string search;
    bool dynamic;
};

enum class ViewerGotoMode { line, percent, decimal, hexadecimal };
enum class FileSearchTarget { viewer, quick_view, diff };

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
    std::string history_id;
    std::vector<std::string> history;
    std::size_t history_index{0};
    std::string draft;
    std::size_t draft_cursor{0};
    std::optional<std::size_t> draft_anchor;
    std::vector<std::string> completions;
    CompletionTarget completion_target;
    std::string completion_warning;
    std::string completed_value;
    std::string message;

    void reset(const std::string_view value, const bool select_initial = false) {
        cursor = value.size();
        anchor = select_initial && !value.empty()
                     ? std::optional<std::size_t>(0U)
                     : std::nullopt;
        dragging = false;
        history_index = history.size();
        draft = value;
        draft_cursor = cursor;
        draft_anchor = anchor;
        completions.clear();
        message.clear();
    }
};

struct ScreenTarget {
    Mode mode{Mode::panels};
    std::uint64_t id{0};
};

struct ViewerSessionState {
    std::uint64_t viewer_screen_id{0};
    ViewerModel viewer;
    std::shared_ptr<TemporaryFile> viewer_temporary_file;
    std::string viewer_display_name;
    std::string viewer_search_needle;
    std::string viewer_search_pattern;
    bool viewer_search_hex{false};
    EditorSearchOptions viewer_search_options;
    bool viewer_search_all_charsets{false};
    ProviderPtr viewer_navigation_provider;
    std::vector<std::string> viewer_navigation_files;
    std::optional<std::string> viewer_switch_pending;
    bool viewer_switch_preserve_list{false};
    unsigned int viewer_ruler{0};
    std::string viewer_message_title{"Viewer"};
    std::string viewer_message;
    Mode viewer_return_mode{Mode::panels};
    ScreenTarget viewer_return_screen;
    bool viewer_save_closes{false};
    bool viewer_quit_after_close{false};
    std::size_t viewer_choice{0};
    std::array<ftxui::Box, 3> viewer_choice_boxes{};
    std::array<ftxui::Box, 2> viewer_wrap_boxes{};
    bool viewer_search_quick{false};
    std::size_t viewer_hex_first_column{0};
    ViewerGotoMode viewer_goto_mode{ViewerGotoMode::line};
    bool viewer_goto_quick{false};
    std::vector<ViewerRow> viewer_rows;
    std::string viewer_rendered_path;
};

struct DiffSessionState {
    std::uint64_t diff_screen_id{0};
    std::array<EditorBuffer, 2> diff_files;
    DiffModel file_diff;
    DiffOptions diff_options;
    DiffOptions diff_draft;
    std::size_t diff_top{0};
    std::size_t diff_hunk{0};
    std::size_t diff_horizontal{0};
    std::size_t diff_order{0};
    std::size_t diff_tab_size{8};
    int diff_split{0};
    bool diff_full{false};
    bool diff_saving{false};
    bool diff_closing{false};
    bool diff_quit_after_close{false};
    bool diff_show_symbols{true};
    bool diff_show_numbers{false};
    ScreenTarget diff_return_screen;
    std::size_t diff_save_index{0};
    std::optional<std::size_t> diff_edit_side;
    std::optional<std::size_t> diff_last_merged_side;
    std::array<ftxui::Box, 2> diff_pane_boxes{};
    std::size_t diff_options_focus{0};
    std::array<ftxui::Box, 10> diff_options_boxes{};
    std::string diff_search_query;
    EditorSearchOptions diff_search_options;
    bool diff_search_all_charsets{false};
    std::optional<std::pair<std::size_t, std::size_t>> diff_search_cursor;
    std::optional<std::pair<std::size_t, std::pair<std::size_t, std::size_t>>> diff_search_match;
};

}  // namespace

struct App::Impl : ViewerSessionState, DiffSessionState {
    Config config;
    struct PaletteSet {
        Skin panels;
        Skin editor;
        Skin viewer;
        Skin help;
        Skin diff;
    };
    enum class PaletteContext { panels, editor, viewer, help, diff };
    std::shared_ptr<const PaletteSet> active_palette;
    std::shared_ptr<const PaletteSet> appearance_original_palette;
    PaletteContext palette_context{PaletteContext::panels};
    InputHistories input_histories{default_config_path().parent_path() / "input-history"};
    std::optional<Keymap> keymap;
    std::optional<KeymapContext> keymap_prefix;
    Mode keymap_prefix_mode{Mode::panels};
    bool keymap_dispatching{false};
    std::optional<HelpBrowser> help_browser;
    std::string help_message;
    std::vector<ftxui::Box> help_row_boxes;
    std::vector<std::optional<std::size_t>> help_row_links;
    std::array<ftxui::Box, 4> help_key_boxes{};
    ProviderPtr local;
    ArchiveCache archives;
    Location left_location;
    Location right_location;
    Pane left;
    Pane right;
    int active_index{0};
    std::array<int, 2> pane_ids{0, 1};
    std::array<std::vector<Pane>, 2> archive_parents;
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
    std::size_t listing_choice{0};
    std::size_t listing_focus{0};
    std::string listing_brief_value;
    std::string listing_user_value;
    bool listing_user_mini{false};
    std::array<std::string, 4> listing_status_values;
    TextInputState listing_brief_input;
    TextInputState listing_user_input;
    TextInputState listing_status_input;
    std::array<ftxui::Box, 10> listing_boxes{};
    ftxui::Box listing_viewport;
    std::string listing_error;
    std::size_t sort_choice{1};
    std::size_t sort_focus{0};
    bool sort_executable_first{false};
    bool sort_case_sensitive{true};
    bool sort_reverse{false};
    std::array<ftxui::Box, 14> sort_boxes{};
    ftxui::Box sort_viewport;
    std::array<bool, 6> confirmation_values{};
    std::size_t confirmation_focus{0};
    std::array<ftxui::Box, 8> confirmation_boxes{};
    bool layout_horizontal{false};
    bool layout_equal{true};
    int layout_left_size{0};
    int layout_top_size{0};
    std::array<bool, 6> layout_visible{};
    std::size_t layout_focus{0};
    std::array<ftxui::Box, 13> layout_boxes{};
    std::array<bool, 16> panel_option_values{};
    std::size_t panel_option_search{2};
    std::size_t panel_option_focus{0};
    std::array<ftxui::Box, 21> panel_option_boxes{};
    bool fast_reload_warning_shown{false};
    std::vector<gid_t> effective_groups;
    std::string terminal_title;
    bool terminal_title_pushed{false};
    std::string appearance_skin;
    std::string appearance_original_skin;
    std::optional<std::string> appearance_original_override;
    bool appearance_shadows{true};
    bool appearance_original_shadows{true};
    std::size_t appearance_focus{0};
    std::array<ftxui::Box, 4> appearance_boxes{};
    std::size_t display_bits_focus{0};
    std::array<ftxui::Box, 3> display_bits_boxes{};
    std::array<bool, 18> configure_values{};
    std::size_t configure_pause{1};
    std::string configure_timeout;
    TextInputState configure_timeout_input;
    std::size_t configure_focus{0};
    std::array<ftxui::Box, 24> configure_boxes{};
    std::string configure_error;
    bool escape_waiting{false};
    std::size_t panelize_cursor{0};
    std::size_t panelize_focus{0};
    std::string panelize_command;
    std::string panelize_error;
    TextInputState panelize_input;
    std::vector<ftxui::Box> panelize_rows;
    ftxui::Box panelize_viewport;
    ftxui::Box panelize_command_box;
    std::array<ftxui::Box, 4> panelize_buttons{};
    std::vector<Entry> attribute_entries;
    std::size_t attribute_index{0};
    unsigned int attribute_original{0};
    unsigned int attribute_value{0};
    std::size_t attribute_focus{0};
    std::array<ftxui::Box, 14> attribute_boxes{};
    std::vector<ftxui::Box> attribute_buttons;
    ftxui::Box attribute_viewport;
    std::string attribute_error;
    std::vector<Entry> ownership_entries;
    std::size_t ownership_index{0};
    std::size_t ownership_focus{0};
    std::size_t ownership_bit{0};
    std::array<char, 11> ownership_flags{};
    std::string ownership_user;
    std::string ownership_group;
    std::array<ftxui::Box, 5> ownership_boxes{};
    std::vector<ftxui::Box> ownership_buttons;
    std::string ownership_error;
    bool exit_after_diff{false};
    std::uint64_t next_screen_id{1};
    std::vector<ViewerSessionState> inactive_viewers;
    std::vector<DiffSessionState> inactive_diffs;
    std::array<std::string, 3> mail_values;
    std::array<TextInputState, 3> mail_inputs;
    std::array<ftxui::Box, 5> mail_boxes{};
    std::size_t mail_focus{0};
    std::string mail_error;
    std::vector<SpellingIssue> spelling_issues;
    std::size_t spelling_index{0};
    std::size_t spelling_suggestion{0};
    std::size_t spelling_focus{0};
    bool spelling_whole_file{false};
    std::string spelling_error;
    std::optional<std::pair<EditorPosition, EditorPosition>> spelling_match;
    std::vector<ftxui::Box> spelling_suggestion_boxes;
    ftxui::Box spelling_list_box;
    std::array<ftxui::Box, 4> spelling_button_boxes{};
    std::string native_input_title;
    std::string native_input_label;
    std::string native_input_value;
    std::string native_input_error;
    std::string native_input_directory;
    TextInputState native_input_state;
    std::size_t native_input_focus{0};
    std::array<ftxui::Box, 3> native_input_boxes{};
    Mode native_input_return{Mode::panels};
    std::function<void(const std::string&)> native_input_apply;
    std::string key_query_title;
    std::string key_query_label;
    bool key_query_cancel{true};
    Mode key_query_return{Mode::editor};
    std::function<void(const ftxui::Event&)> key_query_apply;
    ftxui::Box key_query_cancel_box;
    std::string native_message_title;
    std::string native_message_text;
    Mode native_message_return{Mode::panels};
    ftxui::Box native_message_button;
    std::string native_query_title;
    std::string native_query_text;
    std::vector<std::string> native_query_labels;
    std::vector<ftxui::Box> native_query_boxes;
    std::size_t native_query_choice{0};
    Mode native_query_return{Mode::panels};
    std::function<void(std::size_t)> native_query_apply;
    std::string local_list_title;
    std::vector<std::string> local_list_items;
    std::vector<ftxui::Box> local_list_boxes;
    ftxui::Box local_list_box;
    std::size_t local_list_cursor{0};
    std::function<void(std::size_t)> local_list_activate;
    Mode local_list_return{Mode::panels};
    bool local_list_native{false};
    std::string local_list_hotkeys;
    bool local_list_completion{false};
    std::vector<std::string> completion_choices;
    std::string completion_filter;
    int local_history_kind{0};
    TextInputState* local_history_input{nullptr};
    std::string* local_history_value{nullptr};
    std::vector<std::pair<bool, std::string>> local_history_files;
    std::optional<std::string> local_directory;
    std::vector<std::string> local_directory_choices;
    std::jthread local_command_thread;
    Cancellation local_command_cancellation;
    bool local_command_running{false};
    Mode local_command_return{Mode::panels};
    std::string local_command_title;
    ftxui::Box local_command_cancel_box;
    Mode user_menu_return{Mode::panels};
    std::filesystem::path user_menu_source;
    std::size_t user_menu_cursor{0};
    std::string user_menu_error;
    std::vector<ftxui::Box> user_menu_boxes;
    ftxui::Box user_menu_viewport;
    struct UserMenuInvocation {
        UserMenuEntry entry;
        NativeMenuContext context;
        NativeMenuRequirements requirements;
        std::vector<std::string> answers;
        Mode return_mode{Mode::panels};
        int pane_index{0};
        std::string editor_path;
        std::uint64_t editor_change{0};
        EditorPosition editor_position;
        std::optional<std::pair<EditorPosition, EditorPosition>> editor_selection;
        bool column_selection{false};
        std::string original_block;
        std::shared_ptr<TemporaryFile> block;
        std::shared_ptr<TemporaryFile> errors;
        std::optional<NativeMenuExpansion> running;
        std::optional<int> completed_status;
    };
    std::shared_ptr<UserMenuInvocation> user_menu_pending;
    Mode learn_key_return{Mode::panels};
    std::string learn_key_action;
    std::size_t learn_key_focus{0};
    std::array<bool, 38> learn_key_detected{};
    std::array<ftxui::Box, 40> learn_key_boxes{};
    ftxui::Box learn_keys_viewport;
    std::map<std::string, std::string> learn_keys_draft;
    bool macro_recording{false};
    bool macro_replaying{false};
    std::vector<MacroKey> macro_keys;
    std::vector<MacroKey> macro_playback;
    std::size_t macro_play_index{0};
    std::size_t macro_repeats{0};
    std::vector<EditorLocation> declaration_history;
    std::size_t declaration_cursor{0};
    std::array<ftxui::Box, 10> operation_dialog_boxes{};
    ftxui::Box operation_dialog_viewport;
    std::string status{"Ready"};
    std::optional<std::string> pending_operation_notice;
    OperationEngine operations;
    std::unique_ptr<ViewerStdin> viewer_stdin;
    ftxui::Box message_button_box;
    bool exit_after_viewer{false};
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
    std::vector<std::pair<std::string, std::string>> syntax_rules;
    std::optional<std::string> syntax_buffer_path;
    bool syntax_registry_ready{false};
    std::string syntax_load_error;
    std::string syntax_error_notice;
    ExtensionRules extension_rules;
    struct ExtensionInvocation {
        ExtensionCommand entry;
        ExtensionAction action{ExtensionAction::open};
        NativeMenuContext context;
        NativeMenuRequirements requirements;
        std::vector<std::string> answers;
        ProviderPtr provider;
        ProviderPtr other_provider;
        std::string path;
        Mode return_mode{Mode::panels};
        int pane_id{0};
        bool confirmed{false};
        std::optional<NativeMenuExpansion> prepared;
        std::optional<NativeMenuExpansion> running;
        std::optional<int> completed_status;
    };
    std::shared_ptr<ExtensionInvocation> extension_pending;
    FileHighlightRules highlight_rules;
    std::vector<ftxui::Box> editor_line_boxes;
    ftxui::Box editor_header_box;
    int editor_window_action{0};
    std::array<int, 4> editor_window_previous{};
    bool editor_window_mouse_drag{false};
    int editor_window_mouse_offset{0};
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
    std::string editor_dialog_error;
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
    std::size_t save_mode_choice{1};
    std::size_t save_mode_focus{0};
    std::string save_mode_extension;
    TextInputState save_mode_extension_input;
    bool save_mode_check_newline{false};
    std::array<ftxui::Box, 7> save_mode_boxes{};
    std::size_t newline_warning_choice{0};
    std::array<ftxui::Box, 2> newline_warning_boxes{};
    bool newline_warning_accepted{false};
    bool newline_warning_preserve_links{false};
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
    ProviderPtr editor_save_provider;
    EditorLock editor_save_lock;
    EditorBuffer* editor_unnamed_save_buffer{nullptr};
    std::optional<EditorSavePurpose> editor_save_as_continuation;
    std::string editor_save_path;
    std::string editor_overwrite_revision;
    bool editor_save_overwrite{false};
    bool editor_quit_requested{false};
    std::size_t editor_quit_index{0};
    std::size_t editor_options_focus{0};
    std::string editor_options_tab_width;
    std::string editor_options_wrap_length;
    std::size_t editor_options_wrap{0};
    std::array<bool, 13> editor_options_values{};
    TextInputState editor_tab_input;
    TextInputState editor_wrap_input;
    std::array<ftxui::Box, 20> editor_options_boxes{};
    ftxui::Box editor_options_viewport;
    std::string editor_options_error;
    bool editor_shift_marking{false};
    bool editor_column_marking{false};
    std::string editor_message_title{"Editor error"};
    std::string editor_error_message;
    ftxui::App* terminal{nullptr};
    std::string command_line;
    ShellSession shell;
    bool shell_foreground{false};
    bool shell_exit_requested{false};
    std::optional<int> shell_panel_id;
    std::string shell_expected_directory;
    bool command_prompt_override{false};
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
    ftxui::Box search_panelize_box;
    ftxui::Box search_list_box;
    std::size_t bookmark_cursor{0};
    std::vector<ftxui::Box> bookmark_boxes;
    ftxui::Box bookmark_list_box;
    DirectoryHotlist hotlist;
    HotlistPath hotlist_group;
    std::optional<HotlistPath> hotlist_move_source;
    HotlistPath hotlist_move_origin;
    std::size_t hotlist_move_cursor{0};
    std::size_t hotlist_focus{0};
    std::array<ftxui::Box, 8> hotlist_buttons{};
    std::string hotlist_error;
    bool hotlist_form_open{false};
    bool hotlist_form_group{false};
    std::string hotlist_label;
    std::string hotlist_directory;
    TextInputState hotlist_label_input;
    TextInputState hotlist_directory_input;
    std::size_t hotlist_form_focus{0};
    std::array<ftxui::Box, 5> hotlist_form_boxes{};
    std::string hotlist_form_error;
    std::array<ftxui::Box, 10> function_key_boxes{};
    std::array<bool, 10> function_key_enabled{};
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
    bool modal_tree_search_pending{false};
    bool tree_dynamic{false};
    std::optional<TreeSnapshot> modal_tree_snapshot;
    int modal_tree_source{0};
    std::vector<ftxui::Box> modal_tree_rows;
    std::vector<std::string> modal_tree_paths;
    ftxui::Box modal_tree_viewport;
    std::set<std::string> known_tree_directories;
    bool tree_cache_used{false};
    ViewerModel quick_viewer;
    std::string quick_view_search_needle;
    std::string quick_view_search_pattern;
    bool quick_view_search_hex{false};
    EditorSearchOptions quick_view_search_options;
    bool quick_view_search_all_charsets{false};
    FileSearchTarget file_search_target{FileSearchTarget::viewer};
    std::string file_search_value;
    EditorSearchOptions file_search_draft;
    bool file_search_all_charsets{false};
    std::size_t file_search_focus{0};
    TextInputState file_search_input;
    std::array<ftxui::Box, 11> file_search_boxes{};
    std::string file_search_error;
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
    std::optional<std::string> dialog_destination_mask;
    std::string operation_input_error;
    bool dialog_preserve_attributes{true};
    bool dialog_preserve_symlinks{true};
    bool dialog_shell_patterns{true};
    bool dialog_stable_symlinks{false};
    bool dialog_dive_into_subdirectories{false};
    bool dialog_background{false};
    std::size_t dialog_focus{2};
    std::array<ftxui::Box, 2> delete_button_boxes{};
    std::size_t compare_choice{0};
    std::array<ftxui::Box, 4> compare_boxes{};
    std::optional<Entry> operation_prompt_source;
    std::optional<Entry> operation_prompt_destination;
    CollisionOptions operation_prompt_collision_options;
    std::string operation_prompt_error;
    Mode operation_return_mode{Mode::panels};
    std::size_t operation_return_focus{0};
    std::array<ftxui::Box, 8> operation_choice_boxes{};
    std::vector<std::shared_ptr<OperationJob>> jobs;
    std::shared_ptr<OperationJob> operation_draft;
    std::shared_ptr<OperationJob> progress_job;
    std::shared_ptr<OperationJob> prompt_job;
    std::vector<std::pair<std::shared_ptr<OperationJob>, std::uint64_t>> operation_questions;
    std::uint64_t operation_prompt_sequence{0};
    Mode progress_return_mode{Mode::panels};
    std::array<ftxui::Box, 4> operation_progress_boxes{};
    std::size_t operation_progress_focus{0};
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
    std::uint64_t tree_load_id{0};
    std::optional<std::string> tree_refresh_pending;
    bool tree_refresh_notify_pending{false};
    bool tree_surroundings_pending{false};
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
          keymap(load_configured_keymap(config)),
          local(std::make_shared<LocalProvider>()),
          archives(local, std::chrono::seconds(static_cast<std::chrono::seconds::rep>(config.vfs_timeout))),
          left_location(open_location(left_path, local, &archives)),
          right_location(open_location(right_path, local, &archives)),
          left(left_location.provider, left_location.path, nullptr,
               dynamic_cast<const ArchiveProvider*>(left_location.provider.get()) != nullptr),
          right(right_location.provider, right_location.path, nullptr,
                dynamic_cast<const ArchiveProvider*>(right_location.provider.get()) != nullptr) {
        for (const auto& diagnostic : config.diagnostics) std::cerr << "mcpp: configuration: " << diagnostic << '\n';
        initialize_palette();
        active_index = config.current_is_left.value_or(true) ? 0 : 1;
        viewer.set_wrap(config.viewer_wrap);
        quick_viewer.set_wrap(config.viewer_wrap);
        diff_tab_size = config.diff_tab_size;
        try {
            known_tree_directories = load_tree_cache(tree_cache_path());
        } catch (const std::exception& error) {
            std::cerr << "mcpp: directory Tree cache: " << error.what() << '\n';
            set_error(error);
        }
        const auto group_count = getgroups(0, nullptr);
        if (group_count < 0) throw std::system_error(errno, std::generic_category(), "read user groups");
        effective_groups.resize(static_cast<std::size_t>(group_count));
        if (group_count > 0 && getgroups(group_count, effective_groups.data()) < 0) {
            throw std::system_error(errno, std::generic_category(), "read user groups");
        }
        for (const auto& [key, action] : config.learned_keys) {
            static_cast<void>(decode_hex_query(key));
            static_cast<void>(named_event(action));
        }
        command_history = config.command_history;
        command_input.history = command_history;
        initialize_input_histories();
        try {
            reload_local_rules();
        } catch (const std::exception& error) {
            set_error(error);
        }
        left.set_show_hidden(config.show_hidden);
        right.set_show_hidden(config.show_hidden);
        left.set_directories_first(config.directories_first);
        right.set_directories_first(config.directories_first);
        left.set_sort_options(config.panel_sort[0].value_or(config.sort),
                              config.panel_descending[0].value_or(config.sort_descending),
                              config.panel_case_sensitive[0].value_or(true),
                              config.panel_executable_first[0].value_or(false));
        right.set_sort_options(config.panel_sort[1].value_or(config.sort),
                               config.panel_descending[1].value_or(config.sort_descending),
                               config.panel_case_sensitive[1].value_or(true),
                               config.panel_executable_first[1].value_or(false));
        left.set_reverse_files_only(config.reverse_files_only);
        right.set_reverse_files_only(config.reverse_files_only);
        left.set_show_hidden(config.panel_hidden[0].value_or(config.show_hidden));
        right.set_show_hidden(config.panel_hidden[1].value_or(config.show_hidden));
        left.set_directories_first(config.panel_directories_first[0].value_or(config.directories_first));
        right.set_directories_first(config.panel_directories_first[1].value_or(config.directories_first));
        left.set_show_backups(config.show_backups);
        right.set_show_backups(config.show_backups);
        left.set_fast_reload(config.fast_reload);
        right.set_fast_reload(config.fast_reload);
        left.set_navigation(config.scroll_pages, config.scroll_center);
        right.set_navigation(config.scroll_pages, config.scroll_center);
        left.set_shell_patterns(config.shell_patterns);
        right.set_shell_patterns(config.shell_patterns);
        left.set_follow_links(config.cd_follows_links);
        right.set_follow_links(config.cd_follows_links);
        set_terminal_input_delay(config.single_escape ? static_cast<std::uint64_t>(config.escape_timeout) : 50000U);
        if (auto* provider = dynamic_cast<LocalProvider*>(local.get())) provider->set_preallocate(config.preallocate_space);
        for (std::size_t index = 0; index < 2U; ++index) {
            if (config.panel_listing[index] == ListingFormat::custom) {
                static_cast<void>(parse_listing_format(config.panel_columns[index]));
            }
        }
        editor_syntax = config.editor_syntax;
        editor_line_numbers = config.editor_line_numbers;
        editor.set_tab_width(config.editor_tab_width);
        editor_indent_width = config.editor_indent_width;
        editor_auto_indent = config.editor_auto_indent;
        editor_show_whitespace = config.editor_visible_tabs || config.editor_visible_trailing_spaces;
        editor_history = config.editor_history;
        initialize_archive_parent(left);
        initialize_archive_parent(right);
        left_location = {};
        right_location = {};
    }

    Pane& active() {
        return active_index == 0 ? left : right;
    }

    Pane& passive() {
        return active_index == 0 ? right : left;
    }

    Location resolve_location(const std::string_view value) {
        std::string path(value);
        if (is_archive_location(value)) {
            const auto tar = value.rfind("/utar://");
            const auto cpio = value.rfind("/ucpio://");
            const auto separator = tar == std::string_view::npos ? cpio
                : cpio == std::string_view::npos ? tar : std::max(tar, cpio);
            const auto archive = std::filesystem::path(value.substr(0, separator));
            if (archive.is_relative()) {
                path = std::filesystem::absolute(archive).lexically_normal().string() +
                       std::string(value.substr(separator));
            }
        } else if (value.find("://") == std::string_view::npos &&
                   std::filesystem::path(value).is_relative()) {
            path = std::filesystem::absolute(std::filesystem::path(value)).lexically_normal().string();
        }
        return open_location(path, local, &archives);
    }

    std::string pane_location(const Pane& pane) const {
        return archives.format(pane.provider(), pane.path());
    }

    std::string short_archive_location(const std::string& value) const {
        const auto tar = value.rfind("/utar://");
        const auto cpio = value.rfind("/ucpio://");
        if (tar == std::string::npos && cpio == std::string::npos) return value;
        const auto separator = tar == std::string::npos ? cpio
            : cpio == std::string::npos ? tar : std::max(tar, cpio);
        return local->basename(value.substr(0, separator)) + value.substr(separator);
    }

    std::size_t pane_identity(const Pane& pane) const {
        if (&pane == &left) return static_cast<std::size_t>(pane_ids[0]);
        if (&pane == &right) return static_cast<std::size_t>(pane_ids[1]);
        throw std::logic_error("pane does not belong to this application");
    }

    std::string archive_file_path(const ProviderPtr& provider) const {
        const auto* archive = dynamic_cast<const ArchiveProvider*>(provider.get());
        if (archive == nullptr) throw std::invalid_argument("location is not an archive");
        const auto location = archives.format(provider, "/");
        const auto separator = location.rfind(archive->format() == ArchiveFormat::tar ? "/utar://" : "/ucpio://");
        if (separator == std::string::npos) throw std::logic_error("archive location has no VFS delimiter");
        return location.substr(0, separator);
    }

    Pane make_archive_parent(const Pane& pane) {
        const auto path = archive_file_path(pane.provider());
        Pane parent(local, local->parent(path), &pane);
        const auto found = std::ranges::find(parent.entries(), path, &Entry::path);
        if (found != parent.entries().end()) {
            parent.move_cursor(static_cast<int>(found - parent.entries().begin()));
        }
        return parent;
    }

    void initialize_archive_parent(Pane& pane) {
        if (dynamic_cast<const ArchiveProvider*>(pane.provider().get()) != nullptr) {
            archive_parents[pane_identity(pane)].push_back(make_archive_parent(pane));
        }
    }

    std::string pane_execution_directory(const Pane& pane) const {
        if (dynamic_cast<const ArchiveProvider*>(pane.provider().get()) != nullptr) {
            return local->parent(archive_file_path(pane.provider()));
        }
        return pane.navigation_path();
    }

    void install_pane_location(Pane& pane, Location location) {
        if (pane.provider() == location.provider) {
            pane.change_directory(std::move(location.path));
            return;
        }
        const bool archive = dynamic_cast<const ArchiveProvider*>(location.provider.get()) != nullptr;
        Pane next(location.provider, std::move(location.path), &pane, archive);
        std::vector<Pane> parents;
        if (archive) {
            const auto container = archive_file_path(next.provider());
            if (pane.provider() == local && pane.path() == local->parent(container)) {
                parents.push_back(std::move(next));
                pane.swap_contents(parents.back());
            } else {
                parents.push_back(make_archive_parent(next));
                pane.swap_contents(next);
            }
        } else {
            pane.swap_contents(next);
        }
        archive_parents[pane_identity(pane)] = std::move(parents);
    }

    bool panel_up(Pane& pane) {
        if (dynamic_cast<const ArchiveProvider*>(pane.provider().get()) == nullptr ||
            pane.provider()->parent(pane.path()) != pane.path()) return pane.up();
        auto& parents = archive_parents[pane_identity(pane)];
        if (parents.empty()) parents.push_back(make_archive_parent(pane));
        pane.swap_contents(parents.back());
        try {
            pane.refresh({}, true);
        } catch (...) {
            pane.swap_contents(parents.back());
            throw;
        }
        parents.pop_back();
        return true;
    }

    bool enter_panel_entry(const bool open_archive = true) {
        const auto* selected = active().current();
        if (selected == nullptr) return false;
        if (selected->revision == "mcpp:parent" &&
            active().provider()->parent(active().path()) == active().path()) {
            return panel_up(active());
        }
        if (active().enter()) return true;
        if (!open_archive) return false;
        if (!can_open_file(*selected) || !is_archive_filename(selected->name)) return false;
        if (active().provider() != local) throw std::runtime_error("nested archives are not supported yet");
        const auto path = selected->path;
        const auto directory = active().path();
        const auto id = pane_ids[active_index];
        auto opened = std::make_shared<ProviderPtr>();
        start_local_task("Reading archive", [this, path, opened](const Cancellation& cancellation) {
            *opened = archives.open(path, cancellation);
            return CommandOutput{};
        }, [this, opened, directory, id](CommandOutput) {
            auto& pane = pane_ids[0] == id ? left : right;
            if (pane.provider() != local || pane.path() != directory) {
                throw std::runtime_error("pane changed while the archive was being opened");
            }
            install_pane_location(pane, {*opened, "/"});
            status = "Read-only archive: " + pane_location(pane);
        });
        return true;
    }

    Location resolve_pane_location(const Pane& pane, std::string value) {
        return resolve_relative_location({pane.provider(), pane.navigation_path()}, std::move(value));
    }

    Location resolve_relative_location(const Location& base, std::string value) {
        Location destination;
        if (value.find("://") != std::string::npos) {
            if (!std::filesystem::path(value).is_absolute()) {
                if (base.provider != local) throw std::runtime_error("nested archives are not supported yet");
                const auto separator = value.find("://");
                const auto slash = value.rfind('/', separator);
                if (slash == std::string::npos) throw std::invalid_argument("VFS path has no archive name");
                value = local->join(base.path, value.substr(0, slash)) + value.substr(slash);
            }
            destination = resolve_location(value);
        } else if (std::filesystem::path(value).is_absolute()) {
            destination = {local, local->normalize(value)};
        } else {
            destination = base;
            for (const auto& component : std::filesystem::path(value)) {
                const auto name = component.generic_string();
                if (name.empty() || name == ".") continue;
                if (name == "..") {
                    if (dynamic_cast<const ArchiveProvider*>(destination.provider.get()) != nullptr &&
                        destination.provider->parent(destination.path) == destination.path) {
                        destination = {local, local->parent(archive_file_path(destination.provider))};
                    } else destination.path = destination.provider->parent(destination.path);
                } else destination.path = destination.provider->join(destination.path, name);
            }
        }
        return destination;
    }

    void change_panel_directory(Pane& pane, const std::string& value) {
        auto destination = resolve_pane_location(pane, value);
        const auto entry = destination.provider->stat_follow(destination.path, {});
        if (entry.kind == EntryKind::file && is_archive_filename(entry.name)) {
            if (destination.provider != local) throw std::runtime_error("nested archives are not supported yet");
            destination = {archives.open(destination.path), "/"};
        } else if (entry.kind != EntryKind::directory) {
            throw std::runtime_error("not a directory: " + value);
        }
        install_pane_location(pane, std::move(destination));
    }

    void show_vfs_list() {
        auto locations = archives.locations();
        if (locations.empty()) {
            show_native_message("Active VFS list", "No virtual filesystems are open.");
            return;
        }
        auto activate = [this, locations](const std::size_t index) {
            change_panel_directory(active(), locations.at(index));
        };
        for (auto& location : locations) location = short_archive_location(location);
        show_local_list("Active VFS list", std::move(locations), std::move(activate));
        local_list_native = true;
    }

    void show_vfs_options() {
        show_native_input("Virtual File System Setting", "Timeout for freeing VFSs (sec):",
            std::to_string(config.vfs_timeout), [this](const std::string& value) {
                std::size_t timeout = 0;
                const auto parsed = std::from_chars(value.data(), value.data() + value.size(), timeout);
                if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || timeout > 86400U) {
                    throw std::invalid_argument("VFS timeout must be between 0 and 86400 seconds");
                }
                archives.set_timeout(std::chrono::seconds(static_cast<std::chrono::seconds::rep>(timeout)));
                config.vfs_timeout = timeout;
            }, Mode::panels, {}, "settings.vfs-timeout");
    }

    bool command_prompt_shown() const { return config.command_prompt || command_prompt_override; }

    int file_manager_body_height() const {
        const auto reserved = static_cast<int>(config.menubar_visible) +
                              static_cast<int>(command_prompt_shown()) +
                              static_cast<int>(config.keybar_visible) +
                              static_cast<int>(config.hintbar_visible);
        return std::max(1, ftxui::Terminal::Size().dimy - reserved);
    }

    int split_top_height() const {
        const auto height = file_manager_body_height();
        const auto minimum = std::min(6, std::max(1, height / 2));
        const auto wanted = config.layout_horizontal_equal.value_or(config.equal_split) || config.panel_top_size == 0
                                ? height / 2 : config.panel_top_size;
        return std::clamp(wanted, minimum, std::max(minimum, height - minimum));
    }

    std::size_t panel_visible_rows(const int index = -1) const {
        auto height = file_manager_body_height();
        const auto physical = index < 0 ? active_index : index;
        if (config.layout_horizontal) {
            height = physical == 0 ? split_top_height() : height - split_top_height();
        }
        const bool auxiliary = (tree_panel && *tree_panel == physical) ||
                               (quick_view_panel && *quick_view_panel == physical) ||
                               (information_panel && *information_panel == physical);
        return static_cast<std::size_t>(std::max(1, height - (config.show_mini_status || auxiliary ? 5 : 3)));
    }

    int split_left_width() const {
        const auto width = std::max(2, ftxui::Terminal::Size().dimx);
        return std::clamp(
            config.layout_vertical_equal.value_or(config.equal_split) || config.panel_left_size == 0 ? width / 2 : config.panel_left_size,
            1, width - 1
        );
    }

    bool listing_full_width(const int index) const {
        if ((tree_panel && *tree_panel == index) || (quick_view_panel && *quick_view_panel == index) ||
            (information_panel && *information_panel == index)) return false;
        return pane_listing_spec(index == 0 ? left : right).full_width;
    }

    int panel_width(const int index) const {
        const auto width = std::max(2, ftxui::Terminal::Size().dimx);
        if (config.layout_horizontal || listing_full_width(index)) return width;
        const auto left_width = split_left_width();
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
            panel_visible_rows(quick_view_panel ? *quick_view_panel : 1 - active_index) + 2U,
        };
    }

    std::pair<std::size_t, std::size_t>
    viewer_dimensions() const {
        const auto size = ftxui::Terminal::Size();
        return {
            static_cast<std::size_t>(std::max(1, size.dimx)),
            static_cast<std::size_t>(std::max(1, size.dimy - 2 - (viewer_ruler == 0U ? 0 : 1))),
        };
    }

    std::array<int, 4> editor_window_bounds() const {
        const auto terminal_size = ftxui::Terminal::Size();
        const auto columns = std::max(1, terminal_size.dimx);
        const auto rows = std::max(1, terminal_size.dimy);
        const auto width = editor.fullscreen ? columns : std::clamp(editor.window_width, 1, columns);
        const auto height = editor.fullscreen ? rows : std::clamp(editor.window_height, 1, std::max(1, rows - 2));
        return {editor.fullscreen ? 0 : std::clamp(editor.window_x, 0, columns - width),
                editor.fullscreen ? 0 : std::clamp(editor.window_y, 1, std::max(1, rows - height - 1)), width, height};
    }

    std::pair<int, int> editor_dimensions() const {
        const auto bounds = editor_window_bounds();
        return {std::max(1, bounds[2] - (editor.fullscreen ? 0 : 2)),
                std::max(1, bounds[3] - (editor.fullscreen ? 0 : 2))};
    }

    std::size_t editor_visible_rows() const {
        return static_cast<std::size_t>(
            std::max(1, editor_dimensions().second - (editor.fullscreen ? 2 : 0))
        );
    }

    std::size_t editor_page_rows() const {
        return std::max<std::size_t>(
            1U,
            editor_visible_rows() - 1U
        );
    }

    ftxui::Element render_function_key(
        const std::size_t index,
        const std::string& label,
        const bool enabled = true,
        const int width = 0
    ) {
        using namespace ftxui;
        const auto available = std::max(10, (width == 0 ? Terminal::Size().dimx : width) - 1);
        const auto key_width =
            available / 10 +
            (static_cast<int>(index) < available % 10 ? 1 : 0);
        const auto number = std::to_string(index + 1U);
        const auto label_width = static_cast<std::size_t>(std::max(
            0, key_width - static_cast<int>(number.size())
        ));
        function_key_enabled[index] = enabled && !label.empty();
        auto description = text(truncate_text(label, label_width)) |
                           color(enabled ? palette().key_label.foreground : palette().dim) |
                           bgcolor(palette().key_label.background);
        if (!enabled) {
            description = description | dim;
        }
        return hbox({
                   text(number) |
                       color(enabled ? palette().key_number.foreground : palette().dim) |
                       bgcolor(palette().key_number.background),
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
            1, editor_dimensions().first - (editor_line_numbers ? 6 : 0)
        ));
        const auto column = editor.display_cursor_column();
        if (column < editor.first_column) {
            editor.first_column = column;
        } else if (column - editor.first_column >= width) {
            editor.first_column = column - width + 1U;
        }
    }

    std::string editor_directory(const EditorBuffer& buffer) const {
        const auto& pane = active_index == 0 ? left : right;
        if (dynamic_cast<const ArchiveProvider*>(buffer.provider().get()) != nullptr) {
            return local->parent(archive_file_path(buffer.provider()));
        }
        if (!buffer.path().empty()) return local->parent(buffer.path());
        return buffer.base_directory.empty() ? pane.navigation_path() : buffer.base_directory;
    }

    std::string editor_location(const EditorBuffer& buffer) const {
        return buffer.path().empty() ? std::string{} : archives.format(buffer.provider(), buffer.path());
    }

    Location resolve_editor_location(
        const std::string& value, const EditorBuffer* source = nullptr
    ) {
        const auto& buffer = source == nullptr ? editor : *source;
        if (value.find("://") != std::string::npos || std::filesystem::path(value).is_absolute()) {
            return resolve_location(value);
        }
        if (buffer.open() && !buffer.path().empty() && buffer.provider() != local) {
            const auto provider = buffer.provider();
            const auto path = (std::filesystem::path(provider->parent(buffer.path())) / value)
                                  .lexically_normal().generic_string();
            return {provider, provider->normalize(path)};
        }
        return {local, local->join(editor_directory(buffer), value)};
    }

    std::string editor_relative_path(const std::string& path) const {
        return local->join(editor_directory(editor), path);
    }

    std::string read_local_text(const std::string& path) const {
        EditorModel input;
        input.open(local, path);
        return input.text();
    }

    void write_local_text(
        const std::string& path, const std::string& value,
        const bool replace, const std::size_t linked_choice
    ) {
        std::istringstream input(value);
        write_local_stream(path, input, value.size(), replace, linked_choice);
    }

    void write_local_stream(
        const std::string& path, std::istream& input, const std::uint64_t length,
        const bool replace, const std::size_t linked_choice,
        const Cancellation& cancellation = {}
    ) {
        cancellation.throw_if_requested();
        auto storage = path;
        std::optional<std::string> expected;
        const bool exists = local->exists(path);
        if (exists) {
            if (!replace) throw std::runtime_error("destination exists; choose Replace explicitly");
            const auto entry = local->stat_follow(path, {});
            if (entry.kind != EntryKind::file) throw std::runtime_error("destination is not a regular file");
            storage = entry.path;
            const auto* provider = dynamic_cast<const LocalProvider*>(local.get());
            if (provider == nullptr) throw std::runtime_error("local file writer is unavailable");
            provider->ensure_writable(storage);
            if (provider->hardlink_count(storage) > 1U && linked_choice == 0U) {
                throw std::runtime_error("linked destination: choose Detach or Preserve links");
            }
            expected = entry.revision;
        }
        if (exists && linked_choice == 2U) {
            auto* provider = dynamic_cast<LocalProvider*>(local.get());
            if (provider == nullptr) throw std::runtime_error("linked file writer is unavailable");
            provider->write_in_place(storage, input, length, expected, cancellation);
        } else if (!exists) {
            auto* provider = dynamic_cast<LocalProvider*>(local.get());
            if (provider == nullptr) throw std::runtime_error("local file writer is unavailable");
            provider->write_with_permissions(storage, input, length, false, cancellation, {},
                std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
        } else {
            local->write(storage, input, length, replace, expected, cancellation, {});
        }
    }

    void show_insert_file() {
        show_native_input("Insert file", "Enter file name:", editor_clipboard_path().string(),
            [this](const std::string& file) {
            if (file.empty()) throw std::runtime_error("file name is empty");
            const EditorPosition before{editor.row(), editor.column()};
            const auto path = editor_relative_path(file);
            if (local->normalize(path) == local->normalize(editor_clipboard_path().string())) {
                bool column = false;
                const auto value = load_editor_clipboard(editor_clipboard_path(), &column);
                if (column) editor.paste_column(value);
                else editor.paste(value);
            } else editor.paste_bytes(read_local_text(path));
            if (!config.editor_cursor_after_inserted_block) editor.set_position(before);
            status = "File inserted";
        }, Mode::editor, editor_directory(editor), "filename.insert");
    }

    void show_save_block() {
        if (!editor.selected_text()) {
            show_editor_message("Save block", "No marked block to save");
            return;
        }
        show_native_input("Save block", "Enter file name:", editor_clipboard_path().string(),
            [this](const std::string& file) {
            if (file.empty()) throw std::runtime_error("file name is empty");
            const auto value = editor.selected_bytes();
            if (!value) throw std::runtime_error("the marked block is no longer available");
            const auto path = editor_relative_path(file);
            if (local->normalize(path) == local->normalize(editor_clipboard_path().string())) {
                const auto text = editor.selected_text();
                if (!text) throw std::runtime_error("the marked block is no longer available");
                save_editor_clipboard(editor_clipboard_path(), *text, editor.column_selection());
                status = "Marked block saved";
                return;
            }
            const bool exists = local->exists(path);
            const auto expected = exists ? std::optional<std::string>{local->stat_follow(path, {}).revision}
                                         : std::nullopt;
            const auto save_block = [this, path, value, exists, expected](const std::size_t policy) {
                if (expected && local->stat_follow(path, {}).revision != *expected) {
                    throw std::runtime_error("block destination changed while awaiting confirmation");
                }
                write_local_text(path, *value, exists, policy);
                status = "Marked block saved";
            };
            const auto continue_save = [this, path, save_block, exists](const std::size_t accepted) {
                if (accepted != 0U) return;
                if (exists && std::filesystem::hard_link_count(path) > 1U) {
                    show_native_query("File has multiple hardlinks",
                        display_text(path) + "\nDetach replaces this name atomically. Preserve links writes in place.",
                        {"Detach", "Preserve links", "Cancel"},
                        [save_block](const std::size_t choice) {
                            if (choice < 2U) save_block(choice == 0U ? 1U : 2U);
                        }, Mode::editor);
                } else save_block(0);
            };
            if (exists && config.confirm_overwrite) {
                show_native_query("File exists", "Overwrite " + display_text(path) + "?",
                                  {"Overwrite", "Cancel"}, continue_save, Mode::editor);
            } else continue_save(0);
        }, Mode::editor, editor_directory(editor), "filename.save-block");
    }

    std::string user_menu_syntax() const {
        return syntax_highlighter.language_name(editor.path(), editor.lines(), effective_syntax_language());
    }

    NativeMenuContext user_menu_context(const Mode return_mode, const std::filesystem::path& source) const {
        const auto snapshot = [this](const Pane& pane) {
            NativeMenuPane result;
            result.directory = pane_location(pane);
            result.local = pane.provider() == local;
            if (pane.current() != nullptr) result.file = *pane.current();
            for (const auto& entry : pane.entries()) {
                if (pane.is_selected(entry)) result.tagged.push_back(entry);
            }
            if (result.local) {
                const auto qualify = [this, &pane](Entry& entry) {
                    if (local->parent(entry.path) != pane.path()) entry.name = entry.path;
                };
                if (result.file) qualify(*result.file);
                for (auto& entry : result.tagged) qualify(entry);
            }
            return result;
        };
        NativeMenuContext result;
        result.active = snapshot(active_index == 0 ? left : right);
        result.other = snapshot(active_index == 0 ? right : left);
        result.editor = return_mode == Mode::editor;
        result.menu_file = source.string();
        if (result.editor) {
            result.active.local = editor.provider() == local;
            result.active.directory = result.active.local ? editor_directory(editor)
                : archives.format(editor.provider(), editor.provider()->parent(editor.path()));
            Entry file;
            if (!editor.path().empty() && editor.provider()->exists(editor.path())) {
                file = editor.provider()->stat(editor.path(), {});
            } else {
                file.kind = EntryKind::file;
                file.mode = S_IFREG | 0600U;
            }
            file.path = editor.path();
            file.name = editor.path().empty() ? std::string{} : archives.format(editor.provider(), editor.path());
            result.active.file = std::move(file);
            if (result.active.directory != (active_index == 0 ? left.path() : right.path())) {
                for (auto& tagged : result.active.tagged) tagged.name = tagged.path;
            }
            result.column = editor.display_cursor_column();
            result.syntax = user_menu_syntax();
        }
        const auto directory = result.active.directory;
        result.executable = [directory](const std::string_view name) {
            auto path = std::filesystem::path(name);
            if (path.is_relative()) path = std::filesystem::path(directory) / path;
            struct stat metadata {};
            return ::stat(path.c_str(), &metadata) == 0 && S_ISREG(metadata.st_mode) &&
                   access(path.c_str(), X_OK) == 0;
        };
        return result;
    }

    void open_user_menu(
        const Mode return_mode, const std::filesystem::path& menu_path = {}
    ) {
        user_menu_return = return_mode;
        user_menu_error.clear();
        user_menu_cursor = 0;
        mode = Mode::user_menu;
        try {
            const auto directory = active().provider() == local ? std::filesystem::path(active().path())
                                                               : std::filesystem::path{};
            user_menu_source = menu_path.empty() ? find_user_menu_path(directory, return_mode == Mode::editor)
                                                 : menu_path;
            auto selected = select_native_menu(load_user_menu(user_menu_source, config.shell_patterns),
                                               user_menu_context(return_mode, user_menu_source));
            user_menu = std::move(selected.entries);
            user_menu_cursor = selected.selected;
            if (!selected.trace.empty()) {
                std::string trace;
                for (const auto& line : selected.trace) trace += line + '\n';
                show_native_message("User menu conditions", trace, Mode::user_menu);
            }
        } catch (const std::exception& error) {
            user_menu.clear();
            user_menu_error = error.what();
        }
    }

    std::string read_user_menu_output(const std::filesystem::path& path) const {
        const auto entry = local->stat(path.string(), {});
        if (entry.kind != EntryKind::file || entry.size > 16U * 1024U * 1024U) {
            throw std::runtime_error("menu output must be a regular file no larger than 16 MiB: " + path.string());
        }
        std::ostringstream output;
        local->read(path.string(), 0, entry.size, output, {});
        return output.str();
    }

    void set_user_menu_view_options(const NativeMenuExpansion& expansion) {
        if (expansion.view_hex && viewer.hex() != *expansion.view_hex) viewer.toggle_hex();
        if (expansion.view_nroff && viewer.nroff() != *expansion.view_nroff) viewer.toggle_nroff();
        viewer_hex_first_column = 0;
    }

    void complete_user_menu(
        const std::shared_ptr<UserMenuInvocation>& invocation,
        const NativeMenuExpansion& expansion,
        std::function<void()> after = {}
    ) {
        const auto replacement = invocation->block
            ? std::optional<std::string>{read_user_menu_output(invocation->block->path())} : std::nullopt;
        const auto diagnostics = invocation->errors ? read_user_menu_output(invocation->errors->path()) : std::string{};
        auto apply = [this, invocation, expansion, replacement, diagnostics, after = std::move(after)] {
            if (replacement && *replacement != invocation->original_block) {
                if (!editor.open() || editor.path() != invocation->editor_path ||
                    editor.change_id() != invocation->editor_change ||
                    editor.selection() != invocation->editor_selection ||
                    editor.column_selection() != invocation->column_selection) {
                    throw std::runtime_error("editor buffer or marked block changed while the menu command was running");
                }
                if (invocation->editor_selection) editor.replace_block(*replacement);
                else {
                    editor.set_position(invocation->editor_position);
                    editor.paste(*replacement);
                }
            }
            auto& first = invocation->pane_index == 0 ? left : right;
            auto& second = invocation->pane_index == 0 ? right : left;
            first.unmark_paths(expansion.untag_active);
            second.unmark_paths(expansion.untag_other);
            if (mode == Mode::user_menu) mode = invocation->return_mode;
            status = invocation->entry.label + " completed";
            if (after) after();
            if (!diagnostics.empty()) show_command_warnings(diagnostics);
            invocation->block.reset();
            invocation->errors.reset();
            if (user_menu_pending == invocation) user_menu_pending.reset();
            return true;
        };
        if (replacement && *replacement != invocation->original_block) {
            static_cast<void>(run_editor_action(std::move(apply)));
        } else static_cast<void>(apply());
    }

    void complete_pending_user_menu() {
        if (!user_menu_pending || !user_menu_pending->running || !user_menu_pending->completed_status ||
            (mode != Mode::panels && mode != Mode::editor && mode != Mode::user_menu)) return;
        const auto invocation = user_menu_pending;
        const auto expansion = *invocation->running;
        const auto result = *invocation->completed_status;
        invocation->running.reset();
        invocation->completed_status.reset();
        try {
            if (result != 0) throw std::runtime_error(invocation->entry.label + " exited " + std::to_string(result));
            complete_user_menu(invocation, expansion);
        } catch (const std::exception& error) {
            user_menu_error = error.what();
            if (mode != Mode::user_menu) show_native_message("User menu", user_menu_error, mode);
            user_menu_pending.reset();
        }
    }

    void continue_user_menu(const std::shared_ptr<UserMenuInvocation>& invocation) {
        if (invocation->answers.size() < invocation->requirements.prompts.size()) {
            const auto index = invocation->answers.size();
            show_native_input("User menu", invocation->requirements.prompts[index], "",
                [this, invocation, index](const std::string& answer) {
                    invocation->answers.resize(index + 1U);
                    invocation->answers[index] = answer;
                    continue_user_menu(invocation);
                }, Mode::user_menu, invocation->context.active.local ? invocation->context.active.directory : "");
            return;
        }
        if (user_menu_pending && user_menu_pending->running) {
            throw std::runtime_error("a user-menu command is still running; use Ctrl-O to resume it");
        }
        if (invocation->requirements.block_file) {
            invocation->block = std::make_shared<TemporaryFile>(invocation->original_block);
            invocation->context.block_file = invocation->block->path().string();
        }
        if (invocation->requirements.error_file) {
            invocation->errors = std::make_shared<TemporaryFile>("");
            invocation->context.error_file = invocation->errors->path().string();
        }
        auto expansion = invocation->entry.native
            ? expand_native_menu(invocation->entry.command, invocation->context, invocation->answers)
            : NativeMenuExpansion{};
        if (!invocation->entry.native) {
            if (!invocation->context.active.local) {
                throw std::runtime_error("copy archive members to a local directory before running file commands");
            }
            const auto selected_path = invocation->context.active.file
                ? invocation->context.active.file->path : std::string{};
            expansion.command = expand_user_menu_command(invocation->entry.command, selected_path,
                invocation->context.active.directory, invocation->context.other.directory);
        }
        if (expansion.action == NativeMenuAction::change_directory) {
            const auto words = plain_command_words(expansion.command);
            if (!words || words->size() != 1U) {
                throw std::invalid_argument("%cd needs one directory argument, not shell commands");
            }
            auto destination = words->front();
            if (destination.empty()) throw std::invalid_argument("%cd directory is empty");
            auto& previous = command_previous_directories[static_cast<std::size_t>(pane_ids[active_index])];
            if (destination == "-") {
                if (previous.empty()) throw std::runtime_error("%cd: no previous directory");
                destination = previous;
            } else if (destination == "~" || destination.starts_with("~/")) {
                const auto* home = std::getenv("HOME");
                if (home == nullptr || *home == '\0') throw std::runtime_error("%cd: HOME is not set");
                destination = std::string(home) + destination.substr(1U);
            } else if (destination.starts_with('~')) {
                throw std::invalid_argument("%cd: named home directories are unavailable; use an explicit path");
            } else if (invocation->context.editor) {
                const auto location = resolve_editor_location(destination);
                destination = archives.format(location.provider, location.path);
            }
            const auto old = pane_location(active());
            change_panel_directory(active(), destination);
            previous = old;
            complete_user_menu(invocation, expansion);
            status = "Directory changed: " + pane_location(active());
            return;
        }
        if (expansion.action == NativeMenuAction::view) {
            if (expansion.command.empty()) {
                const auto provider = invocation->context.editor ? editor.provider() : active().provider();
                const auto path = invocation->context.editor ? editor.path()
                    : invocation->context.active.file ? invocation->context.active.file->path : std::string{};
                if (path.empty() || !can_open_file(provider->stat_follow(path, {}))) {
                    throw std::runtime_error("%view needs a selected regular file or a filter command");
                }
                complete_user_menu(invocation, expansion, [this, invocation, expansion, provider, path] {
                    load_viewer(provider, path);
                    set_viewer_return_mode(invocation->return_mode);
                    mode = Mode::viewer;
                    set_user_menu_view_options(expansion);
                });
            } else {
                const auto command = expansion.command;
                const auto directory = invocation->context.active.directory;
                start_local_task(invocation->entry.label,
                    [command, directory](const Cancellation& cancellation) {
                        return capture_shell(command, directory, cancellation);
                    }, [this, invocation, expansion](CommandOutput result) {
                        require_command_success(result);
                        complete_user_menu(invocation, expansion, [this, invocation, expansion, result] {
                            open_output_viewer(invocation->entry.label, result.output);
                            set_viewer_return_mode(invocation->return_mode);
                            set_user_menu_view_options(expansion);
                            show_command_warnings(result.error);
                        });
                    }, Mode::user_menu);
            }
            return;
        }
        user_menu_pending = invocation;
        const auto result = run_process(expansion.command, false, invocation->context.active.directory);
        if (shell.busy()) {
            invocation->running = std::move(expansion);
            mode = invocation->return_mode;
            status = invocation->entry.label + " is running; Ctrl-O resumes it";
            return;
        }
        if (result != 0) throw std::runtime_error(invocation->entry.label + " exited " + std::to_string(result));
        complete_user_menu(invocation, expansion);
    }

    void execute_user_menu_entry(const UserMenuEntry& entry) try {
        auto invocation = std::make_shared<UserMenuInvocation>();
        invocation->entry = entry;
        invocation->context = user_menu_context(user_menu_return, user_menu_source);
        invocation->return_mode = user_menu_return;
        invocation->pane_index = active_index;
        if (entry.native) invocation->requirements = native_menu_requirements(entry.command, invocation->context.editor);
        if (invocation->context.editor) {
            invocation->editor_path = editor.path();
            invocation->editor_change = editor.change_id();
            invocation->editor_position = {editor.row(), editor.column()};
            invocation->editor_selection = editor.selection();
            invocation->column_selection = editor.column_selection();
            if (invocation->requirements.block_file) invocation->original_block = editor.selected_text().value_or("");
        }
        continue_user_menu(invocation);
    } catch (const std::exception& error) {
        user_menu_error = (entry.source.empty() ? std::string{} :
            entry.source + ":" + std::to_string(entry.line) + ": ") + error.what();
        mode = Mode::user_menu;
    }

    void show_editor_encoding() {
        show_codepage_chooser(editor.encoding(), Mode::editor, [this](const std::string& codepage) {
            editor.set_encoding(codepage);
            spelling_issues.clear();
            spelling_match.reset();
            reset_editor_view_state();
            update_editor_view();
            status = "Editor codepage changed";
        });
    }

    void show_editor_mail() {
        for (std::size_t index = 0; index < mail_inputs.size(); ++index) mail_inputs[index].reset(mail_values[index], true);
        mail_focus = 0;
        mail_error.clear();
        mode = Mode::editor_mail_dialog;
    }

    ftxui::Element render_editor_mail() {
        using namespace ftxui;
        Elements rows{text(" mail -s <subject> -c <cc> <to>")};
        static const std::array<std::string, 3> names{"To", "Subject", "Copies to"};
        for (std::size_t index = 0; index < names.size(); ++index) {
            rows.push_back(text(" " + names[index]));
            rows.push_back(hbox({text(" "), render_input(mail_values[index], mail_inputs[index], mail_focus == index, "") |
                                 reflect(mail_boxes[index]), text(" ")}));
        }
        if (!mail_error.empty()) rows.push_back(paragraph(display_text(mail_error)) | bold);
        rows.push_back(separator());
        rows.push_back(hbox({
            native_button("OK", true, mail_focus == 3U, mail_boxes[3]), text(" "),
            native_button("Cancel", false, mail_focus == 4U, mail_boxes[4])
        }) | center);
        return native_window("Mail", vbox(std::move(rows)), 48);
    }

    void send_editor_mail() try {
        for (const auto& field : mail_values) {
            if (std::ranges::any_of(field, [](const unsigned char character) { return std::iscntrl(character) != 0; })) {
                throw std::invalid_argument("mail fields cannot contain control characters");
            }
        }
        const auto recipients = plain_command_words(mail_values[0]);
        const auto copies = plain_command_words(mail_values[2]);
        if (!recipients || recipients->empty() || !copies) throw std::invalid_argument("enter valid mail recipients");
        const auto check_address = [](const std::string& address) {
            if (address.empty() || address.front() == '-') throw std::invalid_argument("mail addresses cannot be command options");
        };
        std::vector<std::string> arguments{"mail", "-s", mail_values[1]};
        if (!copies->empty()) {
            std::string list;
            for (const auto& address : *copies) {
                check_address(address);
                if (!list.empty()) list += ',';
                list += address;
            }
            arguments.insert(arguments.end(), {"-c", list});
        }
        arguments.push_back("--");
        for (const auto& address : *recipients) { check_address(address); arguments.push_back(address); }
        for (std::size_t index = 0; index < mail_values.size(); ++index) remember_input(mail_inputs[index], mail_values[index]);
        auto input = std::make_shared<TemporaryFile>(editor.text());
        const auto directory = editor_directory(editor);
        start_local_task("Mail", [input, arguments, directory](const Cancellation& cancellation) {
            return capture_command(arguments, directory, cancellation, 1024U * 1024U, input->path());
        }, [this, input](CommandOutput result) {
            require_command_success(result);
            mode = Mode::editor;
            show_native_message("Mail", "Mail command completed." + (result.error.empty() ? std::string{} : "\n" + result.error),
                                Mode::editor);
        }, Mode::editor_mail_dialog);
    } catch (const std::exception& error) {
        mail_error = error.what();
    }

    bool handle_editor_mail(const ftxui::Event& event) {
        using namespace ftxui;
        if (modal_cancel(event)) { mode = Mode::editor; return true; }
        if (event.is_mouse()) {
            auto copy = event;
            const auto& mouse = copy.mouse();
            for (std::size_t index = 0; index < mail_inputs.size(); ++index) {
                if (mail_inputs[index].dragging) return input_mouse(mail_values[index], mail_inputs[index], mouse);
            }
            if (mouse.button != Mouse::Left || mouse.motion != Mouse::Pressed) return true;
            for (std::size_t index = 0; index < mail_boxes.size(); ++index) {
                if (!mail_boxes[index].Contain(mouse.x, mouse.y)) continue;
                mail_focus = index;
                if (index < 3U) input_mouse(mail_values[index], mail_inputs[index], mouse);
                else if (index == 3U) send_editor_mail();
                else mode = Mode::editor;
                return true;
            }
        } else if (event == Event::Tab || event == Event::ArrowDown) mail_focus = (mail_focus + 1U) % 5U;
        else if (event == Event::TabReverse || event == Event::ArrowUp) mail_focus = (mail_focus + 4U) % 5U;
        else if (event == Event::Return || (event == Event::Character(' ') && mail_focus >= 3U)) {
            if (mail_focus == 4U) mode = Mode::editor;
            else send_editor_mail();
        } else if (mail_focus < 3U) return edit_input(mail_values[mail_focus], mail_inputs[mail_focus], event);
        return true;
    }

    void show_editor_save_mode() {
        save_mode_choice = editor.save_mode() == EditorSaveMode::in_place ? 0U
                         : editor.save_mode() == EditorSaveMode::atomic ? 1U : 2U;
        save_mode_focus = 0;
        save_mode_extension = config.editor_backup_extension;
        save_mode_extension_input.reset(save_mode_extension, true);
        save_mode_check_newline = config.editor_check_newline;
        mode = Mode::editor_save_mode_dialog;
    }

    ftxui::Element render_editor_save_mode() {
        using namespace ftxui;
        static const std::array<std::string, 3> labels{
            "Quick save", "Safe save", "Do backups with following extension:"
        };
        Elements rows;
        for (std::size_t index = 0; index < labels.size(); ++index) {
            auto row = hbox({text(" "), text(save_mode_choice == index ? "(*) " : "( ) "),
                             mnemonic_label(labels[index], 0)}) | reflect(save_mode_boxes[index]);
            if (save_mode_focus == 0U && save_mode_choice == index) row = focus_dialog(std::move(row));
            rows.push_back(std::move(row));
        }
        auto extension = render_input(save_mode_extension, save_mode_extension_input, save_mode_focus == 1U, "");
        if (save_mode_choice != 2U) extension = extension | dim | color(palette().dim);
        rows.push_back(hbox({text(" "), std::move(extension) | reflect(save_mode_boxes[3]), text(" ")}));
        rows.push_back(separator());
        auto check = hbox({text(save_mode_check_newline ? " [x] " : " [ ] "),
                           mnemonic_label("Check POSIX new line", 6)}) | reflect(save_mode_boxes[4]);
        if (save_mode_focus == 2U) check = focus_dialog(std::move(check));
        rows.push_back(std::move(check));
        return native_window("Edit Save Mode", vbox({
            vbox(std::move(rows)),
            separator(),
            hbox({native_button("OK", true, save_mode_focus == 3U, save_mode_boxes[5]),
                  text(" "), native_button("Cancel", false, save_mode_focus == 4U, save_mode_boxes[6])}) | center,
        }), 44);
    }

    bool handle_editor_save_mode(const ftxui::Event& event) {
        using namespace ftxui;
        if (modal_cancel(event)) { mode = Mode::editor; return true; }
        if (event.is_mouse()) {
            auto copy = event;
            const auto& mouse = copy.mouse();
            if (save_mode_extension_input.dragging) return input_mouse(save_mode_extension, save_mode_extension_input, mouse);
            if (mouse.button != Mouse::Left || mouse.motion != Mouse::Pressed) return true;
            for (std::size_t index = 0; index < save_mode_boxes.size(); ++index) {
                if (!save_mode_boxes[index].Contain(mouse.x, mouse.y)) continue;
                if (index < 3U) { save_mode_choice = index; save_mode_focus = 0; }
                else if (index == 3U && save_mode_choice == 2U) {
                    save_mode_focus = 1;
                    input_mouse(save_mode_extension, save_mode_extension_input, mouse);
                } else if (index == 4U) { save_mode_focus = 2; save_mode_check_newline = !save_mode_check_newline; }
                else if (index == 5U) apply_editor_save_mode();
                else if (index == 6U) mode = Mode::editor;
                return true;
            }
            return true;
        }
        if (event == Event::Tab || event == Event::TabReverse) {
            do {
                save_mode_focus = event == Event::Tab ? (save_mode_focus + 1U) % 5U : (save_mode_focus + 4U) % 5U;
            } while (save_mode_focus == 1U && save_mode_choice != 2U);
        } else if (save_mode_focus == 0U && (event == Event::ArrowUp || event == Event::ArrowDown)) {
            save_mode_choice = event == Event::ArrowDown ? (save_mode_choice + 1U) % 3U : (save_mode_choice + 2U) % 3U;
        } else if (event == Event::Return || (event == Event::Character(' ') && save_mode_focus >= 3U)) {
            if (save_mode_focus == 4U) mode = Mode::editor;
            else apply_editor_save_mode();
        } else if (save_mode_focus == 1U) {
            return edit_input(save_mode_extension, save_mode_extension_input, event);
        } else if (save_mode_focus == 2U && event == Event::Character(' ')) {
            save_mode_check_newline = !save_mode_check_newline;
        } else if (event.is_character() && event.character().size() == 1U) {
            const auto key = static_cast<char>(std::tolower(static_cast<unsigned char>(event.character()[0])));
            if (key == 'q') { save_mode_choice = 0; save_mode_focus = 0; }
            else if (key == 's') { save_mode_choice = 1; save_mode_focus = 0; }
            else if (key == 'd') { save_mode_choice = 2; save_mode_focus = 0; }
            else if (key == 'p') { save_mode_check_newline = !save_mode_check_newline; save_mode_focus = 2; }
        }
        return true;
    }

    void apply_editor_save_mode() {
        const auto selected = save_mode_choice == 0U ? EditorSaveMode::in_place
                            : save_mode_choice == 1U ? EditorSaveMode::atomic : EditorSaveMode::backup;
        editor.set_backup_extension(save_mode_extension);
        editor.set_save_mode(selected);
        for (auto& buffer : editor_buffers) {
            buffer.set_backup_extension(save_mode_extension);
            buffer.set_save_mode(selected);
        }
        config.editor_backup_extension = save_mode_extension;
        config.editor_check_newline = save_mode_check_newline;
        config.editor_save_mode = selected == EditorSaveMode::atomic ? "atomic"
                                : selected == EditorSaveMode::in_place ? "in-place" : "backup";
        mode = Mode::editor;
        status = "Save mode changed";
    }

    ftxui::Element render_newline_warning() {
        using namespace ftxui;
        return native_window("Warning", vbox({
            text(" The file you are saving does not end with a newline. "),
            separator(),
            hbox({native_button("Continue", true, newline_warning_choice == 0U, newline_warning_boxes[0]),
                  text(" "), native_button("Cancel", false, newline_warning_choice == 1U, newline_warning_boxes[1])}) | center,
        }), 57);
    }

    bool handle_newline_warning(const ftxui::Event& event) {
        const auto input = handle_button_input(event, newline_warning_choice, newline_warning_boxes, "oc");
        if (input == ButtonInput::cancel || (input == ButtonInput::activate && newline_warning_choice == 1U)) {
            editor_quit_requested = false;
            mode = editor_save_purpose == EditorSavePurpose::save_as ? Mode::prompt : Mode::editor;
            editor_save_buffer = nullptr;
        } else if (input == ButtonInput::activate) {
            newline_warning_accepted = true;
            complete_editor_buffer_save(newline_warning_preserve_links);
        }
        return input != ButtonInput::ignored;
    }

    void show_editor_window(const int action) {
        if (action == 5) {
            editor_window_action = 0;
            if (editor.fullscreen && !editor.window_geometry_saved) {
                const auto dimensions = ftxui::Terminal::Size();
                editor.window_x = 0;
                editor.window_y = 1;
                editor.window_width = dimensions.dimx;
                editor.window_height = std::max(1, dimensions.dimy - 2);
                editor.window_geometry_saved = true;
            }
            editor.fullscreen = !editor.fullscreen;
            update_editor_view();
            return;
        }
        if (editor.fullscreen) return;
        editor_window_previous = editor_window_bounds();
        editor.window_x = editor_window_previous[0];
        editor.window_y = editor_window_previous[1];
        editor.window_width = editor_window_previous[2];
        editor.window_height = editor_window_previous[3];
        editor_window_action = action;
        editor_window_mouse_drag = false;
    }

    bool handle_editor_window(const ftxui::Event& event) {
        using namespace ftxui;
        const auto dimensions = Terminal::Size();
        const auto bounds = editor_window_bounds();
        editor.window_x = bounds[0]; editor.window_y = bounds[1];
        editor.window_width = bounds[2]; editor.window_height = bounds[3];
        const auto maximum_width = std::max(1, dimensions.dimx - editor.window_x);
        const auto maximum_height = std::max(1, dimensions.dimy - editor.window_y - 1);
        if (modal_cancel(event) || event == Event::CtrlC) {
            editor.window_x = editor_window_previous[0];
            editor.window_y = editor_window_previous[1];
            editor.window_width = editor_window_previous[2];
            editor.window_height = editor_window_previous[3];
            editor_window_action = 0;
            editor_window_mouse_drag = false;
        } else if (event == Event::Return) {
            editor_window_action = 0;
            editor_window_mouse_drag = false;
        } else if (event == Event::ArrowLeft || event == Event::ArrowRight ||
                   event == Event::ArrowUp || event == Event::ArrowDown) {
            const auto dx = event == Event::ArrowLeft ? -1 : event == Event::ArrowRight ? 1 : 0;
            const auto dy = event == Event::ArrowUp ? -1 : event == Event::ArrowDown ? 1 : 0;
            if (editor_window_action == 3) {
                editor.window_x = std::clamp(editor.window_x + dx, 0, std::max(0, dimensions.dimx - editor.window_width));
                editor.window_y = std::clamp(editor.window_y + dy, 1, std::max(1, dimensions.dimy - editor.window_height - 1));
            } else {
                editor.window_width = std::clamp(editor.window_width + dx, std::min(12, maximum_width), maximum_width);
                editor.window_height = std::clamp(editor.window_height + dy, std::min(4, maximum_height), maximum_height);
            }
        } else if (event.is_mouse()) {
            auto copy = event;
            const auto& mouse = copy.mouse();
            if (mouse.button == Mouse::Left && mouse.motion == Mouse::Pressed) {
                editor_window_mouse_drag = true;
                editor_window_mouse_offset = mouse.x - editor.window_x;
            } else if (editor_window_mouse_drag && mouse.motion == Mouse::Moved) {
                if (editor_window_action == 3) {
                    editor.window_x = std::clamp(mouse.x - editor_window_mouse_offset, 0,
                                                std::max(0, dimensions.dimx - editor.window_width));
                    editor.window_y = std::clamp(mouse.y, 1, std::max(1, dimensions.dimy - editor.window_height - 1));
                } else {
                    editor.window_width = std::clamp(mouse.x - editor.window_x + 1, std::min(12, maximum_width), maximum_width);
                    editor.window_height = std::clamp(mouse.y - editor.window_y + 1, std::min(4, maximum_height), maximum_height);
                }
            } else if (editor_window_mouse_drag && mouse.motion == Mouse::Released) {
                editor_window_mouse_drag = false;
                editor_window_action = 0;
            }
        }
        editor.window_geometry_saved = true;
        update_editor_view();
        return true;
    }

    void reload_syntax_rules() {
        const auto directory = default_config_path().parent_path();
        try {
            auto candidate = syntax_highlighter;
            candidate.load_registry(directory / "Syntax", directory / "syntax.d");
            auto syntax = read_local_rules(directory / "syntax");
            for (const auto& [pattern, language] : syntax) {
                static_cast<void>(pattern);
                if (!candidate.supports_language(language)) {
                    throw std::runtime_error("unknown syntax language in legacy rules: " + language);
                }
            }
            if (!candidate.supports_language(config.editor_syntax_language)) {
                throw std::runtime_error("syntax definition is unavailable: " + config.editor_syntax_language);
            }
            syntax_highlighter = std::move(candidate);
            syntax_rules = std::move(syntax);
            syntax_registry_ready = true;
            syntax_load_error.clear();
            syntax_error_notice.clear();
            syntax_spans.clear();
            syntax_path.clear();
        } catch (const std::exception& error) {
            if (!syntax_registry_ready) syntax_load_error = error.what();
            throw;
        }
    }

    void report_syntax_error(const std::exception& error) {
        const auto message = std::string(error.what());
        if (syntax_error_notice == message) return;
        syntax_error_notice = message;
        status = "Syntax error: " + message;
        post_ui([this, message] {
            if (mode == Mode::editor) show_editor_message("Syntax error", message);
        });
    }

    void reload_local_rules() {
        const auto directory = default_config_path().parent_path();
        auto extensions = ExtensionRules::load(find_extension_file(directory));
        auto highlighting = FileHighlightRules::load(directory, highlight_rules.resource());
        reload_syntax_rules();
        extension_rules = std::move(extensions);
        highlight_rules = std::move(highlighting);
        syntax_spans.clear();
        syntax_path.clear();
    }

    std::string effective_syntax_language() const {
        if (config.editor_syntax_language != "auto") return config.editor_syntax_language;
        const auto name = std::filesystem::path(editor.path()).filename().string();
        for (const auto& [pattern, language] : syntax_rules) {
            if (EditorSearch::filename_matches(name, pattern, true)) return language;
        }
        return "auto";
    }

    void show_syntax_settings() {
        std::vector<std::pair<std::string, std::string>> syntaxes{
            {"< Plain text >", "plain"},
            {"C/C++ Program", "cpp"}, {"Rust Program", "rust"}, {"GoLang Program", "go"},
            {"JavaScript Program", "javascript"}, {"Python Program", "python"},
            {"Shell Script", "shell"}, {"JSON File", "json"}, {"Markdown", "markdown"},
            {"INI-based File", "config"}
        };
        for (const auto& definition : syntax_highlighter.definitions()) {
            syntaxes.emplace_back(definition.name, definition.id);
        }
        if (config.editor_syntax_language != "auto" &&
            !syntax_highlighter.supports_language(config.editor_syntax_language)) {
            syntaxes.emplace_back(config.editor_syntax_language + " [unavailable]", config.editor_syntax_language);
        }
        std::vector<std::string> names{"< Auto >", "< Reload Current Syntax >"};
        for (const auto& [name, language] : syntaxes) {
            static_cast<void>(language);
            names.push_back(name);
        }
        show_local_list("Choose syntax highlighting", std::move(names),
            [this, syntaxes](const std::size_t selected) {
                if (selected == 1U) reload_syntax_rules();
                else {
                    const auto language = selected == 0U ? "auto" : syntaxes.at(selected - 2U).second;
                    if (!syntax_highlighter.supports_language(language)) {
                        throw std::runtime_error("syntax definition is unavailable: " + language);
                    }
                    if (language == "auto" && !syntax_load_error.empty()) {
                        throw std::runtime_error(syntax_load_error);
                    }
                    config.editor_syntax_language = language;
                }
                syntax_spans.clear();
                syntax_path.clear();
            }, Mode::editor);
        local_list_native = true;
        for (std::size_t index = 0; index < syntaxes.size(); ++index) {
            if (syntaxes[index].second == config.editor_syntax_language) local_list_cursor = index + 2U;
        }
    }

    static std::string extension_action_name(const ExtensionAction action) {
        switch (action) {
            case ExtensionAction::open: return "Open";
            case ExtensionAction::view: return "View";
            case ExtensionAction::edit: return "Edit";
        }
        return "Open";
    }

    std::shared_ptr<ExtensionInvocation> extension_invocation(
        ExtensionCommand command, const ExtensionAction action, const ProviderPtr& provider,
        const std::string& path, const Mode return_mode, const bool allow_missing = false
    ) const {
        auto invocation = std::make_shared<ExtensionInvocation>();
        invocation->entry = std::move(command);
        invocation->action = action;
        invocation->provider = provider;
        invocation->other_provider = (active_index == 0 ? right : left).provider();
        invocation->path = path;
        invocation->return_mode = return_mode;
        invocation->pane_id = pane_ids[active_index];
        invocation->context = user_menu_context(Mode::panels, invocation->entry.source);
        auto& pane = invocation->context.active;
        pane.local = provider == local;
        const auto& source_pane = active_index == 0 ? left : right;
        const auto directory = invocation->entry.legacy && provider == source_pane.provider()
            ? source_pane.path() : provider->parent(path);
        pane.directory = pane.local ? directory : archives.format(provider, directory);
        if (allow_missing && !provider->exists(path)) {
            pane.file = Entry{};
            pane.file->kind = EntryKind::file;
            pane.file->path = path;
        } else pane.file = provider->stat(path, {});
        pane.file->name = invocation->entry.legacy ? path : provider->basename(path);
        if (provider != (active_index == 0 ? left : right).provider()) pane.tagged.clear();
        for (auto& tagged : pane.tagged) {
            tagged.name = provider->parent(tagged.path) == directory ? provider->basename(tagged.path) : tagged.path;
        }
        if (!invocation->entry.legacy) {
            invocation->requirements = native_menu_requirements(invocation->entry.command, false);
        }
        return invocation;
    }

    void guard_extension_editor(const std::shared_ptr<ExtensionInvocation>& invocation) const {
        const auto conflicts = [this](const EditorBuffer& buffer, const ProviderPtr& provider, const Entry& file) {
            if (!buffer.open() || !buffer.dirty() || buffer.provider() != provider) return false;
            if (provider->normalize(buffer.path()) == provider->normalize(file.path)) return true;
            if (provider != local) return false;
            std::error_code error;
            return std::filesystem::equivalent(buffer.path(), file.path, error) && !error;
        };
        const auto inspect = [&](const ProviderPtr& provider, const NativeMenuPane& pane) {
            const auto check = [&](const Entry& file) {
                if (conflicts(editor, provider, file) ||
                    std::ranges::any_of(editor_buffers, [&](const EditorBuffer& buffer) {
                        return conflicts(buffer, provider, file);
                    })) {
                    throw std::runtime_error("save or close the dirty editor buffer before running an extension command for " +
                                             display_text(file.name));
                }
            };
            if (pane.file) check(*pane.file);
            for (const auto& file : pane.tagged) check(file);
        };
        inspect(invocation->provider, invocation->context.active);
        inspect(invocation->other_provider, invocation->context.other);
    }

    static std::string extension_failure(
        const ExtensionInvocation& invocation, const int result, const std::string& error = {}
    ) {
        const auto source = invocation.entry.source.empty() ? std::string{} :
            " (" + invocation.entry.source + ":" + std::to_string(invocation.entry.line) + ")";
        return "extension " + extension_action_name(invocation.action) + " exited " + std::to_string(result) +
               (result == 127 ? "; a required helper command is missing" : "") +
               (error.empty() ? "" : ": " + error) + source;
    }

    std::string extension_shell_command(
        const ExtensionInvocation& invocation, const std::string& command
    ) const {
        const auto scalar = [&](const std::string& value) {
            auto context = invocation.context;
            context.active.file->name = value;
            return expand_native_menu("%p", context).command;
        };
        std::string tagged;
        for (const auto& file : invocation.context.active.tagged) {
            if (!tagged.empty()) tagged += ' ';
            tagged += file.name;
        }
        const auto& file = *invocation.context.active.file;
        return "( export MC_EXT_FILENAME=" + scalar(file.name) +
            " MC_EXT_BASENAME=" + scalar(invocation.provider->basename(invocation.path)) +
            " MC_EXT_CURRENTDIR=" + scalar(invocation.context.active.directory) +
            " MC_EXT_SELECTED=" + scalar(tagged.empty() ? file.name : tagged) +
            " MC_EXT_ONLYTAGGED=" + scalar(tagged) + "\n" + command + "\n)";
    }

    void complete_extension_action(
        const std::shared_ptr<ExtensionInvocation>& invocation, const NativeMenuExpansion& expansion
    ) {
        auto& pane = pane_ids[0] == invocation->pane_id ? left : right;
        auto& other = pane_ids[0] == invocation->pane_id ? right : left;
        if (pane.provider() == invocation->provider) pane.unmark_paths(expansion.untag_active);
        if (other.provider() == invocation->other_provider) other.unmark_paths(expansion.untag_other);
        mode = invocation->return_mode;
        status = "Extension " + extension_action_name(invocation->action) + " completed";
        if (invocation->action == ExtensionAction::edit && expansion.action == NativeMenuAction::shell &&
            invocation->provider == local) remember_editor_path(invocation->path);
        if (extension_pending == invocation) extension_pending.reset();
    }

    void complete_pending_extension() {
        if (!extension_pending || !extension_pending->running || !extension_pending->completed_status ||
            (mode != Mode::panels && mode != Mode::search_results)) return;
        const auto invocation = extension_pending;
        const auto result = *invocation->completed_status;
        if (result == 0) complete_extension_action(invocation, *invocation->running);
        else {
            extension_pending.reset();
            show_native_message("File extension", extension_failure(*invocation, result), invocation->return_mode);
        }
    }

    void continue_extension_action(const std::shared_ptr<ExtensionInvocation>& invocation) {
        if (invocation->answers.size() < invocation->requirements.prompts.size()) {
            const auto index = invocation->answers.size();
            show_native_input("File extension", invocation->requirements.prompts[index], "",
                [this, invocation, index](const std::string& answer) {
                    invocation->answers.resize(index + 1U);
                    invocation->answers[index] = answer;
                    continue_extension_action(invocation);
                }, invocation->return_mode,
                invocation->context.active.local ? invocation->context.active.directory : "",
                "extension.argument");
            return;
        }
        if (extension_pending && extension_pending->running) {
            throw std::runtime_error("an extension command is still running; use Ctrl-O to resume it");
        }
        if (invocation->entry.command.find("@EXTHELPERSDIR@") != std::string::npos) {
            throw std::runtime_error("unconfigured @EXTHELPERSDIR@ resource; mcpp does not bundle MC helper scripts");
        }
        NativeMenuExpansion expansion;
        if (invocation->prepared) expansion = *invocation->prepared;
        else if (invocation->entry.legacy) {
            if (!invocation->context.active.local) {
                throw std::runtime_error("copy archive members to a local directory before running extension commands");
            }
            expansion.command = expand_user_menu_command(invocation->entry.command, invocation->path,
                invocation->context.active.directory, invocation->context.other.directory);
        } else expansion = expand_native_menu(invocation->entry.command, invocation->context, invocation->answers);
        if (expansion.action == NativeMenuAction::change_directory) {
            const auto words = plain_command_words(expansion.command);
            if (!words || words->size() != 1U || words->front().empty()) {
                throw std::invalid_argument("extension %cd needs one literal directory, not shell commands");
            }
            auto destination = words->front();
            auto& previous = command_previous_directories[static_cast<std::size_t>(invocation->pane_id)];
            if (destination == "-") {
                if (previous.empty()) throw std::runtime_error("extension %cd: no previous directory");
                destination = previous;
            } else if (destination == "~" || destination.starts_with("~/")) {
                const auto* home = std::getenv("HOME");
                if (home == nullptr || *home == '\0') throw std::runtime_error("extension %cd: HOME is not set");
                destination = std::string(home) + destination.substr(1U);
            } else if (destination.starts_with('~')) {
                throw std::invalid_argument("extension %cd: use an explicit home-directory path");
            } else if (destination.find("://") != std::string::npos) {
                if (!std::filesystem::path(destination).is_absolute()) {
                    if (invocation->provider != local) throw std::runtime_error("nested archives are not supported yet");
                    destination = invocation->context.active.directory + "/" + destination;
                }
            } else {
                destination = archives.format(invocation->provider,
                    invocation->provider->join(invocation->provider->parent(invocation->path), destination));
            }
            auto& pane = pane_ids[0] == invocation->pane_id ? left : right;
            const auto old = pane_location(pane);
            change_panel_directory(pane, destination);
            previous = old;
            complete_extension_action(invocation, expansion);
            status = "Directory changed: " + pane_location(pane);
            return;
        }
        if (expansion.action == NativeMenuAction::view && expansion.command.empty()) {
            if (!can_open_file(invocation->provider->stat_follow(invocation->path, {}))) {
                throw std::runtime_error("extension %view needs a regular file");
            }
            load_viewer(invocation->provider, invocation->path);
            complete_extension_action(invocation, expansion);
            set_viewer_return_mode(invocation->return_mode);
            viewer_search_needle.clear();
            mode = Mode::viewer;
            set_user_menu_view_options(expansion);
            return;
        }
        guard_extension_editor(invocation);
        const auto command = extension_shell_command(*invocation, expansion.command);
        if (expansion.action == NativeMenuAction::view) {
            const auto directory = invocation->context.active.directory;
            start_local_task("Extension " + extension_action_name(invocation->action),
                [command, directory](const Cancellation& cancellation) {
                    return capture_shell(command, directory, cancellation, 16U * 1024U * 1024U);
                }, [this, invocation, expansion](CommandOutput result) {
                    if (result.status != 0) throw std::runtime_error(extension_failure(*invocation, result.status, result.error));
                    complete_extension_action(invocation, expansion);
                    open_output_viewer("Extension " + extension_action_name(invocation->action), result.output);
                    set_viewer_return_mode(invocation->return_mode);
                    set_user_menu_view_options(expansion);
                    show_command_warnings(result.error);
                }, invocation->return_mode);
            return;
        }
        if (invocation->action == ExtensionAction::open && config.confirm_execute && !invocation->confirmed) {
            show_native_query("Execute", "Execute " + display_text(invocation->context.active.file->name) + "?",
                {"Yes", "No"}, [this, invocation](const std::size_t selected) {
                    if (selected != 0U) return;
                    invocation->confirmed = true;
                    continue_extension_action(invocation);
                }, invocation->return_mode);
            return;
        }
        extension_pending = invocation;
        int result = 0;
        try {
            result = run_process(command, false, invocation->context.active.directory);
        } catch (...) {
            extension_pending.reset();
            throw;
        }
        if (shell.busy()) {
            invocation->running = std::move(expansion);
            mode = invocation->return_mode;
            status = "Extension command is running; Ctrl-O resumes it";
            return;
        }
        extension_pending.reset();
        if (result != 0) throw std::runtime_error(extension_failure(*invocation, result));
        complete_extension_action(invocation, expansion);
    }

    bool dispatch_extension_action(
        const ProviderPtr& provider, const std::string& path, const ExtensionAction action,
        const Mode return_mode
    ) {
        extension_rules = ExtensionRules::load(find_extension_file(default_config_path().parent_path()));
        auto entry = provider->stat(path, {});
        entry.name = provider->basename(path);
        if (provider == active().provider()) {
            const auto found = std::ranges::find(active().entries(), path, &Entry::path);
            if (found != active().entries().end()) entry.name = found->name;
        }
        const auto selected = extension_rules.match(entry, action, provider == local,
            [path](const ExtensionType type) { return probe_extension_type(path, type); });
        if (!selected) return false;
        mode = return_mode;
        if (selected->command.empty()) {
            status = "Extension " + extension_action_name(action) + " is disabled for " + display_text(entry.name);
            return true;
        }
        continue_extension_action(extension_invocation(*selected, action, provider, path, return_mode));
        return true;
    }

    void run_extension_external_tool(
        const ProviderPtr& provider, const std::string& path, const Mode return_mode,
        const ExtensionAction action, const std::string& tool, const bool require_existing = true
    ) {
        if (provider != local) {
            throw std::runtime_error("copy archive members to a local directory before using an external " +
                                     std::string(action == ExtensionAction::view ? "viewer" : "editor"));
        }
        auto invocation = extension_invocation({}, action, provider, path, return_mode, !require_existing);
        invocation->context.active.file->name = path;
        invocation->prepared = expand_native_menu("%f", invocation->context);
        invocation->prepared->command = tool + " " + invocation->prepared->command;
        continue_extension_action(invocation);
    }

    void view_file_with_extensions(
        const ProviderPtr& provider, const std::string& path, const Mode return_mode, const bool raw = false
    ) {
        if (!config.use_internal_view) {
            const auto* external = std::getenv("VIEWER");
            if (external == nullptr || *external == '\0') external = std::getenv("PAGER");
            run_extension_external_tool(provider, path, return_mode, ExtensionAction::view,
                external != nullptr && *external != '\0' ? external : "less");
            return;
        }
        if (raw || !dispatch_extension_action(provider, path, ExtensionAction::view, return_mode)) {
            view_file(provider, path, return_mode);
        }
    }

    void edit_file_with_extensions(
        const ProviderPtr& provider, const std::string& path, const Mode return_mode,
        const bool require_existing = true
    ) {
        if ((!require_existing && !provider->exists(path)) ||
            !dispatch_extension_action(provider, path, ExtensionAction::edit, return_mode)) {
            if (config.use_internal_edit) edit_file(provider, path, return_mode, require_existing);
            else {
                const auto* external = std::getenv("EDITOR");
                run_extension_external_tool(provider, path, return_mode, ExtensionAction::edit,
                    external != nullptr && *external != '\0' ? external : "vi", require_existing);
            }
        }
    }

    void activate_file_association() {
        const auto* selected = active().current();
        if (selected == nullptr) return;
        const auto entry = *selected;
        if (entry.revision != "mcpp:parent" &&
            dispatch_extension_action(active().provider(), entry.path, ExtensionAction::open, Mode::panels)) return;
        if (enter_panel_entry()) return;
        if (!can_open_file(entry)) return;
        const auto executable = entry.permissions &
            (std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec |
             std::filesystem::perms::others_exec);
        if (executable == std::filesystem::perms::none) {
            status = "No file association; use F3 to view or F4 to edit";
            return;
        }
        ExtensionCommand command;
        command.command = "%f";
        command.legacy = true;
        continue_extension_action(extension_invocation(command, ExtensionAction::open,
            active().provider(), entry.path, Mode::panels));
    }

    EditorLocation editor_location() const {
        return {editor.path(), {editor.row(), editor.column()}};
    }

    void open_editor_location(const EditorLocation& location, std::function<void()> after_open = {}) {
        const auto source = resolve_editor_location(location.path);
        open_editor_buffer(source.provider, source.path, false, true, [this, location, after_open = std::move(after_open)] {
            editor.set_position(location.position);
            center_editor();
            if (after_open) after_open();
        });
    }

    void follow_declaration(const Declaration& target) {
        const auto origin = editor_location();
        open_editor_location({target.path, {target.line - 1U, 0}}, [this, origin, target] {
            if (declaration_history.empty()) {
                declaration_history.push_back(origin);
            } else {
                declaration_history[declaration_cursor] = origin;
                declaration_history.erase(declaration_history.begin() +
                                              static_cast<std::ptrdiff_t>(declaration_cursor + 1U),
                                          declaration_history.end());
            }
            declaration_history.push_back(editor_location());
            declaration_cursor = declaration_history.size() - 1U;
            status = "Declaration: " + target.path + ":" + std::to_string(target.line);
        });
    }

    void move_declaration_history(const bool forward) {
        if (declaration_history.empty() || (!forward && declaration_cursor == 0U) ||
            (forward && declaration_cursor + 1U >= declaration_history.size())) {
            show_editor_message("Declarations", forward ? "No forward declaration position"
                                                       : "No previous declaration position");
            return;
        }
        declaration_history[declaration_cursor] = editor_location();
        const auto next = forward ? declaration_cursor + 1U : declaration_cursor - 1U;
        open_editor_location(declaration_history[next], [this, next] { declaration_cursor = next; });
    }

    void show_find_declaration() {
        const auto& line = editor.lines()[editor.row()];
        auto begin = std::min(editor.column(), line.size());
        const auto identifier = [](const unsigned char byte) {
            return byte >= 128U || byte == '_' || std::isalnum(byte) != 0;
        };
        while (begin > 0U && identifier(static_cast<unsigned char>(line[begin - 1U]))) --begin;
        auto end = std::min(editor.column(), line.size());
        while (end < line.size() && identifier(static_cast<unsigned char>(line[end]))) ++end;
        const auto word = line.substr(begin, end - begin);
        if (word.empty()) throw std::runtime_error("there is no identifier at the cursor");
        const auto file = editor.path();
        auto targets = std::make_shared<std::vector<Declaration>>();
        start_local_task("Finding declaration: " + word,
            [file, word, targets](const Cancellation& cancellation) {
                *targets = find_declarations(file, word, cancellation);
                return CommandOutput{};
            }, [this, targets](CommandOutput) {
                if (targets->size() == 1U) {
                    follow_declaration(targets->front());
                } else {
                    std::vector<std::string> choices;
                    for (const auto& target : *targets) {
                        choices.push_back(target.path + ":" + std::to_string(target.line));
                    }
                    show_local_list("Declaration targets", std::move(choices),
                        [this, targets](const std::size_t index) { follow_declaration(targets->at(index)); },
                        Mode::editor);
                }
            }, Mode::editor);
    }

    static bool recordable_editor_key(const ftxui::Event& event) {
        using namespace ftxui;
        if (event.is_character() && !event.input().empty()) return true;
        static const std::vector<Event> keys{
            Event::ArrowLeft, Event::ArrowRight, Event::ArrowUp, Event::ArrowDown,
            Event::ArrowLeftCtrl, Event::ArrowRightCtrl, Event::ArrowUpCtrl, Event::ArrowDownCtrl,
            Event::Home, Event::End, Event::PageUp, Event::PageDown,
            Event::Return, Event::Tab, Event::Insert, Event::Delete, Event::Backspace,
            Event::F3, Event::F5, Event::F6, Event::F8, Event::CtrlU, Event::AltR,
            Event::CtrlK, Event::CtrlY, Event::CtrlA, Event::CtrlE,
            Event::Special("\x1B[25~"), Event::Special("\x1B[1;5H"), Event::Special("\x1B[1;5F"),
            Event::Special("\x1B[2;5~"), Event::Special("\x1B[3;2~"), Event::Special("\x1B[2;2~")
        };
        return std::ranges::find(keys, event) != keys.end() ||
               (event.input().starts_with("\x1B[1;") && !event.input().empty() &&
                std::string_view("ABCDHF").find(event.input().back()) != std::string_view::npos);
    }

    static std::string encode_macro(const std::vector<MacroKey>& keys) {
        std::string result;
        for (const auto& key : keys) {
            result += key.character ? 'c' : 's';
            result += encode_hex_bytes(key.input);
            result += ';';
        }
        return result;
    }

    static std::vector<MacroKey> decode_macro(const std::string& text) {
        std::vector<MacroKey> result;
        std::size_t begin = 0;
        while (begin < text.size()) {
            const auto end = text.find(';', begin);
            if (end == std::string::npos || end <= begin + 1U ||
                (text[begin] != 'c' && text[begin] != 's')) {
                throw std::runtime_error("invalid recorded macro");
            }
            MacroKey key{text[begin] == 'c',
                         decode_hex_query(std::string_view(text).substr(begin + 1U, end - begin - 1U))};
            const auto event = key.character ? ftxui::Event::Character(key.input)
                                            : ftxui::Event::Special(key.input);
            if (!recordable_editor_key(event)) throw std::runtime_error("macro contains a non-editing key");
            result.push_back(std::move(key));
            begin = end + 1U;
        }
        if (result.empty()) throw std::runtime_error("macro contains no editing keys");
        return result;
    }

    void toggle_macro_recording() {
        if (macro_recording) {
            macro_recording = false;
            show_key_query("Save macro", "Press the macro's new hotkey:", true,
                [this](const ftxui::Event& event) {
                    if (event == ftxui::Event::CtrlR) throw std::runtime_error("macro recording hotkey is reserved");
                    const auto hotkey = encode_hex_bytes(event.input());
                    if (macro_keys.empty()) config.editor_macros.erase(hotkey);
                    else config.editor_macros[hotkey] = encode_macro(macro_keys);
                    status = "Macro saved";
                });
            return;
        }
        macro_keys.clear();
        macro_recording = true;
        status = "Recording macro";
    }

    void show_delete_macro() {
        show_key_query("Delete macro", "Press macro hotkey:", true, [this](const ftxui::Event& event) {
            if (config.editor_macros.erase(encode_hex_bytes(event.input())) == 0U) {
                show_editor_message("Delete macro", "Macro not deleted");
            } else status = "Macro deleted";
        });
    }

    void step_macro() {
        static_cast<void>(run_editor_action([this] {
            if (!macro_replaying) return true;
            try {
                for (int count = 0; count < 32 && macro_replaying; ++count) {
                    const auto key = macro_playback.at(macro_play_index);
                    handle_editor(key.character ? ftxui::Event::Character(key.input)
                                                : ftxui::Event::Special(key.input));
                    if (mode != Mode::editor) {
                        macro_replaying = false;
                        return true;
                    }
                    if (++macro_play_index == macro_playback.size()) {
                        macro_play_index = 0;
                        if (--macro_repeats == 0U) {
                            macro_replaying = false;
                            status = "Macro completed";
                        }
                    }
                }
                if (macro_replaying) post_ui([this] { step_macro(); });
            } catch (const std::exception& error) {
                macro_replaying = false;
                show_editor_error(error);
            }
            return true;
        }));
    }

    void show_repeat_actions() {
        if (!macro_recording) {
            macro_keys.clear();
            macro_recording = true;
            status = "Recording commands to repeat";
            return;
        }
        macro_recording = false;
        show_native_input("Repeat last commands", "Repeat times:", "", [this](const std::string& value) {
            if (value.empty() || value.front() == '-') throw std::runtime_error("repeat count must be nonnegative");
            std::size_t parsed = 0;
            const auto count = std::stoull(value, &parsed, 0);
            if (parsed != value.size() || count > std::numeric_limits<std::size_t>::max()) {
                throw std::runtime_error("invalid repeat count");
            }
            start_macro(macro_keys, static_cast<std::size_t>(count));
        }, Mode::editor, {}, "editor.repeat-count");
    }

    void start_macro(std::vector<MacroKey> keys, const std::size_t repeats) {
        if (keys.empty() || repeats == 0U) {
            status = "No commands to repeat";
            return;
        }
        macro_playback = std::move(keys);
        macro_recording = false;
        macro_repeats = repeats;
        macro_play_index = 0;
        macro_replaying = true;
        post_ui([this] { step_macro(); });
    }

    void show_learn_keys(const Mode return_mode) {
        learn_key_return = return_mode;
        learn_key_focus = 39;
        learn_key_detected.fill(false);
        learn_keys_draft = config.learned_keys;
        mode = Mode::learn_keys_dialog;
    }

    static const std::vector<std::pair<std::string, std::string>>& learnable_keys() {
        static const auto result = [] {
            std::vector<std::pair<std::string, std::string>> keys{{"Escape", "Escape"}};
            for (int index = 1; index <= 20; ++index) {
                keys.emplace_back("Function key " + std::to_string(index), "F" + std::to_string(index));
            }
            for (const auto& key : std::vector<std::pair<std::string, std::string>>{
                {"Completion/M-tab", "Completion"}, {"BackTab/S-tab", "BackTab"},
                {"Backspace", "Backspace"}, {"Up arrow", "Up"}, {"Down arrow", "Down"},
                {"Left arrow", "Left"}, {"Right arrow", "Right"}, {"Insert", "Insert"},
                {"Delete", "Delete"}, {"Home", "Home"}, {"End key", "End"},
                {"Page Up", "PageUp"}, {"Page Down", "PageDown"},
                {"/ on keypad", "Keypad /"}, {"* on keypad", "Keypad *"},
                {"- on keypad", "Keypad -"}, {"+ on keypad", "Keypad +"}
            }) keys.push_back(key);
            return keys;
        }();
        return result;
    }

    void teach_key(const std::size_t index) {
        learn_key_focus = index;
        learn_key_action = learnable_keys().at(index).second;
        mode = Mode::learn_key;
    }

    ftxui::Element render_learn_keys() {
        using namespace ftxui;
        Elements rows;
        const auto& keys = learnable_keys();
        for (std::size_t row = 0; row < 13U; ++row) {
            Elements columns{text(" ")};
            for (std::size_t column = 0; column < 3U; ++column) {
                const auto index = column * 13U + row;
                Element cell = text("");
                if (index < keys.size()) {
                    cell = text("[" + fit_listing_text(keys[index].first, 16, ListingAlignment::left) + "]") |
                           reflect(learn_key_boxes[index]);
                    if (learn_key_focus == index) cell = focus_dialog(std::move(cell));
                    cell = hbox({std::move(cell), text(learn_key_detected[index] ? " OK" : "   "), filler()});
                }
                columns.push_back(std::move(cell) | size(WIDTH, EQUAL, 23));
            }
            rows.push_back(hbox(std::move(columns)));
        }
        return native_window("Learn keys", vbox({
            vbox(std::move(rows)) | yframe |
                size(HEIGHT, LESS_THAN, std::max(1, Terminal::Size().dimy - 10)) | reflect(learn_keys_viewport),
            separator(),
            text(" Press each listed key. Working keys are marked OK."),
            text(" Press Space on a missing key, or click it, to define it."),
            text(" Use Tab to move between buttons."),
            separator(),
            hbox({native_button("Save", true, learn_key_focus == 38U, learn_key_boxes[38]), text(" "),
                  native_button("Cancel", false, learn_key_focus == 39U, learn_key_boxes[39])}) | center,
        }), 76);
    }

    bool handle_learn_keys(const ftxui::Event& event) {
        using namespace ftxui;
        if (event.is_mouse()) {
            auto copy = event;
            const auto& mouse = copy.mouse();
            if (mouse.button != Mouse::Left || mouse.motion != Mouse::Pressed) return true;
            for (std::size_t index = 0; index < learn_key_boxes.size(); ++index) {
                if (index < 38U && !learn_keys_viewport.Contain(mouse.x, mouse.y)) continue;
                if (!learn_key_boxes[index].Contain(mouse.x, mouse.y)) continue;
                learn_key_focus = index;
                if (index < 38U) teach_key(index);
                else return handle_learn_keys(Event::Return);
                return true;
            }
            return true;
        }
        auto translated = event;
        if (const auto binding = learn_keys_draft.find(encode_hex_bytes(event.input()));
            binding != learn_keys_draft.end()) translated = named_event(binding->second);
        const auto& keys = learnable_keys();
        for (std::size_t index = 0; index < keys.size(); ++index) {
            if (!learn_key_detected[index] && translated == named_event(keys[index].second)) {
                learn_key_detected[index] = true;
                learn_key_focus = index;
                return true;
            }
        }
        if (event == Event::Escape || event == Event::AltC) {
            mode = learn_key_return;
        } else if (event == Event::AltS || (event == Event::Return && learn_key_focus == 38U)) {
            config.learned_keys = learn_keys_draft;
            mode = learn_key_return;
        } else if ((event == Event::Return || event == Event::Character(' ')) && learn_key_focus == 39U) {
            mode = learn_key_return;
        } else if ((event == Event::Return || event == Event::Character(' ')) && learn_key_focus < 38U) {
            teach_key(learn_key_focus);
        } else if (event == Event::Tab || event == Event::Character('j') || event == Event::ArrowDown) {
            learn_key_focus = (learn_key_focus + 1U) % 40U;
        } else if (event == Event::TabReverse || event == Event::Character('k') || event == Event::ArrowUp) {
            learn_key_focus = (learn_key_focus + 39U) % 40U;
        } else if (event == Event::ArrowRight || event == Event::Character('l')) {
            learn_key_focus = learn_key_focus < 26U ? std::min<std::size_t>(37U, learn_key_focus + 13U)
                                                  : learn_key_focus % 13U;
        } else if (event == Event::ArrowLeft || event == Event::Character('h')) {
            learn_key_focus = learn_key_focus >= 13U && learn_key_focus < 38U ? learn_key_focus - 13U
                                                                          : std::min<std::size_t>(37U, learn_key_focus + 26U);
        }
        return true;
    }

    ftxui::Element render_learn_key() {
        using namespace ftxui;
        const auto label = learnable_keys().at(learn_key_focus).first;
        return native_window("Teach me a key", vbox({
            text(" Press " + label + " and wait."),
            text(" Press it again in Learn keys to verify the OK marker."),
            text(" A single Escape cancels teaching."),
        }), 65);
    }

    bool handle_learn_key(const ftxui::Event& event) {
        if (event == ftxui::Event::Escape) {
            mode = Mode::learn_keys_dialog;
        } else if (!event.input().empty() && !event.is_mouse() &&
                   !event.is_cursor_position() && !event.is_cursor_shape() &&
                   !event.IsTerminalNameVersion() && !event.IsTerminalEmulator() &&
                   !event.IsTerminalCapabilities()) {
            const auto& input = event.input();
            const bool printable = input.size() == 1U &&
                                   static_cast<unsigned char>(input[0]) >= 32U &&
                                   static_cast<unsigned char>(input[0]) <= 126U;
            if (printable || event == ftxui::Event::Return || event == ftxui::Event::Tab) {
                show_native_message("Warning", "Cannot accept this key.", Mode::learn_keys_dialog);
            } else {
                learn_keys_draft[encode_hex_bytes(input)] = learn_key_action;
                mode = Mode::learn_keys_dialog;
            }
        }
        return true;
    }

    void show_literal_insert() {
        show_key_query("Insert literal", "Press any key:", false, [this](const ftxui::Event& event) {
            if (event == ftxui::Event::Special("\x1B[32;5u")) editor.paste(std::string(1, '\0'));
            else if (event.is_character() || event.input().size() == 1U) editor.paste(event.input());
        });
    }

    void show_date_insert() {
        const auto now = std::time(nullptr);
        std::tm local_time{};
        if (localtime_r(&now, &local_time) == nullptr) throw std::runtime_error("cannot read local time");
        std::array<char, 4096> value{};
        const auto length = std::strftime(value.data(), value.size(), "%c", &local_time);
        if (length == 0U) throw std::runtime_error("cannot format local time");
        editor.paste(std::string_view(value.data(), length));
    }

    void show_paste_output() {
        show_native_input("Paste output of external command", "Enter shell command(s):", "",
            [this](const std::string& command) {
            if (command.empty()) throw std::runtime_error("command is empty");
            const auto path = editor.path();
            const auto change = editor.change_id();
            const EditorPosition position{editor.row(), editor.column()};
            const auto directory = editor_directory(editor);
            start_local_task("Capturing command output",
                [command, directory](const Cancellation& cancellation) {
                    return capture_shell(command, directory, cancellation, 16U * 1024U * 1024U);
                }, [this, path, change, position](CommandOutput result) {
                    require_command_success(result);
                    if (editor.path() != path || editor.change_id() != change) {
                        throw std::runtime_error("buffer changed while the command was running");
                    }
                    editor.set_position(position);
                    editor.paste(result.output);
                    if (!config.editor_cursor_after_inserted_block) editor.set_position(position);
                    status = result.output.empty() ? "Command produced no output" : "Command output inserted";
                    show_command_warnings(result.error);
                }, Mode::editor);
        }, Mode::editor, {}, "command.paste-output");
    }

    void show_editor_transform(const int action) {
        if (action == 3) {
            if (!editor.selection()) {
                show_editor_message("Error", "You must first highlight a block of text");
                return;
            }
            show_native_input("Run sort", "Enter sort options (see sort(1) manpage) separated by whitespace:", "",
                [this](const std::string& options) {
                    const auto words = plain_command_words("sort " + options);
                    if (!words) throw std::invalid_argument("sort options must be plain arguments, not shell operators");
                    run_editor_transform(*words, "", false);
                }, Mode::editor, {}, "editor.sort-options");
        } else if (action == 2) {
            run_editor_transform({"fmt", "-w", std::to_string(config.editor_wrap_length)}, "", true);
        } else {
            const auto* data = std::getenv("XDG_DATA_HOME");
            const auto* home = std::getenv("HOME");
            if ((data == nullptr || *data == '\0') && (home == nullptr || *home == '\0')) {
                throw std::runtime_error("XDG_DATA_HOME and HOME are unavailable for external formatter configuration");
            }
            const auto directory = data != nullptr && *data != '\0' ? std::filesystem::path(data)
                                   : std::filesystem::path(home) / ".local/share";
            const auto path = directory / "mcpp/mcedit/macros.d/macro.0.sh";
            if (!local->exists(path.string())) {
                throw std::runtime_error("external formatter menu is not configured: " + path.string());
            }
            open_user_menu(Mode::editor, path);
            if (user_menu.empty()) {
                if (user_menu_error.empty()) user_menu_error = "external formatter menu has no matching command";
                return;
            }
            execute_user_menu_entry(user_menu[user_menu_cursor]);
        }
    }

    void run_editor_transform(std::vector<std::string> arguments, std::string command, const bool paragraph) {
        const auto selected = editor.selected_text();
        std::pair<EditorPosition, EditorPosition> range;
        std::string source = selected.value_or("");
        if (paragraph && !selected) {
            const auto blank = [](const std::string& line) {
                return std::ranges::all_of(line, [](const unsigned char byte) { return std::isspace(byte) != 0; });
            };
            auto first = editor.row(), last = first;
            if (blank(editor.lines()[first])) return;
            while (first > 0U && !blank(editor.lines()[first - 1U])) --first;
            while (last + 1U < editor.lines().size() && !blank(editor.lines()[last + 1U])) ++last;
            range = {{first, 0}, last + 1U < editor.lines().size() ? EditorPosition{last + 1U, 0}
                                                                : EditorPosition{last, editor.lines()[last].size()}};
            source = editor.text_range(range.first, range.second);
        }
        const bool crlf = source.find("\r\n") != std::string::npos;
        if (editor.line_separator() == '\r') std::replace(source.begin(), source.end(), '\r', '\n');
        auto input = std::make_shared<TemporaryFile>(source);
        const auto file = editor.path();
        const auto change = editor.change_id();
        const auto directory = editor_directory(editor);
        if (command.empty()) arguments.insert(arguments.end(), {"--", input->path().string()});
        else command += " < " + expand_user_menu_command("%f", input->path().string(), directory, directory);
        start_local_task("Transforming editor text",
            [input, arguments, command, directory](const Cancellation& cancellation) {
                return command.empty() ? capture_command(arguments, directory, cancellation, 16U * 1024U * 1024U)
                                       : capture_shell(command, directory, cancellation, 16U * 1024U * 1024U);
            }, [this, input, file, change, range, selected, crlf, paragraph](CommandOutput result) {
                require_command_success(result);
                if (editor.path() != file || editor.change_id() != change) {
                    throw std::runtime_error("buffer changed while transforming text");
                }
                if (paragraph && crlf) {
                    std::string converted;
                    for (std::size_t index = 0; index < result.output.size(); ++index) {
                        if (result.output[index] == '\n' && (index == 0U || result.output[index - 1U] != '\r')) converted += '\r';
                        converted += result.output[index];
                    }
                    result.output = std::move(converted);
                }
                if (selected) editor.replace_block(result.output);
                else if (paragraph) editor.replace_text(range.first, range.second, result.output);
                else editor.paste(result.output);
                status = "Text transformed; Undo restores the original";
                show_command_warnings(result.error);
            }, Mode::editor);
    }

    void editor_bookmark_action(const int action) {
        if (action == 3) editor.toggle_bookmark();
        else if (action == 6) editor.clear_bookmarks();
        else if (!editor.jump_bookmark(action == 4 ? 1 : -1)) {
            show_editor_message("Bookmarks", "No bookmarks in this buffer");
            return;
        }
        remember_editor_position(editor);
        update_editor_view();
    }

    void finish_spelling(const bool cancelled) {
        spelling_issues.clear();
        spelling_match.reset();
        spelling_error.clear();
        mode = Mode::editor;
        status = cancelled ? "Spell check cancelled" : "Spell check complete";
    }

    void show_next_spelling_issue() {
        if (spelling_index >= spelling_issues.size()) {
            finish_spelling(false);
            return;
        }
        const auto& issue = spelling_issues[spelling_index];
        const auto text = editor.text();
        if (issue.offset > text.size() || issue.word.size() > text.size() - issue.offset ||
            text.compare(issue.offset, issue.word.size(), issue.word) != 0) {
            throw std::runtime_error("the misspelled word no longer matches the draft");
        }
        spelling_match = {{editor.position(issue.offset), editor.position(issue.offset + issue.word.size())}};
        editor.set_position(spelling_match->first);
        update_editor_view();
        spelling_suggestion = spelling_focus = 0;
        spelling_error.clear();
        mode = Mode::editor_spelling_dialog;
    }

    std::optional<std::vector<SyntaxSpellingRange>> spelling_context_ranges(const bool whole_file) const {
        if (!whole_file) return std::nullopt;
        const auto language = effective_syntax_language();
        if (!syntax_load_error.empty() && (language == "auto" || language.starts_with("external:"))) {
            throw std::runtime_error(syntax_load_error);
        }
        return syntax_highlighter.spelling_ranges(editor.path(), editor.lines(), language);
    }

    static void filter_spelling_contexts(
        std::vector<SpellingIssue>& issues,
        const std::optional<std::vector<SyntaxSpellingRange>>& eligible,
        const std::size_t first = 0U
    ) {
        if (!eligible) return;
        const auto begin = issues.begin() + static_cast<std::ptrdiff_t>(std::min(first, issues.size()));
        issues.erase(std::remove_if(begin, issues.end(), [&](const SpellingIssue& issue) {
            const auto next = std::upper_bound(eligible->begin(), eligible->end(), issue.offset,
                [](const std::size_t offset, const SyntaxSpellingRange& range) { return offset < range.begin; });
            return next == eligible->begin() || !(next - 1)->contains(issue.offset, issue.word.size());
        }), issues.end());
    }

    void show_spelling(const bool whole_file) {
        if (config.editor_spelling_language == "NONE") {
            show_native_message("Spell check", "Spell checking is disabled by spell_language=NONE.", Mode::editor);
            return;
        }
        const auto text = editor.text();
        const auto path = editor.path();
        const auto change = editor.change_id();
        const auto cursor = editor.cursor_offset();
        const auto directory = editor_directory(editor);
        const auto language = config.editor_spelling_language;
        const auto eligible = spelling_context_ranges(whole_file);
        auto issues = std::make_shared<std::vector<SpellingIssue>>();
        spelling_match.reset();
        spelling_whole_file = whole_file;
        start_local_task("Checking spelling",
            [issues, text, language, directory](const Cancellation& cancellation) {
                *issues = check_spelling(text, language, directory, cancellation);
                return CommandOutput{};
            }, [this, issues, path, change, cursor, whole_file, eligible](CommandOutput) {
                if (editor.path() != path || editor.change_id() != change) {
                    throw std::runtime_error("buffer changed while checking spelling");
                }
                if (whole_file) filter_spelling_contexts(*issues, eligible);
                else {
                    std::erase_if(*issues, [cursor](const SpellingIssue& issue) {
                        return cursor < issue.offset || cursor - issue.offset >= issue.word.size();
                    });
                }
                spelling_issues = std::move(*issues);
                spelling_index = 0;
                show_next_spelling_issue();
            }, Mode::editor);
    }

    void show_spelling_languages() {
        auto languages = std::make_shared<std::vector<std::string>>();
        const auto directory = editor_directory(editor);
        start_local_task("Loading spelling languages",
            [languages, directory](const Cancellation& cancellation) {
                *languages = spelling_languages(directory, cancellation);
                return CommandOutput{};
            }, [this, languages, directory](CommandOutput) {
                show_local_list("Select language", *languages,
                    [this, languages, directory](const std::size_t index) {
                        const auto language = languages->at(index);
                        start_local_task("Changing spelling language",
                            [language, directory](const Cancellation& cancellation) {
                                static_cast<void>(check_spelling("", language, directory, cancellation));
                                return CommandOutput{};
                            }, [this, language](CommandOutput) {
                                config.editor_spelling_language = language;
                                status = "Spelling language: " + language;
                            }, Mode::editor);
                    }, Mode::editor);
                local_list_native = true;
                const auto current = std::ranges::find(*languages, config.editor_spelling_language);
                if (current != languages->end()) {
                    local_list_cursor = static_cast<std::size_t>(current - languages->begin());
                }
            }, Mode::editor);
    }

    void spelling_action(const std::size_t action) {
        if (action == 3U) {
            finish_spelling(true);
            return;
        }
        const auto issue = spelling_issues.at(spelling_index);
        if (action == 0U) {
            const auto text = editor.text();
            const auto path = editor.path();
            const auto change = editor.change_id();
            const auto language = config.editor_spelling_language;
            const auto directory = editor_directory(editor);
            const auto resume = issue.offset + issue.word.size();
            const auto whole_file = spelling_whole_file;
            const auto eligible = spelling_context_ranges(whole_file);
            auto remaining = std::make_shared<std::vector<SpellingIssue>>();
            start_local_task("Adding dictionary word",
                [issue, text, language, directory, resume, whole_file, remaining](const Cancellation& cancellation) {
                    add_spelling_word(issue.word, language, directory, cancellation);
                    if (whole_file) *remaining = check_spelling(std::string_view(text).substr(resume),
                                                               language, directory, cancellation);
                    return CommandOutput{};
                }, [this, remaining, path, change, resume, eligible](CommandOutput) {
                    if (editor.path() != path || editor.change_id() != change) {
                        throw std::runtime_error("buffer changed while saving the dictionary word");
                    }
                    for (auto& next : *remaining) next.offset += resume;
                    filter_spelling_contexts(*remaining, eligible);
                    spelling_issues = std::move(*remaining);
                    spelling_index = 0;
                    show_next_spelling_issue();
                }, Mode::editor_spelling_dialog);
            return;
        }
        if (action == 1U) {
            if (spelling_suggestion >= issue.suggestions.size()) {
                throw std::runtime_error("no replacement suggestion is selected");
            }
            const auto& replacement = issue.suggestions[spelling_suggestion];
            editor.replace_text(editor.position(issue.offset),
                                editor.position(issue.offset + issue.word.size()), replacement);
            for (auto index = spelling_index + 1U; index < spelling_issues.size(); ++index) {
                if (replacement.size() >= issue.word.size()) {
                    spelling_issues[index].offset += replacement.size() - issue.word.size();
                } else {
                    spelling_issues[index].offset -= issue.word.size() - replacement.size();
                }
            }
            if (spelling_whole_file) {
                filter_spelling_contexts(spelling_issues, spelling_context_ranges(true), spelling_index + 1U);
            }
        }
        ++spelling_index;
        show_next_spelling_issue();
    }

    ftxui::Element render_spelling() {
        using namespace ftxui;
        const auto& issue = spelling_issues.at(spelling_index);
        const bool compact = Terminal::Size().dimy < 15;
        spelling_suggestion_boxes.assign(issue.suggestions.size(), {});
        Elements suggestions;
        for (std::size_t index = 0; index < issue.suggestions.size(); ++index) {
            auto row = text(display_text(issue.suggestions[index])) | reflect(spelling_suggestion_boxes[index]);
            if (index == spelling_suggestion) {
                row = focus_dialog(std::move(row));
            }
            suggestions.push_back(std::move(row));
        }
        if (suggestions.empty()) suggestions.push_back(text("(no suggestions)") | dim);
        const auto list = window(text("Suggest"), vbox(std::move(suggestions)) | yframe |
            size(HEIGHT, LESS_THAN, std::max(1, std::min(7, Terminal::Size().dimy - 11))) |
            reflect(spelling_list_box), LIGHT) | size(WIDTH, EQUAL, std::max(10, std::min(25, Terminal::Size().dimx - 21)));
        static const std::array<std::string, 4> labels{"Add word", "Replace", "Skip", "Cancel"};
        Elements buttons;
        for (std::size_t index = 0; index < labels.size(); ++index) {
            buttons.push_back(native_button(labels[index], false, spelling_focus == index + 1U,
                                            spelling_button_boxes[index]));
            if (!compact && index + 1U < labels.size()) buttons.push_back(text(""));
        }
        Elements rows{
            text(" Language: " + (config.editor_spelling_language.empty() ? std::string("Default")
                                                                         : config.editor_spelling_language)),
        };
        if (!compact) rows.push_back(text(""));
        rows.push_back(text(" Misspelled: " + display_text(issue.word)));
        rows.push_back(hbox({list, text(" "), vbox(std::move(buttons))}));
        if (!spelling_error.empty()) rows.push_back(paragraph(display_text(spelling_error)) | bold);
        return native_window("Check word", vbox(std::move(rows)), 46);
    }

    bool handle_spelling(const ftxui::Event& event) try {
        using namespace ftxui;
        if (event == Event::Escape || event == Event::F10) {
            finish_spelling(true);
            return true;
        }
        if (event == Event::Tab || event == Event::TabReverse) {
            spelling_focus = (spelling_focus + (event == Event::Tab ? 1U : 4U)) % 5U;
            return true;
        }
        std::size_t button = spelling_focus == 0U ? 1U : spelling_focus - 1U;
        if (event.is_mouse()) {
            auto copy = event;
            const auto& mouse = copy.mouse();
            if (mouse.button == Mouse::Left && mouse.motion == Mouse::Pressed) {
                for (std::size_t index = 0; index < spelling_button_boxes.size(); ++index) {
                    if (spelling_button_boxes[index].Contain(mouse.x, mouse.y)) {
                        spelling_action(index);
                        return true;
                    }
                }
            }
        }
        if (spelling_focus == 0U || event.is_mouse()) {
            const auto& issue = spelling_issues.at(spelling_index);
            const auto input = handle_list_input(event, spelling_suggestion, issue.suggestions.size(),
                                                 spelling_suggestion_boxes, spelling_list_box);
            if (input == ButtonInput::activate) spelling_action(1);
            if (input == ButtonInput::changed || input == ButtonInput::activate) {
                spelling_focus = 0;
                return true;
            }
        }
        const auto input = handle_button_input(event, button, spelling_button_boxes, "arsc");
        if (input == ButtonInput::changed) spelling_focus = button + 1U;
        else if (input == ButtonInput::activate) spelling_action(button);
        else if (input == ButtonInput::cancel) finish_spelling(true);
        return true;
    } catch (const std::exception& error) {
        spelling_error = error.what();
        return true;
    }

    void open_config_editor(const std::filesystem::path& path, const std::string& starter = {}) {
        const bool exists = local->exists(path.string());
        if (std::filesystem::create_directories(path.parent_path()) &&
            chmod(path.parent_path().c_str(), 0700) != 0) {
            throw std::system_error(errno, std::generic_category(), "protect configuration directory");
        }
        if (mode != Mode::editor && mode != Mode::editor_menu) editor_return_mode = Mode::panels;
        open_editor_buffer(local, path.string(), !exists, false, [this, exists, starter] {
            if (!exists && !starter.empty()) editor.set_text(starter);
        });
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

    void refresh(const bool notify_tree = false, const bool force = false) {
        left.clear_directory_sizes();
        right.clear_directory_sizes();
        left.refresh({}, force);
        right.refresh({}, force);
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

    bool tree_context_active() const { return modal_tree_snapshot.has_value() || tree_panel.has_value(); }

    int tree_context_index() const { return modal_tree_snapshot ? modal_tree_source : *tree_panel; }

    void restore_tree_selection(std::string path) {
        if (!tree_context_active() || tree_nodes.empty()) {
            tree_cursor = 0;
            return;
        }
        const auto provider =
            (tree_context_index() == 0 ? left : right).provider();
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
        if (!tree_context_active() || tree_nodes.empty()) {
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
            (tree_context_index() == 0 ? left : right).provider();
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
        if (!tree_context_active() || tree_cursor >= tree_nodes.size()) {
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

    void finish_tree_loading(const std::uint64_t load_id) {
        if (load_id != tree_load_id) return;
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
        known_tree_directories.insert(root);
        known_tree_directories.insert(ancestry.begin(), ancestry.end());
        tree_cache_used = true;
        std::set<std::string> expanded(ancestry.begin(), ancestry.end());
        expanded.insert(root);
        expanded.erase(pane.path());
        std::map<std::string, std::vector<std::string>> children;
        for (const auto& path : known_tree_directories) {
            if (path == root) continue;
            children[std::filesystem::path(path).parent_path().string()].push_back(path);
        }
        std::vector<TreeNode> nodes;
        const std::function<void(const std::string&, std::size_t)> append =
            [&](const std::string& path, const std::size_t depth) {
                const auto found = children.find(path);
                const bool has_children = found != children.end() && !found->second.empty();
                if (path == pane.path()) selected = nodes.size();
                nodes.push_back({.path=path, .depth=depth, .expanded=expanded.contains(path), .has_children=has_children});
                if (expanded.contains(path) && has_children) {
                    for (const auto& child : found->second) append(child, depth + 1U);
                }
            };
        selected = 0;
        append(root, 0);
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
        if (!tree_context_active() || index >= tree_nodes.size()) {
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
        const auto panel_index = tree_context_index();
        const auto provider =
            (panel_index == 0 ? left : right).provider();
        const auto path = tree_nodes[index].path;
        const auto depth = tree_nodes[index].depth + 1U;
        const auto generation = tree_generation;
        const auto load_id = ++tree_load_id;
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
                load_id,
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
                        load_id,
                        rescan,
                        notify
                    ]() mutable {
                        if (!tree_context_active() ||
                            tree_context_index() != panel_index ||
                            tree_generation != generation) {
                            finish_tree_loading(load_id);
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
                            finish_tree_loading(load_id);
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
                        std::set<std::string> actual_children;
                        for (const auto& child : children) actual_children.insert(child.path);
                        std::vector<std::string> removed;
                        for (const auto& cached : known_tree_directories) {
                            if (std::filesystem::path(cached).parent_path() == path && !actual_children.contains(cached)) {
                                removed.push_back(cached);
                            }
                        }
                        std::erase_if(known_tree_directories, [&](const std::string& cached) {
                            return std::ranges::any_of(removed, [&](const std::string& branch) {
                                return cached == branch || cached.starts_with(branch + "/");
                            });
                        });
                        known_tree_directories.insert(actual_children.begin(), actual_children.end());
                        known_tree_directories.insert(path);
                        tree_cache_used = true;
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
                        if (modal_tree_snapshot && tree_search_mode && modal_tree_search_pending) {
                            modal_tree_search_pending = !select_modal_tree_search();
                        }
                        if (notify) {
                            status = rescan ? "Tree refreshed" : "Ready";
                        }
                        finish_tree_loading(load_id);
                    });
                } catch (const OperationCancelled&) {
                    post_ui([this, load_id] {
                        finish_tree_loading(load_id);
                    });
                } catch (const std::exception& error) {
                    const auto message = std::string(error.what());
                    post_ui([this, path, panel_index, generation, load_id, notify, message] {
                        if (!tree_context_active() ||
                            tree_context_index() != panel_index ||
                            tree_generation != generation) {
                            finish_tree_loading(load_id);
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
                        finish_tree_loading(load_id);
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
        if (operation_draft) return operation_draft->sources;
        if (tree_operation_source) {
            return {*tree_operation_source};
        }
        return active_index == 0
                   ? left.selected_entries()
                   : right.selected_entries();
    }

    std::shared_ptr<OperationJob> capture_operation_job() const {
        auto job = std::make_shared<OperationJob>();
        job->sources = current_operation_sources();
        job->source_side = tree_operation_source ? tree_operation_source_index : active_index;
        const auto& pane = job->source_side == 0 ? left : right;
        job->source_provider = tree_operation_source ? tree_operation_source_provider : pane.provider();
        job->source_pane_id = pane_ids[job->source_side];
        job->source_directory = pane.path();
        job->source_base_directory = pane.navigation_path();
        job->destination_provider = tree_operation_source
            ? tree_operation_destination_provider : (active_index == 0 ? right : left).provider();
        return job;
    }

    std::vector<Entry> matching_operation_sources() const {
        auto sources = current_operation_sources();
        const CopyMask mask(dialog_source_mask, dialog_shell_patterns);
        std::erase_if(
            sources,
            [&](const Entry& entry) {
                return !mask.matches(entry.name);
            }
        );
        return sources;
    }

    void clear_tree_operation_context() {
        operation_draft.reset();
        tree_operation_source.reset();
        tree_operation_source_provider.reset();
        tree_operation_destination_provider.reset();
    }

    bool prepare_tree_operation(
        const bool move_operation,
        const bool remove_operation
    ) {
        if (!tree_context_active() || tree_cursor >= tree_nodes.size()) {
            return false;
        }
        if (tree_cursor == 0U) {
            status = "Cannot operate on the tree root";
            return false;
        }
        const auto panel_index = tree_context_index();
        auto& pane = panel_index == 0 ? left : right;
        tree_operation_source = pane.provider()->stat(
            tree_nodes[tree_cursor].path,
            {}
        );
        tree_operation_source_provider = pane.provider();
        tree_operation_destination_provider = modal_tree_snapshot ? passive().provider() : active().provider();
        tree_operation_source_index = panel_index;
        operation_draft.reset();
        operation_draft = capture_operation_job();
        if (remove_operation) {
            dialog_focus = config.safe_delete ? 1U : 0U;
            mode = Mode::delete_dialog;
            return true;
        }
        dialog_move = move_operation;
        dialog_exact = move_operation;
        dialog_destination = modal_tree_snapshot ? pane_location(passive()) : pane_location(active());
        if (!dialog_destination.ends_with('/')) dialog_destination += '/';
        dialog_shell_patterns = config.shell_patterns;
        dialog_source_mask = dialog_shell_patterns ? "*" : "^(.*)$";
        dialog_destination_mask.reset();
        operation_input_error.clear();
        bind_input_history(destination_input, move_operation ? "file.move-destination" : "file.copy-destination");
        destination_input.reset(dialog_destination, true);
        mask_input.reset(dialog_source_mask, true);
        dialog_preserve_attributes = true;
        dialog_preserve_symlinks = true;
        dialog_stable_symlinks = false;
        dialog_dive_into_subdirectories = false;
        dialog_background = false;
        dialog_focus = 2;
        mode = Mode::operation_dialog;
        return true;
    }

    void sync_dynamic_tree() {
        if (modal_tree_snapshot || !tree_dynamic ||
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
        if (!config.simple_swap && !quick_view_panel && !information_panel && !tree_panel) {
            left.swap_contents(right);
        } else {
            std::swap(left, right);
            std::swap(config.panel_listing[0], config.panel_listing[1]);
            std::swap(config.panel_columns[0], config.panel_columns[1]);
            std::swap(config.panel_brief_columns[0], config.panel_brief_columns[1]);
            std::swap(config.panel_user_mini_status[0], config.panel_user_mini_status[1]);
            std::swap(config.panel_status_formats[0], config.panel_status_formats[1]);
            std::swap(config.panel_encoding[0], config.panel_encoding[1]);
        }
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
        config.command_history = command_history;
        config.show_hidden = left.show_hidden();
        config.directories_first = left.directories_first();
        config.sort = left.sort_field();
        config.sort_descending = left.descending();
        for (std::size_t index = 0; index < 2U; ++index) {
            const auto& pane = index == 0U ? left : right;
            const auto id = index;
            config.panel_sort[id] = pane.sort_field();
            config.panel_descending[id] = pane.descending();
            config.panel_hidden[id] = pane.show_hidden();
            config.panel_directories_first[id] = pane.directories_first();
            config.panel_case_sensitive[id] = pane.sort_case_sensitive();
            config.panel_executable_first[id] = pane.executable_first();
        }
        if (config.auto_save_panels_setup) {
            config.saved_left_path = pane_location(left);
            config.saved_right_path = pane_location(right);
        }
        if (config.current_is_left || config.other_directory) {
            config.current_is_left = active_index == 0;
            config.other_directory = pane_location(active_index == 0 ? right : left);
        }
    }

    EditorBehavior editor_behavior() const {
        return {
            .fake_half_tabs=config.editor_fake_half_tabs,
            .backspace_through_tabs=config.editor_backspace_through_tabs,
            .fill_tabs_with_spaces=config.editor_fill_tabs_with_spaces,
            .persistent_selection=config.editor_persistent_selection,
            .cursor_beyond_eol=config.editor_cursor_beyond_eol,
            .group_undo=config.editor_group_undo,
            .wrap=static_cast<EditorWrapMode>(config.editor_wrap_mode),
            .wrap_length=config.editor_wrap_length
        };
    }

    void store_editor_options_in_config() {
        config.editor_syntax = editor_syntax;
        config.editor_line_numbers = editor_line_numbers;
        config.editor_tab_width = editor.tab_width();
        config.editor_indent_width = editor_indent_width;
        config.editor_auto_indent = editor_auto_indent;
        config.editor_show_whitespace = config.editor_visible_tabs && config.editor_visible_trailing_spaces;
        config.editor_history = editor_history;
        remember_editor_position(editor);
        for (const auto& buffer : editor_buffers) remember_editor_position(buffer);
    }

    void remember_editor_position(const EditorBuffer& buffer) {
        if (!buffer.open() || buffer.path().empty() || !config.editor_save_position) return;
        config.editor_positions[editor_location(buffer)] = {
            buffer.row(), buffer.column(), {buffer.bookmarks().begin(), buffer.bookmarks().end()}
        };
    }

    void show_local_list(
        std::string title, std::vector<std::string> items,
        std::function<void(std::size_t)> activate,
        const Mode return_mode = Mode::panels
    ) {
        local_list_title = std::move(title);
        local_list_items = std::move(items);
        local_list_cursor = 0;
        local_list_activate = std::move(activate);
        local_list_return = return_mode;
        local_list_native = false;
        local_list_hotkeys.clear();
        local_list_completion = false;
        local_history_kind = 0;
        local_history_input = nullptr;
        local_history_value = nullptr;
        local_directory.reset();
        mode = Mode::local_list;
    }

    ftxui::Element render_local_list() {
        using namespace ftxui;
        Elements rows;
        local_list_boxes.assign(local_list_items.size(), {});
        for (std::size_t index = 0; index < local_list_items.size(); ++index) {
            auto row = text(middle_truncate(display_text(local_list_items[index]),
                static_cast<std::size_t>(std::max(1, std::min(90, Terminal::Size().dimx - 4)))));
            if (index == local_list_cursor) row = focus_dialog(std::move(row));
            rows.push_back(std::move(row) | reflect(local_list_boxes[index]));
        }
        if (rows.empty()) rows.push_back(text("<empty>") | dim);
        if (local_list_native) {
            return native_window(local_list_title,
                vbox(std::move(rows)) | yframe |
                    size(HEIGHT, LESS_THAN, std::max(1, Terminal::Size().dimy - 6)) |
                    reflect(local_list_box),
                std::min(44, std::max(1, Terminal::Size().dimx - 2)));
        }
        return gray_dialog(vbox({
            text(local_list_title) | bold | center,
            separator(),
            vbox(std::move(rows)) | yframe |
                size(HEIGHT, LESS_THAN, std::max(1, Terminal::Size().dimy - 6)) |
                reflect(local_list_box),
            separator(),
            text(local_directory
                     ? "Left: parent  Right: open  Enter: choose  Esc: cancel"
                     : "Enter/double-click: select   Esc: cancel"),
        }) | borderStyled(LIGHT) | size(WIDTH, LESS_THAN, std::min(94, Terminal::Size().dimx)));
    }

    bool handle_local_list(const ftxui::Event& event) {
        if (local_list_completion && (event.is_character() || event == ftxui::Event::Backspace)) {
            auto prefix = completion_filter;
            if (event == ftxui::Event::Backspace) erase_last_utf8(prefix);
            else prefix += event.character();
            std::vector<std::string> filtered;
            for (const auto& candidate : completion_choices) {
                if (candidate.starts_with(prefix)) filtered.push_back(candidate);
            }
            if (!filtered.empty()) {
                completion_filter = std::move(prefix);
                local_list_items = std::move(filtered);
                local_list_cursor = 0;
            }
            return true;
        }
        if (!local_list_hotkeys.empty() && event.is_character() && event.character().size() == 1U) {
            const auto selected = local_list_hotkeys.find(event.character()[0]);
            if (selected != std::string::npos && selected < local_list_items.size()) {
                local_list_cursor = selected;
                return handle_local_list(ftxui::Event::Return);
            }
        }
        if (local_history_kind != 0 &&
            (event == ftxui::Event::Delete || event == ftxui::Event::Character('d'))) {
            delete_history_item();
            return true;
        }
        if (local_history_kind != 0 &&
            (event == ftxui::Event::Special("\x1B[3;2~") || event == ftxui::Event::Character('D'))) {
            if (config.confirm_history_cleanup) {
                show_native_query("History cleanup", "Do you want clean this history?", {"Yes", "No"},
                    [this](const std::size_t selected) { if (selected == 0U) clear_history_items(); },
                    Mode::local_list);
            } else clear_history_items();
            return true;
        }
        if (local_directory &&
            (event == ftxui::Event::ArrowLeft || event == ftxui::Event::Backspace)) {
            show_directory_chooser(local->parent(*local_directory));
            return true;
        }
        if (local_directory && event == ftxui::Event::ArrowRight &&
            local_list_cursor < local_directory_choices.size()) {
            show_directory_chooser(local_directory_choices[local_list_cursor]);
            return true;
        }
        const auto input = handle_list_input(
            event, local_list_cursor, local_list_items.size(), local_list_boxes, local_list_box
        );
        if (input == ButtonInput::cancel) {
            mode = local_list_return;
        } else if (input == ButtonInput::activate && local_list_cursor < local_list_items.size()) {
            const auto activate = local_list_activate;
            const auto selected = local_list_cursor;
            mode = local_list_return;
            try {
                activate(selected);
            } catch (const std::exception& error) {
                if (local_list_return == Mode::editor) show_editor_error(error);
                else set_error(error);
            }
        }
        return input != ButtonInput::ignored;
    }

    void show_directory_chooser(const std::string& directory) {
        if (modal_tree_snapshot) throw std::runtime_error("directory Tree is already open");
        invalidate_tree_loading();
        tree_refresh_pending.reset();
        tree_refresh_notify_pending = false;
        if (tree_thread.joinable()) tree_thread.join();
        tree_expansion_running.store(false, std::memory_order_release);
        ++tree_load_id;
        std::size_t selected = 0;
        std::vector<TreeNode> nodes;
        if (directory == active().path()) nodes = build_tree_nodes(active(), selected);
        else {
            Pane origin(local, directory);
            nodes = build_tree_nodes(origin, selected);
        }
        modal_tree_snapshot.emplace(TreeSnapshot{
            std::move(tree_nodes), tree_cursor, tree_search_mode, std::move(tree_search), tree_dynamic
        });
        modal_tree_source = active_index;
        tree_nodes = std::move(nodes);
        tree_cursor = selected;
        tree_search_mode = false;
        tree_search.clear();
        modal_tree_search_pending = false;
        tree_dynamic = false;
        mode = Mode::directory_tree_dialog;
        expand_tree_node(tree_cursor, false, false);
    }

    void close_directory_chooser(const bool accept) {
        if (!modal_tree_snapshot) throw std::runtime_error("directory Tree context is missing");
        if (accept) {
            if (tree_cursor >= tree_nodes.size()) throw std::runtime_error("no directory is selected");
            (modal_tree_source == 0 ? left : right).change_directory(tree_nodes[tree_cursor].path);
            active_index = modal_tree_source;
        }
        invalidate_tree_loading();
        tree_refresh_pending.reset();
        tree_refresh_notify_pending = false;
        if (tree_thread.joinable()) tree_thread.join();
        tree_expansion_running.store(false, std::memory_order_release);
        ++tree_load_id;
        auto saved = std::move(*modal_tree_snapshot);
        modal_tree_snapshot.reset();
        modal_tree_search_pending = false;
        tree_nodes = std::move(saved.nodes);
        tree_cursor = saved.cursor;
        tree_search_mode = saved.searching;
        tree_search = std::move(saved.search);
        tree_dynamic = saved.dynamic;
        modal_tree_rows.clear();
        modal_tree_paths.clear();
        if (accept) {
            tree_panel_focused = quick_view_focused = information_panel_focused = false;
        }
        mode = Mode::panels;
        status = accept ? "Directory changed" : "Directory Tree cancelled";
    }

    void forget_tree_branch(const std::size_t index) {
        if (index == 0U || index >= tree_nodes.size()) {
            status = "The root directory cannot be forgotten";
            return;
        }
        const auto path = tree_nodes[index].path;
        const auto depth = tree_nodes[index].depth;
        invalidate_tree_loading();
        tree_refresh_pending.reset();
        tree_refresh_notify_pending = false;
        auto end = index + 1U;
        while (end < tree_nodes.size() && tree_nodes[end].depth > depth) ++end;
        tree_nodes.erase(tree_nodes.begin() + static_cast<std::ptrdiff_t>(index),
                         tree_nodes.begin() + static_cast<std::ptrdiff_t>(end));
        std::erase_if(known_tree_directories, [&](const std::string& cached) {
            return cached == path || cached.starts_with(path + "/");
        });
        tree_cache_used = true;
        tree_cursor = std::min(index, tree_nodes.empty() ? 0U : tree_nodes.size() - 1U);
        status = "Tree branch forgotten; files were not removed";
    }

    bool select_modal_tree_search() {
        for (std::size_t step = 0; !tree_search.empty() && step < tree_nodes.size(); ++step) {
            const auto index = (tree_cursor + step) % tree_nodes.size();
            const auto name = std::filesystem::path(tree_nodes[index].path).filename().string();
            if (EditorSearch::find(name, tree_search, 0, 0, name.size(),
                                  {.case_sensitive=false}, false, false)) {
                tree_cursor = index;
                return true;
            }
        }
        return false;
    }

    bool handle_directory_chooser(const ftxui::Event& event) try {
        using namespace ftxui;
        const auto changed = [&] { load_tree_surroundings(); };
        if (event == Event::Escape && tree_search_mode) {
            tree_search_mode = false;
            tree_search.clear();
            modal_tree_search_pending = false;
            return true;
        }
        if (modal_cancel(event)) { close_directory_chooser(false); return true; }
        if (event == Event::Return) { close_directory_chooser(true); return true; }
        if (event == Event::F2 || event == Event::CtrlR) { refresh_tree(true); return true; }
        if (event == Event::F3) { forget_tree_branch(tree_cursor); return true; }
        if (event == Event::F4) { tree_dynamic = !tree_dynamic; return true; }
        if (event == Event::F5 || event == Event::F6 || event == Event::F8) {
            if (prepare_tree_operation(event == Event::F6, event == Event::F8) &&
                event == Event::F8 && !config.confirm_delete) run_remove_operation();
            return true;
        }
        if (event == Event::CtrlS || event == Event::AltS) {
            if (!tree_search_mode) tree_search.clear();
            tree_search_mode = true;
            return true;
        }
        if (tree_search_mode && (event.is_character() || event == Event::Backspace)) {
            if (event == Event::Backspace) erase_last_utf8(tree_search);
            else tree_search += event.character();
            modal_tree_search_pending = !tree_search.empty() && !select_modal_tree_search();
            if (!tree_search.empty() && !modal_tree_search_pending) changed();
            return true;
        }
        modal_tree_search_pending = false;
        if (event == Event::ArrowRight) {
            if (!tree_nodes[tree_cursor].expanded) expand_tree_node(tree_cursor);
            else if (tree_cursor + 1U < tree_nodes.size() && tree_nodes[tree_cursor + 1U].depth > tree_nodes[tree_cursor].depth) {
                ++tree_cursor;
                changed();
            }
            return true;
        }
        if (event == Event::ArrowLeft) {
            if (tree_nodes[tree_cursor].expanded) collapse_tree_node(tree_cursor);
            else if (tree_cursor > 0U) {
                const auto depth = tree_nodes[tree_cursor].depth;
                do { --tree_cursor; } while (tree_cursor > 0U && tree_nodes[tree_cursor].depth >= depth);
                changed();
            }
            return true;
        }
        if (event == Event::Home) tree_cursor = 0;
        else if (event == Event::End) tree_cursor = tree_nodes.empty() ? 0U : tree_nodes.size() - 1U;
        else if (event == Event::ArrowUp || event == Event::ArrowDown || event == Event::PageUp || event == Event::PageDown) {
            const bool forward = event == Event::ArrowDown || event == Event::PageDown;
            const auto count = event == Event::PageUp || event == Event::PageDown
                ? static_cast<std::size_t>(std::max(1, Terminal::Size().dimy - 16)) : 1U;
            for (std::size_t step = 0; step < count; ++step) {
                if (!tree_dynamic) {
                    if (forward && tree_cursor + 1U < tree_nodes.size()) ++tree_cursor;
                    else if (!forward && tree_cursor > 0U) --tree_cursor;
                } else {
                    const auto depth = tree_nodes[tree_cursor].depth;
                    auto next = tree_cursor;
                    while ((forward && next + 1U < tree_nodes.size()) || (!forward && next > 0U)) {
                        next = forward ? next + 1U : next - 1U;
                        if (tree_nodes[next].depth < depth) break;
                        if (tree_nodes[next].depth == depth) { tree_cursor = next; break; }
                    }
                }
            }
        } else if (event.is_mouse()) {
            const auto input = handle_list_input(event, tree_cursor, tree_nodes.size(), modal_tree_rows, modal_tree_viewport);
            if (input == ButtonInput::activate) { close_directory_chooser(true); return true; }
        } else return false;
        changed();
        return true;
    } catch (const std::exception& error) {
        show_native_message("Directory tree", error.what(), Mode::directory_tree_dialog);
        return true;
    }

    void show_directory_sizes() {
        const bool marked_directories = std::ranges::any_of(active().entries(), [this](const Entry& entry) {
            return entry.kind == EntryKind::directory && entry.revision != "mcpp:parent" && active().is_selected(entry);
        });
        std::vector<std::string> directories;
        for (const auto& entry : active().entries()) {
            if (entry.kind == EntryKind::directory && entry.revision != "mcpp:parent" &&
                (!marked_directories || active().is_selected(entry))) directories.push_back(entry.path);
        }
        if (directories.empty()) {
            status = "No directories to scan";
            mode = Mode::panels;
            return;
        }
        const auto panel = active_index;
        const auto provider = active().provider();
        auto sizes = std::make_shared<std::map<std::string, std::uint64_t>>();
        start_local_task("Directory scanning",
                [directories, sizes, provider](const Cancellation& cancellation) {
                    for (const auto& directory : directories) {
                        std::uint64_t total = 0;
                        std::vector<std::string> pending{directory};
                        while (!pending.empty()) {
                            cancellation.throw_if_requested();
                            auto current = std::move(pending.back());
                            pending.pop_back();
                            for (const auto& entry : provider->list(current, {.show_hidden=true}, cancellation)) {
                                if (entry.kind == EntryKind::directory) pending.push_back(entry.path);
                                else if (entry.kind == EntryKind::file) {
                                    if (entry.size > std::numeric_limits<std::uint64_t>::max() - total) {
                                        throw std::runtime_error("directory size overflow");
                                    }
                                    total += entry.size;
                                }
                            }
                        }
                        (*sizes)[directory] = total;
                    }
                    return CommandOutput{};
                },
                [this, panel, sizes](CommandOutput) {
                    (panel == 0 ? left : right).set_directory_sizes(*sizes);
                    status = "Directory sizes calculated";
                });
    }

    void show_command_history() {
        auto history = command_history;
        std::reverse(history.begin(), history.end());
        show_local_list("History", history, [this](const std::size_t index) {
            command_line = local_list_items.at(index);
            command_prompt_override = !config.command_prompt;
            command_input.reset(command_line);
            command_mode = true;
        });
        local_list_native = true;
        local_history_kind = 1;
    }

    void show_file_history() {
        std::vector<std::string> labels;
        std::vector<std::pair<bool, std::string>> files;
        for (const auto& path : editor_history) {
            labels.push_back("[Edit] " + path);
            files.emplace_back(true, path);
        }
        for (const auto& path : config.viewer_history) {
            labels.push_back("[View] " + path);
            files.emplace_back(false, path);
        }
        show_local_list("History", std::move(labels),
            [this](const std::size_t index) {
                const auto& [edit, path] = local_history_files.at(index);
                if (edit) {
                    const auto location = resolve_editor_location(path);
                    open_editor_buffer(location.provider, location.path, false, true);
                } else {
                    const auto location = resolve_location(path);
                    load_viewer(location.provider, location.path);
                    set_viewer_return_mode(Mode::panels);
                    mode = Mode::viewer;
                }
            });
        local_history_files = std::move(files);
        local_list_native = true;
        local_history_kind = 2;
    }

    void refresh_input_history_list() {
        auto& input = *local_history_input;
        input.history = input_histories.history(input.history_id);
        input.history_index = input.history.size();
        input.draft = *local_history_value;
        input.draft_cursor = input.cursor;
        input.draft_anchor = input.anchor;
        local_list_items = input.history;
        std::reverse(local_list_items.begin(), local_list_items.end());
        local_list_cursor = local_list_items.empty()
                                ? 0U : std::min(local_list_cursor, local_list_items.size() - 1U);
    }

    void delete_history_item() {
        if (local_list_cursor >= local_list_items.size()) return;
        if (local_history_kind == 3) {
            try {
                input_histories.remove(local_history_input->history_id, local_list_items[local_list_cursor]);
                refresh_input_history_list();
            } catch (const std::exception& error) {
                show_native_message("Input history", error.what(), Mode::local_list);
            }
            return;
        } else if (local_history_kind == 1) {
            const auto& value = local_list_items[local_list_cursor];
            const auto found = std::find(command_history.rbegin(), command_history.rend(), value);
            if (found != command_history.rend()) command_history.erase(std::next(found).base());
            command_input.history = command_history;
            command_input.history_index = command_history.size();
        } else {
            const auto& [edit, path] = local_history_files.at(local_list_cursor);
            if (edit) std::erase(editor_history, path);
            else std::erase(config.viewer_history, path);
            local_history_files.erase(local_history_files.begin() + static_cast<std::ptrdiff_t>(local_list_cursor));
        }
        local_list_items.erase(local_list_items.begin() + static_cast<std::ptrdiff_t>(local_list_cursor));
        if (local_list_cursor >= local_list_items.size() && local_list_cursor > 0U) --local_list_cursor;
    }

    void clear_history_items() {
        if (local_history_kind == 3) {
            try {
                input_histories.clear(local_history_input->history_id);
                refresh_input_history_list();
            } catch (const std::exception& error) {
                show_native_message("Input history", error.what(), Mode::local_list);
            }
            return;
        } else if (local_history_kind == 1) {
            command_history.clear();
            command_input.history.clear();
            command_input.history_index = 0;
        } else {
            editor_history.clear();
            config.viewer_history.clear();
            local_history_files.clear();
        }
        local_list_items.clear();
        local_list_cursor = 0;
    }

    ViewerSessionState& viewer_session() { return *this; }
    DiffSessionState& diff_session() { return *this; }

    ScreenTarget screen_target(const Mode target_mode) {
        if (target_mode == Mode::viewer) return {target_mode, viewer_screen_id};
        if (target_mode == Mode::diff) return {target_mode, diff_screen_id};
        if (target_mode == Mode::editor) {
            if (editor.open() && editor.screen_id == 0U) editor.screen_id = next_screen_id++;
            return {target_mode, editor.screen_id};
        }
        return {target_mode, 0};
    }

    void set_viewer_return_mode(const Mode return_mode) {
        viewer_return_mode = return_mode;
        viewer_return_screen = screen_target(return_mode);
    }

    std::vector<ScreenTarget> screen_targets() {
        std::vector<ScreenTarget> result{{Mode::panels, 0}};
        if (viewer.open()) result.push_back(screen_target(Mode::viewer));
        for (const auto& session : inactive_viewers) result.push_back({Mode::viewer, session.viewer_screen_id});
        if (diff_screen_id != 0U) result.push_back(screen_target(Mode::diff));
        for (const auto& session : inactive_diffs) result.push_back({Mode::diff, session.diff_screen_id});
        if (editor.open()) result.push_back(screen_target(Mode::editor));
        for (auto& buffer : editor_buffers) {
            if (!buffer.open()) continue;
            if (buffer.screen_id == 0U) buffer.screen_id = next_screen_id++;
            result.push_back({Mode::editor, buffer.screen_id});
        }
        std::ranges::sort(result, {}, &ScreenTarget::id);
        return result;
    }

    bool activate_screen(const ScreenTarget target) {
        if (target.mode == Mode::viewer) {
            if (target.id == 0U) return false;
            if (target.id != viewer_screen_id) {
                const auto found = std::ranges::find(inactive_viewers, target.id, &ViewerSessionState::viewer_screen_id);
                if (found == inactive_viewers.end()) return false;
                std::swap(viewer_session(), *found);
                if (!found->viewer.open()) inactive_viewers.erase(found);
            }
            if (!viewer.open()) return false;
        } else if (target.mode == Mode::diff) {
            if (target.id == 0U) return false;
            if (target.id != diff_screen_id) {
                const auto found = std::ranges::find(inactive_diffs, target.id, &DiffSessionState::diff_screen_id);
                if (found == inactive_diffs.end()) return false;
                std::swap(diff_session(), *found);
                if (found->diff_screen_id == 0U) inactive_diffs.erase(found);
            }
        } else if (target.mode == Mode::editor) {
            if (target.id == 0U) return false;
            if (target.id != editor.screen_id) {
                const auto found = std::ranges::find(editor_buffers, target.id, &EditorBuffer::screen_id);
                if (found == editor_buffers.end()) return false;
                std::swap(editor, *found);
                remember_editor_path(editor_location(editor));
                reset_editor_view_state();
            }
            if (!editor.open()) return false;
            editor_session_base = 0;
            editor_return_mode = Mode::panels;
            exit_after_editor = false;
        }
        mode = target.mode;
        return true;
    }

    void switch_screen(const bool forward) {
        const auto current = screen_target(mode);
        const auto targets = screen_targets();
        const auto found = std::ranges::find_if(targets, [&](const ScreenTarget target) {
            return target.mode == current.mode && target.id == current.id;
        });
        const auto index = found == targets.end() ? 0U : static_cast<std::size_t>(found - targets.begin());
        const auto next = forward ? (index + 1U) % targets.size() : (index + targets.size() - 1U) % targets.size();
        static_cast<void>(activate_screen(targets[next]));
    }

    void show_screens(const Mode return_mode) {
        const auto current = screen_target(return_mode);
        const auto targets = screen_targets();
        std::vector<std::string> labels;
        std::size_t selected = 0;
        const auto label_width = static_cast<std::size_t>(std::max(1, std::min(42, ftxui::Terminal::Size().dimx - 4)));
        const auto label = [label_width](const std::string& prefix, const std::string& path) {
            return prefix + path_label(path, label_width > prefix.size() ? label_width - prefix.size() : 1U);
        };
        for (const auto target : targets) {
            if (target.mode == current.mode && target.id == current.id) selected = labels.size();
            if (target.mode == Mode::viewer) {
                const auto& session = target.id == viewer_screen_id ? viewer_session()
                    : *std::ranges::find(inactive_viewers, target.id, &ViewerSessionState::viewer_screen_id);
                const auto title = session.viewer_display_name.empty()
                    ? archives.format(session.viewer.provider(), session.viewer.path()) : session.viewer_display_name;
                labels.push_back(label(session.viewer.dirty() ? "[View*] " : "[View] ", title));
            } else if (target.mode == Mode::diff) {
                const auto& session = target.id == diff_screen_id ? diff_session()
                    : *std::ranges::find(inactive_diffs, target.id, &DiffSessionState::diff_screen_id);
                const auto dirty = session.diff_files[0].dirty() || session.diff_files[1].dirty();
                const std::string prefix = dirty ? "[Diff*] " : "[Diff] ";
                const auto paths_width = label_width > prefix.size() + 3U ? label_width - prefix.size() - 3U : 2U;
                labels.push_back(prefix + path_label(session.diff_files[0].path(), paths_width / 2U) + " : " +
                    path_label(session.diff_files[1].path(), paths_width - paths_width / 2U));
            } else if (target.mode == Mode::editor) {
                const auto& buffer = target.id == editor.screen_id ? editor
                    : *std::ranges::find(editor_buffers, target.id, &EditorBuffer::screen_id);
                labels.push_back(label(buffer.dirty() ? "[Edit*] " : "[Edit] ", buffer.path()));
            } else labels.push_back("File manager");
        }
        show_local_list("Screen list", std::move(labels), [this, targets](const std::size_t index) {
            static_cast<void>(activate_screen(targets.at(index)));
        }, return_mode);
        local_list_cursor = selected;
        local_list_native = true;
    }

    void install_viewer_session(ViewerSessionState next, const bool replace = false) {
        next.viewer_screen_id = replace && viewer.open() ? viewer_screen_id : next_screen_id++;
        if (!replace && viewer.open()) inactive_viewers.push_back(std::move(viewer_session()));
        viewer_session() = std::move(next);
    }

    void restore_screen_after_close(const ScreenTarget return_screen, const bool quitting = false) {
        if (quitting) {
            exit_terminal();
            return;
        }
        const auto targets = screen_targets();
        if (exit_after_viewer || exit_after_diff) {
            if (targets.size() == 1U) {
                exit_terminal();
                return;
            }
            if (return_screen.mode == Mode::panels) {
                static_cast<void>(activate_screen(targets.back()));
                return;
            }
        }
        if (!activate_screen(return_screen)) mode = Mode::panels;
    }

    bool close_dirty_screen_for_exit() {
        if (viewer.dirty()) {
            mode = Mode::viewer;
            request_viewer_close(true);
            return true;
        }
        for (const auto& session : inactive_viewers) {
            if (!session.viewer.dirty()) continue;
            const auto target = ScreenTarget{Mode::viewer, session.viewer_screen_id};
            static_cast<void>(activate_screen(target));
            request_viewer_close(true);
            return true;
        }
        if (diff_files[0].dirty() || diff_files[1].dirty()) {
            mode = Mode::diff;
            request_diff_close(true);
            return true;
        }
        for (const auto& session : inactive_diffs) {
            if (!session.diff_files[0].dirty() && !session.diff_files[1].dirty()) continue;
            const auto target = ScreenTarget{Mode::diff, session.diff_screen_id};
            static_cast<void>(activate_screen(target));
            request_diff_close(true);
            return true;
        }
        return false;
    }

    void load_viewer(const ProviderPtr& provider, const std::string& path, const bool replace = false) {
        ViewerSessionState next;
        if (replace) next = viewer_session();
        else next.viewer = viewer;
        if (!viewer.open()) next.viewer.set_wrap(config.viewer_wrap);
        next.viewer.open(provider, path);
        next.viewer_temporary_file.reset();
        next.viewer_display_name.clear();
        if (viewer_temporary_file && provider == local &&
            local->normalize(path) == local->normalize(viewer_temporary_file->path().string())) {
            next.viewer_temporary_file = viewer_temporary_file;
            next.viewer_display_name = viewer_display_name;
        }
        next.viewer_navigation_provider = provider;
        next.viewer_navigation_files.clear();
        if (next.viewer.streamed()) {
            install_viewer_session(std::move(next), replace);
            return;
        }
        const auto directory = provider->parent(path);
        try {
            const auto candidates = provider->list(directory, {.show_hidden=true, .directories_first=true}, {});
            for (const auto& entry : candidates) {
                if (can_open_file(entry)) next.viewer_navigation_files.push_back(entry.path);
            }
        } catch (const std::exception& error) {
            std::cerr << "mcpp: viewer file navigation unavailable: " << error.what() << '\n';
            next.viewer_navigation_files.push_back(path);
        }
        install_viewer_session(std::move(next), replace);
        const auto location = archives.format(provider, path);
        std::erase(config.viewer_history, location);
        config.viewer_history.insert(config.viewer_history.begin(), location);
        if (config.viewer_history.size() > 20U) config.viewer_history.resize(20U);
    }

    bool refresh_viewer() {
        const auto changed = viewer.refresh();
        if (changed && !viewer.source_error().empty() && mode == Mode::viewer) {
            viewer_message_title = "Viewer error";
            viewer_message = viewer.source_error();
            mode = Mode::viewer_message;
        }
        return changed;
    }

    void view_file(const ProviderPtr& provider, const std::string& path, const Mode return_mode) {
        if (config.use_internal_view) {
            load_viewer(provider, path);
            viewer_search_needle.clear();
            set_viewer_return_mode(return_mode);
            if (return_mode == Mode::search_results) {
                viewer_navigation_files.clear();
                for (const auto& entry : search_results) {
                    if (can_open_file(entry)) viewer_navigation_files.push_back(entry.path);
                }
            }
            mode = Mode::viewer;
            return;
        }
        if (provider != local) {
            throw std::runtime_error("copy this archive member to a local directory before using an external viewer");
        }
        const auto* pager = std::getenv("PAGER");
        const auto command = std::string(pager != nullptr && *pager != '\0' ? pager : "less") + " " +
            expand_user_menu_command("%f", path, provider->parent(path), passive().path());
        const auto result = run_process(command, false, provider->parent(path));
        mode = return_mode;
        if (result != 0) throw std::runtime_error("external viewer exited " + std::to_string(result));
        std::erase(config.viewer_history, path);
        config.viewer_history.insert(config.viewer_history.begin(), path);
        if (config.viewer_history.size() > 20U) config.viewer_history.resize(20U);
        status = "Viewer closed";
    }

    void edit_file(
        const ProviderPtr& provider, const std::string& path, const Mode return_mode,
        const bool require_existing = true
    ) {
        if (config.use_internal_edit) {
            const auto retained = editor_buffers.size() + (editor.open() ? 1U : 0U);
            const auto diff_return = return_mode == Mode::diff && diff_edit_side
                ? std::optional{std::pair{diff_screen_id, *diff_edit_side}} : std::nullopt;
            open_editor_buffer(provider, path, false, require_existing, [this, retained, return_mode, diff_return] {
                editor_session_base = return_mode == Mode::search_results || return_mode == Mode::diff ? retained : 0U;
                editor_return_mode = return_mode;
                editor.diff_screen_return = diff_return;
            });
            return;
        }
        if (provider != local) {
            throw std::runtime_error("copy archive member to a local directory before using an external editor");
        }
        const auto* external = std::getenv("EDITOR");
        const auto command = std::string(external != nullptr && *external != '\0' ? external : "vi") + " " +
            expand_user_menu_command("%f", path, provider->parent(path), passive().path());
        const auto result = run_process(command, false, provider->parent(path));
        mode = return_mode;
        if (result != 0) throw std::runtime_error("external editor exited " + std::to_string(result));
        remember_editor_path(path);
        status = "Editor closed";
    }

    void request_main_edit(const Entry* entry) {
        if (!config.ask_new_filename) {
            if (entry != nullptr && can_open_file(*entry)) edit_file_with_extensions(active().provider(), entry->path, Mode::panels);
            return;
        }
        const auto provider = active().provider();
        const auto directory = active().navigation_path();
        const auto value = entry != nullptr && can_open_file(*entry)
            ? archives.format(provider, entry->path) : std::string{};
        show_native_input("Edit file", "Enter file name:", value,
            [this, provider, directory](const std::string& name) {
               if (name.empty()) throw std::invalid_argument("file name is empty");
               const auto location = resolve_relative_location({provider, directory}, name);
               edit_file_with_extensions(location.provider, location.path, Mode::panels, false);
            }, Mode::panels, archives.format(provider, directory), "filename.edit");
    }

    std::string mkdir_default() const {
        const auto* entry = active_index == 0 ? left.current() : right.current();
        return config.mkdir_autoname && entry != nullptr && entry->revision != "mcpp:parent"
                   ? entry->name : std::string{};
    }

    std::string viewer_title() const {
        return viewer_display_name.empty() ? archives.format(viewer.provider(), viewer.path()) : viewer_display_name;
    }

    void open_output_viewer(const std::string& label, const std::string& value) {
        auto temporary = std::make_shared<TemporaryFile>(value);
        ViewerSessionState next;
        next.viewer = viewer;
        if (!viewer.open()) next.viewer.set_wrap(config.viewer_wrap);
        next.viewer.open(local, temporary->path().string());
        install_viewer_session(std::move(next));
        viewer_temporary_file = std::move(temporary);
        viewer_display_name = label;
        set_viewer_return_mode(Mode::panels);
        viewer_search_needle.clear();
        viewer_navigation_provider = local;
        viewer_navigation_files.clear();
        viewer_switch_pending.reset();
        mode = Mode::viewer;
    }

    static void require_command_success(const CommandOutput& result, const bool difference_ok = false) {
        if (result.status != 0 && !(difference_ok && result.status == 1)) {
            throw std::runtime_error("command exited " + std::to_string(result.status) +
                                     (result.error.empty() ? "" : ": " + result.error));
        }
    }

    void show_command_warnings(const std::string& warning) {
        if (warning.empty()) return;
        if (mode == Mode::viewer) {
            viewer_message_title = "Command diagnostics";
            viewer_message = warning;
            mode = Mode::viewer_message;
        } else if (mode == Mode::editor) {
            show_editor_message("Command diagnostics", warning);
        } else {
            status = "Command warning: " + warning;
        }
    }

    void show_filtered_view() {
        if (active().provider() != local) {
            throw std::runtime_error("copy archive members to a local directory before running a file filter");
        }
        const auto* entry = active().current();
        const auto directory = active().path();
        const auto initial = !command_line.empty() ? command_line : entry == nullptr ? std::string{}
            : expand_user_menu_command("%f", entry->path, directory, passive().path());
        show_native_input("Filtered view", "Filter command and arguments:", initial,
            [this, directory](const std::string& command) {
            if (command.empty()) throw std::runtime_error("filter command is empty");
            start_local_task("Filtered view",
                [command, directory](const Cancellation& cancellation) {
                    return capture_shell(command, directory, cancellation);
                }, [this, command](CommandOutput result) {
                    require_command_success(result);
                    open_output_viewer("Filtered: " + command, result.output);
                    show_command_warnings(result.error);
                });
        }, Mode::panels, {}, "command.filtered-view");
    }

    void show_file_comparison() {
        if (left.provider() != local || right.provider() != local) {
            throw std::runtime_error("copy archive members to local directories before opening a comparison");
        }
        const auto* first = left.current();
        const auto* second = right.current();
        if (first == nullptr || second == nullptr || !can_open_file(*first) || !can_open_file(*second)) {
            throw std::runtime_error("select a regular file in each pane");
        }
        open_file_comparison(first->path, second->path);
    }

    void open_file_comparison(const std::string& first_path, const std::string& second_path) {
        auto next = std::make_shared<DiffSessionState>();
        next->diff_files[0].open(local, first_path);
        next->diff_files[1].open(local, second_path);
        next->diff_options = {config.diff_quality, config.diff_ignore_case, config.diff_ignore_tab_expansion,
                        config.diff_ignore_space_change, config.diff_ignore_all_space, config.diff_strip_trailing_cr};
        next->diff_split = ftxui::Terminal::Size().dimx / 2;
        next->diff_tab_size = config.diff_tab_size;
        next->diff_show_symbols = config.diff_show_symbols;
        next->diff_show_numbers = config.diff_show_numbers;
        if (terminal == nullptr) {
            next->file_diff.compare(next->diff_files[0], next->diff_files[1], next->diff_options);
            install_diff_session(std::move(*next));
            return;
        }
        start_local_task("Comparing files", [next](const Cancellation& cancellation) {
            next->file_diff.compare(next->diff_files[0], next->diff_files[1], next->diff_options, cancellation);
            return CommandOutput{};
        }, [this, next](CommandOutput) {
            install_diff_session(std::move(*next));
        });
    }

    void install_diff_session(DiffSessionState next) {
        next.diff_screen_id = next_screen_id++;
        if (diff_screen_id != 0U) inactive_diffs.push_back(std::move(diff_session()));
        diff_session() = std::move(next);
        mode = Mode::diff;
    }

    std::size_t diff_visible_rows() const {
        return static_cast<std::size_t>(std::max(1, ftxui::Terminal::Size().dimy - 4));
    }

    void rebuild_diff() {
        file_diff.compare(diff_files[0], diff_files[1], diff_options);
        const auto maximum = file_diff.rows().size() > diff_visible_rows() ? file_diff.rows().size() - diff_visible_rows() : 0U;
        diff_top = std::min(diff_top, maximum);
        diff_hunk = file_diff.hunks().empty() ? 0U : std::min(diff_hunk, file_diff.hunks().size() - 1U);
        diff_search_cursor.reset();
        diff_search_match.reset();
    }

    std::string diff_line(const std::string& value) const {
        std::string text;
        std::size_t column = 0;
        for (std::size_t index = 0; index < value.size();) {
            if (value[index] == '\r' && index + 1U == value.size()) break;
            if (value[index] == '\t') {
                const auto count = diff_tab_size - column % diff_tab_size;
                text.append(count, ' ');
                column += count;
                ++index;
            } else {
                const auto end = next_utf8(value, index);
                const auto glyph = display_text(std::string_view(value).substr(index, end - index));
                text += glyph;
                column += static_cast<std::size_t>(std::max(0, ftxui::string_width(glyph)));
                index = end;
            }
        }
        return text;
    }

    ftxui::Element render_diff() {
        using namespace ftxui;
        const auto dimensions = Terminal::Size();
        const auto minimum = std::min(12, std::max(1, dimensions.dimx / 2));
        diff_split = std::clamp(diff_split, minimum, std::max(minimum, dimensions.dimx - minimum));
        Elements headers, panes;
        const auto pane = [&](const std::size_t side, const int width) {
            const auto& buffer = diff_files[side];
            const auto count = DiffModel::line_count(buffer);
            const auto status_text = side == diff_order ? " Col " + std::to_string(diff_horizontal)
                : " Dif " + std::to_string(file_diff.hunks().size());
            headers.push_back(gray_menu(hbox({
                text(std::string(buffer.dirty() ? "* " : "  ") + path_label(display_text(buffer.path()),
                    static_cast<std::size_t>(std::max(1, width - static_cast<int>(status_text.size()) - 3)))) | flex,
                text(status_text + " ")
            })) | size(WIDTH, EQUAL, width));
            Elements rows;
            for (std::size_t row = diff_top; row < file_diff.rows().size() && row - diff_top < diff_visible_rows(); ++row) {
                const auto& aligned = file_diff.rows()[row];
                const auto line = side == 0U ? aligned.left : aligned.right;
                std::string prefix;
                if (diff_show_symbols) prefix = aligned.hunk ? (line ? "|" : "-") : " ";
                if (diff_show_numbers) {
                    std::ostringstream number;
                    if (line) number << std::setw(5) << *line + 1U << ' ';
                    else number << "      ";
                    prefix += number.str();
                }
                const auto content = line && *line < count ? diff_line(buffer.lines()[*line]) : std::string{};
                const auto begin = editor_offset_for_width(content, diff_horizontal, diff_tab_size);
                auto element = text(prefix + content.substr(begin));
                if (aligned.hunk) {
                    element = paint(std::move(element), palette().diff_changed);
                    if (*aligned.hunk == diff_hunk) element = element | bold;
                }
                if (diff_search_match && diff_search_match->first == row && side == diff_order) {
                    element = highlight(std::move(element), palette().search);
                }
                rows.push_back(std::move(element) | size(WIDTH, EQUAL, std::max(1, width - 2)));
            }
            while (rows.size() < diff_visible_rows()) rows.push_back(text(""));
            return vbox(std::move(rows)) | borderStyled(LIGHT) | size(WIDTH, EQUAL, width) |
                   reflect(diff_pane_boxes[side]);
        };
        panes.push_back(pane(diff_order, diff_full ? dimensions.dimx : diff_split));
        if (!diff_full) panes.push_back(pane(diff_order ^ 1U, dimensions.dimx - diff_split));
        else diff_pane_boxes[diff_order ^ 1U] = {};
        static const std::array<std::string, 10> labels{"Help","Save","","Edit","Merge","","Search","","Options","Quit"};
        Elements buttons{text(" ")};
        for (std::size_t index = 0; index < labels.size(); ++index) {
            buttons.push_back(render_function_key(index, labels[index], mode == Mode::diff && !labels[index].empty()));
        }
        return vbox({hbox(std::move(headers)), hbox(std::move(panes)) | flex, hbox(std::move(buttons))});
    }

    void close_diff() {
        const auto return_screen = diff_return_screen;
        const auto quitting = diff_quit_after_close;
        diff_files[0].close();
        diff_files[1].close();
        diff_session() = {};
        restore_screen_after_close(return_screen, quitting);
        if (exiting || mode != Mode::panels) return;
        refresh(false, true);
    }

    void continue_diff_saves() {
        while (diff_save_index < 2U && !diff_files[diff_save_index].dirty()) ++diff_save_index;
        if (diff_save_index < 2U) begin_editor_buffer_save(diff_files[diff_save_index], EditorSavePurpose::diff_save);
        else {
            diff_saving = false;
            if (diff_closing) close_diff();
            else { mode = Mode::diff; status = "Compared files saved"; }
        }
    }

    void save_diff(const bool closing = false) {
        diff_save_index = 0;
        diff_saving = true;
        diff_closing = closing;
        continue_diff_saves();
    }

    void resume_diff_after_edit(const std::optional<Entry>& large_file_approval = std::nullopt) {
        if (diff_edit_side) {
            auto& buffer = diff_files[*diff_edit_side];
            if (buffer.dirty()) {
                diff_edit_side.reset();
                show_native_message("Diff viewer",
                    "The compared file has staged merges. Editor changes were not reloaded.", Mode::diff);
                return;
            }
            const auto path = buffer.path();
            try {
                buffer.open(local, path, {}, large_file_approval);
            } catch (const EditorLargeFileConfirmationRequired& required) {
                const auto approval = required.file();
                confirm_large_editor_file(local, approval, Mode::diff,
                    [this, approval] { resume_diff_after_edit(approval); },
                    [this] {
                        diff_edit_side.reset();
                        mode = Mode::diff;
                    });
                return;
            }
            diff_edit_side.reset();
        }
        rebuild_diff();
        mode = Mode::diff;
    }

    void request_diff_close(const bool quitting = false) {
        diff_quit_after_close = quitting;
        if (!diff_files[0].dirty() && !diff_files[1].dirty()) { close_diff(); return; }
        show_native_query("Confirm", "File(s) was modified. Save with exit?", {"Yes","No","Cancel"},
            [this](const std::size_t selected) {
                if (selected == 0U) save_diff(true);
                else if (selected == 1U) close_diff();
            }, Mode::diff);
    }

    void find_diff(const bool repeat) {
        if (diff_search_query.empty()) throw std::invalid_argument("search text is empty");
        const auto start = repeat && diff_search_cursor ? diff_search_cursor->first : diff_top;
        auto column = repeat && diff_search_cursor ? diff_search_cursor->second : 0U;
        for (std::size_t step = 0; step < file_diff.rows().size(); ++step) {
            if (diff_search_options.backwards && step > start) break;
            const auto row = diff_search_options.backwards ? start - step : start + step;
            if (row >= file_diff.rows().size()) break;
            const auto line = diff_order == 0U ? file_diff.rows()[row].left : file_diff.rows()[row].right;
            if (line) {
                const auto& text = diff_files[diff_order].lines()[*line];
                const auto origin = diff_search_options.backwards && (!repeat || step != 0U) ? text.size() : column;
                const auto found = diff_search_all_charsets
                    ? EditorSearch::find_codepages(text, diff_search_query, origin, 0, text.size(), diff_search_options, false, false)
                    : EditorSearch::find(text, diff_search_query, origin, 0, text.size(), diff_search_options, false, false);
                if (found) {
                    diff_top = row;
                    diff_horizontal = editor_display_width(text, found->begin, diff_tab_size);
                    diff_search_cursor = {{row, diff_search_options.backwards ? found->begin
                        : found->end > found->begin ? found->end : found->begin + 1U}};
                    diff_search_match = {{row, {found->begin, found->end}}};
                    mode = Mode::diff;
                    return;
                }
            }
            column = 0;
        }
        show_native_message("Search", "Search string not found", Mode::diff);
    }

    Mode file_search_return() const {
        return file_search_target == FileSearchTarget::viewer ? Mode::viewer
             : file_search_target == FileSearchTarget::quick_view ? Mode::panels : Mode::diff;
    }

    void show_file_search(const FileSearchTarget target, const std::optional<bool> backwards = std::nullopt) {
        file_search_target = target;
        file_search_value = target == FileSearchTarget::viewer ? viewer_search_needle
                          : target == FileSearchTarget::quick_view ? quick_view_search_needle : diff_search_query;
        file_search_draft = target == FileSearchTarget::viewer ? viewer_search_options
                          : target == FileSearchTarget::quick_view ? quick_view_search_options : diff_search_options;
        file_search_all_charsets = target == FileSearchTarget::viewer ? viewer_search_all_charsets
                                 : target == FileSearchTarget::quick_view ? quick_view_search_all_charsets : diff_search_all_charsets;
        if (file_search_value.empty() && target != FileSearchTarget::diff &&
            (target == FileSearchTarget::viewer ? viewer.hex() : quick_viewer.hex())) {
            file_search_draft.mode = EditorSearchMode::hexadecimal;
        }
        if (backwards) file_search_draft.backwards = *backwards;
        bind_input_history(file_search_input, target == FileSearchTarget::diff ? "search.diff" : "search.viewer");
        file_search_input.reset(file_search_value, true);
        file_search_focus = 0;
        file_search_error.clear();
        mode = Mode::file_search_dialog;
    }

    ftxui::Element render_file_search() {
        using namespace ftxui;
        static const std::array<EditorSearchMode, 4> modes{
            EditorSearchMode::normal, EditorSearchMode::regular_expression, EditorSearchMode::hexadecimal, EditorSearchMode::wildcard
        };
        static const std::array<std::string, 4> names{"Normal", "Regular expression", "Hexadecimal", "Wildcard search"};
        static const std::array<std::string, 4> flags{"Case sensitive", "Backwards", "Whole words", "All charsets"};
        const std::array<bool, 4> checked{file_search_draft.case_sensitive, file_search_draft.backwards,
                                        file_search_draft.whole_words, file_search_all_charsets};
        Elements types, options;
        for (std::size_t index = 0; index < modes.size(); ++index) {
            auto row = text(std::string(file_search_draft.mode == modes[index] ? " (*) " : " ( ) ") + names[index]) |
                       reflect(file_search_boxes[index + 1U]);
            if (file_search_focus == 1U && file_search_draft.mode == modes[index]) row = focus_dialog(std::move(row));
            types.push_back(std::move(row));
            auto checkbox = text(std::string(checked[index] ? " [x] " : " [ ] ") + flags[index]) |
                            reflect(file_search_boxes[index + 5U]);
            if (file_search_focus == index + 2U) checkbox = focus_dialog(std::move(checkbox));
            options.push_back(std::move(checkbox));
        }
        Elements rows{
            text(" Enter search string:"),
            hbox({text(" "), render_input(file_search_value, file_search_input, file_search_focus == 0U, "") |
                  reflect(file_search_boxes[0]), text(" ")}),
            separator(),
            hbox({vbox(std::move(types)) | size(WIDTH, EQUAL, 27), vbox(std::move(options)) | flex})
        };
        if (!file_search_error.empty()) rows.push_back(paragraph(display_text(file_search_error)) | bold);
        rows.push_back(separator());
        rows.push_back(hbox({
            native_button("OK", true, file_search_focus == 6U, file_search_boxes[9]), text(" "),
            native_button("Cancel", false, file_search_focus == 7U, file_search_boxes[10])
        }) | center);
        return native_window("Search", vbox(std::move(rows)), 56);
    }

    void apply_file_search() try {
        if (file_search_value.empty()) throw std::invalid_argument("search text is empty");
        if (file_search_draft.mode == EditorSearchMode::hexadecimal) {
            static_cast<void>(EditorSearch::decode_hexadecimal(file_search_value));
        } else static_cast<void>(EditorSearch::find("", file_search_value, 0, 0, 0, file_search_draft, false, false));
        remember_input(file_search_input, file_search_value);
        if (file_search_target == FileSearchTarget::diff) {
            diff_search_query = file_search_value;
            diff_search_options = file_search_draft;
            diff_search_all_charsets = file_search_all_charsets;
            diff_search_cursor.reset();
            find_diff(false);
        } else if (file_search_target == FileSearchTarget::viewer) {
            viewer_search_needle = file_search_value;
            viewer_search_options = file_search_draft;
            viewer_search_all_charsets = file_search_all_charsets;
            run_viewer_search(false, false);
        } else {
            quick_view_search_needle = file_search_value;
            quick_view_search_options = file_search_draft;
            quick_view_search_all_charsets = file_search_all_charsets;
            run_viewer_search(true, false);
        }
    } catch (const std::exception& error) {
        file_search_error = error.what();
        mode = Mode::file_search_dialog;
    }

    bool handle_file_search(const ftxui::Event& event) {
        using namespace ftxui;
        const auto toggle = [&](const std::size_t option) {
            if (option == 0U) file_search_draft.case_sensitive = !file_search_draft.case_sensitive;
            else if (option == 1U) file_search_draft.backwards = !file_search_draft.backwards;
            else if (option == 2U) file_search_draft.whole_words = !file_search_draft.whole_words;
            else file_search_all_charsets = !file_search_all_charsets;
        };
        static const std::array<EditorSearchMode, 4> modes{
            EditorSearchMode::normal, EditorSearchMode::regular_expression, EditorSearchMode::hexadecimal, EditorSearchMode::wildcard
        };
        if (modal_cancel(event)) { mode = file_search_return(); return true; }
        if (event.is_mouse()) {
            auto copy = event;
            const auto& mouse = copy.mouse();
            if (file_search_input.dragging) return input_mouse(file_search_value, file_search_input, mouse);
            if (mouse.button != Mouse::Left || mouse.motion != Mouse::Pressed) return true;
            for (std::size_t index = 0; index < file_search_boxes.size(); ++index) {
                if (!file_search_boxes[index].Contain(mouse.x, mouse.y)) continue;
                if (index == 0U) { file_search_focus = 0; input_mouse(file_search_value, file_search_input, mouse); }
                else if (index < 5U) { file_search_focus = 1; file_search_draft.mode = modes[index - 1U]; }
                else if (index < 9U) { file_search_focus = index - 3U; toggle(index - 5U); }
                else if (index == 9U) apply_file_search();
                else mode = file_search_return();
                return true;
            }
        } else if (event == Event::Tab || event == Event::TabReverse) {
            file_search_focus = event == Event::Tab ? (file_search_focus + 1U) % 8U : (file_search_focus + 7U) % 8U;
        } else if (event == Event::Return || (event == Event::Character(' ') && file_search_focus >= 6U)) {
            if (file_search_focus == 7U) mode = file_search_return();
            else apply_file_search();
        } else if (file_search_focus == 0U) return edit_input(file_search_value, file_search_input, event);
        else if (file_search_focus == 1U && (event == Event::ArrowUp || event == Event::ArrowDown)) {
            const auto selected = static_cast<std::size_t>(std::ranges::find(modes, file_search_draft.mode) - modes.begin());
            file_search_draft.mode = modes[event == Event::ArrowDown ? (selected + 1U) % 4U : (selected + 3U) % 4U];
        } else if (file_search_focus > 1U && file_search_focus < 6U && event == Event::Character(' ')) {
            toggle(file_search_focus - 2U);
        }
        return true;
    }

    static constexpr std::array<bool DiffOptions::*, 5> diff_option_members{
        &DiffOptions::ignore_case, &DiffOptions::ignore_tab_expansion, &DiffOptions::ignore_space_change,
        &DiffOptions::ignore_all_space, &DiffOptions::strip_trailing_cr
    };

    ftxui::Element render_diff_options() {
        using namespace ftxui;
        static const std::array<std::string, 3> quality{
            "Normal", "Fastest (Assume large files)", "Minimal (Find a smaller set of change)"
        };
        static const std::array<std::string, 5> extras{
            "Ignore case", "Ignore tab expansion", "Ignore space change", "Ignore all whitespace", "Strip trailing carriage return"
        };
        Elements algorithms, options;
        for (std::size_t index = 0; index < quality.size(); ++index) {
            auto row = text(std::string(diff_draft.quality == index ? " (*) " : " ( ) ") + quality[index]) |
                       reflect(diff_options_boxes[index]);
            if (diff_options_focus == 0U && diff_draft.quality == index) row = focus_dialog(std::move(row));
            algorithms.push_back(std::move(row));
        }
        for (std::size_t index = 0; index < extras.size(); ++index) {
            auto row = text(std::string(diff_draft.*diff_option_members[index] ? " [x] " : " [ ] ") + extras[index]) |
                       reflect(diff_options_boxes[index + 3U]);
            if (diff_options_focus == index + 1U) row = focus_dialog(std::move(row));
            options.push_back(std::move(row));
        }
        return native_window("Diff Options", vbox({
            hbox({text(" "), vbox({
                window(text(" Diff algorithm "), vbox(std::move(algorithms)), LIGHT),
                window(text(" Diff extra options "), vbox(std::move(options)), LIGHT)
            }) | flex, text(" ")}),
            separator(),
            hbox({native_button("OK", true, diff_options_focus == 6U, diff_options_boxes[8]), text(" "),
                  native_button("Cancel", false, diff_options_focus == 7U, diff_options_boxes[9])}) | center
        }), 54);
    }

    void apply_diff_options() {
        DiffModel comparison;
        comparison.compare(diff_files[0], diff_files[1], diff_draft);
        diff_options = diff_draft;
        file_diff = std::move(comparison);
        config.diff_quality = diff_options.quality;
        config.diff_ignore_case = diff_options.ignore_case;
        config.diff_ignore_tab_expansion = diff_options.ignore_tab_expansion;
        config.diff_ignore_space_change = diff_options.ignore_space_change;
        config.diff_ignore_all_space = diff_options.ignore_all_space;
        config.diff_strip_trailing_cr = diff_options.strip_trailing_cr;
        diff_top = diff_hunk = 0;
        diff_search_cursor.reset();
        diff_search_match.reset();
        mode = Mode::diff;
    }

    bool handle_diff_options(const ftxui::Event& event) try {
        using namespace ftxui;
        if (modal_cancel(event)) { mode = Mode::diff; return true; }
        if (event.is_mouse()) {
            auto copy = event;
            const auto& mouse = copy.mouse();
            if (mouse.button != Mouse::Left || mouse.motion != Mouse::Pressed) return true;
            for (std::size_t index = 0; index < diff_options_boxes.size(); ++index) {
                if (!diff_options_boxes[index].Contain(mouse.x, mouse.y)) continue;
                if (index < 3U) { diff_draft.quality = index; diff_options_focus = 0; }
                else if (index < 8U) {
                    diff_options_focus = index - 2U;
                    const auto member = diff_option_members[index - 3U];
                    diff_draft.*member = !(diff_draft.*member);
                } else if (index == 8U) apply_diff_options();
                else mode = Mode::diff;
                return true;
            }
        } else if (event == Event::Tab || event == Event::TabReverse) {
            diff_options_focus = event == Event::Tab ? (diff_options_focus + 1U) % 8U : (diff_options_focus + 7U) % 8U;
        } else if (diff_options_focus == 0U && (event == Event::ArrowUp || event == Event::ArrowDown)) {
            diff_draft.quality = event == Event::ArrowDown ? (diff_draft.quality + 1U) % 3U : (diff_draft.quality + 2U) % 3U;
        } else if (event == Event::Character(' ') && diff_options_focus > 0U && diff_options_focus < 6U) {
            const auto member = diff_option_members[diff_options_focus - 1U];
            diff_draft.*member = !(diff_draft.*member);
        } else if (event == Event::Return || (event == Event::Character(' ') && diff_options_focus >= 6U)) {
            if (diff_options_focus == 7U) mode = Mode::diff;
            else apply_diff_options();
        }
        return true;
    } catch (const std::exception& error) {
        show_native_message("Diff Options", error.what(), Mode::diff_options);
        return true;
    }

    bool handle_diff(const ftxui::Event& event) try {
        using namespace ftxui;
        if (modal_cancel(event) || event == Event::Character('q') || event == Event::Character('Q')) {
            request_diff_close();
            return true;
        }
        if (event == Event::F2) save_diff();
        else if (event == Event::F9) { diff_draft = diff_options; diff_options_focus = 0; mode = Mode::diff_options; }
        else if (event == Event::F4 || event == Event::Special("\x1b[26~")) {
            const auto side = event == Event::F4 ? diff_order : diff_order ^ 1U;
            if (diff_files[side].dirty()) show_native_message("Edit", "Save the merged file before opening it in the editor.", Mode::diff);
            else {
                diff_edit_side = side;
                edit_file(local, diff_files[side].path(), Mode::diff);
                if (mode == Mode::diff) resume_diff_after_edit();
            }
        } else if (event == Event::F5 || event == Event::Special("\x1b[28~")) {
            if (file_diff.hunks().empty()) return true;
            const auto& selected = file_diff.hunks()[diff_hunk];
            if (selected.display_row < diff_top || selected.display_row >= diff_top + diff_visible_rows()) {
                diff_top = selected.display_row;
                return true;
            }
            const bool forward = event == Event::F5 ? diff_order == 0U : diff_order != 0U;
            file_diff.merge(diff_hunk, forward, diff_files[0], diff_files[1]);
            diff_last_merged_side = forward ? 1U : 0U;
            rebuild_diff();
        } else if (event == Event::CtrlZ || event == Event::AltR) {
            auto& buffer = diff_files[diff_last_merged_side.value_or(diff_order ^ 1U)];
            const bool changed = event == Event::CtrlZ ? buffer.undo() : buffer.redo();
            if (changed) rebuild_diff();
        } else if (event == Event::CtrlR) {
            for (auto& buffer : diff_files) {
                if (!buffer.dirty()) { const auto path = buffer.path(); buffer.open(local, path); }
            }
            rebuild_diff();
        }
        else if (event == Event::CtrlU) { diff_order ^= 1U; diff_search_cursor.reset(); diff_search_match.reset(); }
        else if (event == Event::AltE) {
            show_codepage_chooser(diff_files[diff_order].encoding(), Mode::diff, [this](const std::string& codepage) {
                auto files = diff_files;
                for (auto& file : files) file.set_encoding(codepage);
                DiffModel comparison;
                comparison.compare(files[0], files[1], diff_options);
                diff_files = std::move(files);
                file_diff = std::move(comparison);
                diff_top = diff_hunk = diff_horizontal = 0;
                diff_search_cursor.reset();
                diff_search_match.reset();
            });
        }
        else if (event == Event::F7) {
            show_file_search(FileSearchTarget::diff);
        } else if (event == Event::Special("\x1b[31~")) find_diff(true);
        else if (event == Event::Character('n') || event == Event::Return || event == Event::Character(' ') ||
                 event == Event::Character('p') || event == Event::Backspace) {
            if (file_diff.hunks().empty()) return true;
            const bool forward = event != Event::Character('p') && event != Event::Backspace;
            if (forward) {
                if (file_diff.hunks()[diff_hunk].display_row <= diff_top && diff_hunk + 1U < file_diff.hunks().size()) ++diff_hunk;
            } else if (file_diff.hunks()[diff_hunk].display_row >= diff_top && diff_hunk > 0U) --diff_hunk;
            diff_top = file_diff.hunks()[diff_hunk].display_row;
        } else if (event == Event::ArrowDown) ++diff_top;
        else if (event == Event::ArrowUp) diff_top -= std::min(diff_top, std::size_t{1});
        else if (event == Event::PageDown) diff_top += diff_visible_rows();
        else if (event == Event::PageUp) diff_top -= std::min(diff_top, diff_visible_rows());
        else if (event == Event::Special("\x1b[1;5H") || event == Event::Home) { diff_top = 0; diff_horizontal = 0; }
        else if (event == Event::Special("\x1b[1;5F") || event == Event::End) diff_top = file_diff.rows().size();
        else if (event == Event::ArrowRight) ++diff_horizontal;
        else if (event == Event::ArrowLeft) diff_horizontal -= std::min(diff_horizontal, std::size_t{1});
        else if (event == Event::Character('f')) diff_full = !diff_full;
        else if (event == Event::Character('=')) diff_split = Terminal::Size().dimx / 2;
        else if (event == Event::Character('<')) --diff_split;
        else if (event == Event::Character('>')) ++diff_split;
        else if (event == Event::Character('s') || event == Event::AltS) config.diff_show_symbols = diff_show_symbols = !diff_show_symbols;
        else if (event == Event::Character('l') || event == Event::AltN) config.diff_show_numbers = diff_show_numbers = !diff_show_numbers;
        else if (event == Event::Character('2') || event == Event::Character('3') ||
                 event == Event::Character('4') || event == Event::Character('8')) {
            config.diff_tab_size = diff_tab_size = static_cast<std::size_t>(event.character()[0] - '0');
        }
        else if (event == Event::Character('g') || event == Event::Character('G')) {
            show_native_input("Goto line (right)", "Enter line:", "", [this](const std::string& value) {
                std::size_t line = 0;
                const auto parsed = std::from_chars(value.data(), value.data() + value.size(), line);
                if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || line == 0U) {
                    throw std::invalid_argument("line numbers start at 1");
                }
                for (std::size_t row = 0; row < file_diff.rows().size(); ++row) {
                    const auto number = diff_order == 0U ? file_diff.rows()[row].right : file_diff.rows()[row].left;
                    if (number && *number + 1U >= line) { diff_top = row; return; }
                }
                throw std::out_of_range("line is past the end of the file");
            }, Mode::diff, {}, "goto.diff");
        } else if (event.is_mouse()) {
            auto copy = event;
            if (copy.mouse().button == Mouse::WheelDown) diff_top += 3U;
            else if (copy.mouse().button == Mouse::WheelUp) diff_top -= std::min(diff_top, std::size_t{3});
        } else return false;
        const auto maximum = file_diff.rows().size() > diff_visible_rows() ? file_diff.rows().size() - diff_visible_rows() : 0U;
        diff_top = std::min(diff_top, maximum);
        for (std::size_t hunk = 0; hunk < file_diff.hunks().size(); ++hunk) {
            if (file_diff.hunks()[hunk].display_row >= diff_top) { diff_hunk = hunk; break; }
        }
        return true;
    } catch (const std::exception& error) {
        show_native_message("Diff viewer", error.what(), Mode::diff);
        return true;
    }

    void show_external_panelize() {
        panelize_cursor = 0;
        panelize_focus = 0;
        panelize_command.clear();
        panelize_error.clear();
        panelize_input.reset("", true);
        mode = Mode::external_panelize_dialog;
    }

    ftxui::Element render_external_panelize() {
        using namespace ftxui;
        Elements rows;
        panelize_rows.assign(config.panelize_commands.size() + 1U, {});
        for (std::size_t index = 0; index < panelize_rows.size(); ++index) {
            const auto label = index == 0U ? std::string("Other command") : config.panelize_commands[index - 1U].first;
            auto row = text(" " + middle_truncate(display_text(label),
                static_cast<std::size_t>(std::max(1, Terminal::Size().dimx - 16)))) |
                reflect(panelize_rows[index]);
            if (panelize_cursor == index) {
                row = row | inverted;
                if (panelize_focus == 0U) row = row | focus;
            }
            rows.push_back(std::move(row));
        }
        while (rows.size() < 10U) rows.push_back(text(""));
        const auto list_height = std::max(1, std::min(10, Terminal::Size().dimy - 12));
        auto list = vbox(std::move(rows)) | yframe | size(HEIGHT, EQUAL, list_height) | reflect(panelize_viewport);
        Elements content{
            hbox({text(" "), std::move(list) | borderStyled(LIGHT) | flex, text(" ")}),
            text(" Command"),
            hbox({text(" "), render_input(panelize_command, panelize_input, panelize_focus == 1U, "") |
                  reflect(panelize_command_box), text(" ")})
        };
        if (!panelize_error.empty()) content.push_back(text(" " + display_text(panelize_error)));
        content.push_back(separator());
        content.push_back(hbox({
            native_button("Panelize", true, panelize_focus == 2U, panelize_buttons[0]), text(" "),
            native_button("Remove", false, panelize_focus == 3U, panelize_buttons[1]), text(" "),
            native_button("Add new", false, panelize_focus == 4U, panelize_buttons[2]), text(" "),
            native_button("Cancel", false, panelize_focus == 5U, panelize_buttons[3])
        }) | center);
        return native_window("External panelize", vbox(std::move(content)), std::max(1, Terminal::Size().dimx - 8));
    }

    void activate_panelize_button(const std::size_t button) {
        if (button == 3U) { mode = Mode::panels; return; }
        if (button == 1U) {
            if (panelize_cursor == 0U) { panelize_error = "Other command cannot be removed"; return; }
            config.panelize_commands.erase(config.panelize_commands.begin() + static_cast<std::ptrdiff_t>(panelize_cursor - 1U));
            mode = Mode::panels;
            status = "Panelize command removed";
            return;
        }
        if (panelize_command.empty()) { panelize_error = "Enter a command first"; return; }
        if (button == 2U) {
            const auto command = panelize_command;
            show_native_input("Add to external panelize", "Enter command label:", "",
                [this, command](const std::string& label) {
                    if (label.empty()) throw std::invalid_argument("command label is empty");
                    const auto found = std::ranges::find(config.panelize_commands, label,
                        &std::pair<std::string, std::string>::first);
                    remember_input(panelize_input, command);
                    if (found != config.panelize_commands.end()) found->second = command;
                    else config.panelize_commands.emplace_back(label, command);
                    status = "Panelize command saved";
                }, Mode::panels, {}, "command.panelize-label");
            return;
        }
        try {
            remember_input(panelize_input, panelize_command);
        } catch (const std::exception& error) {
            panelize_error = error.what();
            return;
        }
        run_external_panelize(panelize_command);
    }

    bool handle_external_panelize(const ftxui::Event& event) {
        using namespace ftxui;
        if (modal_cancel(event)) { mode = Mode::panels; return true; }
        const auto selected_profile = [&] {
            panelize_command = panelize_cursor == 0U ? std::string{} : config.panelize_commands.at(panelize_cursor - 1U).second;
            panelize_input.reset(panelize_command, true);
        };
        if (event.is_mouse()) {
            auto copy = event;
            const auto& mouse = copy.mouse();
            if (panelize_input.dragging) return input_mouse(panelize_command, panelize_input, mouse);
            if (mouse.button == Mouse::Left && mouse.motion == Mouse::Pressed) {
                if (panelize_command_box.Contain(mouse.x, mouse.y)) {
                    panelize_focus = 1;
                    return input_mouse(panelize_command, panelize_input, mouse);
                }
                for (std::size_t index = 0; index < panelize_buttons.size(); ++index) {
                    if (panelize_buttons[index].Contain(mouse.x, mouse.y)) {
                        panelize_focus = index + 2U;
                        activate_panelize_button(index);
                        return true;
                    }
                }
            }
            const auto result = handle_list_input(event, panelize_cursor, panelize_rows.size(), panelize_rows, panelize_viewport);
            if (result == ButtonInput::changed || result == ButtonInput::activate) {
                panelize_focus = 0;
                selected_profile();
                if (result == ButtonInput::activate) activate_panelize_button(0);
            }
            return true;
        }
        if (event == Event::Tab || event == Event::TabReverse) {
            panelize_focus = event == Event::Tab ? (panelize_focus + 1U) % 6U : (panelize_focus + 5U) % 6U;
        } else if (event == Event::Return || (event == Event::Character(' ') && panelize_focus >= 2U)) {
            activate_panelize_button(panelize_focus < 2U ? 0U : panelize_focus - 2U);
        } else if (panelize_focus == 1U) {
            return edit_input(panelize_command, panelize_input, event);
        } else if (panelize_focus == 0U) {
            const auto result = handle_list_input(event, panelize_cursor, panelize_rows.size(), panelize_rows, panelize_viewport);
            if (result == ButtonInput::changed) selected_profile();
            else if (event == Event::Character('l') || event == Event::AltL) activate_panelize_button(0);
            else if (event == Event::Character('r') || event == Event::AltR) activate_panelize_button(1);
            else if (event == Event::Character('a') || event == Event::AltA) activate_panelize_button(2);
            else if (event == Event::Character('c') || event == Event::AltC) activate_panelize_button(3);
        }
        return true;
    }

    void run_external_panelize(const std::string& command) {
        if (active().provider() != local) {
            throw std::runtime_error("external panelization requires a local directory");
        }
        const auto directory = active().path();
        const auto panel = active_index;
            start_local_task("External panelization",
                [command, directory](const Cancellation& cancellation) {
                    return capture_shell(command, directory, cancellation);
                }, [this, directory, panel](CommandOutput result) {
                    require_command_success(result);
                    const auto separator = result.output.find('\0') != std::string::npos ? '\0' : '\n';
                    std::vector<Entry> entries;
                    std::set<std::string> seen;
                    std::size_t begin = 0;
                    while (begin < result.output.size()) {
                        const auto end = result.output.find(separator, begin);
                        const auto line = result.output.substr(begin, end == std::string::npos
                                                                         ? std::string::npos : end - begin);
                        begin = end == std::string::npos ? result.output.size() : end + 1U;
                        if (line.empty()) continue;
                        const auto path = local->join(directory, line);
                        if (!seen.insert(path).second) continue;
                        auto entry = local->stat(path, {});
                        entry.name = line;
                        entries.push_back(std::move(entry));
                    }
                    (panel == 0 ? left : right).set_panelized(std::move(entries));
                    status = "External panelization complete";
                    show_command_warnings(result.error);
                });
    }

    void show_edit_symlink() {
        const auto* selected = active().current();
        if (selected == nullptr || selected->kind != EntryKind::symlink ||
            selected->revision == "mcpp:parent") {
            throw std::runtime_error("select a symlink to edit");
        }
        const auto entry = *selected;
        show_native_input("Edit symlink", "Symlink '" + display_text(entry.name) + "' points to:", entry.target,
            [this, entry](const std::string& target) {
            if (target == entry.target) return;
            auto* provider = dynamic_cast<LocalProvider*>(local.get());
            if (provider == nullptr) throw std::runtime_error("symlink editing requires the local provider");
            provider->replace_symlink(entry.path, target, entry.revision);
            refresh(false, true);
            status = "Symlink updated";
        }, Mode::panels, active().navigation_path(), "filename.symlink-target");
    }

    std::vector<Entry> real_selection() {
        auto entries = active().selected_entries();
        std::erase_if(entries, [](const Entry& entry) { return entry.revision == "mcpp:parent"; });
        if (entries.empty()) throw std::runtime_error("select a real file or directory first");
        return entries;
    }

    std::vector<Entry> unique_metadata_selection() {
        auto entries = real_selection();
        const auto* provider = dynamic_cast<const LocalProvider*>(local.get());
        if (provider == nullptr) throw std::runtime_error("metadata editing requires the local provider");
        std::vector<Entry> unique;
        for (auto& entry : entries) {
            const bool alias = entry.kind == EntryKind::file && std::ranges::any_of(unique, [&](const Entry& existing) {
                return existing.kind == EntryKind::file &&
                       provider->same_file(entry.path, *provider, existing.path);
            });
            if (!alias) unique.push_back(std::move(entry));
        }
        return unique;
    }

    void show_advanced_chown() {
        ownership_entries = unique_metadata_selection();
        ownership_index = 0;
        ownership_buttons.assign(ownership_entries.size() > 1U ? 4U : 2U, {});
        load_ownership_entry();
        mode = Mode::advanced_chown_dialog;
    }

    void load_ownership_entry() {
        auto& entry = ownership_entries.at(ownership_index);
        const auto name = entry.name;
        entry = local->stat(entry.path, {});
        entry.name = name;
        ownership_user = entry.owner;
        ownership_group = entry.group;
        ownership_flags.fill('=');
        ownership_focus = 0;
        ownership_bit = 0;
        ownership_error.clear();
    }

    unsigned int ownership_mode(const Entry& entry) const {
        auto mode_value = static_cast<unsigned int>(entry.permissions) & 07777U;
        for (std::size_t bit = 0; bit < 9U; ++bit) {
            const auto mask = 1U << (8U - static_cast<unsigned int>(bit));
            if (ownership_flags[bit] == '+') mode_value |= mask;
            else if (ownership_flags[bit] == '-') mode_value &= ~mask;
        }
        return mode_value;
    }

    ftxui::Element render_advanced_chown() {
        using namespace ftxui;
        const auto& entry = ownership_entries.at(ownership_index);
        const auto permissions = ownership_mode(entry);
        Elements values{text("      ")};
        constexpr std::string_view letters{"rwx"};
        for (std::size_t group = 0; group < 3U; ++group) {
            Elements button{text("[")};
            for (std::size_t bit = 0; bit < 3U; ++bit) {
                const auto absolute = group * 3U + bit;
                auto character = text(std::string(1, (permissions & (1U << (8U - static_cast<unsigned int>(absolute)))) != 0U
                                                       ? letters[bit] : '-'));
                if (ownership_focus == group && ownership_bit == absolute) character = character | underlined | bold;
                button.push_back(std::move(character));
            }
            button.push_back(text("]"));
            auto field = hbox(std::move(button)) | reflect(ownership_boxes[group]);
            if (ownership_focus == group) field = focus_dialog(std::move(field));
            values.push_back(std::move(field));
            values.push_back(text(group == 2U ? "     " : "   "));
        }
        auto user = text("[" + middle_truncate(display_text(ownership_user), 15) + "]") | reflect(ownership_boxes[3]);
        auto group = text("[" + middle_truncate(display_text(ownership_group), 15) + "]") | reflect(ownership_boxes[4]);
        if (ownership_focus == 3U) user = focus_dialog(std::move(user));
        if (ownership_focus == 4U) group = focus_dialog(std::move(group));
        values.push_back(std::move(user) | size(WIDTH, EQUAL, 18));
        values.push_back(std::move(group) | size(WIDTH, EQUAL, 18));
        std::string flags = " Flag  " + std::string(ownership_flags.data(), 3) + "     " +
            std::string(ownership_flags.data() + 3, 3) + "     " +
            std::string(ownership_flags.data() + 6, 3) + "       " +
            std::string(15, ownership_flags[9]) + "   " + std::string(15, ownership_flags[10]);
        std::ostringstream octal;
        octal << " Permissions (octal): " << std::oct << ((entry.mode & ~07777U) | permissions);
        const std::vector<std::string> labels = ownership_entries.size() > 1U
            ? std::vector<std::string>{"Set all","Skip","Set","Cancel"} : std::vector<std::string>{"Set","Cancel"};
        Elements buttons;
        for (std::size_t index = 0; index < labels.size(); ++index) {
            if (index != 0U) buttons.push_back(text(" "));
            buttons.push_back(native_button(labels[index], labels[index] == "Set", ownership_focus == index + 5U,
                                           ownership_buttons[index]));
        }
        return native_window("Chown advanced command", vbox({
            hbox({text(" " + display_text(entry.name)), filler(),
                  text("(" + std::to_string(ownership_index + 1U) + "/" + std::to_string(ownership_entries.size()) + ")  ")}),
            separator(),
            text("      owner   group   other     owner             group"),
            hbox(std::move(values)), text(flags), text(octal.str()), separator(),
            hbox(std::move(buttons)) | center
        }), 72);
    }

    void choose_account(const bool user) {
        struct Enumeration {
            bool user;
            ~Enumeration() { if (user) endpwent(); else endgrent(); }
        } enumeration{user};
        std::vector<std::string> names;
        if (user) {
            setpwent();
            for (;;) {
                errno = 0;
                const auto* account = getpwent();
                if (account == nullptr) break;
                names.emplace_back(account->pw_name);
            }
        } else {
            setgrent();
            for (;;) {
                errno = 0;
                const auto* account = getgrent();
                if (account == nullptr) break;
                names.emplace_back(account->gr_name);
            }
        }
        if (errno != 0 && errno != ENOENT) throw std::system_error(errno, std::generic_category(), "read accounts");
        std::ranges::sort(names);
        const auto current = user ? ownership_user : ownership_group;
        show_local_list(user ? "Owner" : "Group", names, [this, names, user](const std::size_t selected) {
            (user ? ownership_user : ownership_group) = names.at(selected);
            ownership_flags[user ? 9U : 10U] = '+';
        }, Mode::advanced_chown_dialog);
        local_list_native = true;
        const auto found = std::ranges::find(names, current);
        local_list_cursor = found == names.end() ? 0U : static_cast<std::size_t>(found - names.begin());
    }

    void apply_advanced_chown(const std::size_t button) try {
        const auto action = ownership_entries.size() > 1U ? button : button + 2U;
        if (action == 3U) { mode = Mode::panels; return; }
        const auto last = action == 0U ? ownership_entries.size() : ownership_index + 1U;
        if (action != 1U) {
            for (auto index = ownership_index; index < last; ++index) {
                const auto& entry = ownership_entries[index];
                if (local->stat(entry.path, {}).revision != entry.revision) {
                    throw std::runtime_error("selected path changed: " + entry.path);
                }
            }
            for (auto index = ownership_index; index < last; ++index) {
                const auto& entry = ownership_entries[index];
                const auto owner = ownership_flags[9] == '+' && ownership_user != entry.owner
                                     ? std::optional<std::string>{ownership_user} : std::nullopt;
                const auto group = ownership_flags[10] == '+' && ownership_group != entry.group
                                     ? std::optional<std::string>{ownership_group} : std::nullopt;
                if (owner || group) local->set_owner(entry.path, owner, group);
                const auto permissions = ownership_mode(entry);
                if (permissions != (static_cast<unsigned int>(entry.permissions) & 07777U)) {
                    local->set_permissions(entry.path, static_cast<std::filesystem::perms>(permissions));
                }
            }
        }
        ownership_index = last;
        if (ownership_index < ownership_entries.size()) load_ownership_entry();
        else { mode = Mode::panels; refresh(false, true); status = "Owner/permission request completed"; }
    } catch (const std::exception& error) {
        ownership_error = error.what();
        show_native_message("Error", ownership_error, Mode::advanced_chown_dialog);
    }

    bool handle_advanced_chown(const ftxui::Event& event) {
        using namespace ftxui;
        if (modal_cancel(event)) { mode = Mode::panels; return true; }
        if (event.is_mouse()) {
            auto copy = event;
            const auto& mouse = copy.mouse();
            if (mouse.button != Mouse::Left || mouse.motion != Mouse::Pressed) return true;
            for (std::size_t index = 0; index < ownership_boxes.size(); ++index) {
                if (!ownership_boxes[index].Contain(mouse.x, mouse.y)) continue;
                ownership_focus = index;
                if (index < 3U) ownership_bit = index * 3U + static_cast<std::size_t>(std::clamp(mouse.x - ownership_boxes[index].x_min - 1, 0, 2));
                else choose_account(index == 3U);
                return true;
            }
            for (std::size_t index = 0; index < ownership_buttons.size(); ++index) {
                if (ownership_buttons[index].Contain(mouse.x, mouse.y)) { apply_advanced_chown(index); return true; }
            }
            return true;
        }
        const auto count = 5U + ownership_buttons.size();
        if (event == Event::Tab || event == Event::TabReverse) {
            ownership_focus = event == Event::Tab ? (ownership_focus + 1U) % count : (ownership_focus + count - 1U) % count;
            if (ownership_focus < 3U) ownership_bit = ownership_focus * 3U;
        } else if (ownership_focus < 3U && (event == Event::ArrowLeft || event == Event::ArrowRight)) {
            ownership_bit = event == Event::ArrowRight ? (ownership_bit + 1U) % 9U : (ownership_bit + 8U) % 9U;
            ownership_focus = ownership_bit / 3U;
        } else if (ownership_focus < 3U && event.is_character() && event.character().size() == 1U) {
            const auto key = event.character()[0];
            if (key == '+' || key == '-' || key == '=') ownership_flags[ownership_bit] = key;
            else {
                if (const auto selected = std::string_view("rwx").find(key); selected != std::string_view::npos) {
                    ownership_bit = ownership_focus * 3U + selected;
                } else if (key != ' ') return true;
                const auto bit = 1U << (8U - static_cast<unsigned int>(ownership_bit));
                ownership_flags[ownership_bit] = (ownership_mode(ownership_entries[ownership_index]) & bit) != 0U ? '-' : '+';
            }
        } else if (event == Event::Return || event == Event::Character(' ')) {
            if (ownership_focus == 3U || ownership_focus == 4U) choose_account(ownership_focus == 3U);
            else if (ownership_focus >= 5U) apply_advanced_chown(ownership_focus - 5U);
        }
        return true;
    }

    struct FileAttribute {
        std::string_view label;
        char letter;
        unsigned int bit;
        std::size_t slot;
    };

    static const std::array<FileAttribute, 14>& file_attributes() {
        static const std::array<FileAttribute, 14> attributes{{
            {"Secure deletion",'s',FS_SECRM_FL,0}, {"Undelete",'u',FS_UNRM_FL,1},
            {"Synchronous updates",'S',FS_SYNC_FL,2}, {"Synchronous directory updates",'D',FS_DIRSYNC_FL,3},
            {"Immutable",'i',FS_IMMUTABLE_FL,4}, {"Append only",'a',FS_APPEND_FL,5},
            {"No dump",'d',FS_NODUMP_FL,6}, {"No update atime",'A',FS_NOATIME_FL,7},
            {"Compress",'c',FS_COMPR_FL,8}, {"No tail merging",'t',FS_NOTAIL_FL,11},
            {"Top of directory hierarchies",'T',FS_TOPDIR_FL,12}, {"Inode uses extents",'e',FS_EXTENT_FL,17},
            {"Casefolded file",'F',FS_CASEFOLD_FL,23}, {"Project hierarchy",'P',FS_PROJINHERIT_FL,22}
        }};
        return attributes;
    }

    void load_attribute_entry() {
        auto& entry = attribute_entries.at(attribute_index);
        const auto name = entry.name;
        entry = local->stat(entry.path, {});
        entry.name = name;
        attribute_original = local_file_flags(entry.path);
        attribute_value = attribute_original;
        attribute_focus = 0;
        attribute_error.clear();
    }

    void show_file_attributes() {
        attribute_entries = unique_metadata_selection();
        attribute_index = 0;
        load_attribute_entry();
        attribute_buttons.assign(attribute_entries.size() > 1U ? 6U : 2U, {});
        mode = Mode::chattr_dialog;
    }

    ftxui::Element render_file_attributes() {
        using namespace ftxui;
        const auto& attributes = file_attributes();
        std::string letters(25, '-');
        Elements rows;
        for (std::size_t index = 0; index < attributes.size(); ++index) {
            const auto& attribute = attributes[index];
            const bool checked = (attribute_value & attribute.bit) != 0U;
            if (checked) letters[attribute.slot] = attribute.letter;
            auto row = text(std::string(checked ? " [x] (" : " [ ] (") + attribute.letter + ") " +
                            std::string(attribute.label)) | reflect(attribute_boxes[index]);
            if (attribute_focus == index) row = focus_dialog(std::move(row));
            rows.push_back(std::move(row));
        }
        const bool many = attribute_entries.size() > 1U;
        const std::vector<std::string> labels = many
            ? std::vector<std::string>{"Set all","Marked all","Set marked","Clear marked","Set","Cancel"}
            : std::vector<std::string>{"Set","Cancel"};
        attribute_buttons.assign(labels.size(), {});
        Elements buttons;
        for (std::size_t index = 0; index < labels.size(); ++index) {
            if (index != 0U) buttons.push_back(text(" "));
            buttons.push_back(native_button(labels[index], labels[index] == "Set",
                attribute_focus == attributes.size() + index, attribute_buttons[index]));
        }
        Elements content{
            text(" " + display_text(attribute_entries.at(attribute_index).name)) | center,
            text(letters) | center,
            separator(),
            vbox(std::move(rows)) | yframe |
                size(HEIGHT, LESS_THAN, std::max(1, Terminal::Size().dimy - 9)) | reflect(attribute_viewport)
        };
        if (!attribute_error.empty()) content.push_back(text(display_text(attribute_error)));
        content.push_back(separator());
        content.push_back(hbox(std::move(buttons)) | center);
        return native_window("Chattr command", vbox(std::move(content)),
                             many ? std::min(84, Terminal::Size().dimx - 2) : 43);
    }

    void apply_file_attributes(const std::size_t button) try {
        const bool many = attribute_entries.size() > 1U;
        const auto action = many ? button : button + 4U;
        if (action == 5U) { mode = Mode::panels; return; }
        unsigned int known = 0;
        for (const auto& attribute : file_attributes()) known |= attribute.bit;
        const auto changed = attribute_original ^ attribute_value;
        const auto last = action == 4U ? attribute_index + 1U : attribute_entries.size();
        for (auto index = attribute_index; index < last; ++index) {
            const auto& entry = attribute_entries[index];
            if (local->stat(entry.path, {}).revision != entry.revision) {
                throw std::runtime_error("selected path changed: " + entry.path);
            }
        }
        for (auto index = attribute_index; index < last; ++index) {
            const auto& entry = attribute_entries[index];
            const auto original = local_file_flags(entry.path);
            const auto mask = action == 0U ? known : changed;
            const auto updated = action == 2U ? original | mask
                               : action == 3U ? original & ~mask
                               : (original & ~mask) | (attribute_value & mask);
            if (updated != original) set_local_file_flags(entry.path, updated);
        }
        attribute_index = last;
        if (attribute_index < attribute_entries.size()) load_attribute_entry();
        else {
            mode = Mode::panels;
            refresh(false, true);
            status = "File attributes changed";
        }
    } catch (const std::exception& error) {
        attribute_error = error.what();
    }

    bool handle_file_attributes(const ftxui::Event& event) {
        using namespace ftxui;
        if (modal_cancel(event)) { mode = Mode::panels; return true; }
        const auto count = file_attributes().size() + attribute_buttons.size();
        if (event.is_mouse()) {
            auto copy = event;
            const auto& mouse = copy.mouse();
            if (mouse.button != Mouse::Left || mouse.motion != Mouse::Pressed) return true;
            for (std::size_t index = 0; index < attribute_boxes.size(); ++index) {
                if (attribute_viewport.Contain(mouse.x, mouse.y) && attribute_boxes[index].Contain(mouse.x, mouse.y)) {
                    attribute_focus = index;
                    attribute_value ^= file_attributes()[index].bit;
                    return true;
                }
            }
            for (std::size_t index = 0; index < attribute_buttons.size(); ++index) {
                if (attribute_buttons[index].Contain(mouse.x, mouse.y)) { apply_file_attributes(index); return true; }
            }
            return true;
        }
        if (event == Event::Tab || event == Event::ArrowDown) attribute_focus = (attribute_focus + 1U) % count;
        else if (event == Event::TabReverse || event == Event::ArrowUp) attribute_focus = (attribute_focus + count - 1U) % count;
        else if (event == Event::Character(' ') && attribute_focus < 14U) attribute_value ^= file_attributes()[attribute_focus].bit;
        else if (event == Event::Return || event == Event::Character(' ')) {
            apply_file_attributes(attribute_focus < 14U ? attribute_buttons.size() - 2U : attribute_focus - 14U);
        }
        return true;
    }

    void report_local_command_error(const std::string& message) {
        mode = local_command_return;
        if (local_command_return == Mode::editor) {
            show_editor_message("Local command", message);
        } else if (local_command_return == Mode::editor_mail_dialog) {
            mail_error = message;
        } else if (local_command_return == Mode::editor_spelling_dialog) {
            spelling_error = message;
        } else if (local_command_return == Mode::viewer) {
            viewer_message_title = "Local command";
            viewer_message = message;
            mode = Mode::viewer_message;
        } else if (local_command_return == Mode::user_menu) {
            user_menu_error = message;
        } else {
            status = "Error: " + message;
        }
    }

    void start_local_task(
        std::string title,
        std::function<CommandOutput(const Cancellation&)> task,
        std::function<void(CommandOutput)> complete,
        const Mode return_mode = Mode::panels
    ) {
        if (local_command_running || traversal_running.load(std::memory_order_acquire)) {
            throw std::runtime_error("another operation is still running");
        }
        if (local_command_thread.joinable()) local_command_thread.join();
        local_command_title = std::move(title);
        local_command_return = return_mode;
        local_command_cancellation = Cancellation{};
        const auto cancellation = local_command_cancellation;
        local_command_running = true;
        mode = Mode::local_command;
        try {
        local_command_thread = std::jthread(
            [this, cancellation, task = std::move(task), complete = std::move(complete)] {
                try {
                    auto result = task(cancellation);
                    post_ui([this, complete, result = std::move(result)]() mutable {
                        local_command_running = false;
                        mode = local_command_return;
                        auto output = std::make_shared<CommandOutput>(std::move(result));
                        static_cast<void>(run_editor_action([this, complete, output] {
                            try {
                                complete(*output);
                            } catch (const std::exception& error) {
                                report_local_command_error(error.what());
                            }
                            return true;
                        }));
                    });
                } catch (const OperationCancelled&) {
                    post_ui([this] {
                        local_command_running = false;
                        mode = local_command_return;
                        status = local_command_title + " cancelled";
                    });
                } catch (const std::exception& error) {
                    const auto message = std::string(error.what());
                    post_ui([this, message] {
                        local_command_running = false;
                        report_local_command_error(message);
                    });
                }
            }
        );
        } catch (...) {
            local_command_running = false;
            mode = return_mode;
            throw;
        }
    }

    ftxui::Element render_local_command() {
        using namespace ftxui;
        return gray_dialog(vbox({
            text(display_text(local_command_title)) | bold | center,
            separator(),
            text(local_command_cancellation.requested() ? "Cancelling..." : "Running..."),
            separator(),
            dialog_button("Cancel", true, local_command_cancel_box) | center,
        }) | borderStyled(LIGHT) | size(WIDTH, LESS_THAN, std::min(72, Terminal::Size().dimx)));
    }

    bool handle_local_command(const ftxui::Event& event) {
        if (event == ftxui::Event::CtrlC) {
            local_command_cancellation.request();
            return true;
        }
        std::size_t selected = 0;
        const auto input = handle_button_input(event, selected,
            std::span<const ftxui::Box>(&local_command_cancel_box, 1U));
        if (input == ButtonInput::cancel || input == ButtonInput::activate) {
            local_command_cancellation.request();
        }
        return true;
    }

    std::size_t panel_config_index(const Pane& pane) const {
        return &pane == &left ? 0U : 1U;
    }

    bool panel_search_case_sensitive() const {
        return config.quick_search_mode == 1U ||
               (config.quick_search_mode == 2U && (active_index == 0 ? left : right).sort_case_sensitive());
    }

    std::string format_size(const std::uint64_t value) const {
        return ::mcpp::format_size(value, config.use_si_units);
    }

    const std::string& skin_id() const {
        return config.theme_override ? *config.theme_override : config.theme;
    }

    static std::shared_ptr<const PaletteSet> make_palette(Skin skin) {
        auto result = std::make_shared<PaletteSet>();
        result->panels = std::move(skin);
        result->editor = result->viewer = result->help = result->diff = result->panels;
        if (result->panels.resource.empty()) return result;
        const auto adapt = [&](Skin& target, const std::string_view section) {
            target.normal = skin_color(target, section, "_default_", target.normal);
            target.syntax[0] = target.normal.foreground;
        };
        adapt(result->editor, "editor");
        result->editor.selected = skin_color(result->editor, "editor", "editmarked", result->editor.selected);
        result->editor.search = skin_color(result->editor, "editor", "editbold", result->editor.search);
        result->editor.dim = skin_color(result->editor, "editor", "editwhitespace", result->editor.normal).foreground;
        adapt(result->viewer, "viewer");
        adapt(result->help, "help");
        adapt(result->diff, "diffviewer");
        return result;
    }

    void initialize_palette() {
        active_palette = make_palette(find_skin("grayscale"));
        try {
            active_palette = make_palette(load_skin(config.theme, default_config_path().parent_path()));
        } catch (const std::exception& error) {
            std::cerr << "mcpp: skin: " << error.what() << '\n';
            set_error(error);
        }
        if (!config.theme_override) {
            for (const auto* name : {"MCPP_SKIN", "MC_SKIN"}) {
                if (const auto* value = std::getenv(name); value && *value) {
                    config.theme_override = value;
                    break;
                }
            }
        }
        if (config.theme_override) {
            active_palette = make_palette(load_skin(*config.theme_override, default_config_path().parent_path()));
        }
        if (!active_palette->panels.warning.empty()) status = "Skin: " + active_palette->panels.warning;
    }

    void reload_skin_resources() {
        const auto directory = default_config_path().parent_path();
        auto candidate = make_palette(load_skin(active_palette->panels.id, directory));
        auto rules = FileHighlightRules::load(directory, highlight_rules.resource());
        active_palette = std::move(candidate);
        highlight_rules = std::move(rules);
    }

    const Skin& palette() const {
        switch (palette_context) {
            case PaletteContext::editor: return active_palette->editor;
            case PaletteContext::viewer: return active_palette->viewer;
            case PaletteContext::help: return active_palette->help;
            case PaletteContext::diff: return active_palette->diff;
            case PaletteContext::panels: return active_palette->panels;
        }
        return active_palette->panels;
    }

    ftxui::Element paint(ftxui::Element element, const SkinPair& colors) const {
        if (colors.explicit_style) return std::make_shared<SkinPaint>(std::move(element), colors);
        return std::move(element) | ftxui::color(colors.foreground) | ftxui::bgcolor(colors.background);
    }

    ftxui::Element highlight(ftxui::Element element, const SkinPair& colors) const {
        auto reversed = colors;
        std::swap(reversed.foreground, reversed.background);
        return paint(std::move(element), reversed) | ftxui::inverted;
    }

    ftxui::Element focus_dialog(ftxui::Element element) const {
        return highlight(std::move(element), palette().dialog_focus) | ftxui::focus;
    }

    ftxui::Element dialog_button(const std::string& label, const bool focused, ftxui::Box& box) const {
        auto button = ::mcpp::dialog_button(label, false, box);
        if (!focused) return button;
        return highlight(std::move(button), mode == Mode::editor_error ? palette().error_focus
                                                                     : palette().dialog_focus) | ftxui::focus;
    }

    ftxui::Element native_button(
        const std::string& label, const bool primary, const bool focused, ftxui::Box& box
    ) const {
        auto button = ::mcpp::native_button(label, primary, false, box);
        return focused ? focus_dialog(std::move(button)) : button;
    }

    ftxui::Element gray_dialog(ftxui::Element element) const {
        return paint(std::move(element), palette().dialog);
    }

    ftxui::Element gray_menu(ftxui::Element element) const {
        return paint(std::move(element), palette().menu);
    }

    ftxui::Element gray_selected(ftxui::Element element, const bool marked, const bool active) const {
        const auto& skin = palette();
        auto selected = paint(std::move(element), active
            ? (marked ? skin.marked_selected : skin.selected)
            : (marked ? skin.inactive_marked_selected : skin.inactive_selected));
        return marked && skin.resource.empty() ? selected | ftxui::bold : selected;
    }

    ftxui::Element gray_entry(
        ftxui::Element element, const Entry& entry, const bool type_colors, const bool local_entry
    ) const {
        using namespace ftxui;
        const auto& skin = palette();
        if (const auto matched = highlight_rules.color_for(entry, skin, local_entry)) {
            return paint(std::move(element), *matched);
        }
        if (!skin.resource.empty()) {
            const auto group = entry.kind == EntryKind::directory ? "directory" :
                entry.kind == EntryKind::symlink ? "symlink" : entry.kind == EntryKind::other ? "special" :
                (entry.permissions & (std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec |
                                      std::filesystem::perms::others_exec)) != std::filesystem::perms::none
                    ? "executable" : "_default_";
            return paint(std::move(element), skin_color(skin, "filehighlight", group, skin.normal));
        }
        if (entry.kind == EntryKind::directory) {
            return std::move(element) | color(type_colors ? Color::RGB(95, 160, 255) : skin.directory);
        }
        if (entry.kind == EntryKind::symlink) {
            return std::move(element) | color(type_colors ? Color::RGB(95, 215, 215) : skin.symlink);
        }
        if (entry.kind == EntryKind::other) return std::move(element) | color(skin.special);
        const bool executable = (entry.permissions & (std::filesystem::perms::owner_exec |
            std::filesystem::perms::group_exec | std::filesystem::perms::others_exec)) != std::filesystem::perms::none;
        return executable ? std::move(element) |
            color(type_colors ? Color::RGB(95, 215, 135) : skin.executable) | underlined
            : std::move(element) | color(skin.normal.foreground);
    }

    ftxui::Element with_dialog_shadow(ftxui::Element element) const {
        return config.shadows ? std::make_shared<DialogShadow>(std::move(element), palette().shadow) : element;
    }

    std::string display_filename(const Pane& pane, const std::string_view value) const {
        const auto& encoding = config.panel_encoding[panel_config_index(pane)];
        if (encoding.empty() || encoding == "UTF-8") return display_text(value);
        std::string converted;
        constexpr char hex[] = "0123456789ABCDEF";
        for (const auto character : value) {
            const auto byte = static_cast<unsigned char>(character);
            if (byte < 128U) {
                converted += character;
            } else {
                try {
                    converted += convert_encoding(std::string(1, character), encoding, "UTF-8");
                } catch (const std::system_error& error) {
                    if (error.code().value() != EILSEQ && error.code().value() != EINVAL) throw;
                    converted += "\\x";
                    converted += hex[byte >> 4U];
                    converted += hex[byte & 15U];
                }
            }
        }
        return display_text(converted);
    }

    std::size_t panel_page_rows(const Pane& pane) const {
        return panel_visible_rows(&pane == &left ? 0 : 1) * pane_listing_spec(pane).columns;
    }

    ListingSpec pane_listing_spec(const Pane& pane) const {
        const auto id = panel_config_index(pane);
        return parse_listing_format(listing_format(config.panel_listing[id],
                                    config.panel_brief_columns[id], config.panel_columns[id]));
    }

    ftxui::Element render_panel_pair(ftxui::Elements panes) const {
        if (config.layout_horizontal) return ftxui::vbox(std::move(panes));
        if (!listing_full_width(0) && !listing_full_width(1)) return ftxui::hbox(std::move(panes));
        auto focused = active_index;
        if (tree_panel && tree_panel_focused) focused = *tree_panel;
        if (quick_view_panel && quick_view_focused) focused = *quick_view_panel;
        if (information_panel && information_panel_focused) focused = *information_panel;
        const auto positioned = [&](const int index) {
            const auto x = index == 0 || listing_full_width(index) ? 0 : split_left_width();
            return ftxui::hbox({
                ftxui::filler() | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, x),
                panes[static_cast<std::size_t>(index)], ftxui::filler()
            });
        };
        return ftxui::dbox({positioned(1 - focused), positioned(focused)});
    }

    void show_listing_options() {
        const auto id = panel_config_index(active());
        listing_choice = static_cast<std::size_t>(config.panel_listing[id]);
        listing_focus = 0;
        listing_brief_value = std::to_string(config.panel_brief_columns[id]);
        listing_user_value = config.panel_columns[id];
        listing_user_mini = config.panel_user_mini_status[id];
        listing_status_values = config.panel_status_formats[id];
        listing_brief_input.reset(listing_brief_value, true);
        listing_user_input.reset(listing_user_value, true);
        listing_status_input.reset(listing_status_values[listing_choice], true);
        listing_error.clear();
        mode = Mode::listing_dialog;
    }

    void show_native_input(
        std::string title, std::string label, std::string value,
        std::function<void(const std::string&)> apply, const Mode return_mode = Mode::panels,
        std::string directory = {}, const std::string_view history_id = {}
    ) {
        native_input_title = std::move(title);
        native_input_label = std::move(label);
        native_input_value = std::move(value);
        bind_input_history(native_input_state, history_id);
        native_input_state.reset(native_input_value, true);
        native_input_error.clear();
        native_input_directory = std::move(directory);
        native_input_focus = 0;
        native_input_return = return_mode;
        native_input_apply = std::move(apply);
        mode = Mode::native_input;
    }

    ftxui::Element render_native_input() {
        using namespace ftxui;
        Elements rows{
            text(" " + native_input_label),
            hbox({text(" "), render_input(native_input_value, native_input_state,
                                          native_input_focus == 0U, "") |
                              reflect(native_input_boxes[0]), text(" ")})
        };
        if (!native_input_error.empty()) rows.push_back(text(" " + display_text(native_input_error)));
        const auto width = std::max({Terminal::Size().dimx / 2 - 2,
                                    string_width(native_input_title) + 2,
                                    string_width(native_input_label) + 4});
        return native_window(native_input_title, vbox({
            vbox(std::move(rows)),
            separator(),
            hbox({native_button("OK", true, native_input_focus == 1U, native_input_boxes[1]),
                  text(" "), native_button("Cancel", false, native_input_focus == 2U, native_input_boxes[2])}) | center,
        }), std::min(width, std::max(1, Terminal::Size().dimx - 2)));
    }

    bool handle_native_input(const ftxui::Event& event) {
        using namespace ftxui;
        if (event == Event::Special("\x1B\t") && native_input_focus == 0U &&
            !native_input_directory.empty()) {
            try {
                complete_path(native_input_value, native_input_state, native_input_directory,
                              native_input_return == Mode::panels ? active().provider() : local,
                              false, native_input_return != Mode::panels ||
                                  is_archive_location(native_input_directory));
                native_input_error = native_input_state.message;
            } catch (const std::exception& error) {
                native_input_error = error.what();
            }
            return true;
        }
        if (modal_cancel(event)) { mode = native_input_return; return true; }
        if (event.is_mouse()) {
            auto copy = event;
            const auto& mouse = copy.mouse();
            if (native_input_state.dragging) return input_mouse(native_input_value, native_input_state, mouse);
            if (mouse.button != Mouse::Left || mouse.motion != Mouse::Pressed) return true;
            for (std::size_t index = 0; index < native_input_boxes.size(); ++index) {
                if (!native_input_boxes[index].Contain(mouse.x, mouse.y)) continue;
                native_input_focus = index;
                if (index == 0U) return input_mouse(native_input_value, native_input_state, mouse);
                return handle_native_input(Event::Return);
            }
            return true;
        }
        if (event == Event::Tab || event == Event::TabReverse) {
            native_input_focus = event == Event::Tab ? (native_input_focus + 1U) % 3U
                                                     : (native_input_focus + 2U) % 3U;
            return true;
        }
        if (event == Event::Return || (event == Event::Character(' ') && native_input_focus != 0U)) {
            if (native_input_focus == 2U) { mode = native_input_return; return true; }
            const auto apply = native_input_apply;
            const auto value = native_input_value;
            const auto return_mode = native_input_return;
            try {
                if (!native_input_state.history_id.empty()) remember_input(native_input_state, value);
                mode = return_mode;
                apply(value);
            } catch (const std::exception& error) {
                native_input_error = error.what();
                mode = Mode::native_input;
            }
            return true;
        }
        return native_input_focus == 0U ? edit_input(native_input_value, native_input_state, event) : true;
    }

    void show_key_query(
        std::string title, std::string label, const bool cancel,
        std::function<void(const ftxui::Event&)> apply, const Mode return_mode = Mode::editor
    ) {
        key_query_title = std::move(title);
        key_query_label = std::move(label);
        key_query_cancel = cancel;
        key_query_apply = std::move(apply);
        key_query_return = return_mode;
        mode = Mode::native_key_query;
    }

    ftxui::Element render_key_query() {
        using namespace ftxui;
        Elements rows{hbox({text(" " + key_query_label + " "), text(" ") | focusCursorBarBlinking})};
        if (key_query_cancel) {
            rows.push_back(separator());
            rows.push_back(native_button("Cancel", false, false, key_query_cancel_box) | center);
        }
        const auto width = std::max(string_width(key_query_title) + 4, string_width(key_query_label) + 7);
        return native_window(key_query_title, vbox(std::move(rows)), width);
    }

    bool handle_key_query(const ftxui::Event& event) {
        if (key_query_cancel && event == ftxui::Event::Escape) {
            mode = key_query_return;
            return true;
        }
        if (event.is_mouse()) {
            auto copy = event;
            if (key_query_cancel && copy.mouse().button == ftxui::Mouse::Left &&
                copy.mouse().motion == ftxui::Mouse::Pressed &&
                key_query_cancel_box.Contain(copy.mouse().x, copy.mouse().y)) mode = key_query_return;
            return true;
        }
        if (event.input().empty() || event.is_cursor_position() || event.is_cursor_shape() ||
            event.IsTerminalNameVersion() || event.IsTerminalEmulator() || event.IsTerminalCapabilities()) return true;
        const auto apply = key_query_apply;
        mode = key_query_return;
        apply(event);
        return true;
    }

    void show_native_message(
        std::string title, std::string message, const Mode return_mode = Mode::panels
    ) {
        native_message_title = std::move(title);
        native_message_text = std::move(message);
        native_message_return = return_mode;
        mode = Mode::native_message;
    }

    ftxui::Element render_native_message() {
        using namespace ftxui;
        return native_window(native_message_title, vbox({
            paragraph(display_text(native_message_text)),
            separator(),
            native_button("OK", true, true, native_message_button) | center,
        }), std::min(68, std::max(1, Terminal::Size().dimx - 2)));
    }

    bool handle_native_message(const ftxui::Event& event) {
        std::size_t focus = 0;
        const auto result = handle_button_input(event, focus,
            std::span<const ftxui::Box>(&native_message_button, 1U));
        if (result == ButtonInput::cancel || result == ButtonInput::activate) mode = native_message_return;
        return true;
    }

    void show_native_query(
        std::string title, std::string message, std::vector<std::string> buttons,
        std::function<void(std::size_t)> apply, const Mode return_mode = Mode::panels
    ) {
        native_query_title = std::move(title);
        native_query_text = std::move(message);
        native_query_labels = std::move(buttons);
        native_query_boxes.assign(native_query_labels.size(), {});
        native_query_choice = 0;
        native_query_return = return_mode;
        native_query_apply = std::move(apply);
        mode = Mode::native_query;
    }

    ftxui::Element render_native_query() {
        using namespace ftxui;
        Elements paragraphs, buttons;
        std::istringstream input(native_query_text);
        std::string line;
        while (std::getline(input, line)) paragraphs.push_back(paragraph(display_text(line)));
        for (std::size_t index = 0; index < native_query_labels.size(); ++index) {
            if (index != 0U) buttons.push_back(text(" "));
            buttons.push_back(native_button(native_query_labels[index], index == 0U,
                native_query_choice == index, native_query_boxes[index]));
        }
        return native_window(native_query_title, vbox({
            vbox(std::move(paragraphs)), separator(), hbox(std::move(buttons)) | center
        }), std::min(68, std::max(1, Terminal::Size().dimx - 2)));
    }

    bool handle_native_query(const ftxui::Event& event) {
        std::string hotkeys;
        for (const auto& label : native_query_labels) {
            hotkeys += label.empty() ? '\0' : static_cast<char>(std::tolower(static_cast<unsigned char>(label.front())));
        }
        const auto input = handle_button_input(event, native_query_choice, native_query_boxes, hotkeys);
        if (input == ButtonInput::cancel) {
            mode = native_query_return;
        } else if (input == ButtonInput::activate) {
            const auto apply = native_query_apply;
            const auto selected = native_query_choice;
            mode = native_query_return;
            apply(selected);
        }
        return input != ButtonInput::ignored;
    }

    static const std::array<SortField, 9>& native_sort_fields() {
        static const std::array<SortField, 9> fields{
            SortField::unsorted, SortField::name, SortField::version, SortField::extension,
            SortField::size, SortField::modified, SortField::accessed, SortField::changed, SortField::inode
        };
        return fields;
    }

    void show_sort_options() {
        const auto& fields = native_sort_fields();
        sort_choice = static_cast<std::size_t>(std::ranges::find(fields, active().sort_field()) - fields.begin());
        sort_focus = 0;
        sort_executable_first = active().executable_first();
        sort_case_sensitive = active().sort_case_sensitive();
        sort_reverse = active().descending();
        mode = Mode::sort_dialog;
    }

    ftxui::Element render_listing_dialog() {
        using namespace ftxui;
        static const std::array<std::string, 4> labels{
            "Full file list", "Brief file list:", "Long file list", "User defined:"
        };
        Elements rows;
        for (std::size_t index = 0; index < labels.size(); ++index) {
            auto label = hbox({text(listing_choice == index ? "(*) " : "( ) "),
                               mnemonic_label(labels[index], 0)});
            if (listing_focus == 0U && listing_choice == index) label = focus_dialog(std::move(label));
            label = label | reflect(listing_boxes[index]);
            if (index == 1U) {
                auto columns = render_input(listing_brief_value, listing_brief_input,
                                            listing_focus == 1U, "");
                if (listing_choice != 1U) columns = columns | dim | color(palette().dim);
                rows.push_back(hbox({
                    text(" "), label | size(WIDTH, EQUAL, 21),
                    columns | size(WIDTH, EQUAL, 13) | reflect(listing_boxes[4]),
                    text("columns")
                }));
            } else {
                rows.push_back(hbox({text(" "), std::move(label)}));
            }
        }
        auto user = render_input(listing_user_value, listing_user_input, listing_focus == 2U, "");
        if (listing_choice != 3U) user = user | dim | color(palette().dim);
        rows.push_back(hbox({text(" "), std::move(user) | reflect(listing_boxes[5]), text(" ")}));
        rows.push_back(separator());
        auto mini = hbox({text(listing_user_mini ? "[x] " : "[ ] "),
                          mnemonic_label("User mini status", 5)});
        if (listing_focus == 3U) mini = focus_dialog(std::move(mini));
        rows.push_back(hbox({text(" "), std::move(mini) | reflect(listing_boxes[6])}));
        auto mini_input = render_input(listing_status_values[listing_choice], listing_status_input,
                                       listing_focus == 4U, "");
        if (!listing_user_mini) mini_input = mini_input | dim | color(palette().dim);
        rows.push_back(hbox({text(" "), std::move(mini_input) | reflect(listing_boxes[7]), text(" ")}));
        if (!listing_error.empty()) rows.push_back(text(display_text(listing_error)) | bold);
        return native_window("Listing format", vbox({
            vbox(std::move(rows)) | yframe |
                size(HEIGHT, LESS_THAN, std::max(1, Terminal::Size().dimy - 6)) |
                reflect(listing_viewport),
            separator(),
            hbox({
                native_button("OK", true, listing_focus == 5U, listing_boxes[8]),
                text(" "),
                native_button("Cancel", false, listing_focus == 6U, listing_boxes[9])
            }) | center,
        }), std::min(46, Terminal::Size().dimx));
    }

    bool handle_listing_dialog(const ftxui::Event& event) {
        using namespace ftxui;
        const auto enabled = [&](const std::size_t focus) {
            return (focus != 1U || listing_choice == 1U) && (focus != 2U || listing_choice == 3U) &&
                   (focus != 4U || listing_user_mini);
        };
        const auto select_mode = [&](const std::size_t index) {
            listing_choice = index;
            listing_status_input.reset(listing_status_values[index], true);
        };
        const auto move_focus = [&](const bool next) {
            do {
                listing_focus = next ? (listing_focus + 1U) % 7U : (listing_focus + 6U) % 7U;
            } while (!enabled(listing_focus));
        };
        if (modal_cancel(event)) {
            mode = Mode::panels;
            return true;
        }
        bool accept = false;
        if (event.is_mouse()) {
            auto copy = event;
            const auto& mouse = copy.mouse();
            if (listing_focus == 1U && listing_brief_input.dragging) {
                return input_mouse(listing_brief_value, listing_brief_input, mouse);
            }
            if (listing_focus == 2U && listing_user_input.dragging) {
                return input_mouse(listing_user_value, listing_user_input, mouse);
            }
            if (listing_focus == 4U && listing_status_input.dragging) {
                return input_mouse(listing_status_values[listing_choice], listing_status_input, mouse);
            }
            if (mouse.button != Mouse::Left || mouse.motion != Mouse::Pressed) return true;
            for (std::size_t index = 0; index < listing_boxes.size(); ++index) {
                if (index < 8U && !listing_viewport.Contain(mouse.x, mouse.y)) continue;
                if (!listing_boxes[index].Contain(mouse.x, mouse.y)) continue;
                if (index < 4U) {
                    listing_focus = 0;
                    select_mode(index);
                } else if (index == 4U && listing_choice == 1U) {
                    listing_focus = 1;
                    input_mouse(listing_brief_value, listing_brief_input, mouse);
                } else if (index == 5U && listing_choice == 3U) {
                    listing_focus = 2;
                    input_mouse(listing_user_value, listing_user_input, mouse);
                }
                else if (index == 6U) {
                    listing_user_mini = !listing_user_mini;
                    listing_focus = 3;
                } else if (index == 7U && listing_user_mini) {
                    listing_focus = 4;
                    input_mouse(listing_status_values[listing_choice], listing_status_input, mouse);
                }
                else if (index == 8U) accept = true;
                else if (index == 9U) mode = Mode::panels;
                return accept ? apply_listing_dialog() : true;
            }
            return true;
        }
        if (event == Event::Tab || event == Event::TabReverse) {
            move_focus(event == Event::Tab);
            return true;
        }
        if (listing_focus == 0U && (event == Event::ArrowUp || event == Event::ArrowDown)) {
            select_mode(event == Event::ArrowDown ? (listing_choice + 1U) % 4U
                                                 : (listing_choice + 3U) % 4U);
            return true;
        }
        if (event == Event::Return || (event == Event::Character(' ') && listing_focus >= 5U)) {
            if (listing_focus == 6U) mode = Mode::panels;
            else apply_listing_dialog();
            return true;
        }
        if (listing_focus == 1U || listing_focus == 2U || listing_focus == 4U) {
            return edit_input(listing_focus == 1U ? listing_brief_value
                               : listing_focus == 2U ? listing_user_value
                                                    : listing_status_values[listing_choice],
                              listing_focus == 1U ? listing_brief_input
                               : listing_focus == 2U ? listing_user_input : listing_status_input, event);
        }
        if (listing_focus == 3U && event == Event::Character(' ')) {
            listing_user_mini = !listing_user_mini;
            return true;
        }
        if (event.is_character() && event.character().size() == 1U) {
            const auto key = static_cast<char>(std::tolower(static_cast<unsigned char>(event.character()[0])));
            const auto index = std::string_view("fblu").find(key);
            if (index != std::string_view::npos) {
                select_mode(index);
                listing_focus = 0;
            } else if (key == 'm') {
                listing_user_mini = !listing_user_mini;
                listing_focus = 3;
            }
        }
        return true;
    }

    bool apply_listing_dialog() {
        try {
            const auto id = panel_config_index(active());
            auto columns = config.panel_brief_columns[id];
            if (listing_choice == 1U) {
                const auto parsed = std::from_chars(listing_brief_value.data(),
                    listing_brief_value.data() + listing_brief_value.size(), columns);
                if (parsed.ec != std::errc{} || parsed.ptr != listing_brief_value.data() + listing_brief_value.size() ||
                    columns < 1U || columns > 9U) throw std::runtime_error("Brief columns must be 1 through 9");
            }
            if (listing_choice == 3U) static_cast<void>(parse_listing_format(listing_user_value));
            if (listing_user_mini) static_cast<void>(parse_listing_format(listing_status_values[listing_choice]));
            if (listing_choice == 1U) remember_input(listing_brief_input, listing_brief_value);
            if (listing_choice == 3U) remember_input(listing_user_input, listing_user_value);
            if (listing_user_mini) remember_input(listing_status_input, listing_status_values[listing_choice]);
            config.panel_listing[id] = static_cast<ListingFormat>(listing_choice);
            config.panel_brief_columns[id] = columns;
            config.panel_columns[id] = listing_user_value;
            config.panel_user_mini_status[id] = listing_user_mini;
            config.panel_status_formats[id] = listing_status_values;
            mode = Mode::panels;
            status = "Listing format changed";
        } catch (const std::exception& error) {
            listing_error = error.what();
        }
        return true;
    }

    ftxui::Element render_sort_dialog() {
        using namespace ftxui;
        static const std::array<std::string, 9> names{
            "Unsorted", "Name", "Version", "Extension", "Size", "Modify time", "Access time", "Change time", "Inode"
        };
        static constexpr std::array<std::size_t, 9> mnemonics{0, 0, 0, 1, 0, 0, 0, 1, 0};
        Elements rows;
        for (std::size_t index = 0; index < names.size(); ++index) {
            auto radio = hbox({text(sort_choice == index ? "(*) " : "( ) "),
                               mnemonic_label(names[index], mnemonics[index])}) |
                         size(WIDTH, EQUAL, 21) | reflect(sort_boxes[index]);
            if (sort_focus == 0U && sort_choice == index) radio = focus_dialog(std::move(radio));
            Element checkbox = text("");
            if (index < 3U) {
                const bool checked = index == 0U ? sort_executable_first : index == 1U ? sort_case_sensitive : sort_reverse;
                const std::string label = index == 0U ? "Executable first" : index == 1U ? "Case sensitive" : "Reverse";
                checkbox = hbox({text(checked ? "[x] " : "[ ] "),
                                  mnemonic_label(label, index == 0U ? 11U : index == 1U ? 3U : 0U)}) |
                           reflect(sort_boxes[9U + index]);
                if (sort_focus == index + 1U) checkbox = focus_dialog(std::move(checkbox));
            }
            rows.push_back(hbox({text(" "), std::move(radio), std::move(checkbox)}));
        }
        return native_window("Sort order", vbox({
            vbox(std::move(rows)) | yframe |
                size(HEIGHT, LESS_THAN, std::max(1, Terminal::Size().dimy - 6)) | reflect(sort_viewport),
            separator(),
            hbox({native_button("OK", true, sort_focus == 4U, sort_boxes[12]), text(" "),
                  native_button("Cancel", false, sort_focus == 5U, sort_boxes[13])}) | center,
        }), std::min(45, Terminal::Size().dimx));
    }

    bool handle_sort_dialog(const ftxui::Event& event) {
        using namespace ftxui;
        const auto toggle = [&](const std::size_t index) {
            if (index == 1U) sort_executable_first = !sort_executable_first;
            else if (index == 2U) sort_case_sensitive = !sort_case_sensitive;
            else sort_reverse = !sort_reverse;
        };
        if (modal_cancel(event)) { mode = Mode::panels; return true; }
        if (event.is_mouse()) {
            auto copy = event;
            const auto& mouse = copy.mouse();
            if (mouse.button != Mouse::Left || mouse.motion != Mouse::Pressed) return true;
            for (std::size_t index = 0; index < sort_boxes.size(); ++index) {
                if (index < 12U && !sort_viewport.Contain(mouse.x, mouse.y)) continue;
                if (!sort_boxes[index].Contain(mouse.x, mouse.y)) continue;
                if (index < 9U) { sort_choice = index; sort_focus = 0; }
                else if (index < 12U) { sort_focus = index - 8U; toggle(sort_focus); }
                else if (index == 12U) apply_sort_dialog();
                else mode = Mode::panels;
                return true;
            }
            return true;
        }
        if (event == Event::Tab || event == Event::TabReverse) {
            sort_focus = event == Event::Tab ? (sort_focus + 1U) % 6U : (sort_focus + 5U) % 6U;
        } else if (sort_focus == 0U && (event == Event::ArrowUp || event == Event::ArrowDown)) {
            sort_choice = event == Event::ArrowDown ? (sort_choice + 1U) % 9U : (sort_choice + 8U) % 9U;
        } else if (event == Event::Return || (event == Event::Character(' ') && sort_focus >= 4U)) {
            if (sort_focus == 5U) mode = Mode::panels;
            else apply_sort_dialog();
        } else if (sort_focus >= 1U && sort_focus <= 3U && event == Event::Character(' ')) {
            toggle(sort_focus);
        } else if (event.is_character() && event.character().size() == 1U) {
            const auto key = static_cast<char>(std::tolower(static_cast<unsigned char>(event.character()[0])));
            static const std::string keys = "unvxsmahi";
            if (const auto selected = keys.find(key); selected != std::string::npos) { sort_choice = selected; sort_focus = 0; }
            else if (key == 'f') toggle(1);
            else if (key == 'e') toggle(2);
            else if (key == 'r') toggle(3);
        }
        return true;
    }

    void apply_sort_dialog() {
        active().set_sort_options(native_sort_fields()[sort_choice], sort_reverse,
                                  sort_case_sensitive, sort_executable_first);
        store_panel_options_in_config();
        mode = Mode::panels;
        status = "Sort order changed";
    }

    void show_filename_encoding() {
        const auto id = panel_config_index(active());
        show_codepage_chooser(config.panel_encoding[id], Mode::panels, [this, id](const std::string& codepage) {
            config.panel_encoding[id] = codepage;
            status = "Filename display encoding changed; paths were not renamed";
        });
    }

    void show_codepage_chooser(
        const std::string& current, const Mode return_mode,
        std::function<void(const std::string&)> apply
    ) {
        std::vector<std::string> names{"-  < No translation >"};
        std::vector<std::string> values{""};
        std::string hotkeys{"-"};
        const auto& pages = supported_codepages();
        for (std::size_t index = 0; index < pages.size(); ++index) {
            const auto hotkey = static_cast<char>(index < 10U ? '0' + index : 'a' + index - 10U);
            hotkeys += hotkey;
            names.push_back(std::string(1, hotkey) + "  " + pages[index].name);
            values.push_back(pages[index].id);
        }
        show_local_list("Choose codepage", std::move(names),
            [values, apply](const std::size_t selected) { apply(values.at(selected)); }, return_mode);
        local_list_native = true;
        local_list_hotkeys = std::move(hotkeys);
        const auto found = std::ranges::find(values, current);
        local_list_cursor = found == values.end() ? 0U : static_cast<std::size_t>(found - values.begin());
    }

    void show_settings(const int action) {
        if (action == 20) {
            configure_values = {config.verbose_operation, config.compute_totals, config.classic_progressbar,
                config.mkdir_autoname, config.preallocate_space, config.single_escape,
                config.use_internal_edit, config.use_internal_view, config.ask_new_filename,
                config.auto_menus, config.drop_menus, config.shell_patterns, config.completion_show_all,
                config.rotating_dash, config.cd_follows_links, config.safe_delete,
                config.safe_overwrite, config.auto_save_setup};
            configure_pause = config.pause_after_run;
            configure_timeout = std::to_string(config.escape_timeout);
            configure_timeout_input.reset(configure_timeout, true);
            configure_focus = 0;
            configure_error.clear();
            mode = Mode::configure_dialog;
        } else if (action == 21) {
            layout_horizontal = config.layout_horizontal;
            layout_equal = config.split_is_equal();
            layout_left_size = split_left_width();
            layout_top_size = split_top_height();
            layout_visible = {config.menubar_visible, config.command_prompt, config.keybar_visible,
                              config.hintbar_visible, config.xterm_title, config.show_free_space};
            layout_focus = 0;
            mode = Mode::layout_dialog;
        } else if (action == 22) {
            panel_option_values = {config.show_mini_status, config.use_si_units, !config.directories_first,
                config.show_backups, config.show_hidden, config.fast_reload, config.mark_moves_down,
                config.reverse_files_only, config.simple_swap, config.auto_save_panels_setup,
                config.navigate_with_arrows, config.scroll_pages, config.scroll_center,
                config.mouse_page_scrolling, config.highlight_file_types, config.highlight_permissions};
            panel_option_search = config.quick_search_mode;
            panel_option_focus = 0;
            mode = Mode::panel_options_dialog;
        } else if (action == 23) {
            confirmation_values = {config.confirm_delete, config.confirm_overwrite,
                config.confirm_execute, config.confirm_exit, config.confirm_hotlist_delete,
                config.confirm_history_cleanup};
            confirmation_focus = 0;
            mode = Mode::confirmation_dialog;
        } else if (action == 24) {
            appearance_original_skin = config.theme;
            appearance_original_override = config.theme_override;
            appearance_original_palette = active_palette;
            appearance_original_shadows = config.shadows;
            appearance_skin = palette().label;
            appearance_shadows = config.shadows;
            appearance_focus = 0;
            mode = Mode::appearance_dialog;
        } else if (action == 25) {
            display_bits_focus = 0;
            mode = Mode::display_bits_dialog;
        } else {
            throw std::runtime_error("unknown settings action");
        }
    }

    ftxui::Element render_configure_dialog() {
        using namespace ftxui;
        static const std::array<std::string, 18> names{
            "Verbose operation", "Compute totals", "Classic progressbar", "Mkdir autoname", "Preallocate space",
            "Single press", "Use internal edit", "Use internal view", "Ask new file name", "Auto menus",
            "Drop down menus", "Shell patterns", "Complete: show all", "Rotating dash",
            "Cd follows links", "Safe delete", "Safe overwrite", "Auto save setup"
        };
        static constexpr std::array<std::size_t, 18> keys{0,12,11,4,0,1,16,16,1,6,0,1,2,10,14,2,99,1};
        const auto check = [&](const std::size_t index) {
            auto row = hbox({text(configure_values[index] ? " [x] " : " [ ] "), mnemonic_label(names[index], keys[index])}) |
                       reflect(configure_boxes[index]);
            if (configure_focus == index) row = focus_dialog(std::move(row));
            return row;
        };
        Elements file_options, other_options, pause;
        for (std::size_t index = 0; index < 5U; ++index) file_options.push_back(check(index));
        for (std::size_t index = 6; index < 18U; ++index) other_options.push_back(check(index));
        other_options.push_back(text(""));
        other_options.push_back(text(""));
        auto timeout = render_input(configure_timeout, configure_timeout_input, configure_focus == 18U, " Timeout: ") |
                       reflect(configure_boxes[18]);
        if (!configure_values[5]) timeout = timeout | dim | color(palette().dim);
        static const std::array<std::string, 3> pause_labels{"Never", "On dumb terminals", "Always"};
        static constexpr std::array<std::size_t, 3> pause_keys{0,6,4};
        for (std::size_t index = 0; index < 3U; ++index) {
            auto row = hbox({text(configure_pause == index ? " (*) " : " ( ) "),
                             mnemonic_label(pause_labels[index], pause_keys[index])}) |
                       reflect(configure_boxes[19U + index]);
            if (configure_focus == 19U && configure_pause == index) row = focus_dialog(std::move(row));
            pause.push_back(std::move(row));
        }
        Elements content{
            hbox({text(" "), vbox({
                window(text(" File operations "), vbox(std::move(file_options)), LIGHT),
                window(text(" Esc key mode "), vbox({check(5), timeout}), LIGHT),
                window(text(" Pause after run "), vbox(std::move(pause)), LIGHT),
            }) | size(WIDTH, EQUAL, 27), text(" "),
            window(text(" Other options "), vbox(std::move(other_options)), LIGHT) | size(WIDTH, EQUAL, 27),
            text(" ")})
        };
        if (!configure_error.empty()) content.push_back(text(" " + display_text(configure_error)));
        content.push_back(separator());
        content.push_back(hbox({
            native_button("OK", true, configure_focus == 20U, configure_boxes[22]), text(" "),
            native_button("Cancel", false, configure_focus == 21U, configure_boxes[23])
        }) | center);
        return native_window("Configure options", vbox(std::move(content)), 59);
    }

    void apply_configure_dialog() try {
        int timeout = 0;
        const auto parsed = std::from_chars(configure_timeout.data(), configure_timeout.data() + configure_timeout.size(), timeout);
        if (parsed.ec != std::errc{} || parsed.ptr != configure_timeout.data() + configure_timeout.size() || timeout < 0) {
            configure_error = "Timeout must be a nonnegative number of microseconds";
            return;
        }
        if (!configure_values[11]) {
            try {
                for (const auto* pane : {&left, &right}) {
                    if (!pane->filter().empty()) {
                        static_cast<void>(EditorSearch::find("", pane->filter(), 0, 0, 0,
                            {.mode=EditorSearchMode::regular_expression, .case_sensitive=true}, false, false));
                    }
                }
            } catch (const std::exception& error) {
                configure_error = error.what();
                return;
            }
        }
        remember_input(configure_timeout_input, configure_timeout);
        const auto& values = configure_values;
        config.verbose_operation = values[0]; config.compute_totals = values[1];
        config.classic_progressbar = values[2]; config.mkdir_autoname = values[3];
        config.preallocate_space = values[4]; config.single_escape = values[5];
        config.use_internal_edit = values[6]; config.use_internal_view = values[7];
        config.ask_new_filename = values[8]; config.auto_menus = values[9];
        config.drop_menus = values[10]; config.shell_patterns = values[11];
        config.completion_show_all = values[12]; config.rotating_dash = values[13];
        config.cd_follows_links = values[14]; config.safe_delete = values[15];
        config.safe_overwrite = values[16]; config.auto_save_setup = values[17];
        config.escape_timeout = timeout;
        config.pause_after_run = configure_pause;
        set_terminal_input_delay(config.single_escape ? static_cast<std::uint64_t>(timeout) : 50000U);
        escape_waiting = false;
        if (auto* provider = dynamic_cast<LocalProvider*>(local.get())) provider->set_preallocate(config.preallocate_space);
        for (auto* pane : {&left, &right}) {
            pane->set_shell_patterns(config.shell_patterns);
            pane->set_follow_links(config.cd_follows_links);
        }
        mode = Mode::panels;
        status = "Configuration changed";
    } catch (const std::exception& error) {
        configure_error = error.what();
        mode = Mode::configure_dialog;
    }

    bool handle_configure_dialog(const ftxui::Event& event) {
        using namespace ftxui;
        if (modal_cancel(event)) { mode = Mode::panels; return true; }
        if (event.is_mouse()) {
            auto copy = event;
            const auto& mouse = copy.mouse();
            if (configure_timeout_input.dragging) return input_mouse(configure_timeout, configure_timeout_input, mouse);
            if (mouse.button != Mouse::Left || mouse.motion != Mouse::Pressed) return true;
            for (std::size_t index = 0; index < configure_boxes.size(); ++index) {
                if (!configure_boxes[index].Contain(mouse.x, mouse.y)) continue;
                if (index < 18U) { configure_focus = index; configure_values[index] = !configure_values[index]; }
                else if (index == 18U && configure_values[5]) {
                    configure_focus = 18;
                    input_mouse(configure_timeout, configure_timeout_input, mouse);
                } else if (index >= 19U && index < 22U) { configure_focus = 19; configure_pause = index - 19U; }
                else if (index == 22U) apply_configure_dialog();
                else if (index == 23U) mode = Mode::panels;
                return true;
            }
            return true;
        }
        static constexpr std::array<std::size_t, 22> order{
            0,1,2,3,4,5,18,19,6,7,8,9,10,11,12,13,14,15,16,17,20,21
        };
        if (event == Event::Tab || event == Event::TabReverse) {
            auto index = static_cast<std::size_t>(std::ranges::find(order, configure_focus) - order.begin());
            do {
                index = event == Event::Tab ? (index + 1U) % order.size() : (index + order.size() - 1U) % order.size();
                configure_focus = order[index];
            } while (configure_focus == 18U && !configure_values[5]);
        } else if (configure_focus == 19U && (event == Event::ArrowUp || event == Event::ArrowDown)) {
            configure_pause = event == Event::ArrowDown ? (configure_pause + 1U) % 3U : (configure_pause + 2U) % 3U;
        } else if (event == Event::Return || (event == Event::Character(' ') && configure_focus >= 20U)) {
            if (configure_focus == 21U) mode = Mode::panels;
            else apply_configure_dialog();
        } else if (configure_focus == 18U) {
            return edit_input(configure_timeout, configure_timeout_input, event);
        } else if (configure_focus < 18U && event == Event::Character(' ')) {
            configure_values[configure_focus] = !configure_values[configure_focus];
        } else if (configure_focus == 19U && event == Event::Character(' ')) {
            return true;
        } else if (event.is_character() && event.character().size() == 1U) {
            const auto key = static_cast<char>(std::tolower(static_cast<unsigned char>(event.character()[0])));
            if (const auto index = std::string_view("vlgrpitwsedhmakf u").find(key); index != std::string_view::npos && index < 18U) {
                configure_values[index] = !configure_values[index];
                configure_focus = index;
            } else if (const auto pause_index = std::string_view("nby").find(key); pause_index != std::string_view::npos) {
                configure_pause = pause_index;
                configure_focus = 19;
            }
        }
        return true;
    }

    ftxui::Element render_appearance() {
        using namespace ftxui;
        auto skin = text("[ " + fit_listing_text(appearance_skin, 25, ListingAlignment::left) + " ]") |
                    reflect(appearance_boxes[0]);
        if (appearance_focus == 0U) skin = focus_dialog(std::move(skin));
        auto shadows = hbox({text(appearance_shadows ? " [x] " : " [ ] "), mnemonic_label("Shadows", 0)}) |
                       reflect(appearance_boxes[1]);
        if (appearance_focus == 1U) shadows = focus_dialog(std::move(shadows));
        return native_window("Appearance", vbox({
            hbox({text(" Skin:"), filler(), std::move(skin), text(" ")}),
            separator(), std::move(shadows), separator(),
            hbox({native_button("OK", true, appearance_focus == 2U, appearance_boxes[2]), text(" "),
                  native_button("Cancel", false, appearance_focus == 3U, appearance_boxes[3])}) | center,
        }), 53);
    }

    void choose_skin() {
        auto skins = available_skins(default_config_path().parent_path());
        const auto current = active_palette->panels.id;
        if (std::ranges::find(skins, current, &SkinChoice::id) == skins.end()) {
            skins.push_back({current, active_palette->panels.label});
        }
        std::vector<std::string> names;
        for (const auto& skin : skins) names.push_back(skin.label);
        const auto cursor = static_cast<std::size_t>(
            std::ranges::find(skins, current, &SkinChoice::id) - skins.begin());
        show_local_list("Skins", std::move(names), [this, skins = std::move(skins)](const std::size_t selected) {
            try {
                auto candidate = make_palette(load_skin(skins.at(selected).id, default_config_path().parent_path()));
                auto label = candidate->panels.label;
                auto id = candidate->panels.id;
                active_palette = std::move(candidate);
                appearance_skin = std::move(label);
                config.theme = std::move(id);
                config.theme_override.reset();
                if (!active_palette->panels.warning.empty()) status = "Skin: " + active_palette->panels.warning;
            } catch (const std::exception& error) {
                show_native_message("Skin error", error.what(), Mode::appearance_dialog);
            }
        }, Mode::appearance_dialog);
        local_list_native = true;
        local_list_cursor = cursor;
    }

    bool handle_appearance(const ftxui::Event& event) {
        using namespace ftxui;
        if (modal_cancel(event)) {
            config.theme = appearance_original_skin;
            config.theme_override = appearance_original_override;
            active_palette = appearance_original_palette;
            config.shadows = appearance_original_shadows;
            mode = Mode::panels;
            return true;
        }
        if (event.is_mouse()) {
            auto copy = event;
            const auto& mouse = copy.mouse();
            if (mouse.button != Mouse::Left || mouse.motion != Mouse::Pressed) return true;
            for (std::size_t index = 0; index < appearance_boxes.size(); ++index) {
                if (!appearance_boxes[index].Contain(mouse.x, mouse.y)) continue;
                appearance_focus = index;
                return handle_appearance(Event::Character(' '));
            }
            return true;
        }
        if (event == Event::Tab || event == Event::ArrowDown) appearance_focus = (appearance_focus + 1U) % 4U;
        else if (event == Event::TabReverse || event == Event::ArrowUp) appearance_focus = (appearance_focus + 3U) % 4U;
        else if (event == Event::Character(' ') || event == Event::Return) {
            if (appearance_focus == 0U) choose_skin();
            else if (appearance_focus == 1U && event == Event::Character(' ')) {
                appearance_shadows = !appearance_shadows;
                config.shadows = appearance_shadows;
            } else if (appearance_focus == 3U) return handle_appearance(Event::Escape);
            else {
                config.shadows = appearance_shadows;
                mode = Mode::panels;
                status = "Appearance changed";
            }
        } else if (event == Event::Character('s') || event == Event::Character('S')) {
            appearance_shadows = !appearance_shadows;
            config.shadows = appearance_shadows;
            appearance_focus = 1;
        }
        return true;
    }

    ftxui::Element render_display_bits() {
        using namespace ftxui;
        auto codepage = text("[ UTF-8 ]") | reflect(display_bits_boxes[0]);
        if (display_bits_focus == 0U) codepage = focus_dialog(std::move(codepage));
        return native_window("Display bits", vbox({
            hbox({text(" Input / display codepage: "), std::move(codepage), filler()}),
            separator(),
            hbox({text(" [x] "), mnemonic_label("Full 8 bits input", 1)}) | dim | color(palette().dim),
            separator(),
            hbox({native_button("OK", true, display_bits_focus == 1U, display_bits_boxes[1]), text(" "),
                  native_button("Cancel", false, display_bits_focus == 2U, display_bits_boxes[2])}) | center,
        }), 55);
    }

    bool handle_display_bits(const ftxui::Event& event) {
        using namespace ftxui;
        if (modal_cancel(event)) { mode = Mode::panels; return true; }
        if (event.is_mouse()) {
            auto copy = event;
            const auto& mouse = copy.mouse();
            if (mouse.button != Mouse::Left || mouse.motion != Mouse::Pressed) return true;
            for (std::size_t index = 0; index < display_bits_boxes.size(); ++index) {
                if (!display_bits_boxes[index].Contain(mouse.x, mouse.y)) continue;
                display_bits_focus = index;
                return handle_display_bits(Event::Return);
            }
            return true;
        }
        if (event == Event::Tab || event == Event::ArrowDown) display_bits_focus = (display_bits_focus + 1U) % 3U;
        else if (event == Event::TabReverse || event == Event::ArrowUp) display_bits_focus = (display_bits_focus + 2U) % 3U;
        else if (event == Event::Return || event == Event::Character(' ')) {
            if (display_bits_focus == 0U) {
                show_local_list("Choose codepage", {"UTF-8"}, [](const std::size_t) {}, Mode::display_bits_dialog);
                local_list_native = true;
            } else mode = Mode::panels;
        }
        return true;
    }

    ftxui::Element render_panel_options() {
        using namespace ftxui;
        static const std::array<std::string, 16> names{
            "Show mini-status", "Use SI size units", "Mix all files", "Show backup files",
            "Show hidden files", "Fast dir reload", "Mark moves down", "Reverse files only",
            "Simple swap", "Auto save panels setup", "Lynx-like motion", "Page scrolling",
            "Center scrolling", "Mouse page scrolling", "File types", "Permissions"
        };
        static constexpr std::array<std::size_t, 16> hotkeys{7, 9, 2, 5, 5, 0, 2, 2, 8, 1, 1, 2, 7, 0, 5, 0};
        const auto check = [&](const std::size_t index) {
            auto row = hbox({text(panel_option_values[index] ? " [x] " : " [ ] "),
                             mnemonic_label(names[index], hotkeys[index])}) | reflect(panel_option_boxes[index]);
            if (panel_option_focus == index) row = focus_dialog(std::move(row));
            return row;
        };
        Elements main, navigation, highlight, search;
        for (std::size_t index = 0; index < 10U; ++index) main.push_back(check(index));
        for (int index = 0; index < 3; ++index) main.push_back(text(""));
        for (std::size_t index = 10U; index < 14U; ++index) navigation.push_back(check(index));
        highlight.push_back(check(14));
        highlight.push_back(check(15));
        static const std::array<std::string, 3> search_names{
            "Case insensitive", "Case sensitive", "Use panel sort mode"
        };
        static constexpr std::array<std::size_t, 3> search_hotkeys{5, 3, 17};
        for (std::size_t index = 0; index < 3U; ++index) {
            auto row = hbox({text(panel_option_search == index ? " (*) " : " ( ) "),
                             mnemonic_label(search_names[index], search_hotkeys[index])}) |
                       reflect(panel_option_boxes[16U + index]);
            if (panel_option_focus == 16U && panel_option_search == index) row = focus_dialog(std::move(row));
            search.push_back(std::move(row));
        }
        return native_window("Panel options", vbox({
            hbox({text(" "),
                window(text(" Main options "), vbox(std::move(main)), LIGHT) | size(WIDTH, EQUAL, 30),
                text(" "),
                vbox({
                    window(text(" Navigation "), vbox(std::move(navigation)), LIGHT),
                    window(text(" File highlight "), vbox(std::move(highlight)), LIGHT),
                    window(text(" Quick search "), vbox(std::move(search)), LIGHT),
                }) | size(WIDTH, EQUAL, 30), text(" ")}),
            separator(),
            hbox({native_button("OK", true, panel_option_focus == 17U, panel_option_boxes[19]), text(" "),
                  native_button("Cancel", false, panel_option_focus == 18U, panel_option_boxes[20])}) | center,
        }), 65);
    }

    void apply_panel_options() try {
        const auto& values = panel_option_values;
        const bool warn_fast_reload = !config.fast_reload && values[5];
        config.show_mini_status = values[0]; config.use_si_units = values[1];
        config.directories_first = !values[2]; config.show_backups = values[3];
        config.show_hidden = values[4]; config.fast_reload = values[5];
        config.mark_moves_down = values[6]; config.reverse_files_only = values[7];
        config.simple_swap = values[8]; config.auto_save_panels_setup = values[9];
        config.navigate_with_arrows = values[10]; config.scroll_pages = values[11];
        config.scroll_center = values[12]; config.mouse_page_scrolling = values[13];
        config.highlight_file_types = values[14]; config.highlight_permissions = values[15];
        config.quick_search_mode = panel_option_search;
        for (auto* pane : {&left, &right}) {
            pane->set_show_hidden(config.show_hidden);
            pane->set_show_backups(config.show_backups);
            pane->set_directories_first(config.directories_first);
            pane->set_reverse_files_only(config.reverse_files_only);
            pane->set_fast_reload(config.fast_reload);
            pane->set_navigation(config.scroll_pages, config.scroll_center);
        }
        store_panel_options_in_config();
        mode = Mode::panels;
        status = "Panel options changed";
        if (warn_fast_reload && !fast_reload_warning_shown) {
            fast_reload_warning_shown = true;
            show_native_message("Information",
                "Fast directory reload can miss changes to existing files. Use Rescan or Ctrl-R when a full reload is needed.");
        }
    } catch (const std::exception& error) {
        mode = Mode::panels;
        set_error(error);
    }

    bool handle_panel_options(const ftxui::Event& event) {
        using namespace ftxui;
        if (modal_cancel(event)) { mode = Mode::panels; return true; }
        if (event.is_mouse()) {
            auto copy = event;
            const auto& mouse = copy.mouse();
            if (mouse.button != Mouse::Left || mouse.motion != Mouse::Pressed) return true;
            for (std::size_t index = 0; index < panel_option_boxes.size(); ++index) {
                if (!panel_option_boxes[index].Contain(mouse.x, mouse.y)) continue;
                if (index < 16U) { panel_option_focus = index; panel_option_values[index] = !panel_option_values[index]; }
                else if (index < 19U) { panel_option_focus = 16; panel_option_search = index - 16U; }
                else if (index == 19U) apply_panel_options();
                else mode = Mode::panels;
                return true;
            }
            return true;
        }
        if (event == Event::Tab) panel_option_focus = (panel_option_focus + 1U) % 19U;
        else if (event == Event::TabReverse) panel_option_focus = (panel_option_focus + 18U) % 19U;
        else if (panel_option_focus == 16U && (event == Event::ArrowUp || event == Event::ArrowDown)) {
            panel_option_search = event == Event::ArrowDown ? (panel_option_search + 1U) % 3U : (panel_option_search + 2U) % 3U;
        } else if (event == Event::ArrowUp || event == Event::ArrowDown) {
            panel_option_focus = event == Event::ArrowDown ? (panel_option_focus + 1U) % 19U : (panel_option_focus + 18U) % 19U;
        } else if (event == Event::Character(' ') && panel_option_focus < 16U) {
            panel_option_values[panel_option_focus] = !panel_option_values[panel_option_focus];
        } else if (event == Event::Character(' ') && panel_option_focus == 16U) {
            return true;
        } else if (event == Event::Return || event == Event::Character(' ')) {
            if (panel_option_focus == 18U) mode = Mode::panels;
            else apply_panel_options();
        } else if (event.is_character() && event.character().size() == 1U) {
            const auto key = static_cast<char>(std::tolower(static_cast<unsigned char>(event.character()[0])));
            if (const auto index = std::string_view("nzxbhfrvwuygsmtp").find(key); index != std::string_view::npos) {
                panel_option_values[index] = !panel_option_values[index];
                panel_option_focus = index;
            } else if (const auto radio_index = std::string_view("ied").find(key); radio_index != std::string_view::npos) {
                panel_option_search = radio_index;
                panel_option_focus = 16;
            }
        }
        return true;
    }

    ftxui::Element render_layout_dialog() {
        using namespace ftxui;
        const auto check = [&](const std::string& label, const std::size_t mnemonic,
                               const bool value, const std::size_t focus_index, const std::size_t box) {
            auto row = hbox({text(value ? " [x] " : " [ ] "), mnemonic_label(label, mnemonic)}) |
                       reflect(layout_boxes[box]);
            if (layout_focus == focus_index) row = focus_dialog(std::move(row));
            return row;
        };
        const auto radio = [&](const std::string& label, const bool horizontal, const std::size_t box) {
            auto row = hbox({text(layout_horizontal == horizontal ? " (*) " : " ( ) "), mnemonic_label(label, 0)}) |
                       reflect(layout_boxes[box]);
            if (layout_focus == 0U && layout_horizontal == horizontal) row = focus_dialog(std::move(row));
            return row;
        };
        const auto total = layout_horizontal
            ? std::max(1, ftxui::Terminal::Size().dimy - static_cast<int>(layout_visible[0]) -
                          static_cast<int>(layout_visible[1]) - static_cast<int>(layout_visible[2]) -
                          static_cast<int>(layout_visible[3]))
            : ftxui::Terminal::Size().dimx;
        const auto split_size = layout_equal ? total / 2 : layout_horizontal ? layout_top_size : layout_left_size;
        const auto count = [](const int value) {
            std::ostringstream output;
            output << std::setw(3) << std::setfill('0') << value;
            return output.str();
        };
        auto decrease = text("[<]") | reflect(layout_boxes[3]);
        auto increase = text("[>]") | reflect(layout_boxes[4]);
        if (layout_equal) {
            decrease = decrease | dim | color(palette().dim);
            increase = increase | dim | color(palette().dim);
        } else {
            if (layout_focus == 2U) decrease = focus_dialog(std::move(decrease));
            if (layout_focus == 3U) increase = focus_dialog(std::move(increase));
        }
        auto split = window(text(" Panel split "), vbox({
            radio("Vertical", false, 0), radio("Horizontal", true, 1),
            check("Equal split", 0, layout_equal, 1, 2),
            hbox({text(" " + count(split_size)), decrease, text(" = "), increase, text(count(total - split_size))}),
        }), LIGHT) | size(WIDTH, EQUAL, 26);
        auto console = window(text(" Console output "),
            text(" Output lines:[+]00[-]") | dim | color(palette().dim), LIGHT) | size(WIDTH, EQUAL, 26);
        static const std::array<std::string, 6> labels{
            "Menubar visible", "Command prompt", "Keybar visible", "Hintbar visible",
            "XTerm window title", "Show free space"
        };
        static constexpr std::array<std::size_t, 6> mnemonic{0, 8, 0, 1, 0, 0};
        Elements options;
        for (std::size_t index = 0; index < labels.size(); ++index) {
            options.push_back(check(labels[index], mnemonic[index], layout_visible[index], index + 4U, index + 5U));
        }
        options.push_back(text(""));
        return native_window("Layout", vbox({
            hbox({text(" "), vbox({split, console}), text(" "),
                  window(text(" Other options "), vbox(std::move(options)), LIGHT) | size(WIDTH, EQUAL, 26),
                  text(" ")}),
            separator(),
            hbox({native_button("OK", true, layout_focus == 10U, layout_boxes[11]), text(" "),
                  native_button("Cancel", false, layout_focus == 11U, layout_boxes[12])}) | center,
        }), 57);
    }

    void adjust_layout_split(const int amount) {
        if (layout_equal) return;
        const auto total = layout_horizontal
            ? std::max(1, ftxui::Terminal::Size().dimy - static_cast<int>(layout_visible[0]) -
                          static_cast<int>(layout_visible[1]) - static_cast<int>(layout_visible[2]) -
                          static_cast<int>(layout_visible[3]))
            : ftxui::Terminal::Size().dimx;
        auto& split = layout_horizontal ? layout_top_size : layout_left_size;
        const auto minimum = std::min(layout_horizontal ? 6 : 12, std::max(1, total / 2));
        split = std::clamp(split + amount, minimum, std::max(minimum, total - minimum));
    }

    void apply_layout() {
        config.layout_horizontal = layout_horizontal;
        config.set_split_equal(layout_equal);
        config.panel_left_size = layout_left_size;
        config.panel_top_size = layout_top_size;
        config.menubar_visible = layout_visible[0];
        config.command_prompt = layout_visible[1];
        config.keybar_visible = layout_visible[2];
        config.hintbar_visible = layout_visible[3];
        config.xterm_title = layout_visible[4];
        config.show_free_space = layout_visible[5];
        if (!config.command_prompt) {
            command_prompt_override = false;
            command_mode = false;
            command_line.clear();
            command_input.reset(command_line);
        }
        mode = Mode::panels;
        status = "Layout changed";
    }

    bool handle_layout_dialog(const ftxui::Event& event) {
        using namespace ftxui;
        if (modal_cancel(event)) { mode = Mode::panels; return true; }
        const auto equal_changed = [&] {
            layout_equal = !layout_equal;
            if (layout_equal) {
                layout_left_size = Terminal::Size().dimx / 2;
                layout_top_size = file_manager_body_height() / 2;
            }
        };
        if (event.is_mouse()) {
            auto copy = event;
            const auto& mouse = copy.mouse();
            if (mouse.button != Mouse::Left || mouse.motion != Mouse::Pressed) return true;
            for (std::size_t index = 0; index < layout_boxes.size(); ++index) {
                if (!layout_boxes[index].Contain(mouse.x, mouse.y)) continue;
                if (index < 2U) { layout_focus = 0; layout_horizontal = index == 1U; }
                else if (index == 2U) { layout_focus = 1; equal_changed(); }
                else if (index < 5U) {
                    if (!layout_equal) { layout_focus = index - 1U; adjust_layout_split(index == 3U ? -1 : 1); }
                } else if (index < 11U) {
                    layout_focus = index - 1U;
                    layout_visible[index - 5U] = !layout_visible[index - 5U];
                } else if (index == 11U) apply_layout();
                else mode = Mode::panels;
                return true;
            }
            return true;
        }
        if (event == Event::Tab || event == Event::TabReverse) {
            do {
                layout_focus = event == Event::Tab ? (layout_focus + 1U) % 12U : (layout_focus + 11U) % 12U;
            } while (layout_equal && (layout_focus == 2U || layout_focus == 3U));
        } else if (layout_focus == 0U && (event == Event::ArrowUp || event == Event::ArrowDown)) {
            layout_horizontal = !layout_horizontal;
        } else if (event == Event::Return || event == Event::Character(' ')) {
            if (layout_focus == 11U) mode = Mode::panels;
            else if (layout_focus == 2U || layout_focus == 3U) adjust_layout_split(layout_focus == 2U ? -1 : 1);
            else if (event == Event::Return || layout_focus == 10U) apply_layout();
            else if (layout_focus == 1U) equal_changed();
            else if (layout_focus >= 4U && layout_focus <= 9U) layout_visible[layout_focus - 4U] = !layout_visible[layout_focus - 4U];
        } else if (event.is_character() && event.character().size() == 1U) {
            const auto key = static_cast<char>(std::tolower(static_cast<unsigned char>(event.character()[0])));
            if (key == 'v') { layout_horizontal = false; layout_focus = 0; }
            else if (key == 'h') { layout_horizontal = true; layout_focus = 0; }
            else if (key == 'e') { equal_changed(); layout_focus = 1; }
            else if (const auto index = std::string_view("mpkixs").find(key); index != std::string_view::npos) {
                layout_visible[index] = !layout_visible[index];
                layout_focus = index + 4U;
            }
        }
        return true;
    }

    ftxui::Element render_confirmation_dialog() {
        using namespace ftxui;
        static const std::array<std::string, 6> labels{
            "Delete", "Overwrite", "Execute", "Exit", "Directory hotlist delete", "History cleanup"
        };
        static constexpr std::array<std::size_t, 6> mnemonics{0, 1, 0, 1, 2, 0};
        Elements rows;
        for (std::size_t index = 0; index < labels.size(); ++index) {
            auto row = hbox({text(confirmation_values[index] ? " [x] " : " [ ] "),
                             mnemonic_label(labels[index], mnemonics[index])}) |
                       reflect(confirmation_boxes[index]);
            if (confirmation_focus == index) row = focus_dialog(std::move(row));
            rows.push_back(std::move(row));
        }
        return native_window("Confirmation", vbox({
            vbox(std::move(rows)), separator(),
            hbox({native_button("OK", true, confirmation_focus == 6U, confirmation_boxes[6]), text(" "),
                  native_button("Cancel", false, confirmation_focus == 7U, confirmation_boxes[7])}) | center,
        }), 44);
    }

    bool handle_confirmation_dialog(const ftxui::Event& event) {
        using namespace ftxui;
        if (modal_cancel(event)) { mode = Mode::panels; return true; }
        if (event.is_mouse()) {
            auto copy = event;
            const auto& mouse = copy.mouse();
            if (mouse.button != Mouse::Left || mouse.motion != Mouse::Pressed) return true;
            for (std::size_t index = 0; index < confirmation_boxes.size(); ++index) {
                if (!confirmation_boxes[index].Contain(mouse.x, mouse.y)) continue;
                confirmation_focus = index;
                if (index < 6U) confirmation_values[index] = !confirmation_values[index];
                else return handle_confirmation_dialog(Event::Return);
                return true;
            }
            return true;
        }
        if (event == Event::Tab || event == Event::ArrowDown) {
            confirmation_focus = (confirmation_focus + 1U) % 8U;
        } else if (event == Event::TabReverse || event == Event::ArrowUp) {
            confirmation_focus = (confirmation_focus + 7U) % 8U;
        } else if (event == Event::Character(' ') && confirmation_focus < 6U) {
            confirmation_values[confirmation_focus] = !confirmation_values[confirmation_focus];
        } else if (event == Event::Return || event == Event::Character(' ')) {
            if (confirmation_focus != 7U) {
                config.confirm_delete = confirmation_values[0];
                config.confirm_overwrite = confirmation_values[1];
                config.confirm_execute = confirmation_values[2];
                config.confirm_exit = confirmation_values[3];
                config.confirm_hotlist_delete = confirmation_values[4];
                config.confirm_history_cleanup = confirmation_values[5];
            }
            mode = Mode::panels;
        } else if (event.is_character() && event.character().size() == 1U) {
            const auto key = static_cast<char>(std::tolower(static_cast<unsigned char>(event.character()[0])));
            if (const auto index = std::string_view("dvexrh").find(key); index != std::string_view::npos) {
                confirmation_values[index] = !confirmation_values[index];
                confirmation_focus = index;
            }
        }
        return true;
    }

    void request_app_exit(const bool quiet = false) {
        if (config.confirm_exit && !quiet) {
            show_native_query("Quit", "Do you really want to quit?", {"Yes", "No"},
                [this](const std::size_t selected) { if (selected == 0U) exit_terminal(); });
        } else exit_terminal();
    }

    void set_error(const std::exception& error) {
        status = std::string("Error: ") + error.what();
        if (!config.hintbar_visible && terminal != nullptr &&
            (mode == Mode::panels || mode == Mode::pulldown)) {
            const auto message = std::string(error.what());
            post_ui([this, message] { show_native_message("Error", message); });
        }
    }

    void post_ui(std::function<void()> task) {
        if (terminal == nullptr) {
            return;
        }
        auto* app = terminal;
        app->Post([
            this,
            app,
            task = std::move(task)
        ]() mutable {
            task();
            service_operation_prompts();
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
        if (has_running_jobs()) {
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

    bool has_running_jobs() const {
        return std::ranges::any_of(jobs, [](const auto& job) { return !job->finished; });
    }

    void restore_operation_prompt() {
        mode = operation_return_mode;
        dialog_focus = operation_return_focus;
        prompt_job.reset();
        operation_prompt_source.reset();
        operation_prompt_destination.reset();
        operation_prompt_error.clear();
    }

    void service_operation_prompts() {
        if (exiting || shell_foreground || help_browser || prompt_job) return;
        std::erase_if(operation_questions, [](const auto& pending) {
            const auto question = pending.first->control.question();
            return !question || question->sequence != pending.second;
        });
        if (mode != Mode::panels && mode != Mode::jobs && mode != Mode::operation_progress) return;
        if (mode == Mode::panels &&
            (command_mode || panel_search_mode || tree_search_mode || panel_ctrl_x)) return;
        const auto found = std::ranges::find_if(operation_questions, [this](const auto& pending) {
            const auto& job = pending.first;
            return mode == Mode::operation_progress
                ? progress_job == job : !job->background;
        });
        if (found == operation_questions.end()) return;
        auto job = found->first;
        const auto question = job->control.question();
        operation_questions.erase(found);
        if (!question) return;
        operation_return_mode = mode;
        operation_return_focus = dialog_focus;
        prompt_job = std::move(job);
        operation_prompt_sequence = question->sequence;
        operation_prompt_source = question->source;
        operation_prompt_destination = question->destination;
        operation_prompt_collision_options = question->collision_options;
        operation_prompt_error = question->error;
        dialog_focus = question->recursive_delete ? (config.safe_delete ? 1U : 0U)
            : question->destination && config.safe_overwrite ? 1U : 0U;
        mode = question->recursive_delete ? Mode::recursive_delete_dialog
            : question->destination ? Mode::conflict_dialog : Mode::operation_error_dialog;
    }

    void request_operation_cancel(const std::shared_ptr<OperationJob>& job) {
        if (!job || job->finished) return;
        job->control.cancel();
        if (prompt_job == job) restore_operation_prompt();
    }

    void cancel_operations() {
        for (const auto& job : jobs) {
            if (!job->finished) job->control.cancel();
        }
    }

    void shutdown_operations() {
        cancel_operations();
        for (const auto& job : jobs) {
            if (job->worker.joinable()) job->worker.join();
            job->control.finish();
            job->finished = true;
        }
        operation_questions.clear();
        prompt_job.reset();
    }

    OperationChoice ask_operation(
        const std::shared_ptr<OperationJob>& job,
        Entry source,
        std::optional<Entry> destination,
        std::string error = {},
        const CollisionOptions collision_options = {},
        const bool recursive_delete = false
    ) {
        const auto sequence = job->control.begin_question({
            0, std::move(source), std::move(destination), std::move(error),
            collision_options, recursive_delete
        });
        post_ui([this, job, sequence] {
            operation_questions.emplace_back(job, sequence);
            if (job->background && mode == Mode::panels && !command_mode) {
                status = job->label + " waiting for a decision; C-x j opens Background jobs";
            }
            service_operation_prompts();
        });
        return job->control.wait_for_answer(sequence);
    }

    void answer_operation(const OperationChoice answer) {
        const auto job = prompt_job;
        const auto sequence = operation_prompt_sequence;
        if (!job) return;
        restore_operation_prompt();
        static_cast<void>(job->control.answer(sequence, answer));
    }

    OperationErrorResolver error_handler(
        const std::shared_ptr<OperationJob>& job,
        const std::shared_ptr<OperationPolicy>& policy
    ) {
        return [this, job, policy](const Entry& source, const std::string& message) {
            if (policy->ignore_errors) {
                return OperationErrorDecision::skip;
            }
            switch (ask_operation(job, source, std::nullopt, message)) {
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

    void start_job(const std::shared_ptr<OperationJob>& job, const Mode return_mode) {
        while (jobs.size() >= 20U) {
            const auto completed = std::ranges::find_if(jobs, [](const auto& value) {
                return value->finished;
            });
            if (completed == jobs.end()) break;
            jobs.erase(completed);
        }
        job->id = next_job_id++;
        job->started = std::chrono::steady_clock::now();
        jobs.push_back(job);
        progress_job = job;
        job_cursor = jobs.size() - 1U;
        progress_return_mode = return_mode;
        operation_progress_focus = 0;
        mode = config.verbose_operation && !job->background ? Mode::operation_progress : return_mode;
        status = job->label + (job->background ? " running in background" : " starting");
        try {
            job->worker = std::jthread([this, job] { run_job(job); });
        } catch (const std::exception& error) {
            job->status = "Error: " + std::string(error.what());
            job->finished = true;
            job->control.finish();
            mode = return_mode;
            throw;
        }
    }

    void exit_terminal() {
        if (!exiting) {
            if (close_dirty_screen_for_exit()) return;
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
        const std::shared_ptr<OperationJob>& job,
        std::string message,
        const std::vector<std::string>& completed_sources = {},
        const bool notify = false
    ) {
        if (job->worker.joinable()) job->worker.join();
        job->finished = true;
        job->control.finish();
        if (prompt_job == job) restore_operation_prompt();
        const bool visible = mode == Mode::operation_progress && progress_job == job;
        if (visible) {
            mode = progress_return_mode == Mode::directory_tree_dialog && !modal_tree_snapshot
                ? Mode::panels : progress_return_mode;
        }
        try {
            auto& pane = job->source_pane_id == pane_ids[0] ? left : right;
            if (pane.provider() == job->source_provider && pane.path() == job->source_directory) {
                pane.unmark_paths(completed_sources);
            }
            refresh();
        } catch (const std::exception& error) {
            message += "; refresh failed: " + std::string(error.what());
            pending_operation_notice = message;
        }
        job->status = message;
        status = message;
        if (!visible && notify) {
            pending_operation_notice = "Job " + std::to_string(job->id) + ": " + message;
        }
        service_operation_prompts();
    }

    void bind_input_history(TextInputState& input, const std::string_view id) {
        input.history_id = id;
        input.history = input_histories.history(id);
        input.history_index = input.history.size();
    }

    void initialize_input_histories() {
        try {
            input_histories.load();
        } catch (const std::exception& error) {
            std::cerr << "mcpp: input histories: " << error.what() << '\n';
            set_error(error);
        }
        for (const auto& [input, id] : {
                 std::pair{&mask_input, "file.source-mask"},
                 std::pair{&listing_brief_input, "listing.brief-columns"},
                 std::pair{&listing_user_input, "listing.user-format"},
                 std::pair{&listing_status_input, "listing.status-format"},
                 std::pair{&configure_timeout_input, "settings.escape-timeout"},
                 std::pair{&panelize_input, "command.panelize"},
                 std::pair{&editor_query_input, "search.editor"},
                 std::pair{&editor_replacement_input, "replace.editor"},
                 std::pair{&editor_tab_input, "editor.tab-width"},
                 std::pair{&editor_wrap_input, "editor.wrap-length"},
                 std::pair{&mail_inputs[0], "mail.to"},
                 std::pair{&mail_inputs[1], "mail.subject"},
                 std::pair{&mail_inputs[2], "mail.cc"}}) {
            bind_input_history(*input, id);
        }
    }

    static std::string_view prompt_history_id(const PromptAction action) {
        switch (action) {
            case PromptAction::rename: return "filename.rename";
            case PromptAction::mkdir: return "directory.mkdir";
            case PromptAction::view_file: return "filename.view";
            case PromptAction::viewer_find:
            case PromptAction::quick_viewer_find: return "search.viewer";
            case PromptAction::viewer_goto: return "goto.viewer";
            case PromptAction::editor_open: return "filename.editor-open";
            case PromptAction::editor_new: return "filename.editor-new";
            case PromptAction::editor_save_as: return "filename.editor-save";
            case PromptAction::editor_goto: return "goto.editor";
            case PromptAction::hardlink: return "filename.hardlink";
            case PromptAction::symlink: return "filename.symlink";
            case PromptAction::relative_symlink: return "filename.relative-symlink";
            case PromptAction::select_pattern:
            case PromptAction::unselect_pattern: return "file.selection-pattern";
            case PromptAction::quick_cd: return "directory.change";
            case PromptAction::chmod: return "file.permissions";
            case PromptAction::chown: return "file.owner";
            case PromptAction::recursive_search: return "find.filename";
            case PromptAction::panel_filter: return "file.panel-filter";
            case PromptAction::panelize_search: return "find.panelize";
            case PromptAction::none:
            case PromptAction::viewer_path: return {};
        }
        return {};
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
        bind_input_history(prompt_inputs[action], prompt_history_id(action));
        prompt_inputs[action].reset(prompt_value, true);
        prompt_error.clear();
        prompt_focus = 0;
        prompt_boxes = {};
    }

    void remember_input(TextInputState& input, const std::string& value) {
        if (!input.history_id.empty()) {
            input_histories.remember(input.history_id, value);
            input.history = input_histories.history(input.history_id);
        } else if (!value.empty() &&
            (input.history.empty() || input.history.back() != value)) {
            input.history.push_back(value);
            if (input.history.size() > 100U) {
                input.history.erase(input.history.begin());
            }
        }
        input.history_index = input.history.size();
    }

    void show_input_history(std::string& value, TextInputState& input) {
        if (input.history_id.empty()) return;
        if (input.history_index == input.history.size()) {
            input.draft = value;
            input.draft_cursor = input.cursor;
            input.draft_anchor = input.anchor;
        }
        auto history = input.history;
        std::reverse(history.begin(), history.end());
        show_local_list("History", std::move(history),
            [this, destination = &value, state = &input](const std::size_t selected) {
                *destination = local_list_items.at(selected);
                state->history_index = state->history.size() - selected - 1U;
                state->cursor = destination->size();
                state->anchor.reset();
                state->completions.clear();
            }, mode);
        local_list_native = true;
        local_history_kind = 3;
        local_history_input = &input;
        local_history_value = &value;
    }

    bool edit_input(
        std::string& value,
        TextInputState& input,
        const ftxui::Event& input_event
    ) {
        using ftxui::Event;
        const auto mapped = remap_widget_key(KeymapContext::input, input_event);
        if (!mapped) return true;
        const auto& event = *mapped;
        input.completions.clear();
        input.message.clear();
        input.cursor = std::min(input.cursor, value.size());
        if (input.anchor) {
            *input.anchor = std::min(*input.anchor, value.size());
        }
        if (event == Event::AltH && !input.history_id.empty()) {
            show_input_history(value, input);
            return true;
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
            if (input.history.empty() || (backward && input.history_index == 0U) ||
                (!backward && input.history_index == input.history.size())) {
                return true;
            }
            if (backward && input.history_index == input.history.size()) {
                input.draft = value;
                input.draft_cursor = input.cursor;
                input.draft_anchor = input.anchor;
            }
            if (backward && input.history_index > 0U) {
                --input.history_index;
            } else if (!backward && input.history_index < input.history.size()) {
                ++input.history_index;
            }
            if (input.history_index < input.history.size()) {
                value = input.history[input.history_index];
                move(value.size(), false);
            } else {
                value = input.draft;
                input.cursor = std::min(input.draft_cursor, value.size());
                input.anchor = input.draft_anchor;
                if (input.anchor) *input.anchor = std::min(*input.anchor, value.size());
            }
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
        return active ? highlight(std::move(field), palette().dialog_focus) : field;
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

    void insert_completion(
        std::string& value, TextInputState& input, const std::string_view completion
    ) {
        auto edit = completion_edit(input.completion_target, completion);
        value = std::move(edit.value);
        input.cursor = edit.cursor;
        input.anchor.reset();
        input.completed_value = value;
        input.message = input.completion_warning;
    }

    void show_completions(std::string& value, TextInputState& input) {
        const auto return_mode = mode;
        auto* destination = &value;
        auto* state = &input;
        const auto choices = input.completions;
        show_local_list("Completion", choices,
            [this, destination, state](const std::size_t selected) {
                insert_completion(*destination, *state, local_list_items.at(selected));
            }, return_mode);
        local_list_native = true;
        local_list_completion = true;
        completion_choices = choices;
        completion_filter.clear();
    }

    void complete_path(
        std::string& value,
        TextInputState& input,
        const std::string& base,
        const ProviderPtr& provider = {},
        const bool command_line_input = false,
        const bool local_absolute = true
    ) {
        if (!input.completions.empty() && value == input.completed_value) {
            show_completions(value, input);
            return;
        }
        input.completions.clear();
        const auto completion_base = base.find("://") != std::string::npos
            ? resolve_location(base) : Location{provider ? provider : local, base};
        const auto& source = completion_base.provider;
        auto result = complete_input(value, input.cursor, input.anchor, command_line_input,
            [&](const std::string_view directory) {
                auto path = std::string(directory);
                Location location;
                if (path.find("://") != std::string::npos) {
                    if (std::filesystem::path(path).is_relative()) {
                        if (source != local) throw std::runtime_error("nested archives are not supported yet");
                        const auto slash = path.rfind('/', path.find("://"));
                        if (slash == std::string::npos) throw std::invalid_argument("VFS path has no archive name");
                        path = local->join(completion_base.path, path.substr(0, slash)) + path.substr(slash);
                    }
                    location = resolve_location(path);
                } else if (local_absolute && std::filesystem::path(path).is_absolute()) {
                    location = {local, local->normalize(path)};
                } else {
                    location = {source, source->normalize(
                        (std::filesystem::path(completion_base.path) / path).generic_string())};
                }
                return location.provider->list(location.path, {.show_hidden=true}, {});
            }, command_line_input ? pane_execution_directory(active()) : std::string{});
        input.completion_target = std::move(result.target);
        input.completion_warning = std::move(result.warning);
        input.completions = std::move(result.matches);
        const auto& matches = input.completions;
        if (matches.empty()) {
            input.message = "No matching " + std::string(completion_kind_name(result.kind));
            if (!input.completion_warning.empty()) input.message += "; " + input.completion_warning;
            return;
        }
        if (matches.size() == 1U) {
            insert_completion(value, input, matches.front());
            input.completions.clear();
            return;
        }
        insert_completion(value, input, completion_common_prefix(matches));
        input.message = std::to_string(matches.size()) + " " +
            std::string(completion_kind_name(result.kind)) + "; Alt-Tab lists matches";
        if (!input.completion_warning.empty()) {
            input.message += "; " + input.completion_warning;
        }
        if (config.completion_show_all) {
            show_completions(value, input);
        }
    }

    void cancel_prompt() {
        if (prompt_action == PromptAction::editor_save_as && editor_save_as_continuation) {
            editor_unnamed_save_buffer = nullptr;
            editor_save_as_continuation.reset();
            editor_save_buffer = nullptr;
            editor_quit_requested = false;
        }
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
        operation_draft = capture_operation_job();
        dialog_move = move_operation;
        dialog_exact = exact && sources.size() == 1U;
        dialog_destination = pane_location(passive());
        if (!dialog_destination.ends_with('/')) dialog_destination += '/';
        dialog_preserve_attributes = true;
        dialog_preserve_symlinks = true;
        dialog_shell_patterns = config.shell_patterns;
        dialog_stable_symlinks = false;
        dialog_dive_into_subdirectories = false;
        dialog_background = false;
        dialog_source_mask = dialog_shell_patterns ? "*" : "^(.*)$";
        dialog_destination_mask.reset();
        operation_input_error.clear();
        bind_input_history(destination_input, move_operation ? "file.move-destination" : "file.copy-destination");
        destination_input.reset(dialog_destination, true);
        mask_input.reset(dialog_source_mask, true);
        dialog_focus = 2;
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
        const auto& source_pane = tree_operation_source
            ? (tree_operation_source_index == 0 ? left : right) : active();
        const CopyMask mask(dialog_source_mask, dialog_shell_patterns);
        const auto slash = dialog_destination.rfind('/');
        const auto directory = slash == std::string::npos
            ? std::string{} : dialog_destination.substr(0, slash + 1U);
        const auto name = dialog_destination.substr(slash == std::string::npos ? 0U : slash + 1U);
        dialog_destination_mask.reset();
        std::string destination_value;
        bool destination_directory = name.empty();
        if (CopyMask::has_replacements(name)) {
            dialog_destination_mask = name;
            for (const auto& source : sources) {
                static_cast<void>(mask.transform(source.name, name));
            }
            destination_value = CopyMask::unescape_path(directory.empty() ? "." : directory);
            destination_directory = true;
        } else {
            destination_value = CopyMask::unescape_path(directory);
            destination_value += CopyMask::unescape_path(name);
        }
        const auto destination = operation_draft
            ? resolve_relative_location(
                  {operation_draft->source_provider, operation_draft->source_base_directory},
                  destination_value
              )
            : resolve_pane_location(source_pane, destination_value);
        const auto& destination_provider = destination.provider;
        if (!has_capability(destination_provider->capabilities(), ProviderCapability::write)) {
            throw std::runtime_error("the destination filesystem is read-only");
        }
        if (destination_provider->exists(destination.path)) {
            const auto target = destination_provider->stat(
                destination.path,
                {}
            );
            const bool is_directory =
                target.kind == EntryKind::directory ||
                target.target_is_directory;
            if (destination_directory && !is_directory) {
                throw std::runtime_error(
                    "destination is not a directory"
                );
            }
            destination_directory = is_directory || dialog_destination_mask.has_value();
            if (sources.size() > 1U && !is_directory) {
                throw std::runtime_error(
                    "multiple sources require a directory destination"
                );
            }
        }
        dialog_exact = sources.size() == 1U &&
                       !destination_directory;
        const auto source_provider = operation_draft ? operation_draft->source_provider
            : tree_operation_source ? tree_operation_source_provider : source_pane.provider();
        if (dialog_stable_symlinks &&
            (dynamic_cast<LocalProvider*>(source_provider.get()) == nullptr ||
             dynamic_cast<LocalProvider*>(destination_provider.get()) == nullptr)) {
            throw std::runtime_error("stable symlinks require local source and destination filesystems");
        }
        run_operation(
            dialog_move,
            destination.path,
            !config.confirm_overwrite,
            dialog_exact,
            destination_provider
        );
    }

    void start_delete_dialog() {
        clear_tree_operation_context();
        if (active().selected_entries().empty()) {
            throw std::runtime_error("no source selected");
        }
        operation_draft = capture_operation_job();
        dialog_focus = config.safe_delete ? 1U : 0U;
        mode = Mode::delete_dialog;
    }

    void run_operation(
        const bool move_operation,
        const std::string& destination,
        const bool overwrite,
        const bool exact = false,
        ProviderPtr destination_override = {}
    ) {
        if (traversal_running.load(std::memory_order_acquire)) {
            throw std::runtime_error(
                "cannot start an operation while search is running"
            );
        }
        auto job = operation_draft ? operation_draft : capture_operation_job();
        job->sources = matching_operation_sources();
        if (job->sources.empty()) {
            throw std::runtime_error(
                "source mask matches no selected entries"
            );
        }
        job->kind = move_operation ? OperationJob::Kind::move : OperationJob::Kind::copy;
        job->label = move_operation ? "Move" : "Copy";
        job->destination = destination;
        job->exact = exact;
        job->background = dialog_background;
        if (destination_override) job->destination_provider = std::move(destination_override);
        job->options = OperationOptions{
            .overwrite = overwrite,
            .preserve_permissions = dialog_preserve_attributes,
            .preserve_symlinks = dialog_preserve_symlinks,
            .collision = {},
            .error = {},
            .compute_totals = config.compute_totals,
            .source_mask = dialog_source_mask,
            .destination_mask = dialog_destination_mask,
            .shell_patterns = dialog_shell_patterns,
            .stable_symlinks = dialog_stable_symlinks,
            .dive_into_subdirectories = dialog_dive_into_subdirectories,
        };
        const auto return_mode = tree_operation_source && modal_tree_snapshot
            ? Mode::directory_tree_dialog : Mode::panels;
        clear_tree_operation_context();
        start_job(job, return_mode);
    }

    void run_remove_operation() {
        if (traversal_running.load(std::memory_order_acquire)) {
            throw std::runtime_error(
                "cannot delete while search is running"
            );
        }
        auto job = operation_draft ? operation_draft : capture_operation_job();
        job->sources = current_operation_sources();
        if (job->sources.empty()) {
            throw std::runtime_error("no source selected");
        }
        job->kind = OperationJob::Kind::remove;
        job->label = "Delete";
        job->compute_totals = config.compute_totals;
        const auto return_mode = tree_operation_source && modal_tree_snapshot
            ? Mode::directory_tree_dialog : Mode::panels;
        clear_tree_operation_context();
        start_job(job, return_mode);
    }

    void run_job(const std::shared_ptr<OperationJob>& job) {
        OperationResult result;
        try {
            job->control.wait_until_resumed();
            const auto policy = std::make_shared<OperationPolicy>();
            auto options = job->options;
            options.error = error_handler(job, policy);
            options.collision = [this, job, policy](const Entry& source, const Entry& target) {
                if (policy->collisions) return *policy->collisions;
                const auto older = [&] {
                    return source.kind == EntryKind::directory || source.modified > target.modified
                        ? CollisionDecision::overwrite : CollisionDecision::skip;
                };
                if (policy->older) return older();
                switch (ask_operation(
                    job, source, target, {}, OperationEngine::collision_options(
                        *job->source_provider, source, *job->destination_provider, target
                    )
                )) {
                    case OperationChoice::overwrite: return CollisionDecision::overwrite;
                    case OperationChoice::append: return CollisionDecision::append;
                    case OperationChoice::reget: return CollisionDecision::reget;
                    case OperationChoice::skip: return CollisionDecision::skip;
                    case OperationChoice::overwrite_all:
                        policy->collisions = CollisionDecision::overwrite;
                        return CollisionDecision::overwrite;
                    case OperationChoice::skip_all:
                        policy->collisions = CollisionDecision::skip;
                        return CollisionDecision::skip;
                    case OperationChoice::older:
                        policy->older = true;
                        return older();
                    default: return CollisionDecision::cancel;
                }
            };
            const auto callback = [this, job](const TransferProgress& value) {
                if (!job->control.checkpoint(value, job->kind != OperationJob::Kind::remove)) return;
                post_ui([this, job] {
                    const auto current = job->control.consume_progress();
                    if (job->finished) return;
                    if (mode == Mode::operation_progress && progress_job == job) {
                        status = job->label + " " + display_text(current.current_path) +
                            " (" + std::to_string(current.files_done) +
                            "/" + std::to_string(current.files_total) + ")";
                    }
                });
            };
            const auto& cancellation = job->control.cancellation();
            if (job->kind == OperationJob::Kind::remove) {
                operations.remove(
                    *job->source_provider, job->sources, cancellation, callback, &result,
                    options.error, job->compute_totals,
                    [this, job](const Entry& directory) {
                        switch (ask_operation(job, directory, std::nullopt, {}, {}, true)) {
                            case OperationChoice::overwrite: return RecursiveDeleteDecision::yes;
                            case OperationChoice::skip: return RecursiveDeleteDecision::no;
                            case OperationChoice::overwrite_all: return RecursiveDeleteDecision::all;
                            case OperationChoice::skip_all: return RecursiveDeleteDecision::none;
                            default: return RecursiveDeleteDecision::cancel;
                        }
                    }
                );
            } else if (job->kind == OperationJob::Kind::move && job->exact) {
                if (job->sources.size() != 1U) throw std::runtime_error("exact move requires one source");
                operations.move_exact(
                    *job->source_provider, job->sources.front(), *job->destination_provider,
                    job->destination, options, cancellation, callback, &result
                );
            } else if (job->kind == OperationJob::Kind::move) {
                operations.move(
                    *job->source_provider, job->sources, *job->destination_provider,
                    job->destination, options, cancellation, callback, &result
                );
            } else if (job->exact) {
                operations.copy_exact(
                    *job->source_provider, job->sources.front(), *job->destination_provider,
                    job->destination, options, cancellation, callback, &result
                );
            } else {
                operations.copy(
                    *job->source_provider, job->sources, *job->destination_provider,
                    job->destination, options, cancellation, callback, &result
                );
            }
            post_ui([this, job, result = std::move(result)] {
                auto message = job->label + " complete";
                if (!result.skipped.empty()) {
                    message = job->label + " incomplete: " + std::to_string(result.skipped.size()) + " skipped";
                    if (job->kind != OperationJob::Kind::remove) {
                        message += " (" + display_text(result.skipped.front().reason) +
                            "; " + display_text(result.skipped.front().path) + ")";
                    }
                }
                finish_operation(job, std::move(message), result.completed_sources, !result.skipped.empty());
            });
        } catch (const OperationCancelled&) {
            post_ui([this, job, completed = std::move(result.completed_sources)] {
                finish_operation(job, job->kind == OperationJob::Kind::remove
                    ? "Delete cancelled" : "Operation cancelled", completed, true);
            });
        } catch (const std::exception& error) {
            post_ui([this, job, message = std::string(error.what()),
                     completed = std::move(result.completed_sources)] {
                finish_operation(job, "Error: " + message, completed, true);
            });
        }
    }

    void synchronize_shell_directory(const ShellResult& result) {
        if (!shell_panel_id || result.busy) return;
        auto& pane = pane_ids[0] == *shell_panel_id ? left : right;
        if (pane.path() != shell_expected_directory) return;
        if (pane.path() != result.directory) {
            pane.change_directory(result.directory);
            shell_expected_directory = pane.path();
        }
    }

    void process_shell_events(const bool finish_exit) {
        try {
            complete_pending_user_menu();
            complete_pending_extension();
            const auto result = shell.poll();
            if (!result) return;
            if (user_menu_pending && user_menu_pending->running && !result->busy) {
                user_menu_pending->completed_status = result->status;
            }
            if (extension_pending && extension_pending->running && !result->busy) {
                extension_pending->completed_status = result->status;
            }
            synchronize_shell_directory(*result);
            if (result->exited) {
                shell_exit_requested = true;
                if (finish_exit) {
                    shell_exit_requested = false;
                    request_app_exit();
                }
            } else {
                if (mode == Mode::panels && !command_mode && !panel_search_mode && !tree_search_mode) {
                    status = "Command exited " + std::to_string(result->status);
                }
                refresh();
            }
            complete_pending_user_menu();
            complete_pending_extension();
        } catch (const std::exception& error) {
            std::cerr << "mcpp: persistent shell: " << error.what() << '\n';
            if (!finish_exit) throw;
            set_error(error);
        }
    }

    int run_process(
        const std::string& command,
        const bool interactive,
        const std::string& directory = {}
    ) {
        if (traversal_running.load(std::memory_order_acquire)) {
            throw std::runtime_error(
                "cannot launch a command while search is running"
            );
        }
        const bool persistent = config.subshell_override.value_or(config.use_subshell) && ShellSession::supported();
        if (persistent) {
            process_shell_events(false);
            if (shell_exit_requested) throw std::runtime_error("the shell exited; command was not run");
            if (!command.empty() && shell.busy()) {
                throw ShellBusy("shell is already running a command; use Ctrl-O to resume it");
            }
            if (!shell.busy()) {
                shell_panel_id = directory.empty() && active().provider() == local
                    ? std::optional<int>{pane_ids[active_index]} : std::nullopt;
                shell_expected_directory = active().path();
            }
        }
        const auto execution_directory = directory.empty() ? pane_execution_directory(active()) : directory;
        const auto* executable = std::getenv("SHELL");
        if (executable == nullptr || *executable == '\0') executable = "/bin/sh";
        ShellResult result;
        std::exception_ptr failure;
        int child_status = 0;
        struct ShellForegroundGuard {
            bool& flag;
            bool previous;
            explicit ShellForegroundGuard(bool& value) : flag(value), previous(std::exchange(value, true)) {}
            ~ShellForegroundGuard() { flag = previous; }
        } foreground(shell_foreground);
        terminal->WithRestoredIO([&] {
            try {
                if (persistent) {
                    if (!shell.running()) std::cout << "\033[H\033[2J" << std::flush;
                    result = shell.show(execution_directory, command);
                } else {
                    if (interactive) std::cout << "\033[H\033[2J" << std::flush;
                    const auto child = fork();
                    if (child < 0) throw std::runtime_error("failed to fork shell");
                    if (child == 0) {
                        if (chdir(execution_directory.c_str()) != 0) _exit(126);
                        if (interactive) {
                            execlp(executable, executable, static_cast<char*>(nullptr));
                        } else {
                            execlp(executable, executable, "-c", command.c_str(), static_cast<char*>(nullptr));
                        }
                        _exit(127);
                    }
                    while (waitpid(child, &child_status, 0) < 0) {
                        if (errno != EINTR) throw std::runtime_error("failed to wait for shell");
                    }
                }
                const auto* term = std::getenv("TERM");
                const bool dumb_terminal = term != nullptr &&
                    (std::string_view(term) == "dumb" || std::string_view(term) == "linux");
                const bool finished = !persistent || (!result.busy && !result.exited);
                if (!interactive && finished && (config.pause_after_run == 2U ||
                    (config.pause_after_run == 1U && dumb_terminal))) {
                    struct termios original{};
                    if (tcgetattr(STDIN_FILENO, &original) != 0) {
                        throw std::system_error(errno, std::generic_category(), "read terminal settings");
                    }
                    auto one_key = original;
                    one_key.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));
                    one_key.c_cc[VMIN] = 1;
                    one_key.c_cc[VTIME] = 0;
                    if (tcsetattr(STDIN_FILENO, TCSANOW, &one_key) != 0) {
                        throw std::system_error(errno, std::generic_category(), "set pause input mode");
                    }
                    try {
                        std::cout << "\nPress any key to continue..." << std::flush;
                        char key = 0;
                        ssize_t count = 0;
                        do { count = read(STDIN_FILENO, &key, 1); } while (count < 0 && errno == EINTR);
                        if (count != 1) throw std::runtime_error("cannot read pause key");
                        std::cout << '\n' << std::flush;
                    } catch (...) {
                        if (tcsetattr(STDIN_FILENO, TCSANOW, &original) != 0) {
                            std::cerr << "mcpp: cannot restore terminal settings after pause\n";
                        }
                        throw;
                    }
                    if (tcsetattr(STDIN_FILENO, TCSANOW, &original) != 0) {
                        throw std::system_error(errno, std::generic_category(), "restore terminal settings");
                    }
                }
            } catch (...) {
                failure = std::current_exception();
            }
        })();
        if (failure) std::rethrow_exception(failure);
        if (persistent) {
            synchronize_shell_directory(result);
            shell_exit_requested = result.exited;
        }
        terminal_title.clear();
        refresh();
        if (persistent) return result.status;
        if (WIFEXITED(child_status)) {
            return WEXITSTATUS(child_status);
        }
        return WIFSIGNALED(child_status)
                   ? 128 + WTERMSIG(child_status)
                   : 1;
    }

    void execute_command() {
        command_prompt_override = false;
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
                const auto old_path = pane_location(active());
                change_panel_directory(active(), std::move(destination));
                previous = old_path;
                status = "Directory changed: " + pane_location(active());
                return;
            }
            const auto result = run_process(command, false);
            status = shell.busy() ? "Shell command is running; Ctrl-O resumes it"
                                  : "Command exited " + std::to_string(result);
        } catch (const ShellBusy& error) {
            command_line = command;
            command_input.reset(command_line);
            command_mode = true;
            set_error(error);
        } catch (const std::exception& error) {
            set_error(error);
        }
    }

    void execute_prompt() {
        try {
            remember_input(prompt_inputs[prompt_action], prompt_value);
        } catch (const std::exception& error) {
            prompt_error = error.what();
            return;
        }
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
                    prompt_action = PromptAction::none;
                    prompt_value.clear();
                    prompt_error.clear();
                    view_file_with_extensions(provider, path, Mode::panels);
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
                    auto& target = viewer_goto_quick ? quick_viewer : viewer;
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
                        target.goto_line(value);
                    } else if (viewer_goto_mode == ViewerGotoMode::percent) {
                        if (value > 100U) {
                            throw std::out_of_range("percentage must be between 0 and 100");
                        }
                        target.goto_percent(static_cast<unsigned int>(value));
                        if (value == 100U) {
                            const auto [width, height] = viewer_goto_quick ? quick_view_dimensions() : viewer_dimensions();
                            target.go_to_end(width, height);
                        }
                    } else {
                        if (value > target.data_size()) {
                            throw std::out_of_range("offset is outside the file");
                        }
                        target.goto_data_offset(value);
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
                    const auto location = prompt_value.empty()
                        ? Location{editor.open() ? editor.provider() : local, ""}
                        : resolve_editor_location(prompt_value);
                    const bool create = prompt_action == PromptAction::editor_new;
                    prompt_action = PromptAction::none;
                    prompt_value.clear();
                    mode = mode_before_prompt;
                    open_editor_buffer(location.provider, location.path, create);
                    return;
                }
                case PromptAction::editor_save_as:
                    if (prompt_value.empty()) throw std::invalid_argument("file name is empty");
                    request_editor_save_as();
                    return;
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
                case PromptAction::quick_cd: {
                    change_panel_directory(active(), prompt_value);
                    status = "Directory changed";
                    break;
                }
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
                prompt_action == PromptAction::quick_viewer_find ||
                prompt_action == PromptAction::editor_save_as) {
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

    std::string listing_value(const Pane& pane, const Entry& entry, const std::string& field) const {
        if (field == "type") return type_prefix(entry);
        if (field == "name" || field == "version" || field == "extension" || field == "unsorted") {
            return display_filename(pane, entry.name);
        }
        if (field == "size" || field == "bsize") {
            if (entry.revision == "mcpp:parent") return "UP--DIR";
            return field == "size" ? format_panel_size(entry.size, config.use_si_units) : format_size(entry.size);
        }
        if (field == "perm") return permission_text(entry);
        if (field == "mode") {
            std::ostringstream value;
            value << std::oct << std::setfill('0') << std::setw(6) << entry.mode;
            return value.str();
        }
        if (field == "mtime") return format_panel_time(entry.modified);
        if (field == "atime") return format_panel_time(entry.accessed);
        if (field == "ctime") return format_panel_time(entry.changed);
        if (field == "inode") return std::to_string(entry.inode);
        if (field == "nlink") return std::to_string(entry.links);
        if (field == "nuid") return std::to_string(entry.uid);
        if (field == "ngid") return std::to_string(entry.gid);
        if (field == "owner") return display_text(entry.owner);
        if (field == "group") return display_text(entry.group);
        if (field == "mark") return pane.is_selected(entry) ? "*" : " ";
        if (field == "space") return " ";
        if (field == "dot") return ".";
        throw std::runtime_error("unsupported display field: " + field);
    }

    static std::string fit_listing_text(
        const std::string& value, const int width, const ListingAlignment alignment
    ) {
        if (width <= 0) return {};
        auto fitted = middle_truncate(value, static_cast<std::size_t>(width));
        const auto padding = static_cast<std::size_t>(std::max(0, width - ftxui::string_width(fitted)));
        const auto before = alignment == ListingAlignment::right ? padding
                          : alignment == ListingAlignment::center ? padding / 2U : 0U;
        return std::string(before, ' ') + fitted + std::string(padding - before, ' ');
    }

    ftxui::Element render_listing_fields(
        const Pane& pane, const Entry* entry, const ListingSpec& spec, const int width,
        const bool header = false
    ) const {
        using namespace ftxui;
        const auto widths = listing_field_widths(spec, width);
        Elements fields;
        for (std::size_t index = 0; index < spec.fields.size(); ++index) {
            if (widths[index] == 0) continue;
            const auto& field = spec.fields[index];
            if (field.name == "|") {
                fields.push_back(separator() | size(WIDTH, EQUAL, widths[index]));
            } else {
                const auto value = header ? field.title
                                 : entry == nullptr ? std::string{} : listing_value(pane, *entry, field.name);
                const auto fitted = fit_listing_text(value, widths[index],
                    header ? ListingAlignment::center : field.alignment);
                if (!header && entry != nullptr && config.highlight_permissions &&
                    (field.name == "perm" || field.name == "mode")) {
                    const auto group = entry->uid == geteuid() ? 0
                                     : entry->gid == getegid() ||
                                       std::ranges::find(effective_groups, entry->gid) != effective_groups.end() ? 1 : 2;
                    const auto start = field.name == "perm" ? group * 3 + 1 : widths[index] + group - 3;
                    const auto end = start + (field.name == "perm" ? 3 : 1);
                    Elements permission;
                    for (std::size_t character = 0; character < fitted.size(); ++character) {
                        auto cell = text(fitted.substr(character, 1));
                        if (static_cast<int>(character) >= start && static_cast<int>(character) < end) {
                            cell = cell | bold | underlined;
                        }
                        permission.push_back(std::move(cell));
                    }
                    fields.push_back(hbox(std::move(permission)));
                } else fields.push_back(text(fitted));
            }
        }
        return hbox(std::move(fields)) | size(WIDTH, EQUAL, std::max(0, width));
    }

    std::string listing_status(const Pane& pane, const Entry& entry, const int width) const {
        if (entry.revision == "mcpp:parent") return "UP--DIR";
        const auto id = panel_config_index(pane);
        const auto format = config.panel_listing[id];
        const auto spec = parse_listing_format(config.panel_user_mini_status[id]
            ? config.panel_status_formats[id][static_cast<std::size_t>(format)]
            : mini_status_format(format, config.panel_columns[id]));
        const auto widths = listing_field_widths(spec, width);
        std::string result;
        for (std::size_t index = 0; index < spec.fields.size(); ++index) {
            const auto& field = spec.fields[index];
            result += fit_listing_text(field.name == "|" ? "|" : listing_value(pane, entry, field.name),
                                       widths[index], field.alignment);
        }
        return result;
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
        const auto spec = pane_listing_spec(pane);
        const auto visible_rows = panel_visible_rows(&pane == &left ? 0 : 1);
        const auto slots = visible_rows * spec.columns;
        pane.set_viewport_rows(slots);
        Elements rows;
        Elements headers;
        const auto& entries = pane.entries();
        rows.reserve(visible_rows);
        entry_boxes.clear();
        entry_boxes.resize(entries.size());
        const auto end = std::min(entries.size(), pane.first_row() + slots);
        const auto decorate = [&](const std::size_t index, Element row) {
            const auto& entry = entries[index];
            if (index == pane.cursor()) {
                row = gray_selected(std::move(row), pane.is_selected(entry), focused);
            } else {
                row = config.highlight_file_types ? gray_entry(std::move(row), entry, skin_id() == "type-colors",
                                                               pane.provider() == local)
                                                  : std::move(row) | color(palette().normal.foreground);
                if (pane.is_selected(entry)) {
                    row = palette().resource.empty() ? row | bold | color(palette().bright)
                        : paint(std::move(row), skin_color(palette(), "core", "marked", palette().normal));
                }
            }
            return std::move(row) | reflect(entry_boxes[index]);
        };
        const auto column_width = [&](const std::size_t column) {
            const auto content = std::max(0, inner_width - static_cast<int>(spec.columns - 1U));
            return content / static_cast<int>(spec.columns) +
                   (column + 1U == spec.columns ? content % static_cast<int>(spec.columns) : 0);
        };
        const auto field_width = std::max(0, (panel_width_value - 1) / static_cast<int>(spec.columns) - 1);
        for (std::size_t column = 0; column < spec.columns; ++column) {
            if (column != 0U) headers.push_back(separator());
            headers.push_back(hbox({
                render_listing_fields(pane, nullptr, spec, field_width, true), filler()
            }) | size(WIDTH, EQUAL, column_width(column)));
        }
        for (std::size_t row = 0; row < visible_rows; ++row) {
            Elements cells;
            for (std::size_t column = 0; column < spec.columns; ++column) {
                if (column != 0U) cells.push_back(separator());
                const auto index = pane.first_row() + row + column * visible_rows;
                auto line = render_listing_fields(pane, index < end ? &entries[index] : nullptr,
                                                   spec, field_width);
                if (index < end) line = decorate(index, std::move(line));
                cells.push_back(hbox({std::move(line), filler()}) |
                                size(WIDTH, EQUAL, column_width(column)));
            }
            rows.push_back(hbox(std::move(cells)));
        }
        const auto* current = pane.current();
        auto info =
            current == nullptr
                ? std::string{}
                : listing_status(pane, *current, inner_width);
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
        } catch (const std::exception&) {
            available = "Space unavailable";
        }
        if (!config.show_free_space) available.clear();
        auto title = text(" " + path_label(
            display_filename(pane, pane.provider() == local ? pane.path() : short_archive_location(pane_location(pane))) +
                (pane.panelized() ? " [panelized]" : ""),
            static_cast<std::size_t>(
                std::max(1, panel_width_value - 8)
            )
        ) + " ");
        if (focused) {
            title = title | color(palette().bright) | bold;
        } else {
            title = title | color(palette().inactive_title);
        }
        Elements panel_rows{hbox(std::move(headers)) | color(palette().column_header) | bold,
                            vbox(std::move(rows)) | flex};
        if (config.show_mini_status) {
            panel_rows.push_back(separator());
            panel_rows.push_back(text(middle_truncate(info, static_cast<std::size_t>(inner_width))) |
                                 size(WIDTH, EQUAL, inner_width));
        }
        auto content = vbox(std::move(panel_rows));
        auto framed = window(title, content, LIGHT);
        if (!available.empty() && panel_width_value > 8) {
            available = middle_truncate(available, static_cast<std::size_t>(panel_width_value - 6));
            framed = dbox({framed, vbox({
                filler(),
                hbox({filler(), text(" " + available + " ") | color(palette().dim),
                      filler() | size(WIDTH, EQUAL, 1)}),
            })});
        }
        return framed |
               size(WIDTH, EQUAL, panel_width_value) |
               yflex |
               reflect(pane_box);
    }

    ftxui::Element render_viewer() {
        using namespace ftxui;
        const auto [width, height] = viewer_dimensions();
        static_cast<void>(refresh_viewer());
        std::vector<std::string> rows;
        try {
            rows = viewer.lines(width, height);
        } catch (const std::exception& error) {
            if (mode == Mode::viewer) {
                viewer_message_title = "Viewer error";
                viewer_message = error.what();
                mode = Mode::viewer_message;
            }
        }
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
                viewer.hex_cursor() - row_offset < rendered.bytes) {
                const auto byte = static_cast<std::size_t>(viewer.hex_cursor() - row_offset);
                const auto prefix = row.find("  ") + 2U;
                const auto cursor = prefix +
                    (viewer.hex_ascii() ? 49U + byte : byte * 3U + low_nibble);
                const auto counterpart = prefix + (viewer.hex_ascii() ? byte * 3U : 49U + byte);
                const auto counterpart_width = viewer.hex_ascii() ? 2U : 1U;
                const auto first = std::min(viewer_hex_first_column, row.size());
                const auto end = std::min(row.size(), first + width);
                const auto part_kind = [&](const std::size_t column) {
                    if (column >= cursor && column - cursor < cursor_width) return 1;
                    if (column >= counterpart && column - counterpart < counterpart_width) return 2;
                    return 0;
                };
                Elements parts;
                for (auto begin = first; begin < end;) {
                    const auto kind = part_kind(begin);
                    auto finish = begin + 1U;
                    while (finish < end && part_kind(finish) == kind) ++finish;
                    auto part = text(row.substr(begin, finish - begin));
                    if (kind == 1) part = highlight(std::move(part), palette().viewer_cursor) | bold | focusCursorBlock;
                    else if (kind == 2) part = paint(std::move(part), palette().viewer_cursor);
                    parts.push_back(std::move(part));
                    begin = finish;
                }
                line = hbox(std::move(parts));
            }
            if (!viewer.hex()) {
                line = render_viewer_text(row, viewer.text_highlight(index), viewer.text_styles(index));
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
        if (rows.empty() && viewer.streamed() && viewer.reading()) {
            elements.push_back(text("Waiting for input..."));
        } else if (rows.empty() && viewer.streamed() && viewer.data_size() == 0U &&
                   viewer.source_error().empty()) {
            elements.push_back(text("End of input (0 bytes)"));
        }
        if (viewer_ruler != 0U) {
            std::string ruler(width, '-');
            for (std::size_t column = 0; column < width; ++column) {
                const auto value = viewer.horizontal_offset() + column + 1U;
                if (value % 10U == 0U) ruler[column] = static_cast<char>('0' + value / 10U % 10U);
                else if (value % 5U == 0U) ruler[column] = '+';
            }
            auto line = gray_menu(text(ruler));
            if (viewer_ruler == 1U) elements.insert(elements.begin(), std::move(line));
            else elements.push_back(std::move(line));
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
                position << std::setw(9) << viewer.source_view_end();
            }
            position << "/" << viewer.data_size() << (viewer.reading() ? "+" : "")
                     << " M" << viewer.bookmark_slot() << " ";
            std::ostringstream percentage;
            percentage << std::setw(3) << percent << "%";
            header = hbox({
                text(path_label(
                    display_text(viewer_title() + (viewer.reading() ? " [Reading]" : !viewer.source_error().empty() ? " [Error]" : "")),
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
                    display_text(viewer_title() + (viewer.reading() ? " [Reading]" : !viewer.source_error().empty() ? " [Error]" : "")),
                    screen_width > 5U ? screen_width - 5U : 1U
                )) |
                    flex,
                screen_width > 26U
                    ? text(percentage.str())
                    : text(""),
            });
        }
        return vbox({
            paint(std::move(header), palette().viewer_cursor),
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
                key("8", viewer.magic() ? "Raw" : "Parse", true),
                key("9", viewer.nroff() ? "Unform" : "Format", true),
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
                   text((viewer_search_quick ? quick_view_search_options.backwards : viewer_search_options.backwards)
                            ? "Search reached the beginning" : "Search reached the end") | bold | center,
                   separator(),
                   text((viewer_search_quick ? quick_view_search_options.backwards : viewer_search_options.backwards)
                            ? "Continue from the end?" : "Continue from the beginning?"),
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
    ftxui::Element style_syntax(
        ftxui::Element element,
        const SyntaxSpan& span
    ) const {
        using namespace ftxui;
        if (span.color) {
            const auto& appearance = *span.color;
            const auto terminal_color = [](const int index) {
                return index == SyntaxColor::terminal_default ? Color{} :
                    Color(static_cast<Color::Palette256>(index));
            };
            if (appearance.foreground != SyntaxColor::base) {
                element = std::move(element) | color(terminal_color(appearance.foreground));
            }
            if (appearance.background != SyntaxColor::base) {
                element = std::move(element) | bgcolor(terminal_color(appearance.background));
            }
            if ((appearance.attributes & SyntaxColor::bold) != 0U) element = std::move(element) | bold;
            if ((appearance.attributes & SyntaxColor::italic) != 0U) element = std::move(element) | italic;
            if ((appearance.attributes & SyntaxColor::underline) != 0U) element = std::move(element) | underlined;
            if ((appearance.attributes & SyntaxColor::reverse) != 0U) element = std::move(element) | inverted;
            if ((appearance.attributes & SyntaxColor::blink) != 0U) element = std::move(element) | blink;
            return element;
        }
        switch (span.style) {
            case SyntaxStyle::keyword:
                return std::move(element) |
                       color(palette().syntax[1]) | bold;
            case SyntaxStyle::type:
                return std::move(element) |
                       color(palette().syntax[2]);
            case SyntaxStyle::string_literal:
                return std::move(element) |
                       color(palette().syntax[3]);
            case SyntaxStyle::comment:
                return std::move(element) |
                       color(palette().syntax[4]) | italic;
            case SyntaxStyle::number:
                return std::move(element) |
                       color(palette().syntax[5]);
            case SyntaxStyle::preprocessor:
                return std::move(element) |
                       color(palette().syntax[6]) | bold;
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
        auto match_begin = line.size();
        auto match_end = std::size_t{0};
        if (const auto match = mode == Mode::editor_spelling_dialog ? spelling_match : editor.search_selection();
            match && index >= match->first.row && index <= match->second.row) {
            match_begin = index == match->first.row ? match->first.column : 0U;
            match_end = index == match->second.row ? match->second.column : line.size();
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
            std::clamp(match_begin, visible_begin, visible_end),
            std::clamp(match_end, visible_begin, visible_end),
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
                config.editor_visible_tabs,
                config.editor_visible_trailing_spaces &&
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
            const bool matched = begin >= match_begin && begin < match_end;
            if (cursor && line[begin] == '\t') {
                segments.push_back(
                    focusCursorBlock(
                        text(
                            config.editor_visible_tabs &&
                                    !displayed.empty()
                                ? displayed.substr(0U, 1U)
                                : " "
                        ) |
                        color(palette().selected.foreground) |
                        bgcolor(palette().selected.background) |
                        bold
                    )
                );
                if (displayed.size() > 1U) {
                    auto remainder = text(displayed.substr(1U));
                    if (matched) {
                        remainder = paint(std::move(remainder), palette().search) | bold;
                    } else if (selected) {
                        remainder = remainder |
                                    color(palette().selected.foreground) |
                                    bgcolor(palette().selected.background);
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
                    color(palette().selected.foreground) |
                    bgcolor(palette().selected.background) |
                    bold
                );
            } else if (matched) {
                segment = paint(std::move(segment), palette().search) | bold;
            } else if (selected) {
                segment = segment |
                          color(palette().selected.foreground) |
                          bgcolor(palette().selected.background);
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
                        syntax_spans[index][syntax_index]
                    );
                }
            }
            segments.push_back(std::move(segment));
            display_begin += display_width;
        }
        const auto line_width = editor_display_width(line, line.size(), editor.tab_width());
        const auto padding_begin = std::max(first_column, line_width);
        auto padding_end = padding_begin;
        std::optional<std::pair<std::size_t, std::size_t>> virtual_mark;
        if (editor.column_selection()) {
            const auto selected = editor.selection();
            if (selected && index >= selected->first.row && index <= selected->second.row) {
                virtual_mark = editor.column_mark_columns();
                padding_end = std::max(padding_end, virtual_mark->second);
            }
        }
        const auto virtual_cursor = index == editor.row() && editor.column() == line.size() && visible_end == line.size()
            ? std::optional<std::size_t>{editor.display_cursor_column()} : std::nullopt;
        if (virtual_cursor) padding_end = std::max(padding_end, *virtual_cursor + 1U);
        std::vector<std::size_t> padding_bounds{padding_begin, padding_end};
        if (virtual_mark) {
            padding_bounds.push_back(std::clamp(virtual_mark->first, padding_begin, padding_end));
            padding_bounds.push_back(std::clamp(virtual_mark->second, padding_begin, padding_end));
        }
        if (virtual_cursor) {
            padding_bounds.push_back(std::clamp(*virtual_cursor, padding_begin, padding_end));
            padding_bounds.push_back(std::clamp(*virtual_cursor + 1U, padding_begin, padding_end));
        }
        std::ranges::sort(padding_bounds);
        for (std::size_t boundary = 1; boundary < padding_bounds.size(); ++boundary) {
            const auto begin = padding_bounds[boundary - 1U], end = padding_bounds[boundary];
            if (end <= begin) continue;
            auto cell = text(std::string(end - begin, ' '));
            if (virtual_cursor && begin == *virtual_cursor) {
                cell = focusCursorBlock(paint(std::move(cell), palette().selected) | bold);
            } else if (virtual_mark && begin >= virtual_mark->first && begin < virtual_mark->second) {
                cell = paint(std::move(cell), palette().selected);
            }
            segments.push_back(std::move(cell));
        }
        if (segments.empty()) {
            segments.push_back(text(""));
        }
        auto content = hbox(std::move(segments));
        if (editor.bookmarks().contains(index)) content = content | underlined;
        if (!editor_line_numbers) {
            return content;
        }
        return hbox({
            text(std::to_string(index + 1U)) |
                color(palette().dim) |
                size(WIDTH, EQUAL, 6),
            std::move(content),
        });
    }

    ftxui::Element render_editor() {
        using namespace ftxui;
        update_editor_view();
        auto terminal_size = Terminal::Size();
        const auto dimensions = editor_dimensions();
        terminal_size.dimx = dimensions.first;
        terminal_size.dimy = dimensions.second;
        const auto visible_rows = editor_visible_rows();
        const auto& lines = editor.lines();
        if (editor_syntax && syntax_buffer_path != editor.path()) {
            syntax_buffer_path = editor.path();
            try {
                reload_syntax_rules();
            } catch (const std::exception& error) {
                report_syntax_error(error);
            }
        }
        const auto syntax_language = effective_syntax_language();
        const auto syntax_identity = editor.path() + "\n" + syntax_language;
        if (editor_syntax &&
            (
                syntax_change_id != editor.change_id() ||
                syntax_path != syntax_identity ||
                syntax_spans.size() != lines.size()
            )) {
            try {
                if (!syntax_load_error.empty() &&
                    (syntax_language == "auto" || syntax_language.starts_with("external:"))) {
                    throw std::runtime_error(syntax_load_error);
                }
                syntax_spans = syntax_highlighter.highlight(
                    editor.path(),
                    lines,
                    syntax_language
                );
            } catch (const std::exception& error) {
                syntax_spans.assign(lines.size(), {});
                report_syntax_error(error);
            }
            syntax_change_id = editor.change_id();
            syntax_path = syntax_identity;
        } else if (!editor_syntax && !syntax_spans.empty()) {
            syntax_spans.clear();
        }
        const auto first = editor.first_row;
        const auto end = std::min(lines.size(), first + visible_rows);
        const auto& current_line = lines[editor.row()];
        const auto cursor_width = editor.display_cursor_column();
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
            editor_line_first_columns.push_back(
                editor_first_visible_column(lines[index], first_column, editor.tab_width())
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

        Elements menu;
        for (std::size_t index = 0; index < editor_menu_names.size(); ++index) {
            const bool selected = mode == Mode::editor_menu && index == editor_menu_category;
            auto item = mnemonic_label(
                editor_menu_names[index],
                editor_menu_mnemonics[index],
                selected ? std::nullopt : std::optional<ftxui::Color>{palette().menu_hot}
            );
            if (selected) {
                item = paint(std::move(item), palette().menu_selected) | bold;
            }
            menu.push_back(
                std::move(item) | reflect(editor_menu_category_boxes[index])
            );
        }
        const bool at_eof = editor.cursor_offset() == editor.byte_size();
        const auto character =
            editor.column() == current_line.size() && !at_eof
                ? static_cast<unsigned int>(editor.line_separator())
                : utf8_codepoint(current_line, editor.column());
        std::ostringstream editor_status;
        editor_status
            << " L:[" << editor.row() + 1U
            << " C:" << cursor_width + 1U
            << "/" << editor.lines().size() << "]"
            << " *("             << editor.storage_selection_size()
            << "/" << editor.storage_cursor_offset()
            << "/" << editor.storage_byte_size() << "b)";
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
            (macro_recording ||
             (mode == Mode::native_key_query && key_query_title == "Save macro") ||
             (mode == Mode::native_input && native_input_title == "Repeat last commands")) ? 'R' : '-',
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
        auto status_header = hbox({
                      text(path_label(
                          display_text(editor_location(editor)), filename_width
                      )) | bold | flex,
                      text(editor_flags),
                      text(position_status),
                  });
        status_header = gray_menu(std::move(status_header)) |
                        size(WIDTH, EQUAL, terminal_size.dimx);
        auto menubar = gray_menu(hbox(std::move(menu))) | reflect(editor_header_box);
        auto header = mode == Mode::editor_menu ? menubar : status_header | reflect(editor_header_box);
        const auto key = [this](
                             const std::string& number,
                             const std::string& label
                         ) {
            return render_function_key(
                static_cast<std::size_t>(std::stoul(number) - 1U),
                label, mode == Mode::editor
            );
        };
        auto keybar = hbox({
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
            });
        auto body = vbox(std::move(elements)) | flex;
        if (editor.fullscreen) return vbox({header, std::move(body), keybar});
        const auto bounds = editor_window_bounds();
        auto framed = dbox({
            std::move(body) | borderStyled(LIGHT),
            vbox({hbox({filler() | size(WIDTH, EQUAL, 1), status_header,
                         filler() | size(WIDTH, EQUAL, 1)}), filler()}),
        }) | size(WIDTH, EQUAL, bounds[2]) | size(HEIGHT, EQUAL, bounds[3]);
        return vbox({
            menubar,
            vbox({
                filler() | size(HEIGHT, EQUAL, std::max(0, bounds[1] - 1)),
                hbox({filler() | size(WIDTH, EQUAL, bounds[0]), std::move(framed), filler()}),
                filler(),
            }) | flex,
            keybar,
        });
    }

    static const std::array<
        std::vector<MenuRow>,
        7
    >& editor_menu_items() {
        static const std::array<std::vector<MenuRow>, 7> menus{{
            {
                {"Open file...", 0, 'o'},
                {"New              C-n", 1, 'n'},
                {"Close", 2, 'c'},
                {"History...       M-E", 5, 'h'},
                {"", -2},
                {"Save             F2", 3, 's'},
                {"Save as...       F12", 4, 'a'},
                {"", -2},
                {"Insert file...   F15", 7, 'i'},
                {"Copy to file...  C-f", 8, 'y'},
                {"", -2},
                {"User menu...     F11", 9, 'u'},
                {"", -2},
                {"About...", 10, 'b'},
                {"", -2},
                {"Quit             F10", 6, 'q'},
            },
            {
                {"Undo                 C-u", 0, 'u'},
                {"Redo                 M-r", 1, 'r'},
                {"", -2},
                {"Toggle ins/overw     Ins", 2, 't'},
                {"", -2},
                {"Toggle mark          F3", 3, 'g'},
                {"Mark columns         F13", 4, 'm'},
                {"Mark all", 5, 'a'},
                {"Unmark", 6, 'k'},
                {"", -2},
                {"Copy                 F5", 7, 'y'},
                {"Move                 F6", 8, 'v'},
                {"Delete               F8", 9, 'd'},
                {"", -2},
                {"Copy to clipfile     C-Ins", 10, 'p'},
                {"Cut to clipfile      S-Del", 11, 'c'},
                {"Paste from clipfile  S-Ins", 12, 's'},
                {"", -2},
                {"Beginning            C-Home", 13, 'b'},
                {"End                  C-End", 14, 'e'},
            },
            {
                {"Search...        F7", 0, 's'},
                {"Search again     F17", 1, 'a'},
                {"Replace...       F4", 2, 'r'},
                {"", -2},
                {"Toggle bookmark  M-k", 3, 't'},
                {"Next bookmark    M-j", 4, 'n'},
                {"Prev bookmark    M-i", 5, 'p'},
                {"Flush bookmarks  M-o", 6, 'f'},
            },
            {
                {"Go to line...                M-l", 0, 'g'},
                {"Toggle line state            M-n", 2, 't'},
                {"Go to matching bracket       M-b", 1, 'b'},
                {"Toggle syntax highlighting   C-s", 3, 'y'},
                {"", -2},
                {"Find declaration             M-Enter", 4, 'f'},
                {"Back from declaration        M--", 5, 'd'},
                {"Forward to declaration       M-+", 6, 'w'},
                {"", -2},
                {"Encoding...                  M-e", 7, 'i'},
                {"", -2},
                {"Refresh screen               C-l", 8, 'r'},
                {"", -2},
                {"Start/Stop record macro      C-r", 9, 's'},
                {"Delete macro...", 10, 'o'},
                {"Record/Repeat actions", 11, 'a'},
                {"", -2},
                {"Spell check", 13, 'p'},
                {"Check word                   C-p", 14, 'h'},
                {"Change spelling language...", 15, 'l'},
                {"", -2},
                {"Mail...", 12, 'm'},
            },
            {
                {"Insert literal...   C-q", 0, 'l'},
                {"Insert date/time", 1, 'd'},
                {"", -2},
                {"Format paragraph    M-p", 2, 'f'},
                {"Sort...             M-t", 3, 's'},
                {"Paste output of...  M-u", 4, 'p'},
                {"External formatter", 5, 'e'},
            },
            {
                {"Move", 3, 'm'},
                {"Resize", 4, 'r'},
                {"Toggle fullscreen", 5, 't'},
                {"", -2},
                {"Next", 0, 'n'},
                {"Previous", 1, 'p'},
                {"List...", 2, 'l'},
            },
            {
                {"General...", 0, 'g'},
                {"Save mode...", 2, 'm'},
                {"Learn keys...", 3, 'k'},
                {"Syntax highlighting...", 4, 'h'},
                {"", -2},
                {"Syntax file", 5, 'y'},
                {"Menu file", 6, 'm'},
                {"", -2},
                {"Save setup", 1, 's'},
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
            const bool selected = index == editor_menu_item;
            auto row = menu_row_label(items[index], selected || items[index].action < 0
                ? std::nullopt : std::optional<ftxui::Color>{palette().menu_hot});
            if (items[index].action < 0) {
                row = row | color(palette().disabled) | dim;
            }
            if (selected && items[index].action >= 0) {
                row = highlight(std::move(row), palette().popup_selected) | focus;
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
                element = focus_dialog(std::move(element));
            }
            return element | reflect(editor_save_boxes[index]);
        };
        return vbox({
                   text("Save file") | bold | center,
                   separator(),
                   text("\"" + path_label(
                       display_text(editor_location(editor)),
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
        const auto& path = editor_save_purpose == EditorSavePurpose::save_as
                               ? editor_save_path
                               : editor_save_buffer->path();
        return render_link_dialog(path, editor_link_choice, editor_link_boxes);
    }

    ftxui::Element render_link_dialog(
        const std::string& path,
        const std::size_t choice,
        std::array<ftxui::Box, 3>& boxes
    ) {
        using namespace ftxui;
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
                       dialog_button("Detach", choice == 0U, boxes[0]),
                       text(" "),
                       dialog_button("Preserve links", choice == 1U, boxes[1]),
                   }) | center,
                   dialog_button("Cancel", choice == 2U, boxes[2]) | center,
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

    ftxui::Element render_editor_error() {
        using namespace ftxui;
        return paint(vbox({
                   text(editor_message_title) | bold | center,
                   separator(),
                   paragraph(display_text(editor_error_message)),
                   separator(),
                   dialog_button("OK", true, message_button_box) | center,
               }) |
               borderStyled(LIGHT) |
               size(WIDTH, LESS_THAN, 72), palette().error);
    }

    ftxui::Element render_editor_search_dialog() {
        using namespace ftxui;
        const bool replace = mode == Mode::editor_replace_dialog;
        const auto focused = [&](Element element,
                                 const std::size_t index) {
            if (editor_dialog_focus == index) {
                element = focus_dialog(std::move(element));
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
            editor_dialog_error.empty() ? emptyElement() : paragraph(display_text(editor_dialog_error)),
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
        static const std::array<std::string, 13> names{
            "Fake half tabs", "Backspace through tabs", "Fill tabs with spaces",
            "Return does autoindent", "Confirm before saving", "Save file position",
            "Visible trailing spaces", "Visible tabs", "Syntax highlighting",
            "Cursor after inserted block", "Persistent selection", "Cursor beyond end of line", "Group undo"
        };
        static constexpr std::array<std::size_t, 13> hotkeys{0,0,15,0,6,10,0,8,5,1,4,9,0};
        const auto check = [&](const std::size_t index) {
            auto row = hbox({text(editor_options_values[index] ? " [x] " : " [ ] "),
                             mnemonic_label(names[index], hotkeys[index])}) |
                       reflect(editor_options_boxes[index + 3U]);
            const auto focus_index = index < 3U ? index + 1U : index + 2U;
            if (editor_options_focus == focus_index) row = focus_dialog(std::move(row));
            return row;
        };
        static const std::array<std::string, 3> wrap_names{"None", "Dynamic paragraphing", "Type writer wrap"};
        static constexpr std::array<std::size_t, 3> wrap_keys{0,0,5};
        Elements wrap;
        for (std::size_t index = 0; index < 3U; ++index) {
            auto row = hbox({text(editor_options_wrap == index ? " (*) " : " ( ) "),
                             mnemonic_label(wrap_names[index], wrap_keys[index])}) |
                       reflect(editor_options_boxes[index]);
            if (editor_options_focus == 0U && editor_options_wrap == index) row = focus_dialog(std::move(row));
            wrap.push_back(std::move(row));
        }
        const bool columns = Terminal::Size().dimx >= 77;
        const auto group_width = columns ? 35 : std::max(1, Terminal::Size().dimx - 6);
        auto left_column = vbox({
            window(text(" Wrap mode "), vbox(std::move(wrap)), LIGHT),
            text(""), text(""),
            window(text(" Tabulation "), vbox({
                check(0), check(1), check(2),
                render_input(editor_options_tab_width, editor_tab_input, editor_options_focus == 4U, " Tab spacing: ") |
                    reflect(editor_options_boxes[16])
            }), LIGHT)
        }) | size(WIDTH, EQUAL, group_width);
        Elements other;
        for (std::size_t index = 3; index < names.size(); ++index) other.push_back(check(index));
        other.push_back(render_input(editor_options_wrap_length, editor_wrap_input, editor_options_focus == 15U,
                                     " Word wrap line length: ") | reflect(editor_options_boxes[17]));
        auto right_column = window(text(" Other options "), vbox(std::move(other)), LIGHT) | size(WIDTH, EQUAL, group_width);
        auto body = columns ? hbox({text(" "), left_column, text(" "), right_column, text(" ")})
                            : vbox({left_column, right_column});
        Elements rows{
            body | yframe | size(HEIGHT, LESS_THAN, std::max(1, Terminal::Size().dimy - 6)) |
                reflect(editor_options_viewport)
        };
        if (!editor_options_error.empty()) {
            rows.push_back(paragraph(display_text(editor_options_error)) | bold);
        }
        rows.push_back(separator());
        rows.push_back(hbox({
            native_button("OK", true, editor_options_focus == 16U, editor_options_boxes[18]), text(" "),
            native_button("Cancel", false, editor_options_focus == 17U, editor_options_boxes[19])
        }) | center);
        return native_window("Editor options", vbox(std::move(rows)), 75);
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
                    row = focus_dialog(std::move(row));
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
                    row = focus_dialog(std::move(row));
                }
                rows.push_back(std::move(row) | reflect(editor_list_boxes[index]));
            }
        }
        if (rows.empty()) {
            rows.push_back(text("<empty>") | dim);
        }
        return vbox({
                   text(
                       history ? "History" : "Open files"
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

    bool help_key_capture() const {
        return mode == Mode::native_key_query || mode == Mode::learn_key ||
               mode == Mode::learn_keys_dialog ||
               (mode == Mode::editor && editor_window_action != 0);
    }

    HelpTopic help_topic_for_mode(const Mode context, const unsigned depth = 0) const {
        if (depth > 6U) return HelpTopic::dialogs;
        switch (context) {
            case Mode::panels:
                if (tree_panel && tree_panel_focused) return HelpTopic::tree;
                if (quick_view_panel && quick_view_focused) return HelpTopic::quick_view;
                return command_mode ? HelpTopic::command_line : HelpTopic::manager;
            case Mode::viewer: return HelpTopic::viewer;
            case Mode::viewer_message:
            case Mode::viewer_close_dialog:
            case Mode::viewer_link_dialog: return HelpTopic::viewer_edit;
            case Mode::viewer_wrap_dialog: return HelpTopic::viewer_search;
            case Mode::editor:
            case Mode::editor_menu:
            case Mode::editor_mail_dialog:
            case Mode::editor_spelling_dialog: return HelpTopic::editor;
            case Mode::editor_error:
            case Mode::editor_save_dialog:
            case Mode::editor_save_mode_dialog:
            case Mode::editor_newline_warning:
            case Mode::editor_close_dialog:
            case Mode::editor_link_dialog:
            case Mode::editor_overwrite_dialog: return HelpTopic::editor_save;
            case Mode::editor_search_dialog:
            case Mode::editor_replace_dialog:
            case Mode::editor_replace_confirm: return HelpTopic::editor_search;
            case Mode::editor_history:
            case Mode::editor_buffer_list: return HelpTopic::editor_windows;
            case Mode::diff:
            case Mode::diff_options: return HelpTopic::diff;
            case Mode::file_search_dialog:
                return file_search_target == FileSearchTarget::diff ? HelpTopic::diff
                                                                    : HelpTopic::viewer_search;
            case Mode::prompt:
                switch (prompt_action) {
                    case PromptAction::editor_save_as: return HelpTopic::editor_save;
                    case PromptAction::editor_open:
                    case PromptAction::editor_new: return HelpTopic::editor_windows;
                    case PromptAction::editor_goto: return HelpTopic::editor;
                    case PromptAction::viewer_find:
                    case PromptAction::viewer_goto:
                    case PromptAction::quick_viewer_find: return HelpTopic::viewer_search;
                    case PromptAction::viewer_path:
                    case PromptAction::view_file: return HelpTopic::viewer;
                    case PromptAction::select_pattern:
                    case PromptAction::unselect_pattern: return HelpTopic::selection;
                    case PromptAction::recursive_search:
                    case PromptAction::panelize_search: return HelpTopic::find_files;
                    case PromptAction::chmod:
                    case PromptAction::chown:
                    case PromptAction::hardlink:
                    case PromptAction::symlink:
                    case PromptAction::relative_symlink: return HelpTopic::permissions;
                    case PromptAction::panel_filter:
                    case PromptAction::quick_cd: return HelpTopic::panels;
                    default: return HelpTopic::dialogs;
                }
            case Mode::operation_dialog:
            case Mode::conflict_dialog:
            case Mode::operation_error_dialog: return HelpTopic::copy_move;
            case Mode::delete_dialog:
            case Mode::recursive_delete_dialog: return HelpTopic::delete_files;
            case Mode::operation_progress:
            case Mode::jobs: return HelpTopic::jobs;
            case Mode::search_results:
            case Mode::external_panelize_dialog: return HelpTopic::find_files;
            case Mode::directory_tree_dialog: return HelpTopic::tree;
            case Mode::bookmarks: return HelpTopic::hotlist;
            case Mode::chattr_dialog:
            case Mode::advanced_chown_dialog:
            case Mode::information: return HelpTopic::permissions;
            case Mode::listing_dialog:
            case Mode::sort_dialog: return HelpTopic::panels;
            case Mode::configure_dialog:
            case Mode::confirmation_dialog:
            case Mode::layout_dialog:
            case Mode::panel_options_dialog:
            case Mode::appearance_dialog:
            case Mode::display_bits_dialog:
            case Mode::editor_options_dialog: return HelpTopic::settings;
            case Mode::native_input: return help_topic_for_mode(native_input_return, depth + 1U);
            case Mode::native_message: return help_topic_for_mode(native_message_return, depth + 1U);
            case Mode::native_query: return help_topic_for_mode(native_query_return, depth + 1U);
            case Mode::local_list: return help_topic_for_mode(local_list_return, depth + 1U);
            case Mode::local_command: return help_topic_for_mode(local_command_return, depth + 1U);
            case Mode::user_menu:
                return user_menu_return == Mode::editor ? HelpTopic::editor : HelpTopic::command_line;
            case Mode::pulldown: return HelpTopic::command_line;
            case Mode::compare_dialog: return HelpTopic::panels;
            case Mode::learn_key:
            case Mode::learn_keys_dialog:
            case Mode::native_key_query: return HelpTopic::keymaps;
        }
        return HelpTopic::dialogs;
    }

    void open_context_help() {
        if (!help_key_capture() && !help_browser) {
            help_message.clear();
            help_browser.emplace(help_topic_for_mode(mode));
        }
    }

    bool handle_help_action(const std::string_view action) {
        if (!help_browser) return false;
        help_message.clear();
        if (action == "Quit" || action == "Cancel" || action == "Close" || action == "QuitQuiet") {
            help_browser.reset();
        } else if (action == "Help") help_browser->open(HelpTopic::browser);
        else if (action == "Index") help_browser->open(HelpTopic::index);
        else if (action == "Back" || action == "Left") static_cast<void>(help_browser->back());
        else if (action == "Enter" || action == "Right" || action == "Ok" || action == "Select") {
            static_cast<void>(help_browser->follow());
        } else if (action == "Up" || action == "ScrollUp") help_browser->scroll(-1);
        else if (action == "Down" || action == "ScrollDown") help_browser->scroll(1);
        else if (action == "PageUp") help_browser->page(-1);
        else if (action == "PageDown") help_browser->page(1);
        else if (action == "HalfPageUp" || action == "HalfPageDown") {
            help_browser->scroll(action == "HalfPageUp" ? -1 : 1,
                                 std::max<std::size_t>(1U, help_browser->page_rows() / 2U));
        } else if (action == "Top" || action == "Home") help_browser->top();
        else if (action == "Bottom" || action == "End") help_browser->bottom();
        else if (action == "LinkNext") help_browser->next_link();
        else if (action == "LinkPrev") help_browser->next_link(true);
        else return false;
        return true;
    }

    bool handle_help(const ftxui::Event& event) {
        if (event.is_mouse()) {
            auto copy = event;
            const auto& mouse = copy.mouse();
            if (mouse.button == ftxui::Mouse::WheelUp || mouse.button == ftxui::Mouse::WheelDown) {
                help_browser->scroll(mouse.button == ftxui::Mouse::WheelUp ? -1 : 1, 3U);
            } else if (mouse.button == ftxui::Mouse::Left && mouse.motion == ftxui::Mouse::Pressed) {
                static constexpr std::array<std::string_view, 4> actions{"Help", "Index", "Back", "Quit"};
                for (std::size_t index = 0; index < help_key_boxes.size(); ++index) {
                    if (help_key_boxes[index].Contain(mouse.x, mouse.y)) {
                        return handle_help_action(actions[index]);
                    }
                }
                for (std::size_t row = 0; row < help_row_boxes.size(); ++row) {
                    if (help_row_links[row] && help_row_boxes[row].Contain(mouse.x, mouse.y)) {
                        static_cast<void>(help_browser->follow_link(*help_row_links[row]));
                        return true;
                    }
                }
            }
            return true;
        }
        if (const auto key = keymap_event_name(event)) {
            for (const auto& binding : original_keymap_bindings()) {
                if (binding.context == KeymapContext::help && binding_has_key(binding.keys, *key)) {
                    return handle_help_action(binding.action);
                }
            }
        }
        return true;
    }

    ftxui::Element render_help() {
        using namespace ftxui;
        const auto dimensions = Terminal::Size();
        const auto width = std::max(5, std::min(88, dimensions.dimx - 4));
        const auto height = std::max(7, std::min(30, dimensions.dimy - 2));
        const auto rows = static_cast<std::size_t>(height - 6);
        help_browser->resize(static_cast<std::size_t>(width - 4), rows);
        help_row_boxes.assign(rows, {});
        help_row_links.assign(rows, std::nullopt);
        const auto lines = help_browser->lines();
        const auto first = help_browser->first_line();
        Elements body;
        for (std::size_t row = 0; row < rows; ++row) {
            auto line = text("");
            if (first + row < lines.size()) {
                const auto& source = lines[first + row];
                line = text(source.text);
                if (source.heading) line = std::move(line) | bold;
                if (source.link) {
                    help_row_links[row] = source.link;
                    const auto selected = source.link == help_browser->selected_link();
                    line = paint(std::move(line), skin_color(palette(), "help",
                        selected ? "helpslink" : "helplink", selected ? palette().selected : palette().normal));
                    line = std::move(line) | underlined;
                    if (selected) line = std::move(line) | bold | focus;
                }
            }
            body.push_back(hbox({text(" "), std::move(line) | xflex, text(" ")}) |
                           reflect(help_row_boxes[row]));
        }
        const auto last = std::min(lines.size(), first + rows);
        body.push_back(separator());
        body.push_back(text(help_message.empty() ?
            " Lines " + std::to_string(first + 1U) + "-" + std::to_string(last) +
            "/" + std::to_string(lines.size()) + "   " +
            (help_browser->can_go_back() ? "F3: previous page" : "Tab: links") :
            " " + display_text(help_message)) | xflex);
        body.push_back(separator());
        Elements keys;
        static const std::array<std::string, 4> labels{"1 Help", "2 Index", "3 Prev", "10 Quit"};
        for (std::size_t index = 0; index < labels.size(); ++index) {
            keys.push_back(paint(text(" " + labels[index] + " ") | center | xflex |
                                reflect(help_key_boxes[index]), palette().key_label));
        }
        body.push_back(hbox(std::move(keys)));
        return paint(window(text(" mcpp help: " + std::string(help_page(help_browser->topic()).title) + " ") | bold,
                            vbox(std::move(body))), palette().normal) |
               size(WIDTH, EQUAL, width) | size(HEIGHT, EQUAL, height);
    }

    ftxui::Element render_menu() {
        using namespace ftxui;
        Elements header{
            text("User menu") | bold | center,
            separator(),
        };
        Elements rows;
        user_menu_boxes.assign(user_menu.size(), {});
        int width = 50;
        for (std::size_t index = 0; index < user_menu.size(); ++index) {
            const auto& item = user_menu[index];
            const auto label = display_text(item.label);
            width = std::max(width, string_width(label) + 6);
            auto row = hbox({text(std::string(1, item.key)) | color(palette().menu_hot),
                             text("  " + label), filler()});
            if (index == user_menu_cursor) row = highlight(std::move(row), palette().popup_selected) | focus;
            rows.push_back(std::move(row) | reflect(user_menu_boxes[index]));
        }
        if (user_menu.empty()) {
            rows.push_back(text("<no menu entries>") | dim);
        }
        header.push_back(vbox(std::move(rows)) | yframe |
            size(HEIGHT, LESS_THAN, std::max(1, Terminal::Size().dimy - 9)) | reflect(user_menu_viewport));
        if (!user_menu_error.empty()) header.push_back(paragraph(display_text(user_menu_error)) | bold);
        header.push_back(separator());
        header.push_back(text("Enter selects   Esc closes menu"));
        return gray_menu(vbox(std::move(header)) | borderStyled(LIGHT) |
            size(WIDTH, EQUAL, std::min(width, std::max(1, Terminal::Size().dimx - 2))));
    }

    static const std::array<
        std::vector<MenuRow>,
        5
    >& pulldown_items() {
        static const std::array<std::vector<MenuRow>, 5> items{{
            {
                {"File listing", 12, 'g'},
                {"Quick view         C-x q", 10, 'q'},
                {"Info               C-x i", 11, 'i'},
                {"Tree", 14, 't'},
                {"", -2},
                {"Listing format...", 16, 'l'},
                {"Sort order...", 17, 's'},
                {"Filter...", 13, 'f'},
                {"Encoding...        M-e", 18, 'e'},
                {"", -2},
                {"FTP link...", -1},
                {"Shell link...", -1},
                {"SFTP link...", -1},
                {"Panelize", 15, 'z'},
                {"", -2},
                {"Rescan             C-r", 7, 'r'},
            },
            {
                {"View              F3", 0, 'v'},
                {"View file...", 18, 'w'},
                {"Filtered view     M-!", 22, 'f'},
                {"Edit              F4", 1, 'e'},
                {"Copy              F5", 2, 'c'},
                {"Chmod             C-x c", 8, 'h'},
                {"Link              C-x l", 11, 'l'},
                {"Symlink           C-x s", 12, 's'},
                {"Relative symlink  C-x v", 13, 'k'},
                {"Edit symlink      C-x C-s", 19, 'y'},
                {"Chown             C-x o", 9, 'o'},
                {"Advanced chown", 20, 'a'},
                {"Chattr            C-x e", 21, 't'},
                {"Rename/Move       F6", 4, 'r'},
                {"Mkdir             F7", 5, 'm'},
                {"Delete            F8", 6, 'd'},
                {"Quick cd          M-c", 14, 'q'},
                {"", -2},
                {"Select group      +", 15, 'g'},
                {"Unselect group    -", 16, 'n'},
                {"Invert selection  *", 17, 'i'},
                {"", -2},
                {"Exit              F10", 10, 'x'},
            },
            {
                {"User menu                     F2", 8, 'u'},
                {"Directory tree", 11, 'd'},
                {"Find file                     M-?", 0, 'f'},
                {"Swap panels                   C-u", 5, 'w'},
                {"Switch panels on/off          C-o", 3, 'p'},
                {"Compare directories           C-x d", 1, 'c'},
                {"Compare files                 C-x C-d", 12, 'o'},
                {"External panelize             C-x !", 13, 'x'},
                {"Show directory sizes          C-Space", 14, 'i'},
                {"", -2},
                {"Command history               M-h", 15, 'h'},
                {"Viewed/edited files history   M-E", 16, 's'},
                {"Directory hotlist             C-\\", 7, 'r'},
                {"Active VFS list               C-x a", 9},
                {"Background jobs               C-x j", 10, 'b'},
                {"Screen list                   M-`", 17, 't'},
                {"", -2},
                {"Undelete files (ext2fs only)", -1},
                {"", -2},
                {"Edit extension file", 18, 'e'},
                {"Edit menu file", 19, 'm'},
                {"Edit highlighting group file", 20, 'g'},
            },
            {
                {"Configuration...", 20, 'c'},
                {"Layout...", 21, 'l'},
                {"Panel options...", 22, 'p'},
                {"Confirmation...", 23, 'o'},
                {"Appearance...", 24, 'a'},
                {"Display bits...", 25, 'd'},
                {"Learn keys...", 26, 'k'},
                {"Virtual FS...", 27},
                {"", -2},
                {"Save setup", 3, 's'},
            },
            {
                {"File listing", 12, 'g'},
                {"Quick view         C-x q", 10, 'q'},
                {"Info               C-x i", 11, 'i'},
                {"Tree", 14, 't'},
                {"", -2},
                {"Listing format...", 16, 'l'},
                {"Sort order...", 17, 's'},
                {"Filter...", 13, 'f'},
                {"Encoding...        M-e", 18, 'e'},
                {"", -2},
                {"FTP link...", -1},
                {"Shell link...", -1},
                {"SFTP link...", -1},
                {"Panelize", 15, 'z'},
                {"", -2},
                {"Rescan             C-r", 7, 'r'},
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
            const bool selected = index == pulldown_item;
            auto row = menu_row_label(items[index], selected || items[index].action < 0
                ? std::nullopt : std::optional<ftxui::Color>{palette().menu_hot});
            if (items[index].action < 0) {
                row = row | color(palette().disabled) | dim;
            }
            if (selected && items[index].action >= 0) {
                row = highlight(std::move(row), palette().popup_selected) | focus;
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
                active().restore_panelized();
                mode = Mode::panels;
                status = "Panelized listing restored";
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
                    config.panel_listing[panel_config_index(active())] = ListingFormat::brief;
                    break;
                case 1:
                    config.panel_listing[panel_config_index(active())] = ListingFormat::full;
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
                    active().refresh({}, true);
                    break;
                case 8:
                    show_hotlist();
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
                case 16:
                    show_listing_options();
                    return;
                case 17:
                    show_sort_options();
                    return;
                case 18:
                    show_filename_encoding();
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
                        view_file_with_extensions(active().provider(), entry->path, Mode::panels);
                        return;
                    }
                    break;
                case 1:
                    request_main_edit(entry);
                    return;
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
                    start_prompt(PromptAction::mkdir, "New directory", mkdir_default());
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
                    request_app_exit();
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
                case 22: show_filtered_view(); return;
                case 19: show_edit_symlink(); return;
                case 20: show_advanced_chown(); return;
                case 21: show_file_attributes(); return;
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
                case 3: {
                    const auto result = run_process("", true);
                    status = shell.running() ? "Shell hidden; Ctrl-O resumes it"
                                             : "Subshell exited " + std::to_string(result);
                    break;
                }
                case 4:
                    refresh();
                    break;
                case 5:
                    swap_panels();
                    break;
                case 6:
                    install_pane_location(passive(), {active().provider(), active().path()});
                    break;
                case 7:
                    show_hotlist();
                    return;
                case 8:
                    open_user_menu(Mode::panels);
                    return;
                case 9: show_vfs_list(); return;
                case 10:
                    open_jobs();
                    return;
                case 11: show_directory_chooser(active().path()); return;
                case 12: show_file_comparison(); return;
                case 13: show_external_panelize(); return;
                case 14: show_directory_sizes(); return;
                case 15: show_command_history(); return;
                case 16: show_file_history(); return;
                case 17: show_screens(Mode::panels); return;
                case 18:
                    open_config_editor(find_extension_file(default_config_path().parent_path()),
                        "# mcpp local extension actions (mc.ext.ini 4.0 format).\n"
                        "# Filename/Type rules are searched in order for the requested action.\n"
                        "# Empty actions disable that action; commands use native-menu macros.\n"
                        "[mc.ext.ini]\nVersion=4.0\n\n"
                        "# [Text]\n# Shell=.txt\n# View=%view{ascii,unform}\n");
                    return;
                case 19:
                    open_config_editor(find_user_menu_path(
                        active().provider() == local ? std::filesystem::path(active().path())
                                                     : std::filesystem::path{}, false),
                        "# Key and title, followed by indented shell commands.\n"
                        "# Use %% for a literal percent; key|label|command is also accepted.\n"
                        "i Inspect selected path\n    stat -- %f\n"
                        "d Disk usage\n    du -sh -- %f\n");
                    return;
                case 20:
                    {
                        const auto path = find_file_highlight_file(default_config_path().parent_path());
                        open_config_editor(path, path.filename() == "highlighting"
                            ? "# pattern=red,green,blue\n# *.log=255,180,90\n"
                            : "# Groups use colors from the skin's [filehighlight] section.\n"
                              "# [notes]\n# extensions=log;txt\n# extensions_case=false\n");
                    }
                    return;
                default:
                    break;
            }
            mode = Mode::panels;
            return;
        }
        if (pulldown_category == 3U) {
            if (action == 27) { show_vfs_options(); return; }
            if (action == 26) {
                show_learn_keys(Mode::panels);
                return;
            }
            if (action >= 20 && action <= 25) {
                show_settings(action);
                return;
            }
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
                    config.save();
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

    std::filesystem::path hotlist_path() const {
        return default_config_path().parent_path() / "hotlist";
    }

    std::vector<std::optional<std::size_t>> hotlist_rows() const {
        std::vector<std::optional<std::size_t>> result;
        if (!hotlist_group.empty()) result.push_back({});
        const auto& items = hotlist.group(hotlist_group);
        for (std::size_t index = 0; index < items.size(); ++index) {
            if (items[index].kind != HotlistKind::comment) result.push_back(index);
        }
        return result;
    }

    std::optional<HotlistPath> selected_hotlist_item() const {
        const auto rows = hotlist_rows();
        if (bookmark_cursor >= rows.size() || !rows[bookmark_cursor]) return {};
        auto path = hotlist_group;
        path.push_back(*rows[bookmark_cursor]);
        return path;
    }

    void select_hotlist_item(const std::size_t index) {
        const auto rows = hotlist_rows();
        const auto found = std::ranges::find(rows, std::optional<std::size_t>(index));
        bookmark_cursor = found == rows.end() ? 0U : static_cast<std::size_t>(found - rows.begin());
        hotlist_focus = 0;
    }

    void publish_hotlist(DirectoryHotlist candidate) {
        auto bookmarks = candidate.bookmarks();
        candidate.save(hotlist_path());
        hotlist = std::move(candidate);
        config.bookmarks = std::move(bookmarks);
        hotlist_error.clear();
    }

    void add_current_hotlist(const Mode return_mode) {
        const auto directory = pane_location(active());
        const auto group = hotlist_group;
        show_native_input("Add to hotlist", "Label for \"" + display_text(directory) + "\":",
            directory, [this, directory, group](const std::string& label) {
                if (label.empty()) throw std::invalid_argument("directory label is empty");
                auto candidate = hotlist;
                const auto index = candidate.insert(group, {HotlistKind::entry, label, directory, {}});
                publish_hotlist(std::move(candidate));
                hotlist_group = group;
                select_hotlist_item(index);
                status = "Current directory added to hotlist";
            }, return_mode);
    }

    void show_hotlist(const bool add_current = false) {
        try {
            auto loaded = DirectoryHotlist::load(hotlist_path(), config.bookmarks);
            hotlist = std::move(loaded);
            hotlist_group.clear();
            hotlist_move_source.reset();
            hotlist_form_open = false;
            hotlist_focus = 0;
            bookmark_cursor = 0;
            hotlist_error.clear();
            mode = Mode::bookmarks;
            if (add_current) add_current_hotlist(Mode::panels);
        } catch (const std::exception& error) {
            show_native_message("Directory hotlist",
                std::string(error.what()) + "\nThe hotlist was left unchanged: " + hotlist_path().string());
        }
    }

    void hotlist_up() {
        if (hotlist_group.empty()) return;
        const auto index = hotlist_group.back();
        hotlist_group.pop_back();
        select_hotlist_item(index);
        hotlist_error.clear();
    }

    void change_hotlist_directory() {
        const auto selected = selected_hotlist_item();
        if (!selected) {
            hotlist_up();
            return;
        }
        const auto& item = hotlist.group(hotlist_group)[selected->back()];
        if (item.kind == HotlistKind::group) {
            if (hotlist_move_source && selected->size() >= hotlist_move_source->size() &&
                std::equal(hotlist_move_source->begin(), hotlist_move_source->end(), selected->begin())) {
                hotlist_error = "Cannot move a group into itself.";
                return;
            }
            hotlist_group = *selected;
            bookmark_cursor = 0;
            hotlist_focus = 0;
            hotlist_error.clear();
        } else if (!hotlist_move_source) {
            try {
                change_panel_directory(active(), item.directory);
                mode = Mode::panels;
                status = "Directory changed: " + pane_location(active());
            } catch (const std::exception& error) {
                hotlist_error = error.what();
            }
        }
    }

    void start_hotlist_form(const bool group) {
        hotlist_form_group = group;
        hotlist_form_open = true;
        hotlist_label = group ? "" : pane_location(active());
        hotlist_directory = group ? "" : pane_location(active());
        hotlist_label_input.reset(hotlist_label, true);
        hotlist_directory_input.reset(hotlist_directory, true);
        hotlist_form_focus = 0;
        hotlist_form_error.clear();
    }

    void insert_hotlist_form(const bool append) {
        if (hotlist_label.empty()) throw std::invalid_argument(
            hotlist_form_group ? "group name is empty" : "directory label is empty");
        HotlistItem item{hotlist_form_group ? HotlistKind::group : HotlistKind::entry, hotlist_label, {}, {}};
        if (!hotlist_form_group) {
            if (hotlist_directory.empty()) throw std::invalid_argument("directory path is empty");
            const auto location = resolve_pane_location(active(), hotlist_directory);
            item.directory = archives.format(location.provider, location.path);
        }
        std::optional<std::size_t> before;
        if (!append) {
            const auto selected = selected_hotlist_item();
            before = selected ? selected->back() : 0U;
        }
        auto candidate = hotlist;
        const auto index = candidate.insert(hotlist_group, std::move(item), before);
        publish_hotlist(std::move(candidate));
        select_hotlist_item(index);
        hotlist_form_open = false;
        status = hotlist_form_group ? "Hotlist group added" : "Hotlist entry added";
    }

    void remove_hotlist_item(const HotlistPath& path) {
        try {
            auto candidate = hotlist;
            candidate.erase(path);
            publish_hotlist(std::move(candidate));
            const auto count = hotlist_rows().size();
            bookmark_cursor = count == 0U ? 0U : std::min(bookmark_cursor, count - 1U);
            hotlist_focus = 0;
            status = "Hotlist entry removed";
        } catch (const std::exception& error) {
            hotlist_error = error.what();
        }
    }

    void confirm_hotlist_remove() {
        const auto selected = selected_hotlist_item();
        if (!selected) return;
        const auto& item = hotlist.group(hotlist_group)[selected->back()];
        const bool nonempty_group = item.kind == HotlistKind::group && !item.children.empty();
        if (config.confirm_hotlist_delete || nonempty_group) {
            show_native_query("Directory hotlist delete",
                nonempty_group ? "Delete this group and all its entries?" : "Delete this entry?",
                {"Yes", "No"}, [this, path = *selected](const std::size_t choice) {
                    if (choice == 0U) remove_hotlist_item(path);
                }, Mode::bookmarks);
            if (config.safe_delete) native_query_choice = 1;
        } else {
            remove_hotlist_item(*selected);
        }
    }

    void cancel_hotlist_move() {
        hotlist_group = hotlist_move_origin;
        bookmark_cursor = hotlist_move_cursor;
        hotlist_move_source.reset();
        hotlist_focus = 0;
        hotlist_error.clear();
    }

    void finish_hotlist_move(const bool append) {
        try {
            std::optional<std::size_t> before;
            if (!append) {
                const auto selected = selected_hotlist_item();
                before = selected ? selected->back() : 0U;
            }
            auto candidate = hotlist;
            auto destination = candidate.move(*hotlist_move_source, hotlist_group, before);
            publish_hotlist(std::move(candidate));
            const auto index = destination.back();
            destination.pop_back();
            hotlist_group = std::move(destination);
            hotlist_move_source.reset();
            select_hotlist_item(index);
            status = "Hotlist entry moved";
        } catch (const std::exception& error) {
            hotlist_error = error.what();
        }
    }

    void activate_hotlist_button(const std::size_t button) {
        if (hotlist_move_source) {
            switch (button) {
                case 0: change_hotlist_directory(); break;
                case 1: hotlist_up(); break;
                case 2: cancel_hotlist_move(); break;
                case 3: finish_hotlist_move(false); break;
                case 4: finish_hotlist_move(true); break;
                default: break;
            }
            return;
        }
        switch (button) {
            case 0: change_hotlist_directory(); break;
            case 1: add_current_hotlist(Mode::bookmarks); break;
            case 2: hotlist_up(); break;
            case 3: mode = Mode::panels; break;
            case 4: start_hotlist_form(true); break;
            case 5: start_hotlist_form(false); break;
            case 6: confirm_hotlist_remove(); break;
            case 7:
                if (const auto selected = selected_hotlist_item()) {
                    hotlist_move_source = *selected;
                    hotlist_move_origin = hotlist_group;
                    hotlist_move_cursor = bookmark_cursor;
                    hotlist_focus = 0;
                    hotlist_error.clear();
                }
                break;
            default: break;
        }
    }

    ftxui::Element render_hotlist_form() {
        using namespace ftxui;
        Elements fields{
            text(hotlist_form_group ? " Name of new group:" : " Directory label:"),
            hbox({text(" "), render_input(hotlist_label, hotlist_label_input, hotlist_form_focus == 0U, "") |
                                reflect(hotlist_form_boxes[0]), text(" ")})
        };
        hotlist_form_boxes[1] = {};
        if (!hotlist_form_group) {
            fields.push_back(text(" "));
            fields.push_back(text(" Directory path:"));
            fields.push_back(hbox({text(" "),
                render_input(hotlist_directory, hotlist_directory_input, hotlist_form_focus == 1U, "") |
                    reflect(hotlist_form_boxes[1]), text(" ")}));
        }
        if (!hotlist_form_error.empty()) fields.push_back(paragraph(display_text(hotlist_form_error)));
        return native_window(hotlist_form_group ? "New hotlist group" : "New hotlist entry", vbox({
            vbox(std::move(fields)), separator(),
            hbox({native_button("Append", false, hotlist_form_focus == 2U, hotlist_form_boxes[2]), text(" "),
                  native_button("Insert", false, hotlist_form_focus == 3U, hotlist_form_boxes[3]), text(" "),
                  native_button("Cancel", false, hotlist_form_focus == 4U, hotlist_form_boxes[4])}) | center,
        }), std::min(62, std::max(1, Terminal::Size().dimx - 12)));
    }

    ftxui::Element render_bookmarks() {
        using namespace ftxui;
        const bool moving = hotlist_move_source.has_value();
        const auto width = std::max(1, Terminal::Size().dimx - 10);
        const auto visible = hotlist_rows();
        const auto& items = hotlist.group(hotlist_group);
        bookmark_boxes.assign(visible.size(), {});
        Elements rows;
        std::string directory;
        for (std::size_t index = 0; index < visible.size(); ++index) {
            const auto* item = visible[index] ? &items[*visible[index]] : nullptr;
            const auto label = item == nullptr ? ".." :
                (item->kind == HotlistKind::group ? "->" : "") + item->label;
            auto row = text(" " + path_label(display_text(label), static_cast<std::size_t>(std::max(1, width - 8))));
            if (index == bookmark_cursor) {
                row = focus_dialog(std::move(row));
                directory = item == nullptr ? ".." : item->kind == HotlistKind::group
                    ? "Subgroup - press ENTER to see list" : item->directory;
            }
            rows.push_back(std::move(row) | reflect(bookmark_boxes[index]));
        }
        if (rows.empty()) rows.push_back(text(" "));
        auto group_name = std::string("Top level group");
        if (!hotlist_group.empty()) {
            HotlistPath parent(hotlist_group.begin(), hotlist_group.end() - 1);
            group_name = hotlist.group(parent)[hotlist_group.back()].label;
        }
        const auto list_height = std::max(2, Terminal::Size().dimy - (moving ? 13 : 16) -
                                            (hotlist_error.empty() ? 0 : 2));
        Elements body{
            hbox({text(" "), window(text(" " + display_text(group_name) + " "),
                vbox(std::move(rows)) | yframe | reflect(bookmark_list_box) |
                    size(HEIGHT, EQUAL, list_height)) | flex, text(" ")})
        };
        if (!moving) {
            body.push_back(hbox({text(" "), window(text(" Directory path "),
                text(" " + path_label(display_text(directory), static_cast<std::size_t>(std::max(1, width - 8))))) |
                    flex, text(" ")}));
        }
        if (!hotlist_error.empty()) body.push_back(paragraph(display_text(hotlist_error)));
        body.push_back(separator());
        hotlist_buttons.fill({});
        const auto button = [&](const std::string& label, const std::size_t index) {
            return native_button(label, index == 0U, hotlist_focus == index + 1U, hotlist_buttons[index]);
        };
        if (moving) {
            body.push_back(hbox({text(" "), button("Change to", 0), text(" "), button("Up", 1),
                                  filler(), button("Cancel", 2), text(" ")}));
            body.push_back(hbox({text(" "), button("Insert", 3), text(" "), button("Append", 4), filler()}));
        } else {
            body.push_back(hbox({text(" "), button("Change to", 0), text(" "), button("Add current", 1),
                                  text(" "), button("Up", 2), filler(), button("Cancel", 3), text(" ")}));
            body.push_back(hbox({text(" "), button("New group", 4), text(" "), button("New entry", 5),
                                  text(" "), button("Remove", 6), text(" "), button("Move", 7), filler()}));
        }
        std::string title = "Directory hotlist";
        if (moving) {
            HotlistPath parent(hotlist_move_source->begin(), hotlist_move_source->end() - 1);
            title = "Moving " + display_text(hotlist.group(parent)[hotlist_move_source->back()].label);
        }
        auto dialog = native_window(title, vbox(std::move(body)), width);
        if (hotlist_form_open) {
            dialog = dbox({std::move(dialog), render_hotlist_form() | center | clear_under});
        }
        return dialog;
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
                element = focus_dialog(std::move(element));
            }
            return element | reflect(operation_dialog_boxes[index]);
        };
        Elements rows{
            text("Source mask:"),
            render_input(
                dialog_source_mask, mask_input, dialog_focus == 0U
            ) | reflect(operation_dialog_boxes[0]),
            focused(
                text(std::string(dialog_shell_patterns ? "[x] " : "[ ] ") +
                     "Using shell patterns"), 1
            ) | align_right,
            text("to:"),
            render_input(
                dialog_destination, destination_input, dialog_focus == 2U
            ) | reflect(operation_dialog_boxes[2]),
            separator(),
            hbox({
                vbox({
                    focused(text(std::string(dialog_preserve_symlinks ? "[ ] " : "[x] ") +
                                 "Follow links"), 3),
                    focused(text(std::string(dialog_preserve_attributes ? "[x] " : "[ ] ") +
                                 "Preserve attributes"), 4),
                }) | flex,
                vbox({
                    focused(text(std::string(dialog_dive_into_subdirectories ? "[x] " : "[ ] ") +
                                 "Dive into subdir if exists"), 5),
                    focused(text(std::string(dialog_stable_symlinks ? "[x] " : "[ ] ") +
                                 "Stable symlinks"), 6),
                }) | flex,
            }),
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
            operation_input_error.empty() ? emptyElement() : paragraph(display_text(operation_input_error)),
            separator(),
            hbox({
                focused(text(" [ OK ] "), 7),
                text(" "),
                focused(text(" [ Background ] "), 8),
                text(" "),
                focused(text(" [ Cancel ] "), 9),
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
                element = focus_dialog(std::move(element));
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
        const auto job = progress_job;
        if (!job) return text("No file-operation job selected");
        const auto snapshot = job->control.snapshot();
        const auto& current = snapshot.progress;
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
            std::chrono::steady_clock::now() - job->started
        ).count();
        const auto bytes_per_second =
            elapsed > 0.0
                ? static_cast<std::uint64_t>(
                      static_cast<double>(current.bytes_done) /
                      elapsed
                  )
                : 0U;
        const auto progress_bar = [&](const float ratio) {
            return !config.classic_progressbar && job->source_side == 1
                       ? gaugeLeft(std::clamp(ratio, 0.0F, 1.0F))
                       : gauge(std::clamp(ratio, 0.0F, 1.0F));
        };
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
        Elements buttons;
        const std::array<std::string, 4> labels{
            snapshot.paused ? "Continue" : "Pause", "Skip", "Abort",
            job->finished ? "Close" : "Background"
        };
        for (std::size_t index = 0; index < labels.size(); ++index) {
            auto button = text(" [ " + labels[index] + " ] ") |
                reflect(operation_progress_boxes[index]);
            if ((job->finished && index != 3U) ||
                (index == 1U && job->kind == OperationJob::Kind::remove)) {
                button = std::move(button) | dim;
            } else if (operation_progress_focus == index) {
                button = focus_dialog(std::move(button));
            }
            buttons.push_back(std::move(button));
        }
        return vbox({
                   text(job->label + " progress") | bold | center,
                   separator(),
                   text("Job " + std::to_string(job->id) + ": " +
                        (job->finished ? job->status : snapshot.paused ? "Paused" :
                         snapshot.waiting ? "Waiting for decision" : "Running")),
                   text("To: " + display_text(job->destination)),
                   text(display_text(current.current_path)),
                   text(
                       "File bytes: " +
                       format_size(current.bytes_done) + " / " +
                       format_size(current.bytes_total)
                   ),
                   paint(progress_bar(byte_ratio), palette().progress),
                   text(
                       "Files: " +
                       std::to_string(current.files_done) + " / " +
                       std::to_string(current.files_total)
                   ),
                   paint(progress_bar(file_ratio), palette().progress),
                   text(
                       "Speed: " +
                       format_size(bytes_per_second) +
                       "/s   ETA: " + eta
                   ),
                   separator(),
                   hbox(std::move(buttons)) | center,
               }) |
               borderStyled(LIGHT) | size(WIDTH, EQUAL, 72);
    }

    std::vector<OperationChoiceButton> operation_choice_buttons() const {
        if (mode == Mode::recursive_delete_dialog) {
            return {
                {OperationChoice::overwrite, "Yes", 'y'},
                {OperationChoice::skip, "No", 'n'},
                {OperationChoice::overwrite_all, "All", 'l'},
                {OperationChoice::skip_all, "None", 'e'},
                {OperationChoice::cancel, "Abort", 'a'},
            };
        }
        if (mode == Mode::operation_error_dialog) {
            return {
                {OperationChoice::retry, "Retry", 'r'},
                {OperationChoice::skip, "Skip", 's'},
                {OperationChoice::ignore_all, "Ignore all", 'i'},
                {OperationChoice::cancel, "Abort", 'a'},
            };
        }
        std::vector<OperationChoiceButton> buttons{
            {OperationChoice::overwrite, "Yes", 'y'},
            {OperationChoice::skip, "No", 'n'},
        };
        if (operation_prompt_collision_options.append) {
            buttons.push_back({OperationChoice::append, "Append", 'p'});
        }
        if (operation_prompt_collision_options.reget) {
            buttons.push_back({OperationChoice::reget, "Reget", 'r'});
        }
        buttons.insert(buttons.end(), {
            {OperationChoice::overwrite_all, "All", 'a'},
            {OperationChoice::older, "Older", 'o'},
            {OperationChoice::skip_all, "None", 's'},
            {OperationChoice::cancel, "Abort", 'c'},
        });
        return buttons;
    }

    ftxui::Element render_conflict_dialog() {
        using namespace ftxui;
        if (mode == Mode::recursive_delete_dialog) {
            const auto width = std::min(76, Terminal::Size().dimx);
            const auto choices = operation_choice_buttons();
            Elements buttons;
            for (std::size_t index = 0; index < choices.size(); ++index) {
                auto button = text(" [ " + std::string(choices[index].label) + " ] ");
                if (dialog_focus == index) button = focus_dialog(std::move(button));
                buttons.push_back(std::move(button) | reflect(operation_choice_boxes[index]));
            }
            return vbox({
                text("Delete") | bold | center,
                separator(),
                text(prompt_job ? "Job " + std::to_string(prompt_job->id) : ""),
                text("Directory \"" + path_label(
                    operation_prompt_source ? display_text(operation_prompt_source->path) : "",
                    static_cast<std::size_t>(std::max(1, width - 25))
                ) + "\" not empty."),
                text("Delete it recursively?") | center,
                separator(),
                hbox(std::move(buttons)) | center,
            }) | borderStyled(LIGHT) | size(WIDTH, EQUAL, width);
        }
        const bool error = mode == Mode::operation_error_dialog;
        const auto choices = operation_choice_buttons();
        Elements rows{
            text(error ? "File operation error" : "Destination exists") | bold | center,
            separator(),
            text(prompt_job ? "Job " + std::to_string(prompt_job->id) + ": " + prompt_job->label : ""),
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
        if (!error) rows.push_back(text("Overwrite this file?") | center);
        Elements buttons;
        for (std::size_t index = 0; index < choices.size(); ++index) {
            auto button = text(" [ " + std::string(choices[index].label) + " ] ");
            if (dialog_focus == index) {
                button = focus_dialog(std::move(button));
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
            const auto individual = 2U +
                static_cast<std::size_t>(operation_prompt_collision_options.append) +
                static_cast<std::size_t>(operation_prompt_collision_options.reget);
            for (std::size_t index = 0; index + 1U < buttons.size(); ++index) {
                (index < individual ? first : second).push_back(std::move(buttons[index]));
            }
            rows.push_back(hbox(std::move(first)) | center);
            rows.push_back(separator());
            rows.push_back(text("Overwrite all files?") | center);
            rows.push_back(hbox(std::move(second)) | center);
            rows.push_back(separator());
            rows.push_back(std::move(buttons.back()) | center);
            auto shortcuts = std::string("Y yes  N no");
            if (operation_prompt_collision_options.append) shortcuts += "  P append";
            if (operation_prompt_collision_options.reget) shortcuts += "  R reget";
            shortcuts += "  A all  S none  O older  Esc abort";
            rows.push_back(text(shortcuts) | center);
        }
        return vbox(std::move(rows)) | borderStyled(LIGHT) |
               size(WIDTH, LESS_THAN, std::min(90, Terminal::Size().dimx));
    }

    void open_jobs() {
        const auto selected = std::ranges::find(jobs, progress_job);
        if (selected != jobs.end()) {
            job_cursor = static_cast<std::size_t>(std::distance(jobs.begin(), selected));
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
            const auto& job = jobs[index];
            const auto snapshot = job->control.snapshot();
            auto state = job->status;
            if (!job->finished) {
                state = snapshot.paused ? "Paused" :
                    snapshot.waiting ? "Waiting for decision" :
                    job->control.cancellation().requested() ? "Cancelling" : "Running";
                if (snapshot.progress.files_total != 0U) {
                    state += "  " + std::to_string(snapshot.progress.files_done) +
                        "/" + std::to_string(snapshot.progress.files_total);
                }
                if (snapshot.progress.bytes_total != 0U) {
                    state += "  " + format_size(snapshot.progress.bytes_done) +
                        "/" + format_size(snapshot.progress.bytes_total);
                }
            }
            auto row = text(
                std::to_string(job->id) + "  " +
                job->label + "  " +
                middle_truncate(
                    display_text(state),
                    std::max(1, std::min(68, Terminal::Size().dimx - 12))
                )
            );
            if (job_cursor == index) {
                row = focus_dialog(std::move(row));
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
                                 display_text(jobs[job_cursor]->destination),
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
            auto row = text(middle_truncate(
                display_text(search_results[index].path),
                static_cast<std::size_t>(std::max(1, Terminal::Size().dimx - 4))
            ));
            if (index == search_cursor) {
                row = focus_dialog(std::move(row));
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
                   native_button("Panelize", false, false, search_panelize_box) | center,
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
        const auto lines = quick_viewer.lines(content_width, content_height);
        for (std::size_t index = 0; index < lines.size(); ++index) {
            rows.push_back(render_viewer_text(
                lines[index], quick_viewer.text_highlight(index), quick_viewer.text_styles(index)
            ));
        }
        auto title =
            text("Quick view: " + path_label(
                display_text(entry->name),
                static_cast<std::size_t>(std::max(1, width - 16))
            )) |
            bold;
        if (quick_view_focused) {
            title = title | color(palette().bright);
        } else {
            title = title | color(palette().inactive_title);
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
                    color(palette().dim),
            })
        );
    }

    ftxui::Element render_information_panel(const int width) {
        using namespace ftxui;
        const auto* entry = active().current();
        Elements rows;
        rows.push_back(
            text("Location: " + display_text(pane_location(active())))
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
                    ? title | color(palette().bright) | bold
                    : title | color(palette().inactive_title);
        return window(
                   std::move(title),
                   vbox(std::move(rows)),
                   LIGHT
               ) |
               size(WIDTH, EQUAL, width) |
               yflex |
               reflect(information_panel_box);
    }

    ftxui::Element render_tree_panel(const int width, const bool modal = false) {
        using namespace ftxui;
        const auto& nodes = !modal && modal_tree_snapshot ? modal_tree_snapshot->nodes : tree_nodes;
        const auto cursor = !modal && modal_tree_snapshot ? modal_tree_snapshot->cursor : tree_cursor;
        auto& boxes = modal ? modal_tree_rows : tree_row_boxes;
        auto& paths = modal ? modal_tree_paths : tree_row_paths;
        Elements rows;
        boxes.assign(nodes.size(), {});
        paths.assign(nodes.size(), {});
        const auto visible_rows = modal ? static_cast<std::size_t>(std::max(1, Terminal::Size().dimy - 15))
                                        : panel_visible_rows(tree_panel ? *tree_panel : active_index);
        const auto first =
            nodes.size() <= visible_rows
                ? 0U
                : std::min(
                      nodes.size() - visible_rows,
                      cursor > visible_rows / 2U
                          ? cursor - visible_rows / 2U
                          : 0U
                  );
        const auto end = std::min(
            nodes.size(),
            first + visible_rows
        );
        const auto is_last_sibling = [&](const std::size_t index) {
            const auto depth = nodes[index].depth;
            for (auto next = index + 1U;
                 next < nodes.size();
                 ++next) {
                if (nodes[next].depth < depth) {
                    break;
                }
                if (nodes[next].depth == depth) {
                    return false;
                }
            }
            return true;
        };
        for (std::size_t index = first;
             index < end;
             ++index) {
            const auto& node = nodes[index];
            paths[index] = node.path;
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
                    if (nodes[ancestor].depth == depth) {
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
            if (index == cursor) {
                row = row | focus;
                row = modal || tree_panel_focused
                          ? row | inverted
                          : row | color(palette().inactive_tree);
            }
            rows.push_back(
                std::move(row) | reflect(boxes[index])
            );
        }
        if (rows.empty()) {
            rows.push_back(text("<empty tree>") | dim);
        }
        while (rows.size() < visible_rows) {
            rows.push_back(text(""));
        }
        const auto selected_path = cursor < nodes.size() ? display_text(nodes[cursor].path) : std::string{};
        if (modal) {
            return native_window("Directory tree", vbox({
                vbox(std::move(rows)) | size(HEIGHT, EQUAL, static_cast<int>(visible_rows)) | reflect(modal_tree_viewport),
                separator(),
                text(tree_search_mode ? "Search: " + display_text(tree_search)
                                      : middle_truncate(selected_path, static_cast<std::size_t>(std::max(1, width - 2))))
            }), width);
        }
        auto title_text = text(" Directory tree ");
        title_text = tree_panel_focused
                         ? title_text |
                               color(palette().bright) |
                               bold
                         : title_text | color(palette().inactive_title);
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
                           cursor < nodes.size()
                               ? middle_truncate(
                                     selected_path,
                                     static_cast<std::size_t>(
                                         std::max(1, width - 2)
                                     )
                                 )
                               : std::string{}
                       ) |
                           color(palette().dim),
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
            mode == Mode::conflict_dialog || mode == Mode::operation_error_dialog ||
                mode == Mode::recursive_delete_dialog
                ? operation_return_mode
                : mode == Mode::file_search_dialog ? file_search_return()
                : mode == Mode::local_list ? local_list_return
                : mode == Mode::local_command
                    ? (local_command_return == Mode::user_menu ? user_menu_return : local_command_return)
                : mode == Mode::learn_key ? learn_key_return
                : mode == Mode::learn_keys_dialog ? learn_key_return
                : mode == Mode::native_input
                    ? (native_input_return == Mode::user_menu ? user_menu_return : native_input_return)
                : mode == Mode::native_key_query ? key_query_return
                : mode == Mode::native_message
                    ? (native_message_return == Mode::learn_keys_dialog ? learn_key_return :
                       native_message_return == Mode::user_menu ? user_menu_return : native_message_return)
                : mode == Mode::native_query
                    ? (native_query_return == Mode::user_menu ? user_menu_return : native_query_return)
                : mode == Mode::user_menu ? user_menu_return : mode;
        const bool diff_context = display_mode == Mode::diff || display_mode == Mode::diff_options ||
            (diff_saving && editor_save_buffer != nullptr &&
             (display_mode == Mode::editor_link_dialog || display_mode == Mode::editor_newline_warning));
        const bool editor_context =
            !diff_context && (display_mode == Mode::editor ||
            display_mode == Mode::editor_menu ||
            display_mode == Mode::editor_error ||
            display_mode == Mode::editor_search_dialog ||
            display_mode == Mode::editor_replace_dialog ||
            display_mode == Mode::editor_replace_confirm ||
            display_mode == Mode::editor_save_dialog ||
            display_mode == Mode::editor_save_mode_dialog ||
            display_mode == Mode::editor_newline_warning ||
            display_mode == Mode::editor_close_dialog ||
            display_mode == Mode::editor_link_dialog ||
            display_mode == Mode::editor_overwrite_dialog ||
            display_mode == Mode::editor_options_dialog ||
            display_mode == Mode::editor_mail_dialog ||
            display_mode == Mode::editor_spelling_dialog ||
            display_mode == Mode::editor_history ||
            display_mode == Mode::editor_buffer_list ||
            (
                display_mode == Mode::prompt &&
                mode_before_prompt == Mode::editor
            ));
        const bool viewer_context =
            display_mode == Mode::viewer ||
            display_mode == Mode::viewer_message ||
            display_mode == Mode::viewer_close_dialog ||
            display_mode == Mode::viewer_link_dialog ||
            (display_mode == Mode::viewer_wrap_dialog && !viewer_search_quick) ||
            (
                display_mode == Mode::prompt &&
                mode_before_prompt == Mode::viewer
            );
        palette_context = editor_context ? PaletteContext::editor
            : viewer_context ? PaletteContext::viewer : diff_context ? PaletteContext::diff : PaletteContext::panels;
        Element body;
        const auto left_width = panel_width(0);
        const auto right_width = panel_width(1);
        if (editor_context || viewer_context || diff_context) {
            body = emptyElement();
        } else if (tree_panel) {
            if (*tree_panel == 1) {
                right_entry_boxes.clear();
                body = render_panel_pair({
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
                body = render_panel_pair({
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
                body = render_panel_pair({
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
                body = render_panel_pair({
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
                body = render_panel_pair({
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
                body = render_panel_pair({
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
            body = render_panel_pair({
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
        TransferProgress combined_progress;
        for (const auto& job : jobs) {
            if (job->finished) continue;
            const auto current = job->control.snapshot().progress;
            combined_progress.files_done += current.files_done;
            combined_progress.files_total += current.files_total;
        }
        if (combined_progress.files_total != 0U) {
            progress_text = " [" + std::to_string(combined_progress.files_done) +
                "/" + std::to_string(combined_progress.files_total) + "]";
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
        const auto prompt = display_text(
            std::string(user == nullptr ? "user" : user) + "@" +
            hostname.data() + ":" + pane_execution_directory(active()) + "$"
        );
        const auto hint =
            pending_operation_notice
                ? "Background [Esc]: " + *pending_operation_notice
                : status == "Ready"
                      ? tree_panel && tree_panel_focused
                            ? "Tree: Enter open/expand  Space toggle  "
                              "+/- branch  Tab panel"
                            : "Hint: Tab changes your current panel."
                      : status + progress_text;
        Elements menu_elements;
        for (std::size_t index = 0; index < top_menu_names.size(); ++index) {
            const bool selected = mode == Mode::pulldown && index == pulldown_category;
            auto item = mnemonic_label(
                top_menu_names[index],
                top_menu_mnemonics[index],
                selected ? std::nullopt : std::optional<ftxui::Color>{palette().menu_hot}
            );
            if (selected) {
                item = paint(std::move(item), palette().menu_selected) | bold;
            }
            menu_elements.push_back(
                std::move(item) | reflect(top_menu_boxes[index])
            );
        }
        menu_elements.push_back(filler());
        if (config.rotating_dash && has_running_jobs()) {
            const auto running = std::ranges::find_if(jobs, [](const auto& job) { return !job->finished; });
            const auto ticks = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - (*running)->started).count() / 125;
            menu_elements.push_back(text(std::string(1, "-\\|/"[static_cast<std::size_t>(ticks) % 4U])));
        } else menu_elements.push_back(text(" "));
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
                key("1", "Help"),
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
                key("5", "Goto", quick_viewer.open()),
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
                key("8", quick_viewer.magic() ? "Raw" : "Parse", quick_viewer.open()),
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
        if (modal_tree_snapshot) {
            const std::array<std::string, 10> labels{
                "Help", "Rescan", "Forget", tree_dynamic ? "Static" : "Dynamc", "Copy", "RenMov", "", "Rmdir", "", "Quit"
            };
            Elements tree_keys{text(" ")};
            for (std::size_t index = 0; index < labels.size(); ++index) {
                const bool source = tree_cursor > 0U && tree_cursor < tree_nodes.size();
                const bool enabled = !labels[index].empty() && ((index != 4U && index != 5U && index != 7U) || source);
                tree_keys.push_back(render_function_key(index, labels[index], enabled && mode == Mode::directory_tree_dialog));
            }
            keybar = hbox(std::move(tree_keys));
        }
        const auto main_menubar = gray_menu(hbox(std::move(menu_elements)));
        Elements screen_rows;
        if (config.menubar_visible) screen_rows.push_back(main_menubar);
        screen_rows.push_back(body | flex);
        if (config.hintbar_visible) {
            screen_rows.push_back(paint(text(display_text(hint)), palette().hint));
        }
        if (command_prompt_shown()) {
            screen_rows.push_back(render_input(
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
                  ));
        }
        if (config.keybar_visible || modal_tree_snapshot) screen_rows.push_back(std::move(keybar));
        auto document = vbox(std::move(screen_rows));
        if (mode == Mode::pulldown && !config.menubar_visible) {
            document = dbox({document, vbox({main_menubar, filler()})});
        }
        if (config.xterm_title) {
            const auto title = "mcpp - " + display_text(pane_location(active()));
            if (title != terminal_title) {
                if (!terminal_title_pushed) {
                    std::cout << "\033[22;0t";
                    terminal_title_pushed = true;
                }
                terminal_title = title;
                std::cout << "\033]0;" << title << '\a' << std::flush;
            }
        }
        if (viewer_context) {
            document = render_viewer();
        } else if (editor_context) {
            document = render_editor();
        } else if (diff_context) document = render_diff();
        const auto centered_modal = [this](Element element) {
            auto dialog = with_dialog_shadow(gray_dialog(std::move(element)) | clear_under);
            return vbox({
                filler(),
                hbox({
                    filler(),
                    std::move(dialog),
                    filler(),
                }),
                filler(),
            });
        };
        const auto native_modal = [this](Element element) {
            element->ComputeRequirement();
            const auto height = element->requirement().min_y;
            auto top = std::max(0, (Terminal::Size().dimy - height) / 2);
            top -= top > 3 ? 2 : top == 3 ? 1 : 0;
            return vbox({
                filler() | size(HEIGHT, EQUAL, top),
                hbox({filler(), with_dialog_shadow(gray_dialog(std::move(element)) | clear_under), filler()}),
                filler(),
            });
        };
        if (modal_tree_snapshot) {
            document = dbox({document, centered_modal(render_tree_panel(std::max(1, Terminal::Size().dimx - 22), true))});
        }
        if (mode == Mode::listing_dialog) {
            document = dbox({document, native_modal(render_listing_dialog())});
        } else if (mode == Mode::sort_dialog) {
            document = dbox({document, native_modal(render_sort_dialog())});
        } else if (mode == Mode::confirmation_dialog) {
            document = dbox({document, native_modal(render_confirmation_dialog())});
        } else if (mode == Mode::layout_dialog) {
            document = dbox({document, centered_modal(render_layout_dialog())});
        } else if (mode == Mode::panel_options_dialog) {
            document = dbox({document, native_modal(render_panel_options())});
        } else if (mode == Mode::appearance_dialog) {
            document = dbox({document, native_modal(render_appearance())});
        } else if (mode == Mode::display_bits_dialog) {
            document = dbox({document, native_modal(render_display_bits())});
        } else if (mode == Mode::configure_dialog) {
            document = dbox({document, native_modal(render_configure_dialog())});
        } else if (mode == Mode::external_panelize_dialog) {
            document = dbox({document, centered_modal(render_external_panelize())});
        } else if (mode == Mode::chattr_dialog) {
            document = dbox({document, centered_modal(render_file_attributes())});
        } else if (mode == Mode::advanced_chown_dialog) {
            document = dbox({document, centered_modal(render_advanced_chown())});
        } else if (mode == Mode::diff_options) {
            document = dbox({document, native_modal(render_diff_options())});
        } else if (mode == Mode::file_search_dialog) {
            document = dbox({document, native_modal(render_file_search())});
        } else if (mode == Mode::editor_mail_dialog) {
            document = dbox({document, native_modal(render_editor_mail())});
        } else if (mode == Mode::editor_spelling_dialog) {
            document = dbox({document, native_modal(render_spelling())});
        } else if (mode == Mode::native_input) {
            document = dbox({document, native_modal(render_native_input())});
        } else if (mode == Mode::native_key_query) {
            document = dbox({document, native_modal(render_key_query())});
        } else if (mode == Mode::native_message) {
            if (native_message_return == Mode::learn_keys_dialog) {
                document = dbox({document, centered_modal(render_learn_keys())});
            }
            document = dbox({document, native_modal(render_native_message())});
        } else if (mode == Mode::native_query) {
            document = dbox({document, native_modal(render_native_query())});
        } else if (mode == Mode::editor_save_mode_dialog) {
            document = dbox({document, native_modal(render_editor_save_mode())});
        } else if (mode == Mode::editor_newline_warning) {
            document = dbox({document, native_modal(render_newline_warning())});
        } else if (mode == Mode::local_list) {
            if (local_list_return == Mode::appearance_dialog) {
                document = dbox({document, native_modal(render_appearance())});
            }
            document = dbox({document, local_list_native ? native_modal(render_local_list())
                                                        : centered_modal(render_local_list())});
        } else if (mode == Mode::local_command) {
            document = dbox({document, centered_modal(render_local_command())});
        } else if (mode == Mode::learn_key) {
            document = dbox({document, centered_modal(render_learn_keys()), native_modal(render_learn_key())});
        } else if (mode == Mode::learn_keys_dialog) {
            document = dbox({document, centered_modal(render_learn_keys())});
        } else if (mode == Mode::prompt) {
            const auto focused = [&](Element element,
                                     const std::size_t index) {
                if (prompt_focus == index) {
                    element = focus_dialog(std::move(element));
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
                native_modal(render_editor_options_dialog()),
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
        } else if (mode == Mode::viewer_link_dialog) {
            document = dbox({
                document,
                centered_modal(render_link_dialog(
                    viewer.path(), viewer_choice, viewer_choice_boxes
                )),
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
            static constexpr std::array<int, 7> menu_widths{23, 30, 23, 39, 26, 22, 27};
            const auto menu_width = menu_widths[editor_menu_category];
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
                   mode == Mode::operation_error_dialog ||
                   mode == Mode::recursive_delete_dialog) {
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
        document = paint(std::move(document), palette().normal);
        if (help_browser) {
            const auto previous_context = palette_context;
            palette_context = PaletteContext::help;
            auto help = with_dialog_shadow(render_help() | clear_under);
            palette_context = previous_context;
            document = dbox({std::move(document), std::move(help) | center});
        }
        return document;
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
                    auto provider = active().provider();
                    auto directory = active().path();
                    if (mode_before_prompt == Mode::editor) {
                        const auto& buffer = editor_unnamed_save_buffer == nullptr
                            ? editor : *editor_unnamed_save_buffer;
                        provider = buffer.provider() ? buffer.provider() : local;
                        directory = buffer.open() && provider != local
                            ? provider->parent(buffer.path()) : editor_directory(buffer);
                    }
                    complete_path(
                        prompt_value,
                        prompt_inputs[prompt_action],
                        directory,
                        provider
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
        viewer_save_closes = false;
        if (viewer_switch_pending) {
            const auto path = std::move(*viewer_switch_pending);
            viewer_switch_pending.reset();
            const auto provider = viewer_navigation_provider;
            const auto files = viewer_navigation_files;
            load_viewer(provider, path, true);
            if (viewer_switch_preserve_list) viewer_navigation_files = files;
            viewer_switch_preserve_list = false;
            mode = Mode::viewer;
            return;
        }
        const auto return_screen = viewer_return_screen;
        const auto quitting = viewer_quit_after_close;
        viewer.close();
        viewer_session() = {};
        restore_screen_after_close(return_screen, quitting);
    }

    void request_viewer_close(const bool quitting = false) {
        viewer_quit_after_close = quitting;
        if (viewer.dirty()) {
            viewer_choice = 0;
            mode = Mode::viewer_close_dialog;
        } else {
            close_viewer();
        }
    }

    void change_viewer_file(const int direction) {
        if (!viewer_navigation_provider || viewer_navigation_files.empty()) {
            throw std::runtime_error("this viewer has no adjacent local file list");
        }
        const auto current = std::ranges::find(viewer_navigation_files, viewer.path());
        if (current == viewer_navigation_files.end()) throw std::runtime_error("the viewed file is no longer in its file list");
        const auto index = static_cast<std::size_t>(current - viewer_navigation_files.begin());
        if ((direction < 0 && index == 0U) || (direction > 0 && index + 1U >= viewer_navigation_files.size())) {
            throw std::out_of_range(direction < 0 ? "there is no previous file" : "there is no next file");
        }
        viewer_switch_pending = viewer_navigation_files[direction < 0 ? index - 1U : index + 1U];
        viewer_switch_preserve_list = true;
        request_viewer_close();
    }

    void show_viewer_history() {
        std::vector<std::string> paths = config.viewer_history;
        show_local_list("History", paths, [this, paths](const std::size_t selected) {
            const auto location = resolve_location(paths.at(selected));
            viewer_navigation_provider = location.provider;
            viewer_switch_pending = location.path;
            viewer_switch_preserve_list = false;
            request_viewer_close();
        }, Mode::viewer);
        local_list_native = true;
    }

    void request_viewer_save(const bool close_after) {
        viewer_save_closes = close_after;
        if (viewer.dirty() && viewer.hardlink_count() > 1U) {
            viewer_choice = 0;
            mode = Mode::viewer_link_dialog;
            return;
        }
        viewer.save_changes();
        status = "Saved";
        if (close_after) {
            close_viewer();
        } else {
            mode = Mode::viewer;
        }
    }

    bool handle_viewer_link_dialog(const ftxui::Event& event) {
        const auto input = handle_button_input(
            event, viewer_choice, viewer_choice_boxes, "dpc"
        );
        if (input == ButtonInput::cancel ||
            (input == ButtonInput::activate && viewer_choice == 2U)) {
            viewer_save_closes = false;
            viewer_quit_after_close = false;
            viewer_switch_pending.reset();
            mode = Mode::viewer;
        } else if (input == ButtonInput::activate) {
            try {
                viewer.save_changes(
                    {}, viewer_choice == 0U ? ViewerLinkedSave::detach
                                           : ViewerLinkedSave::preserve_links
                );
                status = "Saved";
                if (viewer_save_closes) {
                    close_viewer();
                } else {
                    mode = Mode::viewer;
                }
            } catch (const std::exception& error) {
                viewer_switch_pending.reset();
                viewer_message_title = "Viewer error";
                viewer_message = error.what();
                mode = Mode::viewer_message;
            }
        }
        return input != ButtonInput::ignored;
    }

    bool handle_viewer_close_dialog(const ftxui::Event& event) {
        const auto input = handle_button_input(
            event, viewer_choice, viewer_choice_boxes, "sdc"
        );
        if (input == ButtonInput::cancel ||
            (input == ButtonInput::activate && viewer_choice == 2U)) {
            viewer_switch_pending.reset();
            viewer_quit_after_close = false;
            mode = Mode::viewer;
        } else if (input == ButtonInput::activate) {
            try {
                if (viewer_choice == 0U) {
                    request_viewer_save(true);
                } else {
                    viewer.discard_changes();
                    close_viewer();
                }
            } catch (const std::exception& error) {
                viewer_switch_pending.reset();
                viewer_message_title = "Viewer error";
                viewer_message = error.what();
                mode = Mode::viewer_message;
            }
        }
        return input != ButtonInput::ignored;
    }

    void run_viewer_search(const bool quick, const bool wrap) {
        auto& target = quick ? quick_viewer : viewer;
        const auto& needle = quick ? quick_view_search_needle : viewer_search_needle;
        const auto& options = quick ? quick_view_search_options : viewer_search_options;
        const bool all_charsets = quick ? quick_view_search_all_charsets : viewer_search_all_charsets;
        viewer_search_quick = quick;
        const auto return_mode = quick ? Mode::panels : Mode::viewer;
        if (target.find(needle, options, {}, wrap, all_charsets)) {
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
            static_cast<void>(refresh_viewer());
            if (mode != Mode::viewer || event == Event::CtrlL) return true;
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
                    const auto direction = mouse.button == ftxui::Mouse::WheelUp ? -1 : 1;
                    const auto amount = direction * (config.viewer_mouse_page_scrolling ? 1 : 3);
                    if (config.viewer_mouse_page_scrolling.value_or(false)) {
                        viewer.page(direction, width, height);
                    } else if (viewer.hex()) {
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
            if (event == Event::F2) {
                if (viewer.hex()) {
                    if (viewer_temporary_file) {
                        throw std::runtime_error("command output is read-only");
                    }
                    viewer.toggle_hex_editing();
                } else {
                    viewer.set_wrap(!viewer.wrap());
                    config.viewer_wrap = viewer.wrap();
                }
                return true;
            }
            if (event == Event::F4) {
                viewer.toggle_hex();
                viewer_hex_first_column = 0;
                return true;
            }
            if (event == Event::F5) {
                viewer_goto_quick = false;
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
                request_viewer_save(false);
                return true;
            }
            if (viewer.hex() && event == Event::Tab) {
                viewer.toggle_hex_navigation();
                return true;
            }
            if (event == Event::F7 ||
                (!viewer.hex_editing() && (character('/') || character('?')))) {
                show_file_search(FileSearchTarget::viewer,
                    character('/') || character('?') ? std::optional<bool>{character('?')} : std::nullopt);
                return true;
            }
            if (event == Event::F8) {
                viewer.toggle_magic();
                viewer_hex_first_column = 0;
                return true;
            }
            if (event == Event::F9) {
                viewer.toggle_nroff();
                return true;
            }
            if (event == Event::AltE) {
                show_codepage_chooser(viewer.encoding(), Mode::viewer, [this](const std::string& value) {
                    viewer.set_encoding(value);
                });
                return true;
            }
            if (event == Event::CtrlF || event == Event::CtrlB) {
                change_viewer_file(event == Event::CtrlF ? 1 : -1);
                return true;
            }
            if (event == Event::Special("\x1b" "E")) {
                show_viewer_history();
                return true;
            }
            if (event == Event::AltR) {
                viewer_ruler = (viewer_ruler + 1U) % 3U;
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
            if (event.is_character() && event.character().size() == 1U &&
                event.character()[0] >= '0' && event.character()[0] <= '9') {
                viewer.select_bookmark(static_cast<std::size_t>(event.character()[0] - '0'));
                return true;
            }
            if (character('m') || character('r')) {
                if (character('m')) viewer.set_bookmark();
                else viewer.goto_bookmark();
                return true;
            }
            if (event == Event::Special("\x1B[31~") ||
                event == Event::Special("\x1B[18;2~") ||
                character('n') || character('N') || event == Event::CtrlS || event == Event::CtrlR) {
                if (event == Event::CtrlS) viewer_search_options.backwards = false;
                if (event == Event::CtrlR) viewer_search_options.backwards = true;
                if (character('N')) viewer_search_options.backwards = !viewer_search_options.backwards;
                if (viewer_search_needle.empty()) {
                    show_file_search(FileSearchTarget::viewer);
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
            viewer_switch_pending.reset();
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
        editor_options_tab_width = std::to_string(editor.tab_width());
        editor_options_wrap_length = std::to_string(config.editor_wrap_length);
        editor_options_wrap = config.editor_wrap_mode;
        editor_options_values = {
            config.editor_fake_half_tabs, config.editor_backspace_through_tabs, config.editor_fill_tabs_with_spaces,
            editor_auto_indent, config.editor_confirm_save, config.editor_save_position,
            config.editor_visible_trailing_spaces, config.editor_visible_tabs, editor_syntax,
            config.editor_cursor_after_inserted_block, config.editor_persistent_selection,
            config.editor_cursor_beyond_eol, config.editor_group_undo
        };
        editor_options_error.clear();
        editor_options_focus = 0;
        editor_tab_input.reset(editor_options_tab_width, true);
        editor_wrap_input.reset(editor_options_wrap_length, true);
        mode = Mode::editor_options_dialog;
    }

    void start_editor_search_dialog(const bool replace) {
        editor_dialog_query = editor.search.needle;
        editor_dialog_replacement =
            replace ? editor_replace_text : std::string{};
        editor_query_input.reset(editor_dialog_query, true);
        editor_replacement_input.reset(editor_dialog_replacement, true);
        editor_dialog_error.clear();
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
        syntax_buffer_path.reset();
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

    void new_editor_buffer() {
        const auto directory = editor.open() ? editor_directory(editor) : pane_execution_directory(active());
        open_editor_buffer(local, "", true);
        editor.base_directory = directory;
        mode = Mode::editor;
        status = "New file";
    }

    struct EditorStartupFile {
        Location location;
        std::size_t line{0};
    };
    std::function<void()> editor_open_cancel;
    std::vector<EditorStartupFile> editor_startup_files;
    std::size_t editor_startup_cursor{0};
    std::size_t editor_startup_first_line{0};
    std::size_t editor_startup_opened{0};
    bool editor_startup_placeholder{false};

    void finish_editor_open_confirmation() {
        if (editor_open_cancel && mode != Mode::native_query) {
            auto cancelled = std::exchange(editor_open_cancel, {});
            cancelled();
        }
    }

    EditorLock acquire_editor_lock(
        const ProviderPtr& provider, const std::string& path, const EditorLock& approved = {}
    ) {
        if (path.empty() || dynamic_cast<const LocalProvider*>(provider.get()) == nullptr ||
            !has_capability(provider->capabilities(), ProviderCapability::write)) return {};
        const std::filesystem::path native = provider->normalize(path);
        if (approved.current() && approved.matches_file(native)) return approved;
        if (::access(native.c_str(), W_OK) != 0 &&
            (errno != ENOENT || ::access(native.parent_path().c_str(), W_OK | X_OK) != 0)) return {};
        return EditorLock::acquire(native);
    }

    void confirm_editor_lock(
        const EditorLockConflict& conflict, const Mode return_mode,
        std::function<void(EditorLock)> accepted, std::function<void()> declined
    ) {
        editor_open_cancel = declined;
        show_native_query(
            "File locked", "File \"" + conflict.file().filename().string() +
                "\" is already being edited.\nUser: " + conflict.owner() +
                "\nProcess ID: " + (conflict.process_id() ? std::to_string(*conflict.process_id()) : "unknown"),
            {"Grab lock", "Ignore lock"},
            [this, conflict, accepted = std::move(accepted), declined = std::move(declined), return_mode](
                const std::size_t selected
            ) {
                editor_open_cancel = {};
                try {
                    accepted(selected == 0U ? EditorLock::grab(conflict) : EditorLock::ignore(conflict));
                } catch (const EditorLockConflict& changed) {
                    confirm_editor_lock(changed, return_mode, accepted, declined);
                } catch (const std::exception& error) {
                    editor_open_cancel = declined;
                    show_native_query("Editor error", error.what(), {"OK"},
                        [this, declined](const std::size_t) {
                            editor_open_cancel = {};
                            declined();
                        }, return_mode);
                }
            }, return_mode
        );
    }

    bool run_editor_action(std::function<bool()> action) {
        const auto original_mode = mode;
        const auto original_prefix = keymap_prefix;
        const auto original_prefix_mode = keymap_prefix_mode;
        const auto recorded_keys = macro_keys.size();
        try {
            return action();
        } catch (const EditorEditBlocked& blocked) {
            const auto return_mode = mode;
            const auto resume_macro = std::exchange(macro_replaying, false);
            confirm_editor_lock(blocked.conflict, return_mode,
                [this, action = std::move(action), buffer = blocked.buffer, original_mode,
                 original_prefix, original_prefix_mode, recorded_keys, resume_macro](EditorLock choice) {
                    buffer->file_lock = std::move(choice);
                    mode = original_mode;
                    keymap_prefix = original_prefix;
                    keymap_prefix_mode = original_prefix_mode;
                    if (macro_keys.size() > recorded_keys) macro_keys.resize(recorded_keys);
                    macro_replaying = resume_macro;
                    static_cast<void>(run_editor_action(action));
                    update_editor_view();
                },
                [this, return_mode, recorded_keys] {
                    if (macro_keys.size() > recorded_keys) macro_keys.resize(recorded_keys);
                    macro_replaying = false;
                    mode = return_mode;
                    status = "Editing cancelled";
                });
            return true;
        }
    }

    void confirm_large_editor_file(
        const ProviderPtr& provider, const Entry& file, const Mode return_mode,
        std::function<void()> accepted, std::function<void()> declined = {}
    ) {
        if (!declined) declined = [this] { status = "Opening cancelled"; };
        editor_open_cancel = declined;
        show_native_query(
            "Warning", "File \"" + format_location(provider, file.path) + "\" is too large (" +
                format_size(file.size) + ").\nOpen it anyway?",
            {"Yes", "No"},
            [this, accepted = std::move(accepted), declined = std::move(declined), return_mode](
                const std::size_t selected
            ) {
                editor_open_cancel = {};
                try {
                    if (selected == 0U) accepted();
                    else declined();
                } catch (const std::exception& error) {
                    show_native_message("Editor error", error.what(), return_mode);
                }
            },
            return_mode
        );
    }

    void open_editor_buffer(
        const ProviderPtr& provider,
        const std::string& path,
        const bool create,
        const bool require_existing = false,
        std::function<void()> after_open = {},
        const std::optional<Entry>& large_file_approval = std::nullopt
    ) {
        const auto return_mode = mode == Mode::prompt ? mode_before_prompt
            : mode == Mode::editor_menu || mode == Mode::editor_history ? Mode::editor : mode;
        try {
            load_editor_buffer(provider, path, create, require_existing, large_file_approval);
            mode = Mode::editor;
            status = create ? "New file" : "File opened";
            if (after_open) {
                static_cast<void>(run_editor_action([after_open = std::move(after_open)] {
                    after_open();
                    return true;
                }));
            }
        } catch (const EditorLargeFileConfirmationRequired& required) {
            const auto approval = required.file();
            confirm_large_editor_file(provider, approval, return_mode,
                [this, provider, path, create, require_existing, after_open = std::move(after_open), approval] {
                    open_editor_buffer(provider, path, create, require_existing, after_open, approval);
                });
        }
    }

    void continue_editor_startup(const std::optional<Entry>& large_file_approval = std::nullopt) {
        auto approval = large_file_approval;
        while (editor_startup_cursor < editor_startup_files.size()) {
            const auto file = editor_startup_files[editor_startup_cursor];
            try {
                load_editor_buffer(file.location.provider, file.location.path, false, false, approval);
            } catch (const EditorLargeFileConfirmationRequired& required) {
                const auto next_approval = required.file();
                confirm_large_editor_file(file.location.provider, next_approval, Mode::editor,
                    [this, next_approval] { continue_editor_startup(next_approval); },
                    [this] {
                        ++editor_startup_cursor;
                        continue_editor_startup();
                    });
                return;
            } catch (const std::exception& error) {
                if (terminal == nullptr) throw;
                ++editor_startup_cursor;
                editor_open_cancel = [this] { continue_editor_startup(); };
                show_native_query("Editor error", error.what(), {"OK"}, [this](const std::size_t) {
                    editor_open_cancel = {};
                    continue_editor_startup();
                }, Mode::editor);
                return;
            }
            if (file.line != 0U) {
                editor.goto_line(file.line);
                editor.home();
                update_editor_view();
            }
            ++editor_startup_opened;
            ++editor_startup_cursor;
            approval.reset();
        }
        if (editor_startup_opened > 1U) static_cast<void>(switch_editor_buffer(true));
        if (editor_startup_first_line != 0U) {
            editor.goto_line(editor_startup_first_line);
            editor.home();
            update_editor_view();
        }
        editor_startup_placeholder = false;
        mode = Mode::editor;
    }

    void load_editor_buffer(
        const ProviderPtr& provider,
        const std::string& path,
        const bool create,
        const bool require_existing,
        const std::optional<Entry>& large_file_approval
    ) {
        const bool same_session =
            mode == Mode::editor || mode == Mode::editor_menu ||
            mode == Mode::editor_history ||
            (mode == Mode::prompt && mode_before_prompt == Mode::editor);
        if (!create && same_session && editor.open()) {
            const auto normalized = provider->normalize(path);
            if (!editor.path().empty() && editor.provider()->id() == provider->id() &&
                provider->normalize(editor.path()) == normalized) {
                return;
            }
            for (auto index = editor_session_base; index < editor_buffers.size(); ++index) {
                if (!editor_buffers[index].path().empty() && editor_buffers[index].provider()->id() == provider->id() &&
                    provider->normalize(editor_buffers[index].path()) == normalized) {
                    std::swap(editor, editor_buffers[index]);
                    remember_editor_path(editor_location(editor));
                    reset_editor_view_state();
                    return;
                }
            }
        }
        EditorBuffer next;
        next.base_directory = path.empty()
            ? (editor.open() ? editor_directory(editor) : active().navigation_path())
            : provider->parent(path);
        next.set_tab_width(editor.tab_width());
        next.set_behavior(editor_behavior());
        if (editor.open()) {
            next.fullscreen = editor.fullscreen;
            next.window_x = editor.window_x;
            next.window_y = editor.window_y;
            next.window_width = editor.window_width;
            next.window_height = editor.window_height;
            next.window_geometry_saved = editor.window_geometry_saved;
        }
        next.set_save_mode(config.editor_save_mode == "backup" ? EditorSaveMode::backup
                         : config.editor_save_mode == "in-place" ? EditorSaveMode::in_place
                                                               : EditorSaveMode::atomic);
        next.set_backup_extension(config.editor_backup_extension);
        if (create || (!require_existing && !large_file_approval && !provider->exists(path))) {
            next.create(provider, path);
        } else {
            next.open(provider, path, {}, large_file_approval);
            const auto saved = config.editor_positions.find(editor_location(next));
            if (config.editor_save_position && saved != config.editor_positions.end()) {
                next.set_position({saved->second.row, saved->second.column});
                std::set<std::size_t> bookmarks;
                for (const auto row : saved->second.bookmarks) {
                    if (row < next.lines().size()) bookmarks.insert(row);
                }
                next.set_bookmarks(std::move(bookmarks));
                next.first_row = next.row() > editor_visible_rows() / 2U
                                     ? next.row() - editor_visible_rows() / 2U : 0U;
            }
        }
        next.before_change = [this](EditorBuffer& buffer) {
            if (!buffer.file_lock.current()) {
                buffer.file_lock = acquire_editor_lock(buffer.provider(), buffer.path());
            }
        };
        next.search.change_id = next.change_id();
        if (editor.open() && !editor_startup_placeholder) {
            editor_buffers.push_back(std::move(editor));
        }
        editor = std::move(next);
        editor_startup_placeholder = false;
        remember_editor_path(editor_location(editor));
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
        remember_editor_path(editor_location(editor));
        reset_editor_view_state();
        status = "Buffer: " + editor.path();
        return true;
    }

    void save_editor() {
        if (editor.provider() &&
            !has_capability(editor.provider()->capabilities(), ProviderCapability::write)) {
            show_editor_error(std::runtime_error("this filesystem is read-only; use Save as to save a local copy"));
            return;
        }
        if (!config.editor_confirm_save) {
            begin_editor_buffer_save(editor, EditorSavePurpose::save);
            return;
        }
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
        const auto continuation = editor_save_as_continuation.value_or(purpose);
        try {
            if (editor_save_buffer == nullptr) {
                throw std::runtime_error("no buffer to save");
            }
            const auto value = editor_save_buffer->text();
            if (purpose != EditorSavePurpose::diff_save && config.editor_check_newline && !newline_warning_accepted &&
                !value.empty() && value.back() != '\n') {
                newline_warning_preserve_links = preserve_links;
                newline_warning_choice = 0;
                mode = Mode::editor_newline_warning;
                return;
            }
            if (purpose == EditorSavePurpose::save_as && editor_save_overwrite &&
                editor_save_provider->stat_follow(editor_save_path, {}).revision != editor_overwrite_revision) {
                throw std::runtime_error("Save As destination changed while awaiting confirmation");
            }
            if (purpose != EditorSavePurpose::diff_save) {
                const auto saving_as = purpose == EditorSavePurpose::save_as;
                const auto& approval = editor_save_lock.current() ? editor_save_lock : editor_save_buffer->file_lock;
                editor_save_lock = acquire_editor_lock(
                    saving_as ? editor_save_provider : editor_save_buffer->provider(),
                    saving_as ? editor_save_path : editor_save_buffer->path(), approval
                );
            }
            if (preserve_links) {
                if (purpose == EditorSavePurpose::save_as) {
                    editor_save_buffer->save_as_preserving_links(
                        editor_save_provider, editor_save_path, editor_save_overwrite, editor_save_line_ending
                    );
                } else {
                    editor_save_buffer->save_preserving_links(false);
                }
            } else if (purpose == EditorSavePurpose::save_as) {
                editor_save_buffer->save_as(
                    editor_save_provider, editor_save_path, editor_save_overwrite, editor_save_line_ending
                );
            } else {
                editor_save_buffer->save(false);
            }
            editor_save_buffer->file_lock.reset();
            editor_save_buffer->changed_once = true;
            editor_save_lock.reset();
            if (purpose != EditorSavePurpose::diff_save) remember_editor_path(editor_location(*editor_save_buffer));
            const auto saved_path = std::filesystem::path(editor_save_buffer->path());
            editor_save_buffer = nullptr;
            editor_save_provider.reset();
            editor_unnamed_save_buffer = nullptr;
            editor_save_as_continuation.reset();
            prompt_action = PromptAction::none;
            prompt_value.clear();
            mode = Mode::editor;
            status = "Saved";
            if (purpose == EditorSavePurpose::diff_save) {
                ++diff_save_index;
                mode = Mode::diff;
                continue_diff_saves();
                return;
            }
            if (saved_path.parent_path() == default_config_path().parent_path() &&
                (saved_path.filename() == "syntax" || saved_path.filename() == "extensions" ||
                 saved_path.filename() == "mc.ext.ini" || saved_path.filename() == "mc.ext" ||
                 saved_path.filename() == "highlighting" || saved_path.filename() == "filehighlight.ini")) {
                try {
                    reload_local_rules();
                } catch (const std::exception& error) {
                    editor_quit_requested = false;
                    show_editor_message("Saved; configuration error", error.what());
                    return;
                }
            }
            if (saved_path == active_palette->panels.resource || saved_path == highlight_rules.resource()) {
                try {
                    reload_skin_resources();
                } catch (const std::exception& error) {
                    editor_quit_requested = false;
                    show_editor_message("Saved; skin/highlighting error", error.what());
                    return;
                }
            }
            if (continuation == EditorSavePurpose::close || continuation == EditorSavePurpose::quit) {
                accept_editor_close();
            }
        } catch (const EditorLockConflict& conflict) {
            confirm_editor_lock(conflict, Mode::editor,
                [this, preserve_links](EditorLock choice) {
                    editor_save_lock = std::move(choice);
                    complete_editor_buffer_save(preserve_links);
                },
                [this] {
                    editor_save_lock.reset();
                    editor_save_buffer = nullptr;
                    editor_save_provider.reset();
                    editor_unnamed_save_buffer = nullptr;
                    editor_save_as_continuation.reset();
                    editor_quit_requested = false;
                    prompt_action = PromptAction::none;
                    prompt_value.clear();
                    mode = Mode::editor;
                    status = "Saving cancelled";
                });
        } catch (const std::exception& error) {
            editor_save_lock.reset();
            editor_save_buffer = nullptr;
            editor_save_provider.reset();
            editor_unnamed_save_buffer = nullptr;
            editor_save_as_continuation.reset();
            editor_quit_requested = false;
            if (purpose == EditorSavePurpose::diff_save) {
                diff_saving = diff_closing = false;
                show_native_message("Diff save", error.what(), Mode::diff);
            } else show_editor_error(error);
        }
    }

    void begin_editor_buffer_save(
        EditorBuffer& buffer,
        const EditorSavePurpose purpose
    ) {
        editor_save_lock.reset();
        editor_save_buffer = &buffer;
        editor_save_purpose = purpose;
        if (purpose != EditorSavePurpose::save_as) editor_save_provider = buffer.provider();
        newline_warning_accepted = false;
        if (buffer.path().empty() && purpose != EditorSavePurpose::save_as) {
            editor_unnamed_save_buffer = &buffer;
            editor_save_as_continuation = purpose;
            mode = Mode::editor;
            start_prompt(PromptAction::editor_save_as, "Save as");
            return;
        }
        continue_editor_buffer_save();
    }

    void continue_editor_buffer_save() {
        try {
            if (editor_save_buffer == nullptr) throw std::runtime_error("no buffer to save");
            if (!editor_save_provider ||
                !has_capability(editor_save_provider->capabilities(), ProviderCapability::write)) {
                throw std::runtime_error("this filesystem is read-only; use Save as to save a local copy");
            }
            const auto links = editor_save_purpose == EditorSavePurpose::save_as
                ? (editor_save_provider->exists(editor_save_path)
                    ? editor_save_provider->stat_follow(editor_save_path, {}).links : 1U)
                : editor_save_buffer->hardlink_count();
            if (links > 1U) {
                editor_link_choice = 0;
                mode = Mode::editor_link_dialog;
            } else {
                complete_editor_buffer_save(false);
            }
        } catch (const std::exception& error) {
            editor_save_lock.reset();
            editor_save_buffer = nullptr;
            editor_save_provider.reset();
            editor_quit_requested = false;
            if (editor_save_purpose == EditorSavePurpose::diff_save) {
                diff_saving = diff_closing = false;
                show_native_message("Diff save", error.what(), Mode::diff);
            } else show_editor_error(error);
        }
    }

    void request_editor_save_as() {
        editor_save_lock.reset();
        newline_warning_accepted = false;
        auto& buffer = editor_unnamed_save_buffer == nullptr ? editor : *editor_unnamed_save_buffer;
        const auto destination = resolve_editor_location(prompt_value, &buffer);
        if (!has_capability(destination.provider->capabilities(), ProviderCapability::write)) {
            throw std::runtime_error("this filesystem is read-only; choose a local Save As destination");
        }
        editor_save_path = destination.path;
        editor_save_provider = destination.provider;
        editor_save_overwrite = false;
        editor_save_buffer = &buffer;
        editor_save_purpose = EditorSavePurpose::save_as;
        if (!buffer.path().empty() && buffer.provider() == editor_save_provider &&
            editor_save_path == buffer.provider()->normalize(buffer.path())) {
            begin_editor_buffer_save(buffer, EditorSavePurpose::save_as);
        } else if (editor_save_provider->exists(editor_save_path)) {
            const auto target = editor_save_provider->stat_follow(editor_save_path, {});
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
            const bool unnamed = editor_save_as_continuation.has_value();
            editor_quit_requested = false;
            editor_save_lock.reset();
            editor_save_buffer = nullptr;
            editor_save_provider.reset();
            editor_unnamed_save_buffer = nullptr;
            editor_save_as_continuation.reset();
            mode = unnamed ? Mode::editor : editor_save_purpose == EditorSavePurpose::save_as
                       ? Mode::prompt
                       : editor_save_purpose == EditorSavePurpose::diff_save ? Mode::diff : Mode::editor;
            if (editor_save_purpose == EditorSavePurpose::diff_save) diff_saving = diff_closing = false;
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
            editor_save_lock.reset();
            editor_save_buffer = nullptr;
            editor_save_provider.reset();
            mode = Mode::prompt;
        } else if (input == ButtonInput::activate) {
            try {
                if (editor_save_provider->stat_follow(editor_save_path, {}).revision != editor_overwrite_revision) {
                    throw std::runtime_error("Save As destination changed while awaiting confirmation");
                }
                editor_save_overwrite = true;
                if (editor_save_provider->stat_follow(editor_save_path, {}).links > 1U) {
                    editor_link_choice = 0;
                    mode = Mode::editor_link_dialog;
                } else {
                    complete_editor_buffer_save(false);
                }
            } catch (const std::exception& error) {
                editor_save_lock.reset();
                editor_save_buffer = nullptr;
                editor_save_provider.reset();
                show_editor_error(error);
            }
        }
        return input != ButtonInput::ignored;
    }

    void close_editor() {
        const auto diff_return = std::exchange(editor.diff_screen_return, std::nullopt);
        remember_editor_position(editor);
        reset_editor_view_state();
        editor.close();
        editor.search = {};
        if (diff_return) {
            if (!editor_buffers.empty()) {
                editor = std::move(editor_buffers.back());
                editor_buffers.pop_back();
            }
            editor_return_mode = Mode::panels;
            editor_session_base = 0;
            if (activate_screen({Mode::diff, diff_return->first})) {
                diff_edit_side = diff_return->second;
                resume_diff_after_edit();
            } else restore_screen_after_close({});
            return;
        }
        if (editor_buffers.size() > editor_session_base) {
            editor = std::move(editor_buffers.back());
            editor_buffers.pop_back();
            remember_editor_path(editor_location(editor));
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
        if ((exit_after_viewer || exit_after_diff) && screen_targets().size() == 1U) {
            exit_terminal();
            return;
        }
        if (return_mode == Mode::diff) { resume_diff_after_edit(); return; }
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
        for (const auto& buffer : editor_buffers) remember_editor_position(buffer);
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
                const EditorPosition before{editor.row(), editor.column()};
                editor.paste(editor_clipboard);
                if (!config.editor_cursor_after_inserted_block) editor.set_position(before);
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

    void execute_editor_menu_item() try {
        const auto category = editor_menu_category;
        const auto item =
            editor_menu_items()[category][editor_menu_item].action;
        if (item < 0) {
            return;
        }
        mode = Mode::editor;
        if (category == 0U && item >= 7) {
            switch (item) {
                case 7: show_insert_file(); break;
                case 8: show_save_block(); break;
                case 9: open_user_menu(Mode::editor); break;
                case 10:
                    show_editor_message("About mcpp",
                        std::string("mcpp ") + MCPP_VERSION +
                        "\nIndependent C++20 Linux file manager and editor.");
                    break;
                default: throw std::runtime_error("unknown editor file action");
            }
            return;
        }
        if (category == 2U && item >= 3) {
            editor_bookmark_action(item);
            return;
        }
        if (category == 3U && item == 7) {
            show_editor_encoding();
            return;
        }
        if (category == 3U && item >= 4 && item <= 6) {
            if (item == 4) show_find_declaration();
            else move_declaration_history(item == 6);
            return;
        }
        if (category == 3U && item == 8) {
            std::cout << "\033[H\033[2J" << std::flush;
            return;
        }
        if (category == 3U && item >= 9 && item <= 11) {
            if (item == 9) toggle_macro_recording();
            else if (item == 10) show_delete_macro();
            else show_repeat_actions();
            return;
        }
        if (category == 3U && item == 12) { show_editor_mail(); return; }
        if (category == 3U && (item == 13 || item == 14)) { show_spelling(item == 13); return; }
        if (category == 3U && item == 15) { show_spelling_languages(); return; }
        if (category == 4U) {
            if (item == 0) show_literal_insert();
            else if (item == 1) show_date_insert();
            else if (item == 4) show_paste_output();
            else show_editor_transform(item);
            return;
        }
        if (category == 5U && item >= 3 && item <= 5) {
            show_editor_window(item);
            return;
        }
        if (category == 6U && item == 6) {
            open_config_editor(default_editor_user_menu_path(),
                "# Editor menu: key and title, followed by indented shell commands.\n"
                "i Inspect edited path\n    stat -- %f\n");
            return;
        }
        if (category == 6U && item == 2) {
            show_editor_save_mode();
            return;
        }
        if (category == 6U && item == 3) {
            show_learn_keys(Mode::editor);
            return;
        }
        if (category == 6U && item == 4) {
            show_syntax_settings();
            return;
        }
        if (category == 6U && item == 5) {
            const auto directory = default_config_path().parent_path();
            auto resource = directory / "Syntax";
            const auto language = effective_syntax_language();
            if (syntax_highlighter.supports_language(language)) {
                const auto definition = syntax_highlighter.definition_path(editor.path(), editor.lines(), language);
                if (!definition.empty()) resource = definition;
            }
            open_config_editor(resource,
                "# mcpp native-format syntax registry; definitions belong in syntax.d/.\n"
                "# file .*\\\\.example$ Example\\sLanguage\n"
                "# include example.syntax\n"
                "# Legacy filename=language overrides remain in the lowercase syntax file.\n");
            return;
        }
        if (category == 0U) {
            if (item == 0U) {
                start_prompt(
                    PromptAction::editor_open,
                    "Open file",
                    editor.path()
                );
            } else if (item == 1U) {
                new_editor_buffer();
            } else if (item == 2U) {
                request_editor_close();
            } else if (item == 3U) {
                save_editor();
            } else if (item == 4U) {
                start_prompt(
                    PromptAction::editor_save_as,
                    "Save as",
                    editor_location(editor)
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
                config.save();
                status = "Setup saved";
            } catch (const std::exception& error) {
                show_editor_error(error);
            }
            return;
        }
        status = "No command in this menu";
    } catch (const std::exception& error) {
        show_editor_error(error);
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
        if (!editor_menu_dropped && event.is_character() &&
            event.character().size() == 1U) {
            const auto key = std::tolower(static_cast<unsigned char>(event.character()[0]));
            for (std::size_t index = 0; index < editor_menu_names.size(); ++index) {
                if (std::tolower(static_cast<unsigned char>(
                        editor_menu_names[index][editor_menu_mnemonics[index]]
                    )) == key) {
                    editor_menu_category = index;
                    reset_editor_menu_item();
                    editor_menu_dropped = true;
                    return true;
                }
            }
        }
        if (editor_menu_dropped && event.is_character() && event.character().size() == 1U) {
            const auto key = std::tolower(static_cast<unsigned char>(event.character()[0]));
            const auto& items = editor_menu_items()[editor_menu_category];
            for (std::size_t index = 0; index < items.size(); ++index) {
                if (items[index].mnemonic != 0 && items[index].mnemonic == key) {
                    if (items[index].action >= 0) {
                        editor_menu_item = index;
                        execute_editor_menu_item();
                    }
                    return true;
                }
            }
        }
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
                    mouse.x < ftxui::Terminal::Size().dimx &&
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

    void apply_editor_options() {
        const auto number = [](const std::string& value, const std::size_t maximum) {
            std::size_t result = 0;
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || result < 1U || result > maximum) {
                throw std::invalid_argument("spacing or wrap length is outside its supported range");
            }
            return result;
        };
        try {
            const auto tab = number(editor_options_tab_width, 32);
            const auto wrap = number(editor_options_wrap_length, 4096);
            remember_input(editor_tab_input, editor_options_tab_width);
            remember_input(editor_wrap_input, editor_options_wrap_length);
            const auto& values = editor_options_values;
            config.editor_wrap_mode = editor_options_wrap;
            config.editor_wrap_length = wrap;
            config.editor_fake_half_tabs = values[0];
            config.editor_backspace_through_tabs = values[1];
            config.editor_fill_tabs_with_spaces = values[2];
            config.editor_auto_indent = editor_auto_indent = values[3];
            config.editor_confirm_save = values[4];
            config.editor_save_position = values[5];
            config.editor_visible_trailing_spaces = values[6];
            config.editor_visible_tabs = values[7];
            config.editor_syntax = editor_syntax = values[8];
            config.editor_cursor_after_inserted_block = values[9];
            config.editor_persistent_selection = values[10];
            config.editor_cursor_beyond_eol = values[11];
            config.editor_group_undo = values[12];
            config.editor_tab_width = tab;
            editor_indent_width = values[0] ? std::max<std::size_t>(1U, tab / 2U) : tab;
            editor_show_whitespace = values[6] || values[7];
            editor.set_tab_width(tab);
            editor.set_behavior(editor_behavior());
            for (auto& buffer : editor_buffers) {
                buffer.set_tab_width(tab);
                buffer.set_behavior(editor_behavior());
            }
            syntax_spans.clear();
            syntax_path.clear();
            mode = Mode::editor;
            status = "Editor options updated";
        } catch (const std::exception& error) {
            editor_options_error = error.what();
        }
    }

    bool handle_editor_options_dialog(const ftxui::Event& event) {
        using namespace ftxui;
        if (modal_cancel(event)) { mode = Mode::editor; return true; }
        if (event.is_mouse()) {
            auto copy = event;
            const auto& mouse = copy.mouse();
            if (editor_tab_input.dragging) return input_mouse(editor_options_tab_width, editor_tab_input, mouse);
            if (editor_wrap_input.dragging) return input_mouse(editor_options_wrap_length, editor_wrap_input, mouse);
            if (mouse.button != Mouse::Left || mouse.motion != Mouse::Pressed) return true;
            for (std::size_t index = 0; index < editor_options_boxes.size(); ++index) {
                if (index < 18U && !editor_options_viewport.Contain(mouse.x, mouse.y)) continue;
                if (!editor_options_boxes[index].Contain(mouse.x, mouse.y)) continue;
                if (index < 3U) { editor_options_wrap = index; editor_options_focus = 0; }
                else if (index < 16U) {
                    const auto option = index - 3U;
                    editor_options_values[option] = !editor_options_values[option];
                    editor_options_focus = option < 3U ? option + 1U : option + 2U;
                } else if (index == 16U || index == 17U) {
                    editor_options_focus = index == 16U ? 4U : 15U;
                    input_mouse(index == 16U ? editor_options_tab_width : editor_options_wrap_length,
                                index == 16U ? editor_tab_input : editor_wrap_input, mouse);
                } else if (index == 18U) apply_editor_options();
                else mode = Mode::editor;
                return true;
            }
            return true;
        }
        if (event == Event::Tab || event == Event::TabReverse) {
            editor_options_focus = event == Event::Tab ? (editor_options_focus + 1U) % 18U
                                                       : (editor_options_focus + 17U) % 18U;
        } else if (editor_options_focus == 0U && (event == Event::ArrowUp || event == Event::ArrowDown)) {
            editor_options_wrap = event == Event::ArrowDown ? (editor_options_wrap + 1U) % 3U
                                                          : (editor_options_wrap + 2U) % 3U;
        } else if (event == Event::Return || (event == Event::Character(' ') && editor_options_focus >= 16U)) {
            if (editor_options_focus == 17U) mode = Mode::editor;
            else apply_editor_options();
        } else if (editor_options_focus == 4U || editor_options_focus == 15U) {
            return edit_input(editor_options_focus == 4U ? editor_options_tab_width : editor_options_wrap_length,
                              editor_options_focus == 4U ? editor_tab_input : editor_wrap_input, event);
        } else if (event == Event::ArrowUp || event == Event::ArrowDown) {
            editor_options_focus = event == Event::ArrowDown ? (editor_options_focus + 1U) % 18U
                                                           : (editor_options_focus + 17U) % 18U;
        } else if (event == Event::Character(' ') && editor_options_focus != 0U) {
            const auto option = editor_options_focus < 4U ? editor_options_focus - 1U : editor_options_focus - 2U;
            if (option < editor_options_values.size()) editor_options_values[option] = !editor_options_values[option];
        } else if (event.is_character() && event.character().size() == 1U) {
            const auto key = static_cast<char>(std::tolower(static_cast<unsigned char>(event.character()[0])));
            const auto radio = std::string_view("ndw").find(key);
            const auto option = std::string_view("fbsrmpvtxuiyg").find(key);
            if (radio != std::string_view::npos) { editor_options_wrap = radio; editor_options_focus = 0; }
            else if (option != std::string_view::npos) {
                editor_options_values[option] = !editor_options_values[option];
                editor_options_focus = option < 3U ? option + 1U : option + 2U;
            }
        }
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
            try {
                const auto location = resolve_editor_location(path);
                open_editor_buffer(
                    location.provider,
                    location.path,
                    false
                );
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
        remember_editor_path(editor_location(editor));
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
        try {
            remember_input(editor_query_input, editor_dialog_query);
            if (replace) remember_input(editor_replacement_input, editor_dialog_replacement);
            editor_dialog_error.clear();
        } catch (const std::exception& error) {
            editor_dialog_error = error.what();
            return true;
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
        const auto match = editor.search_selection();
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

    struct EditorMousePosition {
        std::size_t row;
        std::size_t column;
        std::size_t display;
    };

    std::optional<EditorMousePosition> editor_mouse_position(
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
        const auto rendered_row = std::min(
            editor.lines().size() - 1U, editor_rendered_first_row + index
        );
        auto row = rendered_row;
        // Moving past the drawn edge lets update_editor_view scroll the drag.
        if (clamp && mouse.y < editor_line_boxes.front().y_min && row > 0U) {
            --row;
        } else if (clamp && mouse.y > editor_line_boxes.back().y_max &&
                   row + 1U < editor.lines().size()) {
            ++row;
        }
        const auto cell = static_cast<std::size_t>(std::max(
            0, mouse.x - box.x_min - (editor_line_numbers ? 6 : 0)
        ));
        const auto first_column =
            row == rendered_row
                ? editor_line_first_columns[index]
                : editor_first_visible_column(
                      editor.lines()[row], editor.first_column, editor.tab_width()
                  );
        return EditorMousePosition{
            row,
            editor_offset_for_width(
                editor.lines()[row],
                first_column + cell,
                editor.tab_width()
            ),
            first_column + cell
        };
    }

    void place_editor_mouse(const EditorMousePosition& position) {
        if (config.editor_cursor_beyond_eol) editor.set_display_position(position.row, position.display);
        else editor.set_position({position.row, position.column});
    }

    bool handle_editor(const ftxui::Event& event) {
        if (editor_window_action != 0) return handle_editor_window(event);
        if (macro_recording && (event == ftxui::Event::F10 || event == ftxui::Event::Escape)) {
            macro_recording = false;
        }
        if (!macro_replaying && recordable_editor_key(event)) {
            if (macro_recording) macro_keys.push_back({event.is_character(), event.input()});
        }
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
                         position->column != editor.column() ||
                         (config.editor_cursor_beyond_eol && position->display != editor.display_cursor_column()))) {
                        if (editor_mouse_column) {
                            editor.begin_column_mark();
                        } else {
                            editor.begin_mark();
                        }
                        editor_mouse_marking = true;
                    }
                    place_editor_mouse(*position);
                }
                return true;
            }
            if (mouse.button == ftxui::Mouse::Left &&
                mouse.motion == ftxui::Mouse::Pressed) {
                const auto bounds = editor_window_bounds();
                if (!editor.fullscreen && mouse.y == bounds[1] &&
                    mouse.x >= bounds[0] && mouse.x < bounds[0] + bounds[2]) {
                    show_editor_window(3);
                    editor_window_mouse_drag = true;
                    editor_window_mouse_offset = mouse.x - bounds[0];
                    return true;
                }
                if (!editor.fullscreen && mouse.x == bounds[0] + bounds[2] - 1 &&
                    mouse.y == bounds[1] + bounds[3] - 1) {
                    show_editor_window(4);
                    editor_window_mouse_drag = true;
                    return true;
                }
                if (editor_header_box.Contain(mouse.x, mouse.y) &&
                    mouse.x < ftxui::Terminal::Size().dimx) {
                    editor_menu_category = 0;
                    if (!editor.fullscreen) {
                        for (std::size_t index = 0; index < editor_menu_category_boxes.size(); ++index) {
                            if (editor_menu_category_boxes[index].Contain(mouse.x, mouse.y)) editor_menu_category = index;
                        }
                    }
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
                        editor.resume_mark();
                    } else {
                        editor.clear_selection();
                    }
                    place_editor_mouse(*position);
                    editor_mouse_dragging = true;
                    editor_mouse_marking = mouse.shift;
                    editor_mouse_column = editor.column_selection() || mouse.meta;
                    return true;
                }
            }
            return false;
        }
        if (editor_mouse_dragging) {
            editor.finish_mark();
            editor_mouse_dragging = false;
        }
        if (event == ftxui::Event::AltK || event == ftxui::Event::AltJ ||
            event == ftxui::Event::AltI || event == ftxui::Event::AltO) {
            editor_bookmark_action(event == ftxui::Event::AltK ? 3
                                   : event == ftxui::Event::AltJ ? 4
                                   : event == ftxui::Event::AltI ? 5 : 6);
            return true;
        }
        if (event == ftxui::Event::F11) {
            open_user_menu(Mode::editor);
            return true;
        }
        if (event == ftxui::Event::AltE) {
            show_editor_encoding();
            return true;
        }
        if (event == ftxui::Event::AltM) { show_editor_mail(); return true; }
        if (event == ftxui::Event::CtrlP) { show_spelling(false); return true; }
        if (event == ftxui::Event::CtrlR) {
            toggle_macro_recording();
            return true;
        }
        if (event == ftxui::Event::CtrlQ) {
            show_literal_insert();
            return true;
        }
        if (event == ftxui::Event::AltP || event == ftxui::Event::AltT ||
            event == ftxui::Event::AltU) {
            if (event == ftxui::Event::AltU) show_paste_output();
            else show_editor_transform(event == ftxui::Event::AltP ? 2 : 3);
            return true;
        }
        if (event == ftxui::Event::Special("\x1B[28~")) {
            show_insert_file();
            return true;
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
            editor_menu_dropped = config.drop_menus;
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
                editor_location(editor)
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
            new_editor_buffer();
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
            show_save_block();
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
            editor.tab();
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
            editor.backspace();
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
            mode = user_menu_return;
            return true;
        }
        if (user_menu.empty()) return true;
        if (event == ftxui::Event::ArrowUp || event == ftxui::Event::ArrowDown) {
            user_menu_cursor = event == ftxui::Event::ArrowUp
                ? (user_menu_cursor + user_menu.size() - 1U) % user_menu.size()
                : (user_menu_cursor + 1U) % user_menu.size();
            return true;
        }
        if (event == ftxui::Event::Home || event == ftxui::Event::End) {
            user_menu_cursor = event == ftxui::Event::Home ? 0U : user_menu.size() - 1U;
            return true;
        }
        if (event == ftxui::Event::PageUp || event == ftxui::Event::PageDown) {
            const auto page = static_cast<std::size_t>(std::max(1, ftxui::Terminal::Size().dimy - 10));
            user_menu_cursor = event == ftxui::Event::PageUp
                ? user_menu_cursor - std::min(page, user_menu_cursor)
                : std::min(user_menu.size() - 1U, user_menu_cursor + page);
            return true;
        }
        if (event.is_mouse()) {
            auto copy = event;
            const auto& mouse = copy.mouse();
            if (mouse.motion != ftxui::Mouse::Pressed) return true;
            if (mouse.button == ftxui::Mouse::WheelUp || mouse.button == ftxui::Mouse::WheelDown) {
                return handle_menu(mouse.button == ftxui::Mouse::WheelUp ? ftxui::Event::ArrowUp
                                                                       : ftxui::Event::ArrowDown);
            }
            if (mouse.button != ftxui::Mouse::Left || !user_menu_viewport.Contain(mouse.x, mouse.y)) return true;
            for (std::size_t index = 0; index < user_menu_boxes.size(); ++index) {
                if (user_menu_boxes[index].Contain(mouse.x, mouse.y)) {
                    user_menu_cursor = index;
                    execute_user_menu_entry(user_menu[index]);
                    return true;
                }
            }
            return true;
        }
        if (event != ftxui::Event::Return) {
            if (!event.is_character() || event.character().size() != 1U) return false;
            const auto key = event.character().front();
            const auto match = std::ranges::find(user_menu, key, &UserMenuEntry::key);
            if (match == user_menu.end()) return false;
            user_menu_cursor = static_cast<std::size_t>(match - user_menu.begin());
        }
        user_menu_error.clear();
        execute_user_menu_entry(user_menu[user_menu_cursor]);
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
        if (!pulldown_dropped && event.is_character() &&
            event.character().size() == 1U) {
            const auto key = static_cast<char>(std::tolower(
                static_cast<unsigned char>(event.character()[0])
            ));
            for (std::size_t index = 0; index < top_menu_names.size(); ++index) {
                const auto mnemonic = static_cast<unsigned char>(
                    top_menu_names[index][top_menu_mnemonics[index]]
                );
                if (std::tolower(mnemonic) == key) {
                    pulldown_category = index;
                    reset_pulldown_item();
                    pulldown_dropped = true;
                    return true;
                }
            }
        }
        if (pulldown_dropped && event.is_character() && event.character().size() == 1U) {
            const auto key = std::tolower(static_cast<unsigned char>(event.character()[0]));
            const auto& items = pulldown_items()[pulldown_category];
            for (std::size_t index = 0; index < items.size(); ++index) {
                if (items[index].mnemonic != 0 && items[index].mnemonic == key) {
                    if (items[index].action >= 0) {
                        pulldown_item = index;
                        try { execute_pulldown(); }
                        catch (const std::exception& error) { set_error(error); mode = Mode::panels; }
                    }
                    return true;
                }
            }
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
                        mode = modal_tree_snapshot && tree_operation_source ? Mode::directory_tree_dialog : Mode::panels;
                        clear_tree_operation_context();
                        clear_tree_operation_context();
                    }
                    return true;
                }
                if (!pulldown_dropped || !pulldown_viewport.Contain(mouse.x, mouse.y)) {
                    mode = Mode::panels;
                }
                return true;
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

    bool handle_hotlist_form(const ftxui::Event& event) {
        using namespace ftxui;
        if (modal_cancel(event)) {
            hotlist_form_open = false;
            return true;
        }
        if (event.is_mouse()) {
            auto copy = event;
            const auto& mouse = copy.mouse();
            if (hotlist_label_input.dragging) return input_mouse(hotlist_label, hotlist_label_input, mouse);
            if (hotlist_directory_input.dragging) return input_mouse(hotlist_directory, hotlist_directory_input, mouse);
            if (mouse.button != Mouse::Left || mouse.motion != Mouse::Pressed) return true;
            for (std::size_t index = 0; index < hotlist_form_boxes.size(); ++index) {
                if ((hotlist_form_group && index == 1U) || !hotlist_form_boxes[index].Contain(mouse.x, mouse.y)) continue;
                hotlist_form_focus = index;
                if (index == 0U) return input_mouse(hotlist_label, hotlist_label_input, mouse);
                if (index == 1U) return input_mouse(hotlist_directory, hotlist_directory_input, mouse);
                return handle_hotlist_form(Event::Return);
            }
            return true;
        }
        if (event == Event::Tab || event == Event::TabReverse) {
            do {
                hotlist_form_focus = (hotlist_form_focus + (event == Event::Tab ? 1U : 4U)) % 5U;
            } while (hotlist_form_group && hotlist_form_focus == 1U);
            return true;
        }
        const bool append = event == Event::AltA;
        const bool insert = event == Event::AltI;
        if (append || insert || event == Event::Return ||
            (event == Event::Character(' ') && hotlist_form_focus >= 2U)) {
            if (!append && !insert && hotlist_form_focus == 4U) {
                hotlist_form_open = false;
                return true;
            }
            try {
                insert_hotlist_form(append || (!insert && hotlist_form_focus != 3U));
            } catch (const std::exception& error) {
                hotlist_form_error = error.what();
            }
            return true;
        }
        if (hotlist_form_focus == 0U) return edit_input(hotlist_label, hotlist_label_input, event);
        if (hotlist_form_focus == 1U) return edit_input(hotlist_directory, hotlist_directory_input, event);
        return true;
    }

    bool handle_bookmarks(const ftxui::Event& event) {
        using namespace ftxui;
        if (hotlist_form_open) return handle_hotlist_form(event);
        const bool moving = hotlist_move_source.has_value();
        const std::size_t button_count = moving ? 5U : 8U;
        if (modal_cancel(event)) {
            if (moving) cancel_hotlist_move();
            else mode = Mode::panels;
            return true;
        }
        if (event == Event::Tab || event == Event::TabReverse) {
            hotlist_focus = (hotlist_focus + (event == Event::Tab ? 1U : button_count)) % (button_count + 1U);
            return true;
        }
        if (event.is_mouse()) {
            auto copy = event;
            const auto& mouse = copy.mouse();
            if (mouse.button == Mouse::Left && mouse.motion == Mouse::Pressed) {
                for (std::size_t index = 0; index < button_count; ++index) {
                    if (hotlist_buttons[index].Contain(mouse.x, mouse.y)) {
                        hotlist_focus = index + 1U;
                        activate_hotlist_button(index);
                        return true;
                    }
                }
            }
        }
        if ((event == Event::Return || event == Event::Character(' ')) && hotlist_focus != 0U) {
            activate_hotlist_button(hotlist_focus - 1U);
            return true;
        }
        if (event == Event::ArrowLeft || event == Event::ArrowRight) {
            if (hotlist_focus != 0U) {
                hotlist_focus = 1U + (hotlist_focus - 1U +
                    (event == Event::ArrowRight ? 1U : button_count - 1U)) % button_count;
            } else if (event == Event::ArrowLeft) {
                hotlist_up();
            } else {
                const auto selected = selected_hotlist_item();
                if (!selected || hotlist.group(hotlist_group)[selected->back()].kind == HotlistKind::group) {
                    change_hotlist_directory();
                }
            }
            return true;
        }
        if (!moving && (event == Event::Insert || event == Event::AltA)) {
            add_current_hotlist(Mode::bookmarks);
            return true;
        }
        if (!moving && event == Event::Delete) {
            confirm_hotlist_remove();
            return true;
        }
        if (event.is_character() && event.character().size() == 1U) {
            const auto key = static_cast<char>(std::tolower(static_cast<unsigned char>(event.character()[0])));
            if (moving) {
                if (key == 'i') { finish_hotlist_move(false); return true; }
                if (key == 'a') { finish_hotlist_move(true); return true; }
            } else {
                if (key == 'a') { add_current_hotlist(Mode::bookmarks); return true; }
                if (key == 'g') { start_hotlist_form(true); return true; }
                if (key == 'e') { start_hotlist_form(false); return true; }
                if (key == 'r') { confirm_hotlist_remove(); return true; }
                if (key == 'm') { activate_hotlist_button(7); return true; }
            }
            if (key == 'u') { hotlist_up(); return true; }
            if (key == 'c') { change_hotlist_directory(); return true; }
        }
        const auto input = handle_list_input(event, bookmark_cursor, hotlist_rows().size(),
                                             bookmark_boxes, bookmark_list_box);
        if (input == ButtonInput::activate) change_hotlist_directory();
        if (input == ButtonInput::changed) hotlist_focus = 0;
        return true;
    }

    bool handle_search_results(const ftxui::Event& event) {
        bool panelize = event == ftxui::Event::AltP;
        if (event.is_mouse()) {
            auto copy = event;
            panelize = copy.mouse().button == ftxui::Mouse::Left &&
                       copy.mouse().motion == ftxui::Mouse::Pressed &&
                       search_panelize_box.Contain(copy.mouse().x, copy.mouse().y);
        }
        if (panelize) {
            auto results = search_results;
            for (auto& entry : results) {
                const auto relative = std::filesystem::path(entry.path).lexically_relative(active().path());
                entry.name = relative.empty() ? entry.path : relative.string();
            }
            active().set_panelized(std::move(results));
            mode = Mode::panels;
            status = "Find results panelized";
            return true;
        }
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
                        view_file_with_extensions(provider, result.path, Mode::search_results);
                    } else {
                        edit_file_with_extensions(provider, result.path, Mode::search_results);
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
                static_cast<int>(std::min<std::size_t>(dialog_focus, 6U)) + amount,
                0, 6
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
                    if ((index < 7U && !operation_dialog_viewport.Contain(mouse.x, mouse.y)) ||
                        !operation_dialog_boxes[index].Contain(mouse.x, mouse.y)) {
                        continue;
                    }
                    dialog_focus = index;
                    if (index == 0U) {
                        input_mouse(dialog_source_mask, mask_input, mouse);
                    } else if (index == 2U) {
                        input_mouse(dialog_destination, destination_input, mouse);
                    } else if (index < 7U) {
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
              event == ftxui::Event::Character(' ')) && dialog_focus == 9U)) {
            mode = modal_tree_snapshot && tree_operation_source ? Mode::directory_tree_dialog : Mode::panels;
            status = "Operation cancelled";
            clear_tree_operation_context();
            return true;
        }
        const auto mnemonic = [&](const char key) {
            return event == ftxui::Event::Special(std::string("\x1B") + key) ||
                   ((dialog_focus != 0U && dialog_focus != 2U) &&
                    event == ftxui::Event::Character(key));
        };
        for (const auto& [key, index] : std::array<std::pair<char, std::size_t>, 5>{
                 {{'u', 1U}, {'l', 3U}, {'p', 4U}, {'d', 5U}, {'s', 6U}}}) {
            if (mnemonic(key)) {
                dialog_focus = index;
                return handle_operation_dialog(ftxui::Event::Character(' '));
            }
        }
        if (mnemonic('o') || mnemonic('b') || mnemonic('c')) {
            dialog_focus = mnemonic('c') ? 9U : mnemonic('b') ? 8U : 7U;
            return handle_operation_dialog(ftxui::Event::Return);
        }
        if (event == ftxui::Event::Tab) {
            dialog_focus = (dialog_focus + 1U) % 10U;
            return true;
        }
        if (event == ftxui::Event::TabReverse) {
            dialog_focus = (dialog_focus + 9U) % 10U;
            return true;
        }
        if (event == ftxui::Event::ArrowDown) {
            dialog_focus = std::min<std::size_t>(
                9U,
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
                dialog_focus == 1U ||
                dialog_focus == 3U ||
                dialog_focus == 4U ||
                dialog_focus == 5U ||
                dialog_focus == 6U
            )) {
            if (dialog_focus == 1U) {
                dialog_shell_patterns = !dialog_shell_patterns;
            } else if (dialog_focus == 4U) {
                dialog_preserve_attributes =
                    !dialog_preserve_attributes;
            } else if (dialog_focus == 3U) {
                dialog_preserve_symlinks =
                    !dialog_preserve_symlinks;
            } else if (dialog_focus == 5U) {
                dialog_dive_into_subdirectories = !dialog_dive_into_subdirectories;
            } else {
                dialog_stable_symlinks = !dialog_stable_symlinks;
            }
            return true;
        }
        if (event == ftxui::Event::Return ||
            (event == ftxui::Event::Character(' ') && (dialog_focus == 7U || dialog_focus == 8U))) {
            try {
                remember_input(destination_input, dialog_destination);
                remember_input(mask_input, dialog_source_mask);
                operation_input_error.clear();
            } catch (const std::exception& error) {
                operation_input_error = error.what();
                return true;
            }
            try {
                dialog_background = dialog_focus == 8U;
                confirm_operation_dialog();
            } catch (const std::exception& error) {
                operation_input_error = error.what();
            }
            return true;
        }
        if (dialog_focus == 0U || dialog_focus == 2U) {
            if (event == ftxui::Event::Special("\x1B\t")) {
                try {
                    if (dialog_focus == 2U) {
                        complete_path(dialog_destination, destination_input, active().path(), active().provider());
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
                dialog_focus == 2U ? dialog_destination : dialog_source_mask,
                dialog_focus == 2U ? destination_input : mask_input,
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
            mode = modal_tree_snapshot && tree_operation_source ? Mode::directory_tree_dialog : Mode::panels;
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
                mode = modal_tree_snapshot && tree_operation_source ? Mode::directory_tree_dialog : Mode::panels;
                status = "Delete cancelled";
                clear_tree_operation_context();
            }
            return true;
        }
        return input != ButtonInput::ignored;
    }

    bool handle_operation_progress(const ftxui::Event& event) {
        const auto job = progress_job;
        if (!job) { mode = Mode::panels; return true; }
        const auto background = [&] {
            job->background = true;
            mode = progress_return_mode == Mode::directory_tree_dialog && !modal_tree_snapshot
                ? Mode::panels : progress_return_mode;
        };
        const auto activate = [&](const std::size_t action) {
            if (job->finished || action == 3U) background();
            else if (action == 0U) {
                status = job->control.toggle_pause() ? "Operation paused" : "Operation resumed";
            } else if (action == 1U && job->kind != OperationJob::Kind::remove) {
                job->control.skip();
                status = "Skipping current entry";
            } else if (action == 2U) {
                request_operation_cancel(job);
                status = "Cancelling operation";
            }
        };
        if (event.is_mouse()) {
            auto copy = event;
            const auto& mouse = copy.mouse();
            if (mouse.button == ftxui::Mouse::Left && mouse.motion == ftxui::Mouse::Pressed) {
                for (std::size_t index = 0; index < operation_progress_boxes.size(); ++index) {
                    if (operation_progress_boxes[index].Contain(mouse.x, mouse.y)) {
                        operation_progress_focus = index;
                        activate(index);
                        break;
                    }
                }
            }
            return true;
        }
        if (event == ftxui::Event::Tab || event == ftxui::Event::TabReverse) {
            do {
                operation_progress_focus = event == ftxui::Event::Tab
                    ? (operation_progress_focus + 1U) % 4U
                    : (operation_progress_focus + 3U) % 4U;
            } while ((job->finished && operation_progress_focus != 3U) ||
                     (operation_progress_focus == 1U && job->kind == OperationJob::Kind::remove));
        } else if (modal_cancel(event) || event == ftxui::Event::CtrlC ||
                   event == ftxui::Event::Character('a') || event == ftxui::Event::F8) {
            activate(job->finished ? 3U : 2U);
        } else if (event == ftxui::Event::Character('p') || event == ftxui::Event::F2 ||
                   event == ftxui::Event::Character(' ')) {
            activate(0U);
        } else if (event == ftxui::Event::Character('s')) {
            activate(1U);
        } else if (event == ftxui::Event::Character('b')) {
            activate(3U);
        } else if (event == ftxui::Event::Return) {
            activate(operation_progress_focus);
        } else if (event == ftxui::Event::CtrlO) {
            background();
            try {
                const auto result = run_process("", true);
                status = shell.running() ? "Shell hidden; Ctrl-O resumes it"
                    : "Subshell exited " + std::to_string(result);
            } catch (const std::exception& error) { set_error(error); }
        } else {
            return false;
        }
        return true;
    }

    bool handle_conflict_dialog(const ftxui::Event& event) {
        const bool error = mode == Mode::operation_error_dialog;
        const auto choices = operation_choice_buttons();
        if (!error && (event == ftxui::Event::Character('l') || event == ftxui::Event::Character('e'))) {
            answer_operation(event == ftxui::Event::Character('l')
                ? OperationChoice::overwrite_all : OperationChoice::skip_all);
            return true;
        }
        std::string shortcuts;
        for (const auto& choice : choices) shortcuts += choice.shortcut;
        const auto input = handle_button_input(
            event, dialog_focus,
            std::span<const ftxui::Box>(operation_choice_boxes).first(choices.size()),
            shortcuts
        );
        if (input == ButtonInput::cancel) {
            answer_operation(OperationChoice::cancel);
            return true;
        }
        if (input == ButtonInput::activate) {
            answer_operation(choices[dialog_focus].choice);
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
            if (job_cursor >= jobs.size()) {
                status = "No job selected";
                return true;
            }
            const auto job = jobs[job_cursor];
            if (event == ftxui::Event::F2) {
                if (job->finished) status = "No running job selected";
                else status = job->control.toggle_pause() ? "Operation paused" : "Operation resumed";
            } else if (event == ftxui::Event::F8) {
                request_operation_cancel(job);
            } else {
                progress_job = job;
                job->background = false;
                progress_return_mode = Mode::jobs;
                operation_progress_focus = job->finished ? 3U : 0U;
                mode = Mode::operation_progress;
                service_operation_prompts();
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
                 command_prompt_shown() &&
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
                            panel_search_case_sensitive(),
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
                            panel_search_case_sensitive(),
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
                        panel_search_case_sensitive(),
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
                } else if (event == ftxui::Event::h) {
                    show_hotlist(true);
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
                } else if (event == ftxui::Event::CtrlD) {
                    show_file_comparison();
                } else if (event == ftxui::Event::CtrlS) {
                    show_edit_symlink();
                } else if (event == ftxui::Event::e) {
                    show_file_attributes();
                } else if (event == ftxui::Event::j) {
                    open_jobs();
                } else if (event == ftxui::Event::a) {
                    show_vfs_list();
                } else if (
                    event == ftxui::Event::Character('!')
                ) {
                    show_external_panelize();
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
        if (listing_focused) {
            try {
                if (event == ftxui::Event::Special("\x1B\t") && command_prompt_shown()) {
                    command_mode = true;
                    complete_path(command_line, command_input, active().navigation_path(),
                                  active().provider(), true);
                    status = command_input.message.empty() ? "Completed" : command_input.message;
                    return true;
                }
                if (event == ftxui::Event::AltH) {
                    show_command_history();
                    return true;
                }
                if (event == ftxui::Event::AltE) {
                    show_filename_encoding();
                    return true;
                }
                if (event == ftxui::Event::Special("\x1B" "E")) {
                    show_file_history();
                    return true;
                }
                if (event == ftxui::Event::Special("\x1B!")) {
                    show_filtered_view();
                    return true;
                }
                if (event == ftxui::Event::Special("\x1B[32;5u")) {
                    show_directory_sizes();
                    return true;
                }
            } catch (const std::exception& error) {
                set_error(error);
                return true;
            }
        }
        const bool command_navigation =
            event == ftxui::Event::ArrowLeft ||
            event == ftxui::Event::ArrowRight ||
            event == ftxui::Event::Home ||
            event == ftxui::Event::End ||
            event == ftxui::Event::CtrlA;
        if (!command_prompt_shown() && listing_focused && event.is_character() &&
            !selection_shortcut && !panel_search_confirmed) {
            if (!panel_search_mode) panel_search.clear();
            panel_search_mode = true;
            panel_search += event.character();
            active().find(panel_search, panel_search_case_sensitive(), true, true);
            status = "Quick search: " + panel_search;
            return true;
        }
        if (!command_mode && listing_focused &&
            (event == ftxui::Event::AltP || event == ftxui::Event::AltN) &&
            !command_input.history.empty()) {
            command_prompt_override = !config.command_prompt;
            command_mode = true;
            command_input.reset(command_line);
        }
        if (command_mode && listing_focused &&
            (!panel_shortcut || command_navigation) &&
            !panel_search_confirmed) {
            if (event == ftxui::Event::Escape) {
                command_prompt_override = false;
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
                        if (config.menubar_visible && top_menu_boxes[index].Contain(
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
                    if (mouse.button == ftxui::Mouse::WheelUp || mouse.button == ftxui::Mouse::WheelDown) {
                        const auto direction = mouse.button == ftxui::Mouse::WheelUp ? -1 : 1;
                        if (config.viewer_mouse_page_scrolling.value_or(false)) {
                            quick_viewer.page(direction, viewport_width, quick_view_dimensions().second);
                        } else {
                            quick_viewer.scroll_lines(direction * (config.viewer_mouse_page_scrolling ? 1 : 3), viewport_width);
                        }
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
                const bool overlap = !config.layout_horizontal &&
                                     (listing_full_width(0) || listing_full_width(1));
                const bool left_hit = left_box.Contain(mouse.x, mouse.y) &&
                                      (!overlap || active_index == 0 || !right_box.Contain(mouse.x, mouse.y));
                const bool right_hit = right_box.Contain(mouse.x, mouse.y) &&
                                       (!overlap || active_index == 1 || !left_box.Contain(mouse.x, mouse.y));
                if (
                    quick_view_panel != 0 &&
                    information_panel != 0 &&
                    tree_panel != 0 &&
                    left_hit
                ) {
                    pane_index = 0;
                    pane = &left;
                    boxes = &left_entry_boxes;
                } else if (
                    quick_view_panel != 1 &&
                    information_panel != 1 &&
                    tree_panel != 1 &&
                    right_hit
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
                    if (config.mouse_page_scrolling && pane->first_row() > 0U) {
                        pane->page_cursor(-static_cast<int>(panel_page_rows(*pane)));
                    } else pane->move_cursor(-1);
                    return true;
                }
                if (pane != nullptr &&
                    mouse.button == ftxui::Mouse::WheelDown) {
                    active_index = pane_index;
                    quick_view_focused = false;
                    information_panel_focused = false;
                    tree_panel_focused = false;
                    if (config.mouse_page_scrolling && pane->first_row() + panel_page_rows(*pane) < pane->entries().size()) {
                        pane->page_cursor(static_cast<int>(panel_page_rows(*pane)));
                    } else pane->move_cursor(1);
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
                                activate_file_association();
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
                if (event == ftxui::Event::F5) {
                    viewer_goto_quick = true;
                    viewer_goto_mode = quick_viewer.hex() ? ViewerGotoMode::hexadecimal : ViewerGotoMode::line;
                    start_prompt(PromptAction::viewer_goto, "Go to", "");
                    return true;
                }
                if (event == ftxui::Event::F7) { show_file_search(FileSearchTarget::quick_view); return true; }
                if (event == ftxui::Event::F8) { quick_viewer.toggle_magic(); return true; }
                if (event == ftxui::Event::AltE) {
                    show_codepage_chooser(quick_viewer.encoding(), Mode::panels,
                        [this](const std::string& value) { quick_viewer.set_encoding(value); });
                    return true;
                }
                if (event == ftxui::Event::F6 ||
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
                        config.viewer_wrap = quick_viewer.wrap();
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
                    refresh(true, true);
                    return true;
                }
                if (event == ftxui::Event::F3) {
                    tree_refresh_pending.reset();
                    tree_refresh_notify_pending = false;
                    forget_tree_branch(tree_cursor);
                    sync_dynamic_tree();
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
                        tree_cursor > panel_visible_rows(*tree_panel)
                            ? tree_cursor - panel_visible_rows(*tree_panel)
                            : 0U;
                    update_tree_selection();
                    return true;
                }
                if (event == ftxui::Event::PageDown) {
                    tree_cursor = std::min(
                        tree_nodes.empty()
                            ? 0U
                            : tree_nodes.size() - 1U,
                        tree_cursor + panel_visible_rows(*tree_panel)
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
                if (pane_listing_spec(active()).columns > 1U) {
                    active().move_cursor(
                        (event == ftxui::Event::ArrowLeft ? -1 : 1) *
                        static_cast<int>(panel_visible_rows())
                    );
                } else if (config.navigate_with_arrows) {
                    if (event == ftxui::Event::ArrowLeft) static_cast<void>(panel_up(active()));
                    else static_cast<void>(enter_panel_entry());
                }
                return true;
            } else if (
                event == ftxui::Event::Special("\x1B[5;5~")
            ) {
                static_cast<void>(panel_up(active()));
            } else if (
                event == ftxui::Event::Special("\x1B[6;5~")
            ) {
                static_cast<void>(enter_panel_entry());
            } else if (event == ftxui::Event::PageDown) {
                active().page_cursor(
                    static_cast<int>(panel_page_rows(active()))
                );
            } else if (event == ftxui::Event::PageUp) {
                active().page_cursor(
                    -static_cast<int>(panel_page_rows(active()))
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
                if (panel_search_confirmed) static_cast<void>(enter_panel_entry(false));
                else activate_file_association();
            } else if (event == ftxui::Event::AltY) {
                active().history_back();
            } else if (event == ftxui::Event::AltU) {
                active().history_forward();
            } else if (event == ftxui::Event::Insert) {
                active().toggle_selected(config.mark_moves_down ? 1 : 0);
            } else if (event == ftxui::Event::CtrlS ||
                       event == ftxui::Event::AltS) {
                panel_search_mode = true;
                panel_search.clear();
                status = "Quick search: ";
            } else if (event == ftxui::Event::CtrlA) {
                active().select_all();
            } else if (event == ftxui::Event::CtrlR) {
                refresh(true, true);
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
            } else if (event == ftxui::Event::CtrlC && has_running_jobs()) {
                const auto count = std::ranges::count_if(jobs, [](const auto& job) { return !job->finished; });
                if (count == 1) {
                    const auto running = std::ranges::find_if(jobs, [](const auto& job) { return !job->finished; });
                    request_operation_cancel(*running);
                    status = "Cancelling operation";
                } else {
                    open_jobs();
                    status = "Select a job to abort with F8";
                }
            } else if (event == ftxui::Event::CtrlC &&
                       traversal_running.load(
                           std::memory_order_acquire
                       )) {
                traversal_cancellation.request();
                status = "Cancelling search";
            } else if (event == ftxui::Event::CtrlO) {
                const auto result = run_process("", true);
                status = shell.running() ? "Shell hidden; Ctrl-O resumes it"
                                         : "Subshell exited " + std::to_string(result);
            } else if (event == ftxui::Event::F2) {
                open_user_menu(Mode::panels);
            } else if (event == ftxui::Event::F3 ||
                       event == ftxui::Event::Special("\x1B[1;2R") ||
                       event == ftxui::Event::Special("\x1B[25~")) {
                const auto* entry = active().current();
                if (entry != nullptr && can_open_file(*entry)) {
                    view_file_with_extensions(active().provider(), entry->path, Mode::panels,
                                              event != ftxui::Event::F3);
                }
            } else if (event == ftxui::Event::F4) {
                const auto* entry = active().current();
                request_main_edit(entry);
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
                    "New directory",
                    mkdir_default()
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
                pulldown_dropped = config.drop_menus;
                mode = Mode::pulldown;
            } else if (event == ftxui::Event::F10) {
                if (traversal_running.load(
                        std::memory_order_acquire
                    )) {
                    traversal_cancellation.request();
                    status = "Cancel search before quitting";
                    return true;
                }
                request_app_exit();
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

    std::optional<std::string_view> lookup_keymap_action(
        const KeymapContext context,
        const ftxui::Event& event
    ) const {
        if (!keymap) return std::nullopt;
        const auto section = keymap->sections().find(context);
        if (section == keymap->sections().end()) return std::nullopt;
        for (const auto& definition : section->second) {
            for (const auto& key : definition.keys) {
                if (keymap_event_matches(event, key)) return definition.action;
            }
        }
        return std::nullopt;
    }

    std::optional<ftxui::Event> remap_widget_key(
        const KeymapContext context,
        const ftxui::Event& event
    ) const {
        if (!keymap || keymap_dispatching) return event;
        const auto key = keymap_event_name(event);
        if (!key) return event;
        if (const auto action = lookup_keymap_action(context, event)) {
            for (const auto& binding : original_keymap_bindings()) {
                if (binding.context == context && binding.action == *action) {
                    return keymap_named_event(binding.keys.substr(0, binding.keys.find(';')));
                }
            }
            throw std::runtime_error("keymap action " + std::string(*action) +
                " is not available in " + std::string(keymap_context_name(context)));
        }
        for (const auto& binding : original_keymap_bindings()) {
            if (binding.context == context && binding_has_key(binding.keys, *key)) {
                if (keymap->contains_action(context, binding.action)) return std::nullopt;
                break;
            }
        }
        return event;
    }

    std::vector<KeymapContext> keymap_contexts() const {
        using enum KeymapContext;
        if (help_browser) return {help};
        switch (mode) {
            case Mode::panels:
                if (tree_panel && tree_panel_focused) return {tree, filemanager};
                if (quick_view_panel && quick_view_focused) {
                    return quick_viewer.hex() ? std::vector{viewer_hex, viewer, filemanager}
                                              : std::vector{viewer, filemanager};
                }
                return command_mode ? std::vector{filemanager, input}
                                    : std::vector{filemanager, panel};
            case Mode::editor: return {editor};
            case Mode::viewer:
                return this->viewer.hex() ? std::vector{viewer_hex, viewer} : std::vector{viewer};
            case Mode::diff: return {diffviewer};
            case Mode::directory_tree_dialog: return {tree, dialog};
            case Mode::editor_menu:
            case Mode::pulldown:
            case Mode::user_menu: return {menu, dialog};
            case Mode::local_list:
            case Mode::editor_history:
            case Mode::editor_buffer_list:
            case Mode::bookmarks:
            case Mode::search_results:
            case Mode::jobs: return {listbox, dialog};
            case Mode::chattr_dialog: return {chattr, dialog};
            case Mode::prompt:
                return prompt_focus == 0U ? std::vector{input, dialog} : std::vector{dialog};
            case Mode::native_input:
                return native_input_focus == 0U ? std::vector{input, dialog} : std::vector{dialog};
            case Mode::operation_dialog:
                return dialog_focus < 2U ? std::vector{input, dialog} : std::vector{dialog};
            case Mode::editor_mail_dialog:
                return mail_focus < 3U ? std::vector{input, dialog} : std::vector{dialog};
            case Mode::file_search_dialog:
                if (file_search_focus == 0U) return {input, dialog};
                if (file_search_focus == 1U) return {radio, dialog};
                return {dialog};
            case Mode::editor_search_dialog:
            case Mode::editor_replace_dialog:
                if (editor_dialog_focus == 0U ||
                    (mode == Mode::editor_replace_dialog && editor_dialog_focus == 1U)) return {input, dialog};
                if (editor_dialog_focus == 2U) return {radio, dialog};
                return {dialog};
            case Mode::editor_save_mode_dialog:
                if (save_mode_focus == 0U) return {radio, dialog};
                return save_mode_focus == 1U ? std::vector{input, dialog} : std::vector{dialog};
            case Mode::editor_options_dialog:
                if (editor_options_focus == 0U) return {radio, dialog};
                if (editor_options_focus == 4U || editor_options_focus == 15U) return {input, dialog};
                return {dialog};
            case Mode::listing_dialog:
                if (listing_focus == 0U) return {radio, dialog};
                if (listing_focus == 1U || listing_focus == 2U || listing_focus == 4U) return {input, dialog};
                return {dialog};
            case Mode::sort_dialog: return {radio, dialog};
            case Mode::diff_options:
                return diff_options_focus == 0U ? std::vector{radio, dialog} : std::vector{dialog};
            case Mode::configure_dialog:
                if (configure_focus == 18U) return {input, dialog};
                if (configure_focus == 19U) return {radio, dialog};
                return {dialog};
            default: return {dialog};
        }
    }

    void execute_keymap_action(const KeymapContext context, std::string_view action) {
        using enum KeymapContext;
        if (help_browser) {
            if (!handle_help_action(action)) {
                throw std::runtime_error("keymap action " + std::string(action) + " is not available in help");
            }
            return;
        }
        if (action == "Help") {
            open_context_help();
            return;
        }
        if (mode == Mode::panels && action == "ViewRaw") {
            const auto* entry = active().current();
            if (entry != nullptr && can_open_file(*entry)) {
                view_file_with_extensions(active().provider(), entry->path, Mode::panels, true);
            }
            return;
        }
        if (action == "ExtendedKeyMap") {
            if (mode != Mode::panels && mode != Mode::editor) {
                throw std::runtime_error("this screen has no extended keymap");
            }
            keymap_prefix = mode == Mode::editor ? editor_xmap : filemanager_xmap;
            keymap_prefix_mode = mode;
            panel_ctrl_x = false;
            panel_search_mode = false;
            tree_search_mode = false;
            status = "Extended keymap";
            return;
        }
        struct DispatchGuard {
            bool& flag;
            bool previous;
            explicit DispatchGuard(bool& value) : flag(value), previous(std::exchange(value, true)) {}
            ~DispatchGuard() { flag = previous; }
        } guard(keymap_dispatching);
        if ((mode == Mode::panels || mode == Mode::viewer || mode == Mode::editor || mode == Mode::diff) &&
            (action == "ScreenList" || action == "ScreenNext" || action == "ScreenPrev")) {
            if (action == "ScreenList") show_screens(mode);
            else switch_screen(action == "ScreenNext");
            return;
        }
        if (action == "Close" && mode == Mode::editor) { request_editor_close(); return; }
        if (action == "QuitQuiet" && mode == Mode::panels) {
            if (traversal_running.load(std::memory_order_acquire)) {
                static_cast<void>(handle_panels(ftxui::Event::F10));
            } else request_app_exit(true);
            return;
        }
        if (action == "Close" || action == "QuitQuiet") action = "Quit";
        if (action == "Refresh") action = "Reread";
        if (action == "MenuLastSelected" && mode == Mode::panels) {
            pulldown_dropped = true;
            mode = Mode::pulldown;
            return;
        }
        if ((context == panel || context == tree || context == listbox) && action == "Home") action = "Top";
        if ((context == panel || context == tree || context == listbox) && action == "End") action = "Bottom";
        if ((context == dialog || context == listbox || context == menu) &&
            (action == "Enter" || action == "Ok" || (context != dialog && action == "Select"))) {
            static_cast<void>(handle(ftxui::Event::Return));
            return;
        }
        if (action == "Cancel") {
            static_cast<void>(handle(ftxui::Event::Escape));
            return;
        }
        auto targets = std::vector{context};
        if (context == filemanager || context == filemanager_xmap || context == panel) {
            targets = {context, filemanager, panel, filemanager_xmap};
        } else if (context == editor_xmap) targets.push_back(editor);
        else if (context == viewer_hex) targets.push_back(viewer);
        for (const auto target : targets) {
            for (const auto& binding : original_keymap_bindings()) {
                if (binding.context != target || binding.action != action) continue;
                if (binding.context == filemanager_xmap) {
                    panel_ctrl_x = true;
                    panel_search_mode = false;
                    tree_search_mode = false;
                }
                static_cast<void>(handle(keymap_named_event(
                    binding.keys.substr(0, binding.keys.find(';'))
                )));
                return;
            }
        }
        if (mode == Mode::editor) {
            struct MenuCommand { std::string_view action; std::size_t category; int item; };
            static constexpr MenuCommand commands[]{
                {"EditFile", 0, 0}, {"EditNew", 0, 1}, {"Close", 0, 2},
                {"Save", 0, 3}, {"SaveAs", 0, 4}, {"History", 0, 5}, {"Quit", 0, 6},
                {"InsertFile", 0, 7}, {"BlockSave", 0, 8}, {"UserMenu", 0, 9},
                {"Undo", 1, 0}, {"Redo", 1, 1}, {"InsertOverwrite", 1, 2},
                {"Mark", 1, 3}, {"MarkColumn", 1, 4}, {"MarkAll", 1, 5}, {"Unmark", 1, 6},
                {"Copy", 1, 7}, {"Move", 1, 8}, {"Delete", 1, 9},
                {"Store", 1, 10}, {"Cut", 1, 11}, {"Paste", 1, 12},
                {"Top", 1, 13}, {"Bottom", 1, 14},
                {"Search", 2, 0}, {"SearchContinue", 2, 1}, {"Replace", 2, 2},
                {"Bookmark", 2, 3}, {"BookmarkNext", 2, 4}, {"BookmarkPrev", 2, 5}, {"BookmarkFlush", 2, 6},
                {"Goto", 3, 0}, {"MatchBracket", 3, 1}, {"ShowNumbers", 3, 2}, {"SyntaxOnOff", 3, 3},
                {"SelectCodepage", 3, 7}, {"Reread", 3, 8}, {"MacroStartStopRecord", 3, 9},
                {"MacroDelete", 3, 10}, {"RepeatStartStopRecord", 3, 11}, {"Mail", 3, 12},
                {"SpellCheck", 3, 13}, {"SpellCheckCurrentWord", 3, 14}, {"SpellCheckSelectLang", 3, 15},
                {"InsertLiteral", 4, 0}, {"Date", 4, 1}, {"ParagraphFormat", 4, 2}, {"ExternalCommand", 4, 4},
                {"WindowNext", 5, 0}, {"WindowPrev", 5, 1},
                {"WindowList", 5, 2}, {"WindowMove", 5, 3}, {"WindowResize", 5, 4}, {"WindowFullscreen", 5, 5},
                {"Options", 6, 0}, {"SaveSetup", 6, 1}, {"OptionsSaveMode", 6, 2},
                {"LearnKeys", 6, 3}, {"SyntaxChoose", 6, 4},
            };
            for (const auto& command : commands) {
                if (command.action != action) continue;
                const auto& items = editor_menu_items()[command.category];
                const auto found = std::ranges::find(items, command.item, &MenuRow::action);
                if (found == items.end()) throw std::runtime_error("editor keymap action has no menu command");
                editor_menu_category = command.category;
                editor_menu_item = static_cast<std::size_t>(found - items.begin());
                execute_editor_menu_item();
                return;
            }
        }
        if (mode == Mode::panels) {
            if (traversal_running.load(std::memory_order_acquire)) {
                status = "Search running; Ctrl-C cancels";
                return;
            }
            struct MenuCommand { std::string_view action; std::size_t category; int item; };
            static constexpr MenuCommand commands[]{
                {"ViewFile", 1, 18}, {"ViewFiltered", 1, 22}, {"ChangeMode", 1, 8},
                {"ChangeOwn", 1, 9}, {"ChangeAttributes", 1, 21},
                {"Link", 1, 11}, {"LinkSymbolic", 1, 12}, {"LinkSymbolicRelative", 1, 13},
                {"LinkSymbolicEdit", 1, 19}, {"Select", 1, 15}, {"Unselect", 1, 16}, {"SelectInvert", 1, 17},
                {"Find", 2, 0}, {"CompareDirs", 2, 1}, {"CdQuick", 2, 2},
                {"HotList", 2, 7}, {"Jobs", 2, 10},
                {"CompareFiles", 2, 12}, {"ExternalPanelize", 2, 13}, {"DirSize", 2, 14},
                {"History", 2, 15}, {"EditorViewerHistory", 2, 16}, {"ScreenList", 2, 17},
                {"EditExtensionsFile", 2, 18}, {"EditFileHighlightFile", 2, 20},
                {"Options", 3, 20}, {"OptionsLayout", 3, 21}, {"OptionsPanel", 3, 22},
                {"OptionsConfirm", 3, 23}, {"OptionsDisplayBits", 3, 25}, {"OptionsAppearance", 3, 24},
                {"LearnKeys", 3, 26}, {"SaveSetup", 3, 3},
            };
            for (const auto& command : commands) {
                if (command.action == action) {
                    const auto& items = pulldown_items()[command.category];
                    if (std::ranges::find(items, command.item, &MenuRow::action) == items.end()) {
                        throw std::runtime_error("keymap action has no available menu command: " + std::string(action));
                    }
                    execute_pulldown_action(command.category, command.item);
                    return;
                }
            }
            if (action == "PanelOtherSync") {
                install_pane_location(passive(), {active().provider(), active().path()});
                return;
            }
            if (action == "PanelListing" || action == "SetupListingFormat") { show_listing_options(); return; }
            if (action == "VfsList") { show_vfs_list(); return; }
            if (action == "OptionsVfs") { show_vfs_options(); return; }
            if (action == "Sort") { show_sort_options(); return; }
            if (action == "Filter") { start_prompt(PromptAction::panel_filter, "Panel filter", active().filter()); return; }
            if (action == "SortReverse") {
                active().set_sort_options(active().sort_field(), !active().descending(),
                    active().sort_case_sensitive(), active().executable_first());
                return;
            }
            if (action == "SortByName" || action == "SortByExt" || action == "SortBySize" ||
                action == "SortByMTime" || action == "SortByVersion") {
                sort_active(action == "SortByExt" ? SortField::extension : action == "SortBySize" ? SortField::size :
                    action == "SortByMTime" ? SortField::modified : action == "SortByVersion" ? SortField::version : SortField::name);
                return;
            }
        }
        throw std::runtime_error("keymap action " + std::string(action) +
            " is not available in " + std::string(keymap_context_name(context)));
    }

    bool dispatch_keymap(const ftxui::Event& event) {
        if (!keymap || event.is_mouse()) return false;
        const auto key = keymap_event_name(event);
        if (!key) return false;
        if (help_browser) {
            if (const auto action = lookup_keymap_action(KeymapContext::help, event)) {
                execute_keymap_action(KeymapContext::help, *action);
                return true;
            }
            for (const auto& binding : original_keymap_bindings()) {
                if (binding.context == KeymapContext::help && binding_has_key(binding.keys, *key)) {
                    return keymap->contains_action(KeymapContext::help, binding.action);
                }
            }
            return false;
        }
        if (keymap_prefix && keymap_prefix_mode != mode) keymap_prefix.reset();
        if (keymap_prefix || (mode == Mode::panels && panel_ctrl_x)) {
            const auto context = keymap_prefix.value_or(KeymapContext::filemanager_xmap);
            keymap_prefix.reset();
            panel_ctrl_x = false;
            status = "Ready";
            if (*key == "esc") return true;
            if (const auto action = lookup_keymap_action(context, event)) {
                execute_keymap_action(context, *action);
                return true;
            }
            for (const auto& binding : original_keymap_bindings()) {
                if (binding.context == context && binding_has_key(binding.keys, *key) &&
                    !keymap->contains_action(context, binding.action)) {
                    execute_keymap_action(context, binding.action);
                    return true;
                }
            }
            status = "No action for extended key " + *key;
            return true;
        }
        if ((panel_search_mode || tree_search_mode) && mode == Mode::panels &&
            (event.is_character() || event == ftxui::Event::Backspace ||
             event == ftxui::Event::Return || event == ftxui::Event::Escape)) return false;
        if (mode == Mode::viewer && viewer.hex_editing() && event.is_character()) return false;
        if (mode == Mode::native_key_query || mode == Mode::learn_key || mode == Mode::learn_keys_dialog ||
            (mode == Mode::editor && editor_window_action != 0)) return false;
        const auto contexts = keymap_contexts();
        for (const auto context : contexts) {
            if (const auto action = lookup_keymap_action(context, event)) {
                execute_keymap_action(context, *action);
                return true;
            }
        }
        for (const auto context : contexts) {
            for (const auto& binding : original_keymap_bindings()) {
                if (binding.context == context && binding_has_key(binding.keys, *key)) {
                    return keymap->contains_action(context, binding.action);
                }
            }
        }
        return false;
    }

    bool handle_input(const ftxui::Event& event) try {
        archives.prune();
        if (event == ftxui::Event::Custom && !event.is_character()) return false;
        if (!config.single_escape && mode != Mode::native_key_query && mode != Mode::learn_key &&
            mode != Mode::learn_keys_dialog) {
            if (event == ftxui::Event::Escape && !escape_waiting) { escape_waiting = true; return true; }
            escape_waiting = false;
        }
        if (macro_replaying) {
            if (event == ftxui::Event::Escape || event == ftxui::Event::CtrlC) {
                macro_replaying = false;
                status = "Macro playback cancelled; completed edits can be undone";
            }
            return true;
        }
        auto translated = event;
        if (mode != Mode::native_key_query && mode != Mode::learn_key &&
            mode != Mode::learn_keys_dialog && !event.is_mouse() && !event.input().empty()) {
            const auto mapping = config.learned_keys.find(encode_hex_bytes(event.input()));
            if (mapping != config.learned_keys.end()) translated = named_event(mapping->second);
        }
        if (!help_browser && mode == Mode::editor && !macro_recording) {
            const auto macro = config.editor_macros.find(encode_hex_bytes(translated.input()));
            if (macro != config.editor_macros.end()) {
                start_macro(decode_macro(macro->second), 1);
                return true;
            }
        }
        if (dispatch_keymap(translated)) return true;
        return handle(translated);
    } catch (const std::exception& error) {
        if (help_browser) {
            help_message = error.what();
        } else if (mode == Mode::editor || mode == Mode::editor_menu) {
            show_editor_error(error);
        } else if (mode == Mode::viewer) {
            viewer_message_title = "Viewer error";
            viewer_message = error.what();
            mode = Mode::viewer_message;
        } else {
            set_error(error);
        }
        return true;
    }

    bool handle(const ftxui::Event& event) {
        if (help_browser) return handle_help(event);
        if (event == ftxui::Event::F1 && !help_key_capture()) {
            open_context_help();
            return true;
        }
        if (mode == Mode::editor_mail_dialog) return handle_editor_mail(event);
        if (mode == Mode::editor_spelling_dialog) return handle_spelling(event);
        if (mode == Mode::file_search_dialog) return handle_file_search(event);
        if (mode == Mode::advanced_chown_dialog) return handle_advanced_chown(event);
        if (mode == Mode::chattr_dialog) return handle_file_attributes(event);
        if (mode == Mode::external_panelize_dialog) return handle_external_panelize(event);
        if (mode == Mode::configure_dialog) return handle_configure_dialog(event);
        if (mode == Mode::display_bits_dialog) return handle_display_bits(event);
        if (mode == Mode::appearance_dialog) return handle_appearance(event);
        if (mode == Mode::panel_options_dialog) return handle_panel_options(event);
        if (mode == Mode::layout_dialog) return handle_layout_dialog(event);
        if (mode == Mode::confirmation_dialog) return handle_confirmation_dialog(event);
        if (mode == Mode::native_query) return handle_native_query(event);
        if (mode == Mode::learn_keys_dialog) return handle_learn_keys(event);
        if (mode == Mode::native_message) return handle_native_message(event);
        if (mode == Mode::editor_save_mode_dialog) return handle_editor_save_mode(event);
        if (mode == Mode::editor_newline_warning) return handle_newline_warning(event);
        if (mode == Mode::native_input) return handle_native_input(event);
        if (mode == Mode::native_key_query) return handle_key_query(event);
        if (mode == Mode::learn_key) return handle_learn_key(event);
        if (mode == Mode::panels || mode == Mode::viewer || mode == Mode::editor || mode == Mode::diff) {
            if (event == ftxui::Event::Special("\x1B`")) {
                show_screens(mode);
                return true;
            }
            if (event == ftxui::Event::Special("\x1B}") || event == ftxui::Event::Special("\x1B{")) {
                switch_screen(event == ftxui::Event::Special("\x1B}"));
                return true;
            }
        }
        if (event.is_mouse() &&
            (config.keybar_visible || mode == Mode::editor || mode == Mode::viewer || mode == Mode::directory_tree_dialog || mode == Mode::diff) &&
            (mode == Mode::panels || mode == Mode::viewer ||
             mode == Mode::editor || mode == Mode::search_results || mode == Mode::directory_tree_dialog || mode == Mode::diff)) {
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
        if (mode == Mode::directory_tree_dialog) return handle_directory_chooser(event);
        if (mode == Mode::diff) return handle_diff(event);
        if (mode == Mode::diff_options) return handle_diff_options(event);
        if (mode == Mode::prompt) {
            return handle_prompt(event);
        }
        if (mode == Mode::listing_dialog) return handle_listing_dialog(event);
        if (mode == Mode::sort_dialog) return handle_sort_dialog(event);
        if (mode == Mode::local_list) {
            return handle_local_list(event);
        }
        if (mode == Mode::local_command) {
            return handle_local_command(event);
        }
        if (mode == Mode::viewer) {
            return handle_viewer(event);
        }
        if (mode == Mode::viewer_close_dialog) {
            return handle_viewer_close_dialog(event);
        }
        if (mode == Mode::viewer_link_dialog) {
            return handle_viewer_link_dialog(event);
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
        if (mode == Mode::conflict_dialog || mode == Mode::operation_error_dialog ||
            mode == Mode::recursive_delete_dialog) {
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
    if (viewer_stdin_path(path)) {
        auto input = std::make_unique<ViewerStdin>();
        ViewerSessionState next;
        next.viewer = impl_->viewer;
        if (!impl_->viewer.open()) next.viewer.set_wrap(impl_->config.viewer_wrap);
        next.viewer.open_stream(impl_->local, input->data(), path);
        impl_->install_viewer_session(std::move(next));
        impl_->viewer_stdin = std::move(input);
        impl_->viewer_display_name = "Standard input";
        impl_->viewer_navigation_provider.reset();
        impl_->viewer_navigation_files.clear();
    } else {
        const auto location = impl_->resolve_location(path);
        impl_->load_viewer(location.provider, location.path);
    }
    impl_->set_viewer_return_mode(Mode::panels);
    impl_->viewer_search_needle.clear();
    impl_->exit_after_viewer = true;
    impl_->mode = Mode::viewer;
}

void App::open_editor(const std::string& path, const std::size_t line) {
    if (!path.empty()) {
        open_editor_files({{path, line}});
        return;
    }
    impl_->open_editor_buffer(impl_->local, "", true);
    impl_->editor.base_directory = std::filesystem::current_path().string();
    impl_->exit_after_editor = true;
}

void App::open_editors(const std::vector<std::string>& paths, const std::size_t first_line) {
    std::vector<EditorFile> files;
    for (const auto& path : paths) files.push_back({path, 0});
    open_editor_files(files, first_line);
}

void App::open_editor_files(const std::vector<EditorFile>& files, const std::size_t first_line) {
    if (files.empty()) {
        open_editor("", first_line);
        return;
    }
    std::vector<Impl::EditorStartupFile> unique;
    for (const auto& [path, line] : files) {
        if (path.empty()) throw std::invalid_argument("file name is empty");
        auto location = impl_->resolve_location(path);
        location.path = location.provider->normalize(location.path);
        if (std::ranges::none_of(unique, [&](const auto& file) {
                return file.location.provider->id() == location.provider->id() &&
                       file.location.path == location.path;
            })) {
            unique.push_back({std::move(location), line});
        }
    }
    impl_->editor_startup_files = std::move(unique);
    impl_->editor_startup_cursor = 0;
    impl_->editor_startup_first_line = first_line;
    impl_->editor_startup_opened = 0;
    impl_->exit_after_editor = true;
    impl_->mode = Mode::editor;
    if (!impl_->editor.open()) {
        impl_->open_editor_buffer(impl_->local, "", true);
        impl_->editor.base_directory = std::filesystem::current_path().string();
        impl_->editor_startup_placeholder = true;
    }
    impl_->continue_editor_startup();
}

void App::open_diff(const std::string& first, const std::string& second) {
    if (first.empty() || second.empty()) throw std::invalid_argument("file name is empty");
    const auto provider = impl_->active().provider();
    impl_->open_file_comparison(provider->join(impl_->active().path(), first),
                               provider->join(impl_->active().path(), second));
    impl_->exit_after_diff = true;
}

std::string App::active_path() const {
    return impl_->pane_location(impl_->active_index == 0 ? impl_->left : impl_->right);
}

int App::run() {
    struct TerminalCleanup {
        Impl* implementation;
        ~TerminalCleanup() {
            std::cout << "\033[0m\n" << std::flush;
            if (implementation->terminal_title_pushed) {
                std::cout << "\033[23;0t" << std::flush;
                implementation->terminal_title_pushed = false;
            }
        }
    } terminal_cleanup{impl_.get()};
    const auto run_terminal = [&] {
        auto terminal = ftxui::App::FullscreenAlternateScreen();
        terminal.ForceHandleCtrlC(false);
        terminal.ForceHandleCtrlZ(false);
        terminal.TrackMouse(true);
        impl_->terminal = &terminal;
        impl_->shell.set_notify([implementation = impl_.get()] {
            implementation->post_ui([implementation] { implementation->process_shell_events(true); });
        });
        if (impl_->config.auto_menus && impl_->mode == Mode::panels) impl_->open_user_menu(Mode::panels);
        struct OperationJoinGuard {
            Impl* implementation;
            bool finished{false};

            void finish() noexcept {
                if (finished) {
                    return;
                }
                finished = true;
                implementation->cancel_operations();
                implementation->local_command_cancellation.request();
                if (implementation->local_command_thread.joinable()) {
                    implementation->local_command_thread.join();
                }
                implementation->shutdown_operations();
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
                implementation->shell.close();
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
                const auto handled = impl_->run_editor_action([implementation = impl_.get(), event] {
                    return implementation->handle_input(event);
                });
                impl_->finish_editor_open_confirmation();
                impl_->service_operation_prompts();
                if (impl_->shell_exit_requested) {
                    impl_->shell_exit_requested = false;
                    impl_->request_app_exit();
                }
                impl_->update_editor_view();
                return handled;
            }
        );
        std::jthread viewer_refresh([implementation = impl_.get(), &terminal](const std::stop_token stop) {
            std::mutex mutex;
            std::condition_variable_any wake;
            std::unique_lock lock(mutex);
            while (!stop.stop_requested()) {
                wake.wait_for(lock, stop, std::chrono::milliseconds(250), [] { return false; });
                if (stop.stop_requested()) break;
                terminal.Post([implementation, &terminal] {
                    if (implementation->mode == Mode::viewer && implementation->refresh_viewer()) {
                        terminal.RequestAnimationFrame();
                    }
                });
            }
        });
        terminal.Loop(std::move(component));
        viewer_refresh.request_stop();
        viewer_refresh.join();
        if (impl_->viewer.streamed()) impl_->viewer.close();
        operation_guard.finish();
    };
    run_terminal();
    impl_->store_panel_options_in_config();
    impl_->store_editor_options_in_config();
    try {
        if (impl_->tree_cache_used) save_tree_cache(tree_cache_path(), impl_->known_tree_directories);
        if (impl_->config.auto_save_setup) impl_->config.save();
    } catch (const std::exception& error) {
        std::cerr << "mcpp: cannot save setup: " << error.what() << '\n';
        return 1;
    }
    return 0;
}

}  // namespace mcpp
