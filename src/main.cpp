#include "mcpp/app.hpp"
#include "mcpp/config.hpp"
#include "mcpp/local_provider.hpp"
#include "mcpp/location.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

int main(const int argc, char** argv) {
    std::vector<std::string> arguments;
    arguments.reserve(static_cast<std::size_t>(argc > 0 ? argc - 1 : 0));
    for (int index = 1; index < argc; ++index) {
        arguments.emplace_back(argv[index]);
    }
    if (!arguments.empty() &&
        (arguments[0] == "--help" || arguments[0] == "-h")) {
        std::cout
            << "mcpp " MCPP_VERSION "\n"
            << "Usage: mcpp [options] [left-directory] "
               "[right-directory]\n"
            << "  -v, --view <file>    Open viewer\n"
            << "  -e, --edit [files]   Open editor (unnamed if no files)\n"
            << "                      Files accept file:line or file:line:\n"
            << "  -S, --skin <name>   Select a skin for this run\n"
            << "  -K, --keymap <file> Load MC-format key bindings\n"
            << "      --nokeymap     Use built-in key bindings only\n"
            << "  -U, --subshell      Enable persistent Bash for this run\n"
            << "  -u, --nosubshell    Disable persistent Bash for this run\n"
            << "  -P, --printwd <file> Write final directory\n";
        return 0;
    }
    if (!arguments.empty() && (arguments[0] == "--version" || arguments[0] == "-V")) {
        std::cout << "mcpp " MCPP_VERSION << "\n";
        return 0;
    }
    try {
        std::optional<std::string> view_file;
        bool edit = false;
        std::size_t first_line = 0;
        std::optional<std::filesystem::path> print_directory;
        std::optional<std::string> skin;
        std::optional<std::string> keymap;
        bool keymap_disabled = false;
        std::optional<bool> subshell;
        std::vector<std::string> paths;
        for (std::size_t index = 0; index < arguments.size(); ++index) {
            const auto& argument = arguments[index];
            const auto take_value = [&]() -> std::string {
                if (++index >= arguments.size()) {
                    throw std::invalid_argument(
                        argument + " requires a value"
                    );
                }
                return arguments[index];
            };
            if (argument == "-S" || argument == "--skin") {
                skin = take_value();
            } else if (argument.starts_with("--skin=")) {
                skin = argument.substr(7);
            } else if (argument == "-K" || argument == "--keymap") {
                keymap = take_value();
                if (keymap->empty()) throw std::invalid_argument("keymap file name is empty");
                keymap_disabled = false;
            } else if (argument.starts_with("--keymap=")) {
                keymap = argument.substr(9);
                if (keymap->empty()) throw std::invalid_argument("keymap file name is empty");
                keymap_disabled = false;
            } else if (argument == "--nokeymap") {
                keymap_disabled = true;
            } else if (argument == "-U" || argument == "--subshell") {
                subshell = true;
            } else if (argument == "-u" || argument == "--nosubshell") {
                subshell = false;
            } else if (argument == "-v" || argument == "--view") {
                view_file = take_value();
            } else if (argument == "-e" || argument == "--edit") {
                edit = true;
            } else if (argument.starts_with("--edit=")) {
                edit = true;
                paths.push_back(argument.substr(7));
            } else if (edit && argument.starts_with('+')) {
                first_line = mcpp::parse_line_number(std::string_view(argument).substr(1));
            } else if (argument == "-P" ||
                       argument == "--printwd") {
                print_directory = take_value();
            } else if (argument == "--") {
                paths.insert(
                    paths.end(),
                    arguments.begin() +
                        static_cast<std::ptrdiff_t>(index + 1U),
                    arguments.end()
                );
                break;
            } else if (argument.starts_with('-')) {
                throw std::invalid_argument(
                    "unknown option: " + argument
                );
            } else {
                paths.push_back(argument);
            }
        }
        if (!edit && paths.size() > 2U) {
            throw std::invalid_argument(
                "expected at most two pane paths"
            );
        }
        if (view_file && edit) {
            throw std::invalid_argument(
                "--view and --edit cannot be combined"
            );
        }
        auto config = mcpp::Config::load(mcpp::default_config_path());
        config.theme_override = std::move(skin);
        config.keymap_override = std::move(keymap);
        config.keymap_disabled = keymap_disabled;
        config.subshell_override = subshell;
        const auto current = std::filesystem::current_path();
        const auto pane_path = [&](const std::string& value) {
            if (mcpp::is_archive_location(value)) return value;
            const auto path = std::filesystem::path(value);
            return (
                path.is_absolute() ? path : current / path
            ).lexically_normal().string();
        };
        const auto saved_path = [&](const std::string& value, const char* panel) {
            if (!config.auto_save_panels_setup || value.empty()) return current.string();
            const auto path = pane_path(value);
            if (mcpp::is_archive_location(path)) {
                try {
                    const auto location = mcpp::open_location(path, std::make_shared<mcpp::LocalProvider>());
                    if (location.provider->stat_follow(location.path, {}).kind == mcpp::EntryKind::directory) return path;
                } catch (const std::exception& error) {
                    std::cerr << "mcpp: saved " << panel << " panel: " << error.what() << '\n';
                }
            }
            std::error_code error;
            if (std::filesystem::is_directory(path, error) && !error) return path;
            std::cerr << "mcpp: cannot restore saved " << panel << " panel; using the current directory\n";
            return current.string();
        };
        const auto left =
            edit || paths.empty()
                ? saved_path(config.saved_left_path, "left")
                : pane_path(paths[0]);
        const auto right =
            edit || paths.size() < 2U
                ? saved_path(config.saved_right_path, "right")
                : pane_path(paths[1]);
        mcpp::App app(left, right, std::move(config));
        if (view_file) {
            if (view_file->empty()) throw std::invalid_argument("file name is empty");
            app.open_viewer(pane_path(*view_file));
        } else if (edit) {
            std::vector<mcpp::EditorFile> files;
            for (const auto& path : paths) {
                auto file = mcpp::parse_editor_file(path);
                file.path = pane_path(file.path);
                files.push_back(std::move(file));
            }
            app.open_editor_files(files, first_line);
        }
        const auto status = app.run();
        if (print_directory) {
            std::ofstream output(*print_directory, std::ios::trunc);
            if (!output) {
                throw std::runtime_error(
                    "cannot write final directory"
                );
            }
            output << app.active_path() << '\n';
        }
        return status;
    } catch (const std::exception& error) {
        std::cerr << "mcpp: " << error.what() << '\n';
        return 1;
    }
}
