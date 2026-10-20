#pragma once

#include <ftxui/component/event.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace mcpp {

// Returns normalize_key_name's format, not an action. Raw HT is Tab, CR/LF
// are Enter, and BS/DEL are Backspace: those bytes cannot identify modifiers.
// FTXUI's NUL-valued Custom event is not a physical Ctrl-Space key.
[[nodiscard]] std::optional<std::string> keymap_event_name(
    const ftxui::Event& event
);

// Also matches legacy control-byte aliases and FTXUI's single-byte CR/BS/NUL
// normalization. Enhanced reports and distinct Alt-prefixed bytes stay distinct.
// Unavailable or malformed resource names return false.
[[nodiscard]] bool keymap_event_matches(
    const ftxui::Event& event,
    std::string_view canonical_name
);

// Produces one keyboard event, using enhanced encodings where legacy bytes
// would lose the requested key's identity. Unsupported names/encodings throw
// std::invalid_argument. This neither sends bytes nor changes terminal modes.
[[nodiscard]] ftxui::Event keymap_named_event(std::string_view canonical_name);

}  // namespace mcpp
