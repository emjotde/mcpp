#include "mcpp/syntax.hpp"
#include "syntax_rules.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <span>
#include <string_view>
#include <stdexcept>

namespace mcpp {
namespace {

enum class Language {
    none,
    cpp,
    rust,
    go,
    javascript,
    python,
    shell,
    json,
    markdown,
    config,
};

constexpr std::array<std::pair<std::string_view, Language>, 10> named_languages{{
    {"plain", Language::none}, {"cpp", Language::cpp}, {"rust", Language::rust},
    {"go", Language::go}, {"javascript", Language::javascript}, {"python", Language::python},
    {"shell", Language::shell}, {"json", Language::json}, {"markdown", Language::markdown},
    {"config", Language::config}
}};

constexpr std::array<std::string_view, 10> language_names{
    "unknown", "C/C++ Program", "Rust Program", "GoLang Program",
    "JavaScript Program", "Python Program", "Shell Script", "JSON File",
    "Markdown", "INI-based File"
};

using Words = std::span<const std::string_view>;

std::string lowercase(std::string value) {
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](const unsigned char character) {
            return static_cast<char>(std::tolower(character));
        }
    );
    return value;
}

Language language_for(const std::filesystem::path& path) {
    const auto extension = lowercase(path.extension().string());
    const auto name = lowercase(path.filename().string());
    if (extension == ".c" || extension == ".h" ||
        extension == ".cc" || extension == ".cpp" ||
        extension == ".cxx" || extension == ".hh" ||
        extension == ".hpp") {
        return Language::cpp;
    }
    if (extension == ".rs") {
        return Language::rust;
    }
    if (extension == ".go") {
        return Language::go;
    }
    if (extension == ".js" || extension == ".jsx" ||
        extension == ".ts" || extension == ".tsx" ||
        extension == ".java" || extension == ".cs" ||
        extension == ".kt" || extension == ".swift") {
        return Language::javascript;
    }
    if (extension == ".py" || name == "sconstruct") {
        return Language::python;
    }
    if (extension == ".sh" || extension == ".bash" ||
        extension == ".zsh" || name == "makefile" ||
        name == "dockerfile") {
        return Language::shell;
    }
    if (extension == ".json") {
        return Language::json;
    }
    if (extension == ".md" || extension == ".markdown") {
        return Language::markdown;
    }
    if (extension == ".ini" || extension == ".toml" ||
        extension == ".yaml" || extension == ".yml" ||
        extension == ".conf") {
        return Language::config;
    }
    return Language::none;
}

bool identifier_start(const unsigned char value) {
    return value >= 0x80U || std::isalpha(value) != 0 ||
           value == '_';
}

bool identifier_continue(const unsigned char value) {
    return identifier_start(value) || std::isdigit(value) != 0;
}

bool contains(const Words words, const std::string_view word) {
    return std::find(words.begin(), words.end(), word) != words.end();
}

constexpr std::array<std::string_view, 40> cpp_keywords{
    "alignas", "alignof", "auto", "break", "case", "catch",
    "class", "concept", "const", "constexpr", "continue",
    "co_await", "co_return", "co_yield", "default", "delete",
    "do", "else", "enum", "explicit", "export", "extern",
    "for", "friend", "if", "inline", "namespace", "new",
    "noexcept", "operator", "private", "protected", "public",
    "requires", "return", "static", "struct", "switch",
    "template", "while",
};

constexpr std::array<std::string_view, 17> rust_keywords{
    "as", "async", "await", "break", "const", "continue",
    "crate", "else", "enum", "fn", "for", "if", "impl",
    "let", "match", "mod", "move",
};

constexpr std::array<std::string_view, 17> go_keywords{
    "break", "case", "chan", "const", "continue", "default",
    "defer", "else", "fallthrough", "for", "func", "go",
    "goto", "if", "import", "interface", "map",
};

constexpr std::array<std::string_view, 24> javascript_keywords{
    "async", "await", "break", "case", "catch", "class",
    "const", "continue", "default", "do", "else", "export",
    "extends", "finally", "for", "function", "if", "import",
    "let", "new", "return", "switch", "throw", "while",
};

constexpr std::array<std::string_view, 24> python_keywords{
    "and", "as", "assert", "async", "await", "break",
    "class", "continue", "def", "del", "elif", "else",
    "except", "finally", "for", "from", "global", "if",
    "import", "in", "is", "lambda", "return", "while",
};

constexpr std::array<std::string_view, 20> common_types{
    "bool", "byte", "char", "double", "float", "i8", "i16",
    "i32", "i64", "int", "long", "short", "size_t", "string",
    "u8", "u16", "u32", "u64", "unsigned", "void",
};

