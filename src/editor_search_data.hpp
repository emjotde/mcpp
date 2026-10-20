#pragma once

#include "mcpp/editor_search.hpp"

namespace mcpp::search_detail {

[[nodiscard]] bool word_character(std::string_view text);
[[nodiscard]] std::vector<EditorSearchMatch> casefolded_literals(
    std::string_view text, std::string_view pattern,
    std::size_t begin, std::size_t end
);

}  // namespace mcpp::search_detail
