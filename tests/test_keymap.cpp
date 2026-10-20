#include "mcpp/keymap.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using mcpp::Keymap;
using mcpp::KeymapContext;

void check(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

template <typename Function>
void invalid_key(Function&& function) {
    bool rejected = false;
    try {
        function();
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected, "invalid symbolic key/action must be rejected");
}

template <typename Function>
void resource_error(
    Function&& function,
    const std::string_view source,
    const std::size_t line,
    const std::size_t column,
    const std::string_view message
) {
    bool rejected = false;
    try {
        function();
    } catch (const mcpp::KeymapError& error) {
        const auto& diagnostic = error.diagnostic();
        check(diagnostic.source == source, "diagnostic identifies its source");
        check(diagnostic.line == line, "diagnostic identifies its line");
        check(diagnostic.column == column, "diagnostic identifies its byte column");
        check(diagnostic.message.find(message) != std::string::npos,
              "diagnostic explains the rejected resource");
        check(std::string_view(error.what()).find(source) != std::string::npos,
              "exception text includes the source");
        rejected = true;
    }
    check(rejected, "invalid resource must not silently succeed");
}

std::string snapshot(const Keymap& map) {
    std::string result;
    for (const auto& [context, actions] : map.sections()) {
        result += "[" + std::string(mcpp::keymap_context_name(context)) + "]\n";
        for (const auto& definition : actions) {
            result += definition.name + ":" + std::string(definition.action) + "=";
            for (const auto& key : definition.keys) {
                result += key + ";";
            }
            result += "\n";
        }
    }
    return result;
}

void write_file(const std::filesystem::path& path, const std::string_view contents) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << contents;
    output.close();
    check(static_cast<bool>(output), "write independent keymap fixture");
}

