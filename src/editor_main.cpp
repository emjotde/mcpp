#include "mcpp/app.hpp"
#include "mcpp/config.hpp"

#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void usage() {
    std::cout << "mcppedit " MCPP_VERSION "\n"
              << "Usage: mcppedit [options] [+line] [file[:line[:]] ...]\n"
              << "  -h, --help     Show this help\n"
              << "  -V, --version  Show the version\n"
              << "  -S, --skin <name>  Select a skin for this run\n"
              << "  +line          Start at this line in the first file\n"
              << "  --             Treat remaining arguments as file names\n"
              << "With no files, open an unnamed buffer.\n";
}

}  // namespace

int main(const int argc, char** argv) {
    try {
        std::vector<mcpp::EditorFile> files;
        std::size_t first_line = 0;
        std::optional<std::string> skin;
        bool options = true;
        for (int index = 1; index < argc; ++index) {
            const std::string argument(argv[index]);
            if (options && (argument == "--help" || argument == "-h")) {
                usage();
                return 0;
            }
            if (options && (argument == "--version" || argument == "-V")) {
                std::cout << "mcppedit " MCPP_VERSION "\n";
                return 0;
            }
            if (options && argument == "--") options = false;
            else if (options && (argument == "-S" || argument == "--skin")) {
                if (++index >= argc) throw std::invalid_argument(argument + " requires a value");
                skin = argv[index];
            }
            else if (options && argument.starts_with("--skin=")) skin = argument.substr(7);
            else if (options && argument.starts_with('+')) first_line = mcpp::parse_line_number(std::string_view(argument).substr(1));
            else if (options && argument.starts_with('-')) throw std::invalid_argument("unknown option: " + argument);
            else if (argument.empty()) throw std::invalid_argument("file name is empty");
            else files.push_back(mcpp::parse_editor_file(argument));
        }
        auto config = mcpp::Config::load(mcpp::default_config_path());
        config.theme_override = std::move(skin);
        const auto current = std::filesystem::current_path().string();
        mcpp::App app(current, current, std::move(config));
        app.open_editor_files(files, first_line);
        return app.run();
    } catch (const std::exception& error) {
        std::cerr << "mcppedit: " << error.what() << '\n';
        return 1;
    }
}
