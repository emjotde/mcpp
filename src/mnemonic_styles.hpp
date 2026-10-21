#pragma once

#include "mcpp/skin.hpp"
#include "mcpp/skin_glyphs_ui.hpp"
#include "viewer_help_styles.hpp"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/string.hpp>

#include <optional>
#include <string>
#include <utility>

namespace mcpp::detail {

inline const SkinPair* dialog_mnemonic_style(
    const Skin& skin, const bool focused, const bool error = false
) {
    if (error) {
        // Keep errdhotfocus's existing whole-error-focus CLI alias unchanged.
        return focused ? nullptr : find_skin_color(skin, "error", "errdhotnormal");
    }
    return find_skin_color(skin, "dialog", focused ? "dhotfocus" : "dhotnormal");
}

inline const SkinPair* menu_mnemonic_style(const Skin& skin, const bool selected) {
    return find_skin_color(skin, "menu", selected ? "menuhotsel" : "menuhot");
}

inline ftxui::Element render_mnemonic_label(
    const std::string& label, const std::size_t index,
    const SkinPair* colors = nullptr, const bool emphasize_missing = true,
    const std::optional<ftxui::Color> foreground = std::nullopt,
    const bool outer_inverted = false
) {
    using namespace ftxui;
    std::size_t offset = 0;
    for (const auto& glyph : Utf8ToGlyphs(label)) {
        if (offset == index) {
            auto mnemonic = glyph_text(glyph);
            if (colors) {
                auto hot = *colors;
                // The surrounding focus decorator toggles inversion after painting.
                if (outer_inverted && hot.explicit_style) hot.attributes ^= SkinPair::reverse;
                mnemonic = paint_skin_style(std::move(mnemonic), hot);
            } else {
                if (emphasize_missing) mnemonic = mnemonic | underlined | bold;
                if (foreground) mnemonic = mnemonic | color(*foreground);
            }
            return hbox({
                glyph_text(label.substr(0, offset)),
                std::move(mnemonic),
                glyph_text(label.substr(offset + glyph.size())),
            });
        }
        offset += glyph.size();
        if (offset > index) break;
    }
    return glyph_text(label);
}

}  // namespace mcpp::detail
