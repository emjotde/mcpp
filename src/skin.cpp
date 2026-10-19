#include "mcpp/skin.hpp"

#include <ftxui/screen/terminal.hpp>

#include <algorithm>
#include <cstdint>
#include <map>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace mcpp {
namespace {

ftxui::Color rgb(const std::uint32_t value) {
    return ftxui::Color::RGB(static_cast<std::uint8_t>(value >> 16U),
                            static_cast<std::uint8_t>(value >> 8U),
                            static_cast<std::uint8_t>(value));
}

Skin colored_skin(
    std::string id, const std::uint32_t background, const std::uint32_t foreground,
    const std::uint32_t surface, const std::uint32_t surface_foreground,
    const std::uint32_t accent, const std::uint32_t accent_foreground,
    const std::uint32_t muted, const bool light = false, const bool monochrome = false
) {
    Skin skin;
    skin.id = std::move(id);
    skin.label = skin.id;
    skin.light = light;
    skin.normal = {rgb(foreground), rgb(background)};
    skin.menu = skin.dialog = {rgb(surface_foreground), rgb(surface)};
    skin.menu_selected = skin.selected = skin.marked_selected =
        skin.search = skin.viewer_cursor = skin.key_number = {rgb(accent_foreground), rgb(accent)};
    skin.inactive_selected = skin.diff_changed = {rgb(surface_foreground), rgb(surface)};
    skin.inactive_marked_selected = skin.inactive_selected;
    skin.error = {rgb(0xfff2f4), rgb(light ? 0x8a2638 : 0x712335)};
    skin.hint = {rgb(surface_foreground), rgb(surface)};
    skin.key_label = {rgb(foreground), rgb(surface)};
    skin.progress = {rgb(accent_foreground), rgb(accent)};
    skin.bright = skin.column_header = skin.directory = rgb(accent);
    skin.dim = skin.inactive_title = skin.symlink = skin.special = skin.disabled = rgb(muted);
    skin.executable = skin.inactive_tree = rgb(foreground);
    skin.shadow = rgb(light ? 0x87939b : 0x06090d);
    if (light) {
        skin.syntax = {rgb(foreground), rgb(0x174c91), rgb(0x006452),
                       rgb(0x993c22), rgb(0x4b672e), rgb(0x6f3f89), rgb(0x7b257a)};
    }
    if (monochrome) {
        skin.error = {rgb(accent_foreground), rgb(accent)};
        skin.syntax = {rgb(foreground), rgb(accent), rgb(foreground),
                       rgb(foreground), rgb(muted), rgb(accent), rgb(accent)};
    }
    return skin;
}

}  // namespace

const std::vector<Skin>& builtin_skins() {
    static std::mutex mutex;
    static std::map<ftxui::Terminal::Color, std::vector<Skin>> palettes;
    const auto support = ftxui::Terminal::ColorSupport();
    const std::lock_guard lock(mutex);
    if (const auto found = palettes.find(support); found != palettes.end()) return found->second;
    auto skins = [] {
        Skin gray;
        gray.id = "grayscale";
        gray.label = "gray";
        Skin classic = gray;
        classic.id = "default";
        classic.label = "< Default >";
        classic.normal.background = rgb(0x000080);
        classic.menu = classic.selected = {ftxui::Color::Grey0, rgb(0x00a0a0)};
        classic.marked_selected = {ftxui::Color::Grey0, rgb(0x00a0a0)};
        classic.inactive_selected = {ftxui::Color::Grey93, rgb(0x0064a0)};
        classic.inactive_marked_selected = {ftxui::Color::YellowLight, rgb(0x0064a0)};
        return std::vector<Skin>{
            std::move(classic), std::move(gray),
            colored_skin("amber",    0x171206, 0xf5cd73, 0x31250c, 0xffe1a1, 0xdfad3b, 0x1f1605, 0xb89857, false, true),
            colored_skin("cobalt",   0x081b36, 0xe2edff, 0x153a63, 0xecf4ff, 0x6fbbff, 0x061629, 0x93abc7),
            colored_skin("contrast", 0x000000, 0xffffff, 0x121212, 0xffffff, 0xffff00, 0x000000, 0xc0c0c0),
            colored_skin("ember",    0x2b2019, 0xf5eadb, 0x4c3727, 0xfff1df, 0xf1b777, 0x301b08, 0xc7ad94),
            colored_skin("forest",   0x14251b, 0xe5efdf, 0x2c4533, 0xf0f6e7, 0x9fd188, 0x13220b, 0xa1b89a),
            colored_skin("graphite", 0x1c2026, 0xe4e7eb, 0x343b45, 0xf2f4f6, 0xb7c4d6, 0x17202c, 0xa0a6af),
            colored_skin("green",    0x07170d, 0xaee3b7, 0x183523, 0xd4f2d3, 0x75d98f, 0x06210e, 0x82ad8c, false, true),
            colored_skin("ice",      0xedf5f8, 0x193747, 0xd0e3ec, 0x183341, 0x25667c, 0xf6fcff, 0x4b6b79, true),
            colored_skin("midnight", 0x101725, 0xdce6f1, 0x22334b, 0xe6f1ff, 0x79b8ff, 0x08111d, 0x8fa4bc),
            colored_skin("ocean",    0x0c252b, 0xdef1ec, 0x214249, 0xe0f5f2, 0x77d2c6, 0x08251f, 0x9ab9ba),
            colored_skin("paper",    0xf7f5ee, 0x262b32, 0xe0e6ed, 0x1c2734, 0x365f91, 0xffffff, 0x5a6571, true),
            colored_skin("plum",     0x251b2c, 0xeee4f1, 0x48324e, 0xf8ebfb, 0xd4a2e3, 0x291031, 0xb6a1bd),
            colored_skin("rose",     0x2d1c24, 0xf2e3e9, 0x51333f, 0xffedf3, 0xe9a7bc, 0x300d1b, 0xc0a0ad),
            colored_skin("sand",     0xf1e7d2, 0x3a3326, 0xdfcfaf, 0x342819, 0x775a2b, 0xfff4dd, 0x70634d, true),
        };
    }();
    return palettes.emplace(support, std::move(skins)).first->second;
}

const Skin& find_skin(std::string_view id) {
    if (id == "gray" || id == "type-colors") id = "grayscale";
    if (id == "blue") id = "default";
    const auto& skins = builtin_skins();
    const auto match = std::ranges::find(skins, id, &Skin::id);
    if (match == skins.end()) throw std::invalid_argument("unknown skin: " + std::string(id));
    return *match;
}

}  // namespace mcpp
