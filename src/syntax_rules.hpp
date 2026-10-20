#pragma once

#include "mcpp/syntax.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mcpp::detail {

struct SyntaxRuleSet;

struct SyntaxRegistryEntry {
    SyntaxDefinition definition;
    std::shared_ptr<const SyntaxRuleSet> rules;
};

class SyntaxRegistry {
public:
    static std::shared_ptr<const SyntaxRegistry> load(
        const std::filesystem::path& registry,
        const std::filesystem::path& definitions
    );

    [[nodiscard]] const SyntaxRegistryEntry* select(
        const std::filesystem::path& path,
        std::string_view first_line,
        std::string_view language
    ) const;
    [[nodiscard]] std::vector<SyntaxDefinition> definitions() const;

private:
    std::vector<SyntaxRegistryEntry> entries_;
};

[[nodiscard]] std::vector<std::vector<SyntaxSpan>> highlight_rules(
    const SyntaxRuleSet& rules,
    const std::vector<std::string>& lines
);
[[nodiscard]] std::vector<SyntaxSpellingRange> spelling_ranges(
    const SyntaxRuleSet& rules,
    const std::vector<std::string>& lines
);

}  // namespace mcpp::detail
