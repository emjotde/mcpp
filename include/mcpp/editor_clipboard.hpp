#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace mcpp {

[[nodiscard]] std::filesystem::path editor_clipboard_path();
void save_editor_clipboard(
    const std::filesystem::path& path,
    std::string_view content
);
[[nodiscard]] std::string load_editor_clipboard(
    const std::filesystem::path& path
);

}  // namespace mcpp
