#include "mcpp/app.hpp"
#include "mcpp/config.hpp"

#include <charconv>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void usage() {
    std::cout << "mcppedit " MCPP_VERSION "\n"
              << "Usage: mcppedit [options] [+line] file [file ...]\n"
              << "  -h, --help     Show this help\n"
              << "  -V, --version  Show the version\n"
              << "  +line          Start at this line in the first file\n"
              << "  --             Treat remaining arguments as file names\n";
}

std::size_t line_number(const std::string_view value) {
    std::size_t result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || result == 0U) {
        throw std::invalid_argument("line numbers must be positive integers");
    }
    return result;
}

}  // namespace

int main(const int argc, char** argv) {
    try {
        std::vector<std::string> files;
        std::size_t first_line = 0;
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
            else if (options && argument.starts_with('+')) first_line = line_number(std::string_view(argument).substr(1));
            else if (options && argument.starts_with('-')) throw std::invalid_argument("unknown option: " + argument);
            else if (argument.empty()) throw std::invalid_argument("file name is empty");
            else files.push_back(argument);
        }
        if (files.empty()) throw std::invalid_argument("a file name is required; use --help for usage");
        auto config = mcpp::Config::load(mcpp::default_config_path());
        const auto current = std::filesystem::current_path().string();
        mcpp::App app(current, current, std::move(config));
        app.open_editors(files, first_line);
        return app.run();
    } catch (const std::exception& error) {
        std::cerr << "mcppedit: " << error.what() << '\n';
        return 1;
    }
}
