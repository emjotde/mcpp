#include "mcpp/skin_glyphs.hpp"

#include <ftxui/screen/string.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <stdexcept>

namespace mcpp {
namespace {

struct GlyphKey {
    std::string_view section;
    std::string_view key;
    SkinGlyph role;
    unsigned char priority{2};
};

constexpr std::array keys{
    GlyphKey{"Lines", "horiz", SkinGlyph::horiz},
    GlyphKey{"Lines", "vert", SkinGlyph::vert},
    GlyphKey{"Lines", "lefttop", SkinGlyph::lefttop},
    GlyphKey{"Lines", "righttop", SkinGlyph::righttop},
    GlyphKey{"Lines", "leftbottom", SkinGlyph::leftbottom},
    GlyphKey{"Lines", "rightbottom", SkinGlyph::rightbottom},
    GlyphKey{"Lines", "topmiddle", SkinGlyph::topmiddle},
    GlyphKey{"Lines", "bottommiddle", SkinGlyph::bottommiddle},
    GlyphKey{"Lines", "leftmiddle", SkinGlyph::leftmiddle},
    GlyphKey{"Lines", "rightmiddle", SkinGlyph::rightmiddle},
    GlyphKey{"Lines", "cross", SkinGlyph::cross},
    GlyphKey{"Lines", "dhoriz", SkinGlyph::dhoriz},
    GlyphKey{"Lines", "dvert", SkinGlyph::dvert},
    GlyphKey{"Lines", "dlefttop", SkinGlyph::dlefttop},
    GlyphKey{"Lines", "drighttop", SkinGlyph::drighttop},
    GlyphKey{"Lines", "dleftbottom", SkinGlyph::dleftbottom},
    GlyphKey{"Lines", "drightbottom", SkinGlyph::drightbottom},
    GlyphKey{"Lines", "dtopmiddle", SkinGlyph::dtopmiddle},
    GlyphKey{"Lines", "dbottommiddle", SkinGlyph::dbottommiddle},
    GlyphKey{"Lines", "dleftmiddle", SkinGlyph::dleftmiddle},
    GlyphKey{"Lines", "drightmiddle", SkinGlyph::drightmiddle},
    GlyphKey{"widget-panel", "sort-up-char", SkinGlyph::sort_up},
    GlyphKey{"widget-panel", "sort-down-char", SkinGlyph::sort_down},
    GlyphKey{"widget-common", "sort-sign-up", SkinGlyph::sort_up, 1},
    GlyphKey{"widget-common", "sort-sign-down", SkinGlyph::sort_down, 1},
    GlyphKey{"widget-editor", "window-state-char", SkinGlyph::window_state},
    GlyphKey{"widget-editor", "window-close-char", SkinGlyph::window_close},
    GlyphKey{"editor", "window-state-char", SkinGlyph::window_state, 1},
    GlyphKey{"editor", "window-close-char", SkinGlyph::window_close, 1},
};

std::optional<std::uint32_t> scalar(const std::string_view value) {
    if (value.empty()) return {};
    const auto first = static_cast<unsigned char>(value.front());
    std::size_t length{};
    std::uint32_t result{};
    std::uint32_t minimum{};
    if (first < 0x80U) {
        length = 1U;
        result = first;
    } else if (first >= 0xc2U && first <= 0xdfU) {
        length = 2U;
        result = first & 0x1fU;
        minimum = 0x80U;
    } else if (first >= 0xe0U && first <= 0xefU) {
        length = 3U;
        result = first & 0x0fU;
        minimum = 0x800U;
    } else if (first >= 0xf0U && first <= 0xf4U) {
        length = 4U;
        result = first & 0x07U;
        minimum = 0x10000U;
    } else return {};
    if (value.size() != length) return {};
    for (std::size_t index = 1U; index < length; ++index) {
        const auto byte = static_cast<unsigned char>(value[index]);
        if ((byte & 0xc0U) != 0x80U) return {};
        result = (result << 6U) | (byte & 0x3fU);
    }
    if (result < minimum || result > 0x10ffffU || (result >= 0xd800U && result <= 0xdfffU)) return {};
    return result;
}

bool unsafe_scalar(const std::uint32_t value) {
    return value < 0x20U || (value >= 0x7fU && value <= 0x9fU) || value == 0xadU ||
        (value >= 0x600U && value <= 0x605U) || value == 0x61cU || value == 0x6ddU ||
        value == 0x70fU || (value >= 0x890U && value <= 0x891U) || value == 0x8e2U ||
        value == 0x180eU || (value >= 0x200bU && value <= 0x200fU) ||
        (value >= 0x2028U && value <= 0x202eU) || (value >= 0x2060U && value <= 0x206fU) ||
        value == 0xfeffU || (value >= 0xfff9U && value <= 0xfffbU) ||
        value == 0x110bdU || value == 0x110cdU || (value >= 0x13430U && value <= 0x1345fU) ||
        (value >= 0x1bca0U && value <= 0x1bca3U) || (value >= 0x1d173U && value <= 0x1d17aU) ||
        (value >= 0xe0000U && value <= 0xe007fU) || (value >= 0xfdd0U && value <= 0xfdefU) ||
        (value & 0xffffU) >= 0xfffeU;
}

}  // namespace

bool SkinGlyphs::has(const SkinGlyph role) const {
    return role != SkinGlyph::count && !values[static_cast<std::size_t>(role)].empty();
}

bool SkinGlyphs::has_lines() const {
    return std::ranges::any_of(values.begin(), values.begin() + static_cast<std::size_t>(SkinGlyph::sort_up),
                              [](const auto& value) { return !value.empty(); });
}

bool SkinGlyphs::has_double_lines() const {
    return std::ranges::any_of(values.begin() + static_cast<std::size_t>(SkinGlyph::dhoriz),
                              values.begin() + static_cast<std::size_t>(SkinGlyph::sort_up),
                              [](const auto& value) { return !value.empty(); });
}

SkinGlyphSetting set_skin_glyph(
    SkinGlyphs& glyphs, const std::string_view section, const std::string_view key,
    const std::string_view value
) {
    const auto found = std::ranges::find_if(keys, [&](const auto& candidate) {
        return candidate.section == section && candidate.key == key;
    });
    if (found == keys.end()) {
        return section == "Lines" || section.starts_with("widget-") ||
                (section == "editor" && key.starts_with("window-") && key.ends_with("-char"))
            ? SkinGlyphSetting::unsupported : SkinGlyphSetting::color;
    }
    if (!value.empty()) {
        const auto codepoint = scalar(value);
        if (!codepoint || unsafe_scalar(*codepoint) || ftxui::string_width(value) != 1) {
            throw std::invalid_argument("[" + std::string(section) + "] " + std::string(key) +
                " requires one printable, single-cell UTF-8 character");
        }
    }
    const auto index = static_cast<std::size_t>(found->role);
    if (found->priority >= glyphs.priorities[index]) {
        glyphs.values[index] = value;
        glyphs.priorities[index] = found->priority;
    }
    return SkinGlyphSetting::supported;
}

std::string_view skin_glyph(
    const SkinGlyphs& glyphs, const SkinGlyph role, const std::string_view fallback,
    const bool stickchars
) {
    if (stickchars) {
        switch (role) {
            case SkinGlyph::horiz: case SkinGlyph::dhoriz: return "-";
            case SkinGlyph::vert: case SkinGlyph::dvert: return "|";
            case SkinGlyph::sort_up: return "^";
            case SkinGlyph::sort_down: return "v";
            case SkinGlyph::window_state: return "*";
            case SkinGlyph::window_close: return "X";
            case SkinGlyph::count: return fallback;
            default: return "+";
        }
    }
    if (role == SkinGlyph::count) return fallback;
    const auto& value = glyphs.values[static_cast<std::size_t>(role)];
    return value.empty() ? fallback : std::string_view(value);
}

}  // namespace mcpp
