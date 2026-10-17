#include "mcpp/editor_search.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <regex.h>
#include <stdexcept>

namespace mcpp {
namespace {

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

bool word_byte(const unsigned char value) {
    return value >= 0x80U || std::isalnum(value) != 0 ||
           value == '_';
}

bool whole_word(
    const std::string_view text,
    const EditorSearchMatch match
) {
    const bool left =
        match.begin > 0U &&
        word_byte(
            static_cast<unsigned char>(text[match.begin - 1U])
        );
    const bool right =
        match.end < text.size() &&
        word_byte(
            static_cast<unsigned char>(text[match.end])
        );
    return !left && !right;
}

std::string hexadecimal_pattern(const std::string_view pattern) {
    std::string compact;
    for (const auto character : pattern) {
        if (!std::isspace(
                static_cast<unsigned char>(character)
            )) {
            compact += character;
        }
    }
    if (compact.empty() || compact.size() % 2U != 0U) {
        throw std::invalid_argument(
            "hex search requires complete byte pairs"
        );
    }
    const auto nibble = [](const char character) -> unsigned int {
        if (character >= '0' && character <= '9') {
            return static_cast<unsigned int>(character - '0');
        }
        const auto lowered = static_cast<char>(
            std::tolower(static_cast<unsigned char>(character))
        );
        if (lowered >= 'a' && lowered <= 'f') {
            return static_cast<unsigned int>(lowered - 'a' + 10);
        }
        throw std::invalid_argument("invalid hexadecimal search");
    };
    std::string result;
    result.reserve(compact.size() / 2U);
    for (std::size_t index = 0; index < compact.size(); index += 2U) {
        result += static_cast<char>(
            (nibble(compact[index]) << 4U) |
            nibble(compact[index + 1U])
        );
    }
    return result;
}

class RegexPattern {
public:
    RegexPattern(
        const std::string_view pattern,
        const bool case_sensitive
    ) {
        auto expression = std::string(pattern);
        const auto flags =
            REG_EXTENDED | REG_NEWLINE |
            (case_sensitive ? 0 : REG_ICASE);
        const auto status = regcomp(
            &value_,
            expression.c_str(),
            flags
        );
        if (status != 0) {
            std::array<char, 256> message{};
            static_cast<void>(regerror(
                status,
                &value_,
                message.data(),
                message.size()
            ));
            throw std::invalid_argument(
                "invalid search expression: " +
                std::string(message.data())
            );
        }
        valid_ = true;
    }

    ~RegexPattern() {
        if (valid_) {
            regfree(&value_);
        }
    }

    RegexPattern(const RegexPattern&) = delete;
    RegexPattern& operator=(const RegexPattern&) = delete;

