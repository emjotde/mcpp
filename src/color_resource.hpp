#pragma once

#include <ftxui/screen/color.hpp>

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mcpp::detail {

struct ColorSetting {
    std::string key;
    std::string value;
    std::size_t line{0};
};

struct ColorSection {
    std::string name;
    std::size_t line{0};
    std::vector<ColorSetting> settings;

    [[nodiscard]] const ColorSetting* find(std::string_view key) const;
};

[[nodiscard]] std::string_view color_trim(std::string_view value);
[[nodiscard]] ftxui::Color parse_color_value(
    std::string_view value, std::optional<ftxui::Color> base = {}
);
[[nodiscard]] unsigned parse_color_attributes(std::string_view value);
[[noreturn]] void color_resource_error(
    const std::filesystem::path& path, std::size_t line, std::string_view message
);
[[nodiscard]] std::vector<ColorSection> read_color_resource(
    const std::filesystem::path& path, bool allow_global = false
);
[[nodiscard]] bool color_boolean(
    const ColorSetting& setting, const std::filesystem::path& path
);
[[nodiscard]] bool color_resource_exists(const std::filesystem::path& path);

}  // namespace mcpp::detail
