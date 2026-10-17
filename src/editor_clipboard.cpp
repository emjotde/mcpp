#include "mcpp/editor_clipboard.hpp"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace mcpp {
namespace {

constexpr std::string_view clipboard_header{
    "mcpp-clipboard-v1\n"
};
constexpr std::size_t maximum_clipboard_size =
    16U * 1024U * 1024U;
constexpr std::size_t maximum_clipboard_overhead =
    clipboard_header.size() + std::string_view("column\n").size();

}  // namespace

std::filesystem::path editor_clipboard_path() {
    if (const auto* value = std::getenv("XDG_CACHE_HOME");
        value != nullptr && *value != '\0') {
        return std::filesystem::path(value) /
               "mcpp" / "mcedit.clip";
    }
    if (const auto* value = std::getenv("HOME");
        value != nullptr && *value != '\0') {
        return std::filesystem::path(value) /
               ".cache" / "mcpp" / "mcedit.clip";
    }
    return ".mcpp-editor-clip";
}

void save_editor_clipboard(
    const std::filesystem::path& path,
    const std::string_view content,
    const bool column
) {
    if (content.size() > maximum_clipboard_size) {
        throw std::runtime_error("clipboard content is too large");
    }
    const auto created =
        std::filesystem::create_directories(path.parent_path());
    if (created) {
        static_cast<void>(chmod(path.parent_path().c_str(), 0700));
    }
    std::string serialized{clipboard_header};
    serialized += column ? "column\n" : "linear\n";
    serialized += content;
    auto pattern =
        (path.parent_path() / ".mcpp-clip-XXXXXX").string();
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    int descriptor = mkostemp(buffer.data(), O_CLOEXEC);
    if (descriptor < 0) {
        throw std::runtime_error(
            "cannot create clipboard temporary file: " +
            std::string(std::strerror(errno))
        );
    }
    const std::filesystem::path temporary(buffer.data());
    try {
        if (fchmod(descriptor, 0600) != 0) {
            throw std::runtime_error(
                "cannot protect clipboard file"
            );
        }
        std::size_t offset = 0;
        while (offset < serialized.size()) {
            const auto count = write(
                descriptor,
                serialized.data() + offset,
                serialized.size() - offset
            );
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count <= 0) {
                throw std::runtime_error(
                    "cannot write clipboard file"
                );
            }
            offset += static_cast<std::size_t>(count);
        }
        if (fsync(descriptor) != 0) {
            throw std::runtime_error(
                "cannot commit clipboard file"
            );
        }
        if (close(descriptor) != 0) {
            descriptor = -1;
            throw std::runtime_error(
                "cannot commit clipboard file"
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

std::string load_editor_clipboard(
    const std::filesystem::path& path,
    bool* const column
) {
    if (column != nullptr) {
        *column = false;
    }
    const int descriptor = open(
        path.c_str(),
        O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK
    );
    if (descriptor < 0) {
        if (errno == ENOENT) {
            return {};
        }
        throw std::runtime_error(
            "cannot open clipboard file: " +
            std::string(std::strerror(errno))
        );
    }
    struct stat status {};
    if (fstat(descriptor, &status) != 0 ||
        !S_ISREG(status.st_mode) ||
        status.st_size < 0 ||
        static_cast<std::uint64_t>(status.st_size) >
            maximum_clipboard_size +
                maximum_clipboard_overhead) {
        static_cast<void>(close(descriptor));
        throw std::runtime_error("invalid clipboard file");
    }
    std::string result(
        static_cast<std::size_t>(status.st_size),
        '\0'
    );
    std::size_t offset = 0;
    while (offset < result.size()) {
        const auto count = read(
            descriptor,
            result.data() + offset,
            result.size() - offset
        );
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            static_cast<void>(close(descriptor));
            throw std::runtime_error(
                "cannot read clipboard file"
            );
        }
        offset += static_cast<std::size_t>(count);
    }
    if (close(descriptor) != 0) {
        throw std::runtime_error("cannot close clipboard file");
    }
    if (!result.starts_with(clipboard_header)) {
        if (result.size() > maximum_clipboard_size) {
            throw std::runtime_error(
                "clipboard content is too large"
            );
        }
        return result;
    }
    const auto mode_begin = clipboard_header.size();
    const auto content_begin = result.find('\n', mode_begin);
    if (content_begin == std::string::npos) {
        throw std::runtime_error("invalid clipboard header");
    }
    const auto mode = std::string_view(result).substr(
        mode_begin,
        content_begin - mode_begin
    );
    if (mode != "linear" && mode != "column") {
        throw std::runtime_error("invalid clipboard mode");
    }
    if (column != nullptr) {
        *column = mode == "column";
    }
    result.erase(0, content_begin + 1U);
    if (result.size() > maximum_clipboard_size) {
        throw std::runtime_error("clipboard content is too large");
    }
    return result;
}

}  // namespace mcpp
