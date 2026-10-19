#pragma once

#include "mcpp/provider.hpp"

#include <set>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace mcpp {

class Pane {
public:
    Pane(ProviderPtr provider, std::string path);

    void refresh(const Cancellation& cancellation = {}, bool force = false);
    void move_cursor(int amount);
    void page_cursor(int amount);
    void set_viewport_rows(std::size_t rows);
    [[nodiscard]] std::size_t first_row() const noexcept;
    bool enter(const Cancellation& cancellation = {});
    bool up(const Cancellation& cancellation = {});
    bool history_back(const Cancellation& cancellation = {});
    bool history_forward(const Cancellation& cancellation = {});
    void change_directory(
        std::string path,
        const Cancellation& cancellation = {}
    );
    void toggle_selected(int movement = 1);
    void select_all();
    void clear_selection();
    void unmark_paths(const std::vector<std::string>& paths);
    void select_paths(const std::vector<std::string>& paths);
    void set_panelized(std::vector<Entry> entries);
    void restore_panelized();
    [[nodiscard]] bool panelized() const noexcept;
    void select_pattern(std::string_view pattern, bool selected);
    void invert_selection();
    bool find(
        std::string_view needle,
        bool case_sensitive = false,
        bool include_current = false,
        bool prefix = false
    );

    [[nodiscard]] const ProviderPtr& provider() const noexcept;
    [[nodiscard]] const std::string& path() const noexcept;
    [[nodiscard]] const std::vector<Entry>& entries() const noexcept;
    [[nodiscard]] std::size_t cursor() const noexcept;
    [[nodiscard]] const Entry* current() const noexcept;
    [[nodiscard]] std::vector<Entry> selected_entries() const;
    [[nodiscard]] bool is_selected(const Entry& entry) const;
    [[nodiscard]] std::size_t marked_count() const noexcept;
    [[nodiscard]] std::uint64_t marked_size() const noexcept;

    void set_show_hidden(bool value);
    void set_show_backups(bool value);
    void set_fast_reload(bool value);
    void set_navigation(bool page_scrolling, bool center_scrolling);
    void set_shell_patterns(bool value);
    void set_follow_links(bool value) noexcept { follow_links_ = value; }
    [[nodiscard]] std::string navigation_path() const;
    void swap_contents(Pane& other);
    [[nodiscard]] bool show_hidden() const noexcept;
    void set_directories_first(bool value);
    [[nodiscard]] bool directories_first() const noexcept;
    void set_filter(std::string pattern);
    [[nodiscard]] const std::string& filter() const noexcept;
    void set_sort(SortField field, bool descending);
    void set_sort_options(SortField field, bool descending, bool case_sensitive, bool executable_first);
    [[nodiscard]] SortField sort_field() const noexcept;
    [[nodiscard]] bool descending() const noexcept;
    [[nodiscard]] bool sort_case_sensitive() const noexcept { return sort_case_sensitive_; }
    [[nodiscard]] bool executable_first() const noexcept { return executable_first_; }
    void set_reverse_files_only(bool value);
    void set_directory_sizes(const std::map<std::string, std::uint64_t>& sizes);
    void clear_directory_sizes();

private:
    void navigate(
        std::string path,
        bool record,
        const Cancellation& cancellation
    );
    void restore_child_cursor(std::string_view child_path);
    void keep_cursor_visible();
    [[nodiscard]] ListRequest list_request() const;
    [[nodiscard]] bool matches_pattern(std::string_view pattern, std::string_view name) const;

    ProviderPtr provider_;
    std::string path_;
    std::vector<Entry> entries_;
    std::set<std::string, std::less<>> selected_;
    std::size_t cursor_{0};
    std::size_t first_row_{0};
    std::size_t viewport_rows_{1};
    bool show_hidden_{false};
    bool show_backups_{true};
    bool fast_reload_{false};
    bool page_scrolling_{false};
    bool center_scrolling_{false};
    bool shell_patterns_{true};
    bool follow_links_{true};
    std::optional<std::string> directory_revision_;
    bool directories_first_{true};
    std::string filter_;
    SortField sort_{SortField::name};
    bool descending_{false};
    bool sort_case_sensitive_{true};
    bool executable_first_{false};
    bool reverse_files_only_{true};
    bool panelized_{false};
    std::vector<Entry> panelized_entries_;
    std::string panelized_root_;
    std::vector<std::string> history_;
    std::map<std::string, std::uint64_t> directory_sizes_;
    std::size_t history_index_{0};
};

}  // namespace mcpp
