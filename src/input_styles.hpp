#pragma once

#include "mcpp/skin.hpp"

#include <ftxui/dom/node.hpp>

namespace mcpp::detail {

[[nodiscard]] ftxui::Element input_field_style(
    ftxui::Element element, const Skin& skin, bool active, bool pristine
);
[[nodiscard]] ftxui::Element input_selection_style(
    ftxui::Element element, const Skin& skin, bool pristine
);
[[nodiscard]] ftxui::Element command_input_selection_style(
    ftxui::Element element, const Skin& skin, bool active
);

}  // namespace mcpp::detail
