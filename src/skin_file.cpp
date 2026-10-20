#include "mcpp/skin.hpp"

#include "color_resource.hpp"

#include <ftxui/screen/color_info.hpp>
#include <ftxui/screen/terminal.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <optional>
#include <set>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace mcpp {
namespace {

using detail::ColorSection;
using detail::ColorSetting;
using detail::color_resource_error;
using detail::color_trim;

std::vector<std::filesystem::path> skin_directories(const std::filesystem::path& config) {
    std::vector<std::filesystem::path> result{config / "skins"};
    std::filesystem::path data;
    if (const auto* value = std::getenv("XDG_DATA_HOME"); value && *value) {
        data = value;
    } else if (const auto* home = std::getenv("HOME"); home && *home) {
        data = std::filesystem::path(home) / ".local/share";
    }
    if (!data.empty()) {
        result.push_back(data / "mcpp/skins");
        result.push_back(data / "mc/skins");
    }
    result.emplace_back("/etc/mcpp/skins");
    result.emplace_back("/usr/share/mcpp/skins");
    result.emplace_back("/etc/mc/skins");
    result.emplace_back("/usr/share/mc/skins");
    return result;
}

std::optional<std::filesystem::path> locate_skin(
    const std::string_view id, const std::filesystem::path& config
) {
    auto requested = std::filesystem::path(id);
    if (id.starts_with("~/")) {
        if (const auto* home = std::getenv("HOME"); home && *home) {
            requested = std::filesystem::path(home) / id.substr(2);
        }
    }
    const auto try_file = [](const std::filesystem::path& path) -> std::optional<std::filesystem::path> {
        if (detail::color_resource_exists(path)) return std::filesystem::absolute(path).lexically_normal();
        if (path.extension() != ".ini") {
            auto with_extension = path;
            with_extension += ".ini";
            if (detail::color_resource_exists(with_extension)) {
                return std::filesystem::absolute(with_extension).lexically_normal();
            }
        }
        return {};
    };
    if (requested.is_absolute() || requested.has_parent_path()) return try_file(requested);
    for (const auto& directory : skin_directories(config)) {
        if (auto found = try_file(directory / requested)) return found;
    }
    return {};
}

class ColorResolver {
public:
    ColorResolver(const std::filesystem::path& path, const ColorSection* aliases)
        : path_(path), aliases_(aliases) {}

    void validate_aliases() const {
        if (!aliases_) return;
        std::set<std::string, std::less<>> active;
        std::set<std::string, std::less<>> complete;
        const std::function<void(const ColorSetting&)> visit = [&](const ColorSetting& alias) {
            if (complete.contains(alias.key)) return;
            if (!active.insert(alias.key).second) {
                color_resource_error(path_, alias.line, "color/attribute alias cycle involving " + alias.key);
            }
            if (active.size() > 128U) color_resource_error(path_, alias.line, "color/attribute aliases exceed 128 levels");
            const std::string_view value(alias.value);
            if (value.find(';') != std::string_view::npos) {
                color_resource_error(path_, alias.line, "an alias must name one color or attribute field");
            }
            std::size_t begin = 0;
            for (;;) {
                const auto end = value.find('+', begin);
                const auto name = color_trim(value.substr(begin,
                    end == std::string_view::npos ? value.size() - begin : end - begin));
                if (name.empty()) color_resource_error(path_, alias.line, "empty color/attribute alias");
                if (const auto* referenced = aliases_->find(name)) visit(*referenced);
                if (end == std::string_view::npos) break;
                begin = end + 1U;
            }
            active.erase(alias.key);
            complete.insert(alias.key);
        };
        for (const auto& alias : aliases_->settings) visit(alias);
    }

    std::string expand(const std::string_view original, const std::size_t line) const {
        std::string value(color_trim(original));
        std::set<std::string, std::less<>> visited;
        while (aliases_) {
            const auto* alias = aliases_->find(value);
            if (!alias) break;
            if (!visited.insert(value).second) {
                color_resource_error(path_, alias->line, "color/attribute alias cycle involving " + value);
            }
            value = std::string(color_trim(alias->value));
            if (value.find(';') != std::string::npos) {
                color_resource_error(path_, alias->line, "an alias must name one color or attribute field");
            }
        }
        if (value.empty()) color_resource_error(path_, line, "empty color/attribute alias");
        return value;
    }

