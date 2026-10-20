#pragma once

#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace mcpp {

enum class KeymapContext {
    filemanager,
    filemanager_xmap,
    panel,
    dialog,
    menu,
    input,
    listbox,
    radio,
    tree,
    help,
    chattr,
    editor,
    editor_xmap,
    viewer,
    viewer_hex,
    diffviewer,
};

[[nodiscard]] std::optional<KeymapContext> parse_keymap_context(
    std::string_view name
) noexcept;
[[nodiscard]] std::string_view keymap_context_name(KeymapContext context);
[[nodiscard]] std::optional<std::string_view> canonical_keymap_action(
    std::string_view name
) noexcept;

// MC resource names: named keys are case-insensitive; literal letters are not.
// Modifiers precede the key and use '-', '+', or whitespace separators.
[[nodiscard]] std::string normalize_key_name(std::string_view name);

// Additionally accepts mcpp's named_event labels (PageUp, BackTab, Keypad +,
// Completion, etc.) and literal printable event characters, including '+'/'-'.
// This does not decode terminal escape sequences.
[[nodiscard]] std::string normalize_event_key_name(std::string_view name);

struct KeymapDiagnostic {
    std::string source;
    std::size_t line{0};
    // One-based byte position; zero line/column denotes a file I/O error.
    std::size_t column{0};
    std::string message;
};

class KeymapError : public std::runtime_error {
public:
    explicit KeymapError(KeymapDiagnostic diagnostic);
    [[nodiscard]] const KeymapDiagnostic& diagnostic() const noexcept;

private:
    KeymapDiagnostic diagnostic_;
};

struct KeymapAction {
    // Exact INI spelling is significant when replacing a definition.
    std::string name;
    // Canonical action identifiers refer to the model's static catalog.
    std::string_view action;
    std::vector<std::string> keys;
};

class Keymap {
public:
    using Section = std::vector<KeymapAction>;
    using Sections = std::map<KeymapContext, Section>;

    // No built-in bindings or implicit file search. Missing/unreadable files,
    // invalid UTF-8, and unknown sections/actions/keys raise KeymapError.
    [[nodiscard]] static Keymap parse(
        std::string_view resource,
        std::string source = "<keymap>"
    );
    [[nodiscard]] static Keymap load(const std::filesystem::path& path);

    // Duplicate exact-spelling action names replace their entire lists in
    // place. Case variants coexist. The first action defining a key wins.
    // An empty list removes that definition's bindings. Overlays are atomic.
    void overlay(const Keymap& other);
    void overlay(std::string_view resource, std::string source = "<keymap>");
    void overlay_file(const std::filesystem::path& path);

    [[nodiscard]] const Sections& sections() const noexcept;
    [[nodiscard]] bool contains_action(
        KeymapContext context,
        std::string_view action
    ) const;
    // Effective bindings only: keys shadowed by an earlier action are omitted.
    [[nodiscard]] std::vector<std::string> bindings_for(
        KeymapContext context,
        std::string_view action
    ) const;

    // Contexts are independent: callers choose panel/manager precedence and
    // enter/leave prefix maps after handling the ExtendedKeyMap action.
    [[nodiscard]] std::optional<std::string_view> lookup(
        KeymapContext context,
        std::string_view key
    ) const;
    [[nodiscard]] std::optional<std::string_view> lookup_canonical(
        KeymapContext context,
        std::string_view canonical_key
    ) const noexcept;

private:
    Sections sections_;
};

}  // namespace mcpp
