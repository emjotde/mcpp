#include "mcpp/archive_provider.hpp"

#include "archive_data.hpp"
#include "local_tools_stream.hpp"
#include "mcpp/local_provider.hpp"
#include "mcpp/local_tools.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <fstream>
#include <grp.h>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <pwd.h>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <system_error>
#include <tuple>
#include <unistd.h>
#include <utility>

namespace mcpp {
namespace {

using Clock = std::chrono::system_clock;
using Time = Clock::time_point;
namespace fs = std::filesystem;
using archive_detail::Content;
using archive_detail::Data;
using archive_detail::Extent;
using archive_detail::content_range;
using archive_detail::zero_range;

[[noreturn]] void invalid(const std::string& reason) {
    throw std::runtime_error("archive: " + reason);
}

[[noreturn]] void path_error(const std::string& path, const std::errc code) {
    throw fs::filesystem_error("archive member", fs::path(path), std::make_error_code(code));
}

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

void check_system(const int result, const char* operation) {
    if (result < 0) throw std::system_error(errno, std::generic_category(), operation);
}

std::string random_name() {
    std::array<unsigned char, 16> bytes{};
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = ::getrandom(bytes.data() + offset, bytes.size() - offset, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) throw std::system_error(errno, std::generic_category(), "archive draft randomness");
        offset += static_cast<std::size_t>(count);
    }
    constexpr std::string_view digits = "0123456789abcdef";
    std::string result = "archive-";
    for (const auto byte : bytes) {
        result.push_back(digits[byte >> 4U]);
        result.push_back(digits[byte & 15U]);
    }
    return result;
}

class DraftDirectory {
public:
    explicit DraftDirectory(const fs::path& base, const bool retain)
        : base_path_(fs::absolute(base)), retain_(retain),
          base_(open_base(base_path_)), name_(random_name()),
          directory_(make_directory(base_.get(), name_)) {
        check_system(::fsync(base_.get()), "sync archive draft directory");
    }
    ~DraftDirectory() {
        if (retain_) return;
        for (const char* name : {"snapshot", "raw", "candidate"}) {
            if (::unlinkat(directory_.get(), name, 0) < 0 && errno != ENOENT) {
                std::cerr << "mcpp: archive draft cleanup: " << std::strerror(errno) << '\n';
            }
        }
        if (::unlinkat(base_.get(), name_.c_str(), AT_REMOVEDIR) < 0) {
            std::cerr << "mcpp: archive draft cleanup: " << std::strerror(errno) << '\n';
        }
    }
    DraftDirectory(const DraftDirectory&) = delete;
    DraftDirectory& operator=(const DraftDirectory&) = delete;
    [[nodiscard]] fs::path path(const std::string_view name) const {
        return base_path_ / name_ / name;
    }
    [[nodiscard]] fs::path anchored(const std::string_view name) const {
        return fs::path("/proc") / std::to_string(::getpid()) / "fd" /
               std::to_string(directory_.get()) / name;
    }
    void discard() noexcept { retain_ = false; }
    [[nodiscard]] int create(const char* name) const {
        const auto result = ::openat(
            directory_.get(), name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600
        );
        check_system(result, "create archive draft");
        return result;
    }
    void write(const char* name, const Data& bytes, const Cancellation& cancellation) const {
        Descriptor output(create(name));
        archive_detail::write_storage(bytes, output.get(), 0, bytes.size(), 0, cancellation);
        check_system(::ftruncate(output.get(), static_cast<off_t>(bytes.size())), "size archive draft");
        check_system(::fsync(output.get()), "sync archive draft");
        check_system(::fsync(directory_.get()), "sync archive draft entry");
    }
    void special(const mode_t mode, const dev_t device) const {
        if (S_ISFIFO(mode)) check_system(::mkfifoat(directory_.get(), "snapshot", 0600), "prepare archive FIFO");
        else check_system(::mknodat(directory_.get(), "snapshot", (mode & S_IFMT) | 0600U, device),
                          "recreate archive device (mknod requires permission, normally CAP_MKNOD)");
    }
    void metadata(const Entry& entry) const {
        check_system(::fchmodat(directory_.get(), "snapshot", static_cast<mode_t>(entry.permissions), 0),
                     "set archive member snapshot permissions");
        const auto seconds = std::chrono::floor<std::chrono::seconds>(entry.modified.time_since_epoch());
        const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
            entry.modified.time_since_epoch() - seconds);
        const std::array<timespec, 2> times{{
            {.tv_sec = 0, .tv_nsec = UTIME_OMIT},
            {.tv_sec = static_cast<time_t>(seconds.count()), .tv_nsec = static_cast<long>(nanos.count())}
        }};
        check_system(::utimensat(directory_.get(), "snapshot", times.data(), AT_SYMLINK_NOFOLLOW),
                     "set archive member snapshot modification time");
    }

private:
    static int open_base(const fs::path& base) {
        if (base.native().find('\0') != std::string::npos) invalid("NUL in draft directory");
        if (::mkdir(base.c_str(), 0700) < 0 && errno != EEXIST) {
            throw std::system_error(errno, std::generic_category(), "create archive draft directory");
        }
        const int descriptor = ::open(base.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        check_system(descriptor, "open archive draft directory");
        struct stat state{};
        if (::fstat(descriptor, &state) < 0) {
            const auto error = errno;
            static_cast<void>(::close(descriptor));
            throw std::system_error(error, std::generic_category(), "inspect archive draft directory");
        }
        if (state.st_uid != ::geteuid() || (state.st_mode & 0077U) != 0U) {
            static_cast<void>(::close(descriptor));
            invalid("draft directory must be owned by this user and private (0700)");
        }
        return descriptor;
    }
    static int make_directory(const int base, const std::string& name) {
        check_system(::mkdirat(base, name.c_str(), 0700), "create private archive draft");
        const int descriptor = ::openat(base, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        check_system(descriptor, "open private archive draft");
        return descriptor;
    }

    fs::path base_path_;
    bool retain_;
    Descriptor base_;
    std::string name_;
    Descriptor directory_;
};

bool all_zero(const std::string_view value) {
    return std::all_of(value.begin(), value.end(), [](const char byte) { return byte == '\0'; });
}

std::uint64_t unsigned_number(const std::string_view value, const int base, const char* field) {
    if (value.empty()) invalid(std::string("empty ") + field);
    std::uint64_t result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result, base);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
        invalid(std::string("invalid ") + field);
    }
    return result;
}

std::uint32_t uint32_number(const std::uint64_t number, const char* field) {
    if (number > std::numeric_limits<std::uint32_t>::max()) {
        invalid(std::string(field) + " is out of range");
    }
    return static_cast<std::uint32_t>(number);
}

std::string text_field(const std::string_view field) {
    const auto end = field.find('\0');
    if (end == std::string_view::npos) return std::string(field);
    if (!all_zero(field.substr(end))) invalid("nonzero bytes after a header string terminator");
    return std::string(field.substr(0, end));
}

std::int64_t tar_number(std::string_view field) {
    if ((static_cast<unsigned char>(field.front()) & 0x80U) != 0U) {
        const bool negative = (static_cast<unsigned char>(field.front()) & 0x40U) != 0U;
        std::uint64_t number = 0;
        for (std::size_t index = 0; index < field.size(); ++index) {
            auto byte = static_cast<unsigned char>(field[index]);
            if (index == 0U) byte = static_cast<unsigned char>((byte & 0x7fU) | (negative ? 0x80U : 0U));
            if (field.size() - index > 8U) {
                if (byte != (negative ? 0xffU : 0U)) invalid("tar binary number is out of range");
            } else {
                number = (number << 8U) | byte;
            }
        }
        if (field.size() < 8U && negative) number |= ~std::uint64_t{0} << (field.size() * 8U);
        const auto result = std::bit_cast<std::int64_t>(number);
        if ((result < 0) != negative) invalid("tar binary number is out of range");
        return result;
    }
    while (!field.empty() && field.front() == ' ') field.remove_prefix(1);
    while (!field.empty() && (field.back() == ' ' || field.back() == '\0')) field.remove_suffix(1);
    if (field.empty()) return 0;
    const auto number = unsigned_number(field, 8, "tar octal number");
    if (number > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        invalid("tar number is out of range");
    }
    return static_cast<std::int64_t>(number);
}

std::uint64_t nonnegative_tar_number(const std::string_view field) {
    const auto number = tar_number(field);
    if (number < 0) invalid("negative tar size, mode, or identity");
    return static_cast<std::uint64_t>(number);
}

Time parse_time(std::string_view value) {
    bool negative = false;
    if (!value.empty() && value.front() == '-') {
        negative = true;
        value.remove_prefix(1);
    }
    const auto dot = value.find('.');
    const auto seconds = unsigned_number(value.substr(0, dot), 10, "timestamp");
    std::uint64_t nanos = 0;
    if (dot != std::string_view::npos) {
        auto fraction = value.substr(dot + 1U);
        if (fraction.empty() || fraction.size() > 9U) invalid("timestamp precision exceeds nanoseconds");
        nanos = unsigned_number(fraction, 10, "timestamp fraction");
        for (auto size = fraction.size(); size < 9U; ++size) nanos *= 10U;
    }
    constexpr auto maximum = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    const auto bound = maximum + (negative ? 1U : 0U);
    if (seconds > bound / 1000000000U ||
        (seconds == bound / 1000000000U && nanos > bound % 1000000000U)) {
        invalid("timestamp is outside the system clock range");
    }
    const auto magnitude = seconds * 1000000000U + nanos;
    const auto count = negative
        ? (magnitude == maximum + 1U ? std::numeric_limits<std::int64_t>::min()
                                    : -static_cast<std::int64_t>(magnitude))
        : static_cast<std::int64_t>(magnitude);
    return Time{std::chrono::duration_cast<Clock::duration>(std::chrono::nanoseconds(count))};
}

std::string time_text(const Time time) {
    const auto count = std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count();
    const bool negative = count < 0;
    const auto magnitude = negative ? static_cast<std::uint64_t>(-(count + 1)) + 1U
                                    : static_cast<std::uint64_t>(count);
    std::string result = (negative ? "-" : "") + std::to_string(magnitude / 1000000000U);
    if (magnitude % 1000000000U != 0U) {
        auto fraction = std::to_string(1000000000U + magnitude % 1000000000U).substr(1);
        while (fraction.back() == '0') fraction.pop_back();
        result += "." + fraction;
    }
    return result;
}

std::vector<std::string> components(const std::string_view value) {
    std::vector<std::string> result;
    for (std::size_t begin = 0; begin < value.size();) {
        const auto end = value.find('/', begin);
        result.emplace_back(value.substr(begin, end == std::string_view::npos ? value.size() - begin : end - begin));
        if (end == std::string_view::npos) break;
        begin = end + 1U;
    }
    return result;
}

std::string parent_key(const std::string_view path) {
    const auto slash = path.rfind('/');
    return slash == 0U ? "/" : std::string(path.substr(0, slash));
}

std::string base_key(const std::string_view path) {
    return path == "/" ? std::string{} : std::string(path.substr(path.rfind('/') + 1U));
}

std::string virtual_path(std::string_view path, const ArchiveLimits& limits) {
    if (path.find('\0') != std::string_view::npos) invalid("NUL in member path");
    if (path.empty() || path == "/") return "/";
    if (path.front() == '/') path.remove_prefix(1);
    if (!path.empty() && path.back() == '/') path.remove_suffix(1);
    if (path.empty() || path.size() > limits.path_bytes) invalid("invalid member path length");
    for (const auto& part : components(path)) {
        if (part.empty() || part == "." || part == ".." || part.size() > 255U) {
            invalid("ambiguous or unsafe member path");
        }
    }
    if (path.front() == '/' || path.back() == '/') invalid("repeated slash in member path");
    return "/" + std::string(path);
}

std::string member_path(std::string_view raw, const bool directory, const ArchiveLimits& limits) {
    if (raw.empty() || raw.front() == '/' || raw.find('\0') != std::string_view::npos) {
        invalid("absolute, empty, or NUL-containing archive member name");
    }
    if (raw == "." || raw == "./") {
        if (!directory) invalid("non-directory archive root");
        return "/";
    }
    if (raw.starts_with("./")) raw.remove_prefix(2);
    if (raw.empty() || raw.front() == '/') invalid("ambiguous archive member prefix");
    if (!directory && raw.back() == '/') invalid("non-directory member has a trailing slash");
    return virtual_path(raw, limits);
}

void check_link_text(const std::string_view target, const ArchiveLimits& limits) {
    if (target.empty() || target.front() == '/' || target.find('\0') != std::string_view::npos ||
        target.size() > limits.path_bytes || target.find("//") != std::string_view::npos) {
        invalid("absolute, empty, ambiguous, or overlong link target");
    }
    for (const auto& part : components(target)) {
        if (part.size() > 255U) invalid("overlong link target component");
    }
}

struct Node {
    Entry entry;
    std::shared_ptr<const Content> bytes;
    std::string hardlink;
    bool implicit{false};
    std::uint32_t device_major{0};
    std::uint32_t device_minor{0};
    std::uint64_t object{0};
    std::uint64_t version{1};
};

