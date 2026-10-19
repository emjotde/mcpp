#include "mcpp/editor.hpp"
#include "mcpp/local_provider.hpp"
#include "mcpp/operations.hpp"
#include "mcpp/local_tools.hpp"

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
    encoding_.clear();
    storage_offsets_.clear();
    bookmarks_.clear();
    line_separator_ = loaded.find('\n') == std::string::npos &&
                              loaded.find('\r') != std::string::npos
                          ? '\r' : '\n';
    set_content(loaded);
    row_ = 0;
    column_ = 0;
    over_column_ = 0;
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
    if (!path.empty() && std::filesystem::path(path).is_relative() && !path_.empty()) {
        path = provider->join(provider->parent(path_), path);
    }
    if (!path.empty()) path = provider->normalize(path);
    if (!path.empty() && provider->exists(path)) {
        throw std::runtime_error("destination exists: " + path);
    }
    provider_ = std::move(provider);
    path_ = std::move(path);
    storage_path_ = path_;
    revision_.clear();
    encoding_.clear();
    storage_offsets_.clear();
    lines_ = {{}};
    bookmarks_.clear();
    row_ = 0;
    column_ = 0;
    over_column_ = 0;
    preferred_display_column_.reset();
    dirty_ = false;
    overwrite_ = false;
    new_file_ = true;
    line_separator_ = '\n';
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
    encoding_.clear();
    storage_offsets_.clear();
    lines_.clear();
    bookmarks_.clear();
    row_ = 0;
    column_ = 0;
    over_column_ = 0;
    preferred_display_column_.reset();
    dirty_ = false;
    ++change_id_;
    overwrite_ = false;
    new_file_ = false;
    line_separator_ = '\n';
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

std::size_t EditorModel::display_cursor_column() const {
    return lines_.empty() ? 0U : display_column(lines_[row_], column_, tab_width_) + over_column_;
}

void EditorModel::set_behavior(EditorBehavior behavior) {
    if (behavior.wrap_length == 0U || behavior.wrap_length > 4096U) {
        throw std::invalid_argument("word wrap length must be 1 through 4096");
    }
    behavior_ = behavior;
    behavior_configured_ = true;
    if (!behavior_.cursor_beyond_eol) over_column_ = 0;
    break_undo_group();
}

void EditorModel::break_undo_group() noexcept {
    if (edit_depth_ == 0U) ++undo_epoch_;
}

void EditorModel::prepare_navigation() {
    break_undo_group();
    if (!behavior_.persistent_selection && !marking_) clear_selection();
}

bool EditorModel::in_indentation() const {
    return !lines_.empty() && std::all_of(lines_[row_].begin(),
        lines_[row_].begin() + static_cast<std::ptrdiff_t>(column_),
        [](const char character) { return character == ' ' || character == '\t'; });
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
    const auto begin = selection_begin_display_.value_or(display_column(
        lines_[selection_begin_->row],
        selection_begin_->column,
        tab_width_
    ));
    const auto end = selection_end_display_.value_or(display_column(
        lines_[selection_end_->row],
        selection_end_->column,
        tab_width_
    ));
    if (column_selection_width_) {
        return {begin, begin + *column_selection_width_};
    }
    return std::minmax(begin, end);
}

std::optional<std::pair<std::size_t, std::size_t>> EditorModel::column_mark_columns() const {
    return column_selection_ && selection_begin_ && selection_end_
        ? std::optional<std::pair<std::size_t, std::size_t>>{column_selection_columns()} : std::nullopt;
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
            serialized << line_separator_;
        }
        serialized << lines_[index];
    }
    return std::move(serialized).str();
}

std::string EditorModel::encode_content(const std::string_view value) const {
    return encoding_.empty() || encoding_ == "UTF-8" ? std::string(value)
           : convert_encoding(value, "UTF-8", encoding_);
}

void EditorModel::update_storage_offsets() const {
    if (!storage_offsets_.empty() && storage_change_id_ == change_id_) return;
    std::vector<std::size_t> offsets;
    std::size_t bytes = 0;
    const auto separator = encode_content(std::string(1, line_separator_)).size();
    for (std::size_t row = 0; row < lines_.size(); ++row) {
        offsets.push_back(bytes);
        bytes += encode_content(lines_[row]).size();
        if (row + 1U < lines_.size()) bytes += separator;
    }
    offsets.push_back(bytes);
    storage_offsets_ = std::move(offsets);
    storage_change_id_ = change_id_;
}

std::size_t EditorModel::storage_byte_size() const {
    if (encoding_.empty() || encoding_ == "UTF-8") return byte_size();
    update_storage_offsets();
    return storage_offsets_.back();
}

std::size_t EditorModel::storage_cursor_offset() const {
    if (encoding_.empty() || encoding_ == "UTF-8") return cursor_offset();
    update_storage_offsets();
    return storage_offsets_[row_] + encode_content(std::string_view(lines_[row_]).substr(0, column_)).size();
}

