#pragma once

#include "mcpp/cancellation.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace mcpp {

struct SpellingIssue {
    std::size_t offset{0};
    std::string word;
    std::vector<std::string> suggestions;
};

void validate_spelling_language(std::string_view language);
[[nodiscard]] std::vector<std::string> spelling_languages(
    const std::filesystem::path& directory, const Cancellation& cancellation = {}
);
[[nodiscard]] std::vector<SpellingIssue> check_spelling(
    std::string_view text, const std::string& language,
    const std::filesystem::path& directory, const Cancellation& cancellation = {}
);
void add_spelling_word(
    std::string_view word, const std::string& language,
    const std::filesystem::path& directory, const Cancellation& cancellation = {}
);

}  // namespace mcpp