using Model = std::map<std::string, Node, std::less<>>;

Node new_node(const EntryKind kind, const std::uint64_t object) {
    Node node;
    node.object = object;
    node.entry.kind = kind;
    node.entry.permissions = kind == EntryKind::directory ? fs::perms{0755} :
                             kind == EntryKind::symlink ? fs::perms{0777} : fs::perms{0644};
    node.entry.mode = (kind == EntryKind::directory ? S_IFDIR :
                       kind == EntryKind::symlink ? S_IFLNK : S_IFREG) |
                      static_cast<std::uint32_t>(node.entry.permissions);
    node.entry.uid = static_cast<std::uint32_t>(::geteuid());
    node.entry.gid = static_cast<std::uint32_t>(::getegid());
    node.entry.owner = std::to_string(node.entry.uid);
    node.entry.group = std::to_string(node.entry.gid);
    node.entry.modified = Clock::now();
    node.entry.accessed = node.entry.modified;
    node.entry.changed = node.entry.modified;
    if (kind == EntryKind::file) node.bytes = std::make_shared<const Content>();
    return node;
}

struct ParseState {
    Model model;
    std::uint64_t next_object{1};
    std::set<std::string, std::less<>> explicit_names;
    bool incremental{false};
    explicit ParseState() {
        auto root = new_node(EntryKind::directory, next_object++);
        root.implicit = true;
        model.emplace("/", std::move(root));
    }
    void insert(
        const std::string& key, Node node, const ArchiveLimits& limits
    ) {
        if (!explicit_names.insert(key).second) invalid("duplicate or aliased archive member: " + key);
        std::vector<std::string> parents;
        for (auto parent = parent_key(key); parent != "/" && !model.contains(parent); parent = parent_key(parent)) {
            parents.push_back(parent);
        }
        for (auto item = parents.rbegin(); item != parents.rend(); ++item) {
            auto directory = new_node(EntryKind::directory, next_object++);
            directory.implicit = true;
            model.emplace(*item, std::move(directory));
        }
        const auto existing = model.find(key);
        if (existing != model.end() && !existing->second.implicit) invalid("duplicate archive path: " + key);
        if (existing != model.end() && node.entry.kind != EntryKind::directory) {
            invalid("non-directory archive member has children: " + key);
        }
        model.insert_or_assign(key, std::move(node));
        if (model.size() - 1U > limits.members) invalid("archive exceeds the member limit");
    }
};

// Only virtual names are resolved. No archive member is passed to host file APIs.
std::optional<std::string> resolve(
    const Model& model, const std::string& path, const bool follow_last,
    const bool allow_missing = false
) {
    std::deque<std::string> pending;
    for (auto& part : components(std::string_view(path).substr(1))) pending.push_back(std::move(part));
    std::vector<std::string> traversed;
    unsigned int followed = 0;
    while (!pending.empty()) {
        auto part = std::move(pending.front());
        pending.pop_front();
        if (part.empty() || part == ".") continue;
        if (part == "..") {
            if (traversed.empty()) invalid("link target escapes the archive root");
            traversed.pop_back();
            continue;
        }
        std::string candidate;
        for (const auto& parent : traversed) candidate += "/" + parent;
        candidate += "/" + part;
        const auto found = model.find(candidate);
        if (found == model.end()) {
            if (!allow_missing) return std::nullopt;
            traversed.push_back(std::move(part));
            continue;
        }
        if (found->second.entry.kind == EntryKind::symlink && (follow_last || !pending.empty())) {
            if (++followed > 40U) invalid("symbolic link cycle or chain longer than 40 links");
            auto target = components(found->second.entry.target);
            if (found->second.entry.target.ends_with('/')) target.emplace_back(".");
            for (auto item = target.rbegin(); item != target.rend(); ++item) pending.push_front(*item);
        } else {
            if (!pending.empty() && found->second.entry.kind != EntryKind::directory) {
                if (allow_missing) invalid("link target traverses a non-directory");
                return std::nullopt;
            }
            traversed.push_back(std::move(part));
        }
    }
    std::string result;
    for (const auto& part : traversed) result += "/" + part;
    return result.empty() ? "/" : result;
}

std::string require_path(const Model& model, const std::string& path, const bool follow_last) {
    const auto result = resolve(model, path, follow_last);
    if (!result) path_error(path, std::errc::no_such_file_or_directory);
    return *result;
}

void validate_model(const Model& model, const ArchiveLimits& limits, const Cancellation& cancellation) {
    if (model.empty() || model.size() - 1U > limits.members) invalid("archive exceeds the member limit");
    std::uint64_t total = 0;
    std::size_t extents = 0;
    std::set<std::uint64_t> counted;
    for (const auto& [key, node] : model) {
        cancellation.throw_if_requested();
        if (key != "/") {
            static_cast<void>(virtual_path(key, limits));
            const auto parent = model.find(parent_key(key));
            if (parent == model.end() || parent->second.entry.kind != EntryKind::directory) {
                invalid("archive member has a non-directory parent: " + key);
            }
        }
        if (node.entry.kind == EntryKind::symlink) {
            check_link_text(node.entry.target, limits);
            static_cast<void>(resolve(model, key, true, true));
        }
        if (node.entry.kind == EntryKind::file && counted.insert(node.object).second) {
            if (!node.bytes || node.bytes->size() > limits.expanded_bytes - total) {
                invalid("archive file data exceeds the expanded byte limit");
            }
            total += node.bytes->size();
            if (node.bytes->extents.size() > limits.sparse_extents - extents) {
                invalid("archive exceeds the sparse extent limit");
            }
            extents += node.bytes->extents.size();
        }
    }
}

using Pax = std::map<std::string, std::string, std::less<>>;
using SparseMap = std::vector<std::pair<std::uint64_t, std::uint64_t>>;

Pax parse_pax(const std::string_view bytes, SparseMap* sparse, const ArchiveLimits& limits) {
    static const std::set<std::string_view> supported{
        "path", "linkpath", "size", "uid", "gid", "uname", "gname",
        "mtime", "atime", "ctime", "hdrcharset", "GNU.sparse.major", "GNU.sparse.minor",
        "GNU.sparse.name", "GNU.sparse.realsize", "GNU.sparse.size", "GNU.sparse.numblocks",
        "GNU.sparse.map", "GNU.sparse.offset", "GNU.sparse.numbytes", "GNU.dumpdir"
    };
    Pax result;
    std::optional<std::uint64_t> sparse_offset;
    for (std::size_t offset = 0; offset < bytes.size();) {
        const auto space = bytes.find(' ', offset);
        if (space == std::string_view::npos) invalid("invalid PAX record length");
        const auto length = unsigned_number(bytes.substr(offset, space - offset), 10, "PAX record length");
        if (length == 0U || length > bytes.size() - offset || length <= space - offset + 2U) {
            invalid("PAX record exceeds its header");
        }
        const auto record = bytes.substr(space + 1U, static_cast<std::size_t>(length) - (space - offset) - 1U);
        if (record.back() != '\n') invalid("unterminated PAX record");
        const auto equal = record.find('=');
        if (equal == std::string_view::npos || equal == 0U) invalid("invalid PAX key/value");
        const auto key = record.substr(0, equal);
        if (!supported.contains(key)) invalid("unsupported PAX metadata: " + std::string(key));
        auto value = std::string(record.substr(equal + 1U, record.size() - equal - 2U));
        if (key != "GNU.dumpdir" && value.find('\0') != std::string::npos) invalid("NUL in PAX metadata");
        if (key == "GNU.dumpdir" && sparse == nullptr) invalid("global PAX directory dump is unsupported");
        if (key.starts_with("GNU.sparse.") && sparse == nullptr) invalid("global sparse PAX metadata");
        if (key == "GNU.sparse.offset") {
            if (sparse_offset) invalid("unpaired PAX sparse offset");
            sparse_offset = unsigned_number(value, 10, "PAX sparse offset");
        } else if (key == "GNU.sparse.numbytes") {
            if (!sparse_offset || sparse->size() >= limits.sparse_extents) invalid("invalid or excessive PAX sparse map");
            sparse->emplace_back(*sparse_offset, unsigned_number(value, 10, "PAX sparse length"));
            sparse_offset.reset();
        } else if (!result.emplace(std::string(key), std::move(value)).second) invalid("duplicate PAX key");
        offset += static_cast<std::size_t>(length);
    }
    if (sparse_offset) invalid("unpaired PAX sparse offset");
    return result;
}

void validate_dumpdir(
    const std::string_view bytes, const ArchiveLimits& limits, const Cancellation& cancellation
) {
    if (bytes.empty() || bytes.size() > limits.metadata_bytes) invalid("invalid or oversized GNU directory dump");
    bool rename_target = false;
    bool temporary_directory = false;
    for (std::size_t offset = 0; offset < bytes.size();) {
        cancellation.throw_if_requested();
        const char code = bytes[offset];
        if (code == '\0') {
            if (offset + 1U != bytes.size() || rename_target) invalid("invalid GNU directory dump terminator");
            return;
        }
        const auto end = bytes.find('\0', offset + 1U);
        if (end == std::string_view::npos) invalid("unterminated GNU directory dump record");
        const auto name = bytes.substr(offset + 1U, end - offset - 1U);
        if (name.size() > limits.path_bytes) invalid("overlong GNU directory dump name");
        if (rename_target && code != 'T') invalid("unpaired GNU directory dump rename");
        switch (code) {
            case 'Y': case 'N': case 'D':
                static_cast<void>(member_path(name, code == 'D', limits));
                break;
            case 'X':
                if (name.empty()) invalid("invalid GNU directory dump temporary directory");
                temporary_directory = true;
                break;
            case 'R':
                if (name.empty() && !temporary_directory) invalid("missing GNU directory dump rename source");
                rename_target = true;
                break;
            case 'T':
                if (!rename_target || (name.empty() && !temporary_directory)) {
                    invalid("unpaired GNU directory dump rename target");
                }
                rename_target = false;
                break;
            default: invalid("unsupported GNU directory dump control code");
        }
        offset = end + 1U;
    }
    invalid("missing GNU directory dump terminator");
}

void check_tar_checksum(const std::string_view header) {
    std::uint64_t sum = 0;
    std::int64_t signed_sum = 0;
    for (std::size_t index = 0; index < header.size(); ++index) {
        const auto byte = index >= 148U && index < 156U ? static_cast<unsigned char>(' ')
                                                                       : static_cast<unsigned char>(header[index]);
        sum += byte;
        signed_sum += byte < 128U ? byte : static_cast<int>(byte) - 256;
    }
    const auto expected = nonnegative_tar_number(header.substr(148, 8));
    if (expected != sum && (signed_sum < 0 || expected != static_cast<std::uint64_t>(signed_sum))) {
        invalid("tar header checksum mismatch");
    }
}

std::uint64_t padded(const std::uint64_t size, const std::uint64_t alignment) {
    if (size > std::numeric_limits<std::uint64_t>::max() - (alignment - 1U)) invalid("archive size overflow");
    return (size + alignment - 1U) / alignment * alignment;
}

std::shared_ptr<const Content> sparse_content(
    const std::shared_ptr<const Data>& bytes, const SparseMap& map,
    const std::uint64_t logical, const std::uint64_t stored, const std::uint64_t size,
    const ArchiveLimits& limits
) {
    if (logical > limits.expanded_bytes ||
        logical > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) ||
        map.size() > limits.sparse_extents) {
        invalid("sparse member exceeds its size or extent limit");
    }
    auto result = std::make_shared<Content>();
    result->data = bytes;
    result->length = logical;
    auto position = stored;
    std::uint64_t end = 0;
    for (const auto& [offset, length] : map) {
        if (offset < end || offset > logical || length > logical - offset ||
            length > size - (position - stored)) {
            invalid("overlapping, out-of-range, or truncated sparse map");
        }
        end = offset + length;
        if (length != 0U) result->extents.push_back({offset, length, position});
        position += length;
    }
    if (position - stored != size) invalid("sparse map does not describe all stored member data");
    return result;
}

void gnu_sparse_map(const std::string_view header, const std::size_t count, SparseMap& map,
                    const ArchiveLimits& limits) {
    for (std::size_t index = 0; index < count; ++index) {
        const auto offset = nonnegative_tar_number(header.substr(index * 24U, 12));
        const auto length = nonnegative_tar_number(header.substr(index * 24U + 12U, 12));
        if (offset == 0U && length == 0U) continue;
        if (map.size() >= limits.sparse_extents) invalid("archive exceeds the sparse extent limit");
        map.emplace_back(offset, length);
    }
}

bool sparse_extension(const char byte) {
    if (byte != '\0' && byte != '\1') invalid("invalid GNU sparse extension flag");
    return byte == '\1';
}

std::uint64_t sparse_line(
    const Data& bytes, std::uint64_t& position, const std::uint64_t end,
    const Cancellation& cancellation
) {
    const auto text = bytes.bytes(position,
        static_cast<std::size_t>(std::min<std::uint64_t>(32U, end - position)), cancellation);
    const auto newline = text.find('\n');
    if (newline == std::string::npos) invalid("unterminated or oversized sparse map number");
    position += newline + 1U;
    return unsigned_number(std::string_view(text).substr(0, newline), 10, "sparse map number");
}