std::size_t EditorModel::storage_selection_size() const {
    if (encoding_.empty() || encoding_ == "UTF-8") return selection_size();
    const auto selected = selected_text();
    return selected ? encode_content(*selected).size() : 0U;
}

void EditorModel::validate_search_options(const EditorSearchOptions& options) const {
    if (!encoding_.empty() && encoding_ != "UTF-8" && options.mode == EditorSearchMode::hexadecimal) {
        throw std::runtime_error("select No translation before hexadecimal search in a non-UTF-8 buffer");
    }
}

void EditorModel::set_encoding(std::string codepage) {
    if (codepage == encoding_) return;
    if (dirty_ || !undo_.empty() || !redo_.empty() || pending_undo_) {
        throw std::runtime_error("reopen the saved file before changing its codepage; the current undo history is retained");
    }
    if (!codepage.empty() &&
        std::ranges::find(supported_codepages(), codepage, &Codepage::id) == supported_codepages().end()) {
        throw std::invalid_argument("unsupported editor codepage");
    }
    const auto original = content();
    const auto decode = [&](const std::string& value) {
        return codepage.empty() ? value : convert_encoding(value, codepage, "UTF-8");
    };
    const auto decoded = decode(encode_content(original));
    const auto cursor = decode(encode_content(std::string_view(original).substr(0, cursor_offset()))).size();
    const auto mark_offset = [&](const std::optional<EditorPosition>& position_value) -> std::optional<std::size_t> {
        if (!position_value) return std::nullopt;
        return decode(encode_content(std::string_view(original).substr(0, offset(*position_value)))).size();
    };
    const auto begin = mark_offset(selection_begin_);
    const auto end = mark_offset(selection_end_);
    if (decoded.find('\n') != std::string::npos) line_separator_ = '\n';
    else if (decoded.find('\r') != std::string::npos) line_separator_ = '\r';
    set_content(decoded);
    encoding_ = std::move(codepage);
    const auto destination = position(cursor);
    row_ = destination.row;
    column_ = destination.column;
    selection_begin_ = begin ? std::optional<EditorPosition>{position(*begin)} : std::nullopt;
    selection_end_ = end ? std::optional<EditorPosition>{position(*end)} : std::nullopt;
    preferred_display_column_.reset();
    search_match_.reset();
    search_resume_.reset();
    storage_offsets_.clear();
    ++change_id_;
}

std::string EditorModel::search_content() const {
    auto value = content();
    if (line_separator_ == '\r') {
        std::replace(value.begin(), value.end(), '\r', '\n');
    }
    return value;
}

std::string EditorModel::normalize_insert(const std::string_view value) const {
    if (line_separator_ != '\r') {
        return std::string(value);
    }
    std::string result;
    result.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] == '\r' && index + 1U < value.size() && value[index + 1U] == '\n') {
            ++index;
        }
        result += value[index] == '\n' ? '\r' : value[index];
    }
    return result;
}