Words keywords_for(const Language language) {
    switch (language) {
        case Language::cpp:
            return cpp_keywords;
        case Language::rust:
            return rust_keywords;
        case Language::go:
            return go_keywords;
        case Language::javascript:
            return javascript_keywords;
        case Language::python:
            return python_keywords;
        default:
            return {};
    }
}

void add_span(
    std::vector<SyntaxSpan>& spans,
    const std::size_t begin,
    const std::size_t end,
    const SyntaxStyle style
) {
    if (begin < end) {
        spans.push_back({begin, end, style});
    }
}

void highlight_code_line(
    const std::string_view line,
    const Language language,
    bool& block_comment,
    std::vector<SyntaxSpan>& spans
) {
    const auto words = keywords_for(language);
    const auto first = line.find_first_not_of(" \t");
    if (language == Language::cpp &&
        !block_comment &&
        first != std::string_view::npos && line[first] == '#') {
        std::size_t cursor = first;
        while (cursor < line.size()) {
            const auto line_comment = line.find("//", cursor);
            const auto block_begin = line.find("/*", cursor);
            const auto marker = std::min(line_comment, block_begin);
            if (marker == std::string_view::npos) {
                add_span(
                    spans,
                    cursor,
                    line.size(),
                    SyntaxStyle::preprocessor
                );
                break;
            }
            add_span(
                spans,
                cursor,
                marker,
                SyntaxStyle::preprocessor
            );
            if (marker == line_comment) {
                add_span(
                    spans,
                    marker,
                    line.size(),
                    SyntaxStyle::comment
                );
                break;
            }
            const auto block_end = line.find("*/", marker + 2U);
            if (block_end == std::string_view::npos) {
                add_span(
                    spans,
                    marker,
                    line.size(),
                    SyntaxStyle::comment
                );
                block_comment = true;
                break;
            }
            add_span(
                spans,
                marker,
                block_end + 2U,
                SyntaxStyle::comment
            );
            cursor = block_end + 2U;
        }
        return;
    }

    for (std::size_t cursor = 0; cursor < line.size();) {
        if (block_comment) {
            const auto end = line.find("*/", cursor);
            if (end == std::string_view::npos) {
                add_span(
                    spans,
                    cursor,
                    line.size(),
                    SyntaxStyle::comment
                );
                return;
            }
            add_span(spans, cursor, end + 2U, SyntaxStyle::comment);
            cursor = end + 2U;
            block_comment = false;
            continue;
        }
        if (cursor + 1U < line.size() &&
            line.substr(cursor, 2U) == "//") {
            add_span(
                spans,
                cursor,
                line.size(),
                SyntaxStyle::comment
            );
            return;
        }
        if (cursor + 1U < line.size() &&
            line.substr(cursor, 2U) == "/*") {
            block_comment = true;
            continue;
        }
        if (line[cursor] == '"' || line[cursor] == '\'' ||
            (
                language == Language::javascript &&
                line[cursor] == '`'
            )) {
            const auto quote = line[cursor];
            const auto begin = cursor++;
            bool escaped = false;
            while (cursor < line.size()) {
                const auto value = line[cursor++];
                if (!escaped && value == quote) {
                    break;
                }
                escaped = !escaped && value == '\\';
                if (value != '\\') {
                    escaped = false;
                }
            }
            add_span(
                spans,
                begin,
                cursor,
                SyntaxStyle::string_literal
            );
            continue;
        }
        const auto byte = static_cast<unsigned char>(line[cursor]);
        if (identifier_start(byte)) {
            const auto begin = cursor++;
            while (cursor < line.size() &&
                   identifier_continue(
                       static_cast<unsigned char>(line[cursor])
                   )) {
                ++cursor;
            }
            const auto word = line.substr(begin, cursor - begin);
            if (contains(words, word)) {
                add_span(
                    spans,
                    begin,
                    cursor,
                    SyntaxStyle::keyword
                );
            } else if (contains(common_types, word)) {
                add_span(
                    spans,
                    begin,
                    cursor,
                    SyntaxStyle::type
                );
            }
            continue;
        }
        if (std::isdigit(byte) != 0) {
            const auto begin = cursor++;
            while (cursor < line.size()) {
                const auto value =
                    static_cast<unsigned char>(line[cursor]);
                if (std::isalnum(value) == 0 &&
                    value != '.' && value != '_') {
                    break;
                }
                ++cursor;
            }
            add_span(
                spans,
                begin,
                cursor,
                SyntaxStyle::number
            );
            continue;
        }
        ++cursor;
    }
}

