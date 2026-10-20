#include "syntax_rules.hpp"
#include "mcpp/native_regex.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace mcpp::detail {
namespace {

struct SourceLine {
    std::filesystem::path path;
    std::size_t number{0};
    std::vector<std::string> words;
};

[[noreturn]] void fail(const SourceLine& line, const std::string& message) {
    throw std::runtime_error(
        line.path.string() + ":" + std::to_string(line.number) + ": " + message
    );
}

char escaped_character(const char value) {
    switch (value) {
        case 's': return ' ';
        case 't': return '\t';
        case 'n': return '\n';
        case 'r': return '\r';
        default: return value;
    }
}

std::string decode(const std::string_view value, const SourceLine& line) {
    std::string result;
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] != '\\') {
            result.push_back(value[index]);
            continue;
        }
        if (++index == value.size()) fail(line, "unfinished escape");
        result.push_back(escaped_character(value[index]));
    }
    return result;
}

std::vector<std::string> split_words(const std::string_view line) {
    std::vector<std::string> words;
    std::size_t cursor = 0;
    while (cursor < line.size()) {
        const auto begin = line.find_first_not_of(" \t\r", cursor);
        if (begin == std::string_view::npos ||
            (words.empty() && line[begin] == '#')) break;
        const auto end = line.find_first_of(" \t\r", begin);
        words.emplace_back(line.substr(begin, end - begin));
        cursor = end == std::string_view::npos ? line.size() : end;
    }
    return words;
}

class SourceReader {
public:
    explicit SourceReader(std::filesystem::path definitions)
        : definitions_(std::move(definitions)) {}

    std::vector<SourceLine> read(const std::filesystem::path& path) {
        std::vector<SourceLine> result;
        append(path, result, {});
        return result;
    }

private:
    std::filesystem::path definitions_;
    std::set<std::filesystem::path> active_;
    std::size_t bytes_{0};

    void append(
        const std::filesystem::path& path,
        std::vector<SourceLine>& result,
        const SourceLine& origin
    ) {
        std::error_code error;
        const auto canonical = std::filesystem::canonical(path, error);
        if (error) {
            if (!origin.path.empty()) fail(origin, "cannot include " + path.string() + ": " + error.message());
            throw std::runtime_error("cannot read syntax registry " + path.string() + ": " + error.message());
        }
        if (!active_.insert(canonical).second) fail(origin, "cyclic syntax include: " + path.string());
        if (active_.size() > 64U) fail(origin, "syntax include nesting exceeds 64 files");
        if (!std::filesystem::is_regular_file(canonical)) {
            throw std::runtime_error("syntax resource is not a regular file: " + path.string());
        }
        std::ifstream input(canonical);
        if (!input) throw std::runtime_error("cannot read syntax resource: " + path.string());
        std::string text;
        std::size_t number = 0;
        while (std::getline(input, text)) {
            bytes_ += text.size() + 1U;
            if (bytes_ > 8U * 1024U * 1024U) {
                throw std::runtime_error("syntax resources exceed 8 MiB");
            }
            SourceLine line{path, ++number, split_words(text)};
            if (line.words.empty()) continue;
            if (line.words.front() != "include") {
                result.push_back(std::move(line));
                continue;
            }
            if (line.words.size() != 2U) fail(line, "include requires one filename");
            auto included = std::filesystem::path(decode(line.words[1], line));
            if (included.empty()) fail(line, "empty include filename");
            if (!included.is_absolute()) {
                const auto relative = path.parent_path() / included;
                included = std::filesystem::exists(relative) ? relative : definitions_ / included;
            }
            append(included, result, line);
        }
        if (!input.eof()) throw std::runtime_error("cannot read syntax resource: " + path.string());
        active_.erase(canonical);
    }
};

enum class AtomKind { literal, any, word, set_one, set_many };

struct Atom {
    AtomKind kind{AtomKind::literal};
    std::string characters;
};

struct Pattern {
    std::vector<Atom> atoms;
    bool left{false};
    bool right{false};
    bool linestart{false};
    bool starts_with_literal{false};
    std::string left_characters;
    std::string right_characters;
};

struct Keyword {
    Pattern pattern;
    SyntaxColor color;
};