    int find(
        const std::string_view text,
        const std::size_t begin,
        const std::size_t end,
        regmatch_t* matches,
        const std::size_t match_count
    ) const {
        matches[0].rm_so = static_cast<regoff_t>(begin);
        matches[0].rm_eo = static_cast<regoff_t>(end);
        auto flags = REG_STARTEND;
        if (begin > 0U) {
            flags |= REG_NOTBOL;
        }
        if (end < text.size()) {
            flags |= REG_NOTEOL;
        }
        return regexec(
            &value_,
            text.data(),
            match_count,
            matches,
            flags
        );
    }

private:
    regex_t value_{};
    bool valid_{false};
};

bool wildcard_equal(
    const char left,
    const char right,
    const bool case_sensitive
) {
    if (case_sensitive) {
        return left == right;
    }
    return std::tolower(static_cast<unsigned char>(left)) ==
           std::tolower(static_cast<unsigned char>(right));
}

std::optional<std::size_t> wildcard_match(
    const std::string_view text,
    const std::size_t begin,
    const std::size_t end,
    const std::string_view pattern,
    const bool case_sensitive
) {
    std::size_t text_cursor = begin;
    std::size_t pattern_cursor = 0;
    auto star = std::string_view::npos;
    auto star_text = begin;
    while (text_cursor < end && text[text_cursor] != '\n') {
        if (pattern_cursor == pattern.size()) {
            return text_cursor;
        }
        if (pattern_cursor < pattern.size() &&
            pattern[pattern_cursor] == '\\' &&
            pattern_cursor + 1U < pattern.size()) {
            ++pattern_cursor;
            if (wildcard_equal(
                    text[text_cursor],
                    pattern[pattern_cursor],
                    case_sensitive
                )) {
                ++text_cursor;
                ++pattern_cursor;
                continue;
            }
        } else if (
            pattern_cursor < pattern.size() &&
            (
                pattern[pattern_cursor] == '?' ||
                wildcard_equal(
                    text[text_cursor],
                    pattern[pattern_cursor],
                    case_sensitive
                )
            )
        ) {
            ++text_cursor;
            ++pattern_cursor;
            continue;
        } else if (
            pattern_cursor < pattern.size() &&
            pattern[pattern_cursor] == '*'
        ) {
            if (pattern_cursor + 1U == pattern.size()) {
                const auto newline = text.find('\n', text_cursor);
                return std::min(
                    end,
                    newline == std::string_view::npos
                        ? end
                        : newline
                );
            }
            star = pattern_cursor++;
            star_text = text_cursor;
            continue;
        }
        if (star == std::string_view::npos) {
            return std::nullopt;
        }
        pattern_cursor = star + 1U;
        text_cursor = ++star_text;
    }
    while (pattern_cursor < pattern.size() &&
           pattern[pattern_cursor] == '*') {
        ++pattern_cursor;
    }
    return pattern_cursor == pattern.size()
               ? std::optional<std::size_t>{text_cursor}
               : std::nullopt;
}

std::vector<EditorSearchMatch> regex_matches(
    const std::string_view text,
    const std::string_view pattern,
    const std::size_t range_begin,
    const std::size_t range_end,
    const EditorSearchOptions& options,
    const bool overlap,
    const std::size_t limit
) {
    const RegexPattern expression(
        pattern,
        options.case_sensitive
    );
    std::vector<EditorSearchMatch> result;
    auto cursor = range_begin;
    while (cursor <= range_end) {
        std::array<regmatch_t, 1> match{};
        const auto status = expression.find(
            text,
            cursor,
            range_end,
            match.data(),
            match.size()
        );
        if (status == REG_NOMATCH) {
            break;
        }
        if (status != 0) {
            throw std::runtime_error("regular expression search failed");
        }
        const auto match_begin =
            static_cast<std::size_t>(match[0].rm_so);
        const auto match_end =
            static_cast<std::size_t>(match[0].rm_eo);
        const EditorSearchMatch candidate{match_begin, match_end};
        if (match_begin != match_end &&
            (
                !options.whole_words ||
                whole_word(text, candidate)
            )) {
            result.push_back(candidate);
            if (result.size() >= limit) {
                return result;
            }
        }
        const auto next =
            overlap ? match_begin + 1U
                    : std::max(match_begin + 1U, match_end);
        if (next > range_end) {
            break;
        }
        cursor = next;
    }
    return result;
}

std::vector<EditorSearchMatch> wildcard_matches(
    const std::string_view text,
    const std::string_view pattern,
    const std::size_t range_begin,
    const std::size_t range_end,
    const EditorSearchOptions& options,
    const bool overlap,
    const std::size_t limit
) {
    std::vector<EditorSearchMatch> result;
    auto cursor = range_begin;
    while (cursor < range_end) {
        const auto match_end = wildcard_match(
            text,
            cursor,
            range_end,
            pattern,
            options.case_sensitive
        );
        if (match_end && *match_end > cursor) {
            const EditorSearchMatch candidate{
                cursor,
                *match_end,
            };
            if (!options.whole_words ||
                whole_word(text, candidate)) {
                result.push_back(candidate);
                if (result.size() >= limit) {
                    return result;
                }
            }
            cursor =
                overlap ? cursor + 1U : *match_end;
        } else {
            ++cursor;
        }
    }
    return result;
}

std::vector<EditorSearchMatch> literal_matches(
    const std::string_view text,
    const std::string_view pattern,
    const std::size_t range_begin,
    const std::size_t range_end,
    const EditorSearchOptions& options,
    const bool overlap,
    const std::size_t limit
) {
    const auto bytes =
        options.mode == EditorSearchMode::hexadecimal
            ? hexadecimal_pattern(pattern)
            : std::string(pattern);
    if (bytes.empty()) {
        return {};
    }
    const auto haystack =
        options.case_sensitive ||
                options.mode == EditorSearchMode::hexadecimal
            ? std::string(text)
            : lowercase(std::string(text));
    const auto needle =
        options.case_sensitive ||
                options.mode == EditorSearchMode::hexadecimal
            ? bytes
            : lowercase(bytes);
    std::vector<EditorSearchMatch> result;
    auto cursor = range_begin;
    while (cursor <= range_end) {
        const auto found = haystack.find(needle, cursor);
        if (found == std::string::npos ||
            found + needle.size() > range_end) {
            break;
        }
        const EditorSearchMatch candidate{
            found,
            found + needle.size(),
        };
        if (!options.whole_words || whole_word(text, candidate)) {
            result.push_back(candidate);
            if (result.size() >= limit) {
                return result;
            }
        }
        cursor = found +
                 (
                     overlap
                         ? 1U
                         : std::max<std::size_t>(
                               1U,
                               needle.size()
                           )
                 );
    }
    return result;
}

std::vector<EditorSearchMatch> matches(
    const std::string_view text,
    const std::string_view pattern,
    const std::size_t range_begin,
    const std::size_t range_end,
    const EditorSearchOptions& options,
    const bool overlap,
    const std::size_t limit
) {
    if (range_begin > range_end || range_end > text.size() ||
        pattern.empty()) {
        return {};
    }
    if (options.mode == EditorSearchMode::regular_expression) {
        return regex_matches(
            text,
            pattern,
            range_begin,
            range_end,
            options,
            overlap,
            limit
        );
    }
    if (options.mode == EditorSearchMode::wildcard) {
        return wildcard_matches(
            text,
            pattern,
            range_begin,
            range_end,
            options,
            overlap,
            limit
        );
    }
    return literal_matches(
                     text,
                     pattern,
                     range_begin,
                     range_end,
                     options,
                     overlap,
                     limit
                 );
}

}  // namespace

std::optional<EditorSearchMatch> EditorSearch::find(
    const std::string_view text,
    const std::string_view pattern,
    const std::size_t start,
    const std::size_t range_begin,
    const std::size_t range_end,
    const EditorSearchOptions& options,
    const bool wrap,
    const bool overlap
) {
    if (options.backwards) {
        const auto before = matches(
            text,
            pattern,
            range_begin,
            std::clamp(start, range_begin, range_end),
            options,
            overlap,
            std::numeric_limits<std::size_t>::max()
        );
        if (!before.empty()) {
            return before.back();
        }
        if (!wrap) {
            return std::nullopt;
        }
        const auto wrapped = matches(
            text,
            pattern,
            std::clamp(start, range_begin, range_end),
            range_end,
            options,
            overlap,
            std::numeric_limits<std::size_t>::max()
        );
        return wrapped.empty()
                   ? std::nullopt
                   : std::optional<EditorSearchMatch>{
                         wrapped.back()
                     };
    }
    const auto after = matches(
        text,
        pattern,
        std::clamp(start, range_begin, range_end),
        range_end,
        options,
        overlap,
        1U
    );
    if (!after.empty()) {
        return after.front();
    }
    if (!wrap) {
        return std::nullopt;
    }
    const auto wrapped = matches(
        text,
        pattern,
        range_begin,
        std::clamp(start, range_begin, range_end),
        options,
        overlap,
        1U
    );
    return wrapped.empty()
               ? std::nullopt
               : std::optional<EditorSearchMatch>{wrapped.front()};
}

std::vector<EditorSearchMatch> EditorSearch::find_all(
    const std::string_view text,
    const std::string_view pattern,
    const std::size_t range_begin,
    const std::size_t range_end,
    const EditorSearchOptions& options
) {
    return matches(
        text,
        pattern,
        range_begin,
        range_end,
        options,
        false,
        std::numeric_limits<std::size_t>::max()
    );
}

std::string EditorSearch::replacement(
    const std::string_view text,
    const EditorSearchMatch selected,
    const std::string_view pattern,
    const std::string_view replacement_value,
    const EditorSearchOptions& options
) {
    if (options.mode != EditorSearchMode::regular_expression) {
        return options.mode == EditorSearchMode::hexadecimal
                   ? hexadecimal_pattern(replacement_value)
                   : std::string(replacement_value);
    }
    const RegexPattern expression(
        pattern,
        options.case_sensitive
    );
    std::array<regmatch_t, 10> captures{};
    const auto status = expression.find(
        text,
        selected.begin,
        text.size(),
        captures.data(),
        captures.size()
    );
    if (status != 0 ||
        static_cast<std::size_t>(captures[0].rm_so) !=
            selected.begin ||
        static_cast<std::size_t>(captures[0].rm_eo) !=
            selected.end) {
        throw std::runtime_error(
            "selected text no longer matches the expression"
        );
    }
    std::string result;
    for (std::size_t index = 0;
         index < replacement_value.size();
         ++index) {
        const auto marker = replacement_value[index];
        if ((marker == '$' || marker == '\\') &&
            index + 1U < replacement_value.size() &&
            std::isdigit(
                static_cast<unsigned char>(
                    replacement_value[index + 1U]
                )
            ) != 0) {
            const auto capture = static_cast<std::size_t>(
                replacement_value[++index] - '0'
            );
            if (capture < captures.size() &&
                captures[capture].rm_so >= 0) {
                result.append(
                    text,
                    static_cast<std::size_t>(
                        captures[capture].rm_so
                    ),
                    static_cast<std::size_t>(
                        captures[capture].rm_eo -
                        captures[capture].rm_so
                    )
                );
            }
        } else if (marker == '$' &&
                   index + 1U < replacement_value.size() &&
                   replacement_value[index + 1U] == '$') {
            result += '$';
            ++index;
        } else if (marker == '$' &&
                   index + 1U < replacement_value.size() &&
                   replacement_value[index + 1U] == '&') {
            result.append(
                text,
                selected.begin,
                selected.end - selected.begin
            );
            ++index;
        } else {
            result += marker;
        }
    }
    return result;
}

}  // namespace mcpp
