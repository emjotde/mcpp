#include "mcpp/local_tools.hpp"
#include "local_tools_stream.hpp"

#include <array>
#include <cerrno>
#include <csignal>
#include <charconv>
#include <cstring>
#include <fcntl.h>
#include <iconv.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <linux/fs.h>
#include <poll.h>
#include <spawn.h>
#include <stdexcept>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <system_error>
#include <unistd.h>

namespace mcpp {
namespace {

struct Pipe {
    std::array<int, 2> descriptors{-1, -1};
    Pipe() {
        if (pipe2(descriptors.data(), O_CLOEXEC) != 0) {
            throw std::system_error(errno, std::generic_category(), "create command pipe");
        }
    }
    ~Pipe() {
        for (const auto descriptor : descriptors) {
            if (descriptor >= 0) close(descriptor);
        }
    }
    void close_end(const std::size_t index) {
        if (descriptors[index] >= 0) {
            close(descriptors[index]);
            descriptors[index] = -1;
        }
    }
    Pipe(const Pipe&) = delete;
    Pipe& operator=(const Pipe&) = delete;
};

struct SpawnSettings {
    posix_spawn_file_actions_t actions{};
    posix_spawnattr_t attributes{};
    SpawnSettings() {
        const auto result = posix_spawn_file_actions_init(&actions);
        if (result != 0) throw std::system_error(result, std::generic_category(), "command actions");
        const auto attr_result = posix_spawnattr_init(&attributes);
        if (attr_result != 0) {
            posix_spawn_file_actions_destroy(&actions);
            throw std::system_error(attr_result, std::generic_category(), "command attributes");
        }
    }
    ~SpawnSettings() {
        posix_spawn_file_actions_destroy(&actions);
        posix_spawnattr_destroy(&attributes);
    }
};

void checked(const int result, const char* operation) {
    if (result != 0) throw std::system_error(result, std::generic_category(), operation);
}

void reap_cancelled(const pid_t child) {
    kill(-child, SIGTERM);
    int status = 0;
    for (int attempt = 0; attempt < 50; ++attempt) {
        const auto result = waitpid(child, &status, WNOHANG);
        if (result == child || (result < 0 && errno == ECHILD)) return;
        usleep(20000);
    }
    kill(-child, SIGKILL);
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
}

int flags_descriptor(const std::filesystem::path& path) {
    const int descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (descriptor < 0) {
        throw std::system_error(errno, std::generic_category(), "open file attributes: " + path.string());
    }
    struct stat state{};
    if (fstat(descriptor, &state) != 0 || (!S_ISREG(state.st_mode) && !S_ISDIR(state.st_mode))) {
        close(descriptor);
        throw std::runtime_error("file attributes require a regular file or directory");
    }
    return descriptor;
}

}  // namespace

const std::vector<Codepage>& supported_codepages() {
    static const std::vector<Codepage> pages{
        {"ASCII", "7-bit ASCII"}, {"ISO-8859-1", "ISO 8859-1"}, {"ISO-8859-2", "ISO 8859-2"},
        {"ISO-8859-5", "ISO 8859-5"}, {"CP1250", "Windows 1250"}, {"CP1251", "Windows 1251"},
        {"CP437", "CP 437"}, {"CP850", "CP 850"}, {"CP852", "CP 852"}, {"CP866", "CP 866"},
        {"KOI8-R", "KOI8-R"}, {"KOI8-U", "KOI8-U"}, {"UTF-8", "UTF-8"}
    };
    return pages;
}

TemporaryFile::TemporaryFile(const std::string_view value) {
    auto pattern = (std::filesystem::temp_directory_path() / "mcpp-local-XXXXXX").string();
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back('\0');
    int descriptor = mkstemp(name.data());
    if (descriptor < 0) throw std::system_error(errno, std::generic_category(), "create local temporary");
    path_ = name.data();
    try {
        std::size_t offset = 0;
        while (offset < value.size()) {
            const auto written = write(descriptor, value.data() + offset, value.size() - offset);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) throw std::system_error(errno, std::generic_category(), "write local temporary");
            offset += static_cast<std::size_t>(written);
        }
        const auto result = close(descriptor);
        descriptor = -1;
        if (result != 0) throw std::system_error(errno, std::generic_category(), "close local temporary");
    } catch (...) {
        if (descriptor >= 0) close(descriptor);
        std::error_code error;
        std::filesystem::remove(path_, error);
        if (error) std::cerr << "mcpp: temporary cleanup failed: " << error.message() << '\n';
        throw;
    }
}

