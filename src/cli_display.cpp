#include "mcpp/cli_display.hpp"
#include "mcpp/skin_glyphs_ui.hpp"

#include "color_resource.hpp"

#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/terminal.hpp>

#include <algorithm>
#include <array>
#include <stdexcept>
#include <utility>

namespace mcpp {
namespace {

struct ColorRole {
    std::string_view name;
    std::string_view section;
    std::string_view key;
    std::optional<CliPaletteContext> context;
    SkinPair Skin::*pair{nullptr};
    ftxui::Color Skin::*foreground{nullptr};
    bool foreground_only{false};
    SkinPair Skin::*fallback{&Skin::normal};
    ftxui::Color Skin::*fallback_foreground{nullptr};
    unsigned fallback_attributes{0};
};

constexpr std::array roles{
    ColorRole{"normal", "core", "_default_", {}, &Skin::normal},
    ColorRole{"selected", "core", "selected", {}, &Skin::selected},
    ColorRole{"markselect", "core", "markselect", {}, &Skin::marked_selected},
    ColorRole{"reverse", "core", "reverse", {}, &Skin::search},
    ColorRole{"gauge", "core", "gauge", {}, &Skin::progress},
    ColorRole{"input", "core", "input", {}, nullptr, nullptr, false, &Skin::dialog_focus},
    ColorRole{"inputmark", "core", "inputmark", {}, nullptr, nullptr, false,
              &Skin::dialog_focus, nullptr, SkinPair::bold | SkinPair::underline},
    ColorRole{"inputunchanged", "core", "inputunchanged", {}, nullptr, nullptr, false, &Skin::dialog_focus},
    ColorRole{"errors", "error", "_default_", {}, &Skin::error},
    ColorRole{"dnormal", "dialog", "_default_", {}, &Skin::dialog},
    ColorRole{"dfocus", "dialog", "dfocus", {}, &Skin::dialog_focus},
    ColorRole{"dtitle", "dialog", "dtitle", {}, nullptr, nullptr, false, &Skin::dialog},
    ColorRole{"errdfocus", "error", "errdfocus", {}, &Skin::error_focus},
    // Keep the existing whole-focus alias; mnemonic colors are not separate widgets.
    ColorRole{"errdhotfocus", "error", "errdfocus", {}, &Skin::error_focus},
    ColorRole{"errdtitle", "error", "errdtitle", {}, nullptr, nullptr, false, &Skin::error},
    ColorRole{"menunormal", "menu", "_default_", {}, &Skin::menu},
    ColorRole{"menusel", "menu", "menusel", {}, &Skin::menu_selected},
    ColorRole{"pmenunormal", "popupmenu", "_default_", {}, nullptr, nullptr, false, &Skin::menu},
    ColorRole{"pmenusel", "popupmenu", "menusel", {}, &Skin::popup_selected},
    ColorRole{"pmenutitle", "popupmenu", "menutitle", {}, nullptr, nullptr, false, &Skin::menu},
    ColorRole{"bbarhotkey", "buttonbar", "hotkey", {}, &Skin::key_number},
    ColorRole{"bbarbutton", "buttonbar", "button", {}, &Skin::key_label},
    ColorRole{"marked", "core", "marked", {}, nullptr, &Skin::bright, true},
    ColorRole{"disabled", "core", "disabled", {}, nullptr, &Skin::dim, true},
    ColorRole{"header", "core", "header", {}, nullptr, &Skin::column_header, true},
    ColorRole{"menuhot", "menu", "menuhot", {}, nullptr, &Skin::menu_hot, true, &Skin::menu},
    ColorRole{"menuinactive", "menu", "menuinactive", {}, nullptr, &Skin::disabled, false, &Skin::menu},
    ColorRole{"editnormal", "editor", "_default_", CliPaletteContext::editor, &Skin::normal},
    ColorRole{"editbold", "editor", "editbold", CliPaletteContext::editor, &Skin::search},
    ColorRole{"editmarked", "editor", "editmarked", CliPaletteContext::editor, &Skin::selected},
    ColorRole{"editwhitespace", "editor", "editwhitespace", CliPaletteContext::editor, nullptr, &Skin::dim, true},
    ColorRole{"editlinestate", "editor", "editlinestate", CliPaletteContext::editor,
              nullptr, nullptr, false, &Skin::normal, &Skin::dim},
    ColorRole{"viewnormal", "viewer", "_default_", CliPaletteContext::viewer, &Skin::normal},
    ColorRole{"viewbold", "viewer", "viewbold", CliPaletteContext::viewer,
              nullptr, nullptr, false, &Skin::normal, nullptr, SkinPair::bold},
    ColorRole{"viewunderline", "viewer", "viewunderline", CliPaletteContext::viewer,
              nullptr, nullptr, false, &Skin::normal, nullptr, SkinPair::underline},
    ColorRole{"viewselected", "viewer", "viewselected", CliPaletteContext::viewer, &Skin::viewer_cursor},
    ColorRole{"helpnormal", "help", "_default_", CliPaletteContext::help, &Skin::normal},
    ColorRole{"helpbold", "help", "helpbold", CliPaletteContext::help,
              nullptr, nullptr, false, &Skin::normal, nullptr, SkinPair::bold},
    ColorRole{"helpitalic", "help", "helpitalic", CliPaletteContext::help,
              nullptr, nullptr, false, &Skin::normal, nullptr, SkinPair::italic},
    ColorRole{"helplink", "help", "helplink", CliPaletteContext::help},
    ColorRole{"helpslink", "help", "helpslink", CliPaletteContext::help, &Skin::selected},
};

const ColorRole& color_role(const std::string_view name) {
    const auto found = std::ranges::find(roles, name, &ColorRole::name);
    if (found == roles.end()) {
        throw std::invalid_argument("unsupported color role: " + std::string(name) +
                                    "; use --help-color for supported roles");
    }
    return *found;
}

void apply_pair(SkinPair& pair, const CliColorOverride& override) {
    if (override.foreground) pair.foreground = *override.foreground;
    if (override.background) pair.background = *override.background;
    if (override.attributes) pair.attributes = *override.attributes;
    pair.explicit_style = true;
}

class CliDisplay final : public ftxui::Node {
public:
    CliDisplay(
        ftxui::Element element, const bool monochrome, const bool stickchars,
        const std::span<const Skin* const> palettes
    ) : Node({std::move(element)}), monochrome_(monochrome), stickchars_(stickchars) {
        if (!palettes.empty() && palettes.front()) glyphs_ = palettes.front()->glyphs;
        for (const auto* skin : palettes) {
            if (skin == nullptr) continue;
            for (const auto* pair : {
                    &skin->selected, &skin->marked_selected, &skin->inactive_selected,
                    &skin->inactive_marked_selected, &skin->search, &skin->viewer_cursor,
                    &skin->dialog_focus, &skin->menu_selected, &skin->popup_selected,
                    &skin->key_number, &skin->error_focus, &skin->progress}) {
                emphasis_.push_back(*pair);
            }
            for (const auto* pair : {
                    &skin->normal, &skin->dialog, &skin->menu, &skin->error,
                    &skin->hint, &skin->key_label}) {
                normal_.push_back(*pair);
            }
        }
    }