struct Context {
    Pattern begin;
    Pattern end;
    SyntaxColor color;
    bool exclusive{false};
    bool spellcheck{false};
    std::vector<Keyword> keywords;
};

constexpr std::string_view default_whole_characters =
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_";

bool same_character(const char left, const char right, const bool insensitive) {
    return left == right || (insensitive &&
        std::tolower(static_cast<unsigned char>(left)) ==
        std::tolower(static_cast<unsigned char>(right)));
}

bool in_set(const char value, const std::string_view characters, const bool insensitive) {
    return std::ranges::any_of(characters, [&](const char character) {
        return same_character(value, character, insensitive);
    });
}

Pattern parse_pattern(
    const std::string_view value,
    const SourceLine& line,
    Pattern pattern,
    const bool delimiter
) {
    // MC dispatches contexts by the first token, before empty repeat markers disappear.
    pattern.starts_with_literal = !value.empty() && value.front() != '*' && value.front() != '+' &&
        !value.starts_with("\\[") && !value.starts_with("\\{");
    for (std::size_t cursor = 0; cursor < value.size(); ++cursor) {
        const auto character = value[cursor];
        if (character == '*') {
            pattern.atoms.push_back({AtomKind::any, {}});
        } else if (character == '+') {
            pattern.atoms.push_back({AtomKind::word, {}});
        } else if (character != '\\') {
            pattern.atoms.push_back({AtomKind::literal, std::string(1, character)});
        } else {
            if (++cursor == value.size()) fail(line, "unfinished pattern escape");
            const auto escaped = value[cursor];
            if (escaped != '[' && escaped != '{') {
                pattern.atoms.push_back({AtomKind::literal, std::string(1, escaped_character(escaped))});
                continue;
            }
            const auto closing = escaped == '[' ? ']' : '}';
            std::string characters;
            bool closed = false;
            while (++cursor < value.size()) {
                if (value[cursor] != '\\') {
                    characters.push_back(value[cursor]);
                    continue;
                }
                if (++cursor == value.size()) fail(line, "unfinished character-set escape");
                if (value[cursor] == closing) {
                    closed = true;
                    break;
                }
                characters.push_back(escaped_character(value[cursor]));
            }
            if (!closed || (characters.empty() && escaped == '{')) {
                fail(line, "empty or unclosed pattern character set");
            }
            // Native definitions use \[\] to keep reserved words literal.
            if (characters.empty()) continue;
            pattern.atoms.push_back({
                escaped == '[' ? AtomKind::set_many : AtomKind::set_one,
                std::move(characters)
            });
        }
    }
    if (pattern.atoms.empty() && !delimiter) fail(line, "syntax pattern must consume at least one character");
    if (pattern.atoms.size() > 512U) fail(line, "syntax pattern exceeds 512 elements");
    if (delimiter && !pattern.atoms.empty() &&
        (pattern.atoms.front().kind == AtomKind::any || pattern.atoms.back().kind == AtomKind::any)) {
        fail(line, "context wildcard cannot be the first or last pattern element");
    }
    return pattern;
}

