#include "mcpp/command_line.hpp"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <stdexcept>
#include <utility>

namespace mcpp {

std::size_t parse_line_number(const std::string_view value) {
    if (value.empty()) throw std::invalid_argument("line numbers must be nonnegative integers");
    std::size_t result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
        throw std::invalid_argument("line numbers must be nonnegative integers");
    }
    return std::max(std::size_t{1}, result);
}

EditorFile parse_editor_file(std::string path) {
    if (path.empty()) throw std::invalid_argument("file name is empty");
    const auto end = path.size() - (path.ends_with(':') ? 1U : 0U);
    const auto separator = path.rfind(':', end == 0U ? 0U : end - 1U);
    if (separator == std::string::npos || separator == 0U || separator + 1U >= end) {
        return {std::move(path), 0};
    }
    const auto number = std::string_view(path).substr(separator + 1U, end - separator - 1U);
    if (!std::ranges::all_of(number, [](const char value) { return value >= '0' && value <= '9'; }) ||
        std::filesystem::exists(std::filesystem::symlink_status(path))) {
        return {std::move(path), 0};
    }
    const auto line = parse_line_number(number);
    path.resize(separator);
    return {std::move(path), line};
}

}  // namespace mcpp
