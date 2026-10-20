#include "color_resource.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
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
    const auto first = value.find_first_not_of(" \t\r");
    if (first == std::string_view::npos) return {};
    return value.substr(first, value.find_last_not_of(" \t\r") - first + 1U);
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
