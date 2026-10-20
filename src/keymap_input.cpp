#include "mcpp/keymap_input.hpp"

#include "mcpp/keymap.hpp"

#include <array>
#include <charconv>
#include <stdexcept>
#include <utility>
#include <vector>

namespace mcpp {
namespace {

constexpr unsigned int shift = 1U;
constexpr unsigned int alt = 2U;
constexpr unsigned int control = 4U;

struct Key {
    std::string name;
    unsigned int modifiers{0};
};

struct CharacterName {
    std::string_view name;
    char character;
};

constexpr CharacterName characters[]{
    {"space", ' '}, {"exclamation", '!'}, {"hash", '#'}, {"dollar", '$'},
    {"percent", '%'}, {"ampersand", '&'}, {"prime", '\''},
    {"lparenthesis", '('}, {"rparenthesis", ')'}, {"asterisk", '*'},
    {"plus", '+'}, {"comma", ','}, {"minus", '-'}, {"dot", '.'},
    {"slash", '/'}, {"colon", ':'}, {"semicolon", ';'}, {"lt", '<'},
    {"equal", '='}, {"gt", '>'}, {"question", '?'}, {"at", '@'},
    {"lbracket", '['}, {"backslash", '\\'}, {"rbracket", ']'},
    {"caret", '^'}, {"underline", '_'}, {"lbrace", '{'}, {"pipe", '|'},
    {"rbrace", '}'},
};

struct Navigation {
    std::string_view name;
    char final;
};

constexpr Navigation navigation[]{
    {"up", 'A'}, {"down", 'B'}, {"right", 'C'}, {"left", 'D'},
    {"home", 'H'}, {"end", 'F'}, {"b2", 'E'},
};

constexpr std::array<unsigned int, 20> function_codes{
    11U, 12U, 13U, 14U, 15U, 17U, 18U, 19U, 20U, 21U,
    23U, 24U, 25U, 26U, 28U, 29U, 31U, 32U, 33U, 34U,
};

bool unicode_scalar(const unsigned int point) noexcept {
    return point <= 0x10ffffU && !(point >= 0xd800U && point <= 0xdfffU);
}

bool printable(const unsigned int point) noexcept {
    return unicode_scalar(point) && point >= 32U &&
           !(point >= 127U && point <= 159U);
}

std::optional<unsigned int> utf8_scalar(const std::string_view input) noexcept {
    if (input.empty()) return std::nullopt;
    const auto first = static_cast<unsigned char>(input.front());
    if (first < 128U) {
        if (input.size() != 1U) return std::nullopt;
        return first;
    }
    unsigned int value = 0;
    unsigned int minimum = 0;
    std::size_t length = 0;
    if (first >= 0xc2U && first <= 0xdfU) {
        value = first & 0x1fU;
        minimum = 0x80U;
        length = 2;
    } else if (first >= 0xe0U && first <= 0xefU) {
        value = first & 0x0fU;
        minimum = 0x800U;
        length = 3;
    } else if (first >= 0xf0U && first <= 0xf4U) {
        value = first & 0x07U;
        minimum = 0x10000U;
        length = 4;
    } else {
        return std::nullopt;
    }
    if (input.size() != length) return std::nullopt;
    for (std::size_t index = 1; index < length; ++index) {
        const auto next = static_cast<unsigned char>(input[index]);
        if (next < 0x80U || next > 0xbfU) return std::nullopt;
        value = (value << 6U) | (next & 0x3fU);
    }
    if (value < minimum || !unicode_scalar(value)) return std::nullopt;
    return value;
}

std::string utf8(const unsigned int point) {
    std::string result;
    if (point < 0x80U) {
        result += static_cast<char>(point);
    } else if (point < 0x800U) {
        result += static_cast<char>(0xc0U | (point >> 6U));
        result += static_cast<char>(0x80U | (point & 0x3fU));
    } else if (point < 0x10000U) {
        result += static_cast<char>(0xe0U | (point >> 12U));
        result += static_cast<char>(0x80U | ((point >> 6U) & 0x3fU));
        result += static_cast<char>(0x80U | (point & 0x3fU));
    } else {
        result += static_cast<char>(0xf0U | (point >> 18U));
        result += static_cast<char>(0x80U | ((point >> 12U) & 0x3fU));
        result += static_cast<char>(0x80U | ((point >> 6U) & 0x3fU));
        result += static_cast<char>(0x80U | (point & 0x3fU));
    }
    return result;
}

std::string canonical(const Key& key) {
    std::string name;
    if ((key.modifiers & control) != 0U) name += "ctrl-";
    if ((key.modifiers & alt) != 0U) name += "alt-";
    if ((key.modifiers & shift) != 0U) name += "shift-";
    return normalize_key_name(name + key.name);
}

std::optional<Key> printable_key(
    const std::string_view input,
    const unsigned int modifiers = 0
) {
    const auto point = utf8_scalar(input);
    if (!point || !printable(*point)) return std::nullopt;
    return Key{normalize_event_key_name(input), modifiers};
}

std::optional<Key> legacy_control(const unsigned char byte) {
    switch (byte) {
        case 8: case 127: return Key{"backspace"};
        case 9: return Key{"tab"};
        case 10: case 13: return Key{"enter"};
        case 27: return Key{"esc"};
        case 28: return Key{"backslash", control};
        case 29: return Key{"rbracket", control};
        case 30: return Key{"caret", control};
        case 31: return Key{"underline", control};
        default: break;
    }
    if (byte >= 1U && byte <= 26U) {
        return Key{std::string(1, static_cast<char>('a' + byte - 1U)), control};
    }
    return std::nullopt;
}

std::optional<unsigned int> number(const std::string_view text) noexcept {
    if (text.empty()) return std::nullopt;
    unsigned int value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

std::optional<std::vector<std::string_view>> fields(
    const std::string_view text,
    const char delimiter,
    const std::size_t maximum
) {
    std::vector<std::string_view> result;
    std::size_t begin = 0;
    while (true) {
        if (result.size() == maximum) return std::nullopt;
        const auto end = text.find(delimiter, begin);
        result.push_back(text.substr(
            begin, end == std::string_view::npos ? text.size() - begin : end - begin
        ));
        if (end == std::string_view::npos) return result;
        begin = end + 1U;
    }
}

std::optional<unsigned int> modifier_bits(const std::string_view text) noexcept {
    const auto parameter = text.empty() ? std::optional<unsigned int>{1U} : number(text);
    if (!parameter || *parameter < 1U || *parameter > 8U) return std::nullopt;
    return *parameter - 1U;
}

std::optional<Key> protocol_point(
    const unsigned int point,
    const unsigned int modifiers,
    const bool kitty_functional = true
) {
    switch (point) {
        case 8: case 127: return Key{"backspace", modifiers};
        case 9: return Key{"tab", modifiers};
        case 10: case 13: return Key{"enter", modifiers};
        case 27: return Key{"esc", modifiers};
        default: break;
    }
    if (!kitty_functional) {
        return printable(point) ? printable_key(utf8(point), modifiers) : std::nullopt;
    }
    // Kitty's functional-key range is not printable Unicode in CSI-u.
    switch (point) {
        case 57344: return Key{"esc", modifiers};
        case 57345: return Key{"enter", modifiers};
        case 57346: return Key{"tab", modifiers};
        case 57347: return Key{"backspace", modifiers};
        case 57348: return Key{"insert", modifiers};
        case 57349: return Key{"delete", modifiers};
        case 57350: return Key{"left", modifiers};
        case 57351: return Key{"right", modifiers};
        case 57352: return Key{"up", modifiers};
        case 57353: return Key{"down", modifiers};
        case 57354: return Key{"pgup", modifiers};
        case 57355: return Key{"pgdn", modifiers};
        case 57356: return Key{"home", modifiers};
        case 57357: return Key{"end", modifiers};
        default: break;
    }
    if (point >= 57364U && point <= 57383U) {
        return Key{"f" + std::to_string(point - 57363U), modifiers};
    }
    if ((point >= 0xe000U && point <= 0xf8ffU) || !printable(point)) {
        return std::nullopt;
    }
    return printable_key(utf8(point), modifiers);
}

std::optional<Key> csi_unicode(const std::string_view parameters) {
    const auto parts = fields(parameters, ';', 2);
    if (!parts) return std::nullopt;
    const auto codes = fields(parts->front(), ':', 2);
    if (!codes) return std::nullopt;
    const auto primary = number(codes->front());
    if (!primary) return std::nullopt;
    unsigned int modifiers = 0;
    if (parts->size() == 2U) {
        const auto attributes = fields(parts->back(), ':', 2);
        if (!attributes) return std::nullopt;
        const auto bits = modifier_bits(attributes->front());
        if (!bits) return std::nullopt;
        modifiers = *bits;
        if (attributes->size() == 2U) {
            const auto event_type = number(attributes->back());
            if (!event_type || (*event_type != 1U && *event_type != 2U)) {
                return std::nullopt;
            }
        }
    }
    auto key = protocol_point(*primary, modifiers);
    if (!key) return std::nullopt;
    if (codes->size() == 2U) {
        const auto shifted = number(codes->back());
        if (!shifted || (*shifted != 0U &&
                         (!printable(*shifted) ||
                          (*shifted >= 0xe000U && *shifted <= 0xf8ffU)))) {
            return std::nullopt;
        }
        if (*shifted != 0U && (modifiers & shift) != 0U) {
            key = printable_key(utf8(*shifted), modifiers);
        }
    }
    return key;
}

std::optional<Key> letter_key(const char final, const unsigned int modifiers) {
    for (const auto& key : navigation) {
        if (key.final == final) return Key{std::string(key.name), modifiers};
    }
    if (final >= 'P' && final <= 'S') {
        return Key{"f" + std::to_string(final - 'P' + 1), modifiers};
    }
    return std::nullopt;
}

std::optional<unsigned int> letter_modifiers(
    const std::string_view parameters,
    const bool ss3
) {
    if (parameters.empty()) return 0U;
    const auto parts = fields(parameters, ';', 2);
    if (!parts) return std::nullopt;
    if (parts->size() == 1U) {
        if (ss3) return modifier_bits(parts->front());
        return parts->front() == "1"
            ? std::optional<unsigned int>{0U} : std::nullopt;
    }
    if (parts->front() != "1" && !parts->front().empty()) return std::nullopt;
    return modifier_bits(parts->back());
}

std::optional<Key> csi(const std::string_view body) {
    if (body.empty()) return std::nullopt;
    if (body.size() == 2U && body.front() == '[' &&
        body.back() >= 'A' && body.back() <= 'E') {
        return Key{"f" + std::to_string(body.back() - 'A' + 1)};
    }
    const auto final = body.back();
    const auto parameters = body.substr(0, body.size() - 1U);
    if (final == 'u') return csi_unicode(parameters);
    if (final == '~') {
        const auto parts = fields(parameters, ';', 3);
        if (!parts) return std::nullopt;
        const auto code = number(parts->front());
        if (!code) return std::nullopt;
        if (parts->size() == 3U) {
            if (*code != 27U) return std::nullopt;
            const auto modifiers = modifier_bits((*parts)[1]);
            const auto point = number((*parts)[2]);
            if (!modifiers || !point) return std::nullopt;
            return protocol_point(*point, *modifiers, false);
        }
        const auto modifiers = parts->size() == 1U
            ? std::optional<unsigned int>{0U} : modifier_bits(parts->back());
        if (!modifiers) return std::nullopt;
        switch (*code) {
            case 1: case 7: return Key{"home", *modifiers};
            case 4: case 8: return Key{"end", *modifiers};
            case 2: return Key{"insert", *modifiers};
            case 3: return Key{"delete", *modifiers};
            case 5: return Key{"pgup", *modifiers};
            case 6: return Key{"pgdn", *modifiers};
            default: break;
        }
        for (std::size_t index = 0; index < function_codes.size(); ++index) {
            if (function_codes[index] == *code) {
                return Key{"f" + std::to_string(index + 1U), *modifiers};
            }
        }
        return std::nullopt;
    }
    const auto modifiers = letter_modifiers(parameters, false);
    if (!modifiers) return std::nullopt;
    if (final == 'Z') return Key{"tab", *modifiers | shift};
    // Bare CSI P/Q/R/S are terminal controls, not unambiguous function keys.
    if (final >= 'P' && final <= 'S' && parameters.empty()) return std::nullopt;
    return letter_key(final, *modifiers);
}

std::optional<Key> ss3(const std::string_view body) {
    if (body.empty()) return std::nullopt;
    const auto modifiers = letter_modifiers(body.substr(0, body.size() - 1U), true);
    if (!modifiers) return std::nullopt;
    const auto final = body.back();
    if (auto key = letter_key(final, *modifiers)) return key;
    switch (final) {
        case 'M': return Key{"enter", *modifiers};
        case 'k': return Key{"kpplus", *modifiers};
        case 'm': return Key{"kpminus", *modifiers};
        case 'j': return Key{"kpasterisk", *modifiers};
        case 'o': return Key{"kpslash", *modifiers};
        default: return std::nullopt;
    }
}

std::optional<Key> special(
    const std::string_view input,
    const bool allow_alt_prefix = true
) {
    if (input.empty()) return std::nullopt;
    if (input.size() == 1U) {
        return legacy_control(static_cast<unsigned char>(input.front()));
    }
    if (input.front() != '\x1b') return std::nullopt;
    const auto rest = input.substr(1);
    if (rest.front() == '[') return csi(rest.substr(1));
    if (rest.front() == 'O') return ss3(rest.substr(1));
    if (!allow_alt_prefix) return std::nullopt;
    if (rest.front() == '\x1b') {
        auto key = special(rest, false);
        if (key) key->modifiers |= alt;
        return key;
    }
    if (rest.front() == ']' || rest.front() == 'P' || rest.front() == 'N' ||
        rest.front() == 'X' || rest.front() == '^' || rest.front() == '_' ||
        rest.front() == '\\') {
        return std::nullopt;
    }
    if (rest.size() == 1U) {
        if (auto key = legacy_control(static_cast<unsigned char>(rest.front()))) {
            key->modifiers |= alt;
            return key;
        }
    }
    return printable_key(rest, alt);
}

std::optional<unsigned int> character_point(const std::string_view name) {
    for (const auto& entry : characters) {
        if (name == entry.name) {
            return static_cast<unsigned char>(entry.character);
        }
    }
    const auto point = utf8_scalar(name);
    return point && printable(*point) ? point : std::nullopt;
}

std::optional<unsigned int> control_byte(const unsigned int point) noexcept {
    if (point >= 'a' && point <= 'z') return point - 'a' + 1U;
    if (point == '@' || point == ' ') return 0U;
    if (point >= '[' && point <= '_') return point - 64U;
    if (point == '?') return 127U;
    return std::nullopt;
}

ftxui::Event enhanced(const unsigned int point, const unsigned int modifiers) {
    if (point >= 0xe000U && point <= 0xf8ffU) {
        return ftxui::Event::Special(
            "\x1b[27;" + std::to_string(modifiers + 1U) + ";" +
            std::to_string(point) + "~"
        );
    }
    return ftxui::Event::Special(
        "\x1b[" + std::to_string(point) + ";" +
        std::to_string(modifiers + 1U) + "u"
    );
}

ftxui::Event tilde_event(const unsigned int code, const unsigned int modifiers) {
    std::string sequence = "\x1b[" + std::to_string(code);
    if (modifiers != 0U) sequence += ";" + std::to_string(modifiers + 1U);
    return ftxui::Event::Special(sequence + "~");
}

ftxui::Event letter_event(const char final, const unsigned int modifiers) {
    std::string sequence{"\x1b["};
    if (modifiers != 0U) sequence += "1;" + std::to_string(modifiers + 1U);
    sequence += final;
    return ftxui::Event::Special(sequence);
}

bool raw_alt_safe(const unsigned int point) noexcept {
    if (point > 126U) return false;
    constexpr std::string_view introducers{" [OP]NX^_\\#%()*+"};
    return introducers.find(static_cast<char>(point)) == std::string_view::npos;
}

}  // namespace

std::optional<std::string> keymap_event_name(const ftxui::Event& event) {
    if (event.is_mouse() || event.is_cursor_position() || event.is_cursor_shape() ||
        event.IsTerminalNameVersion() || event.IsTerminalEmulator() ||
        event.IsTerminalCapabilities()) {
        return std::nullopt;
    }
    const auto key = event.is_character()
        ? printable_key(event.input()) : special(event.input());
    if (!key) return std::nullopt;
    return canonical(*key);
}

bool keymap_event_matches(
    const ftxui::Event& event,
    const std::string_view canonical_name
) {
    const auto observed = keymap_event_name(event);
    if (!observed) return false;
    std::string requested;
    try {
        requested = normalize_key_name(canonical_name);
    } catch (const std::invalid_argument&) {
        return false;
    }
    if (*observed == requested) return true;

    std::string_view name = requested;
    if (!name.starts_with("ctrl-")) return false;
    name.remove_prefix(5);
    const bool with_alt = name.starts_with("alt-");
    if (with_alt) name.remove_prefix(4);
    const auto point = character_point(name);
    if (!point) return false;
    const auto byte = control_byte(*point);
    if (!byte) return false;
    std::string legacy;
    if (with_alt) legacy += '\x1b';
    legacy += static_cast<char>(*byte);
    if (event.input() == legacy) return true;
    if (with_alt) return false;

    // FTXUI rewrites whole single-byte events, not bytes within ESC sequences.
    if (*byte == 13U) return event.input() == ftxui::Event::Return.input();
    if (*byte == 8U) return event.input() == ftxui::Event::Backspace.input();
    if (*byte == 0U) return event.input() == "\x1b[32;5u";
    return false;
}

ftxui::Event keymap_named_event(const std::string_view canonical_name) {
    const auto normalized = normalize_key_name(canonical_name);
    std::string_view name = normalized;
    unsigned int modifiers = 0;
    if (name.starts_with("ctrl-")) {
        modifiers |= control;
        name.remove_prefix(5);
    }
    if (name.starts_with("alt-")) {
        modifiers |= alt;
        name.remove_prefix(4);
    }
    if (name.starts_with("shift-")) {
        modifiers |= shift;
        name.remove_prefix(6);
    }
    for (const auto& key : navigation) {
        if (name == key.name) return letter_event(key.final, modifiers);
    }
    if (name.size() > 1U && name.front() == 'f') {
        const auto function = number(name.substr(1));
        if (function && *function >= 1U && *function <= function_codes.size()) {
            if (*function <= 4U) {
                const auto final = static_cast<char>('P' + *function - 1U);
                if (modifiers != 0U) return letter_event(final, modifiers);
                return ftxui::Event::Special(std::string("\x1bO") + final);
            }
            return tilde_event(function_codes[*function - 1U], modifiers);
        }
    }
    for (const auto& [key, code] :
         std::array<std::pair<std::string_view, unsigned int>, 4>{{
             {"insert", 2U}, {"delete", 3U}, {"pgup", 5U}, {"pgdn", 6U}
         }}) {
        if (name == key) return tilde_event(code, modifiers);
    }
    for (const auto& [key, final] :
         std::array<std::pair<std::string_view, char>, 4>{{
             {"kpplus", 'k'}, {"kpminus", 'm'}, {"kpasterisk", 'j'}, {"kpslash", 'o'}
         }}) {
        if (name == key) {
            std::string sequence{"\x1bO"};
            if (modifiers != 0U) sequence += "1;" + std::to_string(modifiers + 1U);
            sequence += final;
            return ftxui::Event::Special(sequence);
        }
    }
    for (const auto& [key, point] :
         std::array<std::pair<std::string_view, unsigned int>, 4>{{
             {"tab", 9U}, {"enter", 13U}, {"esc", 27U}, {"backspace", 127U}
         }}) {
        if (name != key) continue;
        if (modifiers == 0U) {
            if (name == "tab") return ftxui::Event::Tab;
            if (name == "enter") return ftxui::Event::Return;
            if (name == "esc") return ftxui::Event::Escape;
            return ftxui::Event::Backspace;
        }
        if (name == "tab" && modifiers == shift) return ftxui::Event::TabReverse;
        if (modifiers == alt) {
            return ftxui::Event::Special(std::string("\x1b") + static_cast<char>(point));
        }
        return enhanced(point, modifiers);
    }
    if (const auto point = character_point(name)) {
        if (modifiers == 0U) return ftxui::Event::Character(utf8(*point));
        if (modifiers == alt && raw_alt_safe(*point)) {
            return ftxui::Event::Special("\x1b" + utf8(*point));
        }
        if ((modifiers == control || modifiers == (control | alt)) &&
            *point >= 'a' && *point <= 'z' &&
            *point != 'h' && *point != 'i' && *point != 'j' && *point != 'm') {
            std::string sequence;
            if ((modifiers & alt) != 0U) sequence += '\x1b';
            sequence += static_cast<char>(*point - 'a' + 1U);
            return ftxui::Event::Special(sequence);
        }
        if ((modifiers == control || modifiers == (control | alt)) &&
            *point >= '\\' && *point <= '_') {
            std::string sequence;
            if ((modifiers & alt) != 0U) sequence += '\x1b';
            sequence += static_cast<char>(*point - 64U);
            return ftxui::Event::Special(sequence);
        }
        return enhanced(*point, modifiers);
    }
    throw std::invalid_argument("unsupported terminal key name: " + normalized);
}

}  // namespace mcpp
