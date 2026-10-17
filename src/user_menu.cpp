#include "mcpp/user_menu.hpp"

#include <cstdlib>
#include <fstream>

namespace mcpp {
namespace {

std::string shell_quote(const std::string_view value) {
    std::string result{"'"};
    for (const auto character : value) {
        if (character == '\'') {
            result += "'\\''";
        } else {
            result += character;
        }
    }
    result += '\'';
    return result;
}

void replace_all(
    std::string& value,
    const std::string_view token,
    const std::string& replacement
) {
    std::size_t offset = 0;
    while ((offset = value.find(token, offset)) !=
           std::string::npos) {
        value.replace(offset, token.size(), replacement);
        offset += replacement.size();
    }
}

}  // namespace

std::vector<UserMenuEntry> load_user_menu(
    const std::filesystem::path& path
) {
    std::ifstream input(path);
    if (!input) {
        return {
            {
                .key = 'i',
                .label = "Inspect selected path",
                .command = "stat -- %f",
            },
            {
                .key = 'd',
                .label = "Disk usage of selected path",
                .command = "du -sh -- %f",
            },
            {
                .key = 't',
                .label = "Show directory tree",
                .command = "find %d -maxdepth 2 -type d -print",
            },
        };
    }
    std::vector<UserMenuEntry> result;
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line.starts_with('#')) {
            continue;
        }
        const auto first = line.find('|');
        const auto second =
            first == std::string::npos
                ? std::string::npos
                : line.find('|', first + 1U);
        if (first != 1U || second == std::string::npos ||
            second + 1U >= line.size()) {
            continue;
        }
        result.push_back({
            .key = line[0],
            .label = line.substr(first + 1U, second - first - 1U),
            .command = line.substr(second + 1U),
        });
    }
    return result;
}

std::filesystem::path default_user_menu_path() {
    if (const auto* value = std::getenv("XDG_CONFIG_HOME");
        value != nullptr && *value != '\0') {
        return std::filesystem::path(value) / "mcpp" / "menu";
    }
    if (const auto* value = std::getenv("HOME");
        value != nullptr && *value != '\0') {
        return std::filesystem::path(value) /
               ".config" / "mcpp" / "menu";
    }
    return std::filesystem::path(".mcpp-menu");
}

std::string expand_user_menu_command(
    const std::string_view command,
    const std::string_view selected_path,
    const std::string_view current_directory,
    const std::string_view other_directory
) {
    auto result = std::string(command);
    replace_all(result, "%f", shell_quote(selected_path));
    replace_all(result, "%d", shell_quote(current_directory));
    replace_all(result, "%D", shell_quote(other_directory));
    return result;
}

}  // namespace mcpp
