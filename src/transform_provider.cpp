#include "mcpp/transform_provider.hpp"

#include "mcpp/local_tools.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <sys/random.h>
#include <sys/stat.h>
#include <system_error>
#include <thread>
#include <tuple>
#include <unistd.h>
#include <utility>

namespace mcpp {
namespace {
namespace fs = std::filesystem;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error("sfs: " + message);
}

[[noreturn]] void readonly() {
    fail("transformed files are read-only; copy to a local file before editing");
}

[[noreturn]] void path_error(const std::string_view path, const std::errc error) {
    throw fs::filesystem_error("SFS file", fs::path(path), std::make_error_code(error));
}

class Descriptor {
public:
    explicit Descriptor(const int value = -1) noexcept : value_(value) {}
    ~Descriptor() { if (value_ >= 0) static_cast<void>(::close(value_)); }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    Descriptor(Descriptor&& other) noexcept : value_(std::exchange(other.value_, -1)) {}
    Descriptor& operator=(Descriptor&& other) noexcept {
        if (this != &other) {
            if (value_ >= 0) static_cast<void>(::close(value_));
            value_ = std::exchange(other.value_, -1);
        }
        return *this;
    }
    [[nodiscard]] int get() const noexcept { return value_; }
private:
    int value_;
};

void check(const int result, const char* operation) {
    if (result < 0) throw std::system_error(errno, std::generic_category(), operation);
}

void validate_limits(const TransformLimits& limits) {
    constexpr std::size_t maximum_bytes = 512U * 1024U * 1024U;
    if (limits.input_bytes == 0U || limits.input_bytes > maximum_bytes ||
        limits.output_bytes == 0U || limits.output_bytes > maximum_bytes ||
        limits.registry_bytes == 0U || limits.registry_bytes > 4U * 1024U * 1024U ||
        limits.rules == 0U || limits.rules > 1024U ||
        limits.path_bytes == 0U || limits.path_bytes > 65536U ||
        limits.memory_bytes < 16U * 1024U * 1024U ||
        limits.memory_bytes > 8ULL * 1024ULL * 1024ULL * 1024ULL ||
        limits.cpu_seconds == 0U || limits.cpu_seconds > 300U ||
        limits.elapsed.count() <= 0 || limits.elapsed > std::chrono::minutes(10)) {
        fail("invalid conversion limits");
    }
}

bool valid_name(const std::string_view name) {
    return !name.empty() && name.size() <= 64U &&
        std::all_of(name.begin(), name.end(), [](const unsigned char byte) {
            return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                   (byte >= '0' && byte <= '9') || byte == '_' || byte == '-';
        });
}

void validate_path(const std::string_view path, const TransformLimits& limits, const char* kind) {
    if (path.empty() || path.size() > limits.path_bytes || path.find('\0') != std::string_view::npos) {
        fail(std::string("invalid ") + kind);
    }
}

std::string identity_part(const std::string_view value) {
    return std::to_string(value.size()) + ":" + std::string(value);
}

fs::path storage_base(const TransformOpenOptions& options) {
    fs::path base = options.cache_directory;
    if (base.empty()) {
        if (const auto* cache = std::getenv("XDG_CACHE_HOME");
            cache != nullptr && fs::path(cache).is_absolute()) {
            base = fs::path(cache) / "mcpp" / "sfs";
        } else if (const auto* home = std::getenv("HOME");
                   home != nullptr && fs::path(home).is_absolute()) {
            base = fs::path(home) / ".cache" / "mcpp" / "sfs";
        } else {
            fail("conversion needs an absolute HOME or XDG_CACHE_HOME for private storage");
        }
    }
    validate_path(base.native(), options.limits, "private storage path");
    return fs::absolute(base).lexically_normal();
}

std::string random_name() {
    std::array<unsigned char, 16> bytes{};
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = ::getrandom(bytes.data() + offset, bytes.size() - offset, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) throw std::system_error(errno, std::generic_category(), "name SFS workspace");
        offset += static_cast<std::size_t>(count);
    }
    constexpr char digits[] = "0123456789abcdef";
    std::string result = "sfs-";
    for (const auto byte : bytes) {
        result += digits[byte >> 4U];
        result += digits[byte & 15U];
    }
    return result;
}

