#include "mcpp/app.hpp"

#include "mcpp/local_provider.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <ctime>
#include <functional>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <utility>

#include <ftxui/component/app.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

namespace mcpp {
namespace {

enum class Mode {
    panels,
    viewer,
    editor,
    prompt,
    help,
    menu,
};

enum class PromptAction {
    none,
    rename,
    copy,
    move,
    mkdir,
    remove,
    find,
    quick_cd,
    chmod,
    symlink,
    hardlink,
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

}  // namespace

struct App::Impl {
    Config config;
    ProviderPtr local;
    Pane left;
    Pane right;
    int active_index{0};
    Mode mode{Mode::panels};
    Mode mode_before_prompt{Mode::panels};
    PromptAction prompt_action{PromptAction::none};
    std::string prompt_title;
    std::string prompt_value;
    std::string status{"Ready"};
    OperationEngine operations;
    Cancellation operation_cancellation;
    TransferProgress progress;
    ViewerModel viewer;
    EditorModel editor;
    ftxui::App* terminal{nullptr};

    Impl(
        std::filesystem::path left_path,
        std::filesystem::path right_path,
        Config configured
    )
        : config(std::move(configured)),
          local(std::make_shared<LocalProvider>()),
          left(local, std::move(left_path).string()),
          right(local, std::move(right_path).string()) {
        left.set_show_hidden(config.show_hidden);
        right.set_show_hidden(config.show_hidden);
        left.set_sort(config.sort, false);
        right.set_sort(config.sort, false);
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
        const std::string& destination
    ) {
        const auto sources = active().selected_entries();
        if (sources.empty()) {
            throw std::runtime_error("no source selected");
        }
        operation_cancellation = Cancellation{};
        progress = {};
        const OperationOptions options{
            .overwrite = false,
            .preserve_permissions = true,
            .preserve_symlinks = true,
            .collision =
                [&](const Entry&, const Entry&) {
                    return config.confirm_overwrite
                               ? CollisionDecision::skip
                               : CollisionDecision::overwrite;
                },
        };
        const auto callback = [&](const TransferProgress& value) {
            progress = value;
            status =
                (move_operation ? "Moving " : "Copying ") +
                value.current_path + " (" +
                std::to_string(value.files_done) + "/" +
                std::to_string(value.files_total) + ")";
        };
        if (move_operation) {
            operations.move(
                *active().provider(),
                sources,
                *passive().provider(),
                destination,
                options,
                operation_cancellation,
                callback
            );
        } else {
            operations.copy(
                *active().provider(),
                sources,
                *passive().provider(),
                destination,
                options,
                operation_cancellation,
                callback
            );
        }
        active().clear_selection();
        refresh();
        status = move_operation ? "Move complete" : "Copy complete";
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
                    run_operation(false, prompt_value);
                    break;
                case PromptAction::move:
                    run_operation(true, prompt_value);
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
                        operations.remove(
                            *active().provider(),
                            active().selected_entries(),
                            operation_cancellation
                        );
                        active().clear_selection();
                        active().refresh();
                        status = "Removed";
                    }
                    break;
                case PromptAction::find:
                    status = active().find(prompt_value)
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

    ftxui::Element render_pane(Pane& pane, const bool focused) {
        using namespace ftxui;
        Elements rows;
        const auto& entries = pane.entries();
        rows.reserve(entries.size());
        for (std::size_t index = 0; index < entries.size(); ++index) {
            const auto& entry = entries[index];
            auto name = type_prefix(entry) + entry.name;
            auto row = hbox({
                text(pane.is_selected(entry) ? "*" : " "),
                text(name) | flex,
                text(entry.kind == EntryKind::directory
                         ? "<DIR>"
                         : format_size(entry.size)) |
                    size(WIDTH, EQUAL, 10),
                text(format_time(entry.modified)) |
                    size(WIDTH, EQUAL, 16),
            });
            if (index == pane.cursor() && focused) {
                row = row | inverted;
            } else if (pane.is_selected(entry)) {
                row = row | bold | color(Color::Cyan);
            }
            rows.push_back(std::move(row));
        }
        if (rows.empty()) {
            rows.push_back(text("  <empty>") | dim);
        }
        const auto* current = pane.current();
        const auto info =
            current == nullptr
                ? std::string{}
                : current->name + "  " + format_size(current->size) +
                      "  " + current->owner + ":" + current->group;
        auto title = text(
            " " + pane.provider()->display_name() + ": " +
            pane.path() + " "
        );
        if (focused) {
            title = title | bold | color(Color::Yellow);
        }
        return vbox({
                   title,
                   separator(),
                   vbox(std::move(rows)) | yframe | flex,
                   separator(),
                   text(info) | dim,
               }) |
               border | flex;
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
                   text("F2        Rename"),
                   text("F3        View"),
                   text("F4        Edit"),
                   text("F5/F6     Copy/Move"),
                   text("F7/F8     Mkdir/Delete"),
                   text("F9/F10    Menu/Quit"),
                   separator(),
                   text("Press Esc or F1"),
               }) |
               border | size(WIDTH, GREATER_THAN, 48) | center;
    }