ParseState parse_tar(
    const std::shared_ptr<const Data>& bytes, const ArchiveLimits& limits,
    const Cancellation& cancellation
) {
    ParseState state;
    Pax global;
    Pax local;
    SparseMap local_sparse;
    std::optional<std::string> long_name;
    std::optional<std::string> long_link;
    bool pending_extension = false;
    std::size_t records = 0;
    std::uint64_t offset = 0;
    while (offset <= bytes->size() && bytes->size() - offset >= 512U) {
        cancellation.throw_if_requested();
        const auto header_bytes = bytes->bytes(offset, 512, cancellation);
        const std::string_view header(header_bytes);
        if (all_zero(header)) {
            if (pending_extension || bytes->size() - offset < 1024U ||
                !zero_range(*bytes, offset, bytes->size() - offset, cancellation)) {
                invalid("truncated tar terminator, dangling extension, or trailing archive data");
            }
            return state;
        }
        if (++records > limits.members * 4U + 16U) invalid("too many tar extension headers");
        check_tar_checksum(header);
        const auto magic = header.substr(257, 6);
        const bool gnu = magic == std::string_view("ustar ", 6);
        const bool ustar = magic == std::string_view("ustar\0", 6);
        if (!gnu && !ustar && !all_zero(magic)) invalid("unsupported tar header format");
        if ((ustar && header.substr(263, 2) != "00") ||
            (gnu && header.substr(263, 2) != std::string_view(" \0", 2))) {
            invalid("unsupported tar header version");
        }
        auto name = text_field(header.substr(0, 100));
        if (ustar) {
            const auto prefix = text_field(header.substr(345, 155));
            if (!prefix.empty()) name = prefix + "/" + name;
        }
        auto link = text_field(header.substr(157, 100));
        const char type = header[156];
        auto size = nonnegative_tar_number(header.substr(124, 12));
        Pax fields = global;
        const bool extension = type == 'x' || type == 'g' || type == 'L' || type == 'K';
        if (!extension) {
            for (const auto& [key, value] : local) {
                if (value.empty() && !key.starts_with("GNU.sparse.") && key != "GNU.dumpdir") fields.erase(key);
                else fields.insert_or_assign(key, value);
            }
            if (const auto found = fields.find("size"); found != fields.end()) size = unsigned_number(found->second, 10, "PAX size");
        }
        offset += 512U;
        SparseMap sparse = local_sparse;
        if (type == 'S') {
            if (!gnu || !sparse.empty() ||
                std::any_of(fields.begin(), fields.end(), [](const auto& field) {
                    return field.first.starts_with("GNU.sparse.");
                })) invalid("conflicting GNU sparse formats");
            gnu_sparse_map(header.substr(386, 96), 4, sparse, limits);
            auto extended = sparse_extension(header[482]);
            std::size_t sparse_headers = 0;
            while (extended) {
                if (bytes->size() - offset < 512U) invalid("truncated GNU sparse extension");
                const auto block = bytes->bytes(offset, 512, cancellation);
                gnu_sparse_map(block, 21, sparse, limits);
                extended = sparse_extension(block[504]);
                if (!all_zero(std::string_view(block).substr(505))) invalid("nonzero GNU sparse extension padding");
                offset += 512U;
                if (++sparse_headers > limits.metadata_bytes / 512U) {
                    invalid("too many GNU sparse extension headers");
                }
            }
        }
        if (size > limits.expanded_bytes || size > bytes->size() - offset) invalid("truncated or oversized tar member");
        const auto body_offset = offset;
        const auto stored_size = padded(size, 512U);
        if (stored_size > bytes->size() - offset) invalid("truncated tar padding");
        if (!zero_range(*bytes, offset + size, stored_size - size, cancellation)) invalid("nonzero tar member padding");
        offset += stored_size;
        if (extension) {
            if (size > limits.metadata_bytes) invalid("tar extension exceeds the metadata byte limit");
            const auto body = bytes->bytes(body_offset, static_cast<std::size_t>(size), cancellation);
            pending_extension = true;
            if (type == 'x') {
                if (!local.empty()) invalid("stacked local PAX headers");
                local = parse_pax(body, &local_sparse, limits);
            } else if (type == 'g') {
                for (auto& [key, value] : parse_pax(body, nullptr, limits)) {
                    if (key == "path" || key == "linkpath" || key == "size") invalid("global PAX path or size is unsupported");
                    if (value.empty()) global.erase(key);
                    else global.insert_or_assign(key, std::move(value));
                }
            } else {
                if (body.empty() || body.back() != '\0') invalid("unterminated GNU long name");
                auto value = text_field(body);
                if (value.empty() || value.size() > limits.path_bytes + 3U) invalid("overlong GNU long name");
                auto& destination = type == 'L' ? long_name : long_link;
                if (destination) invalid("duplicate GNU long name header");
                destination = std::move(value);
            }
            continue;
        }
        if (long_name) name = *long_name;
        if (long_link) link = *long_link;
        if (long_name && fields.contains("path") && fields.at("path") != *long_name) {
            invalid("conflicting GNU and PAX member names");
        }
        if (long_link && fields.contains("linkpath") && fields.at("linkpath") != *long_link) {
            invalid("conflicting GNU and PAX link targets");
        }
        if (const auto found = fields.find("path"); found != fields.end()) name = found->second;
        if (const auto found = fields.find("linkpath"); found != fields.end()) link = found->second;
        const bool pax_sparse = !sparse.empty() ||
            std::any_of(fields.begin(), fields.end(), [](const auto& field) {
                return field.first.starts_with("GNU.sparse.");
            });
        if (const auto found = fields.find("GNU.sparse.name"); found != fields.end()) name = found->second;
        if (const auto found = fields.find("hdrcharset"); found != fields.end() &&
            found->second != "BINARY" && found->second != "ISO-IR 10646 2000 UTF-8") {
            invalid("unsupported PAX character set");
        }
        EntryKind kind;
        std::uint32_t type_mode = 0;
        switch (type) {
            case '\0': case '0': case '1': case '7': case 'S': kind = EntryKind::file; break;
            case '2': kind = EntryKind::symlink; break;
            case '5': case 'D': kind = EntryKind::directory; break;
            case '3': kind = EntryKind::other; type_mode = S_IFCHR; break;
            case '4': kind = EntryKind::other; type_mode = S_IFBLK; break;
            case '6': kind = EntryKind::other; type_mode = S_IFIFO; break;
            default: invalid("unsupported tar member type (multivolume or vendor extension)");
        }
        const bool regular_data = type == '\0' || type == '0' || type == '7' || type == 'S';
        if ((!regular_data && type != 'D' && size != 0U) || (pax_sparse && !regular_data)) {
            invalid("data or sparse map on a non-regular tar member");
        }
        const auto dumpdir = fields.find("GNU.dumpdir");
        if (dumpdir != fields.end()) {
            if (kind != EntryKind::directory || type == 'D') invalid("conflicting or misplaced PAX directory dump");
            validate_dumpdir(dumpdir->second, limits, cancellation);
            state.incremental = true;
        } else if (type == 'D') {
            if (size > limits.metadata_bytes) invalid("GNU directory dump exceeds the metadata byte limit");
            validate_dumpdir(bytes->bytes(body_offset, static_cast<std::size_t>(size), cancellation), limits, cancellation);
            state.incremental = true;
        }
        auto node = new_node(kind, state.next_object++);
        auto& entry = node.entry;
        if (type_mode != 0U) entry.mode = type_mode;
        const auto mode = nonnegative_tar_number(header.substr(100, 8));
        if (mode > 07777U) invalid("unsupported tar permission bits");
        entry.permissions = static_cast<fs::perms>(mode);
        entry.mode = (entry.mode & S_IFMT) | static_cast<std::uint32_t>(mode);
        entry.uid = uint32_number(nonnegative_tar_number(header.substr(108, 8)), "tar uid");
        entry.gid = uint32_number(nonnegative_tar_number(header.substr(116, 8)), "tar gid");
        entry.owner = text_field(header.substr(265, 32));
        entry.group = text_field(header.substr(297, 32));
        entry.modified = parse_time(std::to_string(tar_number(header.substr(136, 12))));
        entry.accessed = entry.modified;
        entry.changed = entry.modified;
        if (gnu) {
            entry.accessed = parse_time(std::to_string(tar_number(header.substr(345, 12))));
            entry.changed = parse_time(std::to_string(tar_number(header.substr(357, 12))));
            if (nonnegative_tar_number(header.substr(369, 12)) != 0U || !all_zero(header.substr(381, 5)) ||
                !all_zero(header.substr(495)) || (type != 'S' && !all_zero(header.substr(386, 109)))) {
                invalid("unsupported GNU multivolume or reserved metadata");
            }
        }
        node.device_major = uint32_number(nonnegative_tar_number(header.substr(329, 8)), "tar device major");
        node.device_minor = uint32_number(nonnegative_tar_number(header.substr(337, 8)), "tar device minor");
        if (kind != EntryKind::other && (node.device_major != 0U || node.device_minor != 0U)) {
            invalid("device number on a non-device tar member");
        }
        for (const auto& [key, value] : fields) {
            if (key == "uid") entry.uid = uint32_number(unsigned_number(value, 10, "PAX uid"), "PAX uid");
            else if (key == "gid") entry.gid = uint32_number(unsigned_number(value, 10, "PAX gid"), "PAX gid");
            else if (key == "uname") entry.owner = value;
            else if (key == "gname") entry.group = value;
            else if (key == "mtime") entry.modified = parse_time(value);
            else if (key == "atime") entry.accessed = parse_time(value);
            else if (key == "ctime") entry.changed = parse_time(value);
        }
        if (entry.owner.empty()) entry.owner = std::to_string(entry.uid);
        if (entry.group.empty()) entry.group = std::to_string(entry.gid);
        const auto key = member_path(name, kind == EntryKind::directory, limits);
        if (kind == EntryKind::symlink) {
            check_link_text(link, limits);
            entry.target = std::move(link);
        } else if (type == '1') {
            node.hardlink = member_path(link, false, limits);
        } else if (kind == EntryKind::file) {
            auto data_offset = body_offset;
            auto data_size = size;
            std::uint64_t logical = size;
            if (type == 'S') {
                logical = nonnegative_tar_number(header.substr(483, 12));
            } else if (pax_sparse) {
                if (fields.contains("GNU.sparse.major") || fields.contains("GNU.sparse.minor")) {
                    if (!fields.contains("GNU.sparse.major") || !fields.contains("GNU.sparse.minor") ||
                        fields.at("GNU.sparse.major") != "1" || fields.at("GNU.sparse.minor") != "0" ||
                        !fields.contains("GNU.sparse.realsize") || fields.contains("GNU.sparse.map") ||
                        fields.contains("GNU.sparse.size") || fields.contains("GNU.sparse.numblocks") ||
                        !sparse.empty()) invalid("unsupported or conflicting PAX sparse version");
                    logical = unsigned_number(fields.at("GNU.sparse.realsize"), 10, "sparse logical size");
                    auto map_end = body_offset;
                    const auto count = sparse_line(*bytes, map_end, body_offset + size, cancellation);
                    if (count > limits.sparse_extents) invalid("archive exceeds the sparse extent limit");
                    for (std::uint64_t index = 0; index < count; ++index) {
                        const auto start = sparse_line(*bytes, map_end, body_offset + size, cancellation);
                        const auto length = sparse_line(*bytes, map_end, body_offset + size, cancellation);
                        sparse.emplace_back(start, length);
                        if (map_end - body_offset > limits.metadata_bytes) invalid("sparse map exceeds the metadata byte limit");
                    }
                    const auto map_size = padded(map_end - body_offset, 512U);
                    if (map_size > size || !zero_range(*bytes, map_end,
                        map_size - (map_end - body_offset), cancellation)) invalid("invalid PAX sparse map padding");
                    data_offset += map_size;
                    data_size -= map_size;
                } else {
                    if (!fields.contains("GNU.sparse.size") || !fields.contains("GNU.sparse.numblocks") ||
                        fields.contains("GNU.sparse.realsize")) invalid("incomplete PAX sparse metadata");
                    logical = unsigned_number(fields.at("GNU.sparse.size"), 10, "sparse logical size");
                    if (const auto map = fields.find("GNU.sparse.map"); map != fields.end()) {
                        if (!sparse.empty()) invalid("conflicting PAX sparse maps");
                        const std::string_view text(map->second);
                        for (std::size_t begin = 0; begin < text.size();) {
                            const auto comma = text.find(',', begin);
                            if (comma == std::string_view::npos) invalid("unpaired PAX sparse map value");
                            const auto next = text.find(',', comma + 1U);
                            const auto start = unsigned_number(text.substr(begin, comma - begin), 10, "sparse offset");
                            const auto length = unsigned_number(text.substr(comma + 1U,
                                next == std::string_view::npos ? text.size() - comma - 1U : next - comma - 1U),
                                10, "sparse length");
                            if (sparse.size() >= limits.sparse_extents) invalid("archive exceeds the sparse extent limit");
                            sparse.emplace_back(start, length);
                            if (next == std::string_view::npos) break;
                            begin = next + 1U;
                            if (begin == text.size()) invalid("trailing comma in PAX sparse map");
                        }
                    }
                    if (sparse.size() != unsigned_number(fields.at("GNU.sparse.numblocks"), 10, "sparse block count")) {
                        invalid("PAX sparse map count mismatch");
                    }
                }
            }
            node.bytes = type == 'S' || pax_sparse
                ? sparse_content(bytes, sparse, logical, data_offset, data_size, limits)
                : content_range(bytes, body_offset, size, limits.sparse_extents, cancellation);
        }
        state.insert(key, std::move(node), limits);
        local.clear();
        local_sparse.clear();
        long_name.reset();
        long_link.reset();
        pending_extension = false;
    }
    invalid("missing tar end-of-archive blocks");
}

