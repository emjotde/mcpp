#include "mcpp/editor.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
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

bool continuation(const unsigned char value) {
    return (value & 0xC0U) == 0x80U;
}

std::size_t previous_character(
    const std::string& line,
    std::size_t column
) {
    if (column == 0U) {
        return 0U;
    }
    --column;
    while (column > 0U &&
           continuation(
               static_cast<unsigned char>(line[column])
           )) {
        --column;
    }
    return column;
}

std::size_t next_character(
    const std::string& line,
    std::size_t column
) {
    if (column >= line.size()) {
        return line.size();
    }
    ++column;
    while (column < line.size() &&
           continuation(
               static_cast<unsigned char>(line[column])
           )) {
        ++column;
    }
    return column;
}

}  // namespace

void EditorModel::open(
    ProviderPtr provider,
    std::string path,
    const Cancellation& cancellation
) {
    provider_ = std::move(provider);
    path_ = std::move(path);
    const auto entry = provider_->stat_follow(path_, cancellation);
    if (entry.kind != EntryKind::file) {
        throw std::runtime_error("editor requires a regular file");
    }
    storage_path_ = entry.path;
    constexpr std::uint64_t maximum = 16U * 1024U * 1024U;
    if (entry.size > maximum) {
        throw std::runtime_error(
            "internal editor limit is 16 MiB"
        );
    }
    revision_ = entry.revision;
    std::ostringstream output;
    provider_->read(
        storage_path_,
        0,
        entry.size,
        output,
        cancellation
    );
    const auto loaded = std::move(output).str();
    if (loaded.size() != entry.size) {
        throw std::runtime_error(
            "file changed while it was being opened"
        );
    }
    set_content(loaded);
    row_ = 0;
    column_ = 0;
    dirty_ = false;
    clear_selection();
    undo_.clear();
    redo_.clear();
}