std::optional<std::size_t> match(
    const Pattern& pattern,
    const std::string_view text,
    const std::size_t begin,
    const bool insensitive,
    const bool allow_empty = false
) {
    if (begin >= text.size() ||
        (pattern.linestart && begin != 0U && text[begin - 1U] != '\n') ||
        (pattern.left && begin != 0U &&
         in_set(text[begin - 1U], pattern.left_characters, insensitive))) return {};

    std::set<std::pair<std::size_t, std::size_t>> failed;
    const auto visit = [&](auto&& self, const std::size_t index, const std::size_t cursor)
        -> std::optional<std::size_t> {
        if (index == pattern.atoms.size()) {
            if ((!allow_empty && cursor == begin) || (pattern.right && cursor < text.size() &&
                in_set(text[cursor], pattern.right_characters, insensitive))) return {};
            return cursor;
        }
        const auto& atom = pattern.atoms[index];
        if (atom.kind == AtomKind::literal || atom.kind == AtomKind::set_one) {
            if (cursor >= text.size()) return {};
            const auto accepts = atom.kind == AtomKind::literal
                ? same_character(text[cursor], atom.characters.front(), insensitive)
                : in_set(text[cursor], atom.characters, insensitive);
            return accepts ? self(self, index + 1U, cursor + 1U) : std::optional<std::size_t>{};
        }
        if (failed.contains({index, cursor})) return {};
        if (atom.kind == AtomKind::any && allow_empty) {
            if (auto found = self(self, index + 1U, cursor)) return found;
        }
        auto end = cursor;
        while (end < text.size()) {
            const auto value = text[end];
            const auto accepts = atom.kind == AtomKind::set_many
                ? in_set(value, atom.characters, insensitive)
                : value != '\n' && (atom.kind != AtomKind::word ||
                    std::isspace(static_cast<unsigned char>(value)) == 0);
            if (!accepts) break;
            ++end;
        }
        const auto minimum = cursor + (atom.kind == AtomKind::word ? 1U : 0U);
        if (atom.kind == AtomKind::any) {
            for (auto candidate = minimum; candidate <= end; ++candidate) {
                if (auto found = self(self, index + 1U, candidate)) return found;
            }
        } else if (minimum <= end) {
            for (auto candidate = end;; --candidate) {
                if (auto found = self(self, index + 1U, candidate)) return found;
                if (candidate == minimum) break;
            }
        }
        failed.insert({index, cursor});
        return {};
    };
    return visit(visit, 0U, begin);
}

std::optional<std::size_t> match_delimiter(
    const Pattern& pattern,
    const std::string_view text,
    const std::size_t begin,
    const bool insensitive
) {
    if (!pattern.starts_with_literal || pattern.atoms.empty()) return {};
    return match(pattern, text, begin, insensitive);
}

int parse_color(const std::string_view original, const SourceLine& line) {
    const auto value = original.substr(0, original.find('/'));
    if (value == "base") return SyntaxColor::base;
    if (value == "default") return SyntaxColor::terminal_default;
    static constexpr std::array<std::string_view, 16> names{
        "black", "red", "green", "brown", "blue", "magenta", "cyan", "lightgray",
        "gray", "brightred", "brightgreen", "yellow", "brightblue", "brightmagenta",
        "brightcyan", "white"
    };
    const auto named = std::ranges::find(names, value);
    if (named != names.end()) return static_cast<int>(named - names.begin());
    const auto decimal = [](const std::string_view digits) -> std::optional<int> {
        if (digits.empty()) return {};
        int number = 0;
        for (const auto character : digits) {
            if (character < '0' || character > '9' || number > 255) return {};
            number = number * 10 + character - '0';
        }
        return number;
    };
    if (value.starts_with("color")) {
        if (const auto number = decimal(value.substr(5)); number && *number >= 16 && *number <= 255) return *number;
    } else if (value.starts_with("gray")) {
        if (const auto number = decimal(value.substr(4)); number && *number <= 23) return 232 + *number;
    } else if (value.starts_with("rgb") && value.size() == 6U &&
               std::ranges::all_of(value.substr(3), [](const char character) {
                   return character >= '0' && character <= '5';
               })) {
        return 16 + 36 * (value[3] - '0') + 6 * (value[4] - '0') + value[5] - '0';
    }
    fail(line, "unknown syntax color: " + std::string(original));
}

unsigned parse_attributes(const std::string_view value, const SourceLine& line) {
    unsigned attributes = 0;
    std::size_t cursor = 0;
    while (cursor < value.size()) {
        const auto end = value.find('+', cursor);
        const auto attribute = value.substr(cursor, end - cursor);
        if (attribute == "bold") attributes |= SyntaxColor::bold;
        else if (attribute == "italic") attributes |= SyntaxColor::italic;
        else if (attribute == "underline") attributes |= SyntaxColor::underline;
        else if (attribute == "reverse") attributes |= SyntaxColor::reverse;
        else if (attribute == "blink") attributes |= SyntaxColor::blink;
        else fail(line, "unsupported syntax attribute: " + std::string(attribute));
        if (end == std::string_view::npos) break;
        cursor = end + 1U;
        if (cursor == value.size()) fail(line, "empty syntax attribute");
    }
    return attributes;
}