void resolve_hardlinks(ParseState& state, const Cancellation& cancellation) {
    for (auto& [key, node] : state.model) {
        cancellation.throw_if_requested();
        if (node.hardlink.empty()) continue;
        std::set<std::string, std::less<>> visited{key};
        auto target = node.hardlink;
        for (;;) {
            if (!visited.insert(target).second) invalid("cyclic tar hardlink");
            const auto found = state.model.find(target);
            if (found == state.model.end() ||
                (found->second.entry.kind != EntryKind::file && found->second.entry.kind != EntryKind::other)) {
                invalid("tar hardlink does not refer to a file or special archive member");
            }
            if (!found->second.hardlink.empty()) {
                target = found->second.hardlink;
                continue;
            }
            node.bytes = found->second.bytes;
            node.object = found->second.object;
            node.entry = found->second.entry;
            node.device_major = found->second.device_major;
            node.device_minor = found->second.device_minor;
            node.hardlink.clear();
            break;
        }
    }
}

std::optional<ArchiveFormat> cpio_format(const std::string_view bytes) {
    if (bytes.starts_with("070701")) return ArchiveFormat::cpio_newc;
    if (bytes.starts_with("070702")) return ArchiveFormat::cpio_crc;
    if (bytes.starts_with("070707")) return ArchiveFormat::cpio_odc;
    if (bytes.starts_with(std::string_view("\xc7\x71", 2))) return ArchiveFormat::cpio_binary_le;
    if (bytes.starts_with(std::string_view("\x71\xc7", 2))) return ArchiveFormat::cpio_binary_be;
    return std::nullopt;
}

std::uint32_t checksum_data(
    const Data& bytes, const std::uint64_t offset, const std::uint64_t length,
    const Cancellation& cancellation
) {
    std::uint32_t checksum = 0;
    archive_detail::storage_ranges(bytes.descriptor(), offset, length, [&](auto position, auto remaining) {
        while (remaining > 0U) {
            const auto block = bytes.bytes(position,
                static_cast<std::size_t>(std::min<std::uint64_t>(remaining, 64U * 1024U)), cancellation);
            if (block.empty()) invalid("truncated checksummed member");
            for (const unsigned char byte : block) checksum += byte;
            position += block.size();
            remaining -= block.size();
        }
    }, cancellation);
    return checksum;
}

std::uint32_t checksum_content(const Content& content, const Cancellation& cancellation) {
    std::uint32_t result = 0;
    for (const auto& extent : content.extents) {
        result += checksum_data(*content.data, extent.stored, extent.length, cancellation);
    }
    return result;
}

ParseState parse_cpio(
    const std::shared_ptr<const Data>& bytes, const ArchiveLimits& limits,
    const ArchiveFormat format, const Cancellation& cancellation
) {
    ParseState state;
    struct Group {
        std::uint64_t object{0};
        Entry metadata;
        std::shared_ptr<const Content> bytes;
        std::uint32_t device_major{0};
        std::uint32_t device_minor{0};
        std::uint64_t declared_links{0};
        std::uint64_t members{0};
    };
    using Identity = std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>;
    std::map<Identity, Group> groups;
    std::map<std::uint64_t, const Group*> groups_by_object;
    const bool modern = format == ArchiveFormat::cpio_newc || format == ArchiveFormat::cpio_crc;
    const bool crc = format == ArchiveFormat::cpio_crc;
    const bool odc = format == ArchiveFormat::cpio_odc;
    const std::uint64_t header_size = modern ? 110U : odc ? 76U : 26U;
    const std::uint64_t alignment = modern ? 4U : odc ? 1U : 2U;
    std::uint64_t offset = 0;
    while (offset <= bytes->size() && bytes->size() - offset >= header_size) {
        cancellation.throw_if_requested();
        const auto header_bytes = bytes->bytes(offset, static_cast<std::size_t>(header_size), cancellation);
        const std::string_view header(header_bytes);
        if (cpio_format(header) != format) invalid("unsupported or mixed cpio header format");
        std::array<std::uint64_t, 13> fields{};
        if (modern) {
            for (std::size_t index = 0; index < fields.size(); ++index) {
                fields[index] = unsigned_number(header.substr(6U + index * 8U, 8), 16, "cpio field");
            }
        } else if (odc) {
            const auto number = [&](const std::size_t start, const std::size_t length = 6U) {
                return unsigned_number(header.substr(start, length), 8, "odc cpio field");
            };
            const auto device = number(6);
            const auto rdevice = number(42);
            fields = {number(12), number(18), number(24), number(30), number(36), number(48, 11),
                      number(65, 11), device >> 8U, device & 255U, rdevice >> 8U, rdevice & 255U,
                      number(59), 0};
        } else {
            const bool little = format == ArchiveFormat::cpio_binary_le;
            const auto word = [&](const std::size_t index) {
                const auto first = static_cast<unsigned char>(header[index * 2U]);
                const auto second = static_cast<unsigned char>(header[index * 2U + 1U]);
                return static_cast<std::uint64_t>(little ? first | (second << 8U) : (first << 8U) | second);
            };
            const auto device = word(1);
            const auto rdevice = word(7);
            fields = {word(2), word(3), word(4), word(5), word(6), (word(8) << 16U) | word(9),
                      (word(11) << 16U) | word(12), device >> 8U, device & 255U,
                      rdevice >> 8U, rdevice & 255U, word(10), 0};
            if (fields[6] > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())) {
                invalid("V7 binary cpio member exceeds its signed 32-bit size range");
            }
        }
        const auto [ino, mode, uid, gid, links, mtime, size, devmajor, devminor, rdevmajor, rdevminor, name_size, checksum] =
            std::tuple{fields[0], fields[1], fields[2], fields[3], fields[4], fields[5], fields[6],
                       fields[7], fields[8], fields[9], fields[10], fields[11], fields[12]};
        offset += header_size;
        if (name_size < 1U || name_size > limits.path_bytes + 4U || name_size > bytes->size() - offset) {
            invalid("truncated or overlong cpio member name");
        }
        const auto raw_name = bytes->bytes(offset, static_cast<std::size_t>(name_size), cancellation);
        if (raw_name.back() != '\0' || raw_name.substr(0, raw_name.size() - 1U).find('\0') != std::string_view::npos) {
            invalid("invalid NUL in cpio member name");
        }
        const auto name = std::string_view(raw_name).substr(0, raw_name.size() - 1U);
        const auto data_offset = padded(offset + name_size, alignment);
        if (data_offset > bytes->size() ||
            !zero_range(*bytes, offset + name_size, data_offset - offset - name_size, cancellation)) {
            invalid("invalid cpio name padding");
        }
        offset = data_offset;
        if (size > limits.expanded_bytes || size > bytes->size() - offset) invalid("truncated or oversized cpio member");
        const auto body_offset = offset;
        const auto next_offset = padded(offset + size, alignment);
        if (next_offset > bytes->size() ||
            !zero_range(*bytes, offset + size, next_offset - offset - size, cancellation)) {
            invalid("invalid cpio data padding");
        }
        offset = next_offset;
        const auto actual_checksum = crc ? checksum_data(*bytes, body_offset, size, cancellation) : 0U;
        if (checksum != actual_checksum) invalid("cpio checksum mismatch");
        if (name == "TRAILER!!!") {
            if (size != 0U || !zero_range(*bytes, offset, bytes->size() - offset, cancellation)) {
                invalid("invalid cpio trailer or trailing archive data");
            }
            for (const auto& [identity, group] : groups) {
                static_cast<void>(identity);
                if (group.members > group.declared_links) invalid("cpio hardlink count exceeds its declared link count");
            }
            for (auto& [key, node] : state.model) {
                static_cast<void>(key);
                if (node.entry.kind != EntryKind::file && node.entry.kind != EntryKind::other) continue;
                if (const auto found = groups_by_object.find(node.object); found != groups_by_object.end()) {
                    const auto& group = *found->second;
                    node.bytes = group.bytes ? group.bytes : node.bytes;
                    node.entry = group.metadata;
                    node.device_major = group.device_major;
                    node.device_minor = group.device_minor;
                }
            }
            return state;
        }
        if (links == 0U || (mode & ~std::uint64_t{0177777}) != 0U) {
            invalid("unsupported cpio identity or mode");
        }
        EntryKind kind;
        switch (mode & S_IFMT) {
            case S_IFREG: kind = EntryKind::file; break;
            case S_IFDIR: kind = EntryKind::directory; break;
            case S_IFLNK: kind = EntryKind::symlink; break;
            case S_IFIFO: case S_IFCHR: case S_IFBLK: case S_IFSOCK: kind = EntryKind::other; break;
            default: invalid("unsupported cpio file type (PWB and vendor extensions are not V7 cpio)");
        }
        if ((kind == EntryKind::directory || kind == EntryKind::other) && size != 0U) {
            invalid("data on a cpio directory or special member");
        }
        if (kind == EntryKind::symlink && links != 1U) invalid("hardlinked cpio symlink is unsupported");
        auto node = new_node(kind, state.next_object++);
        auto& entry = node.entry;
        entry.permissions = static_cast<fs::perms>(mode & 07777U);
        entry.mode = static_cast<std::uint32_t>(mode);
        entry.uid = uint32_number(uid, "cpio uid");
        entry.gid = uint32_number(gid, "cpio gid");
        if (S_ISCHR(mode) || S_ISBLK(mode)) {
            node.device_major = uint32_number(rdevmajor, "cpio device major");
            node.device_minor = uint32_number(rdevminor, "cpio device minor");
        }
        entry.owner = std::to_string(uid);
        entry.group = std::to_string(gid);
        entry.modified = parse_time(std::to_string(mtime));
        entry.accessed = entry.modified;
        entry.changed = entry.modified;
        if (kind == EntryKind::symlink) {
            if (size > limits.path_bytes) invalid("overlong cpio symlink target");
            const auto body = bytes->bytes(body_offset, static_cast<std::size_t>(size), cancellation);
            check_link_text(body, limits);
            entry.target = body;
        } else if ((kind == EntryKind::file || kind == EntryKind::other) && links > 1U) {
            const Identity identity{devmajor, devminor, ino};
            auto [found, inserted] = groups.try_emplace(identity);
            auto& group = found->second;
            if (inserted) {
                group.object = node.object;
                group.metadata = entry;
                group.device_major = node.device_major;
                group.device_minor = node.device_minor;
                group.declared_links = links;
                groups_by_object.emplace(group.object, &group);
            } else if (group.metadata.mode != entry.mode || group.metadata.uid != entry.uid ||
                       group.metadata.gid != entry.gid || group.metadata.modified != entry.modified ||
                       group.declared_links != links || group.device_major != node.device_major ||
                       group.device_minor != node.device_minor) {
                invalid("inconsistent cpio hardlink metadata");
            }
            if (size != 0U) {
                auto body = content_range(bytes, body_offset, size, limits.sparse_extents, cancellation);
                if (group.bytes && !archive_detail::equal(*group.bytes, *body, cancellation)) {
                    invalid("conflicting cpio hardlink data");
                }
                group.bytes = std::move(body);
            }
            ++group.members;
            node.object = group.object;
        } else if (kind == EntryKind::file) {
            node.bytes = content_range(bytes, body_offset, size, limits.sparse_extents, cancellation);
        }
        state.insert(member_path(name, kind == EntryKind::directory, limits), std::move(node), limits);
    }
    invalid("missing cpio trailer");
}

