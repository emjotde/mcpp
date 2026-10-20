#pragma once

#include <optional>
#include <string_view>

namespace mcpp::detail {

[[nodiscard]] inline bool tree_selection_follows(
    const std::optional<bool> xtree_mode,
    const bool legacy_dynamic
) noexcept {
    return xtree_mode.value_or(legacy_dynamic);
}

[[nodiscard]] inline bool tree_selection_changes_location(
    const bool same_provider,
    const std::string_view current_path,
    const std::string_view selected_path
) noexcept {
    return !same_provider || current_path != selected_path;
}

}  // namespace mcpp::detail
