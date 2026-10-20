#include "color_resource.hpp"

#include "mcpp/skin.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <fcntl.h>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>

namespace mcpp::detail {

const ColorSetting* ColorSection::find(const std::string_view key) const {
    const auto found = std::ranges::find(settings, key, &ColorSetting::key);
    return found == settings.end() ? nullptr : &*found;
}

std::string_view color_trim(const std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1U);
}

ftxui::Color parse_color_value(
    const std::string_view value, const std::optional<ftxui::Color> base
) {
    const auto name = color_trim(value);
    if (name == "base" && base) return *base;
    if (name == "default") return {};
    static constexpr std::array<std::string_view, 16> names{
        "black", "red", "green", "brown", "blue", "magenta", "cyan", "lightgray",
        "gray", "brightred", "brightgreen", "yellow", "brightblue", "brightmagenta",
        "brightcyan", "white"
    };
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
    if (name.starts_with('#') && (name.size() == 4U || name.size() == 7U)) {
        std::array<std::uint8_t, 3> components{};
        const auto digits = (name.size() - 1U) / 3U;
        bool valid = true;
        for (std::size_t index = 0; index < components.size(); ++index) {
            const auto fragment = name.substr(1U + index * digits, digits);
            unsigned component{};
            const auto parsed = std::from_chars(fragment.data(), fragment.data() + fragment.size(), component, 16);
            valid = valid && parsed.ec == std::errc{} && parsed.ptr == fragment.data() + fragment.size();
            components[index] = static_cast<std::uint8_t>(digits == 1U ? component * 17U : component);
        }
        if (valid) return ftxui::Color::RGB(components[0], components[1], components[2]);
    }
    throw std::invalid_argument("unknown color or alias: " + std::string(name));
}

unsigned parse_color_attributes(const std::string_view value) {
    const auto attributes = color_trim(value);
    if (attributes == "none") return 0;
    unsigned result = 0;
    std::size_t begin = 0;
    for (;;) {
        const auto end = attributes.find('+', begin);
        const auto attribute = color_trim(attributes.substr(begin,
            end == std::string_view::npos ? attributes.size() - begin : end - begin));
        if (attribute == "bold") result |= SkinPair::bold;
        else if (attribute == "italic") result |= SkinPair::italic;
        else if (attribute == "underline") result |= SkinPair::underline;
        else if (attribute == "reverse" || attribute == "inverse") result |= SkinPair::reverse;
        else if (attribute == "blink") result |= SkinPair::blink;
        else throw std::invalid_argument("unknown color attribute or alias: " + std::string(attribute));
        if (end == std::string_view::npos) break;
        begin = end + 1U;
    }
    return result;
}

[[noreturn]] void color_resource_error(
    const std::filesystem::path& path, const std::size_t line, const std::string_view message
) {
    throw std::runtime_error(path.string() + (line == 0U ? "" : ":" + std::to_string(line)) +
                             ": " + std::string(message));
}

bool color_resource_exists(const std::filesystem::path& path) {
    struct stat state{};
    if (::lstat(path.c_str(), &state) == 0) return true;
    if (errno == ENOENT || errno == ENOTDIR) return false;
    throw std::system_error(errno, std::generic_category(), "inspect color resource " + path.string());
}

std::vector<ColorSection> read_color_resource(
    const std::filesystem::path& path, const bool allow_global
) {
    auto descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (descriptor < 0) {
        throw std::system_error(errno, std::generic_category(), "open color resource " + path.string());
    }
    std::string contents;
    try {
        struct stat state{};
        if (::fstat(descriptor, &state) != 0) {
            throw std::system_error(errno, std::generic_category(), "inspect color resource " + path.string());
        }
        if (!S_ISREG(state.st_mode)) color_resource_error(path, 0, "expected a regular color resource file");
        std::array<char, 4096> buffer{};
        for (;;) {
            const auto count = ::read(descriptor, buffer.data(), buffer.size());
            if (count < 0 && errno == EINTR) continue;
            if (count < 0) {
                throw std::system_error(errno, std::generic_category(), "read color resource " + path.string());
            }
            if (count == 0) break;
            if (contents.size() + static_cast<std::size_t>(count) > 1024U * 1024U) {
                color_resource_error(path, 0, "color resource exceeds 1 MiB");
            }
            contents.append(buffer.data(), static_cast<std::size_t>(count));
        }
        const auto result = ::close(descriptor);
        descriptor = -1;
        if (result != 0) {
            throw std::system_error(errno, std::generic_category(), "close color resource " + path.string());
        }
    } catch (...) {
        if (descriptor >= 0) ::close(descriptor);
        throw;
    }
    if (contents.find('\0') != std::string::npos) color_resource_error(path, 0, "NUL byte in color resource");
    if (contents.starts_with("\xef\xbb\xbf")) contents.erase(0, 3);
    std::vector<ColorSection> sections;
    if (allow_global) sections.push_back({"", 0, {}});
    std::size_t current_section = 0;
    std::istringstream input(contents);
    std::string line;
    std::size_t number = 0;
    while (std::getline(input, line)) {
        ++number;
        const auto value = color_trim(line);
        if (value.empty() || value.front() == '#') continue;
        if (value.front() == '[') {
            if (value.back() != ']') color_resource_error(path, number, "unterminated section name");
            const auto name = color_trim(value.substr(1, value.size() - 2U));
            if (name.empty()) color_resource_error(path, number, "empty section name");
            const auto found = std::ranges::find(sections, name, &ColorSection::name);
            if (found == sections.end()) {
                current_section = sections.size();
                sections.push_back({std::string(name), number, {}});
            } else {
                current_section = static_cast<std::size_t>(found - sections.begin());
            }
            continue;
        }
        if (sections.empty()) color_resource_error(path, number, "expected a [section] before a setting");
        const auto equals = value.find('=');
        if (equals == std::string_view::npos) color_resource_error(path, number, "expected key=value");
        const auto key = color_trim(value.substr(0, equals));
        if (key.empty()) color_resource_error(path, number, "empty setting name");
        auto& settings = sections[current_section].settings;
        const auto found = std::ranges::find(settings, key, &ColorSetting::key);
        ColorSetting setting{std::string(key), std::string(color_trim(value.substr(equals + 1U))), number};
        if (found != settings.end() && !(allow_global && sections[current_section].name.empty())) {
            *found = std::move(setting);
        } else settings.push_back(std::move(setting));
    }
    return sections;
}

bool color_boolean(const ColorSetting& setting, const std::filesystem::path& path) {
    auto value = setting.value;
    std::ranges::transform(value, value.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    if (value == "true" || value == "yes" || value == "1") return true;
    if (value == "false" || value == "no" || value == "0") return false;
    color_resource_error(path, setting.line, "expected true or false for " + setting.key);
}

}  // namespace mcpp::detail
