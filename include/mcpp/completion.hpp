#pragma once

#include "mcpp/entry.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mcpp {

enum class CompletionKind { path, command, variable, username, hostname };
enum class CompletionQuote { literal, unquoted, single, double_quote };

struct CompletionTarget {
    std::string prefix;
    std::string suffix;
    CompletionQuote quote{CompletionQuote::literal};
    CompletionQuote suffix_quote{CompletionQuote::literal};
    std::size_t suffix_slash_bytes{0};
    bool close_variable{false};
    bool quoted_word{false};
    bool command{false};
};

struct CompletionResult {
    CompletionTarget target;
    std::vector<std::string> matches;
    CompletionKind kind{CompletionKind::path};
    std::string warning;
};

struct CompletionEdit {
    std::string value;
    std::size_t cursor{0};
};

using CompletionPathSource =
    std::function<std::vector<Entry>(std::string_view directory)>;

[[nodiscard]] CompletionResult complete_input(
    std::string_view value,
    std::size_t cursor,
    std::optional<std::size_t> anchor,
    bool command_line,
    const CompletionPathSource& paths,
    std::string_view command_directory = {}
);

[[nodiscard]] CompletionEdit completion_edit(
    const CompletionTarget& target, std::string_view match
);

[[nodiscard]] std::string completion_common_prefix(
    const std::vector<std::string>& matches
);

[[nodiscard]] std::string_view completion_kind_name(CompletionKind kind);

}  // namespace mcpp
