#include "mcpp/tree_cache.hpp"
#include "mcpp/local_provider.hpp"

#include <array>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <sys/stat.h>
#include <unistd.h>

namespace mcpp {

std::filesystem::path tree_cache_path() {
    if (const auto* value = std::getenv("XDG_CACHE_HOME"); value != nullptr && *value != '\0') {
        return std::filesystem::path(value) / "mcpp/Tree";
    }
    if (const auto* value = std::getenv("HOME"); value != nullptr && *value != '\0') {
        return std::filesystem::path(value) / ".cache/mcpp/Tree";
    }
    throw std::runtime_error("HOME and XDG_CACHE_HOME are unavailable for the directory Tree cache");
}

std::set<std::string> load_tree_cache(const std::filesystem::path& path) {
    const int descriptor = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (descriptor < 0) {
        if (errno == ENOENT) return {};
        throw std::system_error(errno, std::generic_category(), "open directory Tree cache");
    }
    struct Descriptor {
        int value;
        ~Descriptor() { close(value); }
    } owned{descriptor};
    struct stat status{};
    if (fstat(descriptor, &status) != 0) throw std::system_error(errno, std::generic_category(), "stat directory Tree cache");
    if (!S_ISREG(status.st_mode) || status.st_uid != geteuid() || (status.st_mode & 0022) != 0) {
        throw std::runtime_error("directory Tree cache must be an owned regular file without group/other write access");
    }
    std::string content;
    std::array<char, 8192> buffer{};
    for (;;) {
        const auto count = read(descriptor, buffer.data(), buffer.size());
        if (count == 0) break;
        if (count < 0) {
            if (errno == EINTR) continue;
            throw std::system_error(errno, std::generic_category(), "read directory Tree cache");
        }
        content.append(buffer.data(), static_cast<std::size_t>(count));
    }
    std::istringstream input(content);
    std::set<std::string> directories;
    input >> std::ws;
    while (!input.eof()) {
        std::string directory;
        if (!(input >> std::quoted(directory)) || directory.find('\0') != std::string::npos ||
            !std::filesystem::path(directory).is_absolute()) {
            throw std::runtime_error("invalid directory Tree cache");
        }
        directories.insert(std::filesystem::path(directory).lexically_normal().string());
        input >> std::ws;
    }
    return directories;
}

void save_tree_cache(const std::filesystem::path& path, const std::set<std::string>& directories) {
    const bool created = std::filesystem::create_directories(path.parent_path());
    if (created && chmod(path.parent_path().c_str(), 0700) != 0) {
        throw std::system_error(errno, std::generic_category(), "protect directory Tree cache");
    }
    struct stat status{};
    if (lstat(path.c_str(), &status) == 0) {
        if (!S_ISREG(status.st_mode) || status.st_uid != geteuid() || (status.st_mode & 0022) != 0) {
            throw std::runtime_error("refusing an unsafe directory Tree cache destination");
        }
    } else if (errno != ENOENT) {
        throw std::system_error(errno, std::generic_category(), "stat directory Tree cache destination");
    }
    std::ostringstream output;
    for (const auto& directory : directories) output << std::quoted(directory) << '\n';
    const auto content = output.str();
    std::istringstream input(content);
    LocalProvider provider("/");
    provider.write_with_permissions(path.string(), input, content.size(), true, {}, {},
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
}

}  // namespace mcpp