class ArchiveOutput {
public:
    explicit ArchiveOutput(const std::uint64_t limit) : limit_(limit), bytes_(std::make_shared<Data>()) {}
    void append(const std::string_view value) {
        check(value.size());
        bytes_->append(value);
    }
    void zeros(const std::uint64_t count) {
        check(count);
        bytes_->resize(bytes_->size() + count);
    }
    void append(const Content& content, const Cancellation& cancellation) {
        check(content.size());
        const auto begin = bytes_->size();
        for (const auto& extent : content.extents) {
            archive_detail::copy_storage(content.data->descriptor(), *bytes_, extent.stored,
                extent.length, begin + extent.offset, cancellation);
        }
        bytes_->resize(begin + content.size());
    }
    void stored(const Content& content, const Cancellation& cancellation) {
        for (const auto& extent : content.extents) {
            check(extent.length);
            archive_detail::copy_storage(content.data->descriptor(), *bytes_, extent.stored,
                extent.length, bytes_->size(), cancellation);
        }
    }
    void align(const std::uint64_t alignment) { zeros(padded(bytes_->size(), alignment) - bytes_->size()); }
    [[nodiscard]] std::shared_ptr<const Data> finish() && { return std::move(bytes_); }
private:
    void check(const std::uint64_t count) const {
        if (count > limit_ - bytes_->size()) invalid("serialized archive exceeds the expanded byte limit");
    }
    std::uint64_t limit_;
    std::shared_ptr<Data> bytes_;
};

void tar_octal(std::string& header, const std::size_t offset, const std::size_t width, std::uint64_t number) {
    std::string field(width, '0');
    field.back() = '\0';
    for (std::size_t index = width - 1U; index > 0U;) {
        --index;
        field[index] = static_cast<char>('0' + (number & 7U));
        number >>= 3U;
    }
    if (number != 0U) invalid("tar header number is out of range");
    header.replace(offset, width, field);
}

std::string tar_header(
    const std::string_view name, const char type, const std::uint64_t size,
    const std::uint32_t permissions, const std::uint32_t device_major = 0,
    const std::uint32_t device_minor = 0
) {
    if (name.size() > 100U) invalid("internal tar header name is too long");
    std::string header(512, '\0');
    header.replace(0, name.size(), name);
    tar_octal(header, 100, 8, permissions & 07777U);
    tar_octal(header, 108, 8, 0);
    tar_octal(header, 116, 8, 0);
    tar_octal(header, 124, 12, size <= 077777777777ULL ? size : 0U);
    tar_octal(header, 136, 12, 0);
    header.replace(148, 8, 8, ' ');
    header[156] = type;
    header.replace(257, 6, std::string_view("ustar\0", 6));
    header.replace(263, 2, "00");
    const auto device_field = [&](const std::size_t offset, const std::uint32_t number) {
        if (number <= 07777777U) tar_octal(header, offset, 8, number);
        else {
            std::uint64_t remaining = number;
            for (std::size_t index = 8; index > 0;) {
                --index;
                header[offset + index] = static_cast<char>(remaining & 255U);
                remaining >>= 8U;
            }
            header[offset] = static_cast<char>(static_cast<unsigned char>(header[offset]) | 0x80U);
        }
    };
    device_field(329, device_major);
    device_field(337, device_minor);
    std::uint64_t checksum = 0;
    for (const unsigned char byte : header) checksum += byte;
    tar_octal(header, 148, 7, checksum);
    header[155] = ' ';
    return header;
}

std::string pax_record(const std::string_view key, const std::string_view value) {
    const auto body = std::string(key) + "=" + std::string(value) + "\n";
    auto length = body.size() + 2U;
    for (;;) {
        const auto number = std::to_string(length);
        const auto next = number.size() + 1U + body.size();
        if (next == length) return number + " " + body;
        length = next;
    }
}

std::string archive_member_name(const std::string& key, const bool directory) {
    if (key == "/") return "./";
    return "./" + key.substr(1) + (directory ? "/" : "");
}

std::shared_ptr<const Data> serialize_tar(
    const Model& model, const ArchiveLimits& limits,
    const Cancellation& cancellation
) {
    ArchiveOutput output(limits.expanded_bytes);
    std::map<std::uint64_t, std::string> first_links;
    for (const auto& [key, node] : model) {
        cancellation.throw_if_requested();
        const auto& entry = node.entry;
        char type = entry.kind == EntryKind::directory ? '5' :
                    entry.kind == EntryKind::symlink ? '2' : '0';
        if (entry.kind == EntryKind::other) {
            if (S_ISFIFO(entry.mode)) type = '6';
            else if (S_ISCHR(entry.mode)) type = '3';
            else if (S_ISBLK(entry.mode)) type = '4';
            else invalid("tar cannot represent this special file type");
        }
        std::string target = entry.target;
        if (entry.kind == EntryKind::file || entry.kind == EntryKind::other) {
            const auto [first, inserted] = first_links.emplace(node.object, key);
            if (!inserted) {
                type = '1';
                target = archive_member_name(first->second, false);
            }
        }
        const auto name = archive_member_name(key, entry.kind == EntryKind::directory);
        std::string pax = pax_record("hdrcharset", "BINARY") + pax_record("path", name) +
                          pax_record("uid", std::to_string(entry.uid)) +
                          pax_record("gid", std::to_string(entry.gid)) +
                          pax_record("uname", entry.owner) + pax_record("gname", entry.group) +
                          pax_record("mtime", time_text(entry.modified)) +
                          pax_record("atime", time_text(entry.accessed)) +
                          pax_record("ctime", time_text(entry.changed));
        if (type == '1' || type == '2') pax += pax_record("linkpath", target);
        const bool sparse = type == '0' && node.bytes->sparse();
        std::string map;
        auto size = type == '0' ? node.bytes->size() : 0U;
        if (sparse) {
            map = std::to_string(node.bytes->extents.size()) + "\n";
            for (const auto& extent : node.bytes->extents) {
                map += std::to_string(extent.offset) + "\n" + std::to_string(extent.length) + "\n";
                if (map.size() > limits.metadata_bytes) invalid("sparse map exceeds the metadata byte limit");
            }
            size = padded(map.size(), 512U) + node.bytes->stored_size();
            pax += pax_record("GNU.sparse.major", "1") + pax_record("GNU.sparse.minor", "0") +
                   pax_record("GNU.sparse.name", name) + pax_record("GNU.sparse.realsize", std::to_string(node.bytes->size()));
        }
        if (size > 077777777777ULL) pax += pax_record("size", std::to_string(size));
        if (pax.size() > limits.metadata_bytes) invalid("tar extension exceeds the metadata byte limit");
        output.append(tar_header("PaxHeaders/mcpp", 'x', pax.size(), 0600));
        output.append(pax);
        output.align(512);
        output.append(tar_header("mcpp-member", type, size, static_cast<std::uint32_t>(entry.permissions),
            type == '3' || type == '4' ? node.device_major : 0U,
            type == '3' || type == '4' ? node.device_minor : 0U));
        if (sparse) {
            output.append(map);
            output.align(512);
            output.stored(*node.bytes, cancellation);
        } else if (size != 0U) output.append(*node.bytes, cancellation);
        output.align(512);
    }
    output.zeros(1024);
    return std::move(output).finish();
}

std::string hexadecimal(const std::uint64_t number) {
    if (number > std::numeric_limits<std::uint32_t>::max()) invalid("cpio field exceeds 32 bits");
    std::array<char, 8> buffer{};
    std::string result(8, '0');
    const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), number, 16);
    if (converted.ec != std::errc{}) invalid("cannot encode cpio field");
    const auto size = static_cast<std::size_t>(converted.ptr - buffer.data());
    result.replace(result.size() - size, size, buffer.data(), size);
    return result;
}

void cpio_record(
    ArchiveOutput& output, const std::string_view name, const std::string_view data,
    const Content* content, const std::array<std::uint64_t, 13>& fields,
    const ArchiveFormat format, const Cancellation& cancellation
) {
    const bool modern = format == ArchiveFormat::cpio_newc || format == ArchiveFormat::cpio_crc;
    const bool odc = format == ArchiveFormat::cpio_odc;
    std::string header;
    if (modern) {
        header = format == ArchiveFormat::cpio_crc ? "070702" : "070701";
        for (const auto field : fields) header += hexadecimal(field);
    } else {
        if (fields[9] > 0x3ffffffU || fields[10] > 255U) invalid("device number exceeds the old cpio range");
        const auto device = (fields[9] << 8U) | fields[10];
        const std::array<std::uint64_t, 10> legacy{
            0, fields[0], fields[1], fields[2], fields[3], fields[4], device, fields[5], fields[11], fields[6]
        };
        if (odc) {
            header = "070707";
            for (std::size_t index = 0; index < legacy.size(); ++index) {
                const std::size_t width = index == 7U || index == 9U ? 11U : 6U;
                std::array<char, 24> buffer{};
                const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), legacy[index], 8);
                const auto count = static_cast<std::size_t>(converted.ptr - buffer.data());
                if (converted.ec != std::errc{} || count > width) invalid("member metadata exceeds the odc cpio field range");
                header.append(width - count, '0');
                header.append(buffer.data(), count);
            }
        } else {
            if (fields[6] > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()) ||
                fields[5] > std::numeric_limits<std::uint32_t>::max()) invalid("member exceeds the V7 binary cpio range");
            const std::array<std::uint64_t, 13> words{
                070707, 0, fields[0], fields[1], fields[2], fields[3], fields[4], device,
                fields[5] >> 16U, fields[5] & 65535U, fields[11], fields[6] >> 16U, fields[6] & 65535U
            };
            for (const auto word : words) {
                if (word > 65535U) invalid("member metadata exceeds the V7 binary cpio field range");
                const auto low = static_cast<char>(word & 255U);
                const auto high = static_cast<char>(word >> 8U);
                header.push_back(format == ArchiveFormat::cpio_binary_le ? low : high);
                header.push_back(format == ArchiveFormat::cpio_binary_le ? high : low);
            }
        }
    }
    output.append(header);
    output.append(name);
    output.zeros(1);
    const auto alignment = modern ? 4U : odc ? 1U : 2U;
    output.align(alignment);
    if (content != nullptr) output.append(*content, cancellation);
    else output.append(data);
    output.align(alignment);
}

std::shared_ptr<const Data> serialize_cpio(
    const Model& model, const ArchiveFormat format, const ArchiveLimits& limits,
    const Cancellation& cancellation
) {
    ArchiveOutput output(limits.expanded_bytes);
    struct FileInfo {
        std::uint64_t inode{0};
        std::uint64_t links{0};
        std::string last;
    };
    std::map<std::uint64_t, FileInfo> files;
    std::uint64_t next_inode = 1;
    for (const auto& [key, node] : model) {
        if (node.entry.kind != EntryKind::file && node.entry.kind != EntryKind::other) continue;
        auto& information = files[node.object];
        if (information.inode == 0U) information.inode = next_inode++;
        ++information.links;
        information.last = key;
    }
    const bool crc = format == ArchiveFormat::cpio_crc;
    const bool modern = format == ArchiveFormat::cpio_newc || crc;
    for (const auto& [key, node] : model) {
        cancellation.throw_if_requested();
        const auto& entry = node.entry;
        const auto name = archive_member_name(key, false);
        std::string_view data;
        const Content* content = nullptr;
        auto inode = next_inode++;
        std::uint64_t links = entry.kind == EntryKind::directory ? 2U : 1U;
        if (entry.kind == EntryKind::file || entry.kind == EntryKind::other) {
            const auto& information = files.at(node.object);
            inode = information.inode;
            links = information.links;
            if (entry.kind == EntryKind::file && (!modern || key == information.last)) content = node.bytes.get();
        } else if (entry.kind == EntryKind::symlink) {
            data = entry.target;
        }
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(entry.modified.time_since_epoch()).count();
        if (seconds < 0) {
            invalid("cpio cannot represent negative modification times");
        }
        std::uint32_t checksum = 0;
        if (crc) {
            if (content != nullptr) checksum = checksum_content(*content, cancellation);
            else for (const unsigned char byte : data) checksum += byte;
        }
        cpio_record(output, name, data, content, {
            inode, entry.mode, entry.uid, entry.gid, links, static_cast<std::uint64_t>(seconds),
            content != nullptr ? content->size() : data.size(), 0, 0,
            node.device_major, node.device_minor, name.size() + 1U, checksum
        }, format, cancellation);
    }
    cpio_record(output, "TRAILER!!!", {}, nullptr, {0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 11, 0}, format, cancellation);
    output.align(512);
    return std::move(output).finish();
}

ArchiveCompression detect_compression(const std::string_view bytes) {
    if (bytes.starts_with(std::string_view("\x1f\x8b", 2))) return ArchiveCompression::gzip;
    if (bytes.starts_with("BZh")) return ArchiveCompression::bzip2;
    if (bytes.starts_with(std::string_view("\xfd\x37\x7a\x58\x5a\x00", 6))) return ArchiveCompression::xz;
    if (bytes.starts_with(std::string_view("\x28\xb5\x2f\xfd", 4))) return ArchiveCompression::zstd;
    return ArchiveCompression::none;
}

std::string compression_tool(const ArchiveCompression compression) {
    const char* name = nullptr;
    switch (compression) {
        case ArchiveCompression::gzip: name = "gzip"; break;
        case ArchiveCompression::bzip2: name = "bzip2"; break;
        case ArchiveCompression::xz: name = "xz"; break;
        case ArchiveCompression::zstd: name = "zstd"; break;
        case ArchiveCompression::none: invalid("no compression tool was requested");
    }
    for (const auto* directory : {"/usr/bin", "/bin"}) {
        const auto path = fs::path(directory) / name;
        if (::access(path.c_str(), X_OK) == 0) return path.string();
    }
    invalid(std::string("required compression tool is unavailable: ") + name);
}

