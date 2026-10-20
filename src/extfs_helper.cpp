#include "extfs_helper.hpp"

#include "mcpp/local_tools.hpp"
#include "mcpp/local_provider.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <map>
#include <sys/random.h>
#include <sys/stat.h>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <utility>

namespace mcpp {
namespace {
namespace fs = std::filesystem;

class Descriptor {
public:
    explicit Descriptor(const int value = -1) noexcept : value_(value) {}
    ~Descriptor() { if (value_ >= 0) static_cast<void>(::close(value_)); }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    [[nodiscard]] int get() const noexcept { return value_; }
private:
    int value_;
};

void check(const int result, const char* operation) {
    if (result < 0) throw std::system_error(errno, std::generic_category(), operation);
}

bool valid_name(const std::string_view name) {
    return !name.empty() && name.size() <= 64U &&
        std::all_of(name.begin(), name.end(), [](const unsigned char byte) {
            return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                   (byte >= '0' && byte <= '9') || byte == '_' || byte == '-';
        });
}

std::vector<fs::path> helper_directories(const ExtfsOpenOptions& options) {
    if (!options.helper_directories.empty()) return options.helper_directories;
    std::vector<fs::path> result;
    if (const auto* data = std::getenv("XDG_DATA_HOME");
        data != nullptr && fs::path(data).is_absolute()) {
        result.emplace_back(fs::path(data) / "mc" / "extfs.d");
    } else if (const auto* home = std::getenv("HOME");
               home != nullptr && fs::path(home).is_absolute()) {
        result.emplace_back(fs::path(home) / ".local" / "share" / "mc" / "extfs.d");
    }
    for (const auto* path : {"/usr/local/libexec/mc/extfs.d", "/usr/local/lib/mc/extfs.d",
                            "/usr/libexec/mc/extfs.d", "/usr/lib/mc/extfs.d"}) {
        result.emplace_back(path);
    }
    return result;
}

bool present(const fs::path& path) {
    std::error_code error;
    const auto state = fs::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory || error == std::errc::not_a_directory) return false;
    if (error) throw fs::filesystem_error("inspect extfs helper", path, error);
    return state.type() != fs::file_type::not_found;
}

bool executable_candidate(const fs::path& path) {
    if (!present(path)) return false;
    std::error_code error;
    const auto state = fs::status(path, error);
    if (error == std::errc::no_such_file_or_directory) return false;
    if (error) throw fs::filesystem_error("inspect extfs executable", path, error);
    return fs::is_regular_file(state) && ::access(path.c_str(), X_OK) == 0;
}

std::string helper_revision(const fs::path& path) {
    struct stat state{};
    check(::lstat(path.c_str(), &state), "inspect extfs executable");
    if (!S_ISREG(state.st_mode) || (state.st_uid != 0U && state.st_uid != ::geteuid()) ||
        (state.st_mode & 0022U) != 0U || ::access(path.c_str(), X_OK) != 0) {
        extfs::fail("helper must be an executable regular file owned by this user or root "
                    "and not writable by others: " + path.string());
    }
    return std::to_string(state.st_dev) + ":" + std::to_string(state.st_ino) + ":" +
        std::to_string(state.st_size) + ":" + std::to_string(state.st_mode) + ":" +
        std::to_string(state.st_mtim.tv_sec) + ":" + std::to_string(state.st_mtim.tv_nsec) + ":" +
        std::to_string(state.st_ctim.tv_sec) + ":" + std::to_string(state.st_ctim.tv_nsec);
}

ExtfsHelper inspect_helper(const fs::path& directory, const std::string& name, const bool source) {
    const auto path = fs::canonical(directory / (name + (source ? "" : "+")));
    const auto parent = fs::canonical(directory);
    struct stat state{};
    check(::stat(parent.c_str(), &state), "inspect extfs helper directory");
    if ((state.st_uid != 0U && state.st_uid != ::geteuid()) || (state.st_mode & 0022U) != 0U) {
        extfs::fail("helper directory must be owned by this user or root and not writable by others: " +
                    directory.string());
    }
    return {name, path, helper_revision(path), source};
}

fs::path storage_base(const ExtfsOpenOptions& options) {
    fs::path base = options.cache_directory;
    if (base.empty()) {
        if (const auto* cache = std::getenv("XDG_CACHE_HOME");
            cache != nullptr && fs::path(cache).is_absolute()) {
            base = fs::path(cache) / "mcpp" / "extfs";
        } else if (const auto* home = std::getenv("HOME");
                   home != nullptr && fs::path(home).is_absolute()) {
            base = fs::path(home) / ".cache" / "mcpp" / "extfs";
        } else {
            extfs::fail("helpers need an absolute HOME or XDG_CACHE_HOME for private storage");
        }
    }
    if (base.native().size() > options.limits.path_bytes ||
        base.native().find('\0') != std::string::npos) extfs::fail("invalid private storage path");
    return fs::absolute(base).lexically_normal();
}

std::string random_name() {
    std::array<unsigned char, 16> bytes{};
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = ::getrandom(bytes.data() + offset, bytes.size() - offset, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) throw std::system_error(errno, std::generic_category(), "name extfs workspace");
        offset += static_cast<std::size_t>(count);
    }
    constexpr char digits[] = "0123456789abcdef";
    std::string result = "extfs-";
    for (const auto byte : bytes) {
        result += digits[byte >> 4U];
        result += digits[byte & 15U];
    }
    return result;
}

