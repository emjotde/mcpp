#pragma once

#include "mcpp/native_menu.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace mcpp {

using UserMenuEntry = NativeMenuEntry;

[[nodiscard]] std::vector<UserMenuEntry> load_user_menu(
    const std::filesystem::path& path,
    bool shell_patterns = true
);
[[nodiscard]] std::filesystem::path default_user_menu_path();
[[nodiscard]] std::filesystem::path default_editor_user_menu_path();
[[nodiscard]] std::filesystem::path find_user_menu_path(
    const std::filesystem::path& local_directory,
    bool editor
);
[[nodiscard]] std::string expand_user_menu_command(
    std::string_view command,
    std::string_view selected_path,
    std::string_view current_directory,
    std::string_view other_directory
);

}  // namespace mcpp