TemporaryFile::~TemporaryFile() {
    std::error_code error;
    std::filesystem::remove(path_, error);
    if (error) std::cerr << "mcpp: temporary cleanup failed: " << error.message() << '\n';
}

namespace {

CommandOutput capture_command_impl(
    const std::vector<std::string>& arguments,
    const std::filesystem::path& directory,
    const Cancellation& cancellation,
    const std::size_t limit,
    const std::optional<std::filesystem::path> input,
    std::ostream* streamed_output
) {
    if (arguments.empty() || arguments.front().empty()) {
        throw std::invalid_argument("command is empty");
    }
    for (const auto& argument : arguments) {
        if (argument.find('\0') != std::string::npos) {
            throw std::invalid_argument("command arguments cannot contain NUL");
        }
    }
    cancellation.throw_if_requested();
    Pipe output;
    Pipe error;
    SpawnSettings settings;
    struct Input {
        int descriptor{-1};
        ~Input() { if (descriptor >= 0) close(descriptor); }
    } source;
    if (input) {
        if (input->native().find('\0') != std::string::npos) throw std::invalid_argument("input path cannot contain NUL");
        source.descriptor = open(input->c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
        if (source.descriptor < 0) throw std::system_error(errno, std::generic_category(), "open command input");
        struct stat status{};
        if (fstat(source.descriptor, &status) != 0) throw std::system_error(errno, std::generic_category(), "stat command input");
        if (!S_ISREG(status.st_mode)) throw std::runtime_error("command input must be a regular file");
        checked(posix_spawn_file_actions_adddup2(&settings.actions, source.descriptor, STDIN_FILENO), "command standard input");
        checked(posix_spawn_file_actions_addclose(&settings.actions, source.descriptor), "close command input descriptor");
    } else {
        checked(posix_spawn_file_actions_addopen(&settings.actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0),
                "command standard input");
    }
    checked(posix_spawn_file_actions_adddup2(&settings.actions, output.descriptors[1], STDOUT_FILENO),
            "command standard output");
    checked(posix_spawn_file_actions_adddup2(&settings.actions, error.descriptors[1], STDERR_FILENO),
            "command standard error");
    for (const auto descriptor : {output.descriptors[0], output.descriptors[1],
                                  error.descriptors[0], error.descriptors[1]}) {
        checked(posix_spawn_file_actions_addclose(&settings.actions, descriptor), "close command descriptor");
    }
    checked(posix_spawn_file_actions_addchdir_np(&settings.actions, directory.c_str()), "command directory");
    checked(posix_spawnattr_setpgroup(&settings.attributes, 0), "command process group");
    checked(posix_spawnattr_setflags(&settings.attributes, POSIX_SPAWN_SETPGROUP), "command process flags");
    auto owned_arguments = arguments;
    std::vector<char*> pointers;
    for (auto& argument : owned_arguments) pointers.push_back(argument.data());
    pointers.push_back(nullptr);
    pid_t child = -1;
    checked(posix_spawnp(&child, pointers.front(), &settings.actions, &settings.attributes,
                        pointers.data(), ::environ), "start local command");
    output.close_end(1);
    error.close_end(1);
    CommandOutput result;
    try {
        while (output.descriptors[0] >= 0 || error.descriptors[0] >= 0) {
            cancellation.throw_if_requested();
            std::array<pollfd, 2> descriptors{{
                {output.descriptors[0], POLLIN, 0}, {error.descriptors[0], POLLIN, 0}
            }};
            const auto ready = poll(descriptors.data(), descriptors.size(), 50);
            if (ready < 0 && errno == EINTR) continue;
            if (ready < 0) throw std::system_error(errno, std::generic_category(), "read command output");
            for (std::size_t index = 0; index < descriptors.size(); ++index) {
                if (descriptors[index].fd < 0 || descriptors[index].revents == 0) continue;
                std::array<char, 8192> bytes{};
                const auto count = read(descriptors[index].fd, bytes.data(), bytes.size());
                if (count < 0 && errno == EINTR) continue;
                if (count < 0) throw std::system_error(errno, std::generic_category(), "read command pipe");
                auto& pipe = index == 0U ? output : error;
                auto& text = index == 0U ? result.output : result.error;
                if (count == 0) {
                    pipe.close_end(0);
                } else if (index == 0U && streamed_output != nullptr) {
                    streamed_output->write(bytes.data(), count);
                    if (!*streamed_output) throw std::runtime_error("cannot write local command output");
                } else {
                    const auto size = static_cast<std::size_t>(count);
                    if (text.size() > limit || size > limit - text.size()) {
                        throw std::runtime_error("local command output exceeds the configured byte limit");
                    }
                    text.append(bytes.data(), size);
                }
            }
        }
        int status = 0;
        for (;;) {
            cancellation.throw_if_requested();
            const auto waited = waitpid(child, &status, WNOHANG);
            if (waited == child) break;
            if (waited < 0 && errno != EINTR) {
                throw std::system_error(errno, std::generic_category(), "wait for command");
            }
            static_cast<void>(poll(nullptr, 0, 50));
        }
        result.status = WIFEXITED(status) ? WEXITSTATUS(status)
                      : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 1;
    } catch (...) {
        reap_cancelled(child);
        throw;
    }
    return result;
}

}  // namespace

CommandOutput capture_command(
    const std::vector<std::string>& arguments,
    const std::filesystem::path& directory,
    const Cancellation& cancellation,
    const std::size_t limit,
    const std::optional<std::filesystem::path> input
) {
    return capture_command_impl(arguments, directory, cancellation, limit, input, nullptr);
}

CommandOutput capture_command_to(
    std::ostream& output,
    const std::vector<std::string>& arguments,
    const std::filesystem::path& directory,
    const Cancellation& cancellation,
    const std::size_t error_limit,
    const std::optional<std::filesystem::path> input
) {
    return capture_command_impl(arguments, directory, cancellation, error_limit, input, &output);
}

CommandOutput capture_shell(
    const std::string_view command, const std::filesystem::path& directory,
    const Cancellation& cancellation, const std::size_t limit
) {
    return capture_command({"/bin/bash", "-c", std::string(command)}, directory, cancellation, limit);
}

std::vector<Declaration> find_declarations(
    const std::filesystem::path& source, const std::string_view word,
    const Cancellation& cancellation
) {
    if (word.empty()) throw std::invalid_argument("there is no identifier at the cursor");
    auto directory = source.parent_path();
    std::filesystem::path index;
    for (;;) {
        cancellation.throw_if_requested();
        const auto candidate = directory / "tags";
        if (std::filesystem::exists(candidate)) {
            if (!std::filesystem::is_regular_file(candidate)) throw std::runtime_error("tags index is not a regular file");
            index = candidate;
            break;
        }
        const auto parent = directory.parent_path();
        if (parent == directory || parent.empty()) break;
        directory = parent;
    }
    if (index.empty()) throw std::runtime_error("no tags index found; generate a ctags tags file for this project");
    std::ifstream input(index);
    if (!input) throw std::runtime_error("cannot read tags index: " + index.string());
    std::vector<Declaration> targets;
    std::string row;
    while (std::getline(input, row)) {
        cancellation.throw_if_requested();
        if (!row.empty() && row.back() == '\r') row.pop_back();
        const auto first = row.find('\t');
        if (first == std::string::npos || std::string_view(row).substr(0, first) != word) continue;
        const auto second = row.find('\t', first + 1U);
        if (second == std::string::npos) throw std::runtime_error("invalid declaration entry in tags index");
        auto path = std::filesystem::path(row.substr(first + 1U, second - first - 1U));
        if (path.is_relative()) path = index.parent_path() / path;
        path = path.lexically_normal();
        if (!std::filesystem::is_regular_file(path)) {
            throw std::runtime_error("declaration target is not a regular file: " + path.string());
        }
        auto address = row.substr(second + 1U);
        const auto fields_begin = address.find(";\"");
        if (fields_begin != std::string::npos) {
            std::istringstream fields(address.substr(fields_begin + 2U));
            std::string field;
            while (fields >> field) {
                if (field.starts_with("line:")) {
                    address = field.substr(5U);
                    break;
                }
            }
            if (!address.empty() && address.front() != '/' && address.front() != '?') {
                address = address.substr(0, address.find_first_of(";\t "));
            } else {
                address = address.substr(0, fields_begin);
            }
        }
        std::size_t line = 0;
        const auto parsed = std::from_chars(address.data(), address.data() + address.size(), line);
        if (parsed.ec != std::errc{} || parsed.ptr != address.data() + address.size()) {
            if (address.size() < 2U ||
                (address.front() != '/' && address.front() != '?') || address.back() != address.front()) {
                throw std::runtime_error("unsupported declaration address in tags index");
            }
            auto pattern = address.substr(1U, address.size() - 2U);
            const bool starts = pattern.starts_with('^');
            const bool ends = pattern.ends_with('$');
            if (starts) pattern.erase(0, 1);
            if (ends) pattern.pop_back();
            std::string literal;
            for (std::size_t offset = 0; offset < pattern.size(); ++offset) {
                if (pattern[offset] == '\\' && offset + 1U < pattern.size()) {
                    const auto next = pattern[++offset];
                    literal += next == 't' ? '\t' : next == 'r' ? '\r' : next;
                } else {
                    literal += pattern[offset];
                }
            }
            std::ifstream file(path);
            if (!file) throw std::runtime_error("cannot read declaration target: " + path.string());
            std::string text;
            bool matched = false;
            while (std::getline(file, text)) {
                cancellation.throw_if_requested();
                ++line;
                if (!text.empty() && text.back() == '\r') text.pop_back();
                const bool match = starts && ends ? text == literal
                                 : starts ? text.starts_with(literal)
                                 : ends ? text.ends_with(literal) : text.find(literal) != std::string::npos;
                if (match) {
                    matched = true;
                    break;
                }
            }
            if (!file && !file.eof()) throw std::runtime_error("cannot read declaration target");
            if (!matched) throw std::runtime_error("declaration address no longer matches the target file");
        } else {
            std::ifstream file(path);
            if (!file) throw std::runtime_error("cannot read declaration target: " + path.string());
            std::string text;
            for (std::size_t current = 0; current < line; ++current) {
                cancellation.throw_if_requested();
                if (!std::getline(file, text)) throw std::runtime_error("declaration line is outside the target file");
            }
        }
        if (line == 0U) throw std::runtime_error("declaration line numbers start at 1");
        targets.push_back({path.string(), line});
    }
    if (input.bad()) throw std::runtime_error("failed while reading tags index");
    if (targets.empty()) throw std::runtime_error("identifier is not present in tags index: " + std::string(word));
    return targets;
}

std::vector<std::pair<std::string, std::string>> read_local_rules(const std::filesystem::path& path) {
    int descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (descriptor < 0) {
        if (errno == ENOENT) return {};
        throw std::system_error(errno, std::generic_category(), "open local rules: " + path.string());
    }
    std::string contents;
    try {
        struct stat state{};
        if (fstat(descriptor, &state) != 0) {
            throw std::system_error(errno, std::generic_category(), "inspect local rules");
        }
        if (!S_ISREG(state.st_mode) || (state.st_uid != geteuid() && state.st_uid != 0U) ||
            (state.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
            throw std::runtime_error("refusing insecure local rules: " + path.string());
        }
        std::array<char, 8192> bytes{};
        for (;;) {
            const auto count = read(descriptor, bytes.data(), bytes.size());
            if (count < 0 && errno == EINTR) continue;
            if (count < 0) throw std::system_error(errno, std::generic_category(), "read local rules");
            if (count == 0) break;
            if (contents.size() + static_cast<std::size_t>(count) > 1024U * 1024U) {
                throw std::runtime_error("local rules file exceeds 1 MiB");
            }
            contents.append(bytes.data(), static_cast<std::size_t>(count));
        }
        const auto closed = close(descriptor);
        descriptor = -1;
        if (closed != 0) throw std::system_error(errno, std::generic_category(), "close local rules");
    } catch (...) {
        if (descriptor >= 0) close(descriptor);
        throw;
    }
    std::vector<std::pair<std::string, std::string>> rules;
    std::istringstream input(contents);
    std::string line;
    const auto trim = [](std::string text) {
        const auto first = text.find_first_not_of(" \t\r");
        if (first == std::string::npos) return std::string{};
        const auto last = text.find_last_not_of(" \t\r");
        return text.substr(first, last - first + 1U);
    };
    while (std::getline(input, line)) {
        line = trim(std::move(line));
        if (line.empty() || line.starts_with('#')) continue;
        const auto equals = line.find('=');
        if (equals == std::string::npos) throw std::runtime_error("local rule needs pattern=value: " + path.string());
        auto key = trim(line.substr(0, equals));
        auto value = trim(line.substr(equals + 1U));
        if (key.empty() || value.empty() || key.find('\0') != std::string::npos ||
            value.find('\0') != std::string::npos) throw std::runtime_error("empty or invalid local rule");
        rules.emplace_back(std::move(key), std::move(value));
    }
    return rules;
}

std::string convert_encoding(
    const std::string_view value, const std::string& source, const std::string& destination
) {
    const auto descriptor = iconv_open(destination.c_str(), source.c_str());
    if (descriptor == reinterpret_cast<iconv_t>(-1)) {
        throw std::system_error(errno, std::generic_category(), "open encoding conversion");
    }
    try {
        std::string input(value);
        std::string result(input.size() + 64U, '\0');
        auto* begin = input.data();
        auto remaining = input.size();
        auto* target = result.data();
        auto available = result.size();
        for (int phase = 0; phase < 2; ++phase) {
            for (;;) {
                const auto converted = phase == 0
                                           ? iconv(descriptor, &begin, &remaining, &target, &available)
                                           : iconv(descriptor, nullptr, nullptr, &target, &available);
                if (converted != static_cast<std::size_t>(-1)) break;
                if (errno != E2BIG) {
                    throw std::system_error(errno, std::generic_category(), "convert text from " + source);
                }
                const auto used = result.size() - available;
                result.resize(result.size() * 2U);
                target = result.data() + used;
                available = result.size() - used;
            }
        }
        result.resize(result.size() - available);
        iconv_close(descriptor);
        return result;
    } catch (...) {
        iconv_close(descriptor);
        throw;
    }
}

unsigned int local_file_flags(const std::filesystem::path& path) {
    const auto descriptor = flags_descriptor(path);
    unsigned int flags = 0;
    const auto result = ioctl(descriptor, FS_IOC_GETFLAGS, &flags);
    const auto error = errno;
    close(descriptor);
    if (result != 0) throw std::system_error(error, std::generic_category(), "read file attributes");
    return flags;
}

void set_local_file_flags(const std::filesystem::path& path, unsigned int flags) {
    const auto descriptor = flags_descriptor(path);
    const auto result = ioctl(descriptor, FS_IOC_SETFLAGS, &flags);
    const auto error = errno;
    close(descriptor);
    if (result != 0) throw std::system_error(error, std::generic_category(), "set file attributes");
}

}  // namespace mcpp