SyntaxColor parse_colors(
    const SourceLine& line,
    const std::size_t cursor,
    SyntaxColor color
) {
    const auto count = line.words.size() - cursor;
    if (count > 3U) fail(line, "too many color or attribute fields");
    if (count >= 1U) color.foreground = parse_color(line.words[cursor], line);
    if (count >= 2U) color.background = parse_color(line.words[cursor + 1U], line);
    if (count == 3U) color.attributes = parse_attributes(line.words[cursor + 2U], line);
    return color;
}

NativeRegex registry_regex(const std::string& value, const SourceLine& line) {
    try {
        return NativeRegex(value);
    } catch (const std::invalid_argument& error) {
        fail(line, error.what());
    }
}

}  // namespace

struct SyntaxRuleSet {
    bool insensitive{false};
    std::vector<Context> contexts;
    NativeRegex filename{""};
    std::optional<NativeRegex> first_line;
};

namespace {

class RuleParser {
public:
    std::shared_ptr<SyntaxRuleSet> rules = std::make_shared<SyntaxRuleSet>();
    std::filesystem::path source;

    void parse(SourceLine line) {
        const auto command = line.words.front();
        if (command == "define") {
            if (line.words.size() < 3U) fail(line, "define requires a name and value");
            macros_[line.words[1]] = std::vector<std::string>(line.words.begin() + 2, line.words.end());
            return;
        }
        expand(line);
        if (command == "caseinsensitive") {
            if (line.words.size() != 1U || !rules->contexts.empty()) {
                fail(line, "caseinsensitive must precede contexts");
            }
            rules->insensitive = true;
        } else if (command == "wholechars") {
            if (line.words.size() == 2U) {
                left_ = right_ = decode(line.words[1], line);
            } else if (line.words.size() == 3U &&
                       (line.words[1] == "left" || line.words[1] == "right")) {
                (line.words[1] == "left" ? left_ : right_) = decode(line.words[2], line);
            } else fail(line, "wholechars requires [left|right] characters");
        } else if (command == "context") {
            context(line);
        } else if (command == "keyword") {
            keyword(line);
        } else if (command == "spellcheck") {
            if (line.words.size() != 1U || rules->contexts.empty()) {
                fail(line, "spellcheck requires a preceding context and no arguments");
            }
            rules->contexts.back().spellcheck = true;
        } else {
            fail(line, "unsupported syntax directive: " + command);
        }
    }

private:
    std::string left_{default_whole_characters};
    std::string right_{default_whole_characters};
    std::map<std::string, std::vector<std::string>> macros_;

    void expand(SourceLine& line) const {
        std::vector<std::string> expanded{line.words.front()};
        std::set<std::string> active;
        const auto append = [&](auto&& self, const std::string& word) -> void {
            const auto macro = macros_.find(word);
            if (macro == macros_.end()) {
                expanded.push_back(word);
                return;
            }
            if (!active.insert(word).second || active.size() > 64U) fail(line, "cyclic syntax define: " + word);
            for (const auto& replacement : macro->second) self(self, replacement);
            active.erase(word);
        };
        for (std::size_t index = 1; index < line.words.size(); ++index) append(append, line.words[index]);
        line.words = std::move(expanded);
    }

    Pattern options(const SourceLine& line, std::size_t& cursor) const {
        Pattern pattern;
        pattern.left_characters = left_;
        pattern.right_characters = right_;
        if (cursor < line.words.size()) {
            const auto& option = line.words[cursor];
            if (option == "whole" || option == "wholeleft" || option == "wholeright") {
                pattern.left = option != "wholeright";
                pattern.right = option != "wholeleft";
                ++cursor;
            }
        }
        if (cursor < line.words.size() && line.words[cursor] == "linestart") {
            pattern.linestart = true;
            ++cursor;
        }
        return pattern;
    }