void highlight_hash_comments(
    const std::string_view line,
    const Language language,
    std::vector<SyntaxSpan>& spans
) {
    const auto words = keywords_for(language);
    for (std::size_t cursor = 0; cursor < line.size();) {
        if (line[cursor] == '#') {
            add_span(
                spans,
                cursor,
                line.size(),
                SyntaxStyle::comment
            );
            return;
        }
        if (line[cursor] == '"' || line[cursor] == '\'') {
            const auto quote = line[cursor];
            const auto begin = cursor++;
            bool escaped = false;
            while (cursor < line.size()) {
                const auto value = line[cursor++];
                if (!escaped && value == quote) {
                    break;
                }
                escaped = !escaped && value == '\\';
                if (value != '\\') {
                    escaped = false;
                }
            }
            add_span(
                spans,
                begin,
                cursor,
                SyntaxStyle::string_literal
            );
            continue;
        }
        const auto byte = static_cast<unsigned char>(line[cursor]);
        if (identifier_start(byte)) {
            const auto begin = cursor++;
            while (cursor < line.size() &&
                   identifier_continue(
                       static_cast<unsigned char>(line[cursor])
                   )) {
                ++cursor;
            }
            if (contains(
                    words,
                    line.substr(begin, cursor - begin)
                )) {
                add_span(
                    spans,
                    begin,
                    cursor,
                    SyntaxStyle::keyword
                );
            }
            continue;
        }
        if (std::isdigit(byte) != 0) {
            const auto begin = cursor++;
            while (cursor < line.size() &&
                   (
                       std::isdigit(
                           static_cast<unsigned char>(line[cursor])
                       ) != 0 ||
                       line[cursor] == '.'
                   )) {
                ++cursor;
            }
            add_span(
                spans,
                begin,
                cursor,
                SyntaxStyle::number
            );
            continue;
        }
        ++cursor;
    }
}

void highlight_data_line(
    const std::string_view line,
    const Language language,
    std::vector<SyntaxSpan>& spans
) {
    if (language == Language::markdown) {
        const auto first = line.find_first_not_of(' ');
        if (first != std::string_view::npos && line[first] == '#') {
            add_span(
                spans,
                first,
                line.size(),
                SyntaxStyle::keyword
            );
            return;
        }
    }
    if (language == Language::config) {
        const auto first = line.find_first_not_of(" \t");
        if (first != std::string_view::npos &&
            (line[first] == '#' || line[first] == ';')) {
            add_span(
                spans,
                first,
                line.size(),
                SyntaxStyle::comment
            );
            return;
        }
        const auto equals = line.find('=');
        if (equals != std::string_view::npos) {
            add_span(
                spans,
                0,
                equals,
                SyntaxStyle::keyword
            );
        }
    }

    for (std::size_t cursor = 0; cursor < line.size();) {
        if (line[cursor] == '"' || line[cursor] == '\'' ||
            line[cursor] == '`') {
            const auto quote = line[cursor];
            const auto begin = cursor++;
            bool escaped = false;
            while (cursor < line.size()) {
                const auto value = line[cursor++];
                if (!escaped && value == quote) {
                    break;
                }
                escaped = !escaped && value == '\\';
                if (value != '\\') {
                    escaped = false;
                }
            }
            add_span(
                spans,
                begin,
                cursor,
                SyntaxStyle::string_literal
            );
            continue;
        }
        const auto byte = static_cast<unsigned char>(line[cursor]);
        if (std::isdigit(byte) != 0) {
            const auto begin = cursor++;
            while (cursor < line.size() &&
                   (
                       std::isdigit(
                           static_cast<unsigned char>(line[cursor])
                       ) != 0 ||
                       line[cursor] == '.'
                   )) {
                ++cursor;
            }
            add_span(
                spans,
                begin,
                cursor,
                SyntaxStyle::number
            );
            continue;
        }
        if (language == Language::json &&
            identifier_start(byte)) {
            const auto begin = cursor++;
            while (cursor < line.size() &&
                   identifier_continue(
                       static_cast<unsigned char>(line[cursor])
                   )) {
                ++cursor;
            }
            const auto word = line.substr(begin, cursor - begin);
            if (word == "true" || word == "false" ||
                word == "null") {
                add_span(
                    spans,
                    begin,
                    cursor,
                    SyntaxStyle::keyword
                );
            }
            continue;
        }
        ++cursor;
    }
}

}  // namespace

