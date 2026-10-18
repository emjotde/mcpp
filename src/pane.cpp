#include "mcpp/pane.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace mcpp {
namespace {

bool is_parent_entry(const Entry& entry) {
    return entry.revision == "mcpp:parent";
}

bool wildcard_match(
    const std::string_view pattern,
    const std::string_view value
) {
    std::size_t pattern_index = 0;
    std::size_t value_index = 0;
    std::size_t star = std::string_view::npos;
    std::size_t retry = 0;
    while (value_index < value.size()) {
        if (pattern_index < pattern.size() &&
            (pattern[pattern_index] == '?' ||
             pattern[pattern_index] == value[value_index])) {
            ++pattern_index;
            ++value_index;
        } else if (pattern_index < pattern.size() &&
                   pattern[pattern_index] == '*') {
            star = pattern_index++;
            retry = value_index;
        } else if (star != std::string_view::npos) {
            pattern_index = star + 1U;
            value_index = ++retry;
        } else {
            return false;
        }
    }
    while (pattern_index < pattern.size() &&
           pattern[pattern_index] == '*') {
        ++pattern_index;
    }
    return pattern_index == pattern.size();
}

}  // namespace

Pane::Pane(ProviderPtr provider, std::string path)
    : provider_(std::move(provider)) {
    if (!provider_) {
        throw std::invalid_argument("pane provider is null");
    }
    path_ = provider_->normalize(path);
    history_.push_back(path_);
    refresh();
}

void Pane::refresh(const Cancellation& cancellation) {
    const auto previous = current() != nullptr
                              ? current()->path
                              : std::string{};
    if (panelized_) {
        std::vector<Entry> refreshed;
        refreshed.reserve(panelized_entries_.size());
        for (const auto& entry : panelized_entries_) {
            if (!provider_->exists(entry.path)) {
                continue;
            }
            auto current_entry =
                provider_->stat(entry.path, cancellation);
            current_entry.name = entry.name;
            if (!filter_.empty() &&
                current_entry.kind != EntryKind::directory &&
                !current_entry.target_is_directory &&
                !wildcard_match(filter_, current_entry.name)) {
                continue;
            }
            refreshed.push_back(std::move(current_entry));
        }
        entries_ = std::move(refreshed);
        std::erase_if(
            selected_,
            [&](const std::string& path) {
                return std::ranges::none_of(
                    entries_,
                    [&](const Entry& entry) {
                        return entry.path == path;
                    }
                );
            }
        );
        cursor_ = 0;
        const auto match = std::find_if(
            entries_.begin(),
            entries_.end(),
            [&](const Entry& entry) {
                return entry.path == previous;
            }
        );
        if (match != entries_.end()) {
            cursor_ = static_cast<std::size_t>(
                std::distance(entries_.begin(), match)
            );
        }
        return;
    }
    entries_ = provider_->list(
        path_,
        {
            .show_hidden = show_hidden_,
            .directories_first = directories_first_,
            .sort = sort_,
            .descending = descending_,
        },
        cancellation
    );
    if (!filter_.empty()) {
        std::erase_if(
            entries_,
            [&](const Entry& entry) {
                return entry.kind != EntryKind::directory &&
                       !entry.target_is_directory &&
                       !wildcard_match(filter_, entry.name);
            }
        );
    }
    const auto parent_path = provider_->parent(path_);
    if (parent_path != path_) {
        Entry parent_entry;
        try {
            parent_entry = provider_->stat(parent_path, cancellation);
        } catch (...) {
            parent_entry.path = parent_path;
            parent_entry.kind = EntryKind::directory;
        }
        parent_entry.name = "..";
        parent_entry.path = parent_path;
        parent_entry.kind = EntryKind::directory;
        parent_entry.revision = "mcpp:parent";
        parent_entry.hidden = false;
        parent_entry.target_is_directory = false;
        entries_.insert(
            entries_.begin(),
            std::move(parent_entry)
        );
    }
    cursor_ = 0;
    if (!previous.empty()) {
        const auto match = std::find_if(
            entries_.begin(),
            entries_.end(),
            [&](const Entry& entry) {
                return entry.path == previous;
            }
        );
        if (match != entries_.end()) {
            cursor_ = static_cast<std::size_t>(
                std::distance(entries_.begin(), match)
            );
        }
    }
    std::erase_if(selected_, [&](const std::string& selected) {
        return std::none_of(
            entries_.begin(),
            entries_.end(),
            [&](const Entry& entry) {
                return entry.path == selected;
            }
        );
    });
}

