#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace mcpp {

struct EditorFile {
    std::string path;
    std::size_t line{0};
};

[[nodiscard]] std::size_t parse_line_number(std::string_view value);
[[nodiscard]] EditorFile parse_editor_file(std::string path);

}  // namespace mcpp