    ftxui::Color color(
        const std::string_view original, const std::size_t line, const ftxui::Color& base
    ) const {
        const auto name = expand(original, line);
        try {
            return detail::parse_color_value(name, base);
        } catch (const std::invalid_argument& error) {
            color_resource_error(path_, line, error.what());
        }
    }

    unsigned attributes(const std::string_view original, const std::size_t line, const unsigned depth = 0) const {
        if (depth > 128U) color_resource_error(path_, line, "color/attribute aliases exceed 128 levels");
        const auto value = expand(original, line);
        if (value == "none") return 0;
        unsigned result = 0;
        std::size_t begin = 0;
        while (begin <= value.size()) {
            const auto end = value.find('+', begin);
            const auto token = std::string_view(value).substr(begin,
                end == std::string::npos ? value.size() - begin : end - begin);
            const auto name = expand(token, line);
            if (name.find('+') != std::string::npos) {
                // Attribute-list aliases can be used as individual members of a list.
                result |= attributes(name, line, depth + 1U);
            } else {
                try {
                    if (name == "none") throw std::invalid_argument("unknown attribute or alias: " + name);
                    result |= detail::parse_color_attributes(name);
                } catch (const std::invalid_argument& error) {
                    color_resource_error(path_, line, error.what());
                }
            }
            if (end == std::string::npos) break;
            begin = end + 1U;
        }
        return result;
    }