void Pane::move_cursor(const int amount) {
    if (entries_.empty()) {
        cursor_ = 0;
        return;
    }
    const auto current_index = static_cast<long long>(cursor_);
    const auto last = static_cast<long long>(entries_.size() - 1U);
    cursor_ = static_cast<std::size_t>(
        std::clamp(current_index + amount, 0LL, last)
    );
}

void Pane::page_cursor(const int amount) {
    move_cursor(amount);
}

bool Pane::enter(const Cancellation& cancellation) {
    const auto* entry = current();
    if (entry == nullptr ||
        (entry->kind != EntryKind::directory &&
         !entry->target_is_directory)) {
        return false;
    }
    if (is_parent_entry(*entry)) {
         return up(cancellation);
    }
    navigate(entry->path, true, cancellation);
    return true;
}

bool Pane::up(const Cancellation& cancellation) {
    const auto target = provider_->parent(path_);
    if (target == path_) {
        return false;
    }
    const auto child_path = path_;
    navigate(target, true, cancellation);
    restore_child_cursor(child_path);
    return true;
}

void Pane::restore_child_cursor(
    const std::string_view child_path
) {
    if (provider_->parent(child_path) != path_) {
        return;
    }
    const auto old_name = provider_->basename(child_path);
    const auto match = std::find_if(
        entries_.begin(),
        entries_.end(),
        [&](const Entry& entry) {
            return entry.name == old_name;
        }
    );
    if (match != entries_.end()) {
        cursor_ = static_cast<std::size_t>(
            std::distance(entries_.begin(), match)
        );
    }
}

void Pane::change_directory(
    std::string path,
    const Cancellation& cancellation
) {
    navigate(std::move(path), true, cancellation);
}

void Pane::navigate(
    std::string path,
    const bool record,
    const Cancellation& cancellation
) {
    const auto normalized = provider_->normalize(path);
    const auto entry = provider_->stat(normalized, cancellation);
    if (entry.kind != EntryKind::directory &&
        !entry.target_is_directory) {
        throw std::runtime_error(
            "not a directory: " + normalized
        );
    }
    const auto previous_path = path_;
    const auto previous_entries = entries_;
    const auto previous_selected = selected_;
    const auto previous_cursor = cursor_;
    const auto previous_panelized = panelized_;
    const auto previous_panelized_entries = panelized_entries_;
    path_ = std::move(normalized);
    panelized_ = false;
    panelized_entries_.clear();
    selected_.clear();
    cursor_ = 0;
    try {
        refresh(cancellation);
    } catch (...) {
        path_ = previous_path;
        entries_ = previous_entries;
        selected_ = previous_selected;
        cursor_ = previous_cursor;
        panelized_ = previous_panelized;
        panelized_entries_ = previous_panelized_entries;
        throw;
    }
    if (record) {
        if (history_index_ + 1U < history_.size()) {
            history_.erase(
                history_.begin() +
                    static_cast<std::ptrdiff_t>(history_index_ + 1U),
                history_.end()
            );
        }
        if (history_.empty() || history_.back() != path_) {
            history_.push_back(path_);
        }
        history_index_ = history_.size() - 1U;
    }
}

bool Pane::history_back(const Cancellation& cancellation) {
    if (history_index_ == 0U) {
        return false;
    }
    const auto child_path = path_;
    --history_index_;
    try {
        navigate(history_[history_index_], false, cancellation);
        restore_child_cursor(child_path);
    } catch (...) {
        ++history_index_;
        throw;
    }
    return true;
}

bool Pane::history_forward(const Cancellation& cancellation) {
    if (history_index_ + 1U >= history_.size()) {
        return false;
    }
    const auto child_path = path_;
    ++history_index_;
    try {
        navigate(history_[history_index_], false, cancellation);
        restore_child_cursor(child_path);
    } catch (...) {
        --history_index_;
        throw;
    }
    return true;
}

void Pane::toggle_selected(const int movement) {
    const auto* entry = current();
    if (entry == nullptr) {
        return;
    }
    if (is_parent_entry(*entry)) {
        move_cursor(movement);
        return;
    }
    if (selected_.contains(entry->path)) {
        selected_.erase(entry->path);
    } else {
        selected_.insert(entry->path);
    }
    move_cursor(movement);
}

void Pane::select_all() {
    for (const auto& entry : entries_) {
        if (is_parent_entry(entry)) {
            continue;
        }
        selected_.insert(entry.path);
    }
}

void Pane::clear_selection() {
    selected_.clear();
}

void Pane::unmark_paths(const std::vector<std::string>& paths) {
    for (const auto& path : paths) {
        selected_.erase(path);
    }
}

