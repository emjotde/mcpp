#include "mcpp/app.hpp"
#include "mcpp/config.hpp"

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
            << "  -e, --edit <file>    Open editor\n"
            << "  -P, --printwd <file> Write final directory\n";
        return 0;
    }
    if (!arguments.empty() && arguments[0] == "--version") {
        std::cout << "mcpp " MCPP_VERSION << "\n";
        return 0;
    }
    try {
        std::optional<std::string> view_file;
        std::optional<std::string> edit_file;
        std::optional<std::filesystem::path> print_directory;
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
            if (argument == "-v" || argument == "--view") {
                view_file = take_value();
            } else if (argument == "-e" || argument == "--edit") {
                edit_file = take_value();
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
        if (paths.size() > 2U) {
            throw std::invalid_argument(
                "expected at most two pane paths"
            );
        }
        if (view_file && edit_file) {
            throw std::invalid_argument(
                "--view and --edit cannot be combined"
            );
        }
        auto config = mcpp::Config::load(mcpp::default_config_path());
        const auto current = std::filesystem::current_path();
        const auto pane_path = [&](const std::string& value) {
            const auto path = std::filesystem::path(value);
            return (
                path.is_absolute() ? path : current / path
            ).lexically_normal().string();
        };
        const auto saved_path = [&](const std::string& value, const char* panel) {
            if (!config.auto_save_panels_setup || value.empty()) return current.string();
            const auto path = pane_path(value);
            std::error_code error;
            if (std::filesystem::is_directory(path, error) && !error) return path;
            std::cerr << "mcpp: cannot restore saved " << panel << " panel; using the current directory\n";
            return current.string();
        };
        const auto left =
            paths.empty()
                ? saved_path(config.saved_left_path, "left")
                : pane_path(paths[0]);
        const auto right =
            paths.size() < 2U
                ? saved_path(config.saved_right_path, "right")
                : pane_path(paths[1]);
        mcpp::App app(left, right, std::move(config));
        if (view_file) {
            app.open_viewer(*view_file);
        } else if (edit_file) {
            app.open_editor(*edit_file);
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