    SkinPair pair(const ColorSetting& setting, const SkinPair& fallback, const SkinPair& base) const {
        std::array<std::string_view, 3> fields{};
        std::size_t count = 0;
        std::size_t begin = 0;
        const std::string_view value(setting.value);
        for (;;) {
            if (count == fields.size()) color_resource_error(path_, setting.line, "too many color-pair fields");
            const auto end = value.find(';', begin);
            fields[count++] = color_trim(value.substr(begin,
                end == std::string_view::npos ? value.size() - begin : end - begin));
            if (end == std::string_view::npos) break;
            begin = end + 1U;
        }
        auto result = fallback;
        result.explicit_style = true;
        if (!fields[0].empty()) result.foreground = color(fields[0], setting.line, base.foreground);
        if (!fields[1].empty()) result.background = color(fields[1], setting.line, base.background);
        if (!fields[2].empty()) result.attributes = attributes(fields[2], setting.line);
        return result;
    }

private:
    const std::filesystem::path& path_;
    const ColorSection* aliases_;
};

std::optional<double> color_luminance(const ftxui::Color& color) {
    // FTXUI exposes colors through its ANSI encoder, not RGB accessors.
    const auto encoded = color.Print(false);
    unsigned red{}, green{}, blue{}, index{};
    if (std::sscanf(encoded.c_str(), "38;2;%u;%u;%u", &red, &green, &blue) != 3) {
        ftxui::ColorInfo info{};
        if (std::sscanf(encoded.c_str(), "38;5;%u", &index) == 1) {
            info = ftxui::GetColorInfo(static_cast<ftxui::Color::Palette256>(index));
        } else {
            unsigned code{};
            const auto parsed = std::from_chars(encoded.data(), encoded.data() + encoded.size(), code);
            if (parsed.ec != std::errc{} || parsed.ptr != encoded.data() + encoded.size()) return {};
            if (code >= 30U && code <= 37U) index = code - 30U;
            else if (code >= 90U && code <= 97U) index = code - 90U + 8U;
            else return {};
            info = ftxui::GetColorInfo(static_cast<ftxui::Color::Palette16>(index));
        }
        red = info.red; green = info.green; blue = info.blue;
    }
    const auto linear = [](const unsigned value) {
        const auto component = static_cast<double>(value) / 255.0;
        return component <= 0.04045 ? component / 12.92 : std::pow((component + 0.055) / 1.055, 2.4);
    };
    return linear(red) * 0.2126 + linear(green) * 0.7152 + linear(blue) * 0.0722;
}

void set_native_syntax_fallback(Skin& skin) {
    auto editor = skin_color(skin, "editor", "_default_", skin.normal);
    if ((editor.attributes & SkinPair::reverse) != 0U) std::swap(editor.foreground, editor.background);
    const auto background = color_luminance(editor.background);
    const auto foreground = color_luminance(editor.foreground);
    skin.light = background ? *background > 0.3 : foreground && *foreground < 0.3;
    const auto& preferred = find_skin(skin.light ? "paper" : "grayscale").syntax;
    const auto& alternative = find_skin(skin.light ? "grayscale" : "paper").syntax;
    const auto contrast = [&](const ftxui::Color& color) {
        const auto light = color_luminance(color);
        if (!light || !background) return 0.0;
        return (std::max(*light, *background) + 0.05) / (std::min(*light, *background) + 0.05);
    };
    for (std::size_t index = 1; index < skin.syntax.size(); ++index) {
        skin.syntax[index] = preferred[index];
        if (!background || contrast(skin.syntax[index]) >= 4.5) continue;
        if (contrast(alternative[index]) > contrast(skin.syntax[index])) skin.syntax[index] = alternative[index];
        if (contrast(skin.syntax[index]) >= 4.5) continue;
        if (contrast(editor.foreground) >= 4.5) skin.syntax[index] = editor.foreground;
        else {
            const auto black = ftxui::Color::RGB(0, 0, 0);
            const auto white = ftxui::Color::RGB(255, 255, 255);
            skin.syntax[index] = contrast(black) > contrast(white) ? black : white;
        }
    }
}

Skin parse_skin(const std::filesystem::path& path) {
    const auto document = detail::read_color_resource(path);
    const auto section = [&](const std::string_view name) -> const ColorSection* {
        const auto found = std::ranges::find(document, name, &ColorSection::name);
        return found == document.end() ? nullptr : &*found;
    };
    const ColorResolver resolver(path, section("aliases"));
    resolver.validate_aliases();
    Skin skin = find_skin("grayscale");
    skin.id = path.string();
    skin.label = path.stem().string();
    skin.resource = path;
    if (const auto* meta = section("skin")) {
        if (const auto* description = meta->find("description"); description && !description->value.empty()) {
            skin.label = description->value;
        }
        const auto support = ftxui::Terminal::ColorSupport();
        if (const auto* required = meta->find("truecolors");
            required && detail::color_boolean(*required, path) && support < ftxui::Terminal::TrueColor) {
            color_resource_error(path, required->line, "skin requires a true-color terminal");
        }
        if (const auto* required = meta->find("256colors");
            required && detail::color_boolean(*required, path) && support < ftxui::Terminal::Palette256) {
            color_resource_error(path, required->line, "skin requires a 256-color terminal");
        }
    }
    if (const auto* aliases = section("aliases")) {
        for (const auto& alias : aliases->settings) static_cast<void>(resolver.expand(alias.key, alias.line));
    }
    SkinPair base = skin.normal;
    base.explicit_style = true;
    if (const auto* core = section("core")) {
        if (const auto* setting = core->find("_default_")) base = resolver.pair(*setting, base, base);
    }
    std::size_t count = 0;
    const auto glyph_setting = [&](const ColorSection& group, const ColorSetting& setting) {
        SkinGlyphSetting result{};
        try {
            result = set_skin_glyph(skin.glyphs, group.name, setting.key, setting.value);
        } catch (const std::invalid_argument& error) {
            color_resource_error(path, setting.line, error.what());
        }
        if (result == SkinGlyphSetting::unsupported) {
            if (skin.warning.empty()) skin.warning = "unsupported skin glyph roles: ";
            else skin.warning += ", ";
            skin.warning += "[" + group.name + "] " + setting.key;
        }
        return result != SkinGlyphSetting::color;
    };
    for (const auto& group : document) {
        if (group.name == "skin" || group.name == "aliases") continue;
        if (group.name == "Lines" || group.name.starts_with("widget-")) {
            for (const auto& setting : group.settings) static_cast<void>(glyph_setting(group, setting));
            continue;
        }
        auto fallback = base;
        if (const auto* setting = group.find("_default_")) fallback = resolver.pair(*setting, base, base);
        auto& colors = skin.colors[group.name];
        colors["_default_"] = fallback;
        for (const auto& setting : group.settings) {
            if (glyph_setting(group, setting)) continue;
            colors[setting.key] = resolver.pair(setting, fallback, base);
            ++count;
        }
    }
    if (count == 0U) color_resource_error(path, 0, "skin has no color definitions");
    skin.normal = base;
    const auto role = [&](const std::string_view group, const std::string_view key,
                          const SkinPair& fallback) -> SkinPair {
        return skin_color(skin, group, key, fallback);
    };
    skin.menu = role("menu", "_default_", base);
    skin.menu_selected = role("menu", "menusel", skin.menu);
    skin.popup_selected = role("popupmenu", "menusel", skin.menu_selected);
    skin.dialog = role("dialog", "_default_", base);
    skin.dialog_focus = role("dialog", "dfocus", skin.dialog);
    skin.error = role("error", "_default_", base);
    skin.error_focus = role("error", "errdfocus", skin.error);
    skin.selected = role("core", "selected", base);
    skin.marked_selected = role("core", "markselect", skin.selected);
    skin.inactive_selected = role("core", "inactive", skin.selected);
    skin.inactive_marked_selected = role("core", "inactivemarkselect", skin.marked_selected);
    skin.search = role("core", "reverse", skin.selected);
    skin.viewer_cursor = role("viewer", "viewselected", skin.selected);
    skin.hint = role("core", "hint", base);
    skin.key_number = role("buttonbar", "hotkey", skin.menu);
    skin.key_label = role("buttonbar", "button", skin.menu);
    skin.progress = role("core", "gauge", skin.selected);
    skin.diff_changed = role("diffviewer", "changedline", skin.selected);
    skin.menu_hot = role("menu", "menuhot", skin.menu).foreground;
    skin.bright = role("core", "marked", base).foreground;
    skin.dim = role("core", "disabled", base).foreground;
    skin.inactive_title = role("core", "header", base).foreground;
    skin.column_header = role("core", "header", base).foreground;
    skin.directory = role("filehighlight", "directory", base).foreground;
    skin.symlink = role("filehighlight", "symlink", base).foreground;
    skin.executable = role("filehighlight", "executable", base).foreground;
    skin.special = role("filehighlight", "special", base).foreground;
    skin.inactive_tree = base.foreground;
    skin.disabled = role("menu", "menuinactive", skin.menu).foreground;
    skin.shadow = role("core", "shadow", {ftxui::Color::Black, ftxui::Color::Black}).background;
    skin.syntax[0] = base.foreground;
    set_native_syntax_fallback(skin);
    return skin;
}

}  // namespace

