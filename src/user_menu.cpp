#include "mcpp/user_menu.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace mcpp {
namespace {

std::string shell_quote(const std::string_view value) {
    std::string result{"'"};
    for (const auto character : value) {
        if (character == '\'') {
            result += "'\\''";
        } else {
            result += character;
        }
    }
    result += '\'';
    return result;
}

std::vector<UserMenuEntry> default_menu() {
    return {
        {
            .key = 'i',
            .label = "Inspect selected path",
            .command = "stat -- %f",
        },
        {
            .key = 'd',
            .label = "Disk usage of selected path",
            .command = "du -sh -- %f",
        },
        {
            .key = 't',
            .label = "Show directory tree",
            .command = "find %d -maxdepth 2 -type d -print",
        },
    };
}

void validate_menu_file(
    const std::filesystem::path& path,
    const struct stat& status
) {
    if (!S_ISREG(status.st_mode) ||
        (status.st_uid != geteuid() && status.st_uid != 0U) ||
        (status.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        throw std::runtime_error(
            "refusing insecure user menu: " + path.string()
        );
    }
}

}  // namespace

std::vector<UserMenuEntry> load_user_menu(
    const std::filesystem::path& path
) {
    const int descriptor = open(
        path.c_str(),
        O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK
    );
    if (descriptor < 0) {
        if (errno == ENOENT) {
            return default_menu();
        }
        throw std::runtime_error(
            "cannot open user menu: " +
            std::string(std::strerror(errno))
        );
    }
    struct stat status {};
    if (fstat(descriptor, &status) != 0) {
        const auto failure = errno;
        close(descriptor);
        throw std::runtime_error(
            "cannot inspect user menu: " +
            std::string(std::strerror(failure))
        );
    }
    try {
        validate_menu_file(path, status);
    } catch (...) {
        close(descriptor);
        throw;
    }
    std::string content;
    std::array<char, 4096> buffer{};
    while (true) {
        const auto count = read(
            descriptor,
            buffer.data(),
            buffer.size()
        );
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            const auto failure = errno;
            close(descriptor);
            throw std::runtime_error(
                "cannot read user menu: " +
                std::string(std::strerror(failure))
            );
        }
        if (count == 0) {
            break;
        }
        content.append(
            buffer.data(),
            static_cast<std::size_t>(count)
        );
    }
    close(descriptor);
    std::istringstream input(content);
    std::vector<UserMenuEntry> result;
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line.starts_with('#')) {
            continue;
        }
        const auto first = line.find('|');
        const auto second =
            first == std::string::npos
                ? std::string::npos
                : line.find('|', first + 1U);
        if (first != 1U || second == std::string::npos ||
            second + 1U >= line.size()) {
            continue;
        }
        result.push_back({
            .key = line[0],
            .label = line.substr(first + 1U, second - first - 1U),
            .command = line.substr(second + 1U),
        });
    }
    return result;
}

std::filesystem::path default_user_menu_path() {
    if (const auto* value = std::getenv("XDG_CONFIG_HOME");
        value != nullptr && *value != '\0') {
        return std::filesystem::path(value) / "mcpp" / "menu";
    }
    if (const auto* value = std::getenv("HOME");
        value != nullptr && *value != '\0') {
        return std::filesystem::path(value) /
               ".config" / "mcpp" / "menu";
    }
    return std::filesystem::path(".mcpp-menu");
}

std::string expand_user_menu_command(
    const std::string_view command,
    const std::string_view selected_path,
    const std::string_view current_directory,
    const std::string_view other_directory
) {
    std::string result;
    result.reserve(command.size());
    char quote = '\0';
    bool escaped = false;
    for (std::size_t index = 0; index < command.size(); ++index) {
        const auto character = command[index];
        if (escaped) {
            result += character;
            escaped = false;
            continue;
        }
        if (character == '\\' && quote != '\'') {
            result += character;
            escaped = true;
            continue;
        }
        if ((character == '\'' || character == '"')) {
            if (quote == '\0') {
                quote = character;
            } else if (quote == character) {
                quote = '\0';
            }
            result += character;
            continue;
        }
        if (command[index] != '%' ||
            index + 1U >= command.size()) {
            result += command[index];
            continue;
        }
        const auto token = command[index + 1U];
        if ((token == 'f' || token == 'd' || token == 'D') &&
            quote != '\0') {
            throw std::invalid_argument(
                "user-menu placeholders must not appear inside quotes"
            );
        }
        if (token == 'f') {
            result += shell_quote(selected_path);
        } else if (token == 'd') {
            result += shell_quote(current_directory);
        } else if (token == 'D') {
            result += shell_quote(other_directory);
        } else if (token == '%') {
            result += '%';
        } else {
            result += '%';
            result += token;
        }
        ++index;
    }
    return result;
}

}  // namespace mcpp
