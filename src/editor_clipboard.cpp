#include "mcpp/editor_clipboard.hpp"
#include "mcpp/local_tools.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <utility>
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
constexpr auto command_timeout = std::chrono::seconds(2);

std::string serialize_clipboard(
    const std::string_view content,
    const bool column
) {
    if (content.size() > maximum_clipboard_size) {
        throw std::runtime_error("clipboard content is too large");
    }
    std::string serialized{clipboard_header};
    serialized += column ? "column\n" : "linear\n";
    serialized += content;
    return serialized;
}

class ClipboardFile {
public:
    ClipboardFile(
        const std::filesystem::path& destination,
        const std::string_view content
    ) {
        if (destination.native().find('\0') != std::string::npos) {
            throw std::invalid_argument("clipboard path cannot contain NUL");
        }
        const auto directory = destination.has_parent_path()
            ? destination.parent_path() : std::filesystem::path(".");
        const auto created = std::filesystem::create_directories(directory);
        if (created && chmod(directory.c_str(), 0700) != 0) {
            throw std::runtime_error("cannot protect clipboard directory");
        }
        auto pattern = (directory / ".mcpp-clip-XXXXXX").string();
        std::vector<char> buffer(pattern.begin(), pattern.end());
        buffer.push_back('\0');
        int descriptor = mkostemp(buffer.data(), O_CLOEXEC);
        if (descriptor < 0) {
            throw std::runtime_error(
                "cannot create clipboard temporary file: " +
                std::string(std::strerror(errno))
            );
        }
        path_ = buffer.data();
        try {
            if (fchmod(descriptor, 0600) != 0) {
                throw std::runtime_error("cannot protect clipboard file");
            }
            std::size_t offset = 0;
            while (offset < content.size()) {
                const auto count = write(
                    descriptor, content.data() + offset, content.size() - offset
                );
                if (count < 0 && errno == EINTR) continue;
                if (count <= 0) {
                    throw std::runtime_error("cannot write clipboard file");
                }
                offset += static_cast<std::size_t>(count);
            }
            if (fsync(descriptor) != 0) {
                throw std::runtime_error("cannot commit clipboard file");
            }
            const auto closed = close(descriptor);
            descriptor = -1;
            if (closed != 0) {
                throw std::runtime_error("cannot commit clipboard file");
            }
            path_ = std::filesystem::absolute(path_);
        } catch (...) {
            if (descriptor >= 0) static_cast<void>(close(descriptor));
            discard_unchecked();
            throw;
        }
    }
    ~ClipboardFile() { discard_unchecked(); }
    ClipboardFile(const ClipboardFile&) = delete;
    ClipboardFile& operator=(const ClipboardFile&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

    void publish(const std::filesystem::path& destination) {
        std::filesystem::rename(path_, destination);
        path_.clear();
    }

    void discard() {
        std::filesystem::remove(path_);
        path_.clear();
    }

private:
    void discard_unchecked() noexcept {
        if (!path_.empty()) {
            std::error_code ignored;
            std::filesystem::remove(path_, ignored);
        }
    }

    std::filesystem::path path_;
};

void validate_command(
    const std::string& command,
    const std::filesystem::path& working_directory
) {
    if (command.find('\0') != std::string::npos) {
        throw std::invalid_argument("clipboard command cannot contain NUL");
    }
    if (working_directory.empty() ||
        working_directory.native().find('\0') != std::string::npos) {
        throw std::invalid_argument("clipboard command needs a local working directory");
    }
}

CommandOutput clipboard_command(
    const std::vector<std::string>& arguments,
    const std::filesystem::path& working_directory,
    const Cancellation& cancellation,
    const std::size_t limit,
    const std::string_view operation
) {
    cancellation.throw_if_requested();
    Cancellation bounded;
    std::atomic_bool timed_out{false};
    const auto deadline = std::chrono::steady_clock::now() + command_timeout;
    std::jthread watchdog([&](const std::stop_token stop) {
        while (!stop.stop_requested()) {
            if (cancellation.requested()) {
                bounded.request();
                return;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                timed_out.store(true, std::memory_order_relaxed);
                bounded.request();
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });
    CommandOutput result;
    try {
        result = capture_command(arguments, working_directory, bounded, limit);
    } catch (const OperationCancelled&) {
        watchdog.request_stop();
        watchdog.join();
        if (timed_out.load(std::memory_order_relaxed)) {
            throw std::runtime_error(
                "clipboard " + std::string(operation) + " command timed out after 2 seconds"
            );
        }
        throw;
    }
    watchdog.request_stop();
    watchdog.join();
    cancellation.throw_if_requested();
    if (result.status != 0) {
        throw std::runtime_error(
            "clipboard " + std::string(operation) + " command exited with status " +
            std::to_string(result.status) +
            (result.error.empty() ? std::string{} : ": " + result.error.substr(0, 1024))
        );
    }
    return result;
}

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
    ClipboardFile pending(path, serialize_clipboard(content, column));
    pending.publish(path);
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

void save_editor_clipboard(
    const std::filesystem::path& path,
    const std::string_view content,
    const bool column,
    const std::optional<std::string>& command,
    const std::filesystem::path& working_directory,
    const Cancellation& cancellation
) {
    cancellation.throw_if_requested();
    if (!command || command->empty()) {
        save_editor_clipboard(path, content, column);
        return;
    }
    validate_command(*command, working_directory);
    ClipboardFile pending(path, serialize_clipboard(content, column));
    ClipboardFile raw(path, content);
    // Native store commands take one raw-file argument, never serialized metadata.
    static_cast<void>(clipboard_command(
        {"/bin/sh", "-c", *command + " \"$1\"", "mcpp-clipboard-store", raw.path().string()},
        working_directory, cancellation, 64U * 1024U, "store"
    ));
    raw.discard();
    cancellation.throw_if_requested();
    pending.publish(path);
}

std::string load_editor_clipboard(
    const std::filesystem::path& path,
    bool* const column,
    const std::optional<std::string>& command,
    const std::filesystem::path& working_directory,
    const Cancellation& cancellation
) {
    cancellation.throw_if_requested();
    if (!command || command->empty()) {
        return load_editor_clipboard(path, column);
    }
    validate_command(*command, working_directory);
    auto result = clipboard_command(
        {"/bin/sh", "-c", *command},
        working_directory, cancellation, maximum_clipboard_size, "paste"
    );
    if (!result.output.empty()) {
        save_editor_clipboard(path, result.output);
    }
    if (column != nullptr) *column = false;
    return std::move(result.output);
}

}  // namespace mcpp