class DataSink final : public std::ostream {
public:
    DataSink(Data& data, const std::uint64_t limit) : std::ostream(nullptr), buffer_(data, limit) {
        rdbuf(&buffer_);
        exceptions(std::ios::badbit | std::ios::failbit);
    }
private:
    class Buffer final : public std::streambuf {
    public:
        Buffer(Data& data, const std::uint64_t limit) : data_(data), limit_(limit) {}
    protected:
        std::streamsize xsputn(const char* bytes, const std::streamsize count) override {
            if (count < 0 || static_cast<std::uint64_t>(count) > limit_ - data_.size()) {
                invalid("streamed archive exceeds its configured byte limit");
            }
            data_.append({bytes, static_cast<std::size_t>(count)});
            return count;
        }
        int_type overflow(const int_type byte) override {
            if (!traits_type::eq_int_type(byte, traits_type::eof())) {
                const char value = traits_type::to_char_type(byte);
                xsputn(&value, 1);
            }
            return traits_type::not_eof(byte);
        }
    private:
        Data& data_;
        std::uint64_t limit_;
    };
    Buffer buffer_;
};

std::shared_ptr<const Data> convert_compression(
    const ArchiveCompression compression, const bool decompress,
    const Data& input, const std::uint64_t limit, const Cancellation& cancellation
) {
    const auto tool = compression_tool(compression);
    std::vector<std::string> arguments{tool};
    if (decompress && compression == ArchiveCompression::xz) {
        arguments.push_back("--memlimit-decompress=268435456");
    }
    if (decompress && compression == ArchiveCompression::zstd) {
        arguments.push_back("--memory=268435456");
    }
    if (!decompress && compression == ArchiveCompression::gzip) arguments.push_back("-n");
    arguments.push_back(decompress ? "-dc" : "-c");
    arguments.push_back("--");
    arguments.push_back(input.path());
    auto data = std::make_shared<Data>();
    DataSink output(*data, limit);
    const auto result = capture_command_to(output, arguments, ".", cancellation);
    if (result.status != 0) {
        invalid(tool + (decompress ? " decompression" : " compression") +
                " failed (" + std::to_string(result.status) + "): " + result.error);
    }
    output.flush();
    return data;
}