const SkinPair* find_skin_color(
    const Skin& skin, const std::string_view section, const std::string_view key
) {
    const auto group = skin.colors.find(section);
    if (group == skin.colors.end()) return nullptr;
    const auto found = group->second.find(key);
    return found == group->second.end() ? nullptr : &found->second;
}

const SkinPair& skin_color(
    const Skin& skin, const std::string_view section, const std::string_view key, const SkinPair& fallback
) {
    if (const auto* found = find_skin_color(skin, section, key)) return *found;
    if (const auto* found = find_skin_color(skin, section, "_default_")) return *found;
    return fallback;
}

Skin load_skin(const std::string_view id, const std::filesystem::path& config_directory) {
    try {
        return find_skin(id);
    } catch (const std::invalid_argument&) {
        if (id.empty()) throw std::invalid_argument("unknown skin: empty name");
    }
    if (const auto path = locate_skin(id, config_directory)) return parse_skin(*path);
    throw std::invalid_argument("unknown skin: " + std::string(id) +
        " (file not found; use a skin path or a name in " + (config_directory / "skins").string() + ")");
}

std::vector<SkinChoice> available_skins(const std::filesystem::path& config_directory) {
    std::vector<SkinChoice> choices;
    for (const auto& skin : builtin_skins()) choices.push_back({skin.id, skin.label});
    std::set<std::filesystem::path> seen;
    for (const auto& directory : skin_directories(config_directory)) {
        if (!detail::color_resource_exists(directory)) continue;
        std::error_code error;
        std::filesystem::directory_iterator iterator(directory, error);
        if (error) throw std::system_error(error, "list skins in " + directory.string());
        std::vector<std::filesystem::path> paths;
        for (; iterator != std::filesystem::directory_iterator{}; iterator.increment(error)) {
            if (error) throw std::system_error(error, "list skins in " + directory.string());
            if (iterator->path().extension() == ".ini") paths.push_back(iterator->path());
        }
        if (error) throw std::system_error(error, "list skins in " + directory.string());
        std::ranges::sort(paths);
        for (const auto& path : paths) {
            const auto absolute = std::filesystem::absolute(path).lexically_normal();
            if (!seen.insert(absolute).second) continue;
            choices.push_back({absolute.string(), path.stem().string()});
        }
    }
    return choices;
}

}  // namespace mcpp
