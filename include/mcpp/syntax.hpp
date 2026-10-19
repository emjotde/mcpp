#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace mcpp {

enum class SyntaxStyle {
    normal,
    keyword,
    type,
    string_literal,
    comment,
    number,
    preprocessor,
};

struct SyntaxSpan {
    std::size_t begin{0};
    std::size_t end{0};
    SyntaxStyle style{SyntaxStyle::normal};
};

class SyntaxHighlighter {
public:
    [[nodiscard]] static const std::vector<std::string>& languages();
    [[nodiscard]] std::vector<std::vector<SyntaxSpan>> highlight(
        const std::filesystem::path& path,
        const std::vector<std::string>& lines,
        std::string_view language = "auto"
    ) const;
};

}  // namespace mcpp
