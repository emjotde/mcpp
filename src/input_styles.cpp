#include "input_styles.hpp"

#include "viewer_help_styles.hpp"

#include <ftxui/dom/elements.hpp>

#include <utility>

namespace mcpp::detail {

ftxui::Element input_field_style(
    ftxui::Element element, const Skin& skin, const bool active, const bool pristine
) {
    const auto* colors = pristine ? find_skin_color(skin, "core", "inputunchanged") : nullptr;
    if (!colors) colors = find_skin_color(skin, "core", "input");
    if (colors) return paint_skin_style(std::move(element), *colors);
    if (!active) return element;

    auto focus = skin.dialog_focus;
    std::swap(focus.foreground, focus.background);
    focus.attributes ^= SkinPair::reverse;
    focus.explicit_style = true;
    // Paint the fallback before children so a selected pair can replace its inverse.
    return paint_skin_style(std::move(element), focus);
}

ftxui::Element input_selection_style(
    ftxui::Element element, const Skin& skin, const bool pristine
) {
    if (pristine && find_skin_color(skin, "core", "inputunchanged")) return element;
    if (!pristine) {
        if (const auto* colors = find_skin_color(skin, "core", "inputmark")) {
            return paint_skin_style(std::move(element), *colors);
        }
    }
    return std::move(element) | ftxui::bold | ftxui::underlined;
}

}  // namespace mcpp::detail
