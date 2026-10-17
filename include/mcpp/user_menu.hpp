#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace mcpp {

struct UserMenuEntry {
    char key{'?'};
    std::string label;
    std::string command;
};

[[nodiscard]] std::vector<UserMenuEntry> load_user_menu(
    const std::filesystem::path& path
);
[[nodiscard]] std::filesystem::path default_user_menu_path();
[[nodiscard]] std::string expand_user_menu_command(
    std::string_view command,
    std::string_view selected_path,
    std::string_view current_directory,
    std::string_view other_directory
);

}  // namespace mcpp