void EditorModel::close() {
    provider_.reset();
    path_.clear();
    storage_path_.clear();
    revision_.clear();
    lines_.clear();
    row_ = 0;
    column_ = 0;
    dirty_ = false;
    clear_selection();
    undo_.clear();
    redo_.clear();
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

std::size_t EditorModel::byte_size() const noexcept {
    std::size_t result = lines_.empty() ? 0U : lines_.size() - 1U;
    for (const auto& line : lines_) {
        result += line.size();
    }
    return result;
}

bool EditorModel::marking() const noexcept {
    return marking_;
}

std::optional<std::pair<EditorPosition, EditorPosition>>
EditorModel::selection() const noexcept {
    if (!selection_begin_ || !selection_end_ ||
        *selection_begin_ == *selection_end_) {
        return std::nullopt;
    }
    return std::minmax(*selection_begin_, *selection_end_);
}

std::string EditorModel::content() const {
    std::ostringstream serialized;
    for (std::size_t index = 0; index < lines_.size(); ++index) {
        if (index != 0U) {
            serialized << '\n';
        }
        serialized << lines_[index];
    }
    return std::move(serialized).str();
}

void EditorModel::set_content(const std::string_view content_value) {
    lines_.clear();
    std::size_t begin = 0;
    while (begin < content_value.size()) {
        const auto end = content_value.find('\n', begin);
        if (end == std::string_view::npos) {
            lines_.emplace_back(content_value.substr(begin));
            begin = content_value.size();
        } else {
            lines_.emplace_back(
                content_value.substr(begin, end - begin)
            );
            begin = end + 1U;
            if (begin == content_value.size()) {
                lines_.emplace_back();
            }
        }
    }
    if (lines_.empty()) {
        lines_.emplace_back();
    }
}

std::size_t EditorModel::offset(EditorPosition position_value) const {
    if (lines_.empty()) {
        return 0U;
    }
    position_value.row =
        std::min(position_value.row, lines_.size() - 1U);
    position_value.column = std::min(
        position_value.column,
        lines_[position_value.row].size()
    );
    std::size_t result = 0;
    for (std::size_t row = 0; row < position_value.row; ++row) {
        result += lines_[row].size() + 1U;
    }
    return result + position_value.column;
}

EditorPosition EditorModel::position(std::size_t offset_value) const {
    offset_value = std::min(offset_value, byte_size());
    for (std::size_t row = 0; row < lines_.size(); ++row) {
        if (offset_value <= lines_[row].size()) {
            return {
                .row = row,
                .column = offset_value,
            };
        }
        offset_value -= lines_[row].size();
        if (row + 1U < lines_.size()) {
            --offset_value;
        }
    }
    return {
        .row = lines_.size() - 1U,
        .column = lines_.back().size(),
    };
}

void EditorModel::update_mark() {
    if (marking_) {
        selection_end_ = EditorPosition{
            .row = row_,
            .column = column_,
        };
    }
}

void EditorModel::replace_range(
    const EditorPosition begin,
    const EditorPosition end,
    const std::string_view replacement
) {
    auto serialized = content();
    const auto begin_offset = offset(begin);
    const auto end_offset = offset(end);
    serialized.replace(
        begin_offset,
        end_offset - begin_offset,
        replacement
    );
    set_content(serialized);
    const auto cursor = position(begin_offset + replacement.size());
    row_ = cursor.row;
    column_ = cursor.column;
    clear_selection();
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
    column_ = std::min(column_, lines_[row_].size());
    while (column_ > 0U &&
           column_ < lines_[row_].size() &&
           continuation(
               static_cast<unsigned char>(lines_[row_][column_])
           )) {
        --column_;
    }
    if (columns < 0) {
        for (int count = 0; count > columns; --count) {
            column_ = previous_character(lines_[row_], column_);
        }
    } else {
        for (int count = 0; count < columns; ++count) {
            column_ = next_character(lines_[row_], column_);
        }
    }
    update_mark();
}

void EditorModel::page(const int rows) {
    move(rows, 0);
}

void EditorModel::home() {
    column_ = 0;
    update_mark();
}

void EditorModel::end() {
    column_ = lines_[row_].size();
    update_mark();
}

void EditorModel::goto_line(const std::size_t line) {
    if (lines_.empty()) {
        return;
    }
    row_ = std::min(
        line == 0U ? 0U : line - 1U,
        lines_.size() - 1U
    );
    column_ = std::min(column_, lines_[row_].size());
    while (column_ > 0U &&
           column_ < lines_[row_].size() &&
           continuation(
               static_cast<unsigned char>(lines_[row_][column_])
           )) {
        --column_;
    }
    update_mark();
}

void EditorModel::toggle_mark() {
    const EditorPosition cursor{
        .row = row_,
        .column = column_,
    };
    if (marking_) {
        selection_end_ = cursor;
        marking_ = false;
        return;
    }
    clear_selection();
    selection_begin_ = cursor;
    selection_end_ = cursor;
    marking_ = true;
}

void EditorModel::clear_selection() {
    selection_begin_.reset();
    selection_end_.reset();
    marking_ = false;
}

void EditorModel::insert(const char character) {
    insert(std::string_view(&character, 1U));
}

void EditorModel::insert(const std::string_view text) {
    if (text.empty()) {
        return;
    }
    record_undo();
    if (const auto selected = selection()) {
        replace_range(selected->first, selected->second, text);
        dirty_ = true;
        return;
    }
    std::size_t begin = 0;
    while (begin < text.size()) {
        const auto end = text.find('\n', begin);
        const auto segment_end =
            end == std::string_view::npos ? text.size() : end;
        lines_[row_].insert(
            column_,
            text.substr(begin, segment_end - begin)
        );
        column_ += segment_end - begin;
        if (end == std::string_view::npos) {
            break;
        }
        auto tail = lines_[row_].substr(column_);
        lines_[row_].erase(column_);
        lines_.insert(
            lines_.begin() +
                static_cast<std::ptrdiff_t>(row_ + 1U),
            std::move(tail)
        );
        ++row_;
        column_ = 0;
        begin = end + 1U;
    }
    clear_selection();
    dirty_ = true;
}

void EditorModel::newline() {
    insert("\n");
}

void EditorModel::backspace() {
    if (selection()) {
        static_cast<void>(delete_selection());
        return;
    }
    if (column_ == 0U && row_ == 0U) {
        return;
    }
    clear_selection();
    record_undo();
    if (column_ > 0U) {
        const auto previous =
            previous_character(lines_[row_], column_);
        lines_[row_].erase(previous, column_ - previous);
        column_ = previous;
        dirty_ = true;
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
    if (selection()) {
        static_cast<void>(delete_selection());
        return;
    }
    if (column_ >= lines_[row_].size() &&
        row_ + 1U >= lines_.size()) {
        return;
    }
    clear_selection();
    record_undo();
    if (column_ < lines_[row_].size()) {
        const auto next = next_character(lines_[row_], column_);
        lines_[row_].erase(column_, next - column_);
        dirty_ = true;
        return;
    }
    lines_[row_] += lines_[row_ + 1U];
    lines_.erase(
        lines_.begin() + static_cast<std::ptrdiff_t>(row_ + 1U)
    );
    dirty_ = true;
}

bool EditorModel::copy_selection() {
    const auto selected = selection();
    if (!selected) {
        return false;
    }
    const auto begin_offset = offset(selected->first);
    const auto end_offset = offset(selected->second);
    clipboard_ = content().substr(
        begin_offset,
        end_offset - begin_offset
    );
    record_undo();
    const EditorPosition destination{
        .row = row_,
        .column = column_,
    };
    clear_selection();
    replace_range(destination, destination, clipboard_);
    dirty_ = true;
    return true;
}

bool EditorModel::move_selection() {
    const auto selected = selection();
    if (!selected) {
        return false;
    }
    auto serialized = content();
    auto begin_offset = offset(selected->first);
    auto end_offset = offset(selected->second);
    auto destination = offset({
        .row = row_,
        .column = column_,
    });
    if (destination >= begin_offset && destination <= end_offset) {
        return false;
    }
    clipboard_ = serialized.substr(
        begin_offset,
        end_offset - begin_offset
    );
    record_undo();
    serialized.erase(begin_offset, end_offset - begin_offset);
    if (destination > end_offset) {
        destination -= end_offset - begin_offset;
    }
    serialized.insert(destination, clipboard_);
    set_content(serialized);
    const auto cursor = position(destination + clipboard_.size());
    row_ = cursor.row;
    column_ = cursor.column;
    clear_selection();
    dirty_ = true;
    return true;
}

bool EditorModel::delete_selection() {
    const auto selected = selection();
    if (!selected) {
        return false;
    }
    record_undo();
    replace_range(selected->first, selected->second, "");
    dirty_ = true;
    return true;
}

void EditorModel::record_undo() {
    undo_.push_back({
        .lines = lines_,
        .row = row_,
        .column = column_,
        .selection_begin = selection_begin_,
        .selection_end = selection_end_,
        .marking = marking_,
    });
    constexpr std::size_t maximum = 100U;
    if (undo_.size() > maximum) {
        undo_.erase(undo_.begin());
    }
    redo_.clear();
}

void EditorModel::restore(Snapshot snapshot) {
    lines_ = std::move(snapshot.lines);
    row_ = snapshot.row;
    column_ = snapshot.column;
    selection_begin_ = snapshot.selection_begin;
    selection_end_ = snapshot.selection_end;
    marking_ = snapshot.marking;
    dirty_ = true;
}

bool EditorModel::undo() {
    if (undo_.empty()) {
        return false;
    }
    redo_.push_back({
        .lines = lines_,
        .row = row_,
        .column = column_,
        .selection_begin = selection_begin_,
        .selection_end = selection_end_,
        .marking = marking_,
    });
    auto snapshot = std::move(undo_.back());
    undo_.pop_back();
    restore(std::move(snapshot));
    return true;
}

bool EditorModel::redo() {
    if (redo_.empty()) {
        return false;
    }
    undo_.push_back({
        .lines = lines_,
        .row = row_,
        .column = column_,
        .selection_begin = selection_begin_,
        .selection_end = selection_end_,
        .marking = marking_,
    });
    auto snapshot = std::move(redo_.back());
    redo_.pop_back();
    restore(std::move(snapshot));
    return true;
}

void EditorModel::save(
    const bool force,
    const Cancellation& cancellation
) {
    if (!provider_) {
        throw std::runtime_error("editor is not open");
    }
    auto serialized = content();
    std::istringstream input(serialized);
    provider_->write(
        storage_path_,
        input,
        serialized.size(),
        true,
        force ? std::nullopt
              : std::optional<std::string>(revision_),
        cancellation,
        {}
    );
    revision_ =
        provider_->stat(storage_path_, cancellation).revision;
    dirty_ = false;
}

void EditorModel::save_as(
    std::string path,
    const bool force,
    const Cancellation& cancellation
) {
    if (!provider_) {
        throw std::runtime_error("editor is not open");
    }
    if (std::filesystem::path(path).is_relative()) {
        path = provider_->join(provider_->parent(path_), path);
    }
    path = provider_->normalize(path);
    if (path == provider_->normalize(path_)) {
        save(force, cancellation);
        return;
    }
    if (!force && provider_->exists(path)) {
        throw std::runtime_error("destination exists: " + path);
    }
    auto serialized = content();
    std::istringstream input(serialized);
    provider_->write(
        path,
        input,
        serialized.size(),
        force,
        std::nullopt,
        cancellation,
        {}
    );
    path_ = std::move(path);
    storage_path_ = path_;
    revision_ = provider_->stat(storage_path_, cancellation).revision;
    dirty_ = false;
}

bool EditorModel::find(
    const std::string_view needle,
    const bool case_sensitive
) {
    if (needle.empty()) {
        return false;
    }
    const auto serialized = content();
    const auto haystack =
        case_sensitive ? serialized : lowercase(serialized);
    const auto sought =
        case_sensitive ? std::string(needle)
                       : lowercase(std::string(needle));
    auto begin = offset({
        .row = row_,
        .column = column_,
    });
    if (const auto selected = selection();
        selected && EditorPosition{row_, column_} == selected->first) {
        begin = offset(selected->second);
    }
    auto found = haystack.find(sought, begin);
    if (found == std::string::npos && begin != 0U) {
        found = haystack.find(sought);
    }
    if (found == std::string::npos) {
        return false;
    }
    selection_begin_ = position(found);
    selection_end_ = position(found + needle.size());
    marking_ = false;
    row_ = selection_begin_->row;
    column_ = selection_begin_->column;
    return true;
}

bool EditorModel::replace(
    const std::string_view needle,
    const std::string_view replacement,
    const bool case_sensitive
) {
    if (!find(needle, case_sensitive)) {
        return false;
    }
    const auto selected = selection();
    if (!selected) {
        return false;
    }
    record_undo();
    replace_range(selected->first, selected->second, replacement);
    dirty_ = true;
    return true;
}

}  // namespace mcpp