    ftxui::Element render_menu() const {
        using namespace ftxui;
        return vbox({
                   text("Commands") | bold | center,
                   separator(),
                   text("h  Toggle hidden files"),
                   text("n  Sort by name"),
                   text("e  Sort by extension"),
                   text("s  Sort by size"),
                   text("t  Sort by modified time"),
                   text("/  Find name"),
                   text("g  Change directory"),
                   text("c  Change permissions"),
                   text("l  Create symbolic link"),
                   text("L  Create hard link"),
                   text("r  Refresh panes"),
                   separator(),
                   text("Esc closes menu"),
               }) |
               border | size(WIDTH, GREATER_THAN, 48) | center;
    }

    ftxui::Element render() {
        using namespace ftxui;
        Element body = hbox({
            render_pane(left, active_index == 0),
            render_pane(right, active_index == 1),
        });
        if (mode == Mode::viewer) {
            body = render_viewer();
        } else if (mode == Mode::editor) {
            body = render_editor();
        }
        const auto progress_text =
            progress.files_total == 0U
                ? std::string{}
                : " [" + std::to_string(progress.files_done) + "/" +
                      std::to_string(progress.files_total) + "]";
        auto document = vbox({
            body | flex,
            text(" " + status + progress_text) | inverted,
            hbox({
                text(" 1 Help "),
                text(" 2 Rename "),
                text(" 3 View "),
                text(" 4 Edit "),
                text(" 5 Copy "),
                text(" 6 Move "),
                text(" 7 Mkdir "),
                text(" 8 Delete "),
                text(" 9 Menu "),
                text(" 10 Quit "),
            }) | bgcolor(Color::Blue) | color(Color::White),
        });
        if (mode == Mode::prompt) {
            document = dbox({
                document,
                vbox({
                    text(prompt_title) | bold,
                    separator(),
                    text("> " + prompt_value),
                    separator(),
                    text("Enter confirms; Esc cancels") | dim,
                }) |
                    border | clear_under |
                    size(WIDTH, GREATER_THAN, 54) | center,
            });
        } else if (mode == Mode::help) {
            document = dbox({
                document,
                render_help() | clear_under,
            });
        } else if (mode == Mode::menu) {
            document = dbox({
                document,
                render_menu() | clear_under,
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
                prompt_value.pop_back();
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
            start_prompt(PromptAction::find, "Find in panel");
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

    bool handle_menu(const ftxui::Event& event) {
        if (event == ftxui::Event::Escape ||
            event == ftxui::Event::F9) {
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
                active().set_sort(SortField::name, false);
            } else if (key == "e") {
                active().set_sort(SortField::extension, false);
            } else if (key == "s") {
                active().set_sort(SortField::size, false);
            } else if (key == "t") {
                active().set_sort(SortField::modified, false);
            } else if (key == "/") {
                start_prompt(PromptAction::find, "Find name");
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
            }
        } catch (const std::exception& error) {
            set_error(error);
        }
        return true;
    }

    bool handle_panels(const ftxui::Event& event) {
        try {
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
            } else if (event == ftxui::Event::Insert ||
                       event == ftxui::Event::Character(' ')) {
                active().toggle_selected();
            } else if (event == ftxui::Event::CtrlA) {
                active().select_all();
            } else if (event == ftxui::Event::CtrlR) {
                refresh();
                status = "Refreshed";
            } else if (event == ftxui::Event::F1) {
                mode = Mode::help;
            } else if (event == ftxui::Event::F2) {
                const auto* entry = active().current();
                start_prompt(
                    PromptAction::rename,
                    "Rename",
                    entry == nullptr ? "" : entry->name
                );
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
                start_prompt(
                    PromptAction::move,
                    "Move to directory",
                    passive().path()
                );
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
                mode = Mode::menu;
            } else if (event == ftxui::Event::F10) {
                terminal->Exit();
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
        if (mode == Mode::menu) {
            return handle_menu(event);
        }
        return handle_panels(event);
    }
};

App::App(
    std::filesystem::path left,
    std::filesystem::path right,
    Config config
)
    : impl_(std::make_unique<Impl>(
          std::move(left),
          std::move(right),
          std::move(config)
      )) {}

App::~App() = default;

int App::run() {
    auto terminal = ftxui::App::FullscreenAlternateScreen();
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
    impl_->config.left_path = impl_->left.path();
    impl_->config.right_path = impl_->right.path();
    try {
        impl_->config.save(default_config_path());
    } catch (...) {
    }
    return 0;
}

}  // namespace mcpp
