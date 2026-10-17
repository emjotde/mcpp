#include "mcpp/app.hpp"
#include "mcpp/config.hpp"

#include <filesystem>
#include <iostream>
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
            << "Usage: mcpp [left-directory] [right-directory]\n";
        return 0;
    }
    if (!arguments.empty() && arguments[0] == "--version") {
        std::cout << "mcpp " MCPP_VERSION << "\n";
        return 0;
    }
    if (arguments.size() > 2U) {
        std::cerr << "mcpp: expected at most two pane paths\n";
        return 2;
    }
    try {
        auto config = mcpp::Config::load(mcpp::default_config_path());
        const auto current = std::filesystem::current_path();
        const auto left =
            arguments.empty()
                ? (config.left_path.empty() ? current : config.left_path)
                : std::filesystem::path(arguments[0]);
        const auto right =
            arguments.size() < 2U
                ? (config.right_path.empty() ? current : config.right_path)
                : std::filesystem::path(arguments[1]);
        return mcpp::App(left, right, std::move(config)).run();
    } catch (const std::exception& error) {
        std::cerr << "mcpp: " << error.what() << '\n';
        return 1;
    }
}
