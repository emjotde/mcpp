#include "mcpp/pane.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace mcpp {

Pane::Pane(ProviderPtr provider, std::string path)
    : provider_(std::move(provider)) {
    if (!provider_) {
        throw std::invalid_argument("pane provider is null");
    }
    path_ = provider_->normalize(path);
    refresh();
}

void Pane::refresh(const Cancellation& cancellation) {
    const auto previous = current() != nullptr
                              ? current()->path
                              : std::string{};
    entries_ = provider_->list(
        path_,
        {
            .show_hidden = show_hidden_,
            .sort = sort_,
            .descending = descending_,
        },
        cancellation
    );
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
    if (entry == nullptr || entry->kind != EntryKind::directory) {
        return false;
    }
    path_ = provider_->normalize(entry->path);
    cursor_ = 0;
    selected_.clear();
    refresh(cancellation);
    return true;
}

bool Pane::up(const Cancellation& cancellation) {
    const auto target = provider_->parent(path_);
    if (target == path_) {
        return false;
    }
    const auto old_name = provider_->basename(path_);
    path_ = target;
    selected_.clear();
    refresh(cancellation);
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
    return true;
}

void Pane::change_directory(
    std::string path,
    const Cancellation& cancellation
) {
    const auto normalized = provider_->normalize(path);
    const auto entry = provider_->stat(normalized, cancellation);
    if (entry.kind != EntryKind::directory) {
        throw std::runtime_error(
            "not a directory: " + normalized
        );
    }
    path_ = std::move(normalized);
    selected_.clear();
    cursor_ = 0;
    refresh(cancellation);
}

void Pane::toggle_selected() {
    const auto* entry = current();
    if (entry == nullptr) {
        return;
    }
    if (selected_.contains(entry->path)) {
        selected_.erase(entry->path);
    } else {
        selected_.insert(entry->path);
    }
    move_cursor(1);
}

void Pane::select_all() {
    for (const auto& entry : entries_) {
        selected_.insert(entry.path);
    }
}

void Pane::clear_selection() {
    selected_.clear();
}

bool Pane::find(
    const std::string_view needle,
    const bool case_sensitive
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
    for (std::size_t offset = 1U; offset <= entries_.size(); ++offset) {
        const auto index = (cursor_ + offset) % entries_.size();
        if (normalize(entries_[index].name).find(sought) !=
            std::string::npos) {
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
        return current() == nullptr
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

void Pane::set_show_hidden(const bool value) {
    show_hidden_ = value;
    refresh();
}

bool Pane::show_hidden() const noexcept {
    return show_hidden_;
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
