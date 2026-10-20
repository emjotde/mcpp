#pragma once

#include "mcpp/skin.hpp"

#include <ftxui/dom/node.hpp>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mcpp {

struct Config;

enum class CliPaletteContext { panels, editor, viewer, help, diff };

struct CliColorOverride {
    std::string role;
    std::optional<ftxui::Color> foreground;
    std::optional<ftxui::Color> background;
    std::optional<unsigned> attributes;
};

[[nodiscard]] std::vector<CliColorOverride> parse_cli_colors(std::span<const std::string> values);
[[nodiscard]] std::vector<std::vector<CliColorOverride>> parse_config_colors(
    const Config& config, std::string_view terminal,
    std::optional<std::string_view> environment = {}
);
[[nodiscard]] std::optional<bool> configured_color_mode(const Config& config, std::optional<bool> color);
void apply_cli_colors(
    Skin& skin, std::span<const CliColorOverride> overrides, CliPaletteContext context
);
void prepare_cli_terminal(std::optional<bool> color, bool stickchars);
[[nodiscard]] ftxui::Element cli_display(
    ftxui::Element element, bool monochrome, bool stickchars,
    std::span<const Skin* const> palettes
);

}  // namespace mcpp
