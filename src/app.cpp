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
#include <ftxui/screen/terminal.hpp>

namespace mcpp {
namespace {

enum class Mode {
    panels,
    viewer,
    editor,
    prompt,
    help,
    user_menu,
    pulldown,
    search_results,
    bookmarks,
    information,
};

enum class PromptAction {
    none,
    rename,
    copy,
    move,
    move_exact,
    copy_overwrite,
    move_overwrite,
    move_exact_overwrite,
    mkdir,
    remove,
    find,
    viewer_find,
    editor_find,
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
            .preserve_permissions = true,
            .preserve_symlinks = true,
            .collision =
                [&](const Entry&, const Entry&) {
                    return overwrite
                               ? CollisionDecision::overwrite
                               : CollisionDecision::skip;
                },
        };
        operation_running.store(true, std::memory_order_release);
        status = move_operation ? "Starting move" : "Starting copy";
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
                            if (terminal == nullptr) {
                                return;
                            }
                            terminal->Post([
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
                    terminal->Post([
                        this,
                        move_operation,
                        source_index
                    ] {
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
                        operation_running.store(
                            false,
                            std::memory_order_release
                        );
                    });
                } catch (const OperationCancelled&) {
                    terminal->Post([this] {
                        status = "Operation cancelled";
                        operation_running.store(
                            false,
                            std::memory_order_release
                        );
                    });
                } catch (const std::exception& error) {
                    const auto message = std::string(error.what());
                    terminal->Post([this, message] {
                        status = "Error: " + message;
                        operation_running.store(
                            false,
                            std::memory_order_release
                        );
                    });
                }
            }
        );
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
        status = "Starting delete";
        operation_thread = std::jthread(
            [this, provider, entries, source_index] {
                try {
                    operations.remove(
                        *provider,
                        entries,
                        operation_cancellation,
                        [this](const TransferProgress& value) {
                            terminal->Post([this, value] {
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
                    terminal->Post([this, source_index] {
                        try {
                            (source_index == 0 ? left : right)
                                .clear_selection();
                            refresh();
                            status = "Delete complete";
                        } catch (const std::exception& error) {
                            set_error(error);
                        }
                        operation_running.store(
                            false,
                            std::memory_order_release
                        );
                    });
                } catch (const OperationCancelled&) {
                    terminal->Post([this] {
                        status = "Delete cancelled";
                        operation_running.store(
                            false,
                            std::memory_order_release
                        );
                    });
                } catch (const std::exception& error) {
                    const auto message = std::string(error.what());
                    terminal->Post([this, message] {
                        status = "Error: " + message;
                        operation_running.store(
                            false,
                            std::memory_order_release
                        );
                    });
                }
            }
        );
    }

    int run_process(
        const std::string& command,
        const bool interactive
    ) {
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
                case PromptAction::copy:
                    if (config.confirm_overwrite &&
                        has_collision(prompt_value)) {
                        pending_destination = prompt_value;
                        mode_before_prompt = Mode::panels;
                        mode = Mode::prompt;
                        prompt_action =
                            PromptAction::copy_overwrite;
                        prompt_title =
                            "Type yes to overwrite existing entries";
                        prompt_value.clear();
                        return;
                    }
                    run_operation(
                        false,
                        prompt_value,
                        !config.confirm_overwrite
                    );
                    break;
                case PromptAction::move:
                    if (config.confirm_overwrite &&
                        has_collision(prompt_value)) {
                        pending_destination = prompt_value;
                        mode_before_prompt = Mode::panels;
                        mode = Mode::prompt;
                        prompt_action =
                            PromptAction::move_overwrite;
                        prompt_title =
                            "Type yes to overwrite existing entries";
                        prompt_value.clear();
                        return;
                    }
                    run_operation(
                        true,
                        prompt_value,
                        !config.confirm_overwrite
                    );
                    break;
                case PromptAction::move_exact:
                    if (config.confirm_overwrite &&
                        passive().provider()->exists(prompt_value)) {
                        pending_destination = prompt_value;
                        mode_before_prompt = Mode::panels;
                        mode = Mode::prompt;
                        prompt_action =
                            PromptAction::move_exact_overwrite;
                        prompt_title =
                            "Type yes to overwrite existing entry";
                        prompt_value.clear();
                        return;
                    }
                    run_operation(
                        true,
                        prompt_value,
                        !config.confirm_overwrite,
                        true
                    );
                    break;
                case PromptAction::copy_overwrite:
                    if (prompt_value == "yes") {
                        run_operation(
                            false,
                            pending_destination,
                            true
                        );
                    } else {
                        status = "Copy cancelled";
                    }
                    pending_destination.clear();
                    break;
                case PromptAction::move_overwrite:
                    if (prompt_value == "yes") {
                        run_operation(
                            true,
                            pending_destination,
                            true
                        );
                    } else {
                        status = "Move cancelled";
                    }
                    pending_destination.clear();
                    break;
                case PromptAction::move_exact_overwrite:
                    if (prompt_value == "yes") {
                        run_operation(
                            true,
                            pending_destination,
                            true,
                            true
                        );
                    } else {
                        status = "Move cancelled";
                    }
                    pending_destination.clear();
                    break;
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
                case PromptAction::remove:
                    if (prompt_value == "yes") {
                        run_remove_operation();
                    }
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

    ftxui::Element render_editor() {
        using namespace ftxui;
        Elements elements;
        const auto& lines = editor.lines();
        const auto first = editor.row() > 15U ? editor.row() - 15U : 0U;
        const auto end = std::min(lines.size(), first + 40U);
        for (std::size_t index = first; index < end; ++index) {
            auto row = hbox({
                text(std::to_string(index + 1U)) |
                    size(WIDTH, EQUAL, 6) | dim,
                text(lines[index]),
            });
            if (index == editor.row()) {
                row = row | inverted;
            }
            elements.push_back(std::move(row));
        }
        return vbox({
                   text(
                       "Edit: " + editor.path() +
                       (editor.dirty() ? " *" : "")
                   ) |
                       bold,
                   separator(),
                   vbox(std::move(elements)) | yframe | flex,
                   separator(),
                   text("F2/Ctrl-S Save  F4/Esc Close"),
               }) |
               border;
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
                    start_prompt(
                        PromptAction::copy,
                        "Copy to directory",
                        passive().path()
                    );
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
                    if (sources.size() == 1U) {
                        start_prompt(
                            PromptAction::move_exact,
                            "Move or rename to",
                            passive().provider()->join(
                                passive().path(),
                                sources.front().name
                            )
                        );
                    } else {
                        start_prompt(
                            PromptAction::move,
                            "Move to directory",
                            passive().path()
                        );
                    }
                    return;
                }
                case 5:
                    start_prompt(PromptAction::mkdir, "New directory");
                    return;
                case 6:
                    start_prompt(
                        PromptAction::remove,
                        "Type yes to delete",
                        config.confirm_delete ? "" : "yes"
                    );
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
        } else if (mode == Mode::editor) {
            body = render_editor();
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
        const auto key = [](const std::string& number,
                            const std::string& label) {
            return hbox({
                       text(number) | inverted,
                       text(label),
                   }) |
                   flex;
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
            if (!prompt_value.empty()) {
                auto begin = prompt_value.size() - 1U;
                while (begin > 0U &&
                       (
                           static_cast<unsigned char>(
                               prompt_value[begin]
                           ) &
                           0xC0U
                       ) == 0x80U) {
                    --begin;
                }
                prompt_value.erase(begin);
            }
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

    bool handle_editor(const ftxui::Event& event) {
        if (event == ftxui::Event::F2 ||
            event == ftxui::Event::CtrlS) {
            try {
                editor.save(false);
                status = "Saved";
            } catch (const std::exception& error) {
                set_error(error);
            }
            return true;
        }
        if (event == ftxui::Event::CtrlF) {
            start_prompt(PromptAction::editor_find, "Find in file");
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
        if (event == ftxui::Event::Escape ||
            event == ftxui::Event::F4) {
            if (editor.dirty()) {
                status = "Unsaved changes; press F2 to save";
            } else {
                editor.close();
                mode = Mode::panels;
            }
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

    bool handle_internal_menu_legacy(const ftxui::Event& event) {
        if (event == ftxui::Event::Escape ||
            event == ftxui::Event::F2) {
            mode = Mode::panels;
            return true;
        }

        if (!event.is_character()) {
            return false;
        }
        const auto key = event.character();
        mode = Mode::panels;
        try {
            if (key == "h") {
                active().set_show_hidden(!active().show_hidden());
            } else if (key == "n") {
                sort_active(SortField::name);
            } else if (key == "e") {
                sort_active(SortField::extension);
            } else if (key == "s") {
                sort_active(SortField::size);
            } else if (key == "t") {
                sort_active(SortField::modified);
            } else if (key == "/") {
                start_prompt(PromptAction::find, "Find name");
            } else if (key == "f") {
                start_prompt(
                    PromptAction::recursive_search,
                    "Recursive name pattern",
                    "*"
                );
            } else if (key == "d") {
                const DirectoryComparator comparator;
                const auto differences = comparator.compare(
                    *left.provider(),
                    left.path(),
                    *right.provider(),
                    right.path(),
                    true,
                    operation_cancellation
                );
                std::vector<std::string> left_paths;
                std::vector<std::string> right_paths;
                for (const auto& difference : differences) {
                    if (difference.relative_path.find('/') !=
                        std::string::npos) {
                        continue;
                    }
                    if (difference.kind !=
                        DifferenceKind::right_only) {
                        left_paths.push_back(left.provider()->join(
                            left.path(),
                            difference.relative_path
                        ));
                    }
                    if (difference.kind !=
                        DifferenceKind::left_only) {
                        right_paths.push_back(right.provider()->join(
                            right.path(),
                            difference.relative_path
                        ));
                    }
                }
                left.select_paths(left_paths);
                right.select_paths(right_paths);
                status =
                    std::to_string(differences.size()) +
                    " difference(s)";
            } else if (key == "+") {
                start_prompt(
                    PromptAction::select_pattern,
                    "Select name pattern",
                    "*"
                );
            } else if (key == "-") {
                start_prompt(
                    PromptAction::unselect_pattern,
                    "Unselect name pattern",
                    "*"
                );
            } else if (key == "*") {
                active().invert_selection();
                status = "Selection inverted";
            } else if (key == "i") {
                mode = Mode::information;
            } else if (key == "b") {
                if (std::find(
                        config.bookmarks.begin(),
                        config.bookmarks.end(),
                        active().path()
                    ) == config.bookmarks.end()) {
                    config.bookmarks.push_back(active().path());
                }
                status = "Added to hotlist";
            } else if (key == "B") {
                bookmark_cursor = 0;
                mode = Mode::bookmarks;
            } else if (key == "g") {
                start_prompt(
                    PromptAction::quick_cd,
                    "Change directory",
                    active().path()
                );
            } else if (key == "c") {
                start_prompt(
                    PromptAction::chmod,
                    "Octal permissions",
                    "0644"
                );
            } else if (key == "o") {
                const auto* entry = active().current();
                start_prompt(
                    PromptAction::chown,
                    "Owner:group (blank keeps current)",
                    entry == nullptr
                        ? ""
                        : entry->owner + ":" + entry->group
                );
            } else if (key == "l") {
                start_prompt(
                    PromptAction::symlink,
                    "Symbolic link name"
                );
            } else if (key == "L") {
                start_prompt(
                    PromptAction::hardlink,
                    "Hard link name"
                );
            } else if (key == "r") {
                refresh();
            } else if (key == "v") {
                detailed_view = !detailed_view;
                status = detailed_view
                             ? "Full panel view"
                             : "Brief panel view";
            } else if (key == "q") {
                quick_view = !quick_view;
                quick_viewer.close();
                quick_view_path.clear();
                status = quick_view
                             ? "Quick view enabled"
                             : "Quick view disabled";
            }
        } catch (const std::exception& error) {
            set_error(error);
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
        const auto command = expand_user_menu_command(
            match->command,
            selected == nullptr ? "" : selected->path,
            active().path(),
            passive().path()
        );
        mode = Mode::panels;
        try {
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
                start_prompt(
                    PromptAction::copy,
                    "Copy to directory",
                    passive().path()
                );
            } else if (event == ftxui::Event::F6) {
                const auto sources = active().selected_entries();
                if (sources.size() == 1U) {
                    start_prompt(
                        PromptAction::move_exact,
                        "Move or rename to",
                        passive().provider()->join(
                            passive().path(),
                            sources.front().name
                        )
                    );
                } else {
                    start_prompt(
                        PromptAction::move,
                        "Move to directory",
                        passive().path()
                    );
                }
            } else if (event == ftxui::Event::F7) {
                start_prompt(
                    PromptAction::mkdir,
                    "New directory"
                );
            } else if (event == ftxui::Event::F8) {
                start_prompt(
                    PromptAction::remove,
                    "Type yes to delete",
                    config.confirm_delete ? "" : "yes"
                );
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
