#include "mcpp/editor_search.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <climits>
#include <cwchar>
#include <cwctype>
#include <limits>
#include <locale.h>
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

class Utf8CType {
public:
    explicit Utf8CType(const bool utf8 = true) {
        struct Locale {
            locale_t value;

            explicit Locale(const char* name)
                : value(newlocale(LC_CTYPE_MASK, name, nullptr)) {
                if (value == nullptr) {
                    throw std::runtime_error(std::string(name) + " locale is unavailable");
                }
            }

            ~Locale() {
                freelocale(value);
            }
        };
        static const Locale unicode("C.UTF-8");
        static const Locale bytes("C");
        previous_ = uselocale(utf8 ? unicode.value : bytes.value);
        if (previous_ == nullptr) {
            throw std::runtime_error("cannot select UTF-8 search locale");
        }
    }

    ~Utf8CType() {
        uselocale(previous_);
    }

    Utf8CType(const Utf8CType&) = delete;
    Utf8CType& operator=(const Utf8CType&) = delete;

private:
    locale_t previous_{nullptr};
};

struct FoldedText {
    std::u32string characters;
    std::vector<std::size_t> offsets;
};

FoldedText fold_utf8(const std::string_view text) {
    const Utf8CType locale;
    FoldedText result;
    std::mbstate_t state{};
    std::size_t offset = 0;
    while (offset < text.size()) {
        result.offsets.push_back(offset);
        wchar_t character{};
        auto length = std::mbrtowc(
            &character,
            text.data() + offset,
            text.size() - offset,
            &state
        );
        if (length == static_cast<std::size_t>(-1) ||
            length == static_cast<std::size_t>(-2)) {
            state = {};
            // Keep invalid bytes distinct from valid Unicode characters.
            result.characters.push_back(
                static_cast<char32_t>(
                    0x110000U +
                    static_cast<unsigned char>(text[offset])
                )
            );
            length = 1U;
        } else {
            result.characters.push_back(
                static_cast<char32_t>(
                    std::towlower(static_cast<wint_t>(character))
                )
            );
            if (length == 0U) {
                length = 1U;
            }
        }
        offset += length;
    }
    result.offsets.push_back(text.size());
    return result;
}

std::size_t next_codepoint(const std::string_view text, const std::size_t offset) {
    if (offset >= text.size()) {
        return text.size() + 1U;
    }
    auto next = offset + 1U;
    while (next < text.size() &&
           (static_cast<unsigned char>(text[next]) & 0xC0U) == 0x80U) {
        ++next;
    }
    return next;
}

bool word_at(const std::string_view text, const std::size_t offset) {
    if (offset >= text.size()) {
        return false;
    }
    const Utf8CType locale;
    wchar_t character{};
    std::mbstate_t state{};
    const auto count = std::mbrtowc(&character, text.data() + offset, text.size() - offset, &state);
    return count != static_cast<std::size_t>(-1) &&
           count != static_cast<std::size_t>(-2) &&
           (character == L'_' || std::iswalnum(static_cast<wint_t>(character)) != 0);
}

bool whole_word(
    const std::string_view text,
    const EditorSearchMatch match
) {
    auto previous = match.begin;
    if (previous > 0U) {
        --previous;
        while (previous > 0U &&
               (static_cast<unsigned char>(text[previous]) & 0xC0U) == 0x80U) {
            --previous;
        }
    }
    const bool left = match.begin > 0U && word_at(text, previous);
    const bool right = word_at(text, match.end);
    return !left && !right;
}

