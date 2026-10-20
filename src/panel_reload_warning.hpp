#pragma once

#include <optional>

namespace mcpp::detail {

template <typename ShowWarning>
void maybe_show_fast_reload_warning(
    const bool previously_enabled,
    const bool enabled,
    std::optional<bool>& saved_shown,
    bool& legacy_shown,
    const ShowWarning& show_warning
) {
    if (!enabled || saved_shown.value_or(legacy_shown || previously_enabled)) return;
    show_warning();
    if (saved_shown.has_value()) saved_shown = true;
    else legacy_shown = true;
}

}  // namespace mcpp::detail
