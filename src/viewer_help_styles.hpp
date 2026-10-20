#pragma once

#include "mcpp/skin.hpp"
#include "mcpp/viewer.hpp"

#include <ftxui/dom/node.hpp>

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mcpp::detail {

enum class HelpTextStyle { bold, italic };

[[nodiscard]] ftxui::Element paint_skin_style(ftxui::Element element, const SkinPair& colors);
[[nodiscard]] ftxui::Element render_viewer_text(
    const Skin& skin, const std::string& row,
    std::optional<std::pair<std::size_t, std::size_t>> highlight,
    const std::vector<ViewerTextStyle>& styles = {}
);
[[nodiscard]] ftxui::Element help_text_style(
    ftxui::Element element, const Skin& skin, HelpTextStyle style
);

}  // namespace mcpp::detail
