#pragma once

#include "mcpp/skin_glyphs.hpp"

#include <ftxui/dom/node.hpp>

namespace mcpp {

[[nodiscard]] ftxui::Element glyph_border(ftxui::Element border);
[[nodiscard]] ftxui::Element glyph_text(std::string_view text);
void apply_skin_lines(ftxui::Screen& screen, const SkinGlyphs& glyphs, bool stickchars);

}  // namespace mcpp
