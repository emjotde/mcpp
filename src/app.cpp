#include "mcpp/app.hpp"

#include "mcpp/compare.hpp"
#include "mcpp/local_provider.hpp"
#include "mcpp/location.hpp"
#include "mcpp/search.hpp"
#include "mcpp/user_menu.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <sys/wait.h>
#include <thread>
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

enum class Mode {
    panels,
    viewer,
    editor,
    editor_menu,
    editor_help,
    editor_error,
    editor_close_dialog,
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
};

enum class PromptAction {
    none,
    rename,
    mkdir,
    find,
    viewer_find,
    editor_find,
    editor_replace_find,
    editor_replace_with,
    editor_save_as,
    editor_goto,
    quick_cd,
    chmod,
    chown,
    symlink,
    hardlink,
    recursive_search,
    select_pattern,
    unselect_pattern,
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

void erase_last_utf8(std::string& value) {
    if (value.empty()) {
        return;
    }
    auto begin = value.size() - 1U;
    while (begin > 0U &&
           (
               static_cast<unsigned char>(value[begin]) &
               0xC0U
           ) == 0x80U) {
        --begin;
    }
    value.erase(begin);
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

std::size_t utf8_offset_for_width(
    const std::string_view value,
    const std::size_t target_width
) {
    std::size_t offset = 0;
    std::size_t width = 0;
    while (offset < value.size()) {
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
    if (value.size() <= width) {
        return value;
    }
    if (width <= 3U) {
        return value.substr(0, width);
    }
    return value.substr(0, width - 3U) + "...";
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

}  // namespace

struct App::Impl {
    Config config;
    ProviderPtr local;
    Location left_location;
    Location right_location;
    Pane left;
    Pane right;
    int active_index{0};
    Mode mode{Mode::panels};
    Mode mode_before_prompt{Mode::panels};
    PromptAction prompt_action{PromptAction::none};
    std::string prompt_title;
    std::string prompt_value;
    std::string pending_destination;
    std::string status{"Ready"};
    OperationEngine operations;
    Cancellation operation_cancellation;
    TransferProgress progress;
    ViewerModel viewer;
    EditorModel editor;
    std::string editor_replace_needle;
    std::size_t editor_menu_category{0};
    std::size_t editor_menu_item{0};
    std::size_t editor_close_choice{0};
    std::string editor_error_message;
    ftxui::App* terminal{nullptr};
    std::jthread operation_thread;
    std::atomic_bool operation_running{false};
    std::mutex operation_mutex;
    std::string command_line;
    bool command_mode{false};
    std::vector<std::string> command_history;
    std::size_t command_history_index{0};
    std::vector<UserMenuEntry> user_menu;
    ftxui::Box left_box;
    ftxui::Box right_box;
    std::vector<ftxui::Box> left_entry_boxes;
    std::vector<ftxui::Box> right_entry_boxes;
    std::vector<Entry> search_results;
    std::size_t search_cursor{0};
    std::size_t bookmark_cursor{0};
    bool detailed_view{true};
    bool quick_view{false};
    ViewerModel quick_viewer;
    std::string quick_view_path;
    std::chrono::steady_clock::time_point last_click{};
    int last_click_pane{-1};
    std::size_t last_click_index{
        std::numeric_limits<std::size_t>::max()
    };
    std::size_t pulldown_category{0};
    std::size_t pulldown_item{0};
    std::array<ftxui::Box, 5> top_menu_boxes{};
    bool dialog_move{false};
    bool dialog_exact{false};
    std::string dialog_destination;
    bool dialog_preserve_attributes{true};
    bool dialog_preserve_symlinks{true};
    std::size_t dialog_focus{1};
    std::atomic_bool operation_paused{false};
    std::atomic_bool operation_skip{false};
    std::mutex operation_pause_mutex;
    std::condition_variable operation_pause_condition;
    std::string operation_label{"File operation"};
    bool pending_move{false};
    bool pending_exact{false};

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
        left.set_sort(config.sort, config.sort_descending);
        right.set_sort(config.sort, config.sort_descending);
    }

    Pane& active() {
        return active_index == 0 ? left : right;
    }

    Pane& passive() {
        return active_index == 0 ? right : left;
    }

    void refresh() {
        left.refresh();
        right.refresh();
    }

    void sort_active(const SortField field) {
        const auto descending =
            active().sort_field() == field
                ? !active().descending()
                : false;
        active().set_sort(field, descending);
        status = descending ? "Sort descending" : "Sort ascending";
    }

    void set_error(const std::exception& error) {
        status = std::string("Error: ") + error.what();
    }

    void post_ui(std::function<void()> task) {
        if (terminal == nullptr) {
            return;
        }
        terminal->Post(std::move(task));
        terminal->PostEvent(ftxui::Event::Custom);
    }

    void start_prompt(
        const PromptAction action,
        std::string title,
        std::string value = {}
    ) {
        mode_before_prompt = mode;
        mode = Mode::prompt;
        prompt_action = action;
        prompt_title = std::move(title);
        prompt_value = std::move(value);
    }

    void start_operation_dialog(
        const bool move_operation,
        const bool exact
    ) {
        const auto sources = active().selected_entries();
        if (sources.empty()) {
            throw std::runtime_error("no source selected");
        }
        dialog_move = move_operation;
        dialog_exact = exact && sources.size() == 1U;
        dialog_destination =
            dialog_exact
                ? passive().provider()->join(
                      passive().path(),
                      sources.front().name
                  )
                : passive().path();
        dialog_preserve_attributes = true;
        dialog_preserve_symlinks = true;
        dialog_focus = 0;
        mode = Mode::operation_dialog;
    }

    void confirm_operation_dialog() {
        if (dialog_destination.empty()) {
            throw std::runtime_error("destination is empty");
        }
        if (dialog_exact) {
            if (config.confirm_overwrite &&
                passive().provider()->exists(dialog_destination)) {
                pending_destination = dialog_destination;
                pending_move = dialog_move;
                pending_exact = true;
                dialog_focus = 0;
                mode = Mode::conflict_dialog;
                return;
            }
            run_operation(
                dialog_move,
                dialog_destination,
                !config.confirm_overwrite,
                true
            );
            return;
        }
        if (config.confirm_overwrite &&
            has_collision(dialog_destination)) {
            pending_destination = dialog_destination;
            pending_move = dialog_move;
            pending_exact = false;
            dialog_focus = 0;
            mode = Mode::conflict_dialog;
            return;
        }
        run_operation(
            dialog_move,
            dialog_destination,
            !config.confirm_overwrite
        );
    }

    void start_delete_dialog() {
        if (active().selected_entries().empty()) {
            throw std::runtime_error("no source selected");
        }
        dialog_focus = config.confirm_delete ? 1U : 0U;
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
        const auto sources = active().selected_entries();
        if (sources.empty()) {
            throw std::runtime_error("no source selected");
        }
        if (operation_thread.joinable()) {
            operation_thread.join();
        }
        operation_cancellation = Cancellation{};
        progress = {};
        const auto source_provider = active().provider();
        const auto destination_provider = passive().provider();
        const auto source_index = active_index;
        const OperationOptions options{
            .overwrite = overwrite,
            .preserve_permissions = dialog_preserve_attributes,
            .preserve_symlinks = dialog_preserve_symlinks,
            .collision =
                [overwrite](const Entry&, const Entry&) {
                    return overwrite
                               ? CollisionDecision::overwrite
                               : CollisionDecision::skip;
                },
        };
        operation_running.store(true, std::memory_order_release);
        operation_paused.store(false, std::memory_order_release);
        operation_skip.store(false, std::memory_order_release);
        mode = Mode::operation_progress;
        operation_label = move_operation ? "Move" : "Copy";
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
                source_index,
                exact
            ]() {
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
                                               operation_cancellation
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
                            callback
                        );
                    } else if (move_operation) {
                        operations.move(
                            *source_provider,
                            sources,
                            *destination_provider,
                            destination,
                            options,
                            operation_cancellation,
                            callback
                        );
                    } else {
                        operations.copy(
                            *source_provider,
                            sources,
                            *destination_provider,
                            destination,
                            options,
                            operation_cancellation,
                            callback
                        );
                    }
                    post_ui([
                        this,
                        move_operation,
                        source_index
                    ] {
                        mode = Mode::panels;
                        operation_running.store(
                            false,
                            std::memory_order_release
                        );
                        try {
                            (source_index == 0 ? left : right)
                                .clear_selection();
                            refresh();
                            status = move_operation
                                         ? "Move complete"
                                         : "Copy complete";
                        } catch (const std::exception& error) {
                            set_error(error);
                        }
                    });
                } catch (const OperationCancelled&) {
                    post_ui([this] {
                        status = "Operation cancelled";
                        mode = Mode::panels;
                        operation_running.store(
                            false,
                            std::memory_order_release
                        );
                    });
                } catch (const std::exception& error) {
                    const auto message = std::string(error.what());
                    post_ui([this, message] {
                        status = "Error: " + message;
                        mode = Mode::panels;
                        operation_running.store(
                            false,
                            std::memory_order_release
                        );
                    });
                }
            });
        } catch (...) {
            operation_running.store(
                false,
                std::memory_order_release
            );
            throw;
        }
    }

    bool has_collision(const std::string& destination) {
        const auto sources = active().selected_entries();
        return std::any_of(
            sources.begin(),
            sources.end(),
            [&](const Entry& entry) {
                return passive().provider()->exists(
                    passive().provider()->join(
                        destination,
                        entry.name
                    )
                );
            }
        );
    }

    void run_remove_operation() {
        if (operation_running.load(std::memory_order_acquire)) {
            throw std::runtime_error("an operation is already running");
        }
        const auto entries = active().selected_entries();
        if (entries.empty()) {
            throw std::runtime_error("no source selected");
        }
        if (operation_thread.joinable()) {
            operation_thread.join();
        }
        const auto provider = active().provider();
        const auto source_index = active_index;
        operation_cancellation = Cancellation{};
        progress = {};
        operation_running.store(true, std::memory_order_release);
        operation_paused.store(false, std::memory_order_release);
        operation_skip.store(false, std::memory_order_release);
        mode = Mode::operation_progress;
        operation_label = "Delete";
        status = "Starting delete";
        try {
            operation_thread = std::jthread(
            [this, provider, entries, source_index] {
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
                                               operation_cancellation
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
                        }
                    );
                    post_ui([this, source_index] {
                        mode = Mode::panels;
                        operation_running.store(
                            false,
                            std::memory_order_release
                        );
                        try {
                            (source_index == 0 ? left : right)
                                .clear_selection();
                            refresh();
                            status = "Delete complete";
                        } catch (const std::exception& error) {
                            set_error(error);
                        }
                    });
                } catch (const OperationCancelled&) {
                    post_ui([this] {
                        status = "Delete cancelled";
                        mode = Mode::panels;
                        operation_running.store(
                            false,
                            std::memory_order_release
                        );
                    });
                } catch (const std::exception& error) {
                    const auto message = std::string(error.what());
                    post_ui([this, message] {
                        status = "Error: " + message;
                        mode = Mode::panels;
                        operation_running.store(
                            false,
                            std::memory_order_release
                        );
                    });
                }
            });
        } catch (...) {
            operation_running.store(
                false,
                std::memory_order_release
            );
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
        int child_status = 0;
        terminal->WithRestoredIO([&] {
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
        command_history_index = command_history.size();
        command_line.clear();
        command_mode = false;
        try {
            const auto result = run_process(command, false);
            status = "Command exited " + std::to_string(result);
        } catch (const std::exception& error) {
            set_error(error);
        }
    }

    void execute_prompt() {
        try {
            switch (prompt_action) {
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
                case PromptAction::find:
                    status = active().find(prompt_value)
                                 ? "Match found"
                                 : "No match";
                    break;
                case PromptAction::recursive_search: {
                    const SearchEngine search;
                    search_results = search.search(
                        *active().provider(),
                        active().path(),
                        {
                            .name_pattern =
                                prompt_value.empty()
                                    ? "*"
                                    : prompt_value,
                            .content = {},
                            .case_sensitive = false,
                            .recursive = true,
                            .include_hidden = active().show_hidden(),
                        },
                        operation_cancellation
                    );
                    search_cursor = 0;
                    mode = Mode::search_results;
                    status =
                        std::to_string(search_results.size()) +
                        " search result(s)";
                    prompt_action = PromptAction::none;
                    prompt_value.clear();
                    return;
                }
                case PromptAction::select_pattern:
                    active().select_pattern(prompt_value, true);
                    status = "Selection updated";
                    break;
                case PromptAction::unselect_pattern:
                    active().select_pattern(prompt_value, false);
                    status = "Selection updated";
                    break;
                case PromptAction::viewer_find:
                    status = viewer.find(prompt_value, false)
                                 ? "Match found"
                                 : "No match";
                    break;
                case PromptAction::editor_find:
                    status = editor.find(prompt_value, false)
                                 ? "Match found"
                                 : "No match";
                    break;
                case PromptAction::editor_replace_find:
                    editor_replace_needle = prompt_value;
                    prompt_action = PromptAction::editor_replace_with;
                    prompt_title = "Replace with";
                    prompt_value.clear();
                    return;
                case PromptAction::editor_replace_with:
                    status = editor.replace(
                                 editor_replace_needle,
                                 prompt_value,
                                 false
                             )
                                 ? "Replaced"
                                 : "No match";
                    editor_replace_needle.clear();
                    break;
                case PromptAction::editor_save_as:
                    if (!prompt_value.empty()) {
                        editor.save_as(prompt_value, false);
                        status = "Saved as " + editor.path();
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
                    status = "Line " + std::to_string(line);
                    break;
                }
                case PromptAction::quick_cd:
                    active().change_directory(prompt_value);
                    status = "Directory changed";
                    break;
                case PromptAction::chmod: {
                    const auto* entry = active().current();
                    if (entry == nullptr) {
                        break;
                    }
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
                    active().provider()->set_permissions(
                        entry->path,
                        static_cast<std::filesystem::perms>(
                            parsed_mode
                        )
                    );
                    active().refresh();
                    status = "Permissions changed";
                    break;
                }
                case PromptAction::chown: {
                    const auto* entry = active().current();
                    if (entry == nullptr) {
                        break;
                    }
                    const auto colon = prompt_value.find(':');
                    const auto owner =
                        colon == std::string::npos
                            ? prompt_value
                            : prompt_value.substr(0, colon);
                    const auto group =
                        colon == std::string::npos
                            ? std::string{}
                            : prompt_value.substr(colon + 1U);
                    active().provider()->set_owner(
                        entry->path,
                        owner.empty()
                            ? std::nullopt
                            : std::optional<std::string>(owner),
                        group.empty()
                            ? std::nullopt
                            : std::optional<std::string>(group)
                    );
                    active().refresh();
                    status = "Owner changed";
                    break;
                }
                case PromptAction::symlink: {
                    const auto* entry = active().current();
                    if (entry == nullptr || prompt_value.empty()) {
                        break;
                    }
                    active().provider()->create_symlink(
                        entry->path,
                        active().provider()->join(
                            active().path(),
                            prompt_value
                        )
                    );
                    active().refresh();
                    status = "Symbolic link created";
                    break;
                }
                case PromptAction::hardlink: {
                    const auto* entry = active().current();
                    if (entry == nullptr || prompt_value.empty()) {
                        break;
                    }
                    active().provider()->create_hardlink(
                        entry->path,
                        active().provider()->join(
                            active().path(),
                            prompt_value
                        )
                    );
                    active().refresh();
                    status = "Hard link created";
                    break;
                }
                case PromptAction::none:
                    break;
            }
        } catch (const std::exception& error) {
            if (mode_before_prompt == Mode::editor) {
                editor_replace_needle.clear();
                prompt_action = PromptAction::none;
                prompt_value.clear();
                show_editor_error(error);
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
        ftxui::Box& pane_box,
        std::vector<ftxui::Box>& entry_boxes
    ) {
        using namespace ftxui;
        const auto terminal_size = Terminal::Size();
        const auto panel_width = std::max(30, terminal_size.dimx / 2);
        const auto inner_width = std::max(28, panel_width - 2);
        const auto name_width = std::max(10, inner_width - 24);
        const auto visible_rows = static_cast<std::size_t>(
            std::max(3, terminal_size.dimy - 9)
        );
        Elements rows;
        const auto& entries = pane.entries();
        rows.reserve(visible_rows);
        entry_boxes.clear();
        entry_boxes.resize(entries.size());
        for (std::size_t index = 0; index < entries.size(); ++index) {
            const auto& entry = entries[index];
            auto name = type_prefix(entry) + display_text(entry.name);
            auto row =
                detailed_view
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
                                  : std::to_string(entry.size)
                          ) | size(WIDTH, EQUAL, 8),
                          separator(),
                          text(format_panel_time(entry.modified)) |
                              size(WIDTH, EQUAL, 12),
                      })
                    : hbox({
                          text(pane.is_selected(entry) ? "*" : " "),
                          text(name) | flex,
                      });
            if (index == pane.cursor() && focused) {
                row = row | inverted | focus;
            } else if (pane.is_selected(entry)) {
                row = row | bold;
            }
            rows.push_back(
                std::move(row) | reflect(entry_boxes[index])
            );
        }
        while (rows.size() < visible_rows) {
            rows.push_back(
                detailed_view
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
        const auto info =
            current == nullptr
                ? std::string{}
                : permission_text(*current) + " " +
                      display_text(current->name) + "  " +
                      format_size(current->size) +
                      "  " + current->owner + ":" + current->group;
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
        auto title = text(" " + truncate_text(
            pane.path(),
            static_cast<std::size_t>(std::max(8, panel_width - 8))
        ) + " ");
        if (focused) {
            title = title | bold;
        }
        auto header =
            detailed_view
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
        const auto available_width =
            static_cast<int>(available.size());
        const auto info_width =
            std::max(0, inner_width - available_width - 1);
        auto content = vbox({
                   header,
                   vbox(std::move(rows)) | yframe | flex,
                   separator(),
                   hbox({
                       text(truncate_text(
                           info,
                           static_cast<std::size_t>(info_width)
                       )) | size(WIDTH, EQUAL, info_width),
                       filler(),
                       text(available) | dim,
                   }),
               });
        return window(title, content, LIGHT) |
               flex | reflect(pane_box);
    }

    ftxui::Element render_viewer() {
        using namespace ftxui;
        const auto rows = viewer.lines(120, 40);
        Elements elements;
        for (const auto& row : rows) {
            elements.push_back(text(row));
        }
        return vbox({
                   text(
                       "View: " + viewer.path() +
                       (viewer.hex() ? " [hex]" : "") +
                       (viewer.wrap() ? " [wrap]" : "")
                   ) |
                       bold,
                   separator(),
                   vbox(std::move(elements)) | yframe | flex,
                   separator(),
                   text("F2 Wrap  F4 Hex  / Search  F3/Esc Close"),
               }) |
               border;
    }

    ftxui::Element render_editor_line(
        const std::size_t index,
        const std::size_t first_column
    ) const {
        using namespace ftxui;
        const auto& line = editor.lines()[index];
        auto selected_begin = line.size();
        auto selected_end = std::size_t{0};
        if (const auto selected = editor.selection();
            selected &&
            index >= selected->first.row &&
            index <= selected->second.row) {
            selected_begin =
                index == selected->first.row
                    ? selected->first.column
                    : 0U;
            selected_end =
                index == selected->second.row
                    ? selected->second.column
                    : line.size();
        }

        const auto visible_begin =
            utf8_offset_for_width(line, first_column);
        std::vector<std::size_t> boundaries{
            visible_begin,
            line.size(),
            std::min(selected_begin, line.size()),
            std::min(selected_end, line.size()),
        };
        if (index == editor.row()) {
            boundaries.push_back(
                std::min(editor.column(), line.size())
            );
            boundaries.push_back(
                next_utf8(line, editor.column())
            );
        }
        std::sort(boundaries.begin(), boundaries.end());
        boundaries.erase(
            std::unique(boundaries.begin(), boundaries.end()),
            boundaries.end()
        );

        Elements segments;
        for (std::size_t boundary = 0;
             boundary + 1U < boundaries.size();
             ++boundary) {
            const auto begin = boundaries[boundary];
            const auto end = boundaries[boundary + 1U];
            if (begin < visible_begin || begin == end) {
                continue;
            }
            auto segment = text(line.substr(begin, end - begin));
            if (begin >= selected_begin && begin < selected_end) {
                segment = segment | inverted;
            }
            if (index == editor.row() &&
                begin == editor.column()) {
                segment = segment | inverted | bold;
            }
            segments.push_back(std::move(segment));
        }
        if (index == editor.row() &&
            editor.column() == line.size()) {
            segments.push_back(text(" ") | inverted | bold);
        }
        if (segments.empty()) {
            segments.push_back(text(""));
        }
        return hbox(std::move(segments));
    }

    ftxui::Element render_editor() {
        using namespace ftxui;
        const auto terminal_size = Terminal::Size();
        const auto visible_rows = static_cast<std::size_t>(
            std::max(1, terminal_size.dimy - 2)
        );
        const auto& lines = editor.lines();
        const auto first =
            editor.row() > visible_rows / 2U
                ? editor.row() - visible_rows / 2U
                : 0U;
        const auto end = std::min(lines.size(), first + visible_rows);
        const auto& current_line = lines[editor.row()];
        const auto cursor_width = static_cast<std::size_t>(
            std::max(
                0,
                string_width(
                    std::string_view(current_line).substr(
                        0,
                        editor.column()
                    )
                )
            )
        );
        const auto content_width = static_cast<std::size_t>(
            std::max(1, terminal_size.dimx)
        );
        const auto first_column =
            cursor_width >= content_width
                ? cursor_width - content_width + 1U
                : 0U;
        Elements elements;
        for (std::size_t index = first; index < end; ++index) {
            elements.push_back(
                render_editor_line(index, first_column)
            );
        }

        static const std::array<std::string, 7> menu_names{
            "File", "Edit", "Search", "Command",
            "Format", "Window", "Options"
        };
        Elements menu;
        for (std::size_t index = 0; index < menu_names.size(); ++index) {
            auto item = text(" " + menu_names[index] + " ");
            if (index == editor_menu_category) {
                item = item | bold | underlined;
            }
            menu.push_back(std::move(item));
        }
        const auto header =
            mode == Mode::editor_menu
                ? hbox(std::move(menu)) | inverted
                : hbox({
                      text(display_text(editor.path())) | bold | flex,
                      text(
                          editor.dirty() ? " [-M--] " : " [----] "
                      ),
                      text(
                          "L:" +
                          std::to_string(editor.row() + 1U) +
                          "/" +
                          std::to_string(editor.lines().size()) +
                          " C:" +
                          std::to_string(cursor_width + 1U) +
                          " " +
                          std::to_string(editor.byte_size()) +
                          "b"
                      ),
                  });
        const auto key_width =
            std::max(3, terminal_size.dimx / 10);
        const auto key = [key_width](
                             const std::string& number,
                             const std::string& label
                         ) {
            const auto label_width = static_cast<std::size_t>(
                std::max(
                    0,
                    key_width -
                        static_cast<int>(number.size()) - 1
                )
            );
            return hbox({
                       text(number) | inverted,
                       text(label.substr(0, label_width)),
                       text(" "),
                   }) |
                   size(WIDTH, EQUAL, key_width);
        };
        return vbox({
            header,
            vbox(std::move(elements)) | flex,
            hbox({
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

    ftxui::Element render_editor_menu() const {
        using namespace ftxui;
        static const std::array<std::vector<std::string>, 7> menus{{
            {"Save  F2", "Save as...  F12", "Close  F10"},
            {
                "Undo  C-z",
                "Redo  C-y",
                "Mark  F3",
                "Copy  F5",
                "Move  F6",
                "Delete  F8",
            },
            {"Find  F7", "Replace  F4", "Go to line  C-g"},
            {"No commands"},
            {"No commands"},
            {"No commands"},
            {"Tab inserts 4 spaces"},
        }};
        Elements rows;
        const auto& items = menus[editor_menu_category];
        for (std::size_t index = 0; index < items.size(); ++index) {
            auto row = text(" " + items[index] + " ");
            if (index == editor_menu_item) {
                row = row | inverted;
            }
            rows.push_back(std::move(row));
        }
        return vbox(std::move(rows)) | borderStyled(LIGHT);
    }

    ftxui::Element render_editor_close_dialog() const {
        using namespace ftxui;
        const auto button = [&](const std::string& label,
                                const std::size_t index) {
            auto element = text("[ " + label + " ]");
            return index == editor_close_choice
                       ? element | inverted
                       : element;
        };
        return vbox({
                   text("Close file") | bold | center,
                   separator(),
                   text(
                       "File " + display_text(editor.path()) +
                       " was modified."
                   ),
                   text("Save before close?") | center,
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
                   text("F8        Delete block or character"),
                   text("F9        Pull-down menu"),
                   text("F10       Close editor"),
                   text("F12       Save as"),
                   text("Ctrl-Z/Y  Undo/redo"),
                   text("Ctrl-G    Go to line"),
                   separator(),
                   text("Press F1 or Esc") | center,
               }) |
               borderStyled(LIGHT);
    }

    ftxui::Element render_editor_error() const {
        using namespace ftxui;
        return vbox({
                   text("Editor error") | bold | center,
                   separator(),
                   paragraph(display_text(editor_error_message)),
                   separator(),
                   text("[ OK ]") | inverted | center,
               }) |
               borderStyled(LIGHT) |
               size(WIDTH, LESS_THAN, 72);
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
        std::vector<std::string>,
        5
    >& pulldown_items() {
        static const std::array<std::vector<std::string>, 5> items{{
            {
                "Brief listing",
                "Full listing",
                "Toggle hidden",
                "Sort by name",
                "Sort by size",
                "Sort by time",
                "Refresh",
                "Directory hotlist",
            },
            {
                "View",
                "Edit",
                "Copy",
                "Rename",
                "Move",
                "Make directory",
                "Delete",
                "Information",
                "Change mode",
                "Change owner",
                "Quit",
            },
            {
                "Find recursively",
                "Compare directories",
                "Quick directory",
                "Subshell",
                "Refresh both",
            },
            {
                "Toggle quick view",
                "Confirm delete",
                "Confirm overwrite",
                "Save setup",
            },
            {
                "Brief listing",
                "Full listing",
                "Toggle hidden",
                "Sort by name",
                "Sort by size",
                "Sort by time",
                "Refresh",
                "Directory hotlist",
            },
        }};
        return items;
    }

    ftxui::Element render_pulldown() const {
        using namespace ftxui;
        static const std::array<std::string, 5> names{
            "Left", "File", "Command", "Options", "Right"
        };
        Elements rows;
        const auto& items = pulldown_items()[pulldown_category];
        for (std::size_t index = 0; index < items.size(); ++index) {
            auto row = text(" " + items[index] + " ");
            if (index == pulldown_item) {
                row = row | inverted | focus;
            }
            rows.push_back(std::move(row));
        }
        return window(
                   text(" " + names[pulldown_category] + " ") | bold,
                   vbox(std::move(rows)),
                   LIGHT
               ) |
               size(WIDTH, EQUAL, 32);
    }

    void execute_pulldown() {
        if (pulldown_category == 0U ||
            pulldown_category == 4U) {
            active_index = pulldown_category == 0U ? 0 : 1;
            switch (pulldown_item) {
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
                    active().refresh();
                    break;
                case 7:
                    bookmark_cursor = 0;
                    mode = Mode::bookmarks;
                    return;
                default:
                    break;
            }
            mode = Mode::panels;
            return;
        }
        if (pulldown_category == 1U) {
            const auto* entry = active().current();
            switch (pulldown_item) {
                case 0:
                    if (entry != nullptr &&
                        entry->kind == EntryKind::file) {
                        viewer.open(active().provider(), entry->path);
                        mode = Mode::viewer;
                        return;
                    }
                    break;
                case 1:
                    if (entry != nullptr &&
                        entry->kind == EntryKind::file) {
                        editor.open(active().provider(), entry->path);
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
                case 8:
                    start_prompt(
                        PromptAction::chmod,
                        "Octal permissions",
                        "0644"
                    );
                    return;
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
                    terminal->Exit();
                    return;
                default:
                    break;
            }
            mode = Mode::panels;
            return;
        }
        if (pulldown_category == 2U) {
            switch (pulldown_item) {
                case 0:
                    start_prompt(
                        PromptAction::recursive_search,
                        "Recursive name pattern",
                        "*"
                    );
                    return;
                case 1: {
                    const DirectoryComparator comparator;
                    const auto differences = comparator.compare(
                        *left.provider(),
                        left.path(),
                        *right.provider(),
                        right.path(),
                        true,
                        operation_cancellation
                    );
                    status =
                        std::to_string(differences.size()) +
                        " difference(s)";
                    break;
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
                default:
                    break;
            }
            mode = Mode::panels;
            return;
        }
        if (pulldown_category == 3U) {
            switch (pulldown_item) {
                case 0:
                    quick_view = !quick_view;
                    quick_viewer.close();
                    quick_view_path.clear();
                    break;
                case 1:
                    config.confirm_delete = !config.confirm_delete;
                    break;
                case 2:
                    config.confirm_overwrite =
                        !config.confirm_overwrite;
                    break;
                case 3:
                    config.save(default_config_path());
                    status = "Setup saved";
                    break;
                default:
                    break;
            }
            mode = Mode::panels;
        }
    }

    ftxui::Element render_bookmarks() const {
        using namespace ftxui;
        Elements rows;
        for (std::size_t index = 0;
             index < config.bookmarks.size();
             ++index) {
            auto row = text(config.bookmarks[index]);
            if (index == bookmark_cursor) {
                row = row | inverted | focus;
            }
            rows.push_back(std::move(row));
        }
        if (rows.empty()) {
            rows.push_back(text("<hotlist is empty>") | dim);
        }
        return vbox({
                   text("Directory hotlist") | bold,
                   separator(),
                   vbox(std::move(rows)) | yframe | flex,
                   separator(),
                   text("Enter opens; Delete removes; Esc closes"),
               }) |
               borderStyled(LIGHT) | size(WIDTH, EQUAL, 62);
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

    ftxui::Element render_operation_dialog() const {
        using namespace ftxui;
        const auto sources = active_index == 0
                                 ? left.selected_entries()
                                 : right.selected_entries();
        const auto focused = [&](Element element, const std::size_t index) {
            return dialog_focus == index
                       ? element | inverted | focus
                       : element;
        };
        return vbox({
                   text(
                       dialog_move ? "Move / Rename" : "Copy"
                   ) | bold | center,
                   separator(),
                   text(
                       std::to_string(sources.size()) +
                       " selected item(s)"
                   ),
                   text(
                       sources.size() == 1U
                           ? "Source: " +
                                 display_text(sources.front().name)
                           : ""
                   ),
                   separator(),
                   text("To:"),
                   focused(
                       text("> " + display_text(dialog_destination)),
                       0
                   ),
                   separator(),
                   focused(
                       text(
                           std::string(
                               dialog_preserve_attributes
                                   ? "[x] "
                                   : "[ ] "
                           ) + "Preserve permissions"
                       ),
                       1
                   ),
                   focused(
                       text(
                           std::string(
                               dialog_preserve_symlinks
                                   ? "[x] "
                                   : "[ ] "
                           ) + "Preserve symbolic links"
                       ),
                       2
                   ),
                   separator(),
                   hbox({
                       focused(text(" [ OK ] "), 3),
                       text("  "),
                       focused(text(" [ Cancel ] "), 4),
                   }) | center,
               }) |
               borderStyled(LIGHT) | size(WIDTH, EQUAL, 68);
    }

    ftxui::Element render_delete_dialog() const {
        using namespace ftxui;
        const auto entries = active_index == 0
                                 ? left.selected_entries()
                                 : right.selected_entries();
        const auto focused = [&](Element element, const std::size_t index) {
            return dialog_focus == index
                       ? element | inverted | focus
                       : element;
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
                   gauge(std::clamp(byte_ratio, 0.0F, 1.0F)),
                   text(
                       "Files: " +
                       std::to_string(current.files_done) + " / " +
                       std::to_string(current.files_total)
                   ),
                   gauge(std::clamp(file_ratio, 0.0F, 1.0F)),
                   separator(),
                   text(
                       operation_paused.load(std::memory_order_acquire)
                           ? "Space Continue   S Skip   Esc Abort"
                           : "Space Pause      S Skip   Esc Abort"
                   ) | center,
               }) |
               borderStyled(LIGHT) | size(WIDTH, EQUAL, 72);
    }

    ftxui::Element render_conflict_dialog() const {
        using namespace ftxui;
        const auto focused = [&](Element element, const std::size_t index) {
            return dialog_focus == index
                       ? element | inverted | focus
                       : element;
        };
        return vbox({
                   text("Destination exists") | bold | center,
                   separator(),
                   text(display_text(pending_destination)),
                   text(
                       pending_exact
                           ? "Replace this destination?"
                           : "Some destination entries already exist."
                   ),
                   separator(),
                   hbox({
                       focused(text(" [ Overwrite ] "), 0),
                       text(" "),
                       focused(text(" [ Skip ] "), 1),
                       text(" "),
                       focused(text(" [ Cancel ] "), 2),
                   }) | center,
               }) |
               borderStyled(LIGHT) | size(WIDTH, EQUAL, 72);
    }

    ftxui::Element render_search_results() const {
        using namespace ftxui;
        Elements rows;
        rows.reserve(search_results.size());
        for (std::size_t index = 0;
             index < search_results.size();
             ++index) {
            auto row = text(display_text(search_results[index].path));
            if (index == search_cursor) {
                row = row | inverted | focus;
            }
            rows.push_back(std::move(row));
        }
        if (rows.empty()) {
            rows.push_back(text("<no matches>") | dim);
        }
        return vbox({
                   text("Search results") | bold,
                   separator(),
                   vbox(std::move(rows)) | yframe | flex,
                   separator(),
                   text("Enter opens location; Esc closes"),
               }) |
               border;
    }

    ftxui::Element render_quick_view() {
        using namespace ftxui;
        const auto* entry = active().current();
        if (entry == nullptr || entry->kind != EntryKind::file) {
            quick_viewer.close();
            quick_view_path.clear();
            return vbox({
                       text("Quick view") | bold | center,
                       separator(),
                       text("<select a regular file>") | dim | center,
                   }) |
                   border | flex;
        }
        if (entry->path != quick_view_path) {
            try {
                quick_viewer.open(active().provider(), entry->path);
                quick_view_path = entry->path;
            } catch (const std::exception& error) {
                set_error(error);
                quick_view_path.clear();
                return text("<preview unavailable>") |
                       border | center | flex;
            }
        }
        Elements rows;
        for (const auto& line : quick_viewer.lines(80, 40)) {
            rows.push_back(text(line));
        }
        return vbox({
                   text("Quick view: " + display_text(entry->name)) |
                       bold,
                   separator(),
                   vbox(std::move(rows)) | yframe | flex,
               }) |
               border | flex;
    }

    ftxui::Element render() {
        using namespace ftxui;
        Element body;
        if (quick_view) {
            body =
                active_index == 0
                    ? hbox({
                          render_pane(
                              left,
                              true,
                              left_box,
                              left_entry_boxes
                          ),
                          render_quick_view(),
                      })
                    : hbox({
                          render_quick_view(),
                          render_pane(
                              right,
                              true,
                              right_box,
                              right_entry_boxes
                          ),
                      });
        } else {
            body = hbox({
                render_pane(
                    left,
                    active_index == 0,
                    left_box,
                    left_entry_boxes
                ),
                render_pane(
                    right,
                    active_index == 1,
                    right_box,
                    right_entry_boxes
                ),
            });
        }
        if (mode == Mode::viewer) {
            body = render_viewer();
        } else if (mode == Mode::search_results) {
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
        const auto key_width =
            std::max(3, Terminal::Size().dimx / 10);
        const auto key = [key_width](
                             const std::string& number,
                             const std::string& label
                         ) {
            const auto label_width = static_cast<std::size_t>(
                std::max(
                    0,
                    key_width -
                        static_cast<int>(number.size()) - 1
                )
            );
            return hbox({
                       text(number) | inverted,
                       text(label.substr(0, label_width)),
                       text(" "),
                   }) |
                   size(WIDTH, EQUAL, key_width);
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
            status == "Ready"
                ? "Hint: Tab changes your current panel."
                : status + progress_text;
        static const std::array<std::string, 5> menu_names{
            "Left", "File", "Command", "Options", "Right"
        };
        Elements menu_elements;
        for (std::size_t index = 0; index < menu_names.size(); ++index) {
            auto item = text(" " + menu_names[index] + " ");
            if (mode == Mode::pulldown &&
                index == pulldown_category) {
                item = item | bold | underlined;
            }
            menu_elements.push_back(
                std::move(item) | reflect(top_menu_boxes[index])
            );
        }
        auto document = vbox({
            hbox(std::move(menu_elements)) | inverted,
            body | flex,
            text(hint),
            text(
                command_mode
                    ? prompt + " " + command_line
                    : prompt
            ),
            hbox({
                key("1", "Help"),
                key("2", "Menu"),
                key("3", "View"),
                key("4", "Edit"),
                key("5", "Copy"),
                key("6", "RenMov"),
                key("7", "Mkdir"),
                key("8", "Delete"),
                key("9", "PullDn"),
                key("10", "Quit"),
            }),
        });
        const bool editor_context =
            mode == Mode::editor ||
            mode == Mode::editor_menu ||
            mode == Mode::editor_help ||
            mode == Mode::editor_error ||
            mode == Mode::editor_close_dialog ||
            (
                mode == Mode::prompt &&
                mode_before_prompt == Mode::editor
            );
        if (editor_context) {
            document = render_editor();
        }
        const auto centered_modal = [](Element element) {
            return vbox({
                filler(),
                hbox({
                    filler(),
                    std::move(element) | clear_under,
                    filler(),
                }),
                filler(),
            });
        };
        if (mode == Mode::prompt) {
            auto prompt_dialog = vbox({
                              text(prompt_title) | bold,
                              separator(),
                              text("> " + prompt_value),
                              separator(),
                              text("Enter confirms; Esc cancels") | dim,
                          }) |
                          borderStyled(LIGHT) |
                          size(WIDTH, EQUAL, 56);
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
        } else if (mode == Mode::editor_close_dialog) {
            document = dbox({
                document,
                centered_modal(render_editor_close_dialog()),
            });
        } else if (mode == Mode::editor_menu) {
            static constexpr std::array<int, 7> offsets{
                0, 9, 18, 29, 41, 52, 63
            };
            auto positioned_menu = vbox({
                filler() | size(HEIGHT, EQUAL, 1),
                hbox({
                    filler() |
                        size(
                            WIDTH,
                            EQUAL,
                            offsets[editor_menu_category]
                        ),
                    render_editor_menu() | clear_under,
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
        } else if (mode == Mode::conflict_dialog) {
            document = dbox({
                document,
                centered_modal(render_conflict_dialog()),
            });
        } else if (mode == Mode::pulldown) {
            static constexpr std::array<int, 5> offsets{
                0, 6, 12, 21, 30
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
        return document;
    }

    bool handle_prompt(const ftxui::Event& event) {
        if (event == ftxui::Event::Escape) {
            mode = mode_before_prompt;
            prompt_action = PromptAction::none;
            return true;
        }
        if (event == ftxui::Event::Return) {
            execute_prompt();
            return true;
        }
        if (event == ftxui::Event::Backspace) {
            erase_last_utf8(prompt_value);
            return true;
        }
        if (event.is_character()) {
            prompt_value += event.character();
            return true;
        }
        return false;
    }

    bool handle_viewer(const ftxui::Event& event) {
        if (event == ftxui::Event::Escape ||
            event == ftxui::Event::F3 ||
            event == ftxui::Event::F10) {
            viewer.close();
            mode = Mode::panels;
            return true;
        }
        if (event == ftxui::Event::ArrowDown) {
            viewer.scroll_lines(1, 40);
            return true;
        }
        if (event == ftxui::Event::ArrowUp) {
            viewer.scroll_lines(-1, 40);
            return true;
        }
        if (event == ftxui::Event::PageDown) {
            viewer.page(1, 40);
            return true;
        }
        if (event == ftxui::Event::PageUp) {
            viewer.page(-1, 40);
            return true;
        }
        if (event == ftxui::Event::F2) {
            viewer.set_wrap(!viewer.wrap());
            return true;
        }
        if (event == ftxui::Event::F4) {
            viewer.toggle_hex();
            return true;
        }
        if (event == ftxui::Event::Character('/')) {
            start_prompt(PromptAction::viewer_find, "Find in file");
            return true;
        }
        return false;
    }

    void show_editor_error(const std::exception& error) {
        editor_error_message = error.what();
        status = "Error: " + editor_error_message;
        mode = Mode::editor_error;
    }

    void save_editor() {
        try {
            editor.save(false);
            status = "Saved";
        } catch (const std::exception& error) {
            show_editor_error(error);
        }
    }

    void close_editor() {
        editor.close();
        mode = Mode::panels;
        try {
            refresh();
            status = "Editor closed";
        } catch (const std::exception& error) {
            set_error(error);
        }
    }

    void request_editor_close() {
        if (editor.dirty()) {
            editor_close_choice = 0;
            mode = Mode::editor_close_dialog;
        } else {
            close_editor();
        }
    }

    void execute_editor_menu_item() {
        const auto category = editor_menu_category;
        const auto item = editor_menu_item;
        mode = Mode::editor;
        if (category == 0U) {
            if (item == 0U) {
                save_editor();
            } else if (item == 1U) {
                start_prompt(
                    PromptAction::editor_save_as,
                    "Save as",
                    editor.path()
                );
            } else {
                request_editor_close();
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
                    editor.toggle_mark();
                    status = editor.marking()
                                 ? "Marking block"
                                 : "Block marked";
                    break;
                case 3:
                    status = editor.copy_selection()
                                 ? "Block copied"
                                 : "No marked block";
                    break;
                case 4:
                    status = editor.move_selection()
                                 ? "Block moved"
                                 : "No movable block";
                    break;
                case 5:
                    if (!editor.delete_selection()) {
                        editor.erase();
                    }
                    status = "Deleted";
                    break;
                default:
                    break;
            }
            return;
        }
        if (category == 2U) {
            if (item == 0U) {
                start_prompt(
                    PromptAction::editor_find,
                    "Search"
                );
            } else if (item == 1U) {
                start_prompt(
                    PromptAction::editor_replace_find,
                    "Search for"
                );
            } else {
                start_prompt(
                    PromptAction::editor_goto,
                    "Go to line",
                    std::to_string(editor.row() + 1U)
                );
            }
            return;
        }
        status = "No command in this menu";
    }

    bool handle_editor_menu(const ftxui::Event& event) {
        static constexpr std::array<std::size_t, 7> item_counts{
            3, 6, 3, 1, 1, 1, 1
        };
        if (event == ftxui::Event::Escape ||
            event == ftxui::Event::F9) {
            mode = Mode::editor;
            return true;
        }
        if (event == ftxui::Event::ArrowLeft) {
            editor_menu_category =
                (
                    editor_menu_category +
                    item_counts.size() - 1U
                ) %
                item_counts.size();
            editor_menu_item = 0;
            return true;
        }
        if (event == ftxui::Event::ArrowRight ||
            event == ftxui::Event::Tab) {
            editor_menu_category =
                (editor_menu_category + 1U) % item_counts.size();
            editor_menu_item = 0;
            return true;
        }
        if (event == ftxui::Event::ArrowUp) {
            const auto count = item_counts[editor_menu_category];
            editor_menu_item =
                (editor_menu_item + count - 1U) % count;
            return true;
        }
        if (event == ftxui::Event::ArrowDown) {
            const auto count = item_counts[editor_menu_category];
            editor_menu_item = (editor_menu_item + 1U) % count;
            return true;
        }
        if (event == ftxui::Event::Return) {
            execute_editor_menu_item();
            return true;
        }
        return false;
    }

    bool handle_editor_close_dialog(const ftxui::Event& event) {
        if (event == ftxui::Event::Escape) {
            mode = Mode::editor;
            return true;
        }
        if (event == ftxui::Event::ArrowLeft ||
            event == ftxui::Event::TabReverse) {
            editor_close_choice =
                (editor_close_choice + 2U) % 3U;
            return true;
        }
        if (event == ftxui::Event::ArrowRight ||
            event == ftxui::Event::Tab) {
            editor_close_choice =
                (editor_close_choice + 1U) % 3U;
            return true;
        }
        if (event == ftxui::Event::Character('y')) {
            editor_close_choice = 0;
        } else if (event == ftxui::Event::Character('n')) {
            editor_close_choice = 1;
        } else if (event == ftxui::Event::Character('c')) {
            editor_close_choice = 2;
        } else if (event != ftxui::Event::Return) {
            return false;
        }
        if (editor_close_choice == 2U) {
            mode = Mode::editor;
            return true;
        }
        if (editor_close_choice == 0U) {
            try {
                editor.save(false);
            } catch (const std::exception& error) {
                show_editor_error(error);
                return true;
            }
        }
        close_editor();
        return true;
    }

    bool handle_editor(const ftxui::Event& event) {
        if (event == ftxui::Event::F1) {
            mode = Mode::editor_help;
            return true;
        }
        if (event == ftxui::Event::F2 ||
            event == ftxui::Event::CtrlS) {
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
            start_prompt(
                PromptAction::editor_replace_find,
                "Search for"
            );
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
        if (event == ftxui::Event::F7 ||
            event == ftxui::Event::CtrlF) {
            start_prompt(PromptAction::editor_find, "Search");
            return true;
        }
        if (event == ftxui::Event::F8) {
            if (!editor.delete_selection()) {
                editor.erase();
            }
            return true;
        }
        if (event == ftxui::Event::F9) {
            editor_menu_category = 0;
            editor_menu_item = 0;
            mode = Mode::editor_menu;
            return true;
        }
        if (event == ftxui::Event::F10) {
            request_editor_close();
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
        if (event == ftxui::Event::CtrlG) {
            start_prompt(
                PromptAction::editor_goto,
                "Go to line",
                std::to_string(editor.row() + 1U)
            );
            return true;
        }
        if (event == ftxui::Event::CtrlZ) {
            status = editor.undo() ? "Undo" : "Nothing to undo";
            return true;
        }
        if (event == ftxui::Event::CtrlY) {
            status = editor.redo() ? "Redo" : "Nothing to redo";
            return true;
        }
        if (event == ftxui::Event::Escape) {
            editor.clear_selection();
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
            editor.page(-20);
        } else if (event == ftxui::Event::PageDown) {
            editor.page(20);
        } else if (event == ftxui::Event::Tab) {
            editor.insert("    ");
        } else if (event == ftxui::Event::Return) {
            editor.newline();
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
        if (event == ftxui::Event::Escape ||
            event == ftxui::Event::F2) {
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

    bool handle_pulldown(const ftxui::Event& event) {
        if (event == ftxui::Event::Escape ||
            event == ftxui::Event::F9) {
            mode = Mode::panels;
            return true;
        }
        if (event.is_mouse()) {
            auto mouse_event = event;
            const auto& mouse = mouse_event.mouse();
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
                        pulldown_item = 0;
                        return true;
                    }
                }
            }
        }
        if (event == ftxui::Event::ArrowLeft) {
            pulldown_category = (pulldown_category + 4U) % 5U;
            pulldown_item = 0;
            return true;
        }
        if (event == ftxui::Event::ArrowRight) {
            pulldown_category = (pulldown_category + 1U) % 5U;
            pulldown_item = 0;
            return true;
        }
        const auto item_count =
            pulldown_items()[pulldown_category].size();
        if (event == ftxui::Event::ArrowDown) {
            pulldown_item =
                item_count == 0U
                    ? 0U
                    : (pulldown_item + 1U) % item_count;
            return true;
        }
        if (event == ftxui::Event::ArrowUp) {
            pulldown_item =
                item_count == 0U
                    ? 0U
                    : (pulldown_item + item_count - 1U) %
                          item_count;
            return true;
        }
        if (event == ftxui::Event::Return) {
            try {
                execute_pulldown();
            } catch (const std::exception& error) {
                set_error(error);
                mode = Mode::panels;
            }
            return true;
        }
        return false;
    }

    bool handle_bookmarks(const ftxui::Event& event) {
        if (event == ftxui::Event::Escape ||
            event == ftxui::Event::F10) {
            mode = Mode::panels;
            return true;
        }
        if (event == ftxui::Event::ArrowDown &&
            bookmark_cursor + 1U < config.bookmarks.size()) {
            ++bookmark_cursor;
            return true;
        }
        if (event == ftxui::Event::ArrowUp &&
            bookmark_cursor > 0U) {
            --bookmark_cursor;
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
        if (event == ftxui::Event::Return &&
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
        if (event == ftxui::Event::Escape ||
            event == ftxui::Event::F10) {
            mode = Mode::panels;
            return true;
        }
        if (event == ftxui::Event::ArrowDown &&
            search_cursor + 1U < search_results.size()) {
            ++search_cursor;
            return true;
        }
        if (event == ftxui::Event::ArrowUp && search_cursor > 0U) {
            --search_cursor;
            return true;
        }
        if (event == ftxui::Event::Return &&
            search_cursor < search_results.size()) {
            const auto& result = search_results[search_cursor];
            try {
                const auto parent =
                    active().provider()->parent(result.path);
                active().change_directory(parent);
                active().find(result.name, true);
                mode = Mode::panels;
            } catch (const std::exception& error) {
                set_error(error);
            }
            return true;
        }
        return false;
    }

    bool handle_operation_dialog(const ftxui::Event& event) {
        if (event == ftxui::Event::Escape ||
            (event == ftxui::Event::Return && dialog_focus == 4U)) {
            mode = Mode::panels;
            status = "Operation cancelled";
            return true;
        }
        if (event == ftxui::Event::Tab) {
            dialog_focus = (dialog_focus + 1U) % 5U;
            return true;
        }
        if (event == ftxui::Event::TabReverse) {
            dialog_focus = (dialog_focus + 4U) % 5U;
            return true;
        }
        if (event == ftxui::Event::ArrowDown) {
            dialog_focus = std::min<std::size_t>(4U, dialog_focus + 1U);
            return true;
        }
        if (event == ftxui::Event::ArrowUp) {
            dialog_focus =
                dialog_focus == 0U ? 0U : dialog_focus - 1U;
            return true;
        }
        if (event == ftxui::Event::Character(' ') &&
            (dialog_focus == 1U || dialog_focus == 2U)) {
            if (dialog_focus == 1U) {
                dialog_preserve_attributes =
                    !dialog_preserve_attributes;
            } else {
                dialog_preserve_symlinks =
                    !dialog_preserve_symlinks;
            }
            return true;
        }
        if (event == ftxui::Event::Backspace &&
            dialog_focus == 0U) {
            erase_last_utf8(dialog_destination);
            return true;
        }
        if (event.is_character() && dialog_focus == 0U) {
            dialog_destination += event.character();
            return true;
        }
        if (event == ftxui::Event::Return) {
            try {
                confirm_operation_dialog();
            } catch (const std::exception& error) {
                set_error(error);
                mode = Mode::panels;
            }
            return true;
        }
        return false;
    }

    bool handle_delete_dialog(const ftxui::Event& event) {
        if (event == ftxui::Event::Escape) {
            mode = Mode::panels;
            status = "Delete cancelled";
            return true;
        }
        if (event == ftxui::Event::ArrowLeft ||
            event == ftxui::Event::ArrowRight ||
            event == ftxui::Event::Tab ||
            event == ftxui::Event::TabReverse) {
            dialog_focus = 1U - std::min<std::size_t>(1U, dialog_focus);
            return true;
        }
        if (event == ftxui::Event::Return) {
            if (dialog_focus == 0U) {
                try {
                    run_remove_operation();
                } catch (const std::exception& error) {
                    set_error(error);
                    mode = Mode::panels;
                }
            } else {
                mode = Mode::panels;
                status = "Delete cancelled";
            }
            return true;
        }
        return false;
    }

    bool handle_operation_progress(const ftxui::Event& event) {
        if (event == ftxui::Event::Escape ||
            event == ftxui::Event::F10 ||
            event == ftxui::Event::CtrlC ||
            event == ftxui::Event::Character('a')) {
            operation_cancellation.request();
            operation_pause_condition.notify_all();
            status = "Cancelling operation";
            return true;
        }
        if (event == ftxui::Event::Character(' ') ||
            event == ftxui::Event::Character('p')) {
            const auto paused = !operation_paused.load(
                std::memory_order_acquire
            );
            operation_paused.store(paused, std::memory_order_release);
            if (!paused) {
                operation_pause_condition.notify_all();
            }
            status = paused ? "Operation paused" : "Operation resumed";
            return true;
        }
        if (event == ftxui::Event::Character('s') &&
            operation_label != "Delete") {
            operation_skip.store(true, std::memory_order_release);
            operation_paused.store(false, std::memory_order_release);
            operation_pause_condition.notify_all();
            status = "Skipping current entry";
            return true;
        }
        return false;
    }

    bool handle_conflict_dialog(const ftxui::Event& event) {
        if (event == ftxui::Event::Escape) {
            mode = Mode::panels;
            status = "Operation cancelled";
            pending_destination.clear();
            return true;
        }
        if (event == ftxui::Event::ArrowLeft ||
            event == ftxui::Event::TabReverse) {
            dialog_focus = (dialog_focus + 2U) % 3U;
            return true;
        }
        if (event == ftxui::Event::ArrowRight ||
            event == ftxui::Event::Tab) {
            dialog_focus = (dialog_focus + 1U) % 3U;
            return true;
        }
        if (event != ftxui::Event::Return) {
            return false;
        }
        if (dialog_focus == 2U) {
            mode = Mode::panels;
            status = "Operation cancelled";
            pending_destination.clear();
            return true;
        }
        try {
            run_operation(
                pending_move,
                pending_destination,
                dialog_focus == 0U,
                pending_exact
            );
        } catch (const std::exception& error) {
            set_error(error);
            mode = Mode::panels;
        }
        pending_destination.clear();
        return true;
    }

    bool handle_panels(ftxui::Event event) {
        if (command_mode) {
            if (event == ftxui::Event::Escape) {
                command_mode = false;
                command_line.clear();
            } else if (event == ftxui::Event::Return) {
                execute_command();
            } else if (event == ftxui::Event::Backspace) {
                if (!command_line.empty()) {
                    command_line.pop_back();
                }
            } else if (event == ftxui::Event::ArrowUp) {
                if (!command_history.empty() &&
                    command_history_index > 0U) {
                    --command_history_index;
                    command_line =
                        command_history[command_history_index];
                }
            } else if (event == ftxui::Event::ArrowDown) {
                if (command_history_index + 1U <
                    command_history.size()) {
                    ++command_history_index;
                    command_line =
                        command_history[command_history_index];
                } else {
                    command_history_index =
                        command_history.size();
                    command_line.clear();
                }
            } else if (event.is_character()) {
                command_line += event.character();
            } else {
                return false;
            }
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
                            pulldown_item = 0;
                            mode = Mode::pulldown;
                            return true;
                        }
                    }
                }
                if (mouse.button == ftxui::Mouse::WheelUp) {
                    active().move_cursor(-3);
                    return true;
                }
                if (mouse.button == ftxui::Mouse::WheelDown) {
                    active().move_cursor(3);
                    return true;
                }
                Pane* pane = nullptr;
                std::vector<ftxui::Box>* boxes = nullptr;
                if (left_box.Contain(mouse.x, mouse.y)) {
                    active_index = 0;
                    pane = &left;
                    boxes = &left_entry_boxes;
                } else if (right_box.Contain(mouse.x, mouse.y)) {
                    active_index = 1;
                    pane = &right;
                    boxes = &right_entry_boxes;
                }
                if (pane != nullptr && boxes != nullptr &&
                    mouse.button == ftxui::Mouse::Left &&
                    mouse.motion == ftxui::Mouse::Pressed) {
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
                                if (!pane->enter()) {
                                    const auto* entry = pane->current();
                                    if (entry != nullptr &&
                                        entry->kind ==
                                            EntryKind::file) {
                                        viewer.open(
                                            pane->provider(),
                                            entry->path
                                        );
                                        mode = Mode::viewer;
                                    }
                                }
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
            if (event == ftxui::Event::Tab) {
                active_index = 1 - active_index;
            } else if (event == ftxui::Event::ArrowDown) {
                active().move_cursor(1);
            } else if (event == ftxui::Event::ArrowUp) {
                active().move_cursor(-1);
            } else if (event == ftxui::Event::PageDown) {
                active().page_cursor(20);
            } else if (event == ftxui::Event::PageUp) {
                active().page_cursor(-20);
            } else if (event == ftxui::Event::Home) {
                active().move_cursor(
                    -static_cast<int>(active().cursor())
                );
            } else if (event == ftxui::Event::End) {
                active().move_cursor(
                    static_cast<int>(active().entries().size())
                );
            } else if (event == ftxui::Event::Return) {
                if (!active().enter()) {
                    const auto* entry = active().current();
                    if (entry != nullptr && entry->kind == EntryKind::file) {
                        viewer.open(active().provider(), entry->path);
                        mode = Mode::viewer;
                    }
                }
            } else if (event == ftxui::Event::Backspace) {
                active().up();
            } else if (event == ftxui::Event::AltH) {
                active().history_back();
            } else if (event == ftxui::Event::AltL) {
                active().history_forward();
            } else if (event == ftxui::Event::Insert ||
                       event == ftxui::Event::Character(' ')) {
                active().toggle_selected();
            } else if (event == ftxui::Event::CtrlA) {
                active().select_all();
            } else if (event == ftxui::Event::CtrlR) {
                refresh();
                status = "Refreshed";
            } else if (event == ftxui::Event::CtrlC &&
                       operation_running.load(
                           std::memory_order_acquire
                       )) {
                operation_cancellation.request();
                status = "Cancelling operation";
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
                if (entry != nullptr && entry->kind == EntryKind::file) {
                    viewer.open(active().provider(), entry->path);
                    mode = Mode::viewer;
                }
            } else if (event == ftxui::Event::F4) {
                const auto* entry = active().current();
                if (entry != nullptr && entry->kind == EntryKind::file) {
                    editor.open(active().provider(), entry->path);
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
                pulldown_category = 0;
                pulldown_item = 0;
                mode = Mode::pulldown;
            } else if (event == ftxui::Event::F10) {
                if (operation_running.load(
                        std::memory_order_acquire
                    )) {
                    operation_cancellation.request();
                    status = "Cancel operation before quitting";
                    return true;
                }
                terminal->Exit();
            } else if (event.is_character()) {
                command_mode = true;
                command_line = event.character();
            } else {
                return false;
            }
        } catch (const std::exception& error) {
            set_error(error);
        }
        return true;
    }

    bool handle(const ftxui::Event& event) {
        if (mode == Mode::prompt) {
            return handle_prompt(event);
        }
        if (mode == Mode::viewer) {
            return handle_viewer(event);
        }
        if (mode == Mode::editor) {
            return handle_editor(event);
        }
        if (mode == Mode::editor_menu) {
            return handle_editor_menu(event);
        }
        if (mode == Mode::editor_help) {
            if (event == ftxui::Event::Escape ||
                event == ftxui::Event::F1 ||
                event == ftxui::Event::F10) {
                mode = Mode::editor;
                return true;
            }
            return false;
        }
        if (mode == Mode::editor_error) {
            if (event == ftxui::Event::Escape ||
                event == ftxui::Event::Return ||
                event == ftxui::Event::F10) {
                mode = Mode::editor;
                return true;
            }
            return false;
        }
        if (mode == Mode::editor_close_dialog) {
            return handle_editor_close_dialog(event);
        }
        if (mode == Mode::help) {
            if (event == ftxui::Event::Escape ||
                event == ftxui::Event::F1) {
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
            if (event == ftxui::Event::Escape ||
                event == ftxui::Event::F10) {
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
        if (mode == Mode::conflict_dialog) {
            return handle_conflict_dialog(event);
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
    impl_->editor.open(impl_->active().provider(), resolved);
    impl_->mode = Mode::editor;
}

std::string App::active_path() const {
    return impl_->active_index == 0
               ? impl_->left.path()
               : impl_->right.path();
}

int App::run() {
    auto terminal = ftxui::App::FullscreenAlternateScreen();
    terminal.ForceHandleCtrlC(false);
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
                implementation->operation_cancellation.request();
            }
            if (implementation->operation_thread.joinable()) {
                implementation->operation_thread.join();
            }
            implementation->terminal = nullptr;
        }

        ~OperationJoinGuard() {
            finish();
        }
    } operation_guard{impl_.get()};
    auto component = ftxui::Renderer([&] {
        return impl_->render();
    });
    component = ftxui::CatchEvent(
        std::move(component),
        [&](const ftxui::Event& event) {
            return impl_->handle(event);
        }
    );
    terminal.Loop(std::move(component));
    operation_guard.finish();
    impl_->config.show_hidden = impl_->left.show_hidden();
    impl_->config.sort = impl_->left.sort_field();
    impl_->config.sort_descending = impl_->left.descending();
    impl_->config.left_path = impl_->left.path();
    impl_->config.right_path = impl_->right.path();
    try {
        impl_->config.save(default_config_path());
    } catch (...) {
    }
    return 0;
}

}  // namespace mcpp
