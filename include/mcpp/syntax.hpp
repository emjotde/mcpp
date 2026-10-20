#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mcpp {

namespace detail {
class SyntaxRegistry;
struct SyntaxRegistryEntry;
}  // namespace detail

enum class SyntaxStyle {
    normal,
    keyword,
    type,
    string_literal,
    comment,
    number,
    preprocessor,
};

struct SyntaxColor {
    static constexpr int base = -2;
    static constexpr int terminal_default = -1;
    static constexpr unsigned bold = 1U;
    static constexpr unsigned italic = 2U;
    static constexpr unsigned underline = 4U;
    static constexpr unsigned reverse = 8U;
    static constexpr unsigned blink = 16U;

    int foreground{base};
    int background{base};
    unsigned attributes{0};
    bool operator==(const SyntaxColor&) const = default;
};

struct SyntaxSpan {
    std::size_t begin{0};
    std::size_t end{0};
    SyntaxStyle style{SyntaxStyle::normal};
    std::optional<SyntaxColor> color{};
};

struct SyntaxDefinition {
    std::string id;
    std::string name;
    std::filesystem::path path;
};

struct SyntaxSpellingRange {
    std::size_t begin{0};
    std::size_t end{0};

    [[nodiscard]] bool contains(const std::size_t offset, const std::size_t length) const {
        return offset >= begin && offset <= end && length <= end - offset;
    }
    bool operator==(const SyntaxSpellingRange&) const = default;
};

class SyntaxHighlighter {
public:
    [[nodiscard]] static const std::vector<std::string>& languages();
    void load_registry(
        const std::filesystem::path& registry,
        const std::filesystem::path& definitions
    );
    [[nodiscard]] std::vector<SyntaxDefinition> definitions() const;
    [[nodiscard]] bool supports_language(std::string_view language) const;
    [[nodiscard]] std::filesystem::path definition_path(
        const std::filesystem::path& path,
        const std::vector<std::string>& lines,
        std::string_view language = "auto"
    ) const;
    [[nodiscard]] std::string language_name(
        const std::filesystem::path& path,
        const std::vector<std::string>& lines,
        std::string_view language = "auto"
    ) const;
    // No value leaves builtin/plain spelling unrestricted; an empty result disables it for an external definition.
    [[nodiscard]] std::optional<std::vector<SyntaxSpellingRange>> spelling_ranges(
        const std::filesystem::path& path,
        const std::vector<std::string>& lines,
        std::string_view language = "auto"
    ) const;
    [[nodiscard]] std::vector<std::vector<SyntaxSpan>> highlight(
        const std::filesystem::path& path,
        const std::vector<std::string>& lines,
        std::string_view language = "auto"
    ) const;

private:
    std::shared_ptr<const detail::SyntaxRegistry> registry_;
    [[nodiscard]] const detail::SyntaxRegistryEntry* external_definition(
        const std::filesystem::path& path,
        const std::vector<std::string>& lines,
        std::string_view language
    ) const;
};

}  // namespace mcpp