    void context(const SourceLine& line) {
        if (line.words.size() < 2U) fail(line, "context requires delimiters");
        if (line.words[1] == "default") {
            if (!rules->contexts.empty()) fail(line, "context default must be the first and only default context");
            rules->contexts.emplace_back();
            rules->contexts.back().color = parse_colors(line, 2U, {});
            source = line.path;
            return;
        }
        if (rules->contexts.empty()) fail(line, "the first context must be context default");
        std::size_t cursor = 1U;
        Context result;
        if (line.words[cursor] == "exclusive") {
            result.exclusive = true;
            ++cursor;
        }
        auto begin = options(line, cursor);
        if (cursor >= line.words.size()) fail(line, "context requires a start delimiter");
        result.begin = parse_pattern(line.words[cursor++], line, begin, true);
        begin.linestart = false;
        if (cursor < line.words.size() && line.words[cursor] == "linestart") {
            begin.linestart = true;
            ++cursor;
        }
        if (cursor >= line.words.size()) fail(line, "context requires an end delimiter");
        result.end = parse_pattern(line.words[cursor++], line, begin, true);
        result.color = parse_colors(line, cursor, {});
        rules->contexts.push_back(std::move(result));
    }

    void keyword(const SourceLine& line) {
        if (rules->contexts.empty()) fail(line, "keyword requires context default first");
        std::size_t cursor = 1U;
        auto pattern = options(line, cursor);
        if (cursor >= line.words.size()) fail(line, "keyword requires a pattern");
        pattern = parse_pattern(line.words[cursor++], line, std::move(pattern), false);
        rules->contexts.back().keywords.push_back({
            std::move(pattern), parse_colors(line, cursor, rules->contexts.back().color)
        });
    }
};

}  // namespace

std::shared_ptr<const SyntaxRegistry> SyntaxRegistry::load(
    const std::filesystem::path& registry,
    const std::filesystem::path& definitions
) {
    auto result = std::make_shared<SyntaxRegistry>();
    std::error_code error;
    const auto status = std::filesystem::symlink_status(registry, error);
    if ((error == std::errc::no_such_file_or_directory) ||
        (!error && status.type() == std::filesystem::file_type::not_found)) return result;
    if (error) throw std::runtime_error("cannot inspect syntax registry: " + error.message());
    const auto lines = SourceReader(definitions).read(registry);
    RuleParser parser;
    std::optional<SyntaxDefinition> definition;
    const auto finish = [&] {
        if (!definition) return;
        if (parser.rules->contexts.empty()) throw std::runtime_error(
            "syntax definition has no context default: " + definition->name
        );
        definition->path = parser.source;
        result->entries_.push_back({*definition, parser.rules});
    };
    for (const auto& line : lines) {
        if (line.words.front() != "file") {
            if (!definition) fail(line, "syntax registry requires a file section before rules");
            parser.parse(line);
            continue;
        }
        finish();
        if (line.words.size() < 3U || line.words.size() > 4U) {
            fail(line, "file requires filename-regex description [first-line-regex]");
        }
        const auto name = decode(line.words[2], line);
        if (name.empty() || std::ranges::any_of(name, [](const unsigned char value) {
                return value < 0x20U || value == 0x7FU;
            })) fail(line, "invalid syntax description");
        definition = SyntaxDefinition{"external:" + name, name, {}};
        parser = RuleParser{};
        parser.rules->filename = registry_regex(decode(line.words[1], line), line);
        if (line.words.size() == 4U) {
            parser.rules->first_line = registry_regex(decode(line.words[3], line), line);
        }
    }
    finish();
    return result;
}

const SyntaxRegistryEntry* SyntaxRegistry::select(
    const std::filesystem::path& path,
    const std::string_view first_line,
    const std::string_view language
) const {
    if (language != "auto") {
        const auto selected = std::ranges::find(entries_, language, [](const auto& entry) {
            return std::string_view(entry.definition.id);
        });
        if (selected != entries_.end()) return &*selected;
        if (language.starts_with("external:")) {
            throw std::runtime_error("syntax definition is unavailable: " + std::string(language.substr(9)));
        }
        return nullptr;
    }
    const auto filename = path.string();
    for (const auto& entry : entries_) {
        if (entry.rules->filename.search(filename) ||
            (entry.rules->first_line && entry.rules->first_line->search(first_line))) return &entry;
    }
    return nullptr;
}

