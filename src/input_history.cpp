#include "mcpp/input_history.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace mcpp {
namespace {

constexpr std::string_view signature = "mcpp-input-history-v1\n";
constexpr std::size_t purpose_limit = 128U;
constexpr std::size_t value_limit = 16U * 1024U;
constexpr std::size_t file_limit = 8U * 1024U * 1024U;
using Histories = std::map<std::string, std::vector<std::string>, std::less<>>;

class Descriptor {
public:
    explicit Descriptor(const int value = -1) : value_(value) {}
    ~Descriptor() { if (value_ >= 0) static_cast<void>(::close(value_)); }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    Descriptor(Descriptor&& other) noexcept : value_(std::exchange(other.value_, -1)) {}
    Descriptor& operator=(Descriptor&& other) noexcept {
        if (value_ >= 0) static_cast<void>(::close(value_));
        value_ = std::exchange(other.value_, -1);
        return *this;
    }
    [[nodiscard]] int get() const { return value_; }
    void close() {
        if (::close(std::exchange(value_, -1)) != 0) {
            throw std::system_error(errno, std::generic_category(), "close input history");
        }
    }
private:
    int value_;
};

[[noreturn]] void system_error(const std::string& operation) {
    throw std::system_error(errno, std::generic_category(), operation + " input history");
}

void check_id(const std::string_view id) {
    if (id.empty() || id.size() > 96U ||
        !std::ranges::all_of(id, [](const char character) {
            return (character >= 'a' && character <= 'z') ||
                   (character >= '0' && character <= '9') ||
                   character == '.' || character == '-' || character == '_';
        })) {
        throw std::runtime_error("invalid input history name");
    }
}

void check_value(const std::string_view value) {
    if (value.empty() || value.size() > value_limit || value.find('\0') != std::string_view::npos) {
        throw std::runtime_error("invalid input history value (maximum 16384 bytes, no NUL)");
    }
}

void check_file(const struct stat& status, const bool writable) {
    if (!S_ISREG(status.st_mode) || status.st_uid != ::geteuid() ||
        status.st_nlink != 1 || (status.st_mode & 07077) != 0) {
        throw std::runtime_error("input history must be an owner-private, owned regular file with one link");
    }
    if ((status.st_mode & S_IRUSR) == 0 || (writable && (status.st_mode & S_IWUSR) == 0)) {
        throw std::runtime_error(writable ? "input history is not owner-readable and writable"
                                          : "input history is not owner-readable");
    }
}

Descriptor open_directory(const std::filesystem::path& path, const bool create) {
    Descriptor current(::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (current.get() < 0) system_error("open root directory for");
    for (const auto& component : path.relative_path()) {
        const auto name = component.string();
        int next = ::openat(current.get(), name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0 && errno == ENOENT) {
            if (!create) return Descriptor{};
            if (::mkdirat(current.get(), name.c_str(), 0700) != 0 && errno != EEXIST) {
                system_error("create directory for");
            }
            next = ::openat(current.get(), name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        }
        if (next < 0) system_error("open directory for");
        current = Descriptor(next);
    }
    struct stat status{};
    if (::fstat(current.get(), &status) != 0) system_error("stat directory for");
    if (status.st_uid != ::geteuid() || (status.st_mode & 0022) != 0) {
        throw std::runtime_error("input history directory must be owned and not group/other writable");
    }
    return current;
}

struct Loaded {
    Histories histories;
    std::optional<struct stat> revision;
};

Loaded read_history(const int directory, const std::string& name, const bool writable) {
    if (directory < 0) return {};
    Descriptor file(::openat(directory, name.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
    if (file.get() < 0) {
        if (errno == ENOENT) return {};
        system_error("open");
    }
    struct stat status{};
    if (::fstat(file.get(), &status) != 0) system_error("stat");
    check_file(status, writable);
    if (status.st_size < 0 || static_cast<std::uintmax_t>(status.st_size) > file_limit) {
        throw std::runtime_error("input history exceeds the 8 MiB size limit");
    }
    std::string content;
    std::array<char, 8192> buffer{};
    for (;;) {
        const auto count = ::read(file.get(), buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) system_error("read");
        if (count == 0) break;
        content.append(buffer.data(), static_cast<std::size_t>(count));
        if (content.size() > file_limit) throw std::runtime_error("input history exceeds the 8 MiB size limit");
    }
    if (!content.starts_with(signature)) throw std::runtime_error("invalid input history header");
    Loaded result{{}, status};
    const auto digit = [](const char character) -> unsigned int {
        if (character >= '0' && character <= '9') return static_cast<unsigned int>(character - '0');
        if (character >= 'a' && character <= 'f') return static_cast<unsigned int>(character - 'a') + 10U;
        throw std::runtime_error("invalid hexadecimal input history value");
    };
    for (auto offset = signature.size(); offset < content.size();) {
        const auto end = content.find('\n', offset);
        const auto tab = content.find('\t', offset);
        if (end == std::string::npos || tab == std::string::npos || tab >= end ||
            (end - tab - 1U) % 2U != 0U || end - tab - 1U > 2U * value_limit) {
            throw std::runtime_error("invalid input history record");
        }
        const auto id = content.substr(offset, tab - offset);
        check_id(id);
        std::string value;
        for (auto index = tab + 1U; index < end; index += 2U) {
            value += static_cast<char>((digit(content[index]) << 4U) | digit(content[index + 1U]));
        }
        check_value(value);
        auto& history = result.histories[id];
        if (result.histories.size() > purpose_limit) throw std::runtime_error("input history has too many names");
        history.push_back(std::move(value));
        offset = end + 1U;
    }
    return result;
}

std::string serialize(const Histories& histories) {
    constexpr char hex[] = "0123456789abcdef";
    std::string content(signature);
    for (const auto& [id, values] : histories) {
        for (const auto& value : values) {
            content += id;
            content += '\t';
            for (const auto character : value) {
                const auto byte = static_cast<unsigned char>(character);
                content += hex[byte >> 4U];
                content += hex[byte & 15U];
            }
            content += '\n';
            if (content.size() > file_limit) throw std::runtime_error("input history exceeds the 8 MiB size limit");
        }
    }
    return content;
}

void trim_histories(Histories& histories, const std::size_t limit) {
    for (auto item = histories.begin(); item != histories.end();) {
        auto& values = item->second;
        if (values.size() > limit) {
            values.erase(values.begin(),
                values.begin() + static_cast<std::ptrdiff_t>(values.size() - limit));
        }
        if (values.empty()) item = histories.erase(item);
        else ++item;
    }
}

void check_revision(const int directory, const std::string& name, const std::optional<struct stat>& expected) {
    struct stat status{};
    if (::fstatat(directory, name.c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT && !expected) return;
        if (errno != ENOENT) system_error("check");
    } else if (expected && status.st_dev == expected->st_dev && status.st_ino == expected->st_ino &&
               status.st_size == expected->st_size && status.st_mode == expected->st_mode &&
               status.st_uid == expected->st_uid && status.st_nlink == expected->st_nlink &&
               status.st_mtim.tv_sec == expected->st_mtim.tv_sec &&
               status.st_mtim.tv_nsec == expected->st_mtim.tv_nsec &&
               status.st_ctim.tv_sec == expected->st_ctim.tv_sec &&
               status.st_ctim.tv_nsec == expected->st_ctim.tv_nsec) {
        return;
    }
    throw std::runtime_error("input history changed while it was being saved; retry without discarding the input");
}

void write_history(const int directory, const std::string& name, const Loaded& loaded) {
    const auto content = serialize(loaded.histories);
    static std::atomic_uint64_t sequence{0};
    std::string staging;
    Descriptor output;
    for (unsigned int attempt = 0; attempt < 128U; ++attempt) {
        staging = "." + name + "." + std::to_string(::getpid()) + "." + std::to_string(++sequence);
        output = Descriptor(::openat(directory, staging.c_str(),
            O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
        if (output.get() >= 0) break;
        if (errno != EEXIST) system_error("create staging file for");
    }
    if (output.get() < 0) throw std::runtime_error("cannot create a unique input history staging file");
    try {
        if (::fchmod(output.get(), 0600) != 0) system_error("protect");
        for (std::size_t offset = 0; offset < content.size();) {
            const auto count = ::write(output.get(), content.data() + offset, content.size() - offset);
            if (count < 0 && errno == EINTR) continue;
            if (count < 0) system_error("write");
            if (count == 0) throw std::runtime_error("short write to input history");
            offset += static_cast<std::size_t>(count);
        }
        if (::fsync(output.get()) != 0) system_error("sync");
        output.close();
        check_revision(directory, name, loaded.revision);
        if (::renameat(directory, staging.c_str(), directory, name.c_str()) != 0) system_error("publish");
        if (::fsync(directory) != 0) system_error("sync directory for");
    } catch (...) {
        static_cast<void>(::unlinkat(directory, staging.c_str(), 0));
        throw;
    }
}

}  // namespace

InputHistories::InputHistories(std::filesystem::path path, const std::size_t limit)
    : path_(std::filesystem::absolute(std::move(path)).lexically_normal()), limit_(limit) {}

void InputHistories::load() {
    if (limit_ == 0U) {
        histories_.clear();
        return;
    }
    const auto directory = open_directory(path_.parent_path(), false);
    auto loaded = read_history(directory.get(), path_.filename().string(), false);
    trim_histories(loaded.histories, limit_);
    histories_ = std::move(loaded.histories);
}

const std::vector<std::string>& InputHistories::history(const std::string_view id) const {
    static const std::vector<std::string> empty;
    const auto found = histories_.find(id);
    return found == histories_.end() ? empty : found->second;
}

void InputHistories::remember(const std::string_view id, const std::string_view value) {
    if (limit_ != 0U && !value.empty()) update(id, value, Change::remember);
}

void InputHistories::remove(const std::string_view id, const std::string_view value) {
    update(id, value, Change::remove);
}

void InputHistories::clear(const std::string_view id) {
    update(id, {}, Change::clear);
}

void InputHistories::update(const std::string_view id, const std::string_view value, const Change change) {
    check_id(id);
    if (change == Change::remember) check_value(value);
    const auto directory = open_directory(path_.parent_path(), true);
    const auto name = path_.filename().string();
    const auto lock_name = "." + name + ".lock";
    Descriptor lock(::openat(directory.get(), lock_name.c_str(),
        O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600));
    if (lock.get() < 0) system_error("open lock for");
    struct stat lock_status{};
    if (::fstat(lock.get(), &lock_status) != 0) system_error("stat lock for");
    check_file(lock_status, true);
    while (::flock(lock.get(), LOCK_EX) != 0) {
        if (errno != EINTR) system_error("lock");
    }
    auto loaded = read_history(directory.get(), name, true);
    const auto before = loaded.histories;
    if (change == Change::clear) {
        loaded.histories.erase(std::string(id));
    } else {
        auto& values = loaded.histories[std::string(id)];
        std::erase(values, value);
        if (change == Change::remember) {
            values.emplace_back(value);
        }
        if (values.empty()) loaded.histories.erase(std::string(id));
    }
    if (change == Change::remember) trim_histories(loaded.histories, limit_);
    if (loaded.histories.size() > purpose_limit) throw std::runtime_error("input history has too many names");
    if (loaded.histories != before) write_history(directory.get(), name, loaded);
    histories_ = std::move(loaded.histories);
    trim_histories(histories_, limit_);
}

}  // namespace mcpp