void EditorModel::set_content(const std::string_view content_value) {
    auto previous = bookmarks_.empty() ? std::vector<std::string>{} : std::move(lines_);
    lines_.clear();
    std::size_t begin = 0;
    while (begin < content_value.size()) {
        const auto end = content_value.find(line_separator_, begin);
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
    if (!previous.empty()) {
        std::size_t prefix = 0;
        while (prefix < previous.size() && prefix < lines_.size() &&
               previous[prefix] == lines_[prefix]) ++prefix;
        std::size_t suffix = 0;
        while (suffix < previous.size() - prefix && suffix < lines_.size() - prefix &&
               previous[previous.size() - suffix - 1U] == lines_[lines_.size() - suffix - 1U]) ++suffix;
        std::set<std::size_t> moved;
        for (const auto row : bookmarks_) {
            if (row < prefix) {
                moved.insert(row);
            } else if (row >= previous.size() - suffix) {
                moved.insert(lines_.size() - (previous.size() - row));
            } else if (prefix < lines_.size() - suffix) {
                moved.insert(std::min(row, lines_.size() - suffix - 1U));
            }
        }
        bookmarks_ = std::move(moved);
    }
}

void EditorModel::replace_text(
    const EditorPosition begin, const EditorPosition end, const std::string_view value
) {
    edit_text(begin, end, value);
}

void EditorModel::replace_block(const std::string_view value) {
    const auto selected = selection();
    if (!selected) throw std::runtime_error("no marked block");
    if (!column_selection_) {
        edit_text(selected->first, selected->second, value);
        return;
    }
    const auto normalized = normalize_insert(value);
    static_cast<void>(encode_content(normalized));
    std::vector<std::string> block;
    std::size_t begin = 0;
    while (begin < normalized.size()) {
        const auto end = normalized.find(line_separator_, begin);
        block.push_back(normalized.substr(begin, end == std::string::npos ? std::string::npos : end - begin));
        if (end == std::string::npos) break;
        begin = end + 1U;
    }
    const auto columns = column_selection_columns();
    const auto left = columns.first;
    const auto right = columns.second;
    record_undo();
    for (auto row = selected->first.row; row <= selected->second.row; ++row) {
        auto& line = lines_[row];
        const auto start = byte_column(line, left, tab_width_);
        const auto end = byte_column(line, right, tab_width_);
        line.erase(start, end - start);
    }
    while (lines_.size() < selected->first.row + block.size()) lines_.emplace_back();
    std::size_t width = 0;
    for (std::size_t index = 0; index < block.size(); ++index) {
        auto& line = lines_[selected->first.row + index];
        const auto available = display_column(line, line.size(), tab_width_);
        if (available < left) line.append(left - available, ' ');
        const auto target = prepare_column_insertion(line, left, tab_width_);
        line.insert(target, block[index]);
        width = std::max(width, display_column(line, target + block[index].size(), tab_width_) - left);
    }
    row_ = selected->first.row;
    column_ = byte_column(lines_[row_], left, tab_width_);
    preferred_display_column_ = left;
    if (block.empty()) clear_selection();
    else mark_column_block(row_, block.size(), left, width);
    search_match_.reset();
    search_resume_.reset();
    dirty_ = true;
    ++change_id_;
}

std::string EditorModel::text_range(const EditorPosition begin, const EditorPosition end) const {
    const auto first = offset(begin);
    const auto last = offset(end);
    if (last < first) throw std::invalid_argument("text range is reversed");
    return content().substr(first, last - first);
}

void EditorModel::set_text(const std::string_view value) {
    const auto normalized = normalize_insert(value);
    static_cast<void>(encode_content(normalized));
    record_undo();
    set_content(normalized);
    row_ = std::min(row_, lines_.size() - 1U);
    column_ = std::min(column_, lines_[row_].size());
    preferred_display_column_.reset();
    clear_selection();
    dirty_ = true;
    ++change_id_;
}

void EditorModel::toggle_bookmark() {
    if (bookmarks_.contains(row_)) bookmarks_.erase(row_);
    else bookmarks_.insert(row_);
}

bool EditorModel::jump_bookmark(const int direction) {
    if (bookmarks_.empty()) return false;
    if (direction >= 0) {
        const auto next = bookmarks_.upper_bound(row_);
        goto_line((next == bookmarks_.end() ? *bookmarks_.begin() : *next) + 1U);
    } else {
        auto previous = bookmarks_.lower_bound(row_);
        if (previous == bookmarks_.begin()) previous = bookmarks_.end();
        goto_line(*--previous + 1U);
    }
    return true;
}

void EditorModel::clear_bookmarks() noexcept { bookmarks_.clear(); }

void EditorModel::set_bookmarks(std::set<std::size_t> rows) {
    if (!rows.empty() && *rows.rbegin() >= lines_.size()) {
        throw std::out_of_range("bookmark is outside the document");
    }
    bookmarks_ = std::move(rows);
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
    if (lines_.empty()) throw std::runtime_error("editor is not open");
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
        if (column_selection_) selection_end_display_ = display_cursor_column();
    }
}

void EditorModel::replace_range(
    const EditorPosition begin,
    const EditorPosition end,
    const std::string_view replacement
) {
    edit_text(begin, end, replacement);
}

