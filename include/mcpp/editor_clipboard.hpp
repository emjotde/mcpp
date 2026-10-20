#pragma once

#include "mcpp/cancellation.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace mcpp {

[[nodiscard]] std::filesystem::path editor_clipboard_path();
void save_editor_clipboard(
    const std::filesystem::path& path,
    std::string_view content,
    bool column = false
);
[[nodiscard]] std::string load_editor_clipboard(
    const std::filesystem::path& path,
    bool* column = nullptr
);
void save_editor_clipboard(
    const std::filesystem::path& path,
    std::string_view content,
    bool column,
    const std::optional<std::string>& command,
    const std::filesystem::path& working_directory,
    const Cancellation& cancellation = {}
);
[[nodiscard]] std::string load_editor_clipboard(
    const std::filesystem::path& path,
    bool* column,
    const std::optional<std::string>& command,
    const std::filesystem::path& working_directory,
    const Cancellation& cancellation = {}
);

}  // namespace mcpp
