#pragma once

#include <optional>
#include <string_view>

namespace mcpp::detail {

enum class PanelGroupSelection { none, select, unselect };

[[nodiscard]] inline PanelGroupSelection panel_group_selection_action(
    const std::string_view action
) noexcept {
    if (action == "Select") return PanelGroupSelection::select;
    if (action == "Unselect") return PanelGroupSelection::unselect;
    return PanelGroupSelection::none;
}

[[nodiscard]] inline PanelGroupSelection panel_group_selection_key(
    const std::string_view key,
    const std::string_view command,
    const std::optional<bool> alternate_plus_minus,
    const std::optional<bool> only_leading_plus_minus
) noexcept {
    if (key == "plus" || key == "minus") {
        if (alternate_plus_minus.value_or(false) ||
            (only_leading_plus_minus.value_or(true) && !command.empty())) {
            return PanelGroupSelection::none;
        }
        return key == "plus" ? PanelGroupSelection::select : PanelGroupSelection::unselect;
    }
    if (alternate_plus_minus.has_value() || only_leading_plus_minus.has_value()) {
        if (key == "kpplus" || key == "alt-plus") return PanelGroupSelection::select;
        if (key == "kpminus" || key == "alt-minus") return PanelGroupSelection::unselect;
    }
    return PanelGroupSelection::none;
}

}  // namespace mcpp::detail