const std::vector<std::string>& SyntaxHighlighter::languages() {
    static const auto result = [] {
        std::vector<std::string> names{"auto"};
        for (const auto& [name, language] : named_languages) {
            static_cast<void>(language);
            names.emplace_back(name);
        }
        return names;
    }();
    return result;
}

std::string SyntaxHighlighter::language_name(
    const std::filesystem::path& path,
    const std::vector<std::string>& lines,
    const std::string_view requested_language
) const {
    if (const auto* external = external_definition(path, lines, requested_language)) {
        return external->definition.name;
    }
    const auto selected = requested_language == "auto"
        ? std::ranges::find(named_languages, language_for(path), &std::pair<std::string_view, Language>::second)
        : std::ranges::find(named_languages, requested_language, &std::pair<std::string_view, Language>::first);
    if (selected == named_languages.end()) throw std::runtime_error("unknown syntax language");
    return std::string(language_names[static_cast<std::size_t>(selected->second)]);
}

void SyntaxHighlighter::load_registry(
    const std::filesystem::path& registry,
    const std::filesystem::path& definitions
) {
    if (registry_ && !registry_->definitions().empty() && !std::filesystem::exists(registry)) {
        throw std::runtime_error("syntax registry is missing; keeping current rules: " + registry.string());
    }
    auto candidate = detail::SyntaxRegistry::load(registry, definitions);
    registry_ = std::move(candidate);
}

std::vector<SyntaxDefinition> SyntaxHighlighter::definitions() const {
    return registry_ ? registry_->definitions() : std::vector<SyntaxDefinition>{};
}

bool SyntaxHighlighter::supports_language(const std::string_view language) const {
    if (std::ranges::find(languages(), language) != languages().end()) return true;
    const auto available = definitions();
    return std::ranges::any_of(available, [&](const auto& definition) {
        return definition.id == language;
    });
}

const detail::SyntaxRegistryEntry* SyntaxHighlighter::external_definition(
    const std::filesystem::path& path,
    const std::vector<std::string>& lines,
    const std::string_view language
) const {
    if (registry_) return registry_->select(path, lines.empty() ? std::string_view{} : lines.front(), language);
    if (language.starts_with("external:")) {
        throw std::runtime_error("syntax definition is unavailable: " + std::string(language.substr(9)));
    }
    return nullptr;
}

std::filesystem::path SyntaxHighlighter::definition_path(
    const std::filesystem::path& path,
    const std::vector<std::string>& lines,
    const std::string_view language
) const {
    const auto* external = external_definition(path, lines, language);
    return external ? external->definition.path : std::filesystem::path{};
}

std::optional<std::vector<SyntaxSpellingRange>> SyntaxHighlighter::spelling_ranges(
    const std::filesystem::path& path,
    const std::vector<std::string>& lines,
    const std::string_view requested_language
) const {
    if (const auto* external = external_definition(path, lines, requested_language)) {
        return detail::spelling_ranges(*external->rules, lines);
    }
    if (!supports_language(requested_language)) {
        throw std::runtime_error("unknown syntax language: " + std::string(requested_language));
    }
    return std::nullopt;
}

std::vector<std::vector<SyntaxSpan>> SyntaxHighlighter::highlight(
    const std::filesystem::path& path,
    const std::vector<std::string>& lines,
    const std::string_view requested_language
) const {
    if (const auto* external = external_definition(path, lines, requested_language)) {
        return detail::highlight_rules(*external->rules, lines);
    }
    std::vector<std::vector<SyntaxSpan>> result(lines.size());
    auto language = language_for(path);
    if (requested_language != "auto") {
        const auto selected = std::ranges::find(named_languages, requested_language,
                                               &std::pair<std::string_view, Language>::first);
        if (selected == named_languages.end()) {
            throw std::runtime_error("unknown syntax language: " + std::string(requested_language));
        }
        language = selected->second;
    }
    bool block_comment = false;
    for (std::size_t index = 0; index < lines.size(); ++index) {
        if (language == Language::cpp ||
            language == Language::rust ||
            language == Language::go ||
            language == Language::javascript) {
            highlight_code_line(
                lines[index],
                language,
                block_comment,
                result[index]
            );
        } else if (language == Language::python ||
                   language == Language::shell) {
            highlight_hash_comments(
                lines[index],
                language,
                result[index]
            );
        } else if (language == Language::json ||
                   language == Language::markdown ||
                   language == Language::config) {
            highlight_data_line(
                lines[index],
                language,
                result[index]
            );
        }
    }
    return result;
}

}  // namespace mcpp
