#include "mcpp/app.hpp"
#include "mcpp/config.hpp"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

int main(const int argc, char** argv) {
    try {
        std::vector<std::string> files;
        bool options = true;
        for (int index = 1; index < argc; ++index) {
            const std::string argument(argv[index]);
            if (options && (argument == "--help" || argument == "-h")) {
                std::cout << "mcppview " MCPP_VERSION "\n"
                          << "Usage: mcppview [options] file\n"
                          << "  -h, --help     Show this help\n"
                          << "  -V, --version  Show the version\n"
                          << "  --             Treat remaining arguments as file names\n";
                return 0;
            }
            if (options && (argument == "--version" || argument == "-V")) {
                std::cout << "mcppview " MCPP_VERSION "\n";
                return 0;
            }
            if (options && argument == "--") options = false;
            else if (options && argument.starts_with('-')) throw std::invalid_argument("unknown option: " + argument);
            else if (argument.empty()) throw std::invalid_argument("file name is empty");
            else files.push_back(argument);
        }
        if (files.size() != 1U) throw std::invalid_argument("expected one file; use --help for usage");
        auto config = mcpp::Config::load(mcpp::default_config_path());
        const auto current = std::filesystem::current_path().string();
        mcpp::App app(current, current, std::move(config));
        app.open_viewer(files.front());
        return app.run();
    } catch (const std::exception& error) {
        std::cerr << "mcppview: " << error.what() << '\n';
        return 1;
    }
}
