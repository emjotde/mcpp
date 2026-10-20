#pragma once

#include "mcpp/app.hpp"
#include "mcpp/command_line.hpp"
#include "mcpp/config.hpp"
#include "mcpp/local_provider.hpp"
#include "mcpp/location.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mcpp {

[[nodiscard]] inline std::string resolve_cli_path(
    const std::string& value, const std::filesystem::path& current
) {
    if (is_virtual_location(value)) return value;
    const auto path = std::filesystem::path(value);
    return (path.is_absolute() ? path : current / path).lexically_normal().string();
}

inline int run_cli(const CliFrontend frontend, const int argc, char** argv) {
    try {
        std::vector<std::string_view> arguments;
        arguments.reserve(static_cast<std::size_t>(argc > 0 ? argc - 1 : 0));
        for (int index = 1; index < argc; ++index) arguments.emplace_back(argv[index]);
        auto options = parse_cli(arguments, frontend);
        if (options.action != CliAction::run) {
            print_cli_action(options, std::cout);
            return 0;
        }
        auto config = Config::load(default_config_path());
        options.color = configured_color_mode(config, options.color);
        prepare_cli_terminal(options.color, options.stickchars);
        AppOptions launch{
            .mouse = options.mouse,
            .color = options.color,
            .colors = parse_cli_colors(options.colors),
            .stickchars = options.stickchars
        };
        const auto current = std::filesystem::current_path();
        const auto path = [&](const std::string& value) { return resolve_cli_path(value, current); };
        const auto print_directory = options.print_directory
            ? std::optional(std::filesystem::absolute(*options.print_directory).lexically_normal())
            : std::nullopt;
        std::vector<EditorFile> editor_files;
        if (options.edit) {
            for (const auto& value : options.paths) {
                auto file = parse_editor_file(value);
                file.path = path(file.path);
                editor_files.push_back(std::move(file));
            }
        }
        config.theme_override = std::move(options.skin);
        config.keymap_override = std::move(options.keymap);
        config.keymap_disabled = options.keymap_disabled;
        config.subshell_override = options.subshell;
        const auto saved_path = [&](const std::string& value, const char* panel) {
            const bool restore_other = config.other_directory &&
                ((std::string_view(panel) == "left") != config.current_is_left.value_or(true));
            const auto& requested = restore_other ? *config.other_directory : value;
            if ((!config.auto_save_panels_setup && !restore_other) || requested.empty()) return current.string();
            const auto saved = path(requested);
            if (is_virtual_location(saved)) {
                try {
                    const auto location = open_location(saved, std::make_shared<LocalProvider>());
                    if (location.provider->stat_follow(location.path, {}).kind == EntryKind::directory) return saved;
                } catch (const std::exception& error) {
                    std::cerr << cli_program_name(frontend) << ": saved " << panel << " panel: " << error.what() << '\n';
                }
            }
            std::error_code error;
            if (std::filesystem::is_directory(saved, error) && !error) return saved;
            std::cerr << cli_program_name(frontend) << ": cannot restore saved " << panel
                      << " panel; using the current directory\n";
            return current.string();
        };
        std::string left = current.string();
        std::string right = left;
        if (frontend == CliFrontend::manager) {
            left = options.edit || options.paths.empty()
                ? saved_path(config.saved_left_path, "left") : path(options.paths[0]);
            right = options.edit || options.paths.size() < 2U
                ? saved_path(config.saved_right_path, "right") : path(options.paths[1]);
        }
        App app(std::move(left), std::move(right), std::move(config), std::move(launch));
        if (options.view_file) app.open_viewer(path(*options.view_file));
        else if (options.edit) app.open_editor_files(editor_files, options.first_line);
        else if (frontend == CliFrontend::viewer) app.open_viewer(path(options.paths.front()));
        else if (frontend == CliFrontend::diff) app.open_diff(path(options.paths[0]), path(options.paths[1]));
        const auto status = app.run();
        if (print_directory) {
            std::ofstream output(*print_directory, std::ios::trunc);
            if (output) {
                output << app.active_path() << '\n';
                output.close();
            }
            if (!output) throw std::runtime_error("cannot write final directory to " + print_directory->string());
        }
        return status;
    } catch (const std::exception& error) {
        std::cerr << cli_program_name(frontend) << ": " << error.what() << '\n';
        return 1;
    }
}

}  // namespace mcpp
