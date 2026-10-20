#include "mcpp/pane.hpp"
#include "mcpp/editor_search.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace mcpp {
namespace {

bool is_parent_entry(const Entry& entry) {
    return entry.revision == "mcpp:parent";
}

std::size_t next_character(const std::string_view value, const std::size_t offset) {
    const auto first = static_cast<unsigned char>(value[offset]);
    const auto count = first >= 0xc2U && first <= 0xdfU ? 2U
                     : first >= 0xe0U && first <= 0xefU ? 3U
                     : first >= 0xf0U && first <= 0xf4U ? 4U : 1U;
    if (offset + count > value.size()) {
        return offset + 1U;
    }
    for (std::size_t index = 1; index < count; ++index) {
        if ((static_cast<unsigned char>(value[offset + index]) & 0xc0U) != 0x80U) {
            return offset + 1U;
        }
    }
    if (count > 1U) {
        const auto second = static_cast<unsigned char>(value[offset + 1U]);
        if ((first == 0xe0U && second < 0xa0U) || (first == 0xedU && second >= 0xa0U) ||
            (first == 0xf0U && second < 0x90U) || (first == 0xf4U && second >= 0x90U)) {
            return offset + 1U;
        }
    }
    return offset + count;
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
            value_index = pattern[pattern_index] == '?'
                              ? next_character(value, value_index) : value_index + 1U;
            ++pattern_index;
        } else if (pattern_index < pattern.size() &&
                   pattern[pattern_index] == '*') {
            star = pattern_index++;
            retry = value_index;
        } else if (star != std::string_view::npos) {
            pattern_index = star + 1U;
            retry = next_character(value, retry);
            value_index = retry;
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

Pane::Pane(
    ProviderPtr provider, std::string path,
    const Pane* preferences, const bool external_parent
)
    : provider_(std::move(provider)), external_parent_(external_parent) {
    if (!provider_) {
        throw std::invalid_argument("pane provider is null");
    }
    if (preferences != nullptr) {
        viewport_rows_ = preferences->viewport_rows_;
        show_hidden_ = preferences->show_hidden_;
        show_backups_ = preferences->show_backups_;
        fast_reload_ = preferences->fast_reload_;
        page_scrolling_ = preferences->page_scrolling_;
        center_scrolling_ = preferences->center_scrolling_;
        torben_fj_mode_ = preferences->torben_fj_mode_;
        shell_patterns_ = preferences->shell_patterns_;
        follow_links_ = preferences->follow_links_;
        directories_first_ = preferences->directories_first_;
        filter_ = preferences->filter_;
        sort_ = preferences->sort_;
        descending_ = preferences->descending_;
        sort_case_sensitive_ = preferences->sort_case_sensitive_;
        executable_first_ = preferences->executable_first_;
        reverse_files_only_ = preferences->reverse_files_only_;
    }
    path_ = provider_->normalize(path);
    history_.push_back(path_);
    refresh();
}

void Pane::refresh(const Cancellation& cancellation, const bool force) {
    std::optional<std::string> revision;
    if (!panelized_ && fast_reload_) {
        revision = provider_->stat(path_, cancellation).revision;
        if (!force && directory_revision_ == revision) return;
    }
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
            current_entry.name = panelized_root_.empty() || panelized_root_ == path_ ? entry.name : entry.path;
            if (const auto size = directory_sizes_.find(current_entry.path);
                size != directory_sizes_.end() &&
                (current_entry.kind == EntryKind::directory || current_entry.target_is_directory)) {
                current_entry.size = size->second;
            }
            if (!filter_.empty() &&
                current_entry.kind != EntryKind::directory &&
                !current_entry.target_is_directory &&
                !matches_pattern(filter_, current_entry.name)) {
                continue;
            }
            refreshed.push_back(std::move(current_entry));
        }
        entries_ = std::move(refreshed);
        sort_entries(entries_, list_request());
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
        keep_cursor_visible();
        return;
    }
    entries_ = provider_->list(
        path_,
        list_request(),
        cancellation
    );
    if (!show_backups_) {
        std::erase_if(entries_, [](const Entry& entry) { return entry.name.ends_with('~'); });
    }
    for (auto& entry : entries_) {
        if (const auto size = directory_sizes_.find(entry.path);
            size != directory_sizes_.end() && (entry.kind == EntryKind::directory || entry.target_is_directory)) {
            entry.size = size->second;
        }
    }
    if (sort_ == SortField::size && !directory_sizes_.empty()) {
        sort_entries(entries_, list_request());
    }
    if (!filter_.empty()) {
        std::erase_if(
            entries_,
            [&](const Entry& entry) {
                return entry.kind != EntryKind::directory &&
                       !entry.target_is_directory &&
                       !matches_pattern(filter_, entry.name);
            }
        );
    }
    const auto parent_path = provider_->parent(path_);
    if (parent_path != path_ || external_parent_) {
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
    keep_cursor_visible();
    directory_revision_ = std::move(revision);
}

ListRequest Pane::list_request() const {
    return {.show_hidden=show_hidden_, .directories_first=directories_first_, .sort=sort_,
            .descending=descending_, .case_sensitive=sort_case_sensitive_,
            .executables_first=executable_first_, .reverse_files_only=reverse_files_only_};
}

void Pane::set_sort_options(
    const SortField field, const bool descending, const bool case_sensitive, const bool executable_first
) {
    sort_ = field;
    descending_ = descending;
    sort_case_sensitive_ = case_sensitive;
    executable_first_ = executable_first;
    refresh({}, true);
}

void Pane::set_reverse_files_only(const bool value) {
    reverse_files_only_ = value;
    refresh({}, true);
}

void Pane::move_cursor(const int amount) {
    if (entries_.empty()) {
        cursor_ = 0;
        first_row_ = 0;
        return;
    }
    const auto current_index = static_cast<long long>(cursor_);
    const auto last = static_cast<long long>(entries_.size() - 1U);
    cursor_ = static_cast<std::size_t>(
        std::clamp(current_index + amount, 0LL, last)
    );
    if ((amount == 1 || amount == -1) && cursor_ != static_cast<std::size_t>(current_index)) {
        const auto maximum = entries_.size() > viewport_rows_ ? entries_.size() - viewport_rows_ : 0U;
        if (page_scrolling_ && cursor_ >= first_row_ + viewport_rows_) {
            first_row_ = std::min(maximum, first_row_ + std::max<std::size_t>(1U, viewport_rows_ / 2U));
        } else if (page_scrolling_ && cursor_ < first_row_) {
            const auto half = std::max<std::size_t>(1U, viewport_rows_ / 2U);
            first_row_ = first_row_ > half ? first_row_ - half : 0U;
        } else if (center_scrolling_ && amount > 0 && cursor_ > first_row_ + viewport_rows_ / 2U) {
            first_row_ = std::min(maximum, first_row_ + 1U);
        } else if (center_scrolling_ && amount < 0 && cursor_ < first_row_ + viewport_rows_ / 2U && first_row_ > 0U) {
            --first_row_;
        }
    }
    keep_cursor_visible();
}

void Pane::page_cursor(const int amount) {
    const auto maximum = entries_.size() > viewport_rows_
                             ? entries_.size() - viewport_rows_
                             : 0U;
    first_row_ = static_cast<std::size_t>(std::clamp(
        static_cast<long long>(first_row_) + amount,
        0LL,
        static_cast<long long>(maximum)
    ));
    move_cursor(amount);
}

void Pane::home_cursor() {
    if (!torben_fj_mode_ || entries_.empty()) {
        cursor_ = 0;
    } else {
        const auto offset = cursor_ - first_row_;
        if (offset > viewport_rows_ / 2U) {
            cursor_ = first_row_ + viewport_rows_ / 2U;
        } else if (offset > 0U) {
            cursor_ = first_row_;
        } else {
            cursor_ = 0;
        }
    }
    keep_cursor_visible();
}

void Pane::end_cursor() {
    if (entries_.empty()) {
        cursor_ = 0;
        first_row_ = 0;
        return;
    }
    const auto last = entries_.size() - 1U;
    if (!torben_fj_mode_) {
        cursor_ = last;
    } else {
        const auto offset = cursor_ - first_row_;
        const auto middle = viewport_rows_ / 2U;
        const auto bottom = viewport_rows_ - 1U;
        if (offset < middle) {
            cursor_ = first_row_ + std::min(middle, last - first_row_);
        } else if (offset < bottom) {
            cursor_ = first_row_ + std::min(bottom, last - first_row_);
        } else {
            cursor_ = last;
        }
    }
    keep_cursor_visible();
}

void Pane::set_directory_sizes(const std::map<std::string, std::uint64_t>& sizes) {
    for (const auto& [path, size] : sizes) directory_sizes_[path] = size;
    refresh({}, true);
}

void Pane::clear_directory_sizes() { directory_sizes_.clear(); }

void Pane::set_viewport_rows(const std::size_t rows) {
    viewport_rows_ = std::max<std::size_t>(1U, rows);
    keep_cursor_visible();
}

std::size_t Pane::first_row() const noexcept {
    return first_row_;
}

void Pane::keep_cursor_visible() {
    const auto maximum = entries_.size() > viewport_rows_
                             ? entries_.size() - viewport_rows_
                             : 0U;
    first_row_ = std::min(first_row_, maximum);
    if (cursor_ < first_row_) {
        first_row_ = cursor_;
    } else if (cursor_ - first_row_ >= viewport_rows_) {
        first_row_ = cursor_ - viewport_rows_ + 1U;
    }
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
    const auto child_path = navigation_path();
    const auto target = provider_->parent(child_path);
    if (target == child_path) {
        return false;
    }
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
        keep_cursor_visible();
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
    auto normalized = provider_->normalize(std::filesystem::path(path).is_relative()
        ? provider_->join(navigation_path(), path) : path);
    const auto entry = provider_->stat(normalized, cancellation);
    if (entry.kind != EntryKind::directory &&
        !entry.target_is_directory) {
        throw std::runtime_error(
            "not a directory: " + normalized
        );
    }
    if (!follow_links_) normalized = provider_->stat_follow(normalized, cancellation).path;
    const auto previous_path = path_;
    const auto previous_entries = entries_;
    const auto previous_selected = selected_;
    const auto previous_cursor = cursor_;
    const auto previous_first_row = first_row_;
    const auto previous_panelized = panelized_;
    const auto previous_panelized_entries = panelized_entries_;
    const auto previous_revision = directory_revision_;
    path_ = std::move(normalized);
    panelized_ = false;
    directory_revision_.reset();
    selected_.clear();
    cursor_ = 0;
    first_row_ = 0;
    try {
        refresh(cancellation);
    } catch (...) {
        path_ = previous_path;
        entries_ = previous_entries;
        selected_ = previous_selected;
        cursor_ = previous_cursor;
        first_row_ = previous_first_row;
        panelized_ = previous_panelized;
        panelized_entries_ = previous_panelized_entries;
        directory_revision_ = previous_revision;
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
    panelized_root_ = path_;
    entries_ = panelized_entries_;
    if (!filter_.empty()) {
        std::erase_if(
            entries_,
            [&](const Entry& entry) {
                return entry.kind != EntryKind::directory &&
                       !entry.target_is_directory &&
                       !matches_pattern(filter_, entry.name);
            }
        );
    }
    selected_.clear();
    cursor_ = 0;
    first_row_ = 0;
    panelized_ = true;
}

void Pane::restore_panelized() {
    panelized_ = true;
    selected_.clear();
    cursor_ = 0;
    first_row_ = 0;
    refresh();
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
        if (!matches_pattern(pattern, entry.name)) {
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
    const auto first = include_current ? 0U : 1U;
    for (std::size_t offset = first;
         offset < entries_.size() + first;
         ++offset) {
        const auto index = (cursor_ + offset) % entries_.size();
        const auto& name = entries_[index].name;
        const auto match = EditorSearch::find(
            name, needle, 0U, 0U, name.size(),
            {.case_sensitive = case_sensitive}, false, false
        );
        if (match && (!prefix || match->begin == 0U)) {
            cursor_ = index;
            keep_cursor_visible();
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
    refresh({}, true);
}

void Pane::set_show_backups(const bool value) {
    show_backups_ = value;
    refresh({}, true);
}

void Pane::set_fast_reload(const bool value) {
    fast_reload_ = value;
    directory_revision_.reset();
    refresh({}, true);
}

void Pane::set_navigation(const bool page_scrolling, const bool center_scrolling) {
    page_scrolling_ = page_scrolling;
    center_scrolling_ = center_scrolling;
}

void Pane::set_torben_fj_mode(const bool value) noexcept {
    torben_fj_mode_ = value;
}

std::string Pane::navigation_path() const {
    return follow_links_ ? path_ : provider_->stat_follow(path_, {}).path;
}

bool Pane::matches_pattern(const std::string_view pattern, const std::string_view name) const {
    if (shell_patterns_) return wildcard_match(pattern, name);
    return EditorSearch::find(name, pattern, 0, 0, name.size(),
        {.mode=EditorSearchMode::regular_expression, .case_sensitive=true}, false, false).has_value();
}

void Pane::set_shell_patterns(const bool value) {
    const auto previous = shell_patterns_;
    shell_patterns_ = value;
    try { refresh({}, true); }
    catch (...) { shell_patterns_ = previous; throw; }
}

void Pane::swap_contents(Pane& other) {
    if (provider_ != other.provider_) {
        std::swap(history_, other.history_);
        std::swap(history_index_, other.history_index_);
    }
    std::swap(provider_, other.provider_);
    std::swap(external_parent_, other.external_parent_);
    std::swap(path_, other.path_);
    std::swap(entries_, other.entries_);
    std::swap(selected_, other.selected_);
    std::swap(cursor_, other.cursor_);
    std::swap(first_row_, other.first_row_);
    std::swap(panelized_, other.panelized_);
    std::swap(panelized_entries_, other.panelized_entries_);
    std::swap(panelized_root_, other.panelized_root_);
    std::swap(directory_sizes_, other.directory_sizes_);
    std::swap(directory_revision_, other.directory_revision_);
    for (auto* pane : {this, &other}) {
        const auto selected = pane->current() == nullptr ? std::string{} : pane->current()->path;
        sort_entries(pane->entries_, pane->list_request());
        const auto found = std::ranges::find(pane->entries_, selected, &Entry::path);
        pane->cursor_ = found == pane->entries_.end() ? 0U
                      : static_cast<std::size_t>(found - pane->entries_.begin());
        pane->keep_cursor_visible();
    }
}

bool Pane::show_hidden() const noexcept {
    return show_hidden_;
}

void Pane::set_directories_first(const bool value) {
    directories_first_ = value;
    refresh({}, true);
}

bool Pane::directories_first() const noexcept {
    return directories_first_;
}

void Pane::set_filter(std::string pattern) {
    const auto previous = filter_;
    filter_ = std::move(pattern);
    try {
        refresh({}, true);
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
    refresh({}, true);
}

SortField Pane::sort_field() const noexcept {
    return sort_;
}

bool Pane::descending() const noexcept {
    return descending_;
}

}  // namespace mcpp
