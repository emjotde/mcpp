#include "mcpp/keymap.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <utility>

namespace mcpp {
namespace {

struct ContextName {
    KeymapContext context;
    std::string_view name;
};

constexpr ContextName context_names[]{
    {KeymapContext::filemanager, "filemanager"},
    {KeymapContext::filemanager_xmap, "filemanager:xmap"},
    {KeymapContext::panel, "panel"},
    {KeymapContext::dialog, "dialog"},
    {KeymapContext::menu, "menu"},
    {KeymapContext::input, "input"},
    {KeymapContext::listbox, "listbox"},
    {KeymapContext::radio, "radio"},
    {KeymapContext::tree, "tree"},
    {KeymapContext::help, "help"},
    {KeymapContext::chattr, "chattr"},
    {KeymapContext::editor, "editor"},
    {KeymapContext::editor_xmap, "editor:xmap"},
    {KeymapContext::viewer, "viewer"},
    {KeymapContext::viewer_hex, "viewer:hex"},
    {KeymapContext::diffviewer, "diffviewer"},
};

// Public action identifiers, not implementations or default key assignments.
constexpr std::string_view action_names[]{
    "Back", "BackSpace", "BlockSave", "BlockShiftLeft", "BlockShiftRight",
    "Bookmark", "BookmarkFlush", "BookmarkGoto", "BookmarkNext", "BookmarkPrev",
    "Bottom", "BottomOnScreen", "Cancel", "CdChild", "CdParent", "CdParentSmart",
    "CdQuick", "ChangeAttributes", "ChangeMode", "ChangeOwn", "ChangePanel",
    "Clear", "Close", "CompareDirs", "CompareFiles", "Complete", "ConnectFish",
    "ConnectFtp", "ConnectShell", "Copy", "CopySingle", "Cut", "CycleListingFormat",
    "Date", "Delete", "DeleteLine", "DeleteSingle", "DeleteToEnd", "DeleteToHome",
    "DeleteToWordBegin", "DeleteToWordEnd", "DirSize", "Down", "Edit",
    "EditExtensionsFile", "EditFile", "EditFileHighlightFile", "EditForceInternal",
    "EditNew", "EditOther", "EditorViewerHistory", "End", "Enter", "ExtendedKeyMap",
    "ExternalCommand", "ExternalPanelize", "FileNext", "FilePrev", "Filter", "Find",
    "Forget", "Goto", "HalfPageDown", "HalfPageUp", "Help", "HexEditMode", "HexMode",
    "History", "HistoryNext", "HistoryPrev", "Home", "HotList", "HotListAdd",
    "HunkNext", "HunkPrev", "Ignore", "Index", "InsertFile", "InsertLiteral",
    "InsertOverwrite", "Jobs", "LearnKeys", "Left", "LeftQuick", "Link", "LinkNext",
    "LinkPrev", "LinkSymbolic", "LinkSymbolicEdit", "LinkSymbolicRelative",
    "MacroDelete", "MacroStartRecord", "MacroStartStopRecord", "MacroStopRecord",
    "MagicMode", "Mail", "MakeDir", "Mark", "MarkAll", "MarkAndDown", "MarkColumn",
    "MarkColumnDown", "MarkColumnLeft", "MarkColumnPageDown", "MarkColumnPageUp",
    "MarkColumnParagraphDown", "MarkColumnParagraphUp", "MarkColumnRight",
    "MarkColumnScrollDown", "MarkColumnScrollUp", "MarkColumnUp", "MarkDown",
    "MarkLeft", "MarkLine", "MarkPageDown", "MarkPageUp", "MarkParagraphDown",
    "MarkParagraphUp", "MarkRight", "MarkScrollDown", "MarkScrollUp", "MarkToEnd",
    "MarkToFileBegin", "MarkToFileEnd", "MarkToHome", "MarkToPageBegin",
    "MarkToPageEnd", "MarkToWordBegin", "MarkToWordEnd", "MarkUp", "MarkWord",
    "MatchBracket", "Menu", "MenuLastSelected", "Merge", "MergeOther",
    "MiddleOnScreen", "Move", "MoveSingle", "NodeNext", "NodePrev", "NroffMode",
    "Ok", "Options", "OptionsAppearance", "OptionsConfirm", "OptionsDisplayBits",
    "OptionsLayout", "OptionsPanel", "OptionsSaveMode", "OptionsVfs", "PageDown",
    "PageUp", "PanelInfo", "PanelListing", "PanelOtherCd", "PanelOtherCdLink",
    "PanelOtherSync", "PanelQuickView", "Panelize", "ParagraphDown",
    "ParagraphFormat", "ParagraphUp", "Paste", "PutCurrentFullSelected",
    "PutCurrentLink", "PutCurrentPath", "PutCurrentSelected", "PutCurrentTagged",
    "PutOtherLink", "PutOtherPath", "PutOtherTagged", "Quit", "QuitQuiet", "Redo",
    "Refresh", "Remove", "RepeatStartStopRecord", "Replace", "ReplaceContinue",
    "Reread", "Return", "Right", "RightQuick", "Ruler", "Save", "SaveAs",
    "SaveSetup", "ScreenList", "ScreenNext", "ScreenPrev", "ScrollDown",
    "ScrollEnd", "ScrollHome", "ScrollLeft", "ScrollRight", "ScrollUp", "Search",
    "SearchBackward", "SearchBackwardContinue", "SearchContinue", "SearchForward",
    "SearchForwardContinue", "SearchOppositeContinue", "Select", "SelectCodepage",
    "SelectExt", "SelectInvert", "SetupListingFormat", "Shell", "ShowHidden",
    "ShowMargin", "ShowNumbers", "ShowSymbols", "ShowTabTws", "Sort", "SortByExt",
    "SortByMTime", "SortByName", "SortBySize", "SortByVersion", "SortNext",
    "SortPrev", "SortReverse", "SpellCheck", "SpellCheckCurrentWord",
    "SpellCheckSelectLang", "SplitEqual", "SplitFull", "SplitLess", "SplitMore",
    "SplitVertHoriz", "Store", "Suspend", "Swap", "SyntaxChoose", "SyntaxOnOff",
    "Tab", "Tab2", "Tab3", "Tab4", "Tab8", "ToggleNavigation", "Top",
    "TopOnScreen", "Undo", "Unmark", "Unselect", "Up", "UserMenu", "VfsList",
    "View", "ViewFile", "ViewFiltered", "ViewRaw", "WindowFullscreen",
    "WindowList", "WindowMove", "WindowNext", "WindowPrev", "WindowResize",
    "WordLeft", "WordRight", "WrapMode", "Yank",
};

struct CharacterName {
    char character;
    std::string_view name;
};

constexpr CharacterName character_names[]{
    {' ', "space"}, {'!', "exclamation"}, {'#', "hash"}, {'$', "dollar"},
    {'%', "percent"}, {'&', "ampersand"}, {'\'', "prime"},
    {'(', "lparenthesis"}, {')', "rparenthesis"}, {'*', "asterisk"},
    {'+', "plus"}, {',', "comma"}, {'-', "minus"}, {'.', "dot"},
    {'/', "slash"}, {':', "colon"}, {';', "semicolon"}, {'<', "lt"},
    {'=', "equal"}, {'>', "gt"}, {'?', "question"}, {'@', "at"},
    {'[', "lbracket"}, {'\\', "backslash"}, {']', "rbracket"},
    {'^', "caret"}, {'_', "underline"}, {'{', "lbrace"}, {'|', "pipe"},
    {'}', "rbrace"},
};

constexpr std::string_view special_keys[]{
    "up", "down", "left", "right", "home", "end", "pgup", "pgdn",
    "tab", "enter", "backspace", "insert", "delete", "esc",
    "a1", "a3", "b2", "c1", "c3", "kpplus", "kpminus", "kpasterisk", "kpslash",
};

struct KeyAlias {
    std::string_view name;
    std::string_view key;
};

constexpr KeyAlias native_aliases[]{
    {"escape", "esc"}, {"return", "enter"}, {"bs", "backspace"},
    {"ins", "insert"}, {"del", "delete"}, {"complete", "alt-tab"},
};

constexpr KeyAlias event_aliases[]{
    {"pageup", "pgup"}, {"pagedown", "pgdn"},
    {"arrowup", "up"}, {"arrowdown", "down"},
    {"arrowleft", "left"}, {"arrowright", "right"},
    {"backtab", "shift-tab"}, {"tabreverse", "shift-tab"},
    {"completion", "alt-tab"}, {"keypad /", "kpslash"},
    {"keypad *", "kpasterisk"}, {"keypad -", "kpminus"},
    {"keypad +", "kpplus"},
};

char ascii_lower(const char character) noexcept {
    if (character >= 'A' && character <= 'Z') {
        return static_cast<char>(character - 'A' + 'a');
    }
    return character;
}

bool equal_ascii_case(
    const std::string_view left,
    const std::string_view right
) noexcept {
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin(),
                      [](const char first, const char second) {
                          return ascii_lower(first) == ascii_lower(second);
                      });
}

bool ascii_space(const char character) noexcept {
    return character == ' ' || character == '\t' || character == '\r' ||
           character == '\n' || character == '\f' || character == '\v';
}

std::size_t leading_space(const std::string_view text) noexcept {
    std::size_t index = 0;
    while (index < text.size() && ascii_space(text[index])) {
        ++index;
    }
    return index;
}

std::string_view trim(std::string_view text) noexcept {
    text.remove_prefix(leading_space(text));
    while (!text.empty() && ascii_space(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

std::string quoted(const std::string_view text) {
    constexpr std::string_view hex = "0123456789abcdef";
    std::string result{"'"};
    for (const auto character : text) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte < 32U || byte == 127U) {
            result += "\\x";
            result += hex[byte >> 4U];
            result += hex[byte & 15U];
        } else {
            if (character == '\\' || character == '\'') {
                result += '\\';
            }
            result += character;
        }
    }
    return result + "'";
}

struct Scalar {
    unsigned int value;
    std::size_t length;
};

std::optional<Scalar> scalar_at(
    const std::string_view text,
    const std::size_t offset
) noexcept {
    const auto first = static_cast<unsigned char>(text[offset]);
    if (first < 128U) {
        return Scalar{first, 1};
    }
    std::size_t length = 0;
    unsigned int value = 0;
    unsigned int minimum = 0;
    if (first >= 0xc2U && first <= 0xdfU) {
        length = 2;
        value = first & 0x1fU;
        minimum = 0x80U;
    } else if (first >= 0xe0U && first <= 0xefU) {
        length = 3;
        value = first & 0x0fU;
        minimum = 0x800U;
    } else if (first >= 0xf0U && first <= 0xf4U) {
        length = 4;
        value = first & 0x07U;
        minimum = 0x10000U;
    } else {
        return std::nullopt;
    }
    if (length > text.size() - offset) {
        return std::nullopt;
    }
    for (std::size_t index = 1; index < length; ++index) {
        const auto next = static_cast<unsigned char>(text[offset + index]);
        if (next < 0x80U || next > 0xbfU) {
            return std::nullopt;
        }
        value = (value << 6U) | (next & 0x3fU);
    }
    if (value < minimum || value > 0x10ffffU ||
        (value >= 0xd800U && value <= 0xdfffU)) {
        return std::nullopt;
    }
    return Scalar{value, length};
}

std::string diagnostic_message(const KeymapDiagnostic& diagnostic) {
    std::string result = diagnostic.source;
    if (diagnostic.line != 0U) {
        result += ":" + std::to_string(diagnostic.line);
        if (diagnostic.column != 0U) {
            result += ":" + std::to_string(diagnostic.column);
        }
    }
    return result + ": " + diagnostic.message;
}

[[noreturn]] void fail(
    const std::string& source,
    const std::size_t line,
    const std::size_t column,
    std::string message
) {
    throw KeymapError({source, line, column, std::move(message)});
}

void validate_line(
    const std::string_view line,
    const std::string& source,
    const std::size_t number
) {
    std::size_t index = 0;
    while (index < line.size()) {
        const auto scalar = scalar_at(line, index);
        if (!scalar) {
            fail(source, number, index + 1U, "invalid UTF-8 in keymap");
        }
        if (scalar->value == 0U) {
            fail(source, number, index + 1U, "NUL byte in keymap");
        }
        if ((scalar->value < 32U && line[index] != '\t' && line[index] != '\r') ||
            scalar->value == 127U) {
            fail(source, number, index + 1U, "control character in keymap");
        }
        index += scalar->length;
    }
}

struct Modifiers {
    bool control{false};
    bool alt{false};
    bool shift{false};
};

bool* modifier(
    const std::string_view name,
    Modifiers& modifiers
) noexcept {
    if (equal_ascii_case(name, "ctrl") || equal_ascii_case(name, "control")) {
        return &modifiers.control;
    }
    if (equal_ascii_case(name, "alt") || equal_ascii_case(name, "meta")) {
        return &modifiers.alt;
    }
    if (equal_ascii_case(name, "shift")) {
        return &modifiers.shift;
    }
    return nullptr;
}

std::string normalize_key(const std::string_view name, const bool event_name) {
    if (name == " ") {
        return "space";
    }
    if (name == "\t") {
        return "tab";
    }
    if (name == "\n" || name == "\r") {
        return "enter";
    }
    if (name.find_first_of("\r\n") != std::string_view::npos) {
        throw std::invalid_argument("unknown or malformed key " + quoted(name));
    }
    auto remaining = trim(name);
    if (remaining.empty()) {
        throw std::invalid_argument("empty key name");
    }

    Modifiers modifiers;
    while (true) {
        const auto separator = remaining.find_first_of("-+ \t");
        const auto token = remaining.substr(0, separator);
        auto* flag = modifier(token, modifiers);
        if (flag == nullptr) {
            break;
        }
        *flag = true;
        if (separator == std::string_view::npos) {
            throw std::invalid_argument("modifier without a key in " + quoted(name));
        }
        remaining.remove_prefix(separator);
        while (!remaining.empty() &&
               (remaining.front() == '-' || remaining.front() == '+' ||
                remaining.front() == ' ' ||
                remaining.front() == '\t')) {
            remaining.remove_prefix(1);
        }
        if (remaining.empty()) {
            throw std::invalid_argument("modifier without a key in " + quoted(name));
        }
    }

    const auto expand_alias = [&](const auto& aliases) -> std::optional<std::string> {
        for (const auto& alias : aliases) {
            if (equal_ascii_case(remaining, alias.name)) {
                return normalize_key(alias.key, false);
            }
        }
        return std::nullopt;
    };
    std::optional<std::string> expanded;
    if (event_name) {
        expanded = expand_alias(event_aliases);
    }
    if (!expanded) {
        expanded = expand_alias(native_aliases);
    }
    if (expanded) {
        if (expanded->starts_with("alt-")) {
            modifiers.alt = true;
            expanded->erase(0, 4);
        } else if (expanded->starts_with("shift-")) {
            modifiers.shift = true;
            expanded->erase(0, 6);
        }
        std::string combined;
        if (modifiers.control) combined += "ctrl-";
        if (modifiers.alt) combined += "alt-";
        if (modifiers.shift) combined += "shift-";
        combined += *expanded;
        return normalize_key(combined, false);
    }

    std::string key;
    std::optional<unsigned int> character;
    for (const auto& named : character_names) {
        if (equal_ascii_case(remaining, named.name)) {
            key = named.name;
            character = static_cast<unsigned char>(named.character);
            break;
        }
    }
    if (key.empty()) {
        for (const auto special : special_keys) {
            if (equal_ascii_case(remaining, special)) {
                key = special;
                break;
            }
        }
    }
    if (key.empty() && (remaining.front() == 'f' || remaining.front() == 'F') &&
        remaining.size() > 1U) {
        unsigned int number = 0;
        const auto parsed = std::from_chars(
            remaining.data() + 1,
            remaining.data() + remaining.size(),
            number
        );
        if (parsed.ec == std::errc{} &&
            parsed.ptr == remaining.data() + remaining.size() && number <= 20U &&
            (remaining.size() == 2U || remaining[1] != '0')) {
            key = "f" + std::to_string(number);
        }
    }
    if (key.empty()) {
        const auto scalar = scalar_at(remaining, 0);
        if (!scalar || scalar->length != remaining.size() ||
            scalar->value < 33U ||
            (scalar->value >= 127U && scalar->value <= 159U)) {
            throw std::invalid_argument("unknown or malformed key " + quoted(name));
        }
        if (!event_name && (remaining == "+" || remaining == "-")) {
            throw std::invalid_argument("use plus/minus for literal '+'/'-' keys");
        }
        key = remaining;
        character = scalar->value;
        for (const auto& named : character_names) {
            if (scalar->value == static_cast<unsigned char>(named.character)) {
                key = named.name;
                break;
            }
        }
    }
    if (character && *character < 128U) {
        if (*character >= 'A' && *character <= 'Z' && modifiers.control) {
            key.assign(1, ascii_lower(static_cast<char>(*character)));
        } else if (*character >= 'a' && *character <= 'z' && modifiers.shift &&
                   !modifiers.control) {
            key.assign(1, static_cast<char>(*character - 'a' + 'A'));
        }
        modifiers.shift = false;
    }
    std::string result;
    if (modifiers.control) result += "ctrl-";
    if (modifiers.alt) result += "alt-";
    if (modifiers.shift) result += "shift-";
    return result + key;
}

std::string decode_key(
    const std::string_view token,
    const std::string& source,
    const std::size_t line,
    const std::size_t column
) {
    std::string result;
    for (std::size_t index = 0; index < token.size(); ++index) {
        if (token[index] != '\\') {
            result += token[index];
            continue;
        }
        const auto escape_column = column + index;
        ++index;
        if (index == token.size()) {
            fail(source, line, escape_column, "unfinished keymap escape");
        }
        switch (token[index]) {
            case 's': result += ' '; break;
            case 't': result += '\t'; break;
            case 'n': result += '\n'; break;
            case 'r': result += '\r'; break;
            case '\\': result += '\\'; break;
            case ';': result += ';'; break;
            default:
                fail(source, line, escape_column,
                     "unknown keymap escape " + quoted(token.substr(index - 1U, 2)));
        }
    }
    return result;
}

std::vector<std::string> parse_keys(
    const std::string_view value,
    const std::string& source,
    const std::size_t line,
    const std::size_t column
) {
    std::vector<std::string> keys;
    const auto append = [&](const std::size_t begin, const std::size_t end) {
        const auto raw = value.substr(begin, end - begin);
        const auto token = trim(raw);
        // Empty list elements are valid INI lists, not requests for defaults.
        if (token.empty()) {
            return;
        }
        const auto token_column = column + begin + leading_space(raw);
        const auto decoded = decode_key(token, source, line, token_column);
        std::string key;
        try {
            key = normalize_key_name(decoded);
        } catch (const std::invalid_argument& error) {
            fail(source, line, token_column, error.what());
        }
        if (std::ranges::find(keys, key) == keys.end()) {
            keys.push_back(std::move(key));
        }
    };
    std::size_t begin = 0;
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] == '\\') {
            if (index + 1U < value.size()) {
                ++index;
            }
        } else if (value[index] == ';') {
            append(begin, index);
            begin = index + 1U;
        }
    }
    append(begin, value.size());
    return keys;
}