void test_key_names() {
    constexpr std::pair<std::string_view, std::string_view> names[]{
        {"a", "a"}, {"A", "A"}, {"shift-a", "A"}, {"SHIFT-A", "A"},
        {"alt-a", "alt-a"}, {"ALT-A", "alt-A"}, {"alt-shift-a", "alt-A"},
        {"Ctrl-X", "ctrl-x"}, {"ctrl-shift-X", "ctrl-x"},
        {"control x", "ctrl-x"}, {"meta-x", "alt-x"},
        {" \tCTRL - META - SHIFT - F4 \t", "ctrl-alt-shift-f4"},
        {"ctrl+META+shift+F4", "ctrl-alt-shift-f4"},
        {"ctrl--x", "ctrl-x"}, {"shift-ctrl-alt-Up", "ctrl-alt-shift-up"},
        {"ctrl-ctrl-x", "ctrl-x"}, {"alt-meta-x", "alt-x"},
        {"ctrl+x", "ctrl-x"}, {"control ++ x", "ctrl-x"},
        {"F0", "f0"}, {"F1", "f1"}, {"F9", "f9"}, {"F10", "f10"},
        {"F20", "f20"}, {"CTRL-F20", "ctrl-f20"},
        {"bs", "backspace"}, {"BackSpace", "backspace"},
        {"ins", "insert"}, {"DEL", "delete"}, {"Escape", "esc"},
        {"Return", "enter"}, {"complete", "alt-tab"},
        {"alt-complete", "alt-tab"}, {"control-complete", "ctrl-alt-tab"},
        {"ctrl-i", "ctrl-i"}, {"tab", "tab"},
        {"ctrl-at", "ctrl-at"}, {"ctrl-space", "ctrl-space"},
        {"shift-tab", "shift-tab"}, {"ctrl-shift-enter", "ctrl-shift-enter"},
        {"shift-{", "lbrace"}, {"shift-1", "1"}, {"ctrl-@", "ctrl-at"},
        {"!", "exclamation"}, {"#", "hash"}, {"$", "dollar"},
        {"%", "percent"}, {"&", "ampersand"}, {"'", "prime"},
        {"(", "lparenthesis"}, {")", "rparenthesis"}, {"*", "asterisk"},
        {",", "comma"}, {".", "dot"}, {"/", "slash"}, {":", "colon"},
        {";", "semicolon"}, {"<", "lt"}, {"=", "equal"}, {">", "gt"},
        {"?", "question"}, {"@", "at"}, {"[", "lbracket"},
        {"\\", "backslash"}, {"]", "rbracket"}, {"^", "caret"},
        {"_", "underline"}, {"{", "lbrace"}, {"|", "pipe"},
        {"}", "rbrace"}, {"\"", "\""}, {"`", "`"}, {"~", "~"},
        {"plus", "plus"}, {"minus", "minus"}, {" ", "space"},
        {"\t", "tab"}, {"\n", "enter"}, {"\r", "enter"},
        {"\xc3\xa9", "\xc3\xa9"}, {"\xe4\xb8\xad", "\xe4\xb8\xad"},
        {"\xf0\x9f\x8c\xb2", "\xf0\x9f\x8c\xb2"},
        {"alt-\xc3\xa9", "alt-\xc3\xa9"},
        {"shift-\xc3\xa9", "shift-\xc3\xa9"},
    };
    for (const auto& [input, canonical] : names) {
        const auto actual = mcpp::normalize_key_name(input);
        check(actual == canonical, "normalize public key spelling");
        check(mcpp::normalize_key_name(actual) == actual,
              "canonical key names normalize idempotently");
    }
    constexpr std::string_view special[]{
        "up", "down", "left", "right", "home", "end", "pgup", "pgdn",
        "tab", "enter", "backspace", "insert", "delete", "esc",
        "a1", "a3", "b2", "c1", "c3", "kpplus", "kpminus", "kpasterisk", "kpslash",
    };
    for (const auto key : special) {
        check(mcpp::normalize_key_name(key) == key, "preserve named key identity");
    }
    constexpr std::string_view invalid[]{
        "", " \t ", "ctrl", "ctrl-", "-x", "alt-shift", "ctrl|x",
        "C-x", "M-x", "S-x", "ctl-x",
        "x-ctrl", "x ctrl", "x # comment", "ctrl-x-tail",
        "f01", "f00", "f21", "f999999999999999999999999999",
        "unknown-key", "PageUp", "BackTab", "\"f1\"", "two",
        "\xc0\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xc3",
        "\xc2\x80", "e\xcc\x81",
    };
    for (const auto key : invalid) {
        invalid_key([&] { static_cast<void>(mcpp::normalize_key_name(key)); });
    }
    for (const auto key : {"+", "-"}) {
        invalid_key([&] { static_cast<void>(mcpp::normalize_key_name(key)); });
    }
    invalid_key([] {
        static_cast<void>(mcpp::normalize_key_name(std::string_view("\0", 1)));
    });
}

void test_event_names() {
    constexpr std::pair<std::string_view, std::string_view> names[]{
        {"PageUp", "pgup"}, {"PageDown", "pgdn"}, {"ArrowUp", "up"},
        {"ArrowDown", "down"}, {"ArrowLeft", "left"}, {"ArrowRight", "right"},
        {"BackTab", "shift-tab"}, {"TabReverse", "shift-tab"},
        {"Shift-BackTab", "shift-tab"}, {"Ctrl-PageUp", "ctrl-pgup"},
        {"Completion", "alt-tab"}, {"Alt-Completion", "alt-tab"},
        {"Keypad /", "kpslash"}, {"Keypad *", "kpasterisk"},
        {"Keypad -", "kpminus"}, {"Keypad +", "kpplus"},
        {"ctrl-Keypad +", "ctrl-kpplus"}, {"+", "plus"}, {"-", "minus"},
        {"F13", "f13"}, {"Home", "home"}, {"Enter", "enter"},
        {"Escape", "esc"}, {"Backspace", "backspace"}, {"Tab", "tab"},
    };
    for (const auto& [input, canonical] : names) {
        check(mcpp::normalize_event_key_name(input) == canonical,
              "normalize mcpp symbolic event labels separately");
    }
    invalid_key([] {
        static_cast<void>(mcpp::normalize_event_key_name("\x1b[A"));
    });
}

