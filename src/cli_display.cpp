#include "mcpp/cli_display.hpp"

#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/terminal.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <map>
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
};

constexpr std::array roles{
    ColorRole{"normal", "core", "_default_", {}, &Skin::normal},
    ColorRole{"selected", "core", "selected", {}, &Skin::selected},
    ColorRole{"markselect", "core", "markselect", {}, &Skin::marked_selected},
    ColorRole{"reverse", "core", "reverse", {}, &Skin::search},
    ColorRole{"gauge", "core", "gauge", {}, &Skin::progress},
    ColorRole{"errors", "error", "_default_", {}, &Skin::error},
    ColorRole{"dnormal", "dialog", "_default_", {}, &Skin::dialog},
    ColorRole{"dfocus", "dialog", "dfocus", {}, &Skin::dialog_focus},
    ColorRole{"errdhotfocus", "error", "errdfocus", {}, &Skin::error_focus},
    ColorRole{"menunormal", "menu", "_default_", {}, &Skin::menu},
    ColorRole{"menusel", "menu", "menusel", {}, &Skin::menu_selected},
    ColorRole{"pmenusel", "popupmenu", "pmenusel", {}, &Skin::popup_selected},
    ColorRole{"bbarhotkey", "buttonbar", "hotkey", {}, &Skin::key_number},
    ColorRole{"bbarbutton", "buttonbar", "button", {}, &Skin::key_label},
    ColorRole{"marked", "core", "marked", {}, nullptr, &Skin::bright},
    ColorRole{"disabled", "core", "disabled", {}, nullptr, &Skin::dim},
    ColorRole{"header", "core", "header", {}, nullptr, &Skin::column_header},
    ColorRole{"menuhot", "menu", "menuhot", {}, nullptr, &Skin::menu_hot},
    ColorRole{"menuinactive", "menu", "menuinactive", {}, nullptr, &Skin::disabled},
    ColorRole{"editnormal", "editor", "_default_", CliPaletteContext::editor, &Skin::normal},
    ColorRole{"editbold", "editor", "editbold", CliPaletteContext::editor, &Skin::search},
    ColorRole{"editmarked", "editor", "editmarked", CliPaletteContext::editor, &Skin::selected},
    ColorRole{"editwhitespace", "editor", "editwhitespace", CliPaletteContext::editor, nullptr, &Skin::dim},
    ColorRole{"viewnormal", "viewer", "_default_", CliPaletteContext::viewer, &Skin::normal},
    ColorRole{"viewselected", "viewer", "viewselected", CliPaletteContext::viewer, &Skin::viewer_cursor},
    ColorRole{"helpnormal", "help", "_default_", CliPaletteContext::help, &Skin::normal},
    ColorRole{"helplink", "help", "helplink", CliPaletteContext::help, &Skin::normal},
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

std::string_view trim(const std::string_view value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string_view::npos) return {};
    return value.substr(begin, value.find_last_not_of(" \t\r\n") - begin + 1U);
}

ftxui::Color parse_color(const std::string_view name) {
    static constexpr std::array<std::string_view, 16> names{
        "black", "red", "green", "brown", "blue", "magenta", "cyan", "lightgray",
        "gray", "brightred", "brightgreen", "yellow", "brightblue", "brightmagenta",
        "brightcyan", "white"
    };
    if (name == "default") return {};
    if (const auto found = std::ranges::find(names, name); found != names.end()) {
        return ftxui::Color(static_cast<ftxui::Color::Palette16>(found - names.begin()));
    }
    const auto decimal = [](const std::string_view text, const int maximum) {
        int result = -1;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
        if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
            result < 0 || result > maximum || text.front() == '-') {
            throw std::invalid_argument("invalid color index: " + std::string(text));
        }
        return result;
    };
    if (name.starts_with("color")) {
        return ftxui::Color(static_cast<ftxui::Color::Palette256>(decimal(name.substr(5), 255)));
    }
    if (name.starts_with("gray")) {
        return ftxui::Color(static_cast<ftxui::Color::Palette256>(232 + decimal(name.substr(4), 23)));
    }
    if (name.size() == 6U && name.starts_with("rgb") &&
        std::ranges::all_of(name.substr(3), [](const char digit) { return digit >= '0' && digit <= '5'; })) {
        return ftxui::Color(static_cast<ftxui::Color::Palette256>(
            16 + 36 * (name[3] - '0') + 6 * (name[4] - '0') + name[5] - '0'));
    }
    throw std::invalid_argument("unknown color: " + std::string(name));
}

unsigned parse_attributes(const std::string_view value) {
    if (value == "none") return 0;
    unsigned result = 0;
    std::size_t begin = 0;
    for (;;) {
        const auto end = value.find('+', begin);
        const auto attribute = trim(value.substr(begin,
            end == std::string_view::npos ? value.size() - begin : end - begin));
        if (attribute == "bold") result |= SkinPair::bold;
        else if (attribute == "italic") result |= SkinPair::italic;
        else if (attribute == "underline") result |= SkinPair::underline;
        else if (attribute == "reverse") result |= SkinPair::reverse;
        else if (attribute == "blink") result |= SkinPair::blink;
        else throw std::invalid_argument("unknown color attribute: " + std::string(attribute));
        if (end == std::string_view::npos) break;
        begin = end + 1U;
    }
    return result;
}

void apply_pair(SkinPair& pair, const CliColorOverride& override) {
    if (override.foreground) pair.foreground = *override.foreground;
    if (override.background) pair.background = *override.background;
    if (override.attributes) pair.attributes = *override.attributes;
    pair.explicit_style = true;
}

