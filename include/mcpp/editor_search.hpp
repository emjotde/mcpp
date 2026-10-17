#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mcpp {

enum class EditorSearchMode {
    normal,
    regular_expression,
    wildcard,
    hexadecimal,
};

struct EditorSearchOptions {
    EditorSearchMode mode{EditorSearchMode::normal};
    bool case_sensitive{false};
    bool backwards{false};
    bool whole_words{false};
};

struct EditorSearchMatch {
    std::size_t begin{0};
    std::size_t end{0};
};

class EditorSearch {
public:
    [[nodiscard]] static std::optional<EditorSearchMatch> find(
        std::string_view text,
        std::string_view pattern,
        std::size_t start,
        std::size_t range_begin,
        std::size_t range_end,
        const EditorSearchOptions& options,
        bool wrap,
        bool overlap
    );

    [[nodiscard]] static std::vector<EditorSearchMatch> find_all(
        std::string_view text,
        std::string_view pattern,
        std::size_t range_begin,
        std::size_t range_end,
        const EditorSearchOptions& options
    );

    [[nodiscard]] static std::string replacement(
        std::string_view text,
        EditorSearchMatch match,
        std::string_view pattern,
        std::string_view replacement,
        const EditorSearchOptions& options
    );
};

}  // namespace mcpp