void test_resource_lists() {
    const auto map = Keymap::parse(
        "# Independently chosen bindings exercise the public file format.\r\n"
        "\t[editor]\r\n"
        "  CoPy \t= CTRL-K; Alt-Q ; ;F6;ctrl-k;\r\n"
        "InsertLiteral = \\s; \\t; \\n; \\r; \\\\; \\;;\r\n"
        "Search = #; =; \xc3\xa9; \xe4\xb8\xad\r\n"
        "Quit =\r\n"
        "Save = ; ; ;\r\n"
        "  # Only full-line hash comments are comments.\r\n",
        "independent-lists.keymap"
    );
    check(map.lookup(KeymapContext::editor, "ctrl-k") == "Copy",
          "action names are case-insensitive");
    check(map.lookup(KeymapContext::editor, "alt-q") == std::nullopt,
          "literal letter case in an alt binding is significant");
    check(map.lookup(KeymapContext::editor, "alt-Q") == "Copy",
          "uppercase literal in an alt binding is preserved");
    check(map.lookup(KeymapContext::editor, "F6") == "Copy",
          "multiple bindings include function keys");
    const auto copy = map.bindings_for(KeymapContext::editor, "cOpY");
    check(copy == std::vector<std::string>{"ctrl-k", "alt-Q", "f6"},
          "binding order is preserved and duplicate keys are not repeated");
    check(map.bindings_for(KeymapContext::editor, "InsertLiteral") ==
              std::vector<std::string>{"space", "tab", "enter", "backslash", "semicolon"},
          "all public INI list escapes are decoded before key normalization");
    for (const auto key : {"#", "=", "\xc3\xa9", "\xe4\xb8\xad"}) {
        check(map.lookup(KeymapContext::editor, key) == "Search",
              "hash/equal characters and UTF-8 literals are values, not comments");
    }
    check(map.contains_action(KeymapContext::editor, "Quit") &&
          map.contains_action(KeymapContext::editor, "Save"),
          "empty values and empty list elements are explicit definitions");
    check(map.bindings_for(KeymapContext::editor, "Quit").empty() &&
          !map.lookup(KeymapContext::editor, "f10"),
          "empty definitions never invent default bindings");
    check(map.sections().at(KeymapContext::editor).front().name == "CoPy",
          "preserve the exact INI key for override matching");
    check(Keymap::parse("").sections().empty() &&
          Keymap::parse("# empty document\n \t\n").sections().empty(),
          "an empty resource is an empty model");
    const auto empty_section = Keymap::parse("[editor]\n");
    check(empty_section.sections().contains(KeymapContext::editor) &&
          empty_section.sections().at(KeymapContext::editor).empty(),
          "an explicit empty section is not a default keymap");
}

