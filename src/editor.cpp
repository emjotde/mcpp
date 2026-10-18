#include "mcpp/editor.hpp"
#include "mcpp/local_provider.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <sstream>
#include <stdexcept>

#include <ftxui/screen/string.hpp>

namespace mcpp {
namespace {

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

std::size_t display_column(
    const std::string_view line,
    const std::size_t byte_column,
    const std::size_t tab_width
) {
    std::size_t result = 0;
    for (std::size_t offset = 0;
         offset < std::min(byte_column, line.size());) {
        if (line[offset] == '\t') {
            result += tab_width - result % tab_width;
            ++offset;
            continue;
        }
        const auto next = next_character(line, offset);
        result += static_cast<std::size_t>(
            std::max(
                0,
                ftxui::string_width(
                    line.substr(offset, next - offset)
                )
            )
        );
        offset = next;
    }
    return result;
}

std::size_t byte_column(
    const std::string_view line,
    const std::size_t target,
    const std::size_t tab_width
) {
    std::size_t display = 0;
    std::size_t offset = 0;
    while (offset < line.size()) {
        std::size_t width = 0;
        std::size_t next = offset + 1U;
        if (line[offset] == '\t') {
            width = tab_width - display % tab_width;
        } else {
            next = next_character(line, offset);
            width = static_cast<std::size_t>(
                std::max(
                    0,
                    ftxui::string_width(
                        line.substr(offset, next - offset)
                    )
                )
            );
        }
        if (display + width > target) {
            break;
        }
        display += width;
        offset = next;
    }
    return offset;
}

std::size_t prepare_column_insertion(
    std::string& line,
    const std::size_t target,
    const std::size_t tab_width
) {
    auto offset = byte_column(line, target, tab_width);
    const auto actual =
        display_column(line, offset, tab_width);
    if (actual < target) {
        const auto padding = target - actual;
        line.insert(offset, padding, ' ');
        offset += padding;
    }
    return offset;
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
    if (!provider) {
        throw std::invalid_argument("editor provider is null");
    }
    const auto entry = provider->stat_follow(path, cancellation);
    if (entry.kind != EntryKind::file) {
        throw std::runtime_error("editor requires a regular file");
    }
    constexpr std::uint64_t maximum = 16U * 1024U * 1024U;
    if (entry.size > maximum) {
        throw std::runtime_error(
            "internal editor limit is 16 MiB"
        );
    }
    std::ostringstream output;
    provider->read(
        entry.path,
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
    provider_ = std::move(provider);
    path_ = std::move(path);
    storage_path_ = entry.path;
    revision_ = entry.revision;
    set_content(loaded);
    row_ = 0;
    column_ = 0;
    preferred_display_column_.reset();
    dirty_ = false;
    ++change_id_;
    overwrite_ = false;
    new_file_ = false;
    clear_selection();
    undo_.clear();
    redo_.clear();
    pending_undo_.reset();
    pending_content_.clear();
    search_match_.reset();
    search_resume_.reset();
    undo_bytes_ = 0;
    redo_bytes_ = 0;
}

void EditorModel::create(
    ProviderPtr provider,
    std::string path
) {
    if (!provider) {
        throw std::invalid_argument("editor provider is null");
    }
    if (std::filesystem::path(path).is_relative() && !path_.empty()) {
        path = provider->join(provider->parent(path_), path);
    }
    path = provider->normalize(path);
    if (provider->exists(path)) {
        throw std::runtime_error("destination exists: " + path);
    }
    provider_ = std::move(provider);
    path_ = std::move(path);
    storage_path_ = path_;
    revision_.clear();
    lines_ = {{}};
    row_ = 0;
    column_ = 0;
    preferred_display_column_.reset();
    dirty_ = false;
    overwrite_ = false;
    new_file_ = true;
    clear_selection();
    undo_.clear();
    redo_.clear();
    pending_undo_.reset();
    pending_content_.clear();
    search_match_.reset();
    search_resume_.reset();
    undo_bytes_ = 0;
    redo_bytes_ = 0;
    ++change_id_;
}

void EditorModel::close() {
    provider_.reset();
    path_.clear();
    storage_path_.clear();
    revision_.clear();
    lines_.clear();
    row_ = 0;
    column_ = 0;
    preferred_display_column_.reset();
    dirty_ = false;
    ++change_id_;
    overwrite_ = false;
    new_file_ = false;
    clear_selection();
    undo_.clear();
    redo_.clear();
    pending_undo_.reset();
    pending_content_.clear();
    search_match_.reset();
    search_resume_.reset();
    undo_bytes_ = 0;
    redo_bytes_ = 0;
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

std::size_t EditorModel::cursor_offset() const noexcept {
    return offset({.row = row_, .column = column_});
}

std::size_t EditorModel::selection_size() const {
    const auto selected = selection();
    if (!selected) {
        return 0U;
    }
    if (!column_selection_) {
        return offset(selected->second) -
               offset(selected->first);
    }
    std::size_t result = 0;
    for (auto row = selected->first.row;
         row <= selected->second.row;
         ++row) {
        const auto range = selection_on_line(row);
        result += range->second - range->first;
        if (row != selected->second.row) {
            ++result;
        }
    }
    return result;
}

std::uint64_t EditorModel::change_id() const noexcept {
    return change_id_;
}

bool EditorModel::marking() const noexcept {
    return marking_;
}

bool EditorModel::overwrite() const noexcept {
    return overwrite_;
}

bool EditorModel::column_selection() const noexcept {
    return column_selection_;
}

std::optional<std::pair<EditorPosition, EditorPosition>>
EditorModel::selection() const noexcept {
    if (!selection_begin_ || !selection_end_ ||
        *selection_begin_ == *selection_end_) {
        return std::nullopt;
    }
    return std::minmax(*selection_begin_, *selection_end_);
}

std::pair<std::size_t, std::size_t>
EditorModel::column_selection_columns() const {
    const auto begin = display_column(
        lines_[selection_begin_->row],
        selection_begin_->column,
        tab_width_
    );
    const auto end = display_column(
        lines_[selection_end_->row],
        selection_end_->column,
        tab_width_
    );
    if (column_selection_width_) {
        return {begin, begin + *column_selection_width_};
    }
    return std::minmax(begin, end);
}

std::optional<std::pair<std::size_t, std::size_t>>
EditorModel::selection_on_line(const std::size_t row) const {
    const auto selected = selection();
    if (!selected || row >= lines_.size() ||
        row < selected->first.row ||
        row > selected->second.row) {
        return std::nullopt;
    }
    if (!column_selection_) {
        const auto begin =
            row == selected->first.row
                ? selected->first.column
                : 0U;
        const auto end =
            row == selected->second.row
                ? selected->second.column
                : lines_[row].size();
        return std::pair{begin, end};
    }
    const auto [left, right] = column_selection_columns();
    return std::pair{
        byte_column(lines_[row], left, tab_width_),
        byte_column(lines_[row], right, tab_width_),
    };
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
    preferred_display_column_ =
        display_column(lines_[row_], column_, tab_width_);
    clear_selection();
}

void EditorModel::move(const int rows, const int columns) {
    if (lines_.empty()) {
        return;
    }
    if (rows != 0 && !preferred_display_column_) {
        preferred_display_column_ =
            display_column(lines_[row_], column_, tab_width_);
    }
    row_ = static_cast<std::size_t>(std::clamp<long long>(
        static_cast<long long>(row_) + rows,
        0,
        static_cast<long long>(lines_.size() - 1U)
    ));
    column_ =
        rows == 0 || !preferred_display_column_
            ? std::min(column_, lines_[row_].size())
            : byte_column(
                  lines_[row_],
                  *preferred_display_column_,
                  tab_width_
              );
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
    if (columns != 0) {
        preferred_display_column_ =
            display_column(lines_[row_], column_, tab_width_);
    }
    update_mark();
}

void EditorModel::move_word(const int direction) {
    if (lines_.empty() || direction == 0) {
        return;
    }
    const auto whitespace = [&](const std::size_t column) {
        return std::isspace(
            static_cast<unsigned char>(lines_[row_][column])
        ) != 0;
    };
    if (direction < 0) {
        if (column_ == 0U && row_ > 0U) {
            --row_;
            column_ = lines_[row_].size();
        } else {
            while (column_ > 0U) {
                const auto previous = previous_character(lines_[row_], column_);
                if (!whitespace(previous)) {
                    break;
                }
                column_ = previous;
            }
            if (column_ > 0U) {
                const auto word = word_character(
                    lines_[row_], previous_character(lines_[row_], column_)
                );
                while (column_ > 0U) {
                    const auto previous = previous_character(lines_[row_], column_);
                    if (whitespace(previous) ||
                        word_character(lines_[row_], previous) != word) {
                        break;
                    }
                    column_ = previous;
                }
            }
        }
    } else {
        if (column_ == lines_[row_].size() && row_ + 1U < lines_.size()) {
            ++row_;
            column_ = 0;
        } else {
            const auto word = word_character(lines_[row_], column_);
            while (column_ < lines_[row_].size() && !whitespace(column_) &&
                   word_character(lines_[row_], column_) == word) {
                column_ = next_character(lines_[row_], column_);
            }
            while (column_ < lines_[row_].size() && whitespace(column_)) {
                column_ = next_character(lines_[row_], column_);
            }
        }
    }
    preferred_display_column_ =
        display_column(lines_[row_], column_, tab_width_);
    update_mark();
}

void EditorModel::page(const int rows) {
    move(rows, 0);
}

void EditorModel::home() {
    column_ = 0;
    preferred_display_column_ =
        display_column(lines_[row_], column_, tab_width_);
    update_mark();
}

void EditorModel::end() {
    column_ = lines_[row_].size();
    preferred_display_column_ =
        display_column(lines_[row_], column_, tab_width_);
    update_mark();
}

void EditorModel::goto_line(const std::size_t line) {
    if (lines_.empty()) {
        return;
    }
    if (!preferred_display_column_) {
        preferred_display_column_ =
            display_column(lines_[row_], column_, tab_width_);
    }
    row_ = std::min(
        line == 0U ? 0U : line - 1U,
        lines_.size() - 1U
    );
    column_ = byte_column(
        lines_[row_], *preferred_display_column_, tab_width_
    );
    update_mark();
}

void EditorModel::set_position(EditorPosition destination) {
    if (lines_.empty()) {
        return;
    }
    row_ = std::min(destination.row, lines_.size() - 1U);
    column_ = std::min(
        destination.column,
        lines_[row_].size()
    );
    while (column_ > 0U &&
           column_ < lines_[row_].size() &&
           continuation(
               static_cast<unsigned char>(lines_[row_][column_])
           )) {
        --column_;
    }
    preferred_display_column_ =
        display_column(lines_[row_], column_, tab_width_);
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
    column_selection_ = false;
    selection_begin_ = cursor;
    selection_end_ = cursor;
    marking_ = true;
}

void EditorModel::toggle_column_mark() {
    const EditorPosition cursor{
        .row = row_,
        .column = column_,
    };
    if (marking_ && column_selection_) {
        selection_end_ = cursor;
        marking_ = false;
        return;
    }
    clear_selection();
    selection_begin_ = cursor;
    selection_end_ = cursor;
    marking_ = true;
    column_selection_ = true;
}

void EditorModel::begin_mark() {
    if (marking_) {
        return;
    }
    clear_selection();
    column_selection_ = false;
    selection_begin_ = EditorPosition{
        .row = row_,
        .column = column_,
    };
    selection_end_ = selection_begin_;
    marking_ = true;
}

void EditorModel::begin_column_mark() {
    if (marking_ && column_selection_) {
        return;
    }
    clear_selection();
    selection_begin_ = EditorPosition{
        .row = row_,
        .column = column_,
    };
    selection_end_ = selection_begin_;
    marking_ = true;
    column_selection_ = true;
}

void EditorModel::resume_mark() {
    if (!selection_begin_) {
        begin_mark();
        return;
    }
    column_selection_width_.reset();
    marking_ = true;
    update_mark();
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
    column_selection_ = false;
    column_selection_width_.reset();
}

void EditorModel::select_all() {
    if (lines_.empty()) {
        return;
    }
    column_selection_ = false;
    selection_begin_ = EditorPosition{};
    selection_end_ = EditorPosition{
        .row = lines_.size() - 1U,
        .column = lines_.back().size(),
    };
    marking_ = false;
}

bool EditorModel::match_bracket() {
    const auto serialized = content();
    auto cursor = offset({.row = row_, .column = column_});
    if (cursor >= serialized.size()) {
        return false;
    }
    const auto opening = std::string_view("([{");
    const auto closing = std::string_view(")]}");
    auto index = opening.find(serialized[cursor]);
    int direction = 1;
    char sought_open = '\0';
    char sought_close = '\0';
    if (index != std::string_view::npos) {
        sought_open = opening[index];
        sought_close = closing[index];
    } else {
        index = closing.find(serialized[cursor]);
        if (index == std::string_view::npos && cursor > 0U) {
            --cursor;
            index = closing.find(serialized[cursor]);
        }
        if (index == std::string_view::npos) {
            return false;
        }
        direction = -1;
        sought_open = opening[index];
        sought_close = closing[index];
    }
    int depth = 0;
    auto position_value = static_cast<std::ptrdiff_t>(cursor);
    while (true) {
        const auto value =
            serialized[static_cast<std::size_t>(position_value)];
        if (value == sought_open) {
            depth += direction;
        } else if (value == sought_close) {
            depth -= direction;
        }
        if (depth == 0 &&
            static_cast<std::size_t>(position_value) != cursor) {
            set_position(position(
                static_cast<std::size_t>(position_value)
            ));
            return true;
        }
        position_value += direction;
        if (position_value < 0 ||
            position_value >=
                static_cast<std::ptrdiff_t>(serialized.size())) {
            return false;
        }
    }
}

void EditorModel::toggle_overwrite() noexcept {
    overwrite_ = !overwrite_;
}

void EditorModel::set_tab_width(const std::size_t width) {
    tab_width_ = std::clamp(width, std::size_t{1}, std::size_t{32});
    if (!lines_.empty()) {
        preferred_display_column_ =
            display_column(lines_[row_], column_, tab_width_);
    }
}

std::size_t EditorModel::tab_width() const noexcept {
    return tab_width_;
}

void EditorModel::insert(const char character) {
    insert(std::string_view(&character, 1U));
}

void EditorModel::insert(const std::string_view text) {
    insert_text(text, overwrite_);
}

void EditorModel::paste(const std::string_view text) {
    insert_text(text, false);
}

void EditorModel::paste_column(const std::string_view text) {
    if (text.empty()) {
        return;
    }
    std::vector<std::string> block;
    std::size_t begin = 0;
    while (begin <= text.size()) {
        const auto end = text.find('\n', begin);
        block.emplace_back(
            text.substr(
                begin,
                end == std::string_view::npos
                    ? text.size() - begin
                    : end - begin
            )
        );
        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1U;
    }
    if (block.empty()) {
        return;
    }
    record_undo();
    const auto target_row = row_;
    const auto target_display =
        display_column(lines_[row_], column_, tab_width_);
    while (lines_.size() < target_row + block.size()) {
        lines_.emplace_back();
    }
    for (std::size_t index = 0; index < block.size(); ++index) {
        auto& line = lines_[target_row + index];
        const auto width =
            display_column(line, line.size(), tab_width_);
        if (width < target_display) {
            line.append(target_display - width, ' ');
        }
        const auto target =
            prepare_column_insertion(
                line,
                target_display,
                tab_width_
            );
        line.insert(target, block[index]);
    }
    row_ = target_row;
    column_ = byte_column(
        lines_[row_],
        target_display,
        tab_width_
    );
    preferred_display_column_ = target_display;
    clear_selection();
    dirty_ = true;
    ++change_id_;
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
    preferred_display_column_ =
        display_column(lines_[row_], column_, tab_width_);
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
    ++change_id_;
}

void EditorModel::newline(const bool auto_indent) {
    if (!auto_indent) {
        insert("\n");
        return;
    }
    const auto& line = lines_[row_];
    std::size_t indentation = 0;
    while (indentation < column_ &&
           indentation < line.size() &&
           (
               line[indentation] == ' ' ||
               line[indentation] == '\t'
           )) {
        ++indentation;
    }
    std::string text{"\n"};
    text.append(line, 0, indentation);
    insert(text);
}

void EditorModel::erase_text(
    const EditorPosition begin,
    const EditorPosition end
) {
    edit_text(begin, end, {});
}

void EditorModel::edit_text(
    const EditorPosition begin,
    const EditorPosition end,
    const std::string_view replacement
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
    serialized.replace(begin_offset, end_offset - begin_offset, replacement);
    set_content(serialized);
    const auto cursor = position(begin_offset + replacement.size());
    row_ = cursor.row;
    column_ = cursor.column;
    preferred_display_column_ =
        display_column(lines_[row_], column_, tab_width_);
    if (anchor_offset) {
        auto adjusted_anchor = *anchor_offset;
        const auto removed = end_offset - begin_offset;
        if (end_offset <= adjusted_anchor) {
            adjusted_anchor = adjusted_anchor - removed + replacement.size();
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
            selection_begin = selection_begin - removed + replacement.size();
            selection_end = selection_end - removed + replacement.size();
        } else if (begin_offset < selection_end) {
            selection_begin =
                std::min(selection_begin, begin_offset);
            selection_end =
                selection_end <= end_offset
                    ? begin_offset + replacement.size()
                    : selection_end - removed + replacement.size();
        }
        selection_begin_ = position(selection_begin);
        selection_end_ = position(selection_end);
        marking_ = false;
    } else {
        clear_selection();
    }
    dirty_ = true;
    ++change_id_;
}

void EditorModel::tab(const std::size_t indent_width, const bool fill_spaces) {
    const auto spacing = std::max<std::size_t>(1U, indent_width);
    const auto& line = lines_[row_];
    const bool indentation = std::all_of(
        line.begin(), line.begin() + static_cast<std::ptrdiff_t>(column_),
        [](const char character) { return character == ' ' || character == '\t'; }
    );
    const auto column = display_column(line, column_, tab_width_);
    if (!indentation) {
        insert(fill_spaces ? std::string(tab_width_ - column % tab_width_, ' ') : "\t");
        return;
    }
    const bool collapse =
        !fill_spaces && spacing == tab_width_ / 2U &&
        column_ >= spacing && column % tab_width_ == spacing &&
        std::all_of(
            line.begin() + static_cast<std::ptrdiff_t>(column_ - spacing),
            line.begin() + static_cast<std::ptrdiff_t>(column_),
            [](const char character) { return character == ' '; }
        );
    if (!collapse) {
        insert(std::string(spacing - column % spacing, ' '));
        return;
    }
    edit_text({row_, column_ - spacing}, {row_, column_}, "\t");
}

void EditorModel::backspace(const std::size_t indent_width) {
    if (column_ == 0U && row_ == 0U) {
        return;
    }
    if (column_ > 0U) {
        const auto spacing = indent_width == 0U ? std::max<std::size_t>(1U, tab_width_ / 2U)
                                               : indent_width;
        const auto& line = lines_[row_];
        if (column_ >= spacing &&
            display_column(line, column_, tab_width_) % spacing == 0U &&
            std::all_of(line.begin(), line.begin() + static_cast<std::ptrdiff_t>(column_),
                        [](const char character) { return character == ' ' || character == '\t'; }) &&
            std::all_of(
                line.begin() + static_cast<std::ptrdiff_t>(column_ - spacing),
                line.begin() + static_cast<std::ptrdiff_t>(column_),
                [](const char character) { return character == ' '; }
            )) {
            erase_text({row_, column_ - spacing}, {row_, column_});
            return;
        }
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

void EditorModel::delete_block_or_line() {
    if (selection_begin_) {
        static_cast<void>(delete_selection());
        clear_selection();
    } else {
        delete_line();
    }
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
    erase_text(
        {.row = row_, .column = column_},
        {.row = row_, .column = lines_[row_].size()}
    );
}

void EditorModel::mark_column_block(
    const std::size_t row,
    const std::size_t height,
    const std::size_t left,
    const std::size_t width
) {
    selection_begin_ = EditorPosition{
        .row = row,
        .column = byte_column(lines_[row], left, tab_width_),
    };
    const auto last = row + height - 1U;
    selection_end_ = EditorPosition{
        .row = last,
        .column = byte_column(lines_[last], left + width, tab_width_),
    };
    column_selection_width_ = width;
    column_selection_ = true;
    marking_ = false;
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
    if (column_selection_) {
        const auto block = selected_text();
        if (!block) {
            return false;
        }
        clipboard_ = *block;
        const auto target_row = row_;
        const auto target_display =
            display_column(lines_[row_], column_, tab_width_);
        const auto [left, right] = column_selection_columns();
        const auto width = right - left;
        const auto height =
            selected->second.row - selected->first.row + 1U;
        paste_column(clipboard_);
        mark_column_block(target_row, height, target_display, width);
        return true;
    }
    insert_text(clipboard_, false, true);
    return true;
}

bool EditorModel::move_selection() {
    const auto selected = selection();
    if (!selected) {
        return false;
    }
    if (column_selection_) {
        const auto block_text = selected_text();
        if (!block_text) {
            return false;
        }
        const auto [left, right] = column_selection_columns();
        auto target_display =
            display_column(lines_[row_], column_, tab_width_);
        if (row_ >= selected->first.row &&
            row_ <= selected->second.row &&
            target_display >= left &&
            target_display <= right) {
            return false;
        }
        if (row_ >= selected->first.row &&
            row_ <= selected->second.row &&
            target_display > right) {
            target_display -= right - left;
        }
        std::vector<std::pair<std::size_t, std::size_t>> ranges;
        for (auto row = selected->first.row;
             row <= selected->second.row;
             ++row) {
            ranges.push_back(*selection_on_line(row));
        }
        std::vector<std::string> block;
        std::size_t block_begin = 0;
        while (block_begin <= block_text->size()) {
            const auto block_end =
                block_text->find('\n', block_begin);
            block.emplace_back(block_text->substr(
                block_begin,
                block_end == std::string::npos
                    ? block_text->size() - block_begin
                    : block_end - block_begin
            ));
            if (block_end == std::string::npos) {
                break;
            }
            block_begin = block_end + 1U;
        }
        record_undo();
        for (std::size_t index = 0; index < ranges.size(); ++index) {
            auto& line = lines_[selected->first.row + index];
            line.erase(
                ranges[index].first,
                ranges[index].second - ranges[index].first
            );
        }
        const auto target_row = row_;
        while (lines_.size() < target_row + block.size()) {
            lines_.emplace_back();
        }
        for (std::size_t index = 0; index < block.size(); ++index) {
            auto& line = lines_[target_row + index];
            const auto line_width =
                display_column(line, line.size(), tab_width_);
            if (line_width < target_display) {
                line.append(target_display - line_width, ' ');
            }
            line.insert(
                prepare_column_insertion(
                    line,
                    target_display,
                    tab_width_
                ),
                block[index]
            );
        }
        column_ = byte_column(
            lines_[target_row],
            target_display,
            tab_width_
        );
        row_ = target_row;
        preferred_display_column_ = target_display;
        mark_column_block(
            target_row,
            block.size(),
            target_display,
            right - left
        );
        dirty_ = true;
        ++change_id_;
        return true;
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
    preferred_display_column_ =
        display_column(lines_[row_], column_, tab_width_);
    selection_begin_ = position(destination);
    selection_end_ = cursor;
    marking_ = false;
    dirty_ = true;
    ++change_id_;
    return true;
}

bool EditorModel::delete_selection() {
    const auto selected = selection();
    if (!selected) {
        return false;
    }
    if (column_selection_) {
        std::vector<std::pair<std::size_t, std::size_t>> ranges;
        ranges.reserve(
            selected->second.row - selected->first.row + 1U
        );
        for (auto row = selected->first.row;
             row <= selected->second.row;
             ++row) {
            ranges.push_back(*selection_on_line(row));
        }
        if (std::ranges::all_of(
                ranges,
                [](const auto range) {
                    return range.first == range.second;
                }
            )) {
            return true;
        }
        record_undo();
        for (std::size_t index = 0; index < ranges.size(); ++index) {
            auto& line = lines_[selected->first.row + index];
            line.erase(
                ranges[index].first,
                ranges[index].second - ranges[index].first
            );
        }
        row_ = selected->first.row;
        column_ = ranges.front().first;
        preferred_display_column_ =
            display_column(lines_[row_], column_, tab_width_);
        clear_selection();
        dirty_ = true;
        ++change_id_;
        return true;
    }
    record_undo();
    replace_range(selected->first, selected->second, "");
    dirty_ = true;
    ++change_id_;
    return true;
}

std::optional<std::string> EditorModel::selected_text() const {
    const auto selected = selection();
    if (!selected) {
        return std::nullopt;
    }
    if (column_selection_) {
        const auto [left, right] = column_selection_columns();
        if (left == right) {
            return std::nullopt;
        }
        std::string result;
        for (auto row = selected->first.row;
             row <= selected->second.row;
             ++row) {
            const auto range = selection_on_line(row);
            result.append(
                lines_[row],
                range->first,
                range->second - range->first
            );
            if (row != selected->second.row) {
                result += '\n';
            }
        }
        return result;
    }
    const auto begin_offset = offset(selected->first);
    const auto end_offset = offset(selected->second);
    return content().substr(
        begin_offset,
        end_offset - begin_offset
    );
}

bool EditorModel::replace_selection(
    const std::string_view replacement,
    std::optional<std::pair<
        EditorPosition,
        EditorPosition
    >>* scope
) {
    const auto match = selected_match();
    if (!match) {
        return false;
    }
    const auto edit_begin = match->begin;
    const auto edit_end = match->end;
    const auto begin_position = position(edit_begin);
    const auto end_position = position(edit_end);
    std::optional<std::pair<std::size_t, std::size_t>>
        scope_offsets;
    if (scope != nullptr && *scope) {
        auto begin = offset((*scope)->first);
        auto end = offset((*scope)->second);
        if (begin > end) {
            std::swap(begin, end);
        }
        scope_offsets = {{begin, end}};
    }
    record_undo();
    replace_range(
        begin_position,
        end_position,
        replacement
    );
    if (scope_offsets) {
        auto [begin, end] = *scope_offsets;
        const auto delta =
            static_cast<std::ptrdiff_t>(replacement.size()) -
            static_cast<std::ptrdiff_t>(edit_end - edit_begin);
        if (edit_end <= begin) {
            begin = static_cast<std::size_t>(
                static_cast<std::ptrdiff_t>(begin) + delta
            );
            end = static_cast<std::size_t>(
                static_cast<std::ptrdiff_t>(end) + delta
            );
        } else if (edit_begin < end) {
            end = static_cast<std::size_t>(
                static_cast<std::ptrdiff_t>(end) + delta
            );
        }
        **scope = std::pair{
            position(begin),
            position(end),
        };
    }
    dirty_ = true;
    ++change_id_;
    if (edit_begin == edit_end) {
        const auto value = content();
        const auto cursor = offset({row_, column_});
        search_resume_ = search_match_options_.backwards
                             ? edit_begin
                             : cursor < value.size() ? next_character(value, cursor) : value.size() + 1U;
        search_match_change_id_ = change_id_;
    }
    return true;
}

void EditorModel::record_undo() {
    finalize_undo();
    pending_undo_ = snapshot();
    pending_content_ = content();
    redo_.clear();
    redo_bytes_ = 0;
}

void EditorModel::finalize_undo() {
    if (!pending_undo_) {
        return;
    }
    const auto current = content();
    std::size_t prefix = 0;
    while (prefix < pending_content_.size() && prefix < current.size() &&
           pending_content_[prefix] == current[prefix]) {
        ++prefix;
    }
    std::size_t suffix = 0;
    while (suffix < pending_content_.size() - prefix &&
           suffix < current.size() - prefix &&
           pending_content_[pending_content_.size() - suffix - 1U] ==
               current[current.size() - suffix - 1U]) {
        ++suffix;
    }
    UndoRecord entry{
        *pending_undo_, snapshot(), prefix,
        pending_content_.substr(prefix, pending_content_.size() - prefix - suffix),
        current.substr(prefix, current.size() - prefix - suffix),
    };
    pending_undo_.reset();
    pending_content_.clear();
    if (!entry.removed.empty() || !entry.inserted.empty()) {
        undo_bytes_ += entry.bytes();
        undo_.push_back(std::move(entry));
        trim_undo();
    }
}

EditorModel::Snapshot EditorModel::snapshot() const {
    return {
        .row = row_,
        .column = column_,
        .selection_begin = selection_begin_,
        .selection_end = selection_end_,
        .marking = marking_,
        .column_selection = column_selection_,
        .column_selection_width = column_selection_width_,
        .byte_size = byte_size(),
    };
}

void EditorModel::trim_undo() {
    constexpr std::size_t maximum = 32768U;
    constexpr std::size_t maximum_bytes = 64U * 1024U * 1024U;
    while (undo_.size() > 1U &&
           (
               undo_.size() > maximum ||
               undo_bytes_ > maximum_bytes
           )) {
        undo_bytes_ -= undo_.front().bytes();
        undo_.erase(undo_.begin());
    }
}

void EditorModel::restore(Snapshot snapshot) {
    row_ = snapshot.row;
    column_ = snapshot.column;
    preferred_display_column_ =
        display_column(lines_[row_], column_, tab_width_);
    selection_begin_ = snapshot.selection_begin;
    selection_end_ = snapshot.selection_end;
    marking_ = snapshot.marking;
    column_selection_ = snapshot.column_selection;
    column_selection_width_ = snapshot.column_selection_width;
    dirty_ = true;
    ++change_id_;
}

bool EditorModel::undo() {
    finalize_undo();
    if (undo_.empty()) {
        return false;
    }
    auto previous = std::move(undo_.back());
    undo_bytes_ -= previous.bytes();
    undo_.pop_back();
    previous.after = snapshot();
    auto value = content();
    if (value.substr(previous.offset, previous.inserted.size()) != previous.inserted) {
        throw std::runtime_error("undo history does not match the document");
    }
    value.replace(previous.offset, previous.inserted.size(), previous.removed);
    set_content(value);
    restore(previous.before);
    redo_bytes_ += previous.bytes();
    redo_.push_back(std::move(previous));
    return true;
}

bool EditorModel::redo() {
    finalize_undo();
    if (redo_.empty()) {
        return false;
    }
    auto next = std::move(redo_.back());
    redo_bytes_ -= next.bytes();
    redo_.pop_back();
    auto value = content();
    if (value.substr(next.offset, next.removed.size()) != next.removed) {
        throw std::runtime_error("redo history does not match the document");
    }
    value.replace(next.offset, next.removed.size(), next.inserted);
    set_content(value);
    restore(next.after);
    undo_bytes_ += next.bytes();
    undo_.push_back(std::move(next));
    trim_undo();
    return true;
}

void EditorModel::save(
    const bool force,
    const Cancellation& cancellation
) {
    if (!provider_) {
        throw std::runtime_error("editor is not open");
    }
    if (!new_file_) {
        if (const auto* local = dynamic_cast<const LocalProvider*>(provider_.get())) {
            local->ensure_writable(storage_path_);
        }
    }
    auto serialized = content();
    std::istringstream input(serialized);
    provider_->write(
        storage_path_,
        input,
        serialized.size(),
        force || !new_file_,
        force || new_file_ ? std::nullopt
              : std::optional<std::string>(revision_),
        cancellation,
        {}
    );
    revision_ =
        provider_->stat(storage_path_, cancellation).revision;
    new_file_ = false;
    dirty_ = false;
}

std::uintmax_t EditorModel::hardlink_count() const {
    if (!provider_ || new_file_) {
        return 1U;
    }
    if (const auto* local = dynamic_cast<const LocalProvider*>(provider_.get())) {
        return local->hardlink_count(storage_path_);
    }
    throw std::runtime_error("hardlink inspection is unavailable for this provider");
}

void EditorModel::save_preserving_links(
    const bool force, const Cancellation& cancellation
) {
    if (!provider_) {
        throw std::runtime_error("editor is not open");
    }
    if (new_file_) {
        save(force, cancellation);
        return;
    }
    auto* local = dynamic_cast<LocalProvider*>(provider_.get());
    if (local == nullptr) {
        throw std::runtime_error("linked saves are unavailable for this provider");
    }
    const auto value = content();
    std::istringstream input(value);
    local->write_in_place(
        storage_path_, input, value.size(),
        force ? std::optional<std::string>{provider_->stat(storage_path_, cancellation).revision}
              : std::optional<std::string>{revision_},
        cancellation
    );
    revision_ = provider_->stat(storage_path_, cancellation).revision;
    dirty_ = false;
}

void EditorModel::save_as(
    std::string path,
    const bool force,
    const Cancellation& cancellation
) {
    save_as(std::move(path), force, EditorLineEnding::unchanged, cancellation);
}

void EditorModel::save_as(
    std::string path,
    const bool force,
    const EditorLineEnding ending,
    const Cancellation& cancellation
) {
    save_as_impl(std::move(path), force, ending, false, cancellation);
}

void EditorModel::save_as_preserving_links(
    std::string path,
    const bool force,
    const EditorLineEnding ending,
    const Cancellation& cancellation
) {
    save_as_impl(std::move(path), force, ending, true, cancellation);
}

void EditorModel::save_as_impl(
    std::string path, const bool force, const EditorLineEnding ending,
    const bool preserve_links, const Cancellation& cancellation
) {
    if (!provider_) {
        throw std::runtime_error("editor is not open");
    }
    if (std::filesystem::path(path).is_relative()) {
        path = provider_->join(provider_->parent(path_), path);
    }
    path = provider_->normalize(path);
    if (path == provider_->normalize(path_) && ending == EditorLineEnding::unchanged) {
        if (preserve_links) {
            save_preserving_links(force, cancellation);
        } else {
            save(force, cancellation);
        }
        return;
    }
    const bool same_path = path == provider_->normalize(path_);
    if (!force && !same_path && provider_->exists(path)) {
        throw std::runtime_error("destination exists: " + path);
    }
    auto serialized = content();
    if (ending != EditorLineEnding::unchanged) {
        std::string converted;
        converted.reserve(serialized.size());
        for (std::size_t offset = 0; offset < serialized.size(); ++offset) {
            const char character = serialized[offset];
            if (character == '\r' || character == '\n') {
                if (character == '\r' && offset + 1U < serialized.size() &&
                    serialized[offset + 1U] == '\n') {
                    ++offset;
                }
                if (ending == EditorLineEnding::crlf) {
                    converted += "\r\n";
                } else {
                    converted += ending == EditorLineEnding::cr ? '\r' : '\n';
                }
            } else {
                converted += character;
            }
        }
        serialized = std::move(converted);
    }
    auto storage = path;
    std::optional<std::string> expected;
    const bool target_exists = provider_->exists(path);
    if (target_exists) {
        const auto target = provider_->stat_follow(path, cancellation);
        if (target.kind != EntryKind::file) {
            throw std::runtime_error("save target is not a regular file: " + path);
        }
        storage = target.path;
        expected = same_path && !force ? revision_ : target.revision;
        if (const auto* local = dynamic_cast<const LocalProvider*>(provider_.get())) {
            local->ensure_writable(storage);
        }
    }
    std::istringstream input(serialized);
    if (preserve_links && target_exists) {
        auto* local = dynamic_cast<LocalProvider*>(provider_.get());
        if (local == nullptr) {
            throw std::runtime_error("linked saves are unavailable for this provider");
        }
        local->write_in_place(storage, input, serialized.size(), expected, cancellation);
    } else {
        provider_->write(
            storage, input, serialized.size(),
            target_exists && (force || same_path), expected, cancellation, {}
        );
    }
    if (ending != EditorLineEnding::unchanged) {
        record_undo();
        set_content(serialized);
        row_ = std::min(row_, lines_.size() - 1U);
        column_ = std::min(column_, lines_[row_].size());
        preferred_display_column_.reset();
        clear_selection();
        ++change_id_;
    }
    path_ = std::move(path);
    storage_path_ = std::move(storage);
    revision_ = provider_->stat(storage_path_, cancellation).revision;
    new_file_ = false;
    dirty_ = false;
}

bool EditorModel::find(
    const std::string_view needle,
    const bool case_sensitive,
    const bool wrap,
    const bool overlap
) {
    return find(
        needle,
        {
            .mode = EditorSearchMode::normal,
            .case_sensitive = case_sensitive,
        },
        wrap,
        overlap
    );
}

bool EditorModel::find(
    const std::string_view needle,
    const EditorSearchOptions& options,
    const bool wrap,
    const bool overlap,
    const std::optional<std::pair<
        EditorPosition,
        EditorPosition
    >> scope
) {
    if (needle.empty()) {
        return false;
    }
    const auto serialized = content();
    auto start = offset({
        .row = row_,
        .column = column_,
    });
    if (const auto selected = selected_match();
        selected && EditorPosition{row_, column_} == position(selected->begin)) {
        if (options.backwards) {
            start = selected->begin;
        } else {
            start =
                overlap || selected->begin == selected->end
                    ? next_character(serialized, start)
                    : selected->end;
            if (selected->begin == selected->end && selected->end == serialized.size()) {
                start = serialized.size() + 1U;
            }
        }
    }
    if (search_resume_ && search_match_change_id_ == change_id_) {
        start = *search_resume_;
    }
    search_resume_.reset();
    auto range_begin = std::size_t{0};
    auto range_end = serialized.size();
    if (scope) {
        range_begin = offset(scope->first);
        range_end = offset(scope->second);
        if (range_begin > range_end) {
            std::swap(range_begin, range_end);
        }
        if (start > range_end && !options.backwards) {
            if (!wrap) {
                return false;
            }
            start = range_begin;
        }
    }
    const auto found = EditorSearch::find(
        serialized,
        needle,
        std::clamp(start, range_begin, range_end),
        range_begin,
        range_end,
        options,
        wrap,
        overlap
    );
    if (!found) {
        return false;
    }
    selection_begin_ = position(found->begin);
    selection_end_ = position(found->end);
    marking_ = false;
    column_selection_ = false;
    search_match_ = found;
    search_match_pattern_ = needle;
    search_match_options_ = options;
    search_match_change_id_ = change_id_;
    row_ = selection_begin_->row;
    column_ = selection_begin_->column;
    preferred_display_column_ =
        display_column(lines_[row_], column_, tab_width_);
    return true;
}

std::size_t EditorModel::replace_all(
    const std::string_view needle,
    const std::string_view replacement,
    const bool case_sensitive
) {
    return replace_all(
        needle,
        replacement,
        {
            .mode = EditorSearchMode::normal,
            .case_sensitive = case_sensitive,
        }
    );
}

std::size_t EditorModel::replace_all(
    const std::string_view needle,
    const std::string_view replacement_value,
    const EditorSearchOptions& options,
    const std::optional<std::pair<
        EditorPosition,
        EditorPosition
    >> scope
) {
    const auto selected = selected_match();
    if (!selected || needle.empty()) {
        return 0U;
    }
    const auto original = content();
    auto range_begin =
        options.backwards ? 0U : selected->begin;
    auto range_end =
        options.backwards ? selected->end
                          : original.size();
    if (scope) {
        const auto scope_begin = offset(scope->first);
        const auto scope_end = offset(scope->second);
        range_begin = std::max(
            range_begin,
            std::min(scope_begin, scope_end)
        );
        range_end = std::min(
            range_end,
            std::max(scope_begin, scope_end)
        );
    }
    const auto matches = EditorSearch::find_all(
        original,
        needle,
        range_begin,
        range_end,
        options
    );
    if (matches.empty()) {
        return 0U;
    }
    std::string replaced;
    replaced.reserve(original.size());
    replaced.append(original, 0, range_begin);
    auto cursor = range_begin;
    auto final_cursor = range_begin;
    for (const auto& match : matches) {
        replaced.append(
            original,
            cursor,
            match.begin - cursor
        );
        replaced += EditorSearch::replacement(
            original,
            match,
            needle,
            replacement_value,
            options
        );
        final_cursor = replaced.size();
        cursor = match.end;
    }
    replaced.append(
        original,
        cursor,
        range_end - cursor
    );
    replaced.append(original, range_end, std::string::npos);
    record_undo();
    set_content(replaced);
    const auto destination = position(final_cursor);
    row_ = destination.row;
    column_ = destination.column;
    preferred_display_column_ =
        display_column(lines_[row_], column_, tab_width_);
    clear_selection();
    dirty_ = true;
    ++change_id_;
    return matches.size();
}

std::string EditorModel::replacement_for_selection(
    const std::string_view needle,
    const std::string_view replacement_value,
    const EditorSearchOptions& options
) const {
    auto selected = selected_match();
    if (!selected) {
        return std::string(replacement_value);
    }
    if (needle != search_match_pattern_ || options.mode != search_match_options_.mode ||
        options.case_sensitive != search_match_options_.case_sensitive) {
        selected->captures.clear();
    }
    const auto serialized = content();
    return EditorSearch::replacement(
        serialized,
        *selected,
        needle,
        replacement_value,
        options
    );
}

std::optional<EditorSearchMatch> EditorModel::selected_match() const {
    if (const auto selected = selection()) {
        const auto begin = offset(selected->first);
        const auto end = offset(selected->second);
        if (search_match_ && search_match_change_id_ == change_id_ &&
            search_match_->begin == begin && search_match_->end == end) {
            return search_match_;
        }
        return EditorSearchMatch{begin, end};
    }
    if (search_match_ && search_match_change_id_ == change_id_ &&
        search_match_->begin == search_match_->end &&
        EditorPosition{row_, column_} == position(search_match_->begin) &&
        selection_begin_ == selection_end_ && selection_begin_.has_value()) {
        return search_match_;
    }
    return std::nullopt;
}

std::size_t EditorModel::count_matches(
    const std::string_view needle,
    const EditorSearchOptions& options,
    const std::optional<std::pair<
        EditorPosition,
        EditorPosition
    >> scope
) const {
    const auto serialized = content();
    auto range_begin = std::size_t{0};
    auto range_end = serialized.size();
    if (scope) {
        range_begin = offset(scope->first);
        range_end = offset(scope->second);
        if (range_begin > range_end) {
            std::swap(range_begin, range_end);
        }
    }
    return EditorSearch::find_all(
        serialized,
        needle,
        range_begin,
        range_end,
        options
    ).size();
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
    ++change_id_;
    return true;
}

}  // namespace mcpp
