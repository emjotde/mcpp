#include "mcpp/keymap_input.hpp"

#include "mcpp/keymap.hpp"

#include <ftxui/component/terminal_input_parser.hpp>

#include <array>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using ftxui::Event;

void check(const bool condition, const std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

void expect(const Event& event, const std::string_view name) {
    const auto actual = mcpp::keymap_event_name(event);
    if (actual != name) {
        throw std::runtime_error(
            "expected key " + std::string(name) + ", got " + actual.value_or("<none>")
        );
    }
    check(mcpp::normalize_key_name(*actual) == *actual,
          "decoded names use the keymap model's canonical format");
}

void absent(const Event& event) {
    const auto actual = mcpp::keymap_event_name(event);
    if (actual) {
        throw std::runtime_error("non-key event fabricated key " + *actual);
    }
}

void roundtrip(const std::string_view name) {
    expect(mcpp::keymap_named_event(name), mcpp::normalize_key_name(name));
}

void unsupported(const std::string_view name) {
    bool rejected = false;
    try {
        static_cast<void>(mcpp::keymap_named_event(name));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected, "unsupported key names must throw rather than substitute another key");
}

void test_ftxui_events() {
    const std::pair<Event, std::string_view> keys[]{
        {Event::a, "a"}, {Event::A, "A"}, {Event::Character(' '), "space"},
        {Event::Character('+'), "plus"}, {Event::Character('-'), "minus"},
        {Event::Character(';'), "semicolon"}, {Event::Character('\\'), "backslash"},
        {Event::Character("\xc3\xa9"), "\xc3\xa9"},
        {Event::Character("\xe4\xb8\xad"), "\xe4\xb8\xad"},
        {Event::Character("\xf0\x9f\x8c\xb2"), "\xf0\x9f\x8c\xb2"},
        {Event::ArrowUp, "up"}, {Event::ArrowDown, "down"},
        {Event::ArrowLeft, "left"}, {Event::ArrowRight, "right"},
        {Event::ArrowLeftCtrl, "ctrl-left"}, {Event::ArrowRightCtrl, "ctrl-right"},
        {Event::ArrowUpCtrl, "ctrl-up"}, {Event::ArrowDownCtrl, "ctrl-down"},
        {Event::Home, "home"}, {Event::End, "end"}, {Event::PageUp, "pgup"},
        {Event::PageDown, "pgdn"}, {Event::Insert, "insert"}, {Event::Delete, "delete"},
        {Event::Tab, "tab"}, {Event::TabReverse, "shift-tab"}, {Event::Return, "enter"},
        {Event::Backspace, "backspace"}, {Event::Escape, "esc"},
        {Event::CtrlA, "ctrl-a"}, {Event::CtrlX, "ctrl-x"}, {Event::CtrlZ, "ctrl-z"},
        {Event::AltA, "alt-a"}, {Event::AltZ, "alt-z"}, {Event::CtrlAltX, "ctrl-alt-x"},
        {Event::CtrlH, "backspace"}, {Event::CtrlI, "tab"},
        {Event::CtrlJ, "enter"}, {Event::CtrlM, "enter"},
    };
    for (const auto& [event, name] : keys) expect(event, name);
    const std::array<Event, 12> functions{
        Event::F1, Event::F2, Event::F3, Event::F4, Event::F5, Event::F6,
        Event::F7, Event::F8, Event::F9, Event::F10, Event::F11, Event::F12,
    };
    for (std::size_t index = 0; index < functions.size(); ++index) {
        const auto name = "f" + std::to_string(index + 1U);
        expect(functions[index], name);
        check(mcpp::keymap_named_event(name) == functions[index],
              "unmodified function encodings match normal FTXUI events");
    }
    check(mcpp::keymap_named_event("up") == Event::ArrowUp &&
          mcpp::keymap_named_event("enter") == Event::Return &&
          mcpp::keymap_named_event("shift-tab") == Event::TabReverse,
          "ordinary named events use FTXUI's existing inputs");
    check(mcpp::keymap_named_event("ctrl-i") != Event::Tab &&
          mcpp::keymap_named_event("ctrl-h") != Event::Backspace,
          "enhanced inverse encodings retain distinctions unavailable in legacy bytes");
    check(mcpp::keymap_named_event("ctrl-space").input() == "\x1b[32;5u",
          "Ctrl-Space does not become a NUL-valued Custom event");
}

void test_raw_sequences() {
    constexpr std::pair<std::string_view, std::string_view> keys[]{
        {"\x1bOA", "up"}, {"\x1bOB", "down"}, {"\x1bOC", "right"},
        {"\x1bOD", "left"}, {"\x1bOH", "home"}, {"\x1bOF", "end"},
        {"\x1bOE", "b2"}, {"\x1bOM", "enter"},
        {"\x1bOk", "kpplus"}, {"\x1bOm", "kpminus"},
        {"\x1bOj", "kpasterisk"}, {"\x1bOo", "kpslash"},
        {"\x1b[1~", "home"}, {"\x1b[4~", "end"},
        {"\x1b[7~", "home"}, {"\x1b[8~", "end"},
        {"\x1b[1;2A", "shift-up"}, {"\x1b[1;3B", "alt-down"},
        {"\x1b[1;8D", "ctrl-alt-shift-left"},
        {"\x1b[;5C", "ctrl-right"}, {"\x1b[1;5H", "ctrl-home"},
        {"\x1b[1;6F", "ctrl-shift-end"}, {"\x1b[5;3~", "alt-pgup"},
        {"\x1b[2;5~", "ctrl-insert"}, {"\x1b[3;2~", "shift-delete"},
        {"\x1b[Z", "shift-tab"}, {"\x1b[1;5Z", "ctrl-shift-tab"},
        {"\x1bOP", "f1"}, {"\x1bOQ", "f2"}, {"\x1bOR", "f3"}, {"\x1bOS", "f4"},
        {"\x1b[11~", "f1"}, {"\x1b[14~", "f4"}, {"\x1b[25~", "f13"},
        {"\x1b[34~", "f20"}, {"\x1b[1;2P", "shift-f1"},
        {"\x1b[1;6R", "ctrl-shift-f3"}, {"\x1b[18;2~", "shift-f7"},
        {"\x1bO1;5Q", "ctrl-f2"}, {"\x1bO5P", "ctrl-f1"},
        {"\x1b[[A", "f1"}, {"\x1b[[E", "f5"},
        {"\x1b\t", "alt-tab"}, {"\x1b\r", "alt-enter"}, {"\x1b\x7f", "alt-backspace"},
        {"\x1b\x1b", "alt-esc"}, {"\x1b\x18", "ctrl-alt-x"},
        {"\x1b\x1b[A", "alt-up"}, {"\x1b\x1b[1;5D", "ctrl-alt-left"},
        {"\x1b" "A", "alt-A"}, {"\x1b?", "alt-question"},
        {"\x1b\xc3\xa9", "alt-\xc3\xa9"},
        {"\x08", "backspace"}, {"\x09", "tab"}, {"\x0a", "enter"}, {"\x0d", "enter"},
        {"\x1c", "ctrl-backslash"}, {"\x1d", "ctrl-rbracket"},
        {"\x1e", "ctrl-caret"}, {"\x1f", "ctrl-underline"},
        {"\x1b[97u", "a"}, {"\x1b[97;5u", "ctrl-a"}, {"\x1b[65;3u", "alt-A"},
        {"\x1b[105;5u", "ctrl-i"}, {"\x1b[9;5u", "ctrl-tab"},
        {"\x1b[9;6u", "ctrl-shift-tab"}, {"\x1b[13;2u", "shift-enter"},
        {"\x1b[32;5u", "ctrl-space"}, {"\x1b[64;5u", "ctrl-at"},
        {"\x1b[233;3u", "alt-\xc3\xa9"}, {"\x1b[20013;5u", "ctrl-\xe4\xb8\xad"},
        {"\x1b[97;5:1u", "ctrl-a"}, {"\x1b[97;5:2u", "ctrl-a"},
        {"\x1b[49:33;2u", "exclamation"},
        {"\x1b[27;5;9~", "ctrl-tab"}, {"\x1b[27;6;13~", "ctrl-shift-enter"},
        {"\x1b[27;3;233~", "alt-\xc3\xa9"},
        {"\x1b[57344u", "esc"}, {"\x1b[57348;5u", "ctrl-insert"},
        {"\x1b[57350u", "left"}, {"\x1b[57364;2u", "shift-f1"},
        {"\x1b[57383u", "f20"},
        {"\x1b[27;5;57364~", "ctrl-\xee\x80\x94"},
    };
    for (const auto& [raw, name] : keys) expect(Event::Special(raw), name);
}

void test_roundtrips() {
    constexpr std::string_view names[]{
        "a", "A", "space", "plus", "minus", "prime", "\"", "`", "~",
        "lbracket", "backslash", "semicolon", "at", "hash", "exclamation",
        "\xc3\xa9", "\xe4\xb8\xad", "\xf0\x9f\x8c\xb2", "\xee\x80\x94",
        "ctrl-a", "ctrl-h", "ctrl-i", "ctrl-j", "ctrl-m", "ctrl-z",
        "ctrl-space", "ctrl-at", "ctrl-question", "ctrl-backslash", "ctrl-rbracket",
        "ctrl-caret", "ctrl-underline", "ctrl-alt-h", "ctrl-alt-x", "ctrl-alt-i",
        "alt-A", "alt-plus", "alt-space", "alt-lbracket", "alt-backslash",
        "alt-O", "alt-P", "alt-\xc3\xa9", "ctrl-\xe4\xb8\xad",
        "shift-\xc3\xa9", "ctrl-alt-shift-\xe4\xb8\xad",
        "alt-\xee\x80\x94", "ctrl-\xee\x80\x94",
        "tab", "shift-tab", "alt-tab", "alt-shift-tab", "ctrl-tab", "ctrl-shift-tab",
        "enter", "alt-enter", "shift-enter", "ctrl-enter", "ctrl-alt-shift-enter",
        "esc", "alt-esc", "ctrl-esc", "backspace", "alt-backspace", "ctrl-backspace",
        "b2", "ctrl-b2", "kpplus", "kpminus", "kpasterisk", "kpslash", "ctrl-kpplus",
    };
    for (const auto name : names) roundtrip(name);
    constexpr std::string_view modifiers[]{
        "", "shift-", "alt-", "alt-shift-", "ctrl-", "ctrl-shift-",
        "ctrl-alt-", "ctrl-alt-shift-",
    };
    for (const auto modifier : modifiers) {
        for (const auto key : {"up", "down", "left", "right", "home", "end",
                               "pgup", "pgdn", "insert", "delete"}) {
            roundtrip(std::string(modifier) + key);
        }
        for (unsigned int index = 1; index <= 20U; ++index) {
            roundtrip(std::string(modifier) + "f" + std::to_string(index));
        }
    }
    for (const auto name : {"f0", "f21", "a1", "a3", "c1", "c3", "unknown",
                            "PageUp", "ctrl-", "ctrl|x", "+"}) {
        unsupported(name);
    }
}

void test_non_keys() {
    absent(Event::Custom);
    absent(Event::Special(std::string_view("\0", 1)));
    absent(Event::Character(std::string_view("\0", 1)));
    absent(Event::Mouse("\x1b[A", {}));
    absent(Event::CursorPosition("\x1b[A", 1, 1));
    absent(Event::CursorShape("\x1b[A", 1));
    absent(Event::TerminalNameVersion("\x1bOP", "test", 1));
    absent(Event::TerminalEmulator("\x1bOP", "test", "1"));
    absent(Event::TerminalCapabilities("\x1bOP", {1}));
    for (const auto text : {"", "ab", "e\xcc\x81", "\xc0\xaf", "\xed\xa0\x80",
                            "\xf4\x90\x80\x80", "\xc3", "\xc2\x80", "\x1b[A"}) {
        absent(Event::Character(text));
    }
    constexpr std::string_view special[]{
        "", "a", "custom", "\x1b[", "\x1bO", "\x1bP", "\x1b\\",
        "\x1b[?25h", "\x1b[2A", "\x1b[1;9A", "\x1b[0;5A",
        "\x1b[1;5;2A", "\x1b[999~", "\x1b[3;0~", "\x1b[200~", "\x1b[201~",
        "\x1b[I", "\x1b[O", "\x1b[M", "\x1b[<0;3;4M", "\x1b[<0;3;4m",
        "\x1b[10;20R", "\x1b[1;2c", "\x1b[>0;115;0c",
        "\x1b]0;title\x07", "\x1bPtest\x1b\\", "\x1b[Atrailing",
        "\x1b[97;5:3u", "\x1b[97;5:4u", "\x1b[97;9u", "\x1b[97;0u",
        "\x1b[0;5u", "\x1b[1;5u", "\x1b[128;5u", "\x1b[55296;5u",
        "\x1b[1114112;5u", "\x1b[999999999999999999999u",
        "\x1b[97;5;97u", "\x1b[97:65:97;2u", "\x1b[49:bad;2u",
        "\x1b[57358u", "\x1b[57384u", "\x1b[27;9;97~",
        "\x1b[27;5;1114112~", "\x1b[27;5;97;1~",
        "\x1b\xc3", "\x1b\xc0\xaf", "\x1b\x1b" "a",
    };
    for (const auto raw : special) absent(Event::Special(raw));
}

std::vector<Event> terminal_input(const std::string_view raw) {
    std::vector<Event> events;
    ftxui::TerminalInputParser parser([&](Event event) {
        events.push_back(std::move(event));
    });
    for (const auto byte : raw) parser.Add(byte);
    parser.Timeout(1000);
    return events;
}

void test_binding_matches() {
    const std::pair<Event, std::string_view> matches[]{
        {Event::Return, "ctrl-m"}, {Event::Return, "ctrl-j"},
        {Event::CtrlM, "ctrl-m"}, {Event::Tab, "ctrl-i"},
        {Event::CtrlH, "ctrl-h"}, {Event::CtrlH, "backspace"},
        {Event::Backspace, "ctrl-h"}, {Event::Backspace, "ctrl-question"},
        {Event::Escape, "ctrl-lbracket"},
        {Event::Special("\x1b\r"), "ctrl-alt-m"},
        {Event::Special("\x1b\n"), "ctrl-alt-j"},
        {Event::Special("\x1b\x08"), "ctrl-alt-h"},
        {Event::Special("\x1b\x7f"), "ctrl-alt-question"},
        {Event::Special("\x1b\x1b"), "ctrl-alt-lbracket"},
        {Event::Special("\x1b[32;5u"), "ctrl-at"},
        {Event::Special("\x1b[105;5u"), "ctrl-i"},
        {Event::Special("\x1b[9;5u"), "ctrl-tab"},
        {Event::Special("\x1b[1~"), "home"},
        {Event::Special("\x1b[27;5;9~"), "ctrl-tab"},
        {Event::Character("\xc3\xa9"), "\xc3\xa9"},
    };
    for (const auto& [event, name] : matches) {
        check(mcpp::keymap_event_matches(event, name),
              "resource binding accepts its emitted terminal spelling");
    }
    const std::pair<Event, std::string_view> distinct[]{
        {Event::CtrlM, "ctrl-j"}, {Event::CtrlH, "ctrl-question"},
        {Event::Special("\x1b\r"), "ctrl-alt-j"},
        {Event::Special("\x1b\n"), "ctrl-alt-m"},
        {Event::Special("\x1b\x08"), "ctrl-alt-question"},
        {Event::Special("\x1b\x7f"), "ctrl-alt-h"},
        {Event::Special("\x1b[105;5u"), "tab"},
        {Event::Special("\x1b[9u"), "ctrl-i"},
        {Event::Special("\x1b[9;5u"), "ctrl-i"},
        {Event::Special("\x1b[13u"), "ctrl-m"},
        {Event::Special("\x1b[104;5u"), "backspace"},
        {Event::Special("\x1b[127u"), "ctrl-h"},
        {Event::Special("\x1b[27u"), "ctrl-lbracket"},
        {Event::Special("\x1b[91;5u"), "esc"},
        {Event::Special("\x1b[63;5u"), "backspace"},
        {Event::Special("\x1b[64;5u"), "ctrl-space"},
        {Event::Special("\x1b[27;5;32~"), "ctrl-at"},
        {Event::Special("\x1b[25~"), "shift-f1"},
        {Event::Special("\x1b[1;2P"), "f13"},
        {Event::Custom, "ctrl-space"}, {Event::Mouse("\t", {}), "ctrl-i"},
        {Event::CursorPosition("\n", 1, 1), "ctrl-m"},
        {Event::a, "a1"}, {Event::a, "ctrl-a1"}, {Event::a, "f0"},
        {Event::a, "unknown"}, {Event::a, "ctrl-"}, {Event::Special("\x1b[?25h"), "esc"},
    };
    for (const auto& [event, name] : distinct) {
        check(!mcpp::keymap_event_matches(event, name),
              "matcher preserves observable distinctions and rejects unavailable names");
    }
    for (const auto raw : {"\r", "\t", "\x08", "\x7f", "\x1b"}) {
        const auto events = terminal_input(raw);
        check(events.size() == 1U, "legacy alias produces one real FTXUI event");
        const auto binding = raw == std::string_view("\r") ? "ctrl-m" :
                             raw == std::string_view("\t") ? "ctrl-i" :
                             raw == std::string_view("\x08") ? "ctrl-h" :
                             raw == std::string_view("\x7f") ? "ctrl-question" :
                             "ctrl-lbracket";
        check(mcpp::keymap_event_matches(events.front(), binding),
              "legacy aliases survive the actual FTXUI input normalization");
    }
    const auto resolve = [](const mcpp::Keymap& map, const Event& event)
        -> std::optional<std::string_view> {
        for (const auto& action : map.sections().at(mcpp::KeymapContext::editor)) {
            for (const auto& key : action.keys) {
                if (mcpp::keymap_event_matches(event, key)) return action.action;
            }
        }
        return std::nullopt;
    };
    const auto first = mcpp::Keymap::parse("[editor]\nSearch=ctrl-m\nSave=enter\n");
    const auto reversed = mcpp::Keymap::parse("[editor]\nSave=enter\nSearch=ctrl-m\n");
    check(resolve(first, Event::Return) == "Search" &&
          resolve(reversed, Event::Return) == "Save",
          "declaration order, not alias enumeration order, chooses colliding actions");
    const auto unavailable = mcpp::Keymap::parse("[editor]\nHelp=a1\nCopy=ctrl-i\nEnter=tab\n");
    check(resolve(unavailable, Event::Tab) == "Copy",
          "an unavailable key is skipped without throwing or taking precedence");
}

void test_terminal_pipeline() {
    constexpr std::pair<std::string_view, std::string_view> keys[]{
        {"a", "a"}, {"+", "plus"}, {"\xc3\xa9", "\xc3\xa9"},
        {"\x1bOA", "up"}, {"\x1b[1~", "home"},
        {"\x1b[[A", "f1"}, {"\x1b[11~", "f1"},
        {"\x1b[34~", "f20"}, {"\x1b[1;6D", "ctrl-shift-left"},
        {"\x1bOk", "kpplus"}, {"\x08", "backspace"}, {"\x0d", "enter"},
        {"\x1b[9;5u", "ctrl-tab"}, {"\x1b[27;6;13~", "ctrl-shift-enter"},
        {"\x1b[233;3u", "alt-\xc3\xa9"},
        {"\x1b[27;5;57364~", "ctrl-\xee\x80\x94"},
    };
    for (const auto& [raw, name] : keys) {
        const auto events = terminal_input(raw);
        check(events.size() == 1U, "real terminal input yields one keyboard event");
        expect(events.front(), name);
    }
    const auto space = terminal_input(std::string_view("\0", 1));
    check(space.size() == 1U, "physical Ctrl-Space yields one patched FTXUI event");
    expect(space.front(), "ctrl-space");
    const auto mouse = terminal_input("\x1b[<0;3;4M");
    check(mouse.size() == 1U && mouse.front().is_mouse(), "FTXUI recognizes raw mouse input");
    absent(mouse.front());
    const auto cursor = terminal_input("\x1b[1;2R");
    check(cursor.size() == 1U && cursor.front().is_cursor_position(),
          "FTXUI owns the ambiguous F3/cursor-report classification");
    absent(cursor.front());
}

}  // namespace

int main() {
    if (getuid() == 0 || geteuid() == 0 ||
        !std::filesystem::exists("/review/.operation-regressions-disposable")) {
        std::cerr << "Keymap input error cases require the non-root disposable container.\n";
        return 77;
    }
    try {
        test_ftxui_events();
        test_raw_sequences();
        test_roundtrips();
        test_non_keys();
        test_binding_matches();
        test_terminal_pipeline();
    } catch (const std::runtime_error& error) {
        std::cerr << error.what() << '\n';
        return 1;
    } catch (const std::invalid_argument& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "Keymap input regressions passed.\n";
    return 0;
}
