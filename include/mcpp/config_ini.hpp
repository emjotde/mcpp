#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mcpp {

struct ConfigIniLine {
    std::string raw;
    std::string section;
    std::string key;
    std::string value;
    std::size_t number{0};
    bool applied{false};
};

struct ConfigIniDocument {
    std::filesystem::path path;
    std::vector<ConfigIniLine> lines;
    bool native{false};
    bool present{false};
};

struct ConfigSource {
    ConfigIniDocument primary;
    std::optional<ConfigIniDocument> panels;
};

[[nodiscard]] ConfigIniDocument read_config_document(const std::filesystem::path& path);
[[nodiscard]] std::optional<std::pair<std::string, std::string>>
translate_config_setting(const ConfigIniLine& line, bool native);
[[nodiscard]] std::string config_setting_location(
    const ConfigIniDocument& document, const ConfigIniLine& line);
[[nodiscard]] std::string serialize_config_document(
    const ConfigIniDocument& document, std::string_view flat,
    bool panels_only = false, bool separate_panels = false);

}  // namespace mcpp
