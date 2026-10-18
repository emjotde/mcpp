#include "mcpp/config.hpp"

#include <algorithm>
#include <charconv>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <fcntl.h>
#include <sstream>
#include <map>
#include <stdexcept>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace mcpp {
namespace {

bool parse_bool(const std::string& value, const std::string& key) {
    auto normalized = value;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](const unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    if (normalized == "true" || normalized == "1" || normalized == "yes") {
        return true;
    }
    if (normalized == "false" || normalized == "0" || normalized == "no") {
        return false;
    }
    throw std::runtime_error("invalid boolean setting: " + key + "=" + value);
}

std::size_t parse_width(
    const std::string_view value,
    const std::size_t fallback
) {
    std::size_t parsed = 0;
    const auto result = std::from_chars(
        value.data(),
        value.data() + value.size(),
        parsed
    );
    if (result.ec != std::errc{} ||
        result.ptr != value.data() + value.size() ||
        parsed < 1U ||
        parsed > 32U) {
        return fallback;
    }
    return parsed;
}

std::string sort_name(const SortField field) {
    switch (field) {
        case SortField::extension:
            return "extension";
        case SortField::size:
            return "size";
        case SortField::modified:
            return "modified";
        case SortField::name:
            return "name";
    }
    return "name";
}

SortField parse_sort(const std::string& value) {
    if (value == "extension") {
        return SortField::extension;
    }
    if (value == "size") {
        return SortField::size;
    }
    if (value == "modified") {
        return SortField::modified;
    }
    return SortField::name;
}

std::string encode_value(const std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const auto character : value) {
        if (character == '\\') {
            result += "\\\\";
        } else if (character == '\n') {
            result += "\\n";
        } else if (character == '\r') {
            result += "\\r";
        } else {
            result += character;
        }
    }
    return result;
}

std::string decode_value(const std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] != '\\' || index + 1U >= value.size()) {
            result += value[index];
            continue;
        }
        const auto escaped = value[++index];
        if (escaped == 'n') {
            result += '\n';
        } else if (escaped == 'r') {
            result += '\r';
        } else {
            result += escaped;
        }
    }
    return result;
}

}  // namespace

Config Config::load(const std::filesystem::path& path) {
    Config result;
    std::ifstream input(path);
    if (!input) {
        std::error_code error;
        const bool present = std::filesystem::exists(path, error);
        if (error || present) {
            throw std::runtime_error("cannot read configuration: " + path.string());
        }
        return result;
    }
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line.starts_with('#')) {
            continue;
        }
        const auto equals = line.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        const auto key = line.substr(0, equals);
        const auto value = line.substr(equals + 1U);
        if (key == "show_hidden") {
            result.show_hidden = parse_bool(value, key);
        } else if (key == "confirm_delete") {
            result.confirm_delete = parse_bool(value, key);
        } else if (key == "confirm_overwrite") {
            result.confirm_overwrite = parse_bool(value, key);
        } else if (key == "directories_first") {
            result.directories_first = parse_bool(value, key);
        } else if (key == "sort") {
            result.sort = parse_sort(value);
        } else if (key == "sort_descending") {
            result.sort_descending = parse_bool(value, key);
        } else if (key == "theme") {
            result.theme = decode_value(value);
        } else if (key == "editor_syntax") {
            result.editor_syntax = parse_bool(value, key);
        } else if (key == "editor_line_numbers") {
            result.editor_line_numbers = parse_bool(value, key);
        } else if (key == "editor_tab_width") {
            result.editor_tab_width = parse_width(
                value,
                result.editor_tab_width
            );
        } else if (key == "editor_indent_width") {
            result.editor_indent_width = parse_width(
                value,
                result.editor_indent_width
            );
        } else if (key == "editor_auto_indent") {
            result.editor_auto_indent = parse_bool(value, key);
        } else if (key == "editor_show_whitespace") {
            result.editor_show_whitespace = parse_bool(value, key);
        } else if (key == "bookmark") {
            result.bookmarks.push_back(decode_value(value));
        } else if (key == "editor_history") {
            result.editor_history.push_back(decode_value(value));
        }
    }
    if (input.bad()) {
        throw std::runtime_error("failed while reading configuration: " + path.string());
    }
    return result;
}

void Config::save(const std::filesystem::path& path) const {
    const auto created =
        std::filesystem::create_directories(path.parent_path());
    if (created) {
        static_cast<void>(chmod(path.parent_path().c_str(), 0700));
    }
    std::ostringstream output;
    output << "show_hidden=" << (show_hidden ? "true" : "false")
           << '\n'
           << "confirm_delete=" << (confirm_delete ? "true" : "false")
           << '\n'
           << "confirm_overwrite="
           << (confirm_overwrite ? "true" : "false") << '\n'
           << "directories_first="
           << (directories_first ? "true" : "false") << '\n'
           << "sort=" << sort_name(sort) << '\n'
           << "sort_descending="
           << (sort_descending ? "true" : "false") << '\n'
           << "theme=" << encode_value(theme) << '\n'
           << "editor_syntax="
           << (editor_syntax ? "true" : "false") << '\n'
           << "editor_line_numbers="
           << (editor_line_numbers ? "true" : "false") << '\n'
           << "editor_tab_width=" << editor_tab_width << '\n'
           << "editor_indent_width=" << editor_indent_width << '\n'
           << "editor_auto_indent="
           << (editor_auto_indent ? "true" : "false") << '\n'
           << "editor_show_whitespace="
           << (editor_show_whitespace ? "true" : "false") << '\n';
    for (const auto& bookmark : bookmarks) {
        output << "bookmark=" << encode_value(bookmark) << '\n';
    }
    for (const auto& history_path : editor_history) {
        output << "editor_history="
               << encode_value(history_path) << '\n';
    }
    if (!output) {
        throw std::runtime_error("cannot serialize configuration");
    }
    const auto content = std::move(output).str();
    auto pattern =
        (path.parent_path() / ".mcpp-config-XXXXXX").string();
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    int descriptor = mkostemp(buffer.data(), O_CLOEXEC);
    if (descriptor < 0) {
        throw std::runtime_error(
            "cannot create configuration temporary file: " +
            std::string(std::strerror(errno))
        );
    }
    const std::filesystem::path temporary(buffer.data());
    try {
        if (fchmod(descriptor, 0600) != 0) {
            throw std::runtime_error(
                "cannot protect configuration temporary file"
            );
        }
        std::size_t offset = 0;
        while (offset < content.size()) {
            const auto count = ::write(
                descriptor,
                content.data() + offset,
                content.size() - offset
            );
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count <= 0) {
                throw std::runtime_error(
                    "cannot write configuration"
                );
            }
            offset += static_cast<std::size_t>(count);
        }
        if (fsync(descriptor) != 0) {
            throw std::runtime_error(
                "cannot commit configuration"
            );
        }
        if (close(descriptor) != 0) {
            descriptor = -1;
            throw std::runtime_error(
                "cannot close configuration"
            );
        }
        descriptor = -1;
        std::filesystem::rename(temporary, path);
    } catch (...) {
        if (descriptor >= 0) {
            static_cast<void>(close(descriptor));
        }
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}

std::filesystem::path default_config_path() {
    if (const auto* value = std::getenv("XDG_CONFIG_HOME");
        value != nullptr && *value != '\0') {
        return std::filesystem::path(value) / "mcpp" / "config";
    }
    if (const auto* value = std::getenv("HOME");
        value != nullptr && *value != '\0') {
        return std::filesystem::path(value) /
               ".config" / "mcpp" / "config";
    }
    return std::filesystem::path(".mcpp-config");
}

}  // namespace mcpp