class Workspace {
public:
    explicit Workspace(const TransformOpenOptions& options)
        : base_path_(storage_base(options)), base_(open_base(base_path_)), name_(random_name()),
          directory_(create_directory(base_.get(), name_.c_str())) {
        try {
            input_ = Descriptor(create_directory(directory_.get(), "input"));
        } catch (...) {
            static_cast<void>(::unlinkat(base_.get(), name_.c_str(), AT_REMOVEDIR));
            throw;
        }
    }
    ~Workspace() {
        std::error_code error;
        fs::directory_iterator entries(anchored(), error);
        const fs::directory_iterator end;
        while (!error && entries != end) {
            fs::remove_all(entries->path(), error);
            if (!error) entries.increment(error);
        }
        if (!error && ::unlinkat(base_.get(), name_.c_str(), AT_REMOVEDIR) < 0) {
            error = std::error_code(errno, std::generic_category());
        }
        if (error) std::cerr << "mcpp: SFS workspace cleanup: " << error.message() << '\n';
    }
    Workspace(const Workspace&) = delete;
    Workspace& operator=(const Workspace&) = delete;

    [[nodiscard]] fs::path anchored() const {
        return fs::path("/proc") / std::to_string(::getpid()) / "fd" /
               std::to_string(directory_.get());
    }

    [[nodiscard]] fs::path snapshot(
        const std::string_view basename, const std::string_view bytes,
        const Cancellation& cancellation
    ) const {
        if (basename.empty() || basename == "." || basename == ".." ||
            basename.find_first_of("/\0", 0, 2) != std::string_view::npos) {
            fail("invalid source basename");
        }
        const std::string name(basename);
        Descriptor output(::openat(input_.get(), name.c_str(),
            O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
        check(output.get(), "create SFS input snapshot");
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            cancellation.throw_if_requested();
            const auto count = ::write(output.get(), bytes.data() + offset,
                std::min<std::size_t>(bytes.size() - offset, 65536U));
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) throw std::system_error(errno, std::generic_category(), "write SFS input");
            offset += static_cast<std::size_t>(count);
        }
        check(::fchmod(output.get(), 0400), "protect SFS input snapshot");
        return anchored() / "input" / name;
    }