void EditorModel::move(const int rows, const int columns) {
    if (lines_.empty()) {
        return;
    }
    prepare_navigation();
    if (rows != 0 && !preferred_display_column_) {
        preferred_display_column_ =
            display_cursor_column();
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
    if (rows != 0 && behavior_.cursor_beyond_eol && preferred_display_column_) {
        const auto width = display_column(lines_[row_], lines_[row_].size(), tab_width_);
        over_column_ = *preferred_display_column_ > width ? *preferred_display_column_ - width : 0U;
    } else if (rows != 0) over_column_ = 0;
    while (column_ > 0U &&
           column_ < lines_[row_].size() &&
           continuation(
               static_cast<unsigned char>(lines_[row_][column_])
           )) {
        --column_;
    }
    if (columns < 0) {
        for (int count = 0; count > columns; --count) {
            if (over_column_ > 0U) {
                --over_column_;
            } else if (columns == -1 && behavior_.fake_half_tabs && in_indentation() &&
                       column_ >= std::max<std::size_t>(1U, tab_width_ / 2U) &&
                       std::string_view(lines_[row_]).substr(column_ - std::max<std::size_t>(1U, tab_width_ / 2U),
                           std::max<std::size_t>(1U, tab_width_ / 2U)).find_first_not_of(' ') == std::string_view::npos) {
                column_ -= std::max<std::size_t>(1U, tab_width_ / 2U);
            } else if (column_ == 0U && row_ > 0U) {
                --row_;
                column_ = lines_[row_].size();
            } else {
                column_ =
                    previous_character(lines_[row_], column_);
            }
        }
    } else {
        for (int count = 0; count < columns; ++count) {
            if (behavior_.cursor_beyond_eol && column_ == lines_[row_].size()) {
                ++over_column_;
            } else if (columns == 1 && behavior_.fake_half_tabs && in_indentation() &&
                       std::string_view(lines_[row_]).substr(column_, std::max<std::size_t>(1U, tab_width_ / 2U)) ==
                           std::string(std::max<std::size_t>(1U, tab_width_ / 2U), ' ')) {
                column_ += std::max<std::size_t>(1U, tab_width_ / 2U);
            } else if (column_ == lines_[row_].size() &&
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
            display_cursor_column();
    }
    update_mark();
}

void EditorModel::move_word(const int direction) {
    if (lines_.empty() || direction == 0) {
        return;
    }
    prepare_navigation();
    over_column_ = 0;
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
    prepare_navigation();
    over_column_ = 0;
    column_ = 0;
    preferred_display_column_ =
        display_column(lines_[row_], column_, tab_width_);
    update_mark();
}

void EditorModel::end() {
    prepare_navigation();
    over_column_ = 0;
    column_ = lines_[row_].size();
    preferred_display_column_ =
        display_column(lines_[row_], column_, tab_width_);
    update_mark();
}

void EditorModel::goto_line(const std::size_t line) {
    if (lines_.empty()) {
        return;
    }
    prepare_navigation();
    if (!preferred_display_column_) {
        preferred_display_column_ =
            display_cursor_column();
    }
    row_ = std::min(
        line == 0U ? 0U : line - 1U,
        lines_.size() - 1U
    );
    column_ = byte_column(
        lines_[row_], *preferred_display_column_, tab_width_
    );
    const auto width = display_column(lines_[row_], lines_[row_].size(), tab_width_);
    over_column_ = behavior_.cursor_beyond_eol && *preferred_display_column_ > width
                       ? *preferred_display_column_ - width : 0U;
    update_mark();
}

void EditorModel::set_position(EditorPosition destination) {
    if (lines_.empty()) {
        return;
    }
    prepare_navigation();
    over_column_ = 0;
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

void EditorModel::set_display_position(const std::size_t row, const std::size_t column) {
    if (lines_.empty()) return;
    prepare_navigation();
    row_ = std::min(row, lines_.size() - 1U);
    column_ = byte_column(lines_[row_], column, tab_width_);
    const auto width = display_column(lines_[row_], lines_[row_].size(), tab_width_);
    over_column_ = behavior_.cursor_beyond_eol && column > width ? column - width : 0U;
    preferred_display_column_ = column;
    update_mark();
}

void EditorModel::toggle_mark() {
    break_undo_group();
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
    break_undo_group();
    const EditorPosition cursor{
        .row = row_,
        .column = column_,
    };
    if (marking_ && column_selection_) {
        selection_end_ = cursor;
        selection_end_display_ = display_cursor_column();
        marking_ = false;
        return;
    }
    clear_selection();
    selection_begin_ = cursor;
    selection_end_ = cursor;
    marking_ = true;
    column_selection_ = true;
    selection_begin_display_ = display_cursor_column();
    selection_end_display_ = selection_begin_display_;
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
    selection_begin_display_ = display_cursor_column();
    selection_end_display_ = selection_begin_display_;
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
        update_mark();
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
    selection_begin_display_.reset();
    selection_end_display_.reset();
}

void EditorModel::select_all() {
    break_undo_group();
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
    insert_text(text, overwrite_, false, true);
}

void EditorModel::paste(const std::string_view text) {
    insert_text(text, false);
}

void EditorModel::paste_bytes(const std::string_view bytes) {
    paste(encoding_.empty() ? std::string(bytes) : convert_encoding(bytes, encoding_, "UTF-8"));
}

void EditorModel::paste_column(const std::string_view value) {
    const auto normalized = normalize_insert(value);
    static_cast<void>(encode_content(normalized));
    const std::string_view text = normalized;
    if (text.empty()) {
        return;
    }
    std::vector<std::string> block;
    std::size_t begin = 0;
    while (begin <= text.size()) {
        const auto end = text.find(line_separator_, begin);
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
    const auto target_display = display_cursor_column();
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
    over_column_ = 0;
    clear_selection();
    dirty_ = true;
    ++change_id_;
}

void EditorModel::insert_text(
    const std::string_view value,
    const bool overwrite,
    const bool include_selection_end,
    const bool typed
) {
    auto normalized = normalize_insert(value);
    if (over_column_ > 0U) normalized.insert(0, over_column_, ' ');
    const std::string_view text = normalized;
    if (text.empty()) {
        return;
    }
    static_cast<void>(encode_content(text));
    EditGroup edit(*this, typed ? EditKind::insertion : EditKind::compound);
    if (!behavior_.persistent_selection && !marking_ && selection()) {
        static_cast<void>(delete_selection());
    }
    over_column_ = 0;
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
    if (overwrite && text.find(line_separator_) == std::string_view::npos) {
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
    if (typed && text.find(line_separator_) == std::string_view::npos) wrap_typed_text();
}

void EditorModel::wrap_typed_text() {
    if (behavior_.wrap == EditorWrapMode::none) return;
    if (behavior_.wrap == EditorWrapMode::dynamic) {
        reflow_paragraph();
        return;
    }
    while (display_cursor_column() > behavior_.wrap_length) {
        const auto line = lines_[row_];
        const auto indent_end = line.find_first_not_of(" \t");
        const auto indent = indent_end == std::string::npos ? line.size() : indent_end;
        const auto cut = byte_column(line, behavior_.wrap_length, tab_width_);
        auto split = std::min(cut, line.size());
        while (split > indent && (split == line.size() || (line[split] != ' ' && line[split] != '\t'))) {
            split = previous_character(line, split);
        }
        if (split <= indent) break;
        auto begin = split, end = split;
        while (begin > indent && (line[begin - 1U] == ' ' || line[begin - 1U] == '\t')) --begin;
        while (end < line.size() && (line[end] == ' ' || line[end] == '\t')) ++end;
        if (begin == 0U || end <= begin) break;
        const auto old_row = row_, old_column = column_;
        const auto ending = line_separator_ == '\r' ? std::string("\r")
                           : line.ends_with('\r') ? std::string("\r\n") : std::string("\n");
        edit_text({old_row, begin}, {old_row, end}, ending + line.substr(0, indent));
        const auto destination = old_column >= end
            ? EditorPosition{old_row + 1U, indent + old_column - end}
            : old_column <= begin ? EditorPosition{old_row, old_column} : EditorPosition{old_row + 1U, indent};
        row_ = destination.row;
        column_ = std::min(destination.column, lines_[row_].size());
        over_column_ = 0;
        preferred_display_column_ = display_cursor_column();
        update_mark();
    }
}

void EditorModel::reflow_paragraph() {
    const auto blank = [](const std::string& line) {
        return std::ranges::all_of(line, [](const unsigned char character) { return std::isspace(character) != 0; });
    };
    if (blank(lines_[row_])) return;
    auto first = row_, last = row_;
    while (first > 0U && !blank(lines_[first - 1U])) --first;
    while (last + 1U < lines_.size() && !blank(lines_[last + 1U])) ++last;
    const EditorPosition begin{first, 0}, end{last, lines_[last].size()};
    const auto original = text_range(begin, end);
    const auto indent_end = lines_[first].find_first_not_of(" \t");
    const auto indent = lines_[first].substr(0, indent_end == std::string::npos ? lines_[first].size() : indent_end);
    const auto indent_width = display_column(indent, indent.size(), tab_width_);
    if (indent_width >= behavior_.wrap_length) return;
    const auto cursor = offset({row_, column_}) - offset(begin);
    const bool after_space = cursor > 0U && cursor <= original.size() &&
                             std::isspace(static_cast<unsigned char>(original[cursor - 1U])) != 0;
    std::size_t wanted = 0;
    for (std::size_t index = 0; index < std::min(cursor, original.size()); ++index) {
        if (std::isspace(static_cast<unsigned char>(original[index])) == 0) ++wanted;
    }
    std::vector<std::string_view> words;
    for (std::size_t index = 0; index < original.size();) {
        while (index < original.size() && std::isspace(static_cast<unsigned char>(original[index])) != 0) ++index;
        const auto start = index;
        while (index < original.size() && std::isspace(static_cast<unsigned char>(original[index])) == 0) ++index;
        if (index > start) words.emplace_back(std::string_view(original).substr(start, index - start));
    }
    const bool crlf = line_separator_ == '\n' &&
        std::ranges::any_of(lines_.begin() + static_cast<std::ptrdiff_t>(first),
                           lines_.begin() + static_cast<std::ptrdiff_t>(last + 1U),
                           [](const std::string& line) { return line.ends_with('\r'); });
    const auto ending = line_separator_ == '\r' ? std::string("\r") : crlf ? std::string("\r\n") : std::string("\n");
    std::string formatted = indent;
    std::size_t width = indent_width, seen = 0, mapped = indent.size();
    for (std::size_t index = 0; index < words.size(); ++index) {
        const auto word = words[index];
        const auto word_width = display_column(word, word.size(), tab_width_);
        if (index != 0U) {
            if (width + 1U + word_width > behavior_.wrap_length) {
                formatted += ending + indent;
                width = indent_width;
            } else {
                formatted += ' ';
                ++width;
            }
            if (after_space && seen == wanted) mapped = formatted.size();
        }
        if (wanted >= seen && wanted <= seen + word.size() && !(after_space && wanted == seen && index != 0U)) {
            mapped = formatted.size() + wanted - seen;
        }
        formatted += word;
        seen += word.size();
        width += word_width;
    }
    const auto tail = original.size() - (crlf && original.ends_with('\r') ? 1U : 0U);
    const bool trailing = tail > 0U && (original[tail - 1U] == ' ' || original[tail - 1U] == '\t');
    if (trailing) {
        formatted += ' ';
        if (after_space && wanted == seen) mapped = formatted.size();
    }
    if (crlf && lines_[last].ends_with('\r')) formatted += '\r';
    if (first == row_ && cursor <= indent.size()) mapped = std::min(cursor, indent.size());
    if (formatted == original) return;
    const auto base = offset(begin);
    edit_text(begin, end, formatted);
    const auto destination = position(base + std::min(mapped, formatted.size()));
    row_ = destination.row;
    column_ = destination.column;
    over_column_ = 0;
    preferred_display_column_ = display_cursor_column();
    update_mark();
}

void EditorModel::newline(const bool auto_indent) {
    if (!auto_indent) {
        insert_text("\n", false);
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
    paste(text);
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
    const std::string_view value
) {
    const auto normalized = normalize_insert(value);
    const std::string_view replacement = normalized;
    static_cast<void>(encode_content(replacement));
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
    over_column_ = 0;
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

void EditorModel::tab() {
    const auto spacing = behavior_.fake_half_tabs ? std::max<std::size_t>(1U, tab_width_ / 2U) : tab_width_;
    if (!behavior_.fake_half_tabs && !behavior_.fill_tabs_with_spaces) paste("\t");
    else tab(spacing, behavior_.fill_tabs_with_spaces);
}

void EditorModel::backspace(const std::size_t indent_width) {
    if (!behavior_.persistent_selection && !marking_ && selection()) {
        static_cast<void>(delete_selection());
        return;
    }
    if (over_column_ > 0U) {
        --over_column_;
        preferred_display_column_ = display_cursor_column();
        break_undo_group();
        return;
    }
    if (column_ == 0U && row_ == 0U) {
        return;
    }
    EditGroup edit(*this, EditKind::deletion);
    if (behavior_.backspace_through_tabs && in_indentation() && column_ > 0U) {
        erase_text({row_, 0}, {row_, column_});
        return;
    }
    if (column_ > 0U) {
        const auto spacing = indent_width == 0U ? (behavior_configured_ && !behavior_.fake_half_tabs
                                                      ? 1U : std::max<std::size_t>(1U, tab_width_ / 2U))
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
    if (!behavior_.persistent_selection && !marking_ && selection()) {
        static_cast<void>(delete_selection());
        return;
    }
    if (over_column_ > 0U) {
        const auto padding = std::string(over_column_, ' ');
        const auto begin = EditorPosition{row_, column_};
        const auto end = row_ + 1U < lines_.size() ? EditorPosition{row_ + 1U, 0} : begin;
        over_column_ = 0;
        edit_text(begin, end, padding);
        return;
    }
    if (column_ >= lines_[row_].size() &&
        row_ + 1U >= lines_.size()) {
        return;
    }
    EditGroup edit(*this, EditKind::deletion);
    const auto half = std::max<std::size_t>(1U, tab_width_ / 2U);
    if (behavior_.fake_half_tabs && in_indentation() &&
        std::string_view(lines_[row_]).substr(column_, half) == std::string(half, ' ')) {
        erase_text({row_, column_}, {row_, column_ + half});
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
    selection_begin_display_ = left;
    selection_end_display_ = left + width;
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
    const std::string_view value,
    std::optional<std::pair<
        EditorPosition,
        EditorPosition
    >>* scope
) {
    const auto normalized = normalize_insert(value);
    const std::string_view replacement = normalized;
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
    if (edit_begin == edit_end) {
        const auto current = content();
        const auto cursor = offset({row_, column_});
        search_resume_ = search_match_options_.backwards
                             ? edit_begin
                             : cursor < current.size() ? next_character(current, cursor) : current.size() + 1U;
        search_match_change_id_ = change_id_;
    }
    return true;
}

std::optional<std::string> EditorModel::selected_bytes() const {
    const auto selected = selected_text();
    return selected ? std::optional<std::string>{encode_content(*selected)} : std::nullopt;
}

EditorModel::EditGroup::EditGroup(EditorModel& editor, const EditKind kind) : editor_(editor) {
    editor_.record_undo(kind);
    ++editor_.edit_depth_;
}

EditorModel::EditGroup::~EditGroup() {
    --editor_.edit_depth_;
}

void EditorModel::record_undo(const EditKind kind) {
    if (edit_depth_ > 0U && pending_undo_) return;
    finalize_undo();
    pending_undo_ = snapshot();
    pending_content_ = content();
    pending_kind_ = kind;
    pending_epoch_ = undo_epoch_;
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
        pending_kind_, pending_epoch_,
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
        .over_column = over_column_,
        .selection_begin = selection_begin_,
        .selection_end = selection_end_,
        .marking = marking_,
        .column_selection = column_selection_,
        .column_selection_width = column_selection_width_,
        .selection_begin_display = selection_begin_display_,
        .selection_end_display = selection_end_display_,
        .byte_size = byte_size(),
        .line_separator = line_separator_,
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
    line_separator_ = snapshot.line_separator;
    row_ = snapshot.row;
    column_ = snapshot.column;
    over_column_ = behavior_.cursor_beyond_eol ? snapshot.over_column : 0U;
    preferred_display_column_ =
        display_cursor_column();
    selection_begin_ = snapshot.selection_begin;
    selection_end_ = snapshot.selection_end;
    marking_ = snapshot.marking;
    column_selection_ = snapshot.column_selection;
    column_selection_width_ = snapshot.column_selection_width;
    selection_begin_display_ = snapshot.selection_begin_display;
    selection_end_display_ = snapshot.selection_end_display;
    dirty_ = true;
    ++change_id_;
}

bool EditorModel::undo() {
    break_undo_group();
    finalize_undo();
    if (undo_.empty()) {
        return false;
    }
    const auto kind = undo_.back().kind;
    const auto epoch = undo_.back().epoch;
    do {
        auto previous = std::move(undo_.back());
        undo_bytes_ -= previous.bytes();
        undo_.pop_back();
        previous.after = snapshot();
        auto value = content();
        if (value.substr(previous.offset, previous.inserted.size()) != previous.inserted) {
            throw std::runtime_error("undo history does not match the document");
        }
        value.replace(previous.offset, previous.inserted.size(), previous.removed);
        line_separator_ = previous.before.line_separator;
        set_content(value);
        restore(previous.before);
        redo_bytes_ += previous.bytes();
        redo_.push_back(std::move(previous));
    } while (behavior_.group_undo && kind != EditKind::compound && !undo_.empty() &&
             undo_.back().kind == kind && undo_.back().epoch == epoch);
    return true;
}

bool EditorModel::redo() {
    break_undo_group();
    finalize_undo();
    if (redo_.empty()) {
        return false;
    }
    const auto kind = redo_.back().kind;
    const auto epoch = redo_.back().epoch;
    do {
        auto next = std::move(redo_.back());
        redo_bytes_ -= next.bytes();
        redo_.pop_back();
        auto value = content();
        if (value.substr(next.offset, next.removed.size()) != next.removed) {
            throw std::runtime_error("redo history does not match the document");
        }
        value.replace(next.offset, next.removed.size(), next.inserted);
        line_separator_ = next.after.line_separator;
        set_content(value);
        restore(next.after);
        undo_bytes_ += next.bytes();
        undo_.push_back(std::move(next));
    } while (behavior_.group_undo && kind != EditKind::compound && !redo_.empty() &&
             redo_.back().kind == kind && redo_.back().epoch == epoch);
    trim_undo();
    return true;
}

void EditorModel::save(
    const bool force,
    const Cancellation& cancellation
) {
    break_undo_group();
    if (!provider_) {
        throw std::runtime_error("editor is not open");
    }
    if (path_.empty()) {
        throw std::runtime_error("an unnamed buffer requires Save as");
    }
    if (!new_file_ && save_mode_ == EditorSaveMode::in_place && hardlink_count() == 1U) {
        save_preserving_links(force, cancellation);
        return;
    }
    if (!new_file_) {
        if (const auto* local = dynamic_cast<const LocalProvider*>(provider_.get())) {
            local->ensure_writable(storage_path_);
        }
        if (save_mode_ == EditorSaveMode::backup) {
            backup_target(storage_path_, path_, force ? std::nullopt : std::optional<std::string>{revision_},
                          cancellation);
        }
    }
    auto serialized = encode_content(content());
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

void EditorModel::set_backup_extension(std::string value) {
    if (value.find('\0') != std::string::npos) throw std::invalid_argument("backup extension contains NUL");
    backup_extension_ = std::move(value);
}

void EditorModel::backup_target(
    const std::string& storage, const std::string& path,
    const std::optional<std::string>& expected,
    const Cancellation& cancellation
) {
    const auto original = provider_->stat_follow(storage, cancellation);
    if (expected && original.revision != *expected) {
        throw std::runtime_error("destination changed since it was opened: " + path);
    }
    if (original.size > 16U * 1024U * 1024U) throw std::runtime_error("backup input exceeds the editor size limit");
    if (backup_extension_.empty()) throw std::runtime_error("backup extension is empty");
    const auto backup = path + backup_extension_;
    const bool exists = provider_->exists(backup);
    if (exists) {
        const auto entry = provider_->stat(backup, cancellation);
        if (entry.kind != EntryKind::file) throw std::runtime_error("backup destination is not a regular file");
        if (const auto* local = dynamic_cast<const LocalProvider*>(provider_.get());
            local != nullptr && local->same_file(storage, *local, backup)) {
            throw std::runtime_error("backup destination aliases the original file");
        }
    }
    OperationResult result;
    OperationEngine{}.copy_exact(*provider_, original, *provider_, backup,
        {.overwrite=exists, .preserve_permissions=true, .preserve_symlinks=true, .collision={}},
        cancellation, {}, &result);
    if (result.completed_sources.size() != 1U ||
        provider_->stat_follow(storage, cancellation).revision != original.revision) {
        throw std::runtime_error("file changed or backup was not completed");
    }
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
    local->ensure_writable(storage_path_);
    if (save_mode_ == EditorSaveMode::backup) {
        backup_target(storage_path_, path_, force ? std::nullopt : std::optional<std::string>{revision_},
                      cancellation);
    }
    const auto value = encode_content(content());
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
    if (path.empty()) throw std::invalid_argument("file name is empty");
    if (!path_.empty() && std::filesystem::path(path).is_relative()) {
        path = provider_->join(provider_->parent(path_), path);
    }
    path = provider_->normalize(path);
    const bool same_path = !path_.empty() && path == provider_->normalize(path_);
    if (same_path && ending == EditorLineEnding::unchanged) {
        if (preserve_links) {
            save_preserving_links(force, cancellation);
        } else {
            save(force, cancellation);
        }
        return;
    }
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
    if (target_exists && save_mode_ == EditorSaveMode::backup) {
        backup_target(storage, path, expected, cancellation);
    }
    const auto encoded = encode_content(serialized);
    std::istringstream input(encoded);
    const auto* local_target = dynamic_cast<const LocalProvider*>(provider_.get());
    const bool in_place = target_exists && save_mode_ == EditorSaveMode::in_place &&
                          local_target != nullptr && local_target->hardlink_count(storage) == 1U;
    if ((preserve_links || in_place) && target_exists) {
        auto* local = dynamic_cast<LocalProvider*>(provider_.get());
        if (local == nullptr) {
            throw std::runtime_error("linked saves are unavailable for this provider");
        }
        local->write_in_place(storage, input, encoded.size(), expected, cancellation);
    } else {
        provider_->write(
            storage, input, encoded.size(),
            target_exists && (force || same_path), expected, cancellation, {}
        );
    }
    if (ending != EditorLineEnding::unchanged) {
        record_undo();
        line_separator_ = ending == EditorLineEnding::cr ? '\r' : '\n';
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
    break_undo_group();
    if (needle.empty()) {
        return false;
    }
    validate_search_options(options);
    const auto serialized = search_content();
    auto start = offset({
        .row = row_,
        .column = column_,
    });
    if (const auto selected = search_selection();
        selected && needle == search_match_pattern_ &&
        options.mode == search_match_options_.mode &&
        options.case_sensitive == search_match_options_.case_sensitive &&
        options.backwards == search_match_options_.backwards &&
        options.whole_words == search_match_options_.whole_words &&
        EditorPosition{row_, column_} == selected->first) {
        if (options.backwards) {
            start = search_match_->begin;
        } else {
            start =
                overlap || search_match_->begin == search_match_->end
                    ? next_character(serialized, start)
                    : search_match_->end;
            if (search_match_->begin == search_match_->end &&
                search_match_->end == serialized.size()) {
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
    finish_mark();
    search_match_ = found;
    search_match_pattern_ = needle;
    search_match_options_ = options;
    search_match_change_id_ = change_id_;
    const auto destination = position(found->begin);
    row_ = destination.row;
    column_ = destination.column;
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
    validate_search_options(options);
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
        search_content(),
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
        replaced += normalize_insert(EditorSearch::replacement(
            original,
            match,
            needle,
            replacement_value,
            options
        ));
        final_cursor = replaced.size();
        cursor = match.end;
    }
    replaced.append(
        original,
        cursor,
        range_end - cursor
    );
    replaced.append(original, range_end, std::string::npos);
    static_cast<void>(encode_content(replaced));
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
    validate_search_options(options);
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
    if (search_match_ && search_match_change_id_ == change_id_ &&
        EditorPosition{row_, column_} == position(search_match_->begin)) {
        return search_match_;
    }
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

std::optional<std::pair<EditorPosition, EditorPosition>>
EditorModel::search_selection() const {
    if (!search_match_ || search_match_change_id_ != change_id_) {
        return std::nullopt;
    }
    return std::pair{position(search_match_->begin), position(search_match_->end)};
}

std::size_t EditorModel::count_matches(
    const std::string_view needle,
    const EditorSearchOptions& options,
    const std::optional<std::pair<
        EditorPosition,
        EditorPosition
    >> scope
) const {
    validate_search_options(options);
    const auto serialized = search_content();
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
    return replace_selection(replacement);
}

}  // namespace mcpp
