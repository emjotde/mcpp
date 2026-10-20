#include "mcpp/file_highlight.hpp"

#include "mcpp/editor.hpp"
#include "mcpp/native_regex.hpp"

#include "color_resource.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cctype>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <utility>
#include <vector>

namespace mcpp {
namespace detail {

enum class HighlightType {
    file, executable, directory, directory_link, link, hardlink, symlink,
    stale_link, device, block_device, character_device, special, socket, fifo, door,
};

struct FileHighlightRule {
    std::string group;
    std::optional<HighlightType> type;
    std::optional<NativeRegex> expression;
    std::vector<std::string> extensions;
    bool case_sensitive{false};
    std::string pattern;
    std::optional<SkinPair> direct_color;
};

struct FileHighlightData {
    std::filesystem::path resource;
    std::vector<FileHighlightRule> rules;
};

}  // namespace detail
namespace {

using detail::color_resource_error;
using detail::color_trim;
using detail::FileHighlightRule;
using detail::HighlightType;

std::string lower(std::string value) {
    std::ranges::transform(value, value.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

std::optional<HighlightType> parse_type(const std::string_view value) {
    static const std::map<std::string_view, HighlightType, std::less<>> types{
        {"FILE", HighlightType::file}, {"FILE_EXE", HighlightType::executable},
        {"DIR", HighlightType::directory}, {"LINK_DIR", HighlightType::directory_link},
        {"LINK", HighlightType::link}, {"HARDLINK", HighlightType::hardlink},
        {"SYMLINK", HighlightType::symlink}, {"STALE_LINK", HighlightType::stale_link},
        {"DEVICE", HighlightType::device}, {"DEVICE_BLOCK", HighlightType::block_device},
        {"DEVICE_CHAR", HighlightType::character_device}, {"SPECIAL", HighlightType::special},
        {"SPECIAL_SOCKET", HighlightType::socket}, {"SPECIAL_FIFO", HighlightType::fifo},
        {"SPECIAL_DOOR", HighlightType::door},
    };
    const auto found = types.find(value);
    return found == types.end() ? std::nullopt : std::optional{found->second};
}

std::string regex_value(const std::string_view source) {
    std::string result;
    for (std::size_t index = 0; index < source.size(); ++index) {
        const auto character = source[index];
        result += character;
        if (character == '\\' && index + 1U < source.size() && source[index + 1U] == '\\') ++index;
    }
    return result;
}

NativeRegex compile_expression(
    const detail::ColorSetting& setting, const std::filesystem::path& path
) {
    try {
        return NativeRegex(regex_value(setting.value));
    } catch (const std::invalid_argument& error) {
        color_resource_error(path, setting.line, "invalid filename regexp: " +
                             std::string(error.what()));
    }
}

std::vector<std::string> extension_list(
    const detail::ColorSetting& setting, const std::filesystem::path& path, const bool case_sensitive
) {
    std::vector<std::string> result;
    std::string extension;
    const auto append = [&] {
        auto value = std::string(color_trim(extension));
        if (value.starts_with('.')) value.erase(0, 1);
        if (!value.empty()) result.push_back(case_sensitive ? std::move(value) : lower(std::move(value)));
        extension.clear();
    };
    for (std::size_t index = 0; index < setting.value.size(); ++index) {
        const auto character = setting.value[index];
        if (character == ';') append();
        else if (character == '\\' && index + 1U < setting.value.size() &&
                 (setting.value[index + 1U] == ';' || setting.value[index + 1U] == '\\')) {
            extension += setting.value[++index];
        } else extension += character;
    }
    append();
    if (result.empty()) color_resource_error(path, setting.line, "empty filename extension list");
    return result;
}

SkinPair legacy_color(const detail::ColorSetting& setting, const std::filesystem::path& path) {
    std::array<std::uint8_t, 3> values{};
    const std::string_view source(setting.value);
    std::size_t begin = 0;
    for (std::size_t index = 0; index < values.size(); ++index) {
        const auto end = source.find(',', begin);
        if ((end == std::string_view::npos) != (index + 1U == values.size())) {
            color_resource_error(path, setting.line, "highlight color needs exactly red,green,blue");
        }
        auto value = color_trim(source.substr(begin,
            end == std::string_view::npos ? source.size() - begin : end - begin));
        if (value.starts_with('+')) value.remove_prefix(1);
        unsigned component{};
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), component);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || component > 255U) {
            color_resource_error(path, setting.line, "highlight color values must be 0 through 255");
        }
        values[index] = static_cast<std::uint8_t>(component);
        begin = end == std::string_view::npos ? source.size() : end + 1U;
    }
    return {ftxui::Color::RGB(values[0], values[1], values[2]), ftxui::Color{}};
}

bool type_matches(const HighlightType type, const Entry& entry, const bool stale) {
    const auto symlink = entry.kind == EntryKind::symlink;
    const auto directory = entry.kind == EntryKind::directory || (symlink && entry.target_is_directory);
    const auto hardlink = entry.kind == EntryKind::file && entry.links > 1U;
    const auto executable = entry.kind == EntryKind::file &&
        (entry.permissions & (std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec |
                              std::filesystem::perms::others_exec)) != std::filesystem::perms::none;
    switch (type) {
        case HighlightType::file: return entry.kind == EntryKind::file;
        case HighlightType::executable: return executable;
        case HighlightType::directory: return directory;
        case HighlightType::directory_link: return symlink && entry.target_is_directory;
        case HighlightType::link: return hardlink || (symlink && !stale);
        case HighlightType::hardlink: return hardlink;
        case HighlightType::symlink: return symlink && !stale;
        case HighlightType::stale_link: return symlink && stale;
        case HighlightType::device: return S_ISBLK(entry.mode) || S_ISCHR(entry.mode);
        case HighlightType::block_device: return S_ISBLK(entry.mode);
        case HighlightType::character_device: return S_ISCHR(entry.mode);
        case HighlightType::special: return S_ISFIFO(entry.mode) || S_ISSOCK(entry.mode);
        case HighlightType::socket: return S_ISSOCK(entry.mode);
        case HighlightType::fifo: return S_ISFIFO(entry.mode);
        case HighlightType::door: return false;
    }
    return false;
}

}  // namespace

