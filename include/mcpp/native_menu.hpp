#pragma once

#include "mcpp/entry.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mcpp {

struct NativeMenuTest {
    char predicate{'f'};
    std::string operand;
    bool negate{false};
    char operation{'&'};
};

struct NativeMenuCondition {
    std::vector<NativeMenuTest> tests;
    bool addition{false};
    bool default_selection{false};
    bool debug{false};
    bool shell_patterns{true};
    std::size_t line{0};
};

struct NativeMenuEntry {
    char key{'?'};
    std::string label{};
    std::string command{};
    bool native{false};
    std::vector<NativeMenuCondition> conditions{};
    std::string source{};
    std::size_t line{0};
};

struct NativeMenuPane {
    std::string directory;
    std::optional<Entry> file;
    std::vector<Entry> tagged;
    bool local{true};
};

struct NativeMenuContext {
    NativeMenuPane active;
    NativeMenuPane other;
    bool editor{false};
    std::size_t column{0};
    std::string syntax;
    std::string menu_file;
    std::string block_file;
    std::string error_file;
    std::function<bool(std::string_view)> executable;
};

struct NativeMenuSelection {
    std::vector<NativeMenuEntry> entries;
    std::size_t selected{0};
    std::vector<std::string> trace;
};

struct NativeMenuRequirements {
    std::vector<std::string> prompts;
    bool block_file{false};
    bool error_file{false};
};

enum class NativeMenuAction { shell, view, change_directory };

struct NativeMenuExpansion {
    NativeMenuAction action{NativeMenuAction::shell};
    std::string command;
    std::optional<bool> view_hex;
    std::optional<bool> view_nroff;
    std::vector<std::string> untag_active;
    std::vector<std::string> untag_other;
};

[[nodiscard]] std::vector<NativeMenuEntry> parse_native_menu(
    std::string_view content,
    std::string_view source = {},
    bool shell_patterns = true
);

[[nodiscard]] NativeMenuSelection select_native_menu(
    const std::vector<NativeMenuEntry>& entries,
    const NativeMenuContext& context
);

[[nodiscard]] NativeMenuRequirements native_menu_requirements(
    std::string_view command,
    bool editor
);

[[nodiscard]] bool native_menu_has_view_filter(std::string_view command);

[[nodiscard]] NativeMenuExpansion expand_native_menu(
    std::string_view command,
    const NativeMenuContext& context,
    const std::vector<std::string>& answers = {}
);

[[nodiscard]] NativeMenuExpansion expand_native_view_filter(
    std::string_view command, const NativeMenuContext& context,
    const std::vector<std::string>& answers, std::string_view local_file
);

// Non-local relative paths stay relative for resolution against the captured provider.
[[nodiscard]] std::string native_menu_directory(
    std::string_view expanded_command,
    const NativeMenuContext& context,
    std::string_view previous_directory = {}
);

}  // namespace mcpp
