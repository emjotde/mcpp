#pragma once

#include <cstddef>
#include <filesystem>
#include <iosfwd>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mcpp {

struct EditorFile {
    std::string path;
    std::size_t line{0};
};

[[nodiscard]] std::size_t parse_line_number(std::string_view value);
[[nodiscard]] EditorFile parse_editor_file(std::string path);

enum class CliFrontend { manager, editor, viewer, diff };
enum class CliAction {
    run, help, help_all, help_terminal, help_color, version,
    data_directory, data_directory_info, configure_options
};

struct CliOptions {
    CliFrontend frontend{CliFrontend::manager};
    CliAction action{CliAction::run};
    bool edit{false};
    std::optional<std::string> view_file;
    std::vector<std::string> paths;
    std::size_t first_line{0};
    std::optional<std::filesystem::path> print_directory;
    std::optional<std::string> skin;
    std::optional<std::string> keymap;
    bool keymap_disabled{false};
    std::optional<bool> subshell;
    bool mouse{true};
    std::optional<bool> color;
    std::vector<std::string> colors;
    bool stickchars{false};
};

[[nodiscard]] std::string_view cli_program_name(CliFrontend frontend);
[[nodiscard]] CliOptions parse_cli(
    std::span<const std::string_view> arguments, CliFrontend frontend = CliFrontend::manager
);
[[nodiscard]] std::string_view cli_color_roles();
void print_cli_action(const CliOptions& options, std::ostream& output);

}  // namespace mcpp
