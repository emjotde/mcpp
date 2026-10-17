#pragma once

#include "mcpp/provider.hpp"

#include <set>
#include <string>
#include <vector>

namespace mcpp {

class Pane {
public:
    Pane(ProviderPtr provider, std::string path);

    void refresh(const Cancellation& cancellation = {});
    void move_cursor(int amount);
    void page_cursor(int amount);
    bool enter(const Cancellation& cancellation = {});
    bool up(const Cancellation& cancellation = {});
    bool history_back(const Cancellation& cancellation = {});
    bool history_forward(const Cancellation& cancellation = {});
    void change_directory(
        std::string path,
        const Cancellation& cancellation = {}
    );
    void toggle_selected();
    void select_all();
    void clear_selection();
    void select_paths(const std::vector<std::string>& paths);
    void select_pattern(std::string_view pattern, bool selected);
    void invert_selection();
    bool find(std::string_view needle, bool case_sensitive = false);

    [[nodiscard]] const ProviderPtr& provider() const noexcept;
    [[nodiscard]] const std::string& path() const noexcept;
    [[nodiscard]] const std::vector<Entry>& entries() const noexcept;
    [[nodiscard]] std::size_t cursor() const noexcept;
    [[nodiscard]] const Entry* current() const noexcept;
    [[nodiscard]] std::vector<Entry> selected_entries() const;
    [[nodiscard]] bool is_selected(const Entry& entry) const;

    void set_show_hidden(bool value);
    [[nodiscard]] bool show_hidden() const noexcept;
    void set_sort(SortField field, bool descending);
    [[nodiscard]] SortField sort_field() const noexcept;
    [[nodiscard]] bool descending() const noexcept;

private:
    void navigate(
        std::string path,
        bool record,
        const Cancellation& cancellation
    );

    ProviderPtr provider_;
    std::string path_;
    std::vector<Entry> entries_;
    std::set<std::string, std::less<>> selected_;
    std::size_t cursor_{0};
    bool show_hidden_{false};
    SortField sort_{SortField::name};
    bool descending_{false};
    std::vector<std::string> history_;
    std::size_t history_index_{0};
};

}  // namespace mcpp
