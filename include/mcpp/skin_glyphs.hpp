#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace mcpp {

enum class SkinGlyph {
    horiz, vert, lefttop, righttop, leftbottom, rightbottom,
    topmiddle, bottommiddle, leftmiddle, rightmiddle, cross,
    dhoriz, dvert, dlefttop, drighttop, dleftbottom, drightbottom,
    dtopmiddle, dbottommiddle, dleftmiddle, drightmiddle,
    sort_up, sort_down, window_state, window_close,
    count
};

struct SkinGlyphs {
    std::array<std::string, static_cast<std::size_t>(SkinGlyph::count)> values;
    std::array<unsigned char, static_cast<std::size_t>(SkinGlyph::count)> priorities{};

    [[nodiscard]] bool has(SkinGlyph role) const;
    [[nodiscard]] bool has_lines() const;
    [[nodiscard]] bool has_double_lines() const;
};

enum class SkinGlyphSetting { color, supported, unsupported };

[[nodiscard]] SkinGlyphSetting set_skin_glyph(
    SkinGlyphs& glyphs, std::string_view section, std::string_view key, std::string_view value
);
[[nodiscard]] std::string_view skin_glyph(
    const SkinGlyphs& glyphs, SkinGlyph role, std::string_view fallback, bool stickchars = false
);

}  // namespace mcpp
