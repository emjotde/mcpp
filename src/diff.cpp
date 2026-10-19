#include "mcpp/diff.hpp"
#include "mcpp/local_tools.hpp"

#include <algorithm>
#include <charconv>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace mcpp {
namespace {

std::pair<std::size_t, std::size_t> parse_range(const std::string_view text, std::size_t& index) {
    std::size_t begin = 0, count = 1;
    const auto parsed = std::from_chars(text.data() + index, text.data() + text.size(), begin);
    if (parsed.ec != std::errc{}) throw std::runtime_error("invalid diff hunk range");
    index = static_cast<std::size_t>(parsed.ptr - text.data());
    if (index < text.size() && text[index] == ',') {
        ++index;
        const auto length = std::from_chars(text.data() + index, text.data() + text.size(), count);
        if (length.ec != std::errc{}) throw std::runtime_error("invalid diff hunk count");
        index = static_cast<std::size_t>(length.ptr - text.data());
    }
    if (count != 0U && begin == 0U) throw std::runtime_error("invalid diff line number");
    return {count == 0U ? begin : begin - 1U, count};
}

EditorPosition line_position(const EditorModel& model, const std::size_t line) {
    if (line < model.lines().size()) return {line, 0};
    return {model.lines().size() - 1U, model.lines().back().size()};
}

std::string diff_input(const EditorModel& model) {
    auto input = model.text();
    if (model.line_separator() == '\r') std::replace(input.begin(), input.end(), '\r', '\n');
    return input;
}

}  // namespace

std::size_t DiffModel::line_count(const EditorModel& model) {
    if (model.lines().empty()) return 0;
    return model.lines().size() - (model.lines().back().empty() ? 1U : 0U);
}

void DiffModel::compare(const EditorModel& left, const EditorModel& right, const DiffOptions& options,
                        const Cancellation& cancellation) {
    if (!left.open() || !right.open()) throw std::runtime_error("file comparison requires two open files");
    if (options.quality > 2U) throw std::invalid_argument("invalid diff algorithm");
    TemporaryFile first(diff_input(left)), second(diff_input(right));
    std::vector<std::string> arguments{"diff", "-a", "-U0", "--label", "Left", "--label", "Right"};
    if (options.quality == 1U) arguments.push_back("--speed-large-files");
    if (options.quality == 2U) arguments.push_back("--minimal");
    if (options.ignore_case) arguments.push_back("-i");
    if (options.ignore_tab_expansion) arguments.push_back("-E");
    if (options.ignore_space_change) arguments.push_back("-b");
    if (options.ignore_all_space) arguments.push_back("-w");
    if (options.strip_trailing_cr) arguments.push_back("--strip-trailing-cr");
    arguments.insert(arguments.end(), {"--", first.path().string(), second.path().string()});
    const auto result = capture_command(arguments, "/", cancellation, 64U * 1024U * 1024U);
    if (result.status != 0 && result.status != 1) {
        throw std::runtime_error("diff exited " + std::to_string(result.status) + ": " + result.error);
    }
    std::vector<DiffHunk> hunks;
    std::istringstream output(result.output);
    std::string line;
    while (std::getline(output, line)) {
        if (!line.starts_with("@@ -")) continue;
        std::size_t index = 4;
        const auto first_range = parse_range(line, index);
        if (line.substr(index, 2) != " +") throw std::runtime_error("invalid diff hunk header");
        index += 2;
        const auto second_range = parse_range(line, index);
        if (line.substr(index, 3) != " @@") throw std::runtime_error("invalid diff hunk terminator");
        const auto first_count = line_count(left), second_count = line_count(right);
        if (first_range.first > first_count || first_range.second > first_count - first_range.first ||
            second_range.first > second_count || second_range.second > second_count - second_range.first) {
            throw std::runtime_error("diff hunk exceeds the compared files");
        }
        hunks.push_back({first_range.first, first_range.second, second_range.first, second_range.second});
    }
    std::vector<DiffRow> rows;
    std::size_t first_line = 0, second_line = 0;
    for (std::size_t hunk = 0; hunk < hunks.size(); ++hunk) {
        auto& change = hunks[hunk];
        if (change.left_begin < first_line || change.right_begin < second_line) {
            throw std::runtime_error("diff hunks are out of order");
        }
        while (first_line < change.left_begin || second_line < change.right_begin) {
            DiffRow row;
            if (first_line < change.left_begin) row.left = first_line++;
            if (second_line < change.right_begin) row.right = second_line++;
            rows.push_back(row);
        }
        change.display_row = rows.size();
        for (std::size_t offset = 0; offset < std::max(change.left_count, change.right_count); ++offset) {
            DiffRow row;
            if (offset < change.left_count) row.left = first_line++;
            if (offset < change.right_count) row.right = second_line++;
            row.hunk = hunk;
            rows.push_back(row);
        }
    }
    while (first_line < line_count(left) || second_line < line_count(right)) {
        DiffRow row;
        if (first_line < line_count(left)) row.left = first_line++;
        if (second_line < line_count(right)) row.right = second_line++;
        rows.push_back(row);
    }
    hunks_ = std::move(hunks);
    rows_ = std::move(rows);
}

void DiffModel::merge(const std::size_t index, const bool left_to_right, EditorModel& left, EditorModel& right) const {
    if (index >= hunks_.size()) throw std::out_of_range("no diff hunk is selected");
    const auto& hunk = hunks_[index];
    const auto& source = left_to_right ? left : right;
    auto& target = left_to_right ? right : left;
    const auto source_begin = left_to_right ? hunk.left_begin : hunk.right_begin;
    const auto source_count = left_to_right ? hunk.left_count : hunk.right_count;
    const auto target_begin = left_to_right ? hunk.right_begin : hunk.left_begin;
    const auto target_count = left_to_right ? hunk.right_count : hunk.left_count;
    const auto data = source.text_range(line_position(source, source_begin), line_position(source, source_begin + source_count));
    target.replace_text(line_position(target, target_begin), line_position(target, target_begin + target_count), data);
}

}  // namespace mcpp
