#include "mcpp/config.hpp"

#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <fcntl.h>
#include <sstream>
#include <map>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace mcpp {
namespace {

bool parse_bool(const std::string& value) {
    return value == "true" || value == "1" || value == "yes";
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

}  // namespace

Config Config::load(const std::filesystem::path& path) {
    Config result;
    std::ifstream input(path);
    if (!input) {
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
            result.show_hidden = parse_bool(value);
        } else if (key == "confirm_delete") {
            result.confirm_delete = parse_bool(value);
        } else if (key == "confirm_overwrite") {
            result.confirm_overwrite = parse_bool(value);
        } else if (key == "directories_first") {
            result.directories_first = parse_bool(value);
        } else if (key == "sort") {
            result.sort = parse_sort(value);
        } else if (key == "sort_descending") {
            result.sort_descending = parse_bool(value);
        } else if (key == "theme") {
            result.theme = value;
        } else if (key == "left_path") {
            result.left_path = value;
        } else if (key == "right_path") {
            result.right_path = value;
        } else if (key == "bookmark") {
            result.bookmarks.push_back(value);
        }
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
           << "theme=" << theme << '\n'
           << "left_path=" << left_path << '\n'
           << "right_path=" << right_path << '\n';
    for (const auto& bookmark : bookmarks) {
        output << "bookmark=" << bookmark << '\n';
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