std::filesystem::path find_file_highlight_file(const std::filesystem::path& config_directory) {
    for (const auto& path : {config_directory / "filehighlight.ini", config_directory / "highlighting",
                            config_directory.parent_path() / "mc/filehighlight.ini",
                            std::filesystem::path("/etc/mcpp/filehighlight.ini"),
                            std::filesystem::path("/usr/share/mcpp/filehighlight.ini"),
                            std::filesystem::path("/etc/mc/filehighlight.ini"),
                            std::filesystem::path("/usr/share/mc/filehighlight.ini")}) {
        if (detail::color_resource_exists(path)) return path;
    }
    return config_directory / "filehighlight.ini";
}

FileHighlightRules FileHighlightRules::load(
    const std::filesystem::path& config_directory, const std::filesystem::path& previous_resource
) {
    if (!previous_resource.empty() && !detail::color_resource_exists(previous_resource)) {
        return load_file(previous_resource, previous_resource.filename() == "highlighting");
    }
    const auto path = find_file_highlight_file(config_directory);
    if (detail::color_resource_exists(path)) return load_file(path, path.filename() == "highlighting");
    if (!previous_resource.empty()) return load_file(previous_resource, previous_resource.filename() == "highlighting");
    return {};
}

FileHighlightRules FileHighlightRules::load_file(const std::filesystem::path& path, const bool legacy) {
    const auto document = detail::read_color_resource(path, legacy);
    auto data = std::make_shared<detail::FileHighlightData>();
    data->resource = std::filesystem::absolute(path).lexically_normal();
    for (const auto& group : document) {
        if (legacy) {
            if (!group.name.empty()) color_resource_error(path, group.line, "legacy highlighting uses pattern=red,green,blue");
            for (const auto& setting : group.settings) {
                FileHighlightRule rule;
                rule.pattern = setting.key;
                rule.direct_color = legacy_color(setting, path);
                try {
                    static_cast<void>(EditorSearch::filename_matches("", rule.pattern, true));
                } catch (const std::exception& error) {
                    color_resource_error(path, setting.line, error.what());
                }
                data->rules.push_back(std::move(rule));
            }
            continue;
        }
        FileHighlightRule rule;
        rule.group = group.name;
        for (const auto& setting : group.settings) {
            if (setting.key != "type" && setting.key != "regexp" &&
                setting.key != "extensions" && setting.key != "extensions_case") {
                color_resource_error(path, setting.line, "unsupported filename-highlighting key: " + setting.key);
            }
        }
        if (const auto* type = group.find("type")) {
            rule.type = parse_type(type->value);
            if (!rule.type) color_resource_error(path, type->line, "unknown filename type: " + type->value);
        } else if (const auto* expression = group.find("regexp")) {
            if (expression->value.empty()) color_resource_error(path, expression->line, "empty filename regexp");
            rule.expression = compile_expression(*expression, path);
        } else if (const auto* extensions = group.find("extensions")) {
            if (const auto* sensitive = group.find("extensions_case")) {
                rule.case_sensitive = detail::color_boolean(*sensitive, path);
            }
            rule.extensions = extension_list(*extensions, path, rule.case_sensitive);
        } else color_resource_error(path, group.line, "filename-highlighting group needs type, regexp or extensions");
        data->rules.push_back(std::move(rule));
    }
    FileHighlightRules result;
    result.data_ = std::move(data);
    return result;
}

std::optional<SkinPair> FileHighlightRules::color_for(
    const Entry& entry, const Skin& skin, const bool local_entry
) const {
    if (!data_) return {};
    std::optional<bool> stale;
    for (const auto& rule : data_->rules) {
        bool matches = false;
        if (rule.direct_color) {
            matches = EditorSearch::filename_matches(entry.name, rule.pattern, true);
        } else if (rule.type) {
            if (!stale && local_entry && entry.kind == EntryKind::symlink &&
                (*rule.type == HighlightType::stale_link || *rule.type == HighlightType::link ||
                 *rule.type == HighlightType::symlink)) {
                struct stat target{};
                const auto result = ::stat(entry.path.c_str(), &target);
                stale = result != 0 && (errno == ENOENT || errno == ENOTDIR || errno == ELOOP);
            }
            matches = type_matches(*rule.type, entry, stale.value_or(false));
        } else if (rule.expression) {
            matches = rule.expression->search(entry.name);
        } else {
            const auto name = rule.case_sensitive ? entry.name : lower(entry.name);
            matches = std::ranges::any_of(rule.extensions, [&](const std::string& extension) {
                return name.size() > extension.size() && name.ends_with("." + extension);
            });
        }
        if (!matches) continue;
        if (rule.direct_color) {
            auto color = *rule.direct_color;
            color.background = skin.normal.background;
            color.attributes = skin.normal.attributes;
            color.explicit_style = skin.normal.explicit_style;
            return color;
        }
        if (skin.resource.empty()) return {};
        return skin_color(skin, "filehighlight", rule.group, skin.normal);
    }
    return {};
}

std::filesystem::path FileHighlightRules::resource() const {
    return data_ ? data_->resource : std::filesystem::path{};
}

}  // namespace mcpp