std::vector<SyntaxDefinition> SyntaxRegistry::definitions() const {
    std::vector<SyntaxDefinition> result;
    result.reserve(entries_.size());
    for (const auto& entry : entries_) {
        if (std::ranges::find(result, entry.definition.id, &SyntaxDefinition::id) == result.end()) {
            result.push_back(entry.definition);
        }
    }
    return result;
}

namespace {

template <typename Emit>
void scan_rules(const SyntaxRuleSet& rules, const std::string_view text, Emit emit) {
    std::size_t active = 0;
    for (std::size_t cursor = 0; cursor < text.size();) {
        const auto& context = rules.contexts[active];
        bool consumed = false;
        for (const auto& keyword : context.keywords) {
            if (const auto end = match(keyword.pattern, text, cursor, rules.insensitive, true)) {
                // MC colors the current byte even when a keyword matches no bytes.
                const auto next = std::max(cursor + 1U, *end);
                emit(cursor, next, keyword.color, context.spellcheck);
                cursor = next;
                consumed = true;
                break;
            }
        }
        if (consumed) continue;
        if (active != 0U) {
            if (const auto end = match_delimiter(context.end, text, cursor, rules.insensitive)) {
                const auto& delimiter = context.exclusive ? rules.contexts.front() : context;
                emit(cursor, *end, delimiter.color, delimiter.spellcheck);
                cursor = *end;
                active = 0;
                continue;
            }
        } else {
            for (std::size_t index = 1U; index < rules.contexts.size(); ++index) {
                const auto& candidate = rules.contexts[index];
                if (const auto end = match_delimiter(candidate.begin, text, cursor, rules.insensitive)) {
                    const auto& delimiter = candidate.exclusive ? context : candidate;
                    emit(cursor, *end, delimiter.color, delimiter.spellcheck);
                    cursor = *end;
                    active = index;
                    consumed = true;
                    break;
                }
            }
            if (consumed) continue;
        }
        emit(cursor, cursor + 1U, context.color, context.spellcheck);
        ++cursor;
    }
}

std::string join_lines(const std::vector<std::string>& lines) {
    std::string text;
    for (std::size_t index = 0; index < lines.size(); ++index) {
        if (index != 0U) text.push_back('\n');
        text += lines[index];
    }
    return text;
}

}  // namespace

std::vector<std::vector<SyntaxSpan>> highlight_rules(
    const SyntaxRuleSet& rules,
    const std::vector<std::string>& lines
) {
    std::vector<std::vector<SyntaxSpan>> result(lines.size());
    std::string text;
    std::vector<std::size_t> offsets;
    for (const auto& line : lines) {
        if (!offsets.empty()) text.push_back('\n');
        offsets.push_back(text.size());
        text += line;
    }
    std::size_t row = 0;
    const auto paint = [&](const std::size_t begin, const std::size_t end, const SyntaxColor& color) {
        auto cursor = begin;
        while (cursor < end && row < lines.size()) {
            const auto line_end = offsets[row] + lines[row].size();
            if (cursor >= line_end) {
                ++row;
                continue;
            }
            if (cursor < offsets[row]) cursor = offsets[row];
            if (cursor >= end) break;
            const auto last = std::min(end, line_end);
            const auto first_column = cursor - offsets[row];
            const auto last_column = last - offsets[row];
            auto& spans = result[row];
            if (!spans.empty() && spans.back().end == first_column && spans.back().color == color) {
                spans.back().end = last_column;
            } else {
                spans.push_back({first_column, last_column, SyntaxStyle::normal, color});
            }
            cursor = last;
        }
    };
    scan_rules(rules, text, [&](const auto begin, const auto end, const SyntaxColor& color, const bool) {
        paint(begin, end, color);
    });
    return result;
}

std::vector<SyntaxSpellingRange> spelling_ranges(
    const SyntaxRuleSet& rules,
    const std::vector<std::string>& lines
) {
    const auto text = join_lines(lines);
    std::vector<SyntaxSpellingRange> result;
    scan_rules(rules, text, [&](const auto begin, const auto end, const SyntaxColor&, const bool eligible) {
        if (!eligible) return;
        if (!result.empty() && result.back().end == begin) result.back().end = end;
        else result.push_back({begin, end});
    });
    return result;
}

}  // namespace mcpp::detail