    void create_output() const {
        Descriptor output(::openat(directory_.get(), "output",
            O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
        check(output.get(), "pre-create private SFS output");
    }

    [[nodiscard]] std::string read_output(
        const std::size_t limit, const Cancellation& cancellation
    ) const {
        Descriptor input(::openat(directory_.get(), "output",
            O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
        check(input.get(), "open SFS output");
        struct stat before{};
        check(::fstat(input.get(), &before), "inspect SFS output");
        if (!S_ISREG(before.st_mode) || before.st_uid != ::geteuid() ||
            before.st_nlink != 1 || (before.st_mode & 0077U) != 0U || before.st_size < 0) {
            fail("helper output is not a private regular file");
        }
        if (static_cast<std::uint64_t>(before.st_size) > limit) fail("helper output exceeds its byte limit");
        std::string bytes;
        std::array<char, 65536> buffer{};
        for (;;) {
            cancellation.throw_if_requested();
            const auto count = ::read(input.get(), buffer.data(), buffer.size());
            if (count < 0 && errno == EINTR) continue;
            if (count < 0) throw std::system_error(errno, std::generic_category(), "read SFS output");
            if (count == 0) break;
            const auto size = static_cast<std::size_t>(count);
            if (size > limit - bytes.size()) fail("helper output exceeds its byte limit");
            bytes.append(buffer.data(), size);
        }
        struct stat after{};
        check(::fstat(input.get(), &after), "recheck SFS output");
        if (bytes.size() != static_cast<std::uint64_t>(before.st_size) ||
            before.st_size != after.st_size ||
            before.st_mtim.tv_sec != after.st_mtim.tv_sec ||
            before.st_mtim.tv_nsec != after.st_mtim.tv_nsec ||
            before.st_ctim.tv_sec != after.st_ctim.tv_sec ||
            before.st_ctim.tv_nsec != after.st_ctim.tv_nsec) {
            fail("helper output changed while it was being read");
        }
        return bytes;
    }

private:
    static int open_base(const fs::path& base) {
        fs::create_directories(base.parent_path());
        if (::mkdir(base.c_str(), 0700) < 0 && errno != EEXIST) {
            throw std::system_error(errno, std::generic_category(), "create SFS storage directory");
        }
        const int descriptor = ::open(base.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        check(descriptor, "open private SFS storage directory");
        struct stat state{};
        if (::fstat(descriptor, &state) < 0 ||
            state.st_uid != ::geteuid() || (state.st_mode & 0077U) != 0U) {
            static_cast<void>(::close(descriptor));
            fail("SFS storage directory must be owned by this user and private (0700)");
        }
        return descriptor;
    }

    static int create_directory(const int parent, const char* name) {
        check(::mkdirat(parent, name, 0700), "create private SFS workspace");
        const int descriptor = ::openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (descriptor < 0) {
            const auto error = errno;
            static_cast<void>(::unlinkat(parent, name, AT_REMOVEDIR));
            throw std::system_error(error, std::generic_category(), "open private SFS workspace");
        }
        return descriptor;
    }

    fs::path base_path_;
    Descriptor base_;
    std::string name_;
    Descriptor directory_;
    Descriptor input_;
};

struct Rule {
    std::string name;
    std::string command;
    std::string origin;
    std::string tool;
    bool decode{false};
    bool local{true};

    [[nodiscard]] std::string identity() const {
        return identity_part(name) + identity_part(command) + identity_part(origin) +
            identity_part(tool) + (decode ? "d" : "c") + (local ? "l" : "n");
    }
};

std::optional<Rule> standard_rule(const std::string_view name) {
    constexpr std::array<std::pair<std::string_view, std::string_view>, 8> formats{{
        {"gz", "gzip"}, {"bz", "bzip"}, {"bz2", "bzip2"}, {"lz", "lzip"},
        {"lz4", "lz4"}, {"lzma", "lzma"}, {"xz", "xz"}, {"zst", "zstd"}
    }};
    for (const auto& [suffix, tool] : formats) {
        const bool decode = name.size() == suffix.size() + 1U && name.front() == 'u' &&
                            name.substr(1) == suffix;
        if (name == suffix || decode) {
            return Rule{std::string(name), {}, "built-in compression", std::string(tool), decode, true};
        }
    }
    return {};
}

bool present(const fs::path& path) {
    std::error_code error;
    const auto status = fs::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory) return false;
    if (error) throw fs::filesystem_error("inspect SFS registry", path, error);
    return status.type() != fs::file_type::not_found;
}

std::optional<fs::path> registry_path(const TransformOpenOptions& options) {
    if (options.registry_file) return options.registry_file;
    if (const auto* directory = std::getenv("MC_DATADIR"); directory != nullptr && *directory != '\0') {
        const fs::path path(directory);
        if (!path.is_absolute()) fail("MC_DATADIR must be absolute");
        return path / "sfs.ini";
    }
    for (const auto* path : {"/etc/mcpp/sfs.ini", "/etc/mc/sfs.ini"}) {
        if (present(path)) return fs::path(path);
    }
    return {};
}

std::string read_registry(
    const fs::path& path, const TransformLimits& limits, const Cancellation& cancellation
) {
    validate_path(path.native(), limits, "SFS registry path");
    Descriptor input(::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
    check(input.get(), "open SFS registry");
    struct stat before{};
    check(::fstat(input.get(), &before), "inspect SFS registry");
    if (!S_ISREG(before.st_mode) || before.st_size < 0 ||
        (before.st_uid != ::geteuid() && before.st_uid != 0U) || (before.st_mode & 0022U) != 0U) {
        fail("SFS registry must be a regular file owned by this user or root and not writable by others");
    }
    if (static_cast<std::uint64_t>(before.st_size) > limits.registry_bytes) fail("SFS registry exceeds its byte limit");
    std::array<char, 4096> buffer{};
    std::string result;
    for (;;) {
        cancellation.throw_if_requested();
        const auto count = ::read(input.get(), buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) throw std::system_error(errno, std::generic_category(), "read SFS registry");
        if (count == 0) break;
        const auto size = static_cast<std::size_t>(count);
        if (size > limits.registry_bytes - result.size()) fail("SFS registry exceeds its byte limit");
        result.append(buffer.data(), size);
    }
    struct stat after{};
    check(::fstat(input.get(), &after), "recheck SFS registry");
    if (result.size() != static_cast<std::uint64_t>(before.st_size) ||
        before.st_size != after.st_size || before.st_mtim.tv_sec != after.st_mtim.tv_sec ||
        before.st_mtim.tv_nsec != after.st_mtim.tv_nsec ||
        before.st_ctim.tv_sec != after.st_ctim.tv_sec || before.st_ctim.tv_nsec != after.st_ctim.tv_nsec) {
        fail("SFS registry changed while it was being read");
    }
    if (result.find('\0') != std::string::npos) fail("NUL in SFS registry");
    return result;
}

std::string_view trim(const std::string_view value) {
    const auto begin = value.find_first_not_of(" \t\r");
    if (begin == std::string_view::npos) return {};
    return value.substr(begin, value.find_last_not_of(" \t\r") - begin + 1U);
}

std::string expand_command(const std::string_view command) {
    enum class Quote { none, single, double_quote };
    Quote quote = Quote::none;
    bool escaped = false;
    std::string result;
    for (std::size_t index = 0; index < command.size(); ++index) {
        const char byte = command[index];
        if (escaped) {
            result += byte;
            escaped = false;
            continue;
        }
        if (byte == '\\' && quote != Quote::single) {
            result += byte;
            escaped = true;
        } else if (byte == '\'' && quote != Quote::double_quote) {
            quote = quote == Quote::single ? Quote::none : Quote::single;
            result += byte;
        } else if (byte == '"' && quote != Quote::single) {
            quote = quote == Quote::double_quote ? Quote::none : Quote::double_quote;
            result += byte;
        } else if (byte == '%') {
            if (++index == command.size()) fail("dangling % in helper command");
            const char macro = command[index];
            if (macro == '%') result += '%';
            else if (macro >= '1' && macro <= '3') {
                const std::string variable = "${" + std::string(1, macro) + "}";
                if (quote == Quote::double_quote) result += variable;
                else if (quote == Quote::single) result += "'\"" + variable + "\"'";
                else result += "\"" + variable + "\"";
            } else fail("unsupported helper placeholder %" + std::string(1, macro));
        } else result += byte;
    }
    if (escaped || quote != Quote::none) fail("unterminated quoting in helper command");
    return result;
}

Rule select_rule(
    const std::string_view name, const TransformOpenOptions& options, const Cancellation& cancellation
) {
    if (!valid_name(name)) fail("invalid converter name");
    if (const auto path = registry_path(options)) {
        const auto bytes = read_registry(*path, options.limits, cancellation);
        std::istringstream input(bytes);
        std::string row;
        std::size_t line = 0;
        std::size_t rules = 0;
        std::optional<Rule> selected;
        while (std::getline(input, row)) {
            cancellation.throw_if_requested();
            ++line;
            const auto value = trim(row);
            if (value.empty() || value.front() == '#') continue;
            const auto location = path->string() + ":" + std::to_string(line);
            if (value.size() > 16384U) fail(location + ": helper line exceeds 16 KiB");
            if (++rules > options.limits.rules) fail(location + ": too many SFS rules");
            const auto separator = value.find_first_of(" \t");
            if (separator == std::string_view::npos) fail(location + ": missing helper command");
            const auto key = value.substr(0, separator);
            const auto flag_separator = key.find_first_of("/:");
            if (flag_separator == std::string_view::npos || !valid_name(key.substr(0, flag_separator))) {
                fail(location + ": expected converter/flags and a command");
            }
            unsigned int flags = 0;
            const auto flag = key.substr(flag_separator + 1U);
            const auto parsed = std::from_chars(flag.data(), flag.data() + flag.size(), flags);
            if (flag.empty() || parsed.ec != std::errc{} || parsed.ptr != flag.data() + flag.size()) {
                fail(location + ": invalid helper flags");
            }
            const auto command = trim(value.substr(separator));
            if (command.empty()) fail(location + ": empty helper command");
            const bool local = key[flag_separator] == '/' && flags == 1U;
            if (local) {
                try { static_cast<void>(expand_command(command)); }
                catch (const std::exception& error) { fail(location + ": " + error.what()); }
            }
            if (!selected && key.substr(0, flag_separator) == name) {
                selected = Rule{std::string(name), std::string(command), location, {}, false, local};
            }
        }
        if (selected) {
            if (!selected->local) fail(selected->origin + ": only local source/1 SFS helpers are enabled");
            return *selected;
        }
    }
    if (auto rule = standard_rule(name)) return *rule;
    fail("converter is not registered: " + std::string(name));
}

std::string executable(const std::string_view name) {
    for (const auto* directory : {"/usr/bin", "/bin"}) {
        const auto path = fs::path(directory) / name;
        if (::access(path.c_str(), X_OK) == 0) return path.string();
    }
    fail("required helper is unavailable: " + std::string(name));
}

CommandOutput run_helper(
    std::vector<std::string> arguments, const Workspace& workspace,
    const TransformLimits& limits, const Cancellation& cancellation
) {
    std::vector<std::string> bounded{
        executable("prlimit"),
        "--fsize=" + std::to_string(limits.output_bytes),
        "--as=" + std::to_string(limits.memory_bytes),
        "--cpu=" + std::to_string(limits.cpu_seconds), "--core=0", "--"
    };
    bounded.insert(bounded.end(), arguments.begin(), arguments.end());
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
        auto result = capture_command(bounded, workspace.anchored(), stopped, limits.output_bytes);
        timer.request_stop();
        cancellation.throw_if_requested();
        if (expired.load(std::memory_order_acquire)) fail("helper exceeded its elapsed-time limit");
        return result;
    } catch (const OperationCancelled&) {
        timer.request_stop();
        cancellation.throw_if_requested();
        if (expired.load(std::memory_order_acquire)) fail("helper exceeded its elapsed-time limit");
        throw;
    }
}

std::string convert(
    const Rule& rule, const std::string_view bytes, const std::string_view basename,
    const std::string_view parameter, const TransformOpenOptions& options,
    const Cancellation& cancellation
) {
    cancellation.throw_if_requested();
    Workspace workspace(options);
    const auto input = workspace.snapshot(basename, bytes, cancellation);
    workspace.create_output();
    const auto output = workspace.anchored() / "output";
    std::vector<std::string> arguments;
    if (rule.tool.empty()) {
        arguments = {"/bin/sh", "-c", expand_command(rule.command), "mcpp-sfs",
                     input.string(), std::string(parameter), output.string()};
    } else {
        const auto tool = executable(rule.tool);
        const auto mode = rule.decode ? (rule.tool == "gzip" ? "-dcf" : "-dc") : "-c";
        arguments = {"/bin/sh", "-c", "\"$1\" \"$2\" < \"$3\" > \"$4\"", "mcpp-sfs",
                     tool, mode, input.string(), output.string()};
    }
    const auto result = run_helper(std::move(arguments), workspace, options.limits, cancellation);
    if (result.status != 0) {
        fail(rule.name + " helper " + (result.status == 127 ? "is missing or failed" : "failed") +
             " (" + std::to_string(result.status) + "): " + result.error);
    }
    return workspace.read_output(options.limits.output_bytes, cancellation);
}

class BoundedBuffer final : public std::streambuf {
public:
    explicit BoundedBuffer(const std::size_t limit) : limit_(limit) {}
    [[nodiscard]] std::string take() { return std::move(bytes_); }
protected:
    std::streamsize xsputn(const char* value, const std::streamsize count) override {
        if (count < 0 || static_cast<std::uint64_t>(count) > limit_ - bytes_.size()) {
            fail("source exceeds its byte limit");
        }
        bytes_.append(value, static_cast<std::size_t>(count));
        return count;
    }
    int_type overflow(const int_type value) override {
        if (traits_type::eq_int_type(value, traits_type::eof())) return traits_type::not_eof(value);
        const char byte = traits_type::to_char_type(value);
        static_cast<void>(xsputn(&byte, 1));
        return value;
    }
private:
    std::size_t limit_;
    std::string bytes_;
};

std::string snapshot(
    const TransformSource& source, const Entry& expected, const TransformLimits& limits,
    const Cancellation& cancellation
) {
    if (expected.kind != EntryKind::file) fail("converter source is not a regular file");
    if (expected.size > limits.input_bytes) fail("source exceeds its byte limit");
    BoundedBuffer buffer(limits.input_bytes);
    std::ostream output(&buffer);
    output.exceptions(std::ios::badbit | std::ios::failbit);
    source.provider->read(source.path, 0, expected.size + 1U, output, cancellation);
    auto bytes = buffer.take();
    const auto after = source.provider->stat_follow(source.path, cancellation);
    if (bytes.size() != expected.size || after.kind != EntryKind::file ||
        after.path != expected.path || after.revision != expected.revision) {
        fail("source changed while its local copy was being prepared");
    }
    return bytes;
}

}  // namespace

class TransformProvider::Impl {
public:
    TransformSource source;
    std::string converter;
    std::string parameter;
    std::string identity;
    Entry entry;
    std::string bytes;
};

class TransformCache::Impl {
public:
    using Key = std::tuple<std::uintptr_t, std::string, std::string, std::string, std::string, std::string>;
    struct Record {
        ProviderPtr provider;
        std::chrono::steady_clock::time_point last_used;
    };
    Impl(const std::chrono::seconds lifetime, TransformOpenOptions settings)
        : timeout(lifetime), options(std::move(settings)) {}
    std::chrono::seconds timeout;
    TransformOpenOptions options;
    std::mutex mutex;
    std::map<Key, Record> records;
};

TransformCache::TransformCache(const std::chrono::seconds timeout, TransformOpenOptions options)
    : impl_(std::make_unique<Impl>(timeout, std::move(options))) {
    if (timeout.count() < 0) throw std::invalid_argument("VFS timeout cannot be negative");
    validate_limits(impl_->options.limits);
}
TransformCache::~TransformCache() = default;

ProviderPtr TransformCache::open(
    const ProviderPtr& provider, const std::string_view path,
    const std::string_view source_location, const std::string_view converter,
    const std::string_view parameter, const Cancellation& cancellation
) {
    if (!provider) throw std::invalid_argument("SFS source provider is null");
    if (!has_capability(provider->capabilities(), ProviderCapability::read)) {
        fail("source provider does not allow reading");
    }
    const auto& options = impl_->options;
    validate_path(path, options.limits, "source path");
    validate_path(source_location, options.limits, "logical source location");
    if (parameter.size() > options.limits.path_bytes || parameter.find('\0') != std::string_view::npos) {
        fail("invalid converter parameter");
    }
    prune();
    const auto rule = select_rule(converter, options, cancellation);
    TransformSource source{provider, provider->normalize(path), {}, std::string(source_location)};
    validate_path(source.path, options.limits, "normalized source path");
    const auto entry = provider->stat_follow(source.path, cancellation);
    source.revision = entry.revision;
    if (entry.kind != EntryKind::file) fail("converter source is not a regular file");
    if (source.revision.empty()) fail("source provider did not supply a revision");
    const Impl::Key key{
        reinterpret_cast<std::uintptr_t>(provider.get()), source.path, source.revision,
        rule.identity(), std::string(parameter), source.location
    };
    {
        const std::lock_guard lock(impl_->mutex);
        const auto found = impl_->records.find(key);
        if (found != impl_->records.end()) {
            found->second.last_used = std::chrono::steady_clock::now();
            return found->second.provider;
        }
    }
    const auto basename = provider->basename(source.path);
    const auto leaf = parameter.empty() ? basename : fs::path(parameter).filename().string();
    if (leaf == "." || leaf == "..") fail("converter parameter has an unsafe output basename");
    const auto input = snapshot(source, entry, options.limits, cancellation);
    auto next = std::make_unique<TransformProvider::Impl>();
    next->source = source;
    next->converter = std::string(converter);
    next->parameter = std::string(parameter);
    next->bytes = convert(rule, input, basename, parameter, options, cancellation);
    const auto after = provider->stat_follow(source.path, cancellation);
    if (after.kind != EntryKind::file || after.path != entry.path || after.revision != entry.revision) {
        fail("source changed during conversion");
    }
    static std::atomic_uint64_t sequence{1};
    const auto serial = sequence.fetch_add(1, std::memory_order_relaxed);
    next->identity = "sfs:" + identity_part(provider->id()) + identity_part(source.path) +
        identity_part(source.revision) + identity_part(rule.identity()) + identity_part(parameter) +
        ":" + std::to_string(serial);
    next->entry = entry;
    auto& transformed = next->entry;
    transformed.name = leaf.empty() ? basename : leaf;
    transformed.path = "/";
    transformed.kind = EntryKind::file;
    transformed.size = next->bytes.size();
    transformed.permissions = fs::perms::owner_read | fs::perms::owner_write;
    transformed.mode = S_IFREG | 0600U;
    transformed.inode = serial;
    transformed.links = 1;
    transformed.revision = next->identity;
    transformed.target.clear();
    transformed.target_is_directory = false;
    transformed.hidden = transformed.name.starts_with('.');
    ProviderPtr opened(new TransformProvider(std::move(next)));
    const std::lock_guard lock(impl_->mutex);
    const auto [found, inserted] = impl_->records.try_emplace(
        key, Impl::Record{opened, std::chrono::steady_clock::now()});
    if (!inserted) found->second.last_used = std::chrono::steady_clock::now();
    return found->second.provider;
}

void TransformCache::prune() {
    const auto now = std::chrono::steady_clock::now();
    const std::lock_guard lock(impl_->mutex);
    for (auto item = impl_->records.begin(); item != impl_->records.end();) {
        if (item->second.provider.use_count() > 1) {
            item->second.last_used = now;
            ++item;
        } else if (now - item->second.last_used >= impl_->timeout) {
            item = impl_->records.erase(item);
        } else ++item;
    }
}

void TransformCache::set_timeout(const std::chrono::seconds timeout) {
    if (timeout.count() < 0) throw std::invalid_argument("VFS timeout cannot be negative");
    {
        const std::lock_guard lock(impl_->mutex);
        impl_->timeout = timeout;
    }
    prune();
}

std::vector<std::string> TransformCache::locations() {
    prune();
    const std::lock_guard lock(impl_->mutex);
    std::vector<std::string> result;
    for (const auto& [key, record] : impl_->records) {
        static_cast<void>(key);
        result.push_back(static_cast<const TransformProvider&>(*record.provider).location());
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

TransformProvider::TransformProvider(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
TransformProvider::~TransformProvider() = default;
const TransformSource& TransformProvider::source() const noexcept { return impl_->source; }
const std::string& TransformProvider::converter() const noexcept { return impl_->converter; }
const std::string& TransformProvider::parameter() const noexcept { return impl_->parameter; }
std::string TransformProvider::location() const {
    return impl_->source.location + "/" + impl_->converter + "://" + impl_->parameter;
}
std::string TransformProvider::id() const { return impl_->identity; }
std::string TransformProvider::display_name() const { return location(); }
ProviderCapability TransformProvider::capabilities() const { return ProviderCapability::read; }
std::string TransformProvider::normalize(const std::string_view path) const {
    if (path != "/" && !path.empty()) path_error(path, std::errc::no_such_file_or_directory);
    return "/";
}
std::string TransformProvider::join(const std::string_view directory, std::string_view) const {
    path_error(directory, std::errc::not_a_directory);
}
std::string TransformProvider::parent(const std::string_view path) const { return normalize(path); }
std::string TransformProvider::basename(const std::string_view path) const {
    static_cast<void>(normalize(path));
    return impl_->entry.name;
}
std::vector<Entry> TransformProvider::list(
    const std::string_view path, const ListRequest&, const Cancellation& cancellation
) const {
    cancellation.throw_if_requested();
    path_error(path, std::errc::not_a_directory);
}
Entry TransformProvider::stat(const std::string_view path, const Cancellation& cancellation) const {
    cancellation.throw_if_requested();
    static_cast<void>(normalize(path));
    return impl_->entry;
}
Entry TransformProvider::stat_follow(const std::string_view path, const Cancellation& cancellation) const {
    return stat(path, cancellation);
}
bool TransformProvider::exists(const std::string_view path) const { return path.empty() || path == "/"; }
SpaceInfo TransformProvider::space(const std::string_view path) const {
    static_cast<void>(normalize(path));
    return {};
}
void TransformProvider::read(
    const std::string_view path, const std::uint64_t offset, const std::uint64_t length,
    std::ostream& output, const Cancellation& cancellation
) const {
    static_cast<void>(normalize(path));
    cancellation.throw_if_requested();
    if (offset >= impl_->bytes.size()) return;
    auto remaining = std::min<std::uint64_t>(length, impl_->bytes.size() - offset);
    auto position = static_cast<std::size_t>(offset);
    while (remaining > 0U) {
        cancellation.throw_if_requested();
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, 65536U));
        output.write(impl_->bytes.data() + position, static_cast<std::streamsize>(count));
        if (!output.good()) fail("failed while writing transformed file data");
        position += count;
        remaining -= count;
    }
}
void TransformProvider::write(
    std::string_view, std::istream&, std::optional<std::uint64_t>, bool,
    std::optional<std::string>, const Cancellation&, const ProgressCallback&
) { readonly(); }
void TransformProvider::create_directory(std::string_view) { readonly(); }
void TransformProvider::remove(std::string_view, bool, const Cancellation&, const ProgressCallback&) { readonly(); }
void TransformProvider::rename(std::string_view, std::string_view, bool) { readonly(); }
bool TransformProvider::copy_direct(
    std::string_view, std::string_view, bool, const Cancellation&, const ProgressCallback&
) { readonly(); }
void TransformProvider::set_permissions(std::string_view, fs::perms) { readonly(); }
void TransformProvider::set_owner(
    std::string_view, std::optional<std::string>, std::optional<std::string>
) { readonly(); }
void TransformProvider::create_symlink(
    std::string_view, std::string_view, bool, const Cancellation&
) { readonly(); }
void TransformProvider::create_hardlink(std::string_view, std::string_view) { readonly(); }

std::optional<std::string_view> compression_decoder(const std::string_view header) noexcept {
    if (header.starts_with(std::string_view("\x1f\x8b", 2))) return "ugz";
    if (header.starts_with("BZh")) return "ubz2";
    if (header.starts_with(std::string_view("\xfd\x37\x7a\x58\x5a\x00", 6))) return "uxz";
    if (header.starts_with(std::string_view("\x28\xb5\x2f\xfd", 4))) return "uzst";
    return {};
}

std::string decompress_bytes(
    const std::string_view bytes, const std::string_view decoder,
    const Cancellation& cancellation, TransformOpenOptions options
) {
    validate_limits(options.limits);
    const auto rule = standard_rule(decoder);
    if (!rule || !rule->decode) fail("not a built-in compression decoder");
    if (bytes.size() > options.limits.input_bytes) fail("compressed input exceeds its byte limit");
    return convert(*rule, bytes, "source", {}, options, cancellation);
}

}  // namespace mcpp
