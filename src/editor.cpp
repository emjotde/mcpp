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
    const std::string_view line,
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
    const std::string_view line,
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

bool word_character(
    const std::string_view line,
    const std::size_t column
) {
    if (column >= line.size()) {
        return false;
    }
    const auto byte = static_cast<unsigned char>(line[column]);
    return byte >= 0x80U || std::isalnum(byte) != 0 || byte == '_';
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
    overwrite_ = false;
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
    overwrite_ = false;
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

bool EditorModel::overwrite() const noexcept {
    return overwrite_;
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
            if (column_ == 0U && row_ > 0U) {
                --row_;
                column_ = lines_[row_].size();
            } else {
                column_ =
                    previous_character(lines_[row_], column_);
            }
        }
    } else {
        for (int count = 0; count < columns; ++count) {
            if (column_ == lines_[row_].size() &&
                row_ + 1U < lines_.size()) {
                ++row_;
                column_ = 0;
            } else {
                column_ =
                    next_character(lines_[row_], column_);
            }
        }
    }
    update_mark();
}

void EditorModel::move_word(const int direction) {
    if (lines_.empty() || direction == 0) {
        return;
    }
    if (direction < 0) {
        if (column_ == 0U && row_ > 0U) {
            --row_;
            column_ = lines_[row_].size();
        }
        while (column_ > 0U) {
            const auto previous =
                previous_character(lines_[row_], column_);
            if (word_character(lines_[row_], previous)) {
                break;
            }
            column_ = previous;
        }
        while (column_ > 0U) {
            const auto previous =
                previous_character(lines_[row_], column_);
            if (!word_character(lines_[row_], previous)) {
                break;
            }
            column_ = previous;
        }
    } else {
        while (column_ < lines_[row_].size() &&
               word_character(lines_[row_], column_)) {
            column_ = next_character(lines_[row_], column_);
        }
        while (column_ < lines_[row_].size() &&
               !word_character(lines_[row_], column_)) {
            column_ = next_character(lines_[row_], column_);
        }
        if (column_ == lines_[row_].size() &&
            row_ + 1U < lines_.size()) {
            ++row_;
            column_ = 0;
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

void EditorModel::begin_mark() {
    if (marking_) {
        return;
    }
    clear_selection();
    selection_begin_ = EditorPosition{
        .row = row_,
        .column = column_,
    };
    selection_end_ = selection_begin_;
    marking_ = true;
}

void EditorModel::finish_mark() {
    if (marking_) {
        selection_end_ = EditorPosition{
            .row = row_,
            .column = column_,
        };
        marking_ = false;
    }
}

void EditorModel::clear_selection() {
    selection_begin_.reset();
    selection_end_.reset();
    marking_ = false;
}

void EditorModel::toggle_overwrite() noexcept {
    overwrite_ = !overwrite_;
}

void EditorModel::insert(const char character) {
    insert(std::string_view(&character, 1U));
}

void EditorModel::insert(const std::string_view text) {
    insert_text(text, overwrite_);
}

void EditorModel::insert_text(
    const std::string_view text,
    const bool overwrite,
    const bool include_selection_end
) {
    if (text.empty()) {
        return;
    }
    record_undo();
    const bool active_mark = marking_ && selection_begin_.has_value();
    const auto anchor_offset =
        active_mark
            ? std::optional<std::size_t>{
                  offset(*selection_begin_)
              }
            : std::nullopt;
    const auto selected = selection();
    const auto selection_offsets =
        selected
            ? std::optional<std::pair<std::size_t, std::size_t>>{
                  {
                      offset(selected->first),
                      offset(selected->second),
                  }
              }
            : std::nullopt;
    auto serialized = content();
    const auto begin_offset = offset({
        .row = row_,
        .column = column_,
    });
    auto end_offset = begin_offset;
    if (overwrite && text.find('\n') == std::string_view::npos) {
        auto input_column = std::size_t{0};
        auto source_column = column_;
        while (input_column < text.size() &&
               source_column < lines_[row_].size()) {
            input_column = next_character(text, input_column);
            source_column =
                next_character(lines_[row_], source_column);
        }
        end_offset = offset({
            .row = row_,
            .column = source_column,
        });
    }
    serialized.replace(
        begin_offset,
        end_offset - begin_offset,
        text
    );
    set_content(serialized);
    const auto cursor = position(begin_offset + text.size());
    row_ = cursor.row;
    column_ = cursor.column;
    if (anchor_offset) {
        auto adjusted_anchor = *anchor_offset;
        const auto removed = end_offset - begin_offset;
        if (removed == 0U) {
            if (begin_offset < adjusted_anchor) {
                adjusted_anchor += text.size();
            }
        } else if (end_offset <= adjusted_anchor) {
            const auto delta =
                static_cast<std::ptrdiff_t>(text.size()) -
                static_cast<std::ptrdiff_t>(removed);
            adjusted_anchor = static_cast<std::size_t>(
                static_cast<std::ptrdiff_t>(adjusted_anchor) +
                delta
            );
        } else if (begin_offset < adjusted_anchor) {
            adjusted_anchor = begin_offset + text.size();
        }
        selection_begin_ = position(adjusted_anchor);
        selection_end_ = cursor;
        marking_ = true;
    } else if (selection_offsets) {
        auto selection_begin = selection_offsets->first;
        auto selection_end = selection_offsets->second;
        const auto removed = end_offset - begin_offset;
        if (removed == 0U) {
            if (begin_offset < selection_begin) {
                selection_begin += text.size();
                selection_end += text.size();
            } else if (
                begin_offset < selection_end ||
                (
                    include_selection_end &&
                    begin_offset == selection_end
                )
            ) {
                selection_end += text.size();
            }
        } else if (end_offset <= selection_begin) {
            const auto delta =
                static_cast<std::ptrdiff_t>(text.size()) -
                static_cast<std::ptrdiff_t>(removed);
            selection_begin = static_cast<std::size_t>(
                static_cast<std::ptrdiff_t>(selection_begin) + delta
            );
            selection_end = static_cast<std::size_t>(
                static_cast<std::ptrdiff_t>(selection_end) + delta
            );
        } else if (begin_offset < selection_end) {
            selection_begin =
                std::min(selection_begin, begin_offset);
            selection_end = std::max(
                begin_offset + text.size(),
                selection_end - std::min(removed, selection_end -
                                                      begin_offset) +
                    text.size()
            );
        }
        selection_begin_ = position(selection_begin);
        selection_end_ = position(selection_end);
        marking_ = false;
    } else {
        clear_selection();
    }
    dirty_ = true;
}

void EditorModel::newline() {
    insert("\n");
}

void EditorModel::erase_text(
    const EditorPosition begin,
    const EditorPosition end
) {
    const bool active_mark = marking_ && selection_begin_.has_value();
    const auto anchor_offset =
        active_mark
            ? std::optional<std::size_t>{
                  offset(*selection_begin_)
              }
            : std::nullopt;
    const auto selected = selection();
    const auto selection_offsets =
        selected
            ? std::optional<std::pair<std::size_t, std::size_t>>{
                  {
                      offset(selected->first),
                      offset(selected->second),
                  }
              }
            : std::nullopt;
    record_undo();
    auto serialized = content();
    const auto begin_offset = offset(begin);
    const auto end_offset = offset(end);
    serialized.erase(begin_offset, end_offset - begin_offset);
    set_content(serialized);
    const auto cursor = position(begin_offset);
    row_ = cursor.row;
    column_ = cursor.column;
    if (anchor_offset) {
        auto adjusted_anchor = *anchor_offset;
        const auto removed = end_offset - begin_offset;
        if (end_offset <= adjusted_anchor) {
            adjusted_anchor -= removed;
        } else if (begin_offset < adjusted_anchor) {
            adjusted_anchor = begin_offset;
        }
        selection_begin_ = position(adjusted_anchor);
        selection_end_ = cursor;
        marking_ = true;
    } else if (selection_offsets) {
        auto selection_begin = selection_offsets->first;
        auto selection_end = selection_offsets->second;
        const auto removed = end_offset - begin_offset;
        if (end_offset <= selection_begin) {
            selection_begin -= removed;
            selection_end -= removed;
        } else if (begin_offset < selection_end) {
            selection_begin =
                std::min(selection_begin, begin_offset);
            selection_end =
                selection_end <= end_offset
                    ? begin_offset
                    : selection_end - removed;
        }
        selection_begin_ = position(selection_begin);
        selection_end_ = position(selection_end);
        marking_ = false;
    } else {
        clear_selection();
    }
    dirty_ = true;
}

void EditorModel::backspace() {
    if (column_ == 0U && row_ == 0U) {
        return;
    }
    if (column_ > 0U) {
        const auto previous =
            previous_character(lines_[row_], column_);
        erase_text(
            {.row = row_, .column = previous},
            {.row = row_, .column = column_}
        );
        return;
    }
    erase_text(
        {
            .row = row_ - 1U,
            .column = lines_[row_ - 1U].size(),
        },
        {.row = row_, .column = 0U}
    );
}

void EditorModel::erase() {
    if (column_ >= lines_[row_].size() &&
        row_ + 1U >= lines_.size()) {
        return;
    }
    if (column_ < lines_[row_].size()) {
        const auto next = next_character(lines_[row_], column_);
        erase_text(
            {.row = row_, .column = column_},
            {.row = row_, .column = next}
        );
        return;
    }
    erase_text(
        {.row = row_, .column = column_},
        {.row = row_ + 1U, .column = 0U}
    );
}

void EditorModel::delete_line() {
    const EditorPosition begin{
        .row = row_,
        .column = 0U,
    };
    const EditorPosition end =
        row_ + 1U < lines_.size()
            ? EditorPosition{
                  .row = row_ + 1U,
                  .column = 0U,
              }
            : EditorPosition{
                  .row = row_,
                  .column = lines_[row_].size(),
              };
    if (begin == end) {
        return;
    }
    erase_text(begin, end);
}

void EditorModel::delete_to_end() {
    if (column_ >= lines_[row_].size()) {
        return;
    }
    clear_selection();
    record_undo();
    lines_[row_].erase(column_);
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
    insert_text(clipboard_, false, true);
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