void Pane::select_paths(const std::vector<std::string>& paths) {
    selected_.clear();
    selected_.insert(paths.begin(), paths.end());
}

void Pane::set_panelized(std::vector<Entry> entries) {
    panelized_entries_ = std::move(entries);
    entries_ = panelized_entries_;
    if (!filter_.empty()) {
        std::erase_if(
            entries_,
            [&](const Entry& entry) {
                return entry.kind != EntryKind::directory &&
                       !entry.target_is_directory &&
                       !wildcard_match(filter_, entry.name);
            }
        );
    }
    selected_.clear();
    cursor_ = 0;
    panelized_ = true;
}

bool Pane::panelized() const noexcept {
    return panelized_;
}

void Pane::select_pattern(
    const std::string_view pattern,
    const bool selected
) {
    for (const auto& entry : entries_) {
        if (is_parent_entry(entry)) {
            continue;
        }
        if (!wildcard_match(pattern, entry.name)) {
            continue;
        }
        if (selected) {
            selected_.insert(entry.path);
        } else {
            selected_.erase(entry.path);
        }
    }
}

void Pane::invert_selection() {
    for (const auto& entry : entries_) {
        if (is_parent_entry(entry) ||
            entry.kind == EntryKind::directory) {
            continue;
        }
        if (selected_.contains(entry.path)) {
            selected_.erase(entry.path);
        } else {
            selected_.insert(entry.path);
        }
    }
}

bool Pane::find(
    const std::string_view needle,
    const bool case_sensitive,
    const bool include_current,
    const bool prefix
) {
    if (needle.empty() || entries_.empty()) {
        return false;
    }
    const auto normalize = [&](std::string value) {
        if (!case_sensitive) {
            std::transform(
                value.begin(),
                value.end(),
                value.begin(),
                [](const unsigned char character) {
                    return static_cast<char>(std::tolower(character));
                }
            );
        }
        return value;
    };
    const auto sought = normalize(std::string(needle));
    const auto first = include_current ? 0U : 1U;
    for (std::size_t offset = first;
         offset < entries_.size() + first;
         ++offset) {
        const auto index = (cursor_ + offset) % entries_.size();
        const auto name = normalize(entries_[index].name);
        if (
            (prefix && name.starts_with(sought)) ||
            (!prefix && name.find(sought) != std::string::npos)
        ) {
            cursor_ = index;
            return true;
        }
    }
    return false;
}

const ProviderPtr& Pane::provider() const noexcept {
    return provider_;
}

const std::string& Pane::path() const noexcept {
    return path_;
}

const std::vector<Entry>& Pane::entries() const noexcept {
    return entries_;
}

std::size_t Pane::cursor() const noexcept {
    return cursor_;
}

const Entry* Pane::current() const noexcept {
    return entries_.empty() ? nullptr : &entries_[cursor_];
}

std::vector<Entry> Pane::selected_entries() const {
    if (selected_.empty()) {
        return current() == nullptr || is_parent_entry(*current())
                   ? std::vector<Entry>{}
                   : std::vector<Entry>{*current()};
    }
    std::vector<Entry> result;
    for (const auto& entry : entries_) {
        if (selected_.contains(entry.path)) {
            result.push_back(entry);
        }
    }
    return result;
}

bool Pane::is_selected(const Entry& entry) const {
    return selected_.contains(entry.path);
}

std::size_t Pane::marked_count() const noexcept {
    return selected_.size();
}

std::uint64_t Pane::marked_size() const noexcept {
    std::uint64_t size = 0;
    for (const auto& entry : entries_) {
        if (selected_.contains(entry.path)) {
            size += entry.size;
        }
    }
    return size;
}

void Pane::set_show_hidden(const bool value) {
    show_hidden_ = value;
    refresh();
}

bool Pane::show_hidden() const noexcept {
    return show_hidden_;
}

void Pane::set_directories_first(const bool value) {
    directories_first_ = value;
    refresh();
}

bool Pane::directories_first() const noexcept {
    return directories_first_;
}

void Pane::set_filter(std::string pattern) {
    const auto previous = filter_;
    filter_ = std::move(pattern);
    try {
        refresh();
    } catch (...) {
        filter_ = previous;
        throw;
    }
}

const std::string& Pane::filter() const noexcept {
    return filter_;
}

void Pane::set_sort(
    const SortField field,
    const bool descending
) {
    sort_ = field;
    descending_ = descending;
    refresh();
}

SortField Pane::sort_field() const noexcept {
    return sort_;
}

bool Pane::descending() const noexcept {
    return descending_;
}

}  // namespace mcpp
