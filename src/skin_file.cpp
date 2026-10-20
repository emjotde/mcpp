#include "mcpp/skin.hpp"

#include "color_resource.hpp"

#include <ftxui/screen/terminal.hpp>

#include <algorithm>
#include <array>
#include <charconv>
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
        if (name == "base") return base;
        if (name == "default") return {};
        static const std::map<std::string_view, ftxui::Color::Palette16, std::less<>> names{
            {"black", ftxui::Color::Black}, {"red", ftxui::Color::Red},
            {"green", ftxui::Color::Green}, {"brown", ftxui::Color::Yellow},
            {"blue", ftxui::Color::Blue}, {"magenta", ftxui::Color::Magenta},
            {"cyan", ftxui::Color::Cyan}, {"lightgray", ftxui::Color::GrayLight},
            {"gray", ftxui::Color::GrayDark}, {"brightred", ftxui::Color::RedLight},
            {"brightgreen", ftxui::Color::GreenLight}, {"yellow", ftxui::Color::YellowLight},
            {"brightblue", ftxui::Color::BlueLight}, {"brightmagenta", ftxui::Color::MagentaLight},
            {"brightcyan", ftxui::Color::CyanLight}, {"white", ftxui::Color::White},
        };
        if (const auto found = names.find(name); found != names.end()) return found->second;
        const auto decimal = [](const std::string_view value, const int maximum) -> std::optional<int> {
            int result{};
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
                result < 0 || result > maximum) return {};
            return result;
        };
        if (name.starts_with("color")) {
            if (const auto index = decimal(std::string_view(name).substr(5), 255)) {
                return ftxui::Color(static_cast<ftxui::Color::Palette256>(*index));
            }
        } else if (name.starts_with("gray")) {
            if (const auto index = decimal(std::string_view(name).substr(4), 23)) {
                return ftxui::Color(static_cast<ftxui::Color::Palette256>(232 + *index));
            }
        } else if (name.size() == 6U && name.starts_with("rgb") &&
                   std::ranges::all_of(std::string_view(name).substr(3), [](const char digit) {
                       return digit >= '0' && digit <= '5';
                   })) {
            const auto index = 16 + 36 * (name[3] - '0') + 6 * (name[4] - '0') + (name[5] - '0');
            return ftxui::Color(static_cast<ftxui::Color::Palette256>(index));
        } else if (name.front() == '#' && (name.size() == 4U || name.size() == 7U)) {
            std::array<std::uint8_t, 3> components{};
            const auto digits = (name.size() - 1U) / 3U;
            bool valid = true;
            for (std::size_t index = 0; index < components.size(); ++index) {
                const auto fragment = std::string_view(name).substr(1U + index * digits, digits);
                unsigned component{};
                const auto parsed = std::from_chars(fragment.data(), fragment.data() + fragment.size(), component, 16);
                valid = valid && parsed.ec == std::errc{} && parsed.ptr == fragment.data() + fragment.size();
                components[index] = static_cast<std::uint8_t>(digits == 1U ? component * 17U : component);
            }
            if (valid) return ftxui::Color::RGB(components[0], components[1], components[2]);
        }
        color_resource_error(path_, line, "unknown color or alias: " + name);
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
            } else if (name == "bold") result |= SkinPair::bold;
            else if (name == "italic") result |= SkinPair::italic;
            else if (name == "underline") result |= SkinPair::underline;
            else if (name == "reverse") result |= SkinPair::reverse;
            else if (name == "blink") result |= SkinPair::blink;
            else color_resource_error(path_, line, "unknown attribute or alias: " + name);
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
    for (const auto& group : document) {
        if (group.name == "skin" || group.name == "aliases") continue;
        if (group.name == "Lines") {
            if (!group.settings.empty()) skin.warning = "custom [Lines] glyphs are not supported";
            continue;
        }
        auto fallback = base;
        if (const auto* setting = group.find("_default_")) fallback = resolver.pair(*setting, base, base);
        auto& colors = skin.colors[group.name];
        colors["_default_"] = fallback;
        for (const auto& setting : group.settings) {
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
    skin.menu_selected = skin.popup_selected = role("menu", "menusel", skin.menu);
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
    return skin;
}

}  // namespace

const SkinPair& skin_color(
    const Skin& skin, const std::string_view section, const std::string_view key, const SkinPair& fallback
) {
    const auto group = skin.colors.find(section);
    if (group == skin.colors.end()) return fallback;
    if (const auto found = group->second.find(key); found != group->second.end()) return found->second;
    if (const auto found = group->second.find("_default_"); found != group->second.end()) return found->second;
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