void replace_action(Keymap::Section& section, KeymapAction action) {
    const auto found = std::ranges::find(section, action.name, &KeymapAction::name);
    if (found == section.end()) {
        section.push_back(std::move(action));
    } else {
        *found = std::move(action);
    }
}

std::string_view require_action(const std::string_view name) {
    const auto canonical = canonical_keymap_action(name);
    if (!canonical) {
        throw std::invalid_argument("unknown keymap action " + quoted(name));
    }
    return *canonical;
}

}  // namespace

std::optional<KeymapContext> parse_keymap_context(
    const std::string_view name
) noexcept {
    for (const auto& entry : context_names) {
        if (entry.name == name) {
            return entry.context;
        }
    }
    return std::nullopt;
}

std::string_view keymap_context_name(const KeymapContext context) {
    for (const auto& entry : context_names) {
        if (entry.context == context) {
            return entry.name;
        }
    }
    throw std::invalid_argument("unknown keymap context");
}

std::optional<std::string_view> canonical_keymap_action(
    const std::string_view name
) noexcept {
    for (const auto action : action_names) {
        if (equal_ascii_case(name, action)) {
            return action;
        }
    }
    return std::nullopt;
}

std::string normalize_key_name(const std::string_view name) {
    return normalize_key(name, false);
}