void test_contexts_and_actions() {
    constexpr std::pair<KeymapContext, std::string_view> contexts[]{
        {KeymapContext::filemanager, "filemanager"},
        {KeymapContext::filemanager_xmap, "filemanager:xmap"},
        {KeymapContext::panel, "panel"}, {KeymapContext::dialog, "dialog"},
        {KeymapContext::menu, "menu"}, {KeymapContext::input, "input"},
        {KeymapContext::listbox, "listbox"}, {KeymapContext::radio, "radio"},
        {KeymapContext::tree, "tree"}, {KeymapContext::help, "help"},
        {KeymapContext::chattr, "chattr"}, {KeymapContext::editor, "editor"},
        {KeymapContext::editor_xmap, "editor:xmap"},
        {KeymapContext::viewer, "viewer"},
        {KeymapContext::viewer_hex, "viewer:hex"},
        {KeymapContext::diffviewer, "diffviewer"},
    };
    std::string resource;
    for (const auto& [context, name] : contexts) {
        check(mcpp::parse_keymap_context(name) == context,
              "parse all native section names");
        check(mcpp::keymap_context_name(context) == name, "format native section names");
        resource += "[" + std::string(name) + "]\nHelp=f20\n";
    }
    const auto all = Keymap::parse(resource);
    check(all.sections().size() == std::size(contexts), "retain all context sections");
    for (const auto& [context, name] : contexts) {
        static_cast<void>(name);
        check(all.lookup(context, "f20") == "Help", "contexts have their own bindings");
    }
    check(!mcpp::parse_keymap_context("Editor") &&
          !mcpp::parse_keymap_context("editor:xmap:extra") &&
          !mcpp::parse_keymap_context("viewer:ascii"),
          "section names are exact and case-sensitive");
    check(mcpp::canonical_keymap_action("backspace") == "BackSpace" &&
          mcpp::canonical_keymap_action("ExtendedKeymap") == "ExtendedKeyMap" &&
          mcpp::canonical_keymap_action("Ignore") == "Ignore" &&
          !mcpp::canonical_keymap_action("not-an-action"),
          "canonical action identifiers are separate from executable behavior");
    const auto maps = Keymap::parse(
        "[filemanager]\nExtendedKeyMap=ctrl-b\nCopy=f2\n"
        "[filemanager:xmap]\nChangeMode=z\n"
        "[editor]\nExtendedKeyMap=alt-z\nCopy=f6\n"
        "[editor:xmap]\nSave=p\n"
        "[panel]\nMark=q\n"
        "[tree]\nForget=q\n"
        "[viewer]\nWrapMode=q\n"
        "[viewer:hex]\nHexEditMode=q\n"
        "[diffviewer]\nMerge=q\n"
    );
    check(maps.lookup(KeymapContext::filemanager, "ctrl-b") == "ExtendedKeyMap" &&
          maps.lookup(KeymapContext::filemanager_xmap, "z") == "ChangeMode" &&
          !maps.lookup(KeymapContext::filemanager, "z") &&
          !maps.lookup(KeymapContext::filemanager_xmap, "f2"),
          "prefix maps neither activate themselves nor inherit base context bindings");
    check(maps.lookup(KeymapContext::editor_xmap, "p") == "Save" &&
          !maps.lookup(KeymapContext::editor, "p"),
          "editor prefix map is independent");
    constexpr std::pair<KeymapContext, std::string_view> expected[]{
        {KeymapContext::panel, "Mark"}, {KeymapContext::tree, "Forget"},
        {KeymapContext::viewer, "WrapMode"}, {KeymapContext::viewer_hex, "HexEditMode"},
        {KeymapContext::diffviewer, "Merge"},
    };
    for (const auto& [context, action] : expected) {
        check(maps.lookup(context, "q") == action,
              "the same key selects a different action in each context");
    }
    check(!maps.lookup_canonical(KeymapContext::editor, "F6") &&
          maps.lookup_canonical(KeymapContext::editor, "f6") == "Copy",
          "canonical lookup is deliberately exact");
    invalid_key([&] { static_cast<void>(maps.lookup(KeymapContext::editor, "wrong-key")); });
    invalid_key([&] { static_cast<void>(maps.contains_action(KeymapContext::editor, "Unknown")); });
    invalid_key([&] { static_cast<void>(maps.bindings_for(KeymapContext::editor, "Unknown")); });
}

void test_overrides() {
    auto map = Keymap::parse(
        "[editor]\nCopy=f2;f6\nQuit=f2;f9\nCopy=f3;f2\n"
        "[editor]\nHelp=f3\ncopy=f8\n"
    );
    check(map.sections().at(KeymapContext::editor).size() == 4U,
          "exact duplicate action keys replace rather than append");
    check(map.lookup(KeymapContext::editor, "f2") == "Copy" &&
          map.lookup(KeymapContext::editor, "f3") == "Copy" &&
          !map.lookup(KeymapContext::editor, "f6"),
          "replacing a list retains its first declaration position");
    check(map.bindings_for(KeymapContext::editor, "Help").empty(),
          "shadowed keys are not reported as effective shortcuts");
    check(map.lookup(KeymapContext::editor, "f8") == "Copy" &&
          map.sections().at(KeymapContext::editor).back().name == "copy",
          "case variants coexist as distinct INI definitions");
    map.overlay("[editor]\nCopy=\nQuit=f3\nSave=f20\n[viewer]\nQuit=ctrl-c\n");
    check(!map.lookup(KeymapContext::editor, "f2") &&
          map.lookup(KeymapContext::editor, "f3") == "Quit" &&
          map.lookup(KeymapContext::editor, "f8") == "Copy",
          "overlay removes only the exact-spelling list and retains other definitions");
    check(map.lookup(KeymapContext::editor, "f20") == "Save" &&
          map.lookup(KeymapContext::viewer, "ctrl-c") == "Quit",
          "overlay appends new actions and contexts");
    const auto before = snapshot(map);
    map.overlay(map);
    check(snapshot(map) == before, "self overlay is safe and idempotent");
    resource_error(
        [&] { map.overlay("[editor]\nSave=f1\n[unknown]\n", "bad-overlay"); },
        "bad-overlay", 3, 2, "unknown keymap section"
    );
    check(snapshot(map) == before, "failed overlays never publish partial changes");
    map.overlay(Keymap::parse("[editor]\nHelp=f20\n"));
    check(map.lookup(KeymapContext::editor, "f20") == "Help",
          "the earlier action wins even when a later action is updated more recently");
    const auto ignore = Keymap::parse("[filemanager]\nIgnore=alt-j\n");
    check(ignore.lookup(KeymapContext::filemanager, "alt-j") == "Ignore",
          "Ignore is an explicit action, not a missing lookup");
}