std::vector<EditorSearchMatch> utf8_literal_matches(
    const std::string_view text,
    const std::string_view pattern,
    const std::size_t range_begin,
    const std::size_t range_end,
    const EditorSearchOptions& options,
    const bool overlap,
    const std::size_t limit
) {
    const auto haystack = fold_utf8(text);
    const auto needle = fold_utf8(pattern);
    auto cursor = static_cast<std::size_t>(std::distance(
        haystack.offsets.begin(),
        std::lower_bound(
            haystack.offsets.begin(),
            haystack.offsets.end(),
            range_begin
        )
    ));
    std::vector<EditorSearchMatch> result;
    while (cursor < haystack.characters.size()) {
        const auto found = haystack.characters.find(needle.characters, cursor);
        if (found == std::u32string::npos) {
            break;
        }
        const EditorSearchMatch candidate{
            haystack.offsets[found],
            haystack.offsets[found + needle.characters.size()],
        };
        if (candidate.end > range_end) {
            break;
        }
        if (!options.whole_words || whole_word(text, candidate)) {
            result.push_back(candidate);
            if (result.size() >= limit) {
                break;
            }
        }
        cursor = found + (overlap ? 1U : needle.characters.size());
    }
    return result;
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
        const bool case_sensitive,
        const bool line_oriented = true,
        const bool utf8 = true
    ) : utf8_(utf8) {
        const Utf8CType locale(utf8_);
        if (pattern.find('\0') != std::string_view::npos) {
            throw std::invalid_argument("regular expressions cannot contain a literal NUL; use hex search");
        }
        auto expression = std::string(pattern);
        const auto flags =
            REG_EXTENDED | (line_oriented ? REG_NEWLINE : 0) |
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
        const Utf8CType locale(utf8_);
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

    [[nodiscard]] std::size_t capture_count() const { return value_.re_nsub + 1U; }

private:
    regex_t value_{};
    bool valid_{false};
    bool utf8_{true};
};