std::string normalize_event_key_name(const std::string_view name) {
    return normalize_key(name, true);
}

KeymapError::KeymapError(KeymapDiagnostic diagnostic)
    : std::runtime_error(diagnostic_message(diagnostic)),
      diagnostic_(std::move(diagnostic)) {}

const KeymapDiagnostic& KeymapError::diagnostic() const noexcept {
    return diagnostic_;
}

Keymap Keymap::parse(
    const std::string_view resource,
    std::string source
) {
    Keymap result;
    std::optional<KeymapContext> context;
    std::size_t begin = 0;
    std::size_t number = 0;
    while (begin < resource.size()) {
        ++number;
        const auto end = resource.find('\n', begin);
        const auto length = end == std::string_view::npos
            ? resource.size() - begin : end - begin;
        const auto line = resource.substr(begin, length);
        validate_line(line, source, number);
        if (number == 1U && line.starts_with("\xef\xbb\xbf")) {
            fail(source, number, 1, "UTF-8 BOM is not supported in MC keymaps");
        }
        const auto text = trim(line);
        const auto start_column = leading_space(line) + 1U;
        if (!text.empty() && text.front() != '#') {
            if (text.front() == '[') {
                if (text.size() < 3U || text.back() != ']' ||
                    text.find(']') != text.size() - 1U ||
                    text.find('[', 1) != std::string_view::npos) {
                    fail(source, number, start_column, "malformed keymap section");
                }
                const auto name = text.substr(1, text.size() - 2U);
                context = parse_keymap_context(name);
                if (!context) {
                    fail(source, number, start_column + 1U,
                         "unknown keymap section " + quoted(name));
                }
                result.sections_.try_emplace(*context);
            } else {
                if (!context) {
                    fail(source, number, start_column,
                         "keymap action appears before any section");
                }
                const auto equals = text.find('=');
                if (equals == std::string_view::npos) {
                    fail(source, number, start_column,
                         "expected Action = key; key");
                }
                const auto name = trim(text.substr(0, equals));
                if (name.empty()) {
                    fail(source, number, start_column, "empty keymap action name");
                }
                const auto action = canonical_keymap_action(name);
                if (!action) {
                    fail(source, number, start_column,
                         "unknown keymap action " + quoted(name));
                }
                const auto value = text.substr(equals + 1U);
                auto keys = parse_keys(
                    value, source, number, start_column + equals + 1U
                );
                replace_action(
                    result.sections_.at(*context),
                    {std::string(name), *action, std::move(keys)}
                );
            }
        }
        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1U;
    }
    return result;
}