void test_diagnostics() {
    struct Failure {
        std::string_view text;
        std::size_t line;
        std::size_t column;
        std::string_view message;
    };
    constexpr Failure failures[]{
        {"Quit=f3\n", 1, 1, "before any section"},
        {"[editor\n", 1, 1, "malformed"},
        {"[]\n", 1, 1, "malformed"},
        {"[editor]]\n", 1, 1, "malformed"},
        {"[[editor]\n", 1, 1, "malformed"},
        {"[Editor]\n", 1, 2, "unknown keymap section"},
        {"[ editor ]\n", 1, 2, "unknown keymap section"},
        {"[viewer:ascii]\n", 1, 2, "unknown keymap section"},
        {"[viewer] # comment\n", 1, 1, "malformed"},
        {"[viewer]\n  Unknown=f3\n", 2, 3, "unknown keymap action"},
        {"[viewer]\nQuit f3\n", 2, 1, "expected Action"},
        {"[viewer]\n =f3\n", 2, 2, "empty keymap action"},
        {"[viewer]\nQuit=hello\n", 2, 6, "unknown or malformed key"},
        {"[viewer]\nQuit=ctrl|x\n", 2, 6, "unknown or malformed key"},
        {"[viewer]\nQuit=ctr-a\n", 2, 6, "unknown or malformed key"},
        {"[viewer]\nQuit=alt-gt ctrl-e\n", 2, 6, "unknown or malformed key"},
        {"[viewer]\nQuit=ctrl-\n", 2, 6, "modifier without a key"},
        {"[viewer]\nQuit=ctrl-alt\n", 2, 6, "modifier without a key"},
        {"[viewer]\nQuit=f99\n", 2, 6, "unknown or malformed key"},
        {"[viewer]\nQuit=\"f3\"\n", 2, 6, "unknown or malformed key"},
        {"[viewer]\nQuit=+;\n", 2, 6, "use plus/minus"},
        {"[viewer]\nQuit=x # comment\n", 2, 6, "unknown or malformed key"},
        {"[viewer]\nQuit=x; # comment\n", 2, 9, "unknown or malformed key"},
        {"[viewer]\nQuit=\\q\n", 2, 6, "unknown keymap escape"},
        {"[viewer]\nQuit=x\\\n", 2, 7, "unfinished keymap escape"},
        {"[viewer]\nQuit=x\\n\n", 2, 6, "unknown or malformed key"},
        {"[viewer]\nQuit=bad\nQuit=f3\n", 2, 6, "unknown or malformed key"},
        {"; not a comment\n[viewer]\nQuit=f3\n", 1, 1, "before any section"},
        {"#\xc0\xaf\n", 1, 2, "invalid UTF-8"},
        {"[editor]\nQuit=\xed\xa0\x80\n", 2, 6, "invalid UTF-8"},
        {"[editor]\nQuit=\xf4\x90\x80\x80\n", 2, 6, "invalid UTF-8"},
        {"[editor]\nQuit=\xc3\n", 2, 6, "invalid UTF-8"},
        {"[editor]\nQuit=\x1b\n", 2, 6, "control character"},
        {"\xef\xbb\xbf[editor]\n", 1, 1, "UTF-8 BOM"},
    };
    for (const auto& failure : failures) {
        resource_error(
            [&] { static_cast<void>(Keymap::parse(failure.text, "fixture.keymap")); },
            "fixture.keymap", failure.line, failure.column, failure.message
        );
    }
    const std::string embedded_nul{"[editor]\nQuit=x\0ignored\n", 24};
    resource_error(
        [&] { static_cast<void>(Keymap::parse(embedded_nul, "nul.keymap")); },
        "nul.keymap", 2, 7, "NUL byte"
    );
}

