#include "mcpp/editor.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <stdexcept>

namespace mcpp {
namespace {

std::string lowercase(std::string value) {
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

}  // namespace

void EditorModel::open(
    ProviderPtr provider,
    std::string path,
    const Cancellation& cancellation
) {
    provider_ = std::move(provider);
    path_ = std::move(path);
    const auto entry = provider_->stat(path_, cancellation);
    constexpr std::uint64_t maximum = 16U * 1024U * 1024U;
    if (entry.size > maximum) {
        throw std::runtime_error(
            "internal editor limit is 16 MiB"
        );
    }
    revision_ = entry.revision;
    std::ostringstream output;
    provider_->read(
        path_,
        0,
        entry.size,
        output,
        cancellation
    );
    std::istringstream input(std::move(output).str());
    lines_.clear();
    std::string line;
    while (std::getline(input, line)) {
        lines_.push_back(std::move(line));
    }
    if (lines_.empty()) {
        lines_.emplace_back();
    }
    row_ = 0;
    column_ = 0;
    dirty_ = false;
}

void EditorModel::close() {
    provider_.reset();
    path_.clear();
    revision_.clear();
    lines_.clear();
    row_ = 0;
    column_ = 0;
    dirty_ = false;
}

bool EditorModel::open() const noexcept {
    return provider_ != nullptr;
}

bool EditorModel::dirty() const noexcept {
    return dirty_;
}

const std::string& EditorModel::path() const noexcept {
    return path_;
}

const std::vector<std::string>& EditorModel::lines() const noexcept {
    return lines_;
}

std::size_t EditorModel::row() const noexcept {
    return row_;
}

std::size_t EditorModel::column() const noexcept {
    return column_;
}

void EditorModel::move(const int rows, const int columns) {
    if (lines_.empty()) {
        return;
    }
    row_ = static_cast<std::size_t>(std::clamp<long long>(
        static_cast<long long>(row_) + rows,
        0,
        static_cast<long long>(lines_.size() - 1U)
    ));
    column_ = static_cast<std::size_t>(std::clamp<long long>(
        static_cast<long long>(column_) + columns,
        0,
        static_cast<long long>(lines_[row_].size())
    ));
}

void EditorModel::home() {
    column_ = 0;
}

void EditorModel::end() {
    column_ = lines_[row_].size();
}

void EditorModel::insert(const char character) {
    lines_[row_].insert(
        lines_[row_].begin() + static_cast<std::ptrdiff_t>(column_),
        character
    );
    ++column_;
    dirty_ = true;
}

void EditorModel::insert(const std::string_view text) {
    for (const auto character : text) {
        if (character == '\n') {
            newline();
        } else {
            insert(character);
        }
    }
}

void EditorModel::newline() {
    auto tail = lines_[row_].substr(column_);
    lines_[row_].erase(column_);
    lines_.insert(
        lines_.begin() + static_cast<std::ptrdiff_t>(row_ + 1U),
        std::move(tail)
    );
    ++row_;
    column_ = 0;
    dirty_ = true;
}

void EditorModel::backspace() {
    if (column_ > 0U) {
        lines_[row_].erase(column_ - 1U, 1U);
        --column_;
        dirty_ = true;
        return;
    }
    if (row_ == 0U) {
        return;
    }
    const auto previous_size = lines_[row_ - 1U].size();
    lines_[row_ - 1U] += lines_[row_];
    lines_.erase(
        lines_.begin() + static_cast<std::ptrdiff_t>(row_)
    );
    --row_;
    column_ = previous_size;
    dirty_ = true;
}

void EditorModel::erase() {
    if (column_ < lines_[row_].size()) {
        lines_[row_].erase(column_, 1U);
        dirty_ = true;
        return;
    }
    if (row_ + 1U >= lines_.size()) {
        return;
    }
    lines_[row_] += lines_[row_ + 1U];
    lines_.erase(
        lines_.begin() + static_cast<std::ptrdiff_t>(row_ + 1U)
    );
    dirty_ = true;
}

void EditorModel::save(
    const bool force,
    const Cancellation& cancellation
) {
    if (!provider_) {
        throw std::runtime_error("editor is not open");
    }
    std::ostringstream serialized;
    for (std::size_t index = 0; index < lines_.size(); ++index) {
        if (index != 0U) {
            serialized << '\n';
        }
        serialized << lines_[index];
    }
    auto content = std::move(serialized).str();
    std::istringstream input(content);
    provider_->write(
        path_,
        input,
        content.size(),
        true,
        force ? std::nullopt
              : std::optional<std::string>(revision_),
        cancellation,
        {}
    );
    revision_ = provider_->stat(path_, cancellation).revision;
    dirty_ = false;
}

bool EditorModel::find(
    const std::string_view needle,
    const bool case_sensitive
) {
    if (needle.empty()) {
        return false;
    }
    const auto sought =
        case_sensitive ? std::string(needle)
                       : lowercase(std::string(needle));
    for (std::size_t line_index = row_;
         line_index < lines_.size();
         ++line_index) {
        auto line =
            case_sensitive ? lines_[line_index]
                           : lowercase(lines_[line_index]);
        const auto begin = line_index == row_ ? column_ : 0U;
        const auto found = line.find(sought, begin);
        if (found != std::string::npos) {
            row_ = line_index;
            column_ = found;
            return true;
        }
    }
    return false;
}

}  // namespace mcpp