void literal_file_name(const std::string_view name) {
    if (name.empty() || name == "." || name == ".." ||
        name.find_first_of("/\0", 0, 2) != std::string_view::npos) {
        extfs::fail("invalid private file name");
    }
}

std::string tool(const std::string_view name) {
    for (const auto* directory : {"/usr/bin", "/bin"}) {
        const auto path = fs::path(directory) / name;
        if (::access(path.c_str(), X_OK) == 0) return path.string();
    }
    extfs::fail("required helper tool is unavailable: " + std::string(name));
}

}  // namespace

std::optional<ExtfsHelper> find_extfs_helper(const std::string_view name, const ExtfsOpenOptions& options) {
    if (!valid_name(name)) extfs::fail("invalid helper name (a trailing '+' is not part of a native VFS name)");
    for (const auto& directory : helper_directories(options)) {
        if (!directory.is_absolute() || directory.native().find('\0') != std::string::npos) {
            extfs::fail("helper directories must be absolute");
        }
        const auto plain = directory / name;
        const auto no_source = directory / (std::string(name) + "+");
        const bool has_plain = executable_candidate(plain);
        const bool has_no_source = executable_candidate(no_source);
        if (has_plain && has_no_source) extfs::fail("ambiguous helpers with and without '+': " + plain.string());
        if (has_plain || has_no_source) {
            return inspect_helper(directory, std::string(name), has_plain);
        }
    }
    return {};
}

std::vector<ExtfsHelper> discover_extfs_helpers(const ExtfsOpenOptions& options) {
    std::map<std::string, ExtfsHelper> found;
    for (const auto& directory : helper_directories(options)) {
        if (!directory.is_absolute() || directory.native().find('\0') != std::string::npos) {
            extfs::fail("helper directories must be absolute");
        }
        if (!present(directory)) continue;
        for (const auto& entry : fs::directory_iterator(directory)) {
            auto name = entry.path().filename().string();
            if (name.ends_with('+')) name.pop_back();
            if (!valid_name(name) || found.contains(name)) continue;
            if (const auto helper = find_extfs_helper(name, options)) found.emplace(name, *helper);
        }
    }
    std::vector<ExtfsHelper> result;
    for (auto& [name, helper] : found) {
        static_cast<void>(name);
        result.push_back(std::move(helper));
    }
    return result;
}

namespace extfs {

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error("extfs: " + message);
}

void validate_limits(const ExtfsLimits& limits) {
    constexpr std::size_t maximum = 512U * 1024U * 1024U;
    if (limits.container_bytes == 0U || limits.container_bytes > maximum ||
        limits.member_bytes == 0U || limits.member_bytes > maximum ||
        limits.cached_member_bytes == 0U || limits.cached_member_bytes > maximum ||
        limits.listing_bytes == 0U || limits.listing_bytes > 64U * 1024U * 1024U ||
        limits.members == 0U || limits.members > 1000000U ||
        limits.path_bytes == 0U || limits.path_bytes > 65536U ||
        limits.memory_bytes < 16U * 1024U * 1024U ||
        limits.memory_bytes > 8ULL * 1024ULL * 1024ULL * 1024ULL ||
        limits.cpu_seconds == 0U || limits.cpu_seconds > 300U ||
        limits.elapsed.count() <= 0 || limits.elapsed > std::chrono::minutes(10)) {
        fail("invalid helper limits");
    }
}

void validate_helper(const ExtfsHelper& helper) {
    if (helper_revision(helper.executable) != helper.revision) {
        fail("helper changed since this filesystem was opened; reopen it: " + helper.executable.string());
    }
}

std::string identity_part(const std::string_view value) {
    return std::to_string(value.size()) + ":" + std::string(value);
}