    void SetBox(const ftxui::Box box) override {
        Node::SetBox(box);
        children_.front()->SetBox(box);
    }

    void Render(ftxui::Screen& screen) override {
        Node::Render(screen);
        apply_skin_lines(screen, glyphs_, stickchars_);
        if (!monochrome_) return;
        for (int y = 0; y < screen.dimy(); ++y) {
            for (int x = 0; x < screen.dimx(); ++x) {
                auto& cell = screen.CellAt(x, y);
                const auto foreground = cell.inverted ? cell.background_color : cell.foreground_color;
                const auto background = cell.inverted ? cell.foreground_color : cell.background_color;
                const auto matches = [&](const SkinPair& pair) {
                    return foreground == pair.foreground && background == pair.background;
                };
                if (std::ranges::any_of(emphasis_, matches)) cell.inverted = true;
                else if (std::ranges::any_of(normal_, matches)) cell.inverted = false;
                cell.foreground_color = {};
                cell.background_color = {};
            }
        }
    }

private:
    bool monochrome_;
    bool stickchars_;
    SkinGlyphs glyphs_;
    std::vector<SkinPair> emphasis_;
    std::vector<SkinPair> normal_;
};

}  // namespace

std::vector<CliColorOverride> parse_cli_colors(const std::span<const std::string> values) {
    std::vector<CliColorOverride> result;
    for (const auto& value : values) {
        std::size_t begin = 0;
        for (;;) {
            const auto end = value.find(':', begin);
            const auto entry = detail::color_trim(std::string_view(value).substr(begin,
                end == std::string::npos ? value.size() - begin : end - begin));
            const auto equals = entry.find('=');
            if (equals == std::string_view::npos) {
                throw std::invalid_argument("color overrides require role=foreground,background,attributes");
            }
            const auto& role = color_role(detail::color_trim(entry.substr(0, equals)));
            std::array<std::string_view, 3> fields{};
            std::size_t field = 0;
            std::size_t offset = equals + 1U;
            for (;;) {
                if (field == fields.size()) throw std::invalid_argument("too many fields for color role " + std::string(role.name));
                const auto comma = entry.find(',', offset);
                fields[field++] = detail::color_trim(entry.substr(offset,
                    comma == std::string_view::npos ? entry.size() - offset : comma - offset));
                if (comma == std::string_view::npos) break;
                offset = comma + 1U;
            }
            if (role.foreground_only && (!fields[1].empty() || !fields[2].empty())) {
                throw std::invalid_argument(std::string(role.name) +
                    " currently supports only a foreground color; omit its background and attributes");
            }
            CliColorOverride override;
            override.role = role.name;
            if (!fields[0].empty()) override.foreground = detail::parse_color_value(fields[0]);
            if (!fields[1].empty()) override.background = detail::parse_color_value(fields[1]);
            if (!fields[2].empty()) override.attributes = detail::parse_color_attributes(fields[2]);
            if (!override.foreground && !override.background && !override.attributes) {
                throw std::invalid_argument("color override for " + override.role + " has no fields");
            }
            result.push_back(std::move(override));
            if (end == std::string::npos) break;
            begin = end + 1U;
        }
    }
    return result;
}

void apply_cli_colors(
    Skin& skin, const std::span<const CliColorOverride> overrides, const CliPaletteContext context
) {
    for (const bool specific : {false, true}) {
        for (const auto& override : overrides) {
            const auto& role = color_role(override.role);
            if (role.context.has_value() != specific || (role.context && *role.context != context)) continue;
            auto pair = skin.*(role.pair ? role.pair : role.fallback);
            if (role.foreground) pair.foreground = skin.*role.foreground;
            if (role.fallback_foreground) pair.foreground = skin.*role.fallback_foreground;
            if (const auto* found = find_skin_color(skin, role.section, role.key)) {
                pair = *found;
            } else if (!role.pair && !role.foreground) {
                pair = skin_color(skin, role.section, role.key, pair);
                pair.attributes |= role.fallback_attributes;
            }
            apply_pair(pair, override);
            skin.colors[std::string(role.section)][std::string(role.key)] = pair;
            if (role.pair) skin.*role.pair = pair;
            if (role.foreground) skin.*role.foreground = pair.foreground;
            if (role.name == "normal" || role.name == "editnormal") skin.syntax[0] = skin.normal.foreground;
            if (role.name == "header") skin.inactive_title = pair.foreground;
        }
    }
}

void prepare_cli_terminal(const std::optional<bool> color, const bool stickchars) {
    auto quirks = ftxui::Terminal::GetQuirks();
    if (color && quirks.ColorSupport() == ftxui::Terminal::Palette1) {
        quirks.SetColorSupport(ftxui::Terminal::Palette16);
    }
    if (stickchars) {
        quirks.SetBlockCharacters(false);
        quirks.SetComponentAscii(true);
    }
    ftxui::Terminal::SetQuirks(quirks);
}

ftxui::Element cli_display(
    ftxui::Element element, const bool monochrome, const bool stickchars,
    const std::span<const Skin* const> palettes
) {
    const bool custom_lines = !palettes.empty() && palettes.front() && palettes.front()->glyphs.has_lines();
    if (!monochrome && !stickchars && !custom_lines) return element;
    return std::make_shared<CliDisplay>(std::move(element), monochrome, stickchars, palettes);
}

}  // namespace mcpp