Keymap Keymap::load(const std::filesystem::path& path) {
    const auto source = path.string();
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        fail(source, 0, 0, "cannot open keymap");
    }
    std::string resource;
    std::string line;
    while (std::getline(input, line)) {
        resource += line;
        resource += '\n';
    }
    if (input.bad() || !input.eof()) {
        fail(source, 0, 0, "cannot read keymap");
    }
    return parse(resource, source);
}

void Keymap::overlay(const Keymap& other) {
    auto combined = sections_;
    for (const auto& [context, actions] : other.sections_) {
        auto& target = combined[context];
        for (const auto& action : actions) {
            replace_action(target, action);
        }
    }
    sections_.swap(combined);
}

void Keymap::overlay(
    const std::string_view resource,
    std::string source
) {
    overlay(parse(resource, std::move(source)));
}

void Keymap::overlay_file(const std::filesystem::path& path) {
    overlay(load(path));
}

const Keymap::Sections& Keymap::sections() const noexcept {
    return sections_;
}

bool Keymap::contains_action(
    const KeymapContext context,
    const std::string_view action
) const {
    const auto canonical = require_action(action);
    const auto found = sections_.find(context);
    return found != sections_.end() &&
           std::ranges::any_of(found->second, [&](const auto& definition) {
               return definition.action == canonical;
           });
}

std::vector<std::string> Keymap::bindings_for(
    const KeymapContext context,
    const std::string_view action
) const {
    const auto canonical = require_action(action);
    std::vector<std::string> result;
    if (const auto section = sections_.find(context); section != sections_.end()) {
        for (const auto& definition : section->second) {
            if (definition.action == canonical) {
                for (const auto& key : definition.keys) {
                    if (lookup_canonical(context, key) == canonical &&
                        std::ranges::find(result, key) == result.end()) {
                        result.push_back(key);
                    }
                }
            }
        }
    }
    return result;
}

std::optional<std::string_view> Keymap::lookup(
    const KeymapContext context,
    const std::string_view key
) const {
    return lookup_canonical(context, normalize_key_name(key));
}

std::optional<std::string_view> Keymap::lookup_canonical(
    const KeymapContext context,
    const std::string_view canonical_key
) const noexcept {
    if (const auto found = sections_.find(context); found != sections_.end()) {
        for (const auto& action : found->second) {
            if (std::ranges::find(action.keys, canonical_key) != action.keys.end()) {
                return action.action;
            }
        }
    }
    return std::nullopt;
}

}  // namespace mcpp