DirectoryContext::DirectoryContext(ExtfsDirectory directory, const Cancellation& cancellation)
    : directory_(std::move(directory)) {
    const auto* local = dynamic_cast<const LocalProvider*>(directory_.provider.get());
    if (local == nullptr) fail("source-less helper origins must be local directories");
    if (directory_.path.empty() || directory_.path.size() > 65536U ||
        directory_.path.find('\0') != std::string::npos || directory_.location.empty() ||
        directory_.location.size() > 65536U || directory_.location.find('\0') != std::string::npos) {
        fail("invalid source-less directory origin");
    }
    directory_.path = local->normalize(directory_.path);
    if (local->stat_follow(directory_.path, cancellation).kind != EntryKind::directory) {
        fail("source-less helper origin is not a directory");
    }
    native_ = local->native_path(directory_.path);
    descriptor_ = ::open(native_.c_str(), O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    check(descriptor_, "open source-less helper directory");
    try {
        struct stat state{};
        check(::fstat(descriptor_, &state), "inspect source-less helper directory");
        device_ = static_cast<std::uintmax_t>(state.st_dev);
        inode_ = static_cast<std::uintmax_t>(state.st_ino);
        verify(cancellation);
    } catch (...) {
        static_cast<void>(::close(descriptor_));
        descriptor_ = -1;
        throw;
    }
}

DirectoryContext::~DirectoryContext() {
    if (descriptor_ >= 0) static_cast<void>(::close(descriptor_));
}

fs::path DirectoryContext::working_path() const {
    return fs::path("/proc") / std::to_string(::getpid()) / "fd" / std::to_string(descriptor_);
}

std::string DirectoryContext::identity() const {
    return std::to_string(device_) + ":" + std::to_string(inode_);
}

void DirectoryContext::verify(const Cancellation& cancellation) const {
    cancellation.throw_if_requested();
    const auto* local = dynamic_cast<const LocalProvider*>(directory_.provider.get());
    const auto current = local->native_path(directory_.path);
    Descriptor descriptor(::open(current.c_str(), O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    check(descriptor.get(), "reopen source-less helper directory");
    struct stat state{};
    check(::fstat(descriptor.get(), &state), "recheck source-less helper directory");
    if (current != native_ || static_cast<std::uintmax_t>(state.st_dev) != device_ ||
        static_cast<std::uintmax_t>(state.st_ino) != inode_) {
        fail("source-less directory origin changed; reopen the filesystem");
    }
}

Workspace::Workspace(const ExtfsOpenOptions& options)
    : base_path_(storage_base(options)), name_(random_name()) {
    fs::create_directories(base_path_.parent_path());
    if (::mkdir(base_path_.c_str(), 0700) < 0 && errno != EEXIST) {
        throw std::system_error(errno, std::generic_category(), "create extfs private storage");
    }
    base_ = ::open(base_path_.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    check(base_, "open extfs private storage");
    try {
        struct stat state{};
        check(::fstat(base_, &state), "inspect extfs private storage");
        if (state.st_uid != ::geteuid() || (state.st_mode & 0077U) != 0U) {
            fail("private storage must be owned by this user and private (0700)");
        }
        check(::mkdirat(base_, name_.c_str(), 0700), "create extfs workspace");
        directory_ = ::openat(base_, name_.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (directory_ < 0) {
            const auto error = errno;
            static_cast<void>(::unlinkat(base_, name_.c_str(), AT_REMOVEDIR));
            throw std::system_error(error, std::generic_category(), "open extfs workspace");
        }
    } catch (...) {
        static_cast<void>(::close(base_));
        base_ = -1;
        throw;
    }
}

Workspace::~Workspace() {
    std::error_code error;
    fs::directory_iterator entries(path(), error);
    const fs::directory_iterator end;
    while (!error && entries != end) {
        fs::remove_all(entries->path(), error);
        if (!error) entries.increment(error);
    }
    if (!error && ::unlinkat(base_, name_.c_str(), AT_REMOVEDIR) < 0) {
        error = std::error_code(errno, std::generic_category());
    }
    if (directory_ >= 0) static_cast<void>(::close(directory_));
    if (base_ >= 0) static_cast<void>(::close(base_));
    if (error) std::cerr << "mcpp: extfs workspace cleanup: " << error.message() << '\n';
}

fs::path Workspace::path() const {
    return fs::path("/proc") / std::to_string(::getpid()) / "fd" / std::to_string(directory_);
}

void Workspace::put(
    const std::string_view name, const std::string_view bytes, const Cancellation& cancellation
) const {
    literal_file_name(name);
    const std::string leaf(name);
    Descriptor output(::openat(directory_, leaf.c_str(),
        O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    check(output.get(), "create private extfs file");
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        cancellation.throw_if_requested();
        const auto count = ::write(output.get(), bytes.data() + offset,
            std::min<std::size_t>(bytes.size() - offset, 65536U));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) throw std::system_error(errno, std::generic_category(), "write private extfs file");
        offset += static_cast<std::size_t>(count);
    }
}

std::string Workspace::get(
    const std::string_view name, const std::size_t limit, const Cancellation& cancellation
) const {
    literal_file_name(name);
    const std::string leaf(name);
    Descriptor input(::openat(directory_, leaf.c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
    check(input.get(), "open private extfs result");
    struct stat before{};
    check(::fstat(input.get(), &before), "inspect private extfs result");
    if (!S_ISREG(before.st_mode) || before.st_uid != ::geteuid() || before.st_nlink != 1U ||
        (before.st_mode & 0077U) != 0U || before.st_size < 0) {
        fail("helper result is not a private, singly linked regular file");
    }
    if (static_cast<std::uint64_t>(before.st_size) > limit) fail("helper result exceeds its byte limit");
    std::string bytes;
    std::array<char, 65536> buffer{};
    for (;;) {
        cancellation.throw_if_requested();
        const auto count = ::read(input.get(), buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) throw std::system_error(errno, std::generic_category(), "read private extfs result");
        if (count == 0) break;
        const auto size = static_cast<std::size_t>(count);
        if (size > limit - bytes.size()) fail("helper result exceeds its byte limit");
        bytes.append(buffer.data(), size);
    }
    struct stat after{};
    check(::fstat(input.get(), &after), "recheck private extfs result");
    if (bytes.size() != static_cast<std::uint64_t>(before.st_size) || before.st_size != after.st_size ||
        before.st_mtim.tv_sec != after.st_mtim.tv_sec || before.st_mtim.tv_nsec != after.st_mtim.tv_nsec ||
        before.st_ctim.tv_sec != after.st_ctim.tv_sec || before.st_ctim.tv_nsec != after.st_ctim.tv_nsec) {
        fail("helper result changed while being read");
    }
    return bytes;
}

std::string invoke(
    const ExtfsHelper& helper, const Workspace& workspace,
    const std::string_view command, const std::vector<std::string>& arguments,
    const ExtfsLimits& limits, const Cancellation& cancellation,
    const fs::path& working_directory
) {
    validate_helper(helper);
    const auto file_limit = std::max({limits.container_bytes, limits.member_bytes, limits.listing_bytes});
    std::vector<std::string> argv{
        tool("prlimit"), "--fsize=" + std::to_string(file_limit),
        "--as=" + std::to_string(limits.memory_bytes),
        "--cpu=" + std::to_string(limits.cpu_seconds), "--core=0", "--",
        "/bin/sh", "-c", "umask 077; exec \"$@\"", "mcpp-extfs",
        tool("env"), "LC_ALL=C", "LANG=C", "PYTHONDONTWRITEBYTECODE=1",
        "TMPDIR=" + workspace.path().string(),
        "MC_EXTFS_DIR=" + helper.executable.parent_path().string(),
        helper.executable.string(), std::string(command)
    };
    argv.insert(argv.end(), arguments.begin(), arguments.end());
    Cancellation stopped;
    std::atomic_bool expired{false};
    std::jthread timer([&](const std::stop_token stop) {
        const auto deadline = std::chrono::steady_clock::now() + limits.elapsed;
        while (!stop.stop_requested()) {
            if (cancellation.requested()) { stopped.request(); return; }
            if (std::chrono::steady_clock::now() >= deadline) {
                expired.store(true, std::memory_order_release);
                stopped.request();
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });
    try {
        const auto result = capture_command(argv, working_directory.empty() ? workspace.path() : working_directory, stopped,
            command == "list" ? limits.listing_bytes : 65536U);
        timer.request_stop();
        cancellation.throw_if_requested();
        if (expired.load(std::memory_order_acquire)) fail("helper exceeded its elapsed-time limit");
        validate_helper(helper);
        if (result.status != 0) {
            fail(helper.name + " " + std::string(command) + " failed or is unsupported (" +
                 std::to_string(result.status) + "): " + result.error);
        }
        return result.output;
    } catch (const OperationCancelled&) {
        timer.request_stop();
        cancellation.throw_if_requested();
        if (expired.load(std::memory_order_acquire)) fail("helper exceeded its elapsed-time limit");
        throw;
    }
}

}  // namespace extfs
}  // namespace mcpp