void test_loading(const std::filesystem::path& root) {
    std::filesystem::create_directories(root);
    const auto good = root / "chosen-bindings.keymap";
    write_file(good, "[viewer:hex]\nSave=F11\nQuit=ctrl-g");
    auto map = Keymap::load(good);
    check(map.lookup(KeymapContext::viewer_hex, "f11") == "Save",
          "load a local keymap without a final newline");
    const auto before = snapshot(map);
    const auto missing = root / "missing.keymap";
    resource_error(
        [&] { map.overlay_file(missing); },
        missing.string(), 0, 0, "cannot open keymap"
    );
    check(snapshot(map) == before, "missing overlay files never reset bindings");
    resource_error(
        [&] { static_cast<void>(Keymap::load(root)); },
        root.string(), 0, 0, "cannot read keymap"
    );
    const auto denied = root / "unreadable.keymap";
    write_file(denied, "[tree]\nForget=f12\n");
    std::filesystem::permissions(denied, std::filesystem::perms::owner_write);
    resource_error(
        [&] { static_cast<void>(Keymap::load(denied)); },
        denied.string(), 0, 0, "cannot open keymap"
    );
    std::filesystem::permissions(
        denied, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write
    );
    const auto bad = root / "invalid.keymap";
    write_file(bad, "[viewer:hex]\nSave=f1\nUnknown=f3\n");
    resource_error(
        [&] { map.overlay_file(bad); },
        bad.string(), 3, 1, "unknown keymap action"
    );
    check(snapshot(map) == before, "malformed file overlays are atomic");
    const auto valid_overlay = root / "overlay.keymap";
    write_file(valid_overlay, "[viewer:hex]\nSave=f12\n");
    map.overlay_file(valid_overlay);
    check(!map.lookup(KeymapContext::viewer_hex, "f11") &&
          map.lookup(KeymapContext::viewer_hex, "f12") == "Save",
          "a successful overlay replaces the whole action list");
}

}  // namespace

int main(const int argc, const char* const argv[]) {
    if (getuid() == 0 || geteuid() == 0 ||
        !std::filesystem::exists("/review/.operation-regressions-disposable")) {
        std::cerr << "Keymap error-injection tests require the non-root disposable container.\n";
        return 77;
    }
    if (argc < 2) {
        std::cerr << "Usage: test_keymap CASE-DIRECTORY [NATIVE-KEYMAP ...]\n";
        return 2;
    }
    const auto root = std::filesystem::weakly_canonical(
        std::filesystem::absolute(argv[1])
    );
    if (!root.string().starts_with("/review/cases/agent-keymap/")) {
        std::cerr << "Keymap fixtures must stay below /review/cases/agent-keymap/.\n";
        return 2;
    }
    try {
        test_key_names();
        test_event_names();
        test_resource_lists();
        test_contexts_and_actions();
        test_overrides();
        test_diagnostics();
        test_loading(root);
        for (int index = 2; index < argc; ++index) {
            const auto map = Keymap::load(argv[index]);
            check(!map.sections().empty(), "native keymap has public sections");
            std::cout << "Loaded native format: " << argv[index] << " ("
                      << map.sections().size() << " sections)\n";
        }
    } catch (const std::runtime_error& error) {
        std::cerr << error.what() << '\n';
        return 1;
    } catch (const std::invalid_argument& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "Keymap regressions passed.\n";
    return 0;
}