std::shared_ptr<const Data> read_archive(
    Provider& source, const std::string& path, const Entry& expected,
    const std::uint64_t limit, const Cancellation& cancellation
) {
    if (expected.kind != EntryKind::file) invalid("the archive must be a regular file, not a symlink");
    if (expected.size > limit) invalid("archive exceeds the physical byte limit");
    std::shared_ptr<Data> bytes;
    if (const auto* local = dynamic_cast<const LocalProvider*>(&source)) {
        const auto native = local->native_path(path);
        Descriptor input(::open(native.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
        check_system(input.get(), "open archive snapshot source");
        struct stat before{};
        struct stat named{};
        check_system(::fstat(input.get(), &before), "inspect archive snapshot source");
        check_system(::lstat(native.c_str(), &named), "inspect archive snapshot name");
        if (!S_ISREG(before.st_mode) || before.st_size < 0 ||
            before.st_ino != expected.inode || static_cast<std::uint64_t>(before.st_size) != expected.size ||
            before.st_dev != named.st_dev || before.st_ino != named.st_ino ||
            source.stat(path, cancellation).revision != expected.revision) {
            invalid("archive changed before its snapshot was captured");
        }
        bytes = std::make_shared<Data>();
        archive_detail::copy_storage(input.get(), *bytes, 0, expected.size, 0, cancellation);
        struct stat after{};
        check_system(::fstat(input.get(), &after), "reinspect archive snapshot source");
        if (before.st_dev != after.st_dev || before.st_ino != after.st_ino ||
            before.st_size != after.st_size || before.st_mode != after.st_mode ||
            before.st_uid != after.st_uid || before.st_gid != after.st_gid || before.st_nlink != after.st_nlink ||
            before.st_mtim.tv_sec != after.st_mtim.tv_sec || before.st_mtim.tv_nsec != after.st_mtim.tv_nsec ||
            before.st_ctim.tv_sec != after.st_ctim.tv_sec || before.st_ctim.tv_nsec != after.st_ctim.tv_nsec) {
            invalid("archive changed while its snapshot was captured");
        }
    } else {
        bytes = viewer_detail::capture(expected.size, [&](const auto offset, const auto length,
            std::ostream& output, const Cancellation& stop) { source.read(path, offset, length, output, stop); }, cancellation);
    }
    const auto current = dynamic_cast<const LocalProvider*>(&source) != nullptr
        ? source.stat(path, cancellation) : source.stat_follow(path, cancellation);
    if (current.revision != expected.revision || bytes->size() != expected.size) {
        invalid("archive changed while it was being read");
    }
    return bytes;
}

std::pair<std::uint32_t, std::string> owner_identity(const std::string& name, const bool owner) {
    if (name.empty() || name.find('\0') != std::string::npos) invalid("empty or NUL-containing owner/group");
    std::uint32_t numeric = 0;
    const auto number = std::from_chars(name.data(), name.data() + name.size(), numeric);
    if (number.ec == std::errc{} && number.ptr == name.data() + name.size()) return {numeric, name};
    const auto requested = ::sysconf(owner ? _SC_GETPW_R_SIZE_MAX : _SC_GETGR_R_SIZE_MAX);
    std::vector<char> buffer(static_cast<std::size_t>(requested > 0 ? requested : 16384));
    if (owner) {
        passwd information{};
        passwd* result = nullptr;
        const auto status = ::getpwnam_r(name.c_str(), &information, buffer.data(), buffer.size(), &result);
        if (status != 0) throw std::system_error(status, std::generic_category(), "look up archive owner");
        if (result == nullptr) invalid("unknown archive owner: " + name);
        return {static_cast<std::uint32_t>(information.pw_uid), name};
    }
    group information{};
    group* result = nullptr;
    const auto status = ::getgrnam_r(name.c_str(), &information, buffer.data(), buffer.size(), &result);
    if (status != 0) throw std::system_error(status, std::generic_category(), "look up archive group");
    if (result == nullptr) invalid("unknown archive group: " + name);
    return {static_cast<std::uint32_t>(information.gr_gid), name};
}

}  // namespace

class ArchiveProvider::Impl {
public:
    Impl(ProviderPtr provider, std::string path, ArchiveOpenOptions settings, const Cancellation& cancellation)
        : options(std::move(settings)), source(std::move(provider)) {
        const auto& bounds = options.limits;
        if (bounds.archive_bytes == 0U || bounds.expanded_bytes < 1024U ||
            bounds.members == 0U || bounds.members > (std::numeric_limits<std::size_t>::max() - 16U) / 4U ||
            bounds.path_bytes == 0U || bounds.path_bytes > 65536U ||
            bounds.metadata_bytes < 512U || bounds.metadata_bytes > 64U * 1024U * 1024U ||
            bounds.sparse_extents == 0U || bounds.sparse_extents > 1000000U ||
            bounds.archive_bytes > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) ||
            bounds.expanded_bytes > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
            invalid("invalid archive limits");
        }
        if (!source || path.empty() || path.find('\0') != std::string::npos) invalid("invalid archive source");
        if (options.draft_directory.empty() || options.draft_directory.native().find('\0') != std::string::npos) {
            invalid("invalid archive draft directory");
        }
        options.draft_directory = fs::absolute(options.draft_directory);
        source_path = source->normalize(path);
        const auto* native = dynamic_cast<const LocalProvider*>(source.get());
        if (native != nullptr) file = native->native_path(source_path);
        else if (options.writable) invalid("nested and non-local archives are read-only");
        const auto initial = native != nullptr ? source->stat(source_path, cancellation)
                                               : source->stat_follow(source_path, cancellation);
        if (initial.kind != EntryKind::file || initial.size > bounds.archive_bytes) {
            invalid("archive source is not a regular file within its physical byte limit");
        }
        if (const auto* archive = dynamic_cast<const ArchiveProvider*>(source.get())) {
            std::shared_ptr<const Content> content;
            {
                const std::lock_guard lock(archive->impl_->mutex);
                const auto key = require_path(archive->impl_->model, archive->normalize(source_path), true);
                content = archive->impl_->model.at(key).bytes;
                if (!content) invalid("nested archive source is not a regular file");
            }
            auto snapshot = std::make_shared<Data>();
            for (const auto& extent : content->extents) {
                archive_detail::copy_storage(content->data->descriptor(), *snapshot, extent.stored,
                    extent.length, extent.offset, cancellation);
            }
            snapshot->resize(content->size());
            if (source->stat_follow(source_path, cancellation).revision != initial.revision) {
                invalid("nested archive changed while it was being captured");
            }
            source_bytes = std::move(snapshot);
        } else source_bytes = read_archive(*source, source_path, initial, bounds.archive_bytes, cancellation);
        revision = initial.revision;
        const auto identity_part = [](const std::string& value) { return std::to_string(value.size()) + ":" + value; };
        identity = "archive:" + identity_part(source->id()) + identity_part(source_path) + identity_part(initial.revision);
        entry_revision_base = initial.revision;
        compression = detect_compression(source_bytes->bytes(0, 6, cancellation));
        std::shared_ptr<const Data> expanded;
        if (compression != ArchiveCompression::none) {
            expanded = convert_compression(compression, true, *source_bytes, bounds.expanded_bytes, cancellation);
        } else {
            if (source_bytes->size() > bounds.expanded_bytes) invalid("archive exceeds the expanded byte limit");
            expanded = source_bytes;
        }
        ParseState parsed;
        if (const auto detected = cpio_format(expanded->bytes(0, 6, cancellation))) {
            format = *detected;
            parsed = parse_cpio(expanded, bounds, format, cancellation);
        } else {
            format = ArchiveFormat::tar;
            parsed = parse_tar(expanded, bounds, cancellation);
            resolve_hardlinks(parsed, cancellation);
        }
        validate_model(parsed.model, bounds, cancellation);
        // Restore-only dumpdir names and renames must not be lost by a normal archive rewrite.
        if (options.writable && parsed.incremental) invalid("GNU incremental archives are read-only");
        model = std::move(parsed.model);
        update_links();
        next_object = parsed.next_object;
        if (options.writable) {
            if (initial.links != 1U) invalid("writable archive has multiple host hardlinks");
            local.ensure_writable(file.string());
        }
    }

    void writable() const {
        if (!options.writable) invalid("archive is open read-only");
    }
    void member_writable(const std::string& key) const {
        const auto& member = model.at(key).entry;
        if (member.kind != EntryKind::file) path_error(key, std::errc::invalid_argument);
        constexpr auto write_bits = fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write;
        if (member.permissions == fs::perms::unknown || (member.permissions & write_bits) == fs::perms::none) {
            path_error(key, std::errc::permission_denied);
        }
    }
    [[nodiscard]] std::string normalized(const std::string_view path) const {
        return virtual_path(path, options.limits);
    }
    [[nodiscard]] std::string destination(const Model& in, const std::string_view path) const {
        const auto key = normalized(path);
        if (key == "/") invalid("cannot replace the archive root");
        const auto parent = require_path(in, parent_key(key), true);
        if (in.at(parent).entry.kind != EntryKind::directory) path_error(parent, std::errc::not_a_directory);
        return parent == "/" ? "/" + base_key(key) : parent + "/" + base_key(key);
    }
    [[nodiscard]] Entry entry(const std::string& key) const {
        const auto& node = model.at(key);
        auto result = node.entry;
        result.path = key;
        result.name = base_key(key);
        result.hidden = !result.name.empty() && result.name.front() == '.';
        result.inode = node.object;
        result.revision = entry_revision_base + "#" + std::to_string(node.object) + ":" + std::to_string(node.version);
        if (result.kind == EntryKind::file) {
            result.size = node.bytes->size();
            result.links = link_counts.at(node.object);
        } else if (result.kind == EntryKind::other) {
            result.size = 0;
            result.links = link_counts.at(node.object);
        } else if (result.kind == EntryKind::symlink) {
            result.size = result.target.size();
            const auto target = resolve(model, key, true);
            result.target_is_directory = target && model.at(*target).entry.kind == EntryKind::directory;
        }
        return result;
    }
    void update_links() {
        link_counts.clear();
        for (const auto& [key, node] : model) {
            static_cast<void>(key);
            if (node.entry.kind == EntryKind::file || node.entry.kind == EntryKind::other) ++link_counts[node.object];
        }
    }

    void publish(Model candidate, const Cancellation& cancellation) {
        writable();
        cancellation.throw_if_requested();
        if (format != ArchiveFormat::tar) {
            for (auto& [key, node] : candidate) {
                static_cast<void>(key);
                node.entry.modified = Time{std::chrono::duration_cast<std::chrono::seconds>(
                    node.entry.modified.time_since_epoch()
                )};
                node.entry.accessed = node.entry.modified;
                node.entry.changed = node.entry.modified;
                node.entry.owner = std::to_string(node.entry.uid);
                node.entry.group = std::to_string(node.entry.gid);
            }
        }
        validate_model(candidate, options.limits, cancellation);
        auto raw = format == ArchiveFormat::tar ? serialize_tar(candidate, options.limits, cancellation)
                                               : serialize_cpio(candidate, format, options.limits, cancellation);
        DraftDirectory draft(options.draft_directory, true);
        last_draft = draft.path("raw");
        draft.write("raw", *raw, cancellation);
        std::shared_ptr<const Data> encoded;
        if (compression == ArchiveCompression::none) {
            if (raw->size() > options.limits.archive_bytes) invalid("new archive exceeds the physical byte limit; draft: " + last_draft->string());
            encoded = std::move(raw);
        } else {
            encoded = convert_compression(compression, false, *raw, options.limits.archive_bytes, cancellation);
            draft.write("candidate", *encoded, cancellation);
            last_draft = draft.path("candidate");
        }
        cancellation.throw_if_requested();
        Descriptor lock(::open(file.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
        check_system(lock.get(), "open archive publication lock");
        check_system(::flock(lock.get(), LOCK_EX | LOCK_NB), "lock archive publication");
        const auto current = local.stat(file.string(), cancellation);
        if (current.kind != EntryKind::file || current.links != 1U || current.revision != revision) {
            invalid("archive changed since opening; retained draft: " + last_draft->string());
        }
        if (!archive_detail::equal(*read_archive(local, file.string(), current, options.limits.archive_bytes, cancellation),
                                  *source_bytes, cancellation)) {
            invalid("archive content changed since opening; retained draft: " + last_draft->string());
        }
        std::ifstream input(encoded->path(), std::ios::binary);
        if (!input) invalid("cannot open encoded archive snapshot for publication");
        local.write(file.string(), input, encoded->size(), true, revision, cancellation, {});
        const auto saved = local.stat(file.string(), {});
        if (!archive_detail::equal(*read_archive(local, file.string(), saved, options.limits.archive_bytes, {}),
                                  *encoded, {})) {
            invalid("archive changed immediately after publication; retained draft: " + last_draft->string());
        }
        Descriptor parent(::open(file.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        check_system(parent.get(), "open archive directory for publication sync");
        check_system(::fsync(parent.get()), "sync published archive directory");
        model = std::move(candidate);
        update_links();
        source_bytes = std::move(encoded);
        revision = saved.revision;
        draft.discard();
        last_draft.reset();
    }

    mutable std::mutex mutex;
    LocalProvider local;
    ArchiveOpenOptions options;
    ProviderPtr source;
    std::string source_path;
    std::string identity;
    fs::path file;
    ArchiveFormat format{ArchiveFormat::tar};
    ArchiveCompression compression{ArchiveCompression::none};
    Model model;
    std::map<std::uint64_t, std::uint64_t> link_counts;
    std::uint64_t next_object{1};
    std::string revision;
    std::string entry_revision_base;
    std::shared_ptr<const Data> source_bytes;
    std::optional<fs::path> last_draft;
};

ArchiveProvider::ArchiveProvider(
    fs::path archive, ArchiveOpenOptions options, const Cancellation& cancellation
) : ArchiveProvider(std::make_shared<LocalProvider>(), fs::absolute(archive).string(),
                    std::move(options), cancellation) {
    if (archive.empty()) invalid("invalid archive filename");
}

ArchiveProvider::ArchiveProvider(
    ProviderPtr source, std::string path, ArchiveOpenOptions options, const Cancellation& cancellation
) : impl_(std::make_unique<Impl>(std::move(source), std::move(path), std::move(options), cancellation)) {}

ArchiveProvider::~ArchiveProvider() = default;

fs::path ArchiveProvider::archive_path() const { return impl_->file; }
ProviderPtr ArchiveProvider::source_provider() const { return impl_->source; }
std::string ArchiveProvider::source_path() const { return impl_->source_path; }
ArchiveFormat ArchiveProvider::format() const { return impl_->format; }
ArchiveCompression ArchiveProvider::compression() const { return impl_->compression; }
std::optional<fs::path> ArchiveProvider::last_draft() const {
    const std::lock_guard lock(impl_->mutex);
    return impl_->last_draft;
}

std::uint64_t ArchiveProvider::hardlink_count(const std::string_view path) const {
    const std::lock_guard lock(impl_->mutex);
    const auto key = require_path(impl_->model, normalize(path), true);
    const auto& node = impl_->model.at(key);
    if (node.entry.kind != EntryKind::file && node.entry.kind != EntryKind::other) {
        path_error(key, std::errc::invalid_argument);
    }
    return impl_->link_counts.at(node.object);
}

void ArchiveProvider::ensure_writable(const std::string_view path) const {
    const std::lock_guard lock(impl_->mutex);
    impl_->writable();
    const auto key = require_path(impl_->model, normalize(path), true);
    impl_->member_writable(key);
    impl_->local.ensure_writable(impl_->file.string());
}

void ArchiveProvider::validate_directory_target(
    const std::string_view source, const ArchiveProvider& destination_provider,
    const std::string_view destination
) const {
    const auto validate = [&] {
        if (impl_->file.empty() || destination_provider.impl_->file.empty()) {
            if (impl_->identity != destination_provider.impl_->identity) return;
        } else if (impl_->file != destination_provider.impl_->file) return;
        const auto from = require_path(impl_->model, normalize(source), true);
        if (impl_->model.at(from).entry.kind != EntryKind::directory) return;
        const auto requested = destination_provider.normalize(destination);
        const auto existing = resolve(destination_provider.impl_->model, requested, true);
        const auto to = existing ? *existing :
            destination_provider.impl_->destination(destination_provider.impl_->model, requested);
        if (from == "/" || from == to || to.starts_with(from + "/")) {
            invalid("cannot copy or move an archive directory into itself");
        }
    };
    if (impl_.get() == destination_provider.impl_.get()) {
        const std::lock_guard lock(impl_->mutex);
        validate();
    } else {
        const std::scoped_lock lock(impl_->mutex, destination_provider.impl_->mutex);
        validate();
    }
}

std::string ArchiveProvider::id() const { return impl_->identity; }
std::string ArchiveProvider::display_name() const {
    return (impl_->file.empty() ? impl_->source->display_name() + ":" + impl_->source_path
                               : impl_->file.string()) + "!";
}
ProviderCapability ArchiveProvider::capabilities() const {
    return impl_->options.writable
        ? ProviderCapability::read | ProviderCapability::write | ProviderCapability::remove |
          ProviderCapability::rename | ProviderCapability::direct_copy | ProviderCapability::links |
          ProviderCapability::permissions
        : ProviderCapability::read;
}
std::string ArchiveProvider::normalize(const std::string_view path) const { return impl_->normalized(path); }
std::string ArchiveProvider::join(const std::string_view directory, const std::string_view name) const {
    if (name.empty() || name.find('/') != std::string_view::npos || name == "." || name == "..") {
        invalid("join requires one literal member name");
    }
    const auto base = normalize(directory);
    return normalize((base == "/" ? base : base + "/") + std::string(name));
}
std::string ArchiveProvider::parent(const std::string_view path) const { return parent_key(normalize(path)); }
std::string ArchiveProvider::basename(const std::string_view path) const { return base_key(normalize(path)); }

std::vector<Entry> ArchiveProvider::list(
    const std::string_view path, const ListRequest& request, const Cancellation& cancellation
) const {
    const std::lock_guard lock(impl_->mutex);
    cancellation.throw_if_requested();
    const auto key = require_path(impl_->model, normalize(path), true);
    if (impl_->model.at(key).entry.kind != EntryKind::directory) path_error(key, std::errc::not_a_directory);
    std::vector<Entry> entries;
    for (const auto& [member, node] : impl_->model) {
        cancellation.throw_if_requested();
        static_cast<void>(node);
        if (member != "/" && parent_key(member) == key) {
            auto item = impl_->entry(member);
            if (request.show_hidden || !item.hidden) entries.push_back(std::move(item));
        }
    }
    sort_entries(entries, request);
    const auto begin = std::min(request.offset, entries.size());
    const auto size = request.limit == 0U ? entries.size() - begin : std::min(request.limit, entries.size() - begin);
    return {std::make_move_iterator(entries.begin() + static_cast<std::ptrdiff_t>(begin)),
            std::make_move_iterator(entries.begin() + static_cast<std::ptrdiff_t>(begin + size))};
}
Entry ArchiveProvider::stat(const std::string_view path, const Cancellation& cancellation) const {
    const std::lock_guard lock(impl_->mutex);
    cancellation.throw_if_requested();
    return impl_->entry(require_path(impl_->model, normalize(path), false));
}
Entry ArchiveProvider::stat_follow(const std::string_view path, const Cancellation& cancellation) const {
    const std::lock_guard lock(impl_->mutex);
    cancellation.throw_if_requested();
    return impl_->entry(require_path(impl_->model, normalize(path), true));
}
bool ArchiveProvider::exists(const std::string_view path) const {
    const std::lock_guard lock(impl_->mutex);
    return resolve(impl_->model, normalize(path), false).has_value();
}
SpaceInfo ArchiveProvider::space(const std::string_view path) const {
    const std::lock_guard lock(impl_->mutex);
    static_cast<void>(require_path(impl_->model, normalize(path), true));
    return impl_->source->space(impl_->source_path);
}
void ArchiveProvider::read(
    const std::string_view path, const std::uint64_t offset, const std::uint64_t length,
    std::ostream& output, const Cancellation& cancellation
) const {
    std::unique_lock lock(impl_->mutex);
    cancellation.throw_if_requested();
    const auto key = require_path(impl_->model, normalize(path), true);
    const auto& node = impl_->model.at(key);
    if (node.entry.kind != EntryKind::file) path_error(key, std::errc::invalid_argument);
    const auto snapshot = node.bytes;
    lock.unlock();
    snapshot->read(offset, length, output, cancellation);
}

bool ArchiveProvider::copy_to_local(
    const std::string_view source, LocalProvider& destination_provider,
    const std::string_view destination, const bool overwrite, const bool preserve_permissions,
    const Cancellation& cancellation, const ProgressCallback& progress,
    const std::optional<std::string> expected_source_revision,
    const std::optional<std::string> expected_destination_revision
) const {
    cancellation.throw_if_requested();
    Node snapshot;
    {
        const std::lock_guard lock(impl_->mutex);
        const auto key = require_path(impl_->model, normalize(source), true);
        if (expected_source_revision && impl_->entry(key).revision != *expected_source_revision) {
            invalid("member changed before copying: " + key);
        }
        snapshot = impl_->model.at(key);
    }
    const auto& entry = snapshot.entry;
    if (entry.kind != EntryKind::file && entry.kind != EntryKind::other) return false;
    if (S_ISSOCK(entry.mode)) invalid("cannot copy Unix socket: " + std::string(source));
    if (entry.kind == EntryKind::file && preserve_permissions &&
        (entry.permissions & fs::perms::owner_read) == fs::perms::none) return false;
    DraftDirectory directory(impl_->options.draft_directory, false);
    if (entry.kind == EntryKind::file) {
        Descriptor output(directory.create("snapshot"));
        snapshot.bytes->write_to(output.get(), cancellation);
        check_system(::fsync(output.get()), "sync archive member snapshot");
    } else {
        if (!S_ISFIFO(entry.mode) && !S_ISCHR(entry.mode) && !S_ISBLK(entry.mode)) {
            invalid("unsupported special archive member: " + std::string(source));
        }
        const auto device = makedev(snapshot.device_major, snapshot.device_minor);
        if (major(device) != snapshot.device_major || minor(device) != snapshot.device_minor) {
            invalid("archive device number exceeds the host device range");
        }
        directory.special(entry.mode, device);
    }
    auto metadata = entry;
    if (!preserve_permissions && entry.kind == EntryKind::file) metadata.permissions = fs::perms{0600};
    directory.metadata(metadata);
    LocalProvider local;
    destination_provider.copy_file_from(local, directory.anchored("snapshot").string(), destination,
        overwrite, preserve_permissions, cancellation, progress, LocalProvider::CopyMode::replace,
        std::nullopt, expected_destination_revision);
    return true;
}

void ArchiveProvider::write(
    const std::string_view path, std::istream& input, const std::optional<std::uint64_t> length,
    const bool overwrite, const std::optional<std::string> expected_revision,
    const Cancellation& cancellation, const ProgressCallback& progress
) {
    write_impl(path, input, length, overwrite, expected_revision, cancellation, progress, false);
}

void ArchiveProvider::write_preserving_links(
    const std::string_view path, std::istream& input, const std::optional<std::uint64_t> length,
    const std::optional<std::string> expected_revision, const Cancellation& cancellation
) {
    write_impl(path, input, length, true, expected_revision, cancellation, {}, true);
}

void ArchiveProvider::write_impl(
    const std::string_view path, std::istream& input, const std::optional<std::uint64_t> length,
    const bool overwrite, const std::optional<std::string> expected_revision,
    const Cancellation& cancellation, const ProgressCallback& progress, const bool preserve_links
) {
    std::unique_lock lock(impl_->mutex);
    impl_->writable();
    cancellation.throw_if_requested();
    const auto key = preserve_links
        ? require_path(impl_->model, normalize(path), true)
        : impl_->destination(impl_->model, path);
    const auto old = impl_->model.find(key);
    if (old != impl_->model.end()) {
        if (!overwrite) path_error(key, std::errc::file_exists);
        if (old->second.entry.kind == EntryKind::directory) path_error(key, std::errc::is_a_directory);
        if (old->second.entry.kind == EntryKind::file) impl_->member_writable(key);
    }
    impl_->local.ensure_writable(impl_->file.string());
    if (expected_revision && (old == impl_->model.end() || impl_->entry(key).revision != *expected_revision)) {
        invalid("member changed since opening: " + key);
    }
    const auto revision = impl_->revision;
    if (length && *length > impl_->options.limits.expanded_bytes) invalid("member exceeds the expanded byte limit");
    auto bytes = std::make_shared<Data>();
    std::array<char, 64U * 1024U> buffer{};
    while (input) {
        cancellation.throw_if_requested();
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count <= 0) {
            if (input.bad() || (!input.eof() && input.fail())) invalid("failed reading archive member input");
            break;
        }
        const auto size = static_cast<std::size_t>(count);
        if (size > impl_->options.limits.expanded_bytes - bytes->size()) invalid("member exceeds the expanded byte limit");
        bytes->append({buffer.data(), size});
        if (progress) {
            lock.unlock();
            progress({bytes->size(), length.value_or(0), 0, 1, key});
            lock.lock();
            if (impl_->revision != revision) invalid("archive view changed during the progress callback");
        }
        cancellation.throw_if_requested();
    }
    if (input.bad() || (!input.eof() && input.fail())) invalid("failed reading archive member input");
    if (length && *length != bytes->size()) invalid("member input length does not match its declared length");
    const auto replacement = content_range(bytes, 0, bytes->size(), impl_->options.limits.sparse_extents, cancellation);
    auto candidate = impl_->model;
    if (preserve_links) {
        const auto object = old->second.object;
        const auto modified = Clock::now();
        for (auto& [name, member] : candidate) {
            cancellation.throw_if_requested();
            static_cast<void>(name);
            if (member.entry.kind != EntryKind::file || member.object != object) continue;
            member.bytes = replacement;
            member.entry.modified = modified;
            member.entry.changed = modified;
            ++member.version;
        }
    } else {
        auto node = new_node(EntryKind::file, impl_->next_object++);
        if (old != impl_->model.end() && old->second.entry.kind == EntryKind::file) {
            node.entry = old->second.entry;
            node.entry.modified = Clock::now();
            node.entry.changed = node.entry.modified;
        }
        node.bytes = replacement;
        candidate.insert_or_assign(key, std::move(node));
    }
    impl_->publish(std::move(candidate), cancellation);
}

void ArchiveProvider::create_directory(const std::string_view path) {
    const std::lock_guard lock(impl_->mutex);
    impl_->writable();
    const auto key = impl_->destination(impl_->model, path);
    if (impl_->model.contains(key)) path_error(key, std::errc::file_exists);
    auto candidate = impl_->model;
    candidate.emplace(key, new_node(EntryKind::directory, impl_->next_object++));
    impl_->publish(std::move(candidate), {});
}

void ArchiveProvider::remove(
    const std::string_view path, const bool recursive,
    const Cancellation& cancellation, const ProgressCallback& progress
) {
    std::unique_lock lock(impl_->mutex);
    impl_->writable();
    cancellation.throw_if_requested();
    const auto key = require_path(impl_->model, normalize(path), false);
    if (key == "/") invalid("cannot remove the archive root");
    auto candidate = impl_->model;
    std::uint64_t removed = 0;
    for (auto item = candidate.begin(); item != candidate.end();) {
        cancellation.throw_if_requested();
        if (item->first == key || item->first.starts_with(key + "/")) {
            if (item->first != key && !recursive) path_error(key, std::errc::directory_not_empty);
            item = candidate.erase(item);
            ++removed;
        } else {
            ++item;
        }
    }
    if (progress) {
        const auto revision = impl_->revision;
        lock.unlock();
        progress({0, 0, 0, removed, key});
        lock.lock();
        if (impl_->revision != revision) invalid("archive view changed during the progress callback");
    }
    impl_->publish(std::move(candidate), cancellation);
}

void ArchiveProvider::rename(
    const std::string_view source, const std::string_view destination, const bool overwrite
) {
    const std::lock_guard lock(impl_->mutex);
    impl_->writable();
    const auto from = require_path(impl_->model, normalize(source), false);
    const auto to = impl_->destination(impl_->model, destination);
    if (from == "/") invalid("cannot rename the archive root");
    if (from == to) return;
    if (to.starts_with(from + "/")) invalid("cannot move a directory into itself");
    const auto target = impl_->model.find(to);
    if (target != impl_->model.end()) {
        if (!overwrite) path_error(to, std::errc::file_exists);
        const bool directory = impl_->model.at(from).entry.kind == EntryKind::directory;
        if (directory != (target->second.entry.kind == EntryKind::directory)) {
            invalid("cannot rename between a directory and a non-directory");
        }
        if (!directory && target->second.entry.kind == EntryKind::file &&
            impl_->model.at(from).entry.kind == EntryKind::file &&
            target->second.object == impl_->model.at(from).object) return;
        const auto child = impl_->model.lower_bound(to + "/");
        if (directory && child != impl_->model.end() && child->first.starts_with(to + "/")) {
            path_error(to, std::errc::directory_not_empty);
        }
    }
    auto candidate = impl_->model;
    candidate.erase(to);
    for (const auto& [key, node] : impl_->model) {
        if (key == from || key.starts_with(from + "/")) {
            candidate.erase(key);
            candidate.insert_or_assign(to + key.substr(from.size()), node);
        }
    }
    impl_->publish(std::move(candidate), {});
}

bool ArchiveProvider::copy_direct(
    const std::string_view source, const std::string_view destination, const bool overwrite,
    const Cancellation& cancellation, const ProgressCallback& progress
) {
    std::unique_lock lock(impl_->mutex);
    impl_->writable();
    cancellation.throw_if_requested();
    const auto from = require_path(impl_->model, normalize(source), false);
    const auto to = impl_->destination(impl_->model, destination);
    if (from == "/" || from == to || to.starts_with(from + "/")) invalid("cannot copy a directory into itself");
    if (impl_->model.at(from).entry.kind == EntryKind::directory) {
        invalid("direct archive directory copy is unsupported; use OperationEngine's recursive copy");
    }
    const auto target = impl_->model.find(to);
    if (target != impl_->model.end()) {
        if (!overwrite) path_error(to, std::errc::file_exists);
        if (target->second.entry.kind == EntryKind::directory) path_error(to, std::errc::is_a_directory);
    }
    auto node = impl_->model.at(from);
    node.object = impl_->next_object++;
    auto candidate = impl_->model;
    candidate.insert_or_assign(to, std::move(node));
    if (progress) {
        const auto revision = impl_->revision;
        const auto size = impl_->entry(from).size;
        lock.unlock();
        progress({0, size, 0, 1, from});
        lock.lock();
        if (impl_->revision != revision) invalid("archive view changed during the progress callback");
    }
    impl_->publish(std::move(candidate), cancellation);
    return true;
}

void ArchiveProvider::set_permissions(const std::string_view path, const fs::perms permissions) {
    const std::lock_guard lock(impl_->mutex);
    impl_->writable();
    const auto bits = static_cast<unsigned int>(permissions);
    if ((bits & ~07777U) != 0U) invalid("unknown or unsupported archive permission bits");
    const auto key = require_path(impl_->model, normalize(path), false);
    if (impl_->model.at(key).entry.kind == EntryKind::symlink) invalid("symlink permissions cannot be changed");
    auto candidate = impl_->model;
    const auto object = candidate.at(key).object;
    for (auto& [member, node] : candidate) {
        static_cast<void>(member);
        if (node.object != object) continue;
        node.entry.permissions = permissions;
        node.entry.mode = (node.entry.mode & S_IFMT) | bits;
        node.entry.changed = Clock::now();
        ++node.version;
    }
    impl_->publish(std::move(candidate), {});
}

void ArchiveProvider::set_owner(
    const std::string_view path, const std::optional<std::string> owner,
    const std::optional<std::string> group
) {
    const std::lock_guard lock(impl_->mutex);
    impl_->writable();
    const auto key = require_path(impl_->model, normalize(path), false);
    const auto uid = owner ? std::optional{owner_identity(*owner, true)} : std::nullopt;
    const auto gid = group ? std::optional{owner_identity(*group, false)} : std::nullopt;
    if (!uid && !gid) return;
    auto candidate = impl_->model;
    const auto object = candidate.at(key).object;
    for (auto& [member, node] : candidate) {
        static_cast<void>(member);
        if (node.object != object) continue;
        if (uid) {
            node.entry.uid = uid->first;
            node.entry.owner = uid->second;
        }
        if (gid) {
            node.entry.gid = gid->first;
            node.entry.group = gid->second;
        }
        node.entry.changed = Clock::now();
        ++node.version;
    }
    impl_->publish(std::move(candidate), {});
}

void ArchiveProvider::create_symlink(
    const std::string_view target, const std::string_view path, const bool overwrite,
    const Cancellation& cancellation
) {
    const std::lock_guard lock(impl_->mutex);
    impl_->writable();
    cancellation.throw_if_requested();
    check_link_text(target, impl_->options.limits);
    const auto key = impl_->destination(impl_->model, path);
    const auto old = impl_->model.find(key);
    if (old != impl_->model.end()) {
        if (!overwrite) path_error(key, std::errc::file_exists);
        if (old->second.entry.kind == EntryKind::directory) path_error(key, std::errc::is_a_directory);
    }
    auto node = new_node(EntryKind::symlink, impl_->next_object++);
    node.entry.target = std::string(target);
    auto candidate = impl_->model;
    candidate.insert_or_assign(key, std::move(node));
    impl_->publish(std::move(candidate), cancellation);
}

void ArchiveProvider::create_hardlink(const std::string_view target, const std::string_view path) {
    copy_hardlink(target, path, false, {});
}

void ArchiveProvider::copy_hardlink(
    const std::string_view source, const std::string_view destination,
    const bool overwrite, const Cancellation& cancellation
) {
    const std::lock_guard lock(impl_->mutex);
    impl_->writable();
    cancellation.throw_if_requested();
    const auto from = require_path(impl_->model, normalize(source), false);
    if (impl_->model.at(from).entry.kind != EntryKind::file &&
        impl_->model.at(from).entry.kind != EntryKind::other) {
        invalid("hardlinks require a regular or special archive member");
    }
    const auto to = impl_->destination(impl_->model, destination);
    const auto existing = impl_->model.find(to);
    if (existing != impl_->model.end()) {
        if (!overwrite) path_error(to, std::errc::file_exists);
        if (existing->second.entry.kind == EntryKind::directory) path_error(to, std::errc::is_a_directory);
        if (existing->second.entry.kind == EntryKind::file) {
            impl_->member_writable(to);
            if (existing->second.object == impl_->model.at(from).object) return;
        }
    }
    auto candidate = impl_->model;
    candidate.insert_or_assign(to, impl_->model.at(from));
    impl_->publish(std::move(candidate), cancellation);
}

void ArchiveProvider::set_modified(const std::string_view path, const Time modified) {
    const std::lock_guard lock(impl_->mutex);
    impl_->writable();
    if (impl_->format != ArchiveFormat::tar &&
        std::chrono::duration_cast<std::chrono::seconds>(modified.time_since_epoch()) != modified.time_since_epoch()) {
        invalid("cpio modification times have one-second resolution");
    }
    const auto key = require_path(impl_->model, normalize(path), false);
    auto candidate = impl_->model;
    const auto object = candidate.at(key).object;
    for (auto& [member, node] : candidate) {
        static_cast<void>(member);
        if (node.object != object) continue;
        node.entry.modified = modified;
        node.entry.changed = Clock::now();
        ++node.version;
    }
    impl_->publish(std::move(candidate), {});
}

bool is_archive_filename(const std::string_view name) noexcept {
    for (const auto suffix : {
        ".tar", ".tgz", ".tar.gz", ".tar.bz2", ".tbz", ".tbz2", ".tar.xz", ".txz", ".tar.zst", ".tzst",
        ".cpio", ".cpio.gz", ".cpio.bz2", ".cpio.xz", ".cpio.zst"
    }) {
        if (name.ends_with(suffix)) return true;
    }
    return false;
}

std::shared_ptr<ArchiveProvider> open_archive_provider(
    const fs::path& archive, ArchiveOpenOptions options, const Cancellation& cancellation
) {
    return std::make_shared<ArchiveProvider>(archive, std::move(options), cancellation);
}

std::shared_ptr<ArchiveProvider> open_archive_provider(
    ProviderPtr source, std::string path, ArchiveOpenOptions options, const Cancellation& cancellation
) {
    return std::make_shared<ArchiveProvider>(std::move(source), std::move(path), std::move(options), cancellation);
}

}  // namespace mcpp