unsigned line_connections(const std::string_view character) {
    constexpr unsigned left = 1U, right = 2U, up = 4U, down = 8U;
    if (character == "─" || character == "━" || character == "═" || character == "╍") return left | right;
    if (character == "│" || character == "┃" || character == "║" || character == "╏") return up | down;
    if (character == "┌" || character == "┏" || character == "╔" || character == "╭") return right | down;
    if (character == "┐" || character == "┓" || character == "╗" || character == "╮") return left | down;
    if (character == "└" || character == "┗" || character == "╚" || character == "╰") return right | up;
    if (character == "┘" || character == "┛" || character == "╝" || character == "╯") return left | up;
    if (character == "├" || character == "┣" || character == "╠") return right | up | down;
    if (character == "┤" || character == "┫" || character == "╣") return left | up | down;
    if (character == "┬" || character == "┳" || character == "╦") return left | right | down;
    if (character == "┴" || character == "┻" || character == "╩") return left | right | up;
    if (character == "┼" || character == "╋" || character == "╬") return left | right | up | down;
    return 0;
}

class CliDisplay final : public ftxui::Node {
public:
    CliDisplay(
        ftxui::Element element, const bool monochrome, const bool stickchars,
        const std::span<const Skin* const> palettes
    ) : Node({std::move(element)}), monochrome_(monochrome), stickchars_(stickchars) {
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
        std::vector<unsigned> lines;
        if (stickchars_) {
            lines.resize(static_cast<std::size_t>(screen.dimx()) * static_cast<std::size_t>(screen.dimy()));
            for (int y = 0; y < screen.dimy(); ++y) {
                for (int x = 0; x < screen.dimx(); ++x) {
                    const auto& cell = screen.CellAt(x, y);
                    if (cell.automerge) {
                        lines[static_cast<std::size_t>(y) * static_cast<std::size_t>(screen.dimx()) +
                              static_cast<std::size_t>(x)] = line_connections(cell.character);
                    }
                }
            }
        }
        const auto connects = [&](const int x, const int y, const unsigned direction) {
            return x >= 0 && x < screen.dimx() && y >= 0 && y < screen.dimy() &&
                (lines[static_cast<std::size_t>(y) * static_cast<std::size_t>(screen.dimx()) +
                       static_cast<std::size_t>(x)] & direction) != 0U;
        };
        for (int y = 0; y < screen.dimy(); ++y) {
            for (int x = 0; x < screen.dimx(); ++x) {
                auto& cell = screen.CellAt(x, y);
                if (stickchars_) {
                    const auto connections = line_connections(cell.character);
                    const bool horizontal = connections == 3U;
                    const bool vertical = connections == 12U;
                    // FTXUI marks border edges, but not its corner cells.
                    // A title can overwrite one of the corner's adjacent edges.
                    const bool corner = !horizontal && !vertical && connections != 0U &&
                        (((connections & 1U) != 0U && connects(x - 1, y, 2U)) ||
                         ((connections & 2U) != 0U && connects(x + 1, y, 1U)) ||
                         ((connections & 4U) != 0U && connects(x, y - 1, 8U)) ||
                         ((connections & 8U) != 0U && connects(x, y + 1, 4U)));
                    if (connections != 0U && (cell.automerge || corner)) {
                        cell.character = horizontal ? "-" : vertical ? "|" : "+";
                        cell.automerge = false;
                    }
                }
                if (monochrome_) {
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
    }

private:
    bool monochrome_;
    bool stickchars_;
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
            const auto entry = trim(std::string_view(value).substr(begin,
                end == std::string::npos ? value.size() - begin : end - begin));
            const auto equals = entry.find('=');
            if (equals == std::string_view::npos) {
                throw std::invalid_argument("color overrides require role=foreground,background,attributes");
            }
            const auto& role = color_role(trim(entry.substr(0, equals)));
            std::array<std::string_view, 3> fields{};
            std::size_t field = 0;
            std::size_t offset = equals + 1U;
            for (;;) {
                if (field == fields.size()) throw std::invalid_argument("too many fields for color role " + std::string(role.name));
                const auto comma = entry.find(',', offset);
                fields[field++] = trim(entry.substr(offset,
                    comma == std::string_view::npos ? entry.size() - offset : comma - offset));
                if (comma == std::string_view::npos) break;
                offset = comma + 1U;
            }
            if (role.foreground && (!fields[1].empty() || !fields[2].empty())) {
                throw std::invalid_argument(std::string(role.name) +
                    " currently supports only a foreground color; omit its background and attributes");
            }
            CliColorOverride override;
            override.role = role.name;
            if (!fields[0].empty()) override.foreground = parse_color(fields[0]);
            if (!fields[1].empty()) override.background = parse_color(fields[1]);
            if (!fields[2].empty()) override.attributes = parse_attributes(fields[2]);
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
            auto pair = role.pair ? skin.*role.pair : skin.normal;
            if (role.foreground) pair.foreground = skin.*role.foreground;
            const auto group = skin.colors.find(role.section);
            if (group != skin.colors.end()) {
                if (const auto found = group->second.find(role.key); found != group->second.end()) pair = found->second;
            }
            apply_pair(pair, override);
            skin.colors[std::string(role.section)][std::string(role.key)] = pair;
            if (role.name != "helplink") {
                if (role.pair) skin.*role.pair = pair;
                else skin.*role.foreground = pair.foreground;
            }
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
    if (!monochrome && !stickchars) return element;
    return std::make_shared<CliDisplay>(std::move(element), monochrome, stickchars, palettes);
}

}  // namespace mcpp