std::string wildcard_expression(const std::string_view pattern) {
    std::string expression;
    unsigned int groups = 0;
    const auto literal = [&](const char character) {
        if (std::string_view(".^$|()[]{}*+?\\").find(character) != std::string_view::npos) {
            expression += '\\';
        }
        expression += character;
    };
    for (std::size_t index = 0; index < pattern.size(); ++index) {
        const auto character = pattern[index];
        if (character == '\\') {
            if (++index >= pattern.size()) {
                throw std::invalid_argument("unfinished wildcard escape");
            }
            literal(pattern[index]);
        } else if (character == '*') {
            expression += groups == 0U ? "(.*)" : ".*";
        } else if (character == '?') {
            expression += groups == 0U ? "(.)" : ".";
        } else if (character == '{') {
            expression += '(';
            ++groups;
        } else if (character == '}') {
            if (groups == 0U) {
                throw std::invalid_argument("unmatched wildcard brace");
            }
            expression += ')';
            --groups;
        } else if (character == ',' && groups > 0U) {
            expression += '|';
        } else if (character == '[') {
            const auto begin = index;
            auto end = index + 1U;
            if (end < pattern.size() && (pattern[end] == '!' || pattern[end] == '^')) {
                ++end;
            }
            if (end < pattern.size() && pattern[end] == ']') {
                ++end;
            }
            while (end < pattern.size() && pattern[end] != ']') {
                if (pattern[end] == '[' && end + 1U < pattern.size() &&
                    pattern[end + 1U] == ':') {
                    const auto class_end = pattern.find(":]", end + 2U);
                    if (class_end == std::string_view::npos) {
                        throw std::invalid_argument("unfinished wildcard character class");
                    }
                    end = class_end + 2U;
                } else {
                    ++end;
                }
            }
            if (end >= pattern.size()) {
                throw std::invalid_argument("unfinished wildcard character class");
            }
            expression += '[';
            auto content = begin + 1U;
            if (pattern[content] == '!') {
                expression += '^';
                ++content;
            }
            expression.append(pattern.substr(content, end - content + 1U));
            index = end;
        } else {
            literal(character);
        }
    }
    if (groups != 0U) {
        throw std::invalid_argument("unfinished wildcard brace");
    }
    return expression;
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
        options.mode == EditorSearchMode::wildcard ? wildcard_expression(pattern) : std::string(pattern),
        options.case_sensitive
    );
    std::vector<EditorSearchMatch> result;
    auto cursor = range_begin;
    while (cursor <= range_end) {
        std::vector<regmatch_t> match(expression.capture_count());
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
        EditorSearchMatch candidate{match_begin, match_end};
        for (const auto& capture : match) {
            candidate.captures.emplace_back(
                capture.rm_so < 0 ? std::string::npos : static_cast<std::size_t>(capture.rm_so),
                capture.rm_eo < 0 ? std::string::npos : static_cast<std::size_t>(capture.rm_eo)
            );
        }
        if (!(candidate.begin == candidate.end && candidate.begin == text.size()) &&
            (!options.whole_words || whole_word(text, candidate))) {
            result.push_back(candidate);
            if (result.size() >= limit) {
                return result;
            }
        }
        const auto next =
            overlap || match_begin == match_end ? next_codepoint(text, match_begin) : match_end;
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
    return regex_matches(text, pattern, range_begin, range_end, options, overlap, limit);
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
    const auto non_ascii = [](const std::string_view value) {
        return std::ranges::any_of(value, [](const unsigned char character) {
            return character >= 0x80U;
        });
    };
    if (options.mode == EditorSearchMode::normal &&
        !options.case_sensitive &&
        (non_ascii(text) || non_ascii(bytes))) {
        return utf8_literal_matches(
            text, bytes, range_begin, range_end, options, overlap, limit
        );
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

bool EditorSearch::filename_matches(
    const std::string_view name,
    const std::string_view pattern,
    const bool case_sensitive
) {
    const auto valid_utf8 = [](const std::string_view value) {
        const auto folded = fold_utf8(value);
        return std::ranges::none_of(folded.characters, [](const char32_t character) {
            return character > 0x10FFFFU;
        });
    };
    const RegexPattern expression(
        "^(" + wildcard_expression(pattern) + ")$", case_sensitive, false,
        valid_utf8(name) && valid_utf8(pattern)
    );
    std::array<regmatch_t, 1> match{};
    const auto status = expression.find(name, 0, name.size(), match.data(), match.size());
    if (status != 0 && status != REG_NOMATCH) {
        throw std::runtime_error("filename pattern matching failed");
    }
    return status == 0 &&
           match[0].rm_so == 0 &&
           static_cast<std::size_t>(match[0].rm_eo) == name.size();
}

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
        auto valid = before;
        std::erase_if(valid, [&](const EditorSearchMatch& match) {
            return match.begin == match.end && match.begin == start;
        });
        if (!valid.empty()) {
            return valid.back();
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
    if (options.mode != EditorSearchMode::regular_expression &&
        options.mode != EditorSearchMode::wildcard) {
        return options.mode == EditorSearchMode::hexadecimal
                   ? hexadecimal_pattern(replacement_value)
                   : std::string(replacement_value);
    }
    auto captures = selected.captures;
    if (captures.empty()) {
        const RegexPattern expression(
            options.mode == EditorSearchMode::wildcard
                ? wildcard_expression(pattern) : std::string(pattern),
            options.case_sensitive
        );
        std::vector<regmatch_t> match(expression.capture_count());
        const auto end = selected.end < text.size() && text[selected.end] == '\n'
                             ? selected.end + 1U : selected.end;
        if (expression.find(text, selected.begin, end, match.data(), match.size()) != 0 ||
            static_cast<std::size_t>(match[0].rm_so) != selected.begin ||
            static_cast<std::size_t>(match[0].rm_eo) != selected.end) {
            throw std::runtime_error("selected text no longer matches the expression");
        }
        for (const auto& capture : match) {
            captures.emplace_back(
                capture.rm_so < 0 ? std::string::npos : static_cast<std::size_t>(capture.rm_so),
                capture.rm_eo < 0 ? std::string::npos : static_cast<std::size_t>(capture.rm_eo)
            );
        }
    }
    auto replacement = std::string(replacement_value);
    if (options.mode == EditorSearchMode::wildcard) {
        std::string translated;
        std::size_t capture = 0;
        for (std::size_t index = 0; index < replacement.size(); ++index) {
            if (replacement[index] == '\\' && index + 1U < replacement.size()) {
                translated += replacement[index];
                translated += replacement[++index];
            } else if (replacement[index] == '*' || replacement[index] == '?') {
                translated += "${" + std::to_string(++capture) + "}";
            } else {
                translated += replacement[index];
            }
        }
        replacement = std::move(translated);
    }
    std::string result;
    int transform = 0;
    int next_transform = 0;
    const Utf8CType locale;
    const auto append = [&](const std::string_view value) {
        std::mbstate_t input_state{};
        for (std::size_t offset = 0; offset < value.size();) {
            wchar_t character{};
            auto length = std::mbrtowc(
                &character, value.data() + offset, value.size() - offset, &input_state
            );
            if (length == static_cast<std::size_t>(-1) ||
                length == static_cast<std::size_t>(-2)) {
                input_state = {};
                result += value[offset++];
                next_transform = 0;
                continue;
            }
            if (length == 0U) {
                length = 1U;
            }
            const auto operation = next_transform != 0 ? next_transform : transform;
            next_transform = 0;
            if (operation == 0) {
                result.append(value.substr(offset, length));
            } else {
                character = static_cast<wchar_t>(
                    operation > 0 ? std::towupper(static_cast<wint_t>(character))
                                  : std::towlower(static_cast<wint_t>(character))
                );
                std::array<char, MB_LEN_MAX> encoded{};
                std::mbstate_t output_state{};
                const auto count = std::wcrtomb(encoded.data(), character, &output_state);
                if (count == static_cast<std::size_t>(-1)) {
                    throw std::runtime_error("cannot encode replacement character");
                }
                result.append(encoded.data(), count);
            }
            offset += length;
        }
    };
    const auto append_capture = [&](const std::size_t capture) {
        if (capture >= captures.size()) {
            throw std::invalid_argument("replacement references a nonexistent capture");
        }
        const auto [begin, end] = captures[capture];
        if (begin != std::string::npos) {
            if (begin > end || end > text.size()) {
                throw std::runtime_error("replacement capture is outside the text");
            }
            append(text.substr(begin, end - begin));
        }
    };
    for (std::size_t index = 0; index < replacement.size(); ++index) {
        const auto marker = replacement[index];
        if ((marker == '$' || marker == '\\') &&
            index + 1U < replacement.size() &&
            std::isdigit(
                static_cast<unsigned char>(
                    replacement[index + 1U]
                )
            ) != 0) {
            const auto capture = static_cast<std::size_t>(
                replacement[++index] - '0'
            );
            append_capture(capture);
        } else if (marker == '$' && index + 1U < replacement.size() &&
                   replacement[index + 1U] == '{') {
            const auto end = replacement.find('}', index + 2U);
            if (end == std::string::npos || end == index + 2U) {
                throw std::invalid_argument("invalid replacement capture");
            }
            std::size_t capture = 0;
            for (auto digit = index + 2U; digit < end; ++digit) {
                if (!std::isdigit(static_cast<unsigned char>(replacement[digit])) ||
                    capture > captures.size()) {
                    throw std::invalid_argument("invalid replacement capture");
                }
                capture = capture * 10U + static_cast<std::size_t>(replacement[digit] - '0');
            }
            append_capture(capture);
            index = end;
        } else if (marker == '$' &&
                   index + 1U < replacement.size() &&
                   replacement[index + 1U] == '$') {
            append("$");
            ++index;
        } else if (marker == '$' &&
                   index + 1U < replacement.size() &&
                   replacement[index + 1U] == '&') {
            append(text.substr(selected.begin, selected.end - selected.begin));
            ++index;
        } else if (marker == '\\') {
            if (++index >= replacement.size()) {
                throw std::invalid_argument("unfinished replacement escape");
            }
            switch (replacement[index]) {
                case 'n': append("\n"); break;
                case 'r': append("\r"); break;
                case 't': append("\t"); break;
                case 'a': append("\a"); break;
                case 'b': append("\b"); break;
                case 'f': append("\f"); break;
                case 'v': append("\v"); break;
                case 'U': transform = 1; break;
                case 'L': transform = -1; break;
                case 'u': next_transform = 1; break;
                case 'l': next_transform = -1; break;
                case 'E': transform = 0; next_transform = 0; break;
                default: append(std::string_view(replacement).substr(index, 1U)); break;
            }
        } else {
            const auto end = next_codepoint(replacement, index);
            append(std::string_view(replacement).substr(index, end - index));
            index = end - 1U;
        }
    }
    return result;
}

}  // namespace mcpp
