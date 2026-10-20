#include "mcpp/archive_provider.hpp"

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
#include <system_error>
#include <tuple>
#include <unistd.h>
#include <utility>

namespace mcpp {
namespace {

using Clock = std::chrono::system_clock;
using Time = Clock::time_point;
namespace fs = std::filesystem;

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
    void write(
        const char* name, const std::string_view bytes,
        const Cancellation& cancellation
    ) const {
        Descriptor output(::openat(
            directory_.get(), name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600
        ));
        check_system(output.get(), "create archive draft");
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            cancellation.throw_if_requested();
            const auto count = ::write(
                output.get(), bytes.data() + offset,
                std::min<std::size_t>(bytes.size() - offset, 64U * 1024U)
            );
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) throw std::system_error(errno, std::generic_category(), "write archive draft");
            offset += static_cast<std::size_t>(count);
        }
        check_system(::fsync(output.get()), "sync archive draft");
        check_system(::fsync(directory_.get()), "sync archive draft entry");
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
    std::shared_ptr<const std::string> bytes;
    std::string hardlink;
    bool implicit{false};
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
    if (kind == EntryKind::file) node.bytes = std::make_shared<const std::string>();
    return node;
}

struct ParseState {
    Model model;
    std::uint64_t next_object{1};
    std::set<std::string, std::less<>> explicit_names;
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
    std::size_t total = 0;
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
        }
    }
}

using Pax = std::map<std::string, std::string, std::less<>>;

Pax parse_pax(const std::string_view bytes) {
    static const std::set<std::string_view> supported{
        "path", "linkpath", "size", "uid", "gid", "uname", "gname",
        "mtime", "atime", "ctime", "hdrcharset"
    };
    Pax result;
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
        if (value.find('\0') != std::string::npos) invalid("NUL in PAX metadata");
        if (!result.emplace(std::string(key), std::move(value)).second) invalid("duplicate PAX key");
        offset += static_cast<std::size_t>(length);
    }
    return result;
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

std::size_t padded(const std::size_t size, const std::size_t alignment) {
    if (size > std::numeric_limits<std::size_t>::max() - (alignment - 1U)) invalid("archive size overflow");
    return (size + alignment - 1U) / alignment * alignment;
}

ParseState parse_tar(
    const std::string_view bytes, const ArchiveLimits& limits,
    const Cancellation& cancellation
) {
    ParseState state;
    Pax global;
    Pax local;
    std::optional<std::string> long_name;
    std::optional<std::string> long_link;
    bool pending_extension = false;
    std::size_t records = 0;
    std::size_t offset = 0;
    while (offset <= bytes.size() && bytes.size() - offset >= 512U) {
        cancellation.throw_if_requested();
        const auto header = bytes.substr(offset, 512);
        if (all_zero(header)) {
            if (pending_extension || bytes.size() - offset < 1024U || !all_zero(bytes.substr(offset))) {
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
                if (value.empty()) fields.erase(key);
                else fields.insert_or_assign(key, value);
            }
            if (const auto found = fields.find("size"); found != fields.end()) size = unsigned_number(found->second, 10, "PAX size");
        }
        offset += 512U;
        if (size > limits.expanded_bytes || size > bytes.size() - offset) invalid("truncated or oversized tar member");
        const auto member_size = static_cast<std::size_t>(size);
        const auto body = bytes.substr(offset, member_size);
        const auto stored_size = padded(member_size, 512U);
        if (stored_size > bytes.size() - offset) invalid("truncated tar padding");
        if (!all_zero(bytes.substr(offset + member_size, stored_size - member_size))) invalid("nonzero tar member padding");
        offset += stored_size;
        if (extension) {
            pending_extension = true;
            if (type == 'x') {
                if (!local.empty()) invalid("stacked local PAX headers");
                local = parse_pax(body);
            } else if (type == 'g') {
                for (auto& [key, value] : parse_pax(body)) {
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
        if (const auto found = fields.find("hdrcharset"); found != fields.end() &&
            found->second != "BINARY" && found->second != "ISO-IR 10646 2000 UTF-8") {
            invalid("unsupported PAX character set");
        }
        EntryKind kind;
        switch (type) {
            case '\0': case '0': case '1': kind = EntryKind::file; break;
            case '2': kind = EntryKind::symlink; break;
            case '5': kind = EntryKind::directory; break;
            default: invalid("unsupported tar member type (special, sparse, or incremental file)");
        }
        if (type != '\0' && type != '0' && size != 0U) invalid("data on a non-regular tar member");
        auto node = new_node(kind, state.next_object++);
        auto& entry = node.entry;
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
            if (!all_zero(header.substr(386, 109))) invalid("unsupported GNU sparse or incremental metadata");
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
            node.bytes = std::make_shared<const std::string>(body);
        }
        state.insert(key, std::move(node), limits);
        local.clear();
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
            if (found == state.model.end() || found->second.entry.kind != EntryKind::file) {
                invalid("tar hardlink does not refer to a regular archive member");
            }
            if (!found->second.hardlink.empty()) {
                target = found->second.hardlink;
                continue;
            }
            node.bytes = found->second.bytes;
            node.object = found->second.object;
            node.entry = found->second.entry;
            node.hardlink.clear();
            break;
        }
    }
}

ParseState parse_cpio(
    const std::string_view bytes, const ArchiveLimits& limits,
    ArchiveFormat& format, const Cancellation& cancellation
) {
    ParseState state;
    struct Group {
        std::uint64_t object{0};
        Entry metadata;
        std::shared_ptr<const std::string> bytes;
        std::uint64_t declared_links{0};
        std::uint64_t members{0};
    };
    using Identity = std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>;
    std::map<Identity, Group> groups;
    std::map<std::uint64_t, const Group*> groups_by_object;
    std::size_t offset = 0;
    bool first = true;
    while (offset <= bytes.size() && bytes.size() - offset >= 110U) {
        cancellation.throw_if_requested();
        const auto header = bytes.substr(offset, 110);
        const bool crc = header.substr(0, 6) == "070702";
        if (!crc && header.substr(0, 6) != "070701") invalid("unsupported or mixed cpio header format");
        if (first) {
            format = crc ? ArchiveFormat::cpio_crc : ArchiveFormat::cpio_newc;
            first = false;
        } else if (crc != (format == ArchiveFormat::cpio_crc)) invalid("mixed newc/CRC cpio archive");
        std::array<std::uint64_t, 13> fields{};
        for (std::size_t index = 0; index < fields.size(); ++index) {
            fields[index] = unsigned_number(header.substr(6U + index * 8U, 8), 16, "cpio field");
        }
        const auto [ino, mode, uid, gid, links, mtime, size, devmajor, devminor, rdevmajor, rdevminor, name_size, checksum] =
            std::tuple{fields[0], fields[1], fields[2], fields[3], fields[4], fields[5], fields[6],
                       fields[7], fields[8], fields[9], fields[10], fields[11], fields[12]};
        offset += 110U;
        if (name_size < 1U || name_size > limits.path_bytes + 4U || name_size > bytes.size() - offset) {
            invalid("truncated or overlong cpio member name");
        }
        const auto raw_name = bytes.substr(offset, static_cast<std::size_t>(name_size));
        if (raw_name.back() != '\0' || raw_name.substr(0, raw_name.size() - 1U).find('\0') != std::string_view::npos) {
            invalid("invalid NUL in cpio member name");
        }
        const auto name = raw_name.substr(0, raw_name.size() - 1U);
        const auto data_offset = padded(offset + static_cast<std::size_t>(name_size), 4);
        if (data_offset > bytes.size() || !all_zero(bytes.substr(offset + static_cast<std::size_t>(name_size),
                                                               data_offset - offset - static_cast<std::size_t>(name_size)))) {
            invalid("invalid cpio name padding");
        }
        offset = data_offset;
        if (size > limits.expanded_bytes || size > bytes.size() - offset) invalid("truncated or oversized cpio member");
        const auto body = bytes.substr(offset, static_cast<std::size_t>(size));
        const auto next_offset = padded(offset + static_cast<std::size_t>(size), 4);
        if (next_offset > bytes.size() ||
            !all_zero(bytes.substr(offset + static_cast<std::size_t>(size), next_offset - offset - static_cast<std::size_t>(size)))) {
            invalid("invalid cpio data padding");
        }
        offset = next_offset;
        std::uint32_t actual_checksum = 0;
        if (crc) for (const unsigned char byte : body) actual_checksum += byte;
        if (checksum != actual_checksum) invalid("cpio checksum mismatch");
        if (name == "TRAILER!!!") {
            if (size != 0U || !all_zero(bytes.substr(offset))) invalid("invalid cpio trailer or trailing archive data");
            for (const auto& [identity, group] : groups) {
                static_cast<void>(identity);
                if (group.members > group.declared_links) invalid("cpio hardlink count exceeds its declared link count");
            }
            for (auto& [key, node] : state.model) {
                static_cast<void>(key);
                if (node.entry.kind != EntryKind::file) continue;
                if (const auto found = groups_by_object.find(node.object); found != groups_by_object.end()) {
                    const auto& group = *found->second;
                    node.bytes = group.bytes ? group.bytes : std::make_shared<const std::string>();
                    node.entry = group.metadata;
                }
            }
            return state;
        }
        if (links == 0U || rdevmajor != 0U || rdevminor != 0U || (mode & ~std::uint64_t{0177777}) != 0U) {
            invalid("unsupported cpio identity or mode");
        }
        EntryKind kind;
        switch (mode & S_IFMT) {
            case S_IFREG: kind = EntryKind::file; break;
            case S_IFDIR: kind = EntryKind::directory; break;
            case S_IFLNK: kind = EntryKind::symlink; break;
            default: invalid("unsupported cpio special file");
        }
        if (kind == EntryKind::directory && size != 0U) invalid("data on cpio directory");
        if (kind == EntryKind::symlink && links != 1U) invalid("hardlinked cpio symlink is unsupported");
        auto node = new_node(kind, state.next_object++);
        auto& entry = node.entry;
        entry.permissions = static_cast<fs::perms>(mode & 07777U);
        entry.mode = static_cast<std::uint32_t>(mode);
        entry.uid = static_cast<std::uint32_t>(uid);
        entry.gid = static_cast<std::uint32_t>(gid);
        entry.owner = std::to_string(uid);
        entry.group = std::to_string(gid);
        entry.modified = parse_time(std::to_string(mtime));
        entry.accessed = entry.modified;
        entry.changed = entry.modified;
        if (kind == EntryKind::symlink) {
            check_link_text(body, limits);
            entry.target = std::string(body);
        } else if (kind == EntryKind::file && links > 1U) {
            const Identity identity{devmajor, devminor, ino};
            auto [found, inserted] = groups.try_emplace(identity);
            auto& group = found->second;
            if (inserted) {
                group.object = node.object;
                group.metadata = entry;
                group.declared_links = links;
                groups_by_object.emplace(group.object, &group);
            } else if (group.metadata.mode != entry.mode || group.metadata.uid != entry.uid ||
                       group.metadata.gid != entry.gid || group.metadata.modified != entry.modified ||
                       group.declared_links != links) {
                invalid("inconsistent cpio hardlink metadata");
            }
            if (!body.empty()) {
                if (group.bytes && *group.bytes != body) invalid("conflicting cpio hardlink data");
                group.bytes = std::make_shared<const std::string>(body);
            }
            ++group.members;
            node.object = group.object;
        } else if (kind == EntryKind::file) {
            node.bytes = std::make_shared<const std::string>(body);
        }
        state.insert(member_path(name, kind == EntryKind::directory, limits), std::move(node), limits);
    }
    invalid("missing cpio trailer");
}

class ArchiveOutput {
public:
    explicit ArchiveOutput(const std::size_t limit) : limit_(limit) {}
    void append(const std::string_view value) {
        if (value.size() > limit_ - bytes_.size()) invalid("serialized archive exceeds the expanded byte limit");
        bytes_.append(value);
    }
    void zeros(const std::size_t count) {
        if (count > limit_ - bytes_.size()) invalid("serialized archive exceeds the expanded byte limit");
        bytes_.append(count, '\0');
    }
    void align(const std::size_t alignment) { zeros(padded(bytes_.size(), alignment) - bytes_.size()); }
    [[nodiscard]] std::string finish() && { return std::move(bytes_); }
private:
    std::size_t limit_;
    std::string bytes_;
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
    const std::string_view name, const char type, const std::size_t size,
    const std::uint32_t permissions
) {
    if (name.size() > 100U) invalid("internal tar header name is too long");
    std::string header(512, '\0');
    header.replace(0, name.size(), name);
    tar_octal(header, 100, 8, permissions & 07777U);
    tar_octal(header, 108, 8, 0);
    tar_octal(header, 116, 8, 0);
    tar_octal(header, 124, 12, size);
    tar_octal(header, 136, 12, 0);
    header.replace(148, 8, 8, ' ');
    header[156] = type;
    header.replace(257, 6, std::string_view("ustar\0", 6));
    header.replace(263, 2, "00");
    tar_octal(header, 329, 8, 0);
    tar_octal(header, 337, 8, 0);
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

std::string serialize_tar(
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
        std::string target = entry.target;
        if (entry.kind == EntryKind::file) {
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
        output.append(tar_header("PaxHeaders/mcpp", 'x', pax.size(), 0600));
        output.append(pax);
        output.align(512);
        const auto size = type == '0' ? node.bytes->size() : 0U;
        output.append(tar_header("mcpp-member", type, size, static_cast<std::uint32_t>(entry.permissions)));
        if (size != 0U) output.append(*node.bytes);
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
    const std::array<std::uint64_t, 13>& fields, const bool crc
) {
    output.append(crc ? "070702" : "070701");
    for (const auto field : fields) output.append(hexadecimal(field));
    output.append(name);
    output.zeros(1);
    output.align(4);
    output.append(data);
    output.align(4);
}

std::string serialize_cpio(
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
        if (node.entry.kind != EntryKind::file) continue;
        auto& information = files[node.object];
        if (information.inode == 0U) information.inode = next_inode++;
        ++information.links;
        information.last = key;
    }
    const bool crc = format == ArchiveFormat::cpio_crc;
    for (const auto& [key, node] : model) {
        cancellation.throw_if_requested();
        const auto& entry = node.entry;
        const auto name = archive_member_name(key, false);
        std::string_view data;
        auto inode = next_inode++;
        std::uint64_t links = entry.kind == EntryKind::directory ? 2U : 1U;
        if (entry.kind == EntryKind::file) {
            const auto& information = files.at(node.object);
            inode = information.inode;
            links = information.links;
            if (key == information.last) data = *node.bytes;
        } else if (entry.kind == EntryKind::symlink) {
            data = entry.target;
        }
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(entry.modified.time_since_epoch()).count();
        if (seconds < 0 || static_cast<std::uint64_t>(seconds) > std::numeric_limits<std::uint32_t>::max()) {
            invalid("cpio modification time exceeds its unsigned 32-bit range");
        }
        std::uint32_t checksum = 0;
        if (crc) for (const unsigned char byte : data) checksum += byte;
        cpio_record(output, name, data, {
            inode, entry.mode, entry.uid, entry.gid, links, static_cast<std::uint64_t>(seconds),
            data.size(), 0, 0, 0, 0, name.size() + 1U, checksum
        }, crc);
    }
    cpio_record(output, "TRAILER!!!", {}, {0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 11, 0}, crc);
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

std::string convert_compression(
    const ArchiveCompression compression, const bool decompress,
    const DraftDirectory& directory, const std::string_view input,
    const std::size_t limit, const Cancellation& cancellation
) {
    const auto tool = compression_tool(compression);
    std::vector<std::string> arguments{tool};
    if (decompress && compression == ArchiveCompression::xz) {
        arguments.push_back("--memlimit-decompress=" + std::to_string(limit));
    }
    if (decompress && compression == ArchiveCompression::zstd) {
        arguments.push_back("--memory=" + std::to_string(limit));
    }
    arguments.push_back(decompress ? "-dc" : "-c");
    const auto result = capture_command(
        arguments, directory.anchored({}), cancellation,
        limit, directory.anchored(input)
    );
    if (result.status != 0) {
        invalid(tool + (decompress ? " decompression" : " compression") +
                " failed (" + std::to_string(result.status) + "): " + result.error);
    }
    return result.output;
}

std::string read_archive(
    LocalProvider& local, const fs::path& path, const Entry& expected,
    const std::size_t limit, const Cancellation& cancellation
) {
    if (expected.kind != EntryKind::file) invalid("the archive must be a regular file, not a symlink");
    if (expected.size > limit) invalid("archive exceeds the physical byte limit");
    std::ostringstream output(std::ios::binary);
    local.read(path.string(), 0, expected.size + 1U, output, cancellation);
    auto bytes = std::move(output).str();
    if (bytes.size() != expected.size || local.stat(path.string(), cancellation).revision != expected.revision) {
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
    Impl(fs::path archive, ArchiveOpenOptions settings, const Cancellation& cancellation)
        : options(std::move(settings)) {
        const auto& bounds = options.limits;
        if (bounds.archive_bytes == 0U || bounds.expanded_bytes < 1024U ||
            bounds.members == 0U || bounds.members > (std::numeric_limits<std::size_t>::max() - 16U) / 4U ||
            bounds.path_bytes == 0U || bounds.path_bytes > 65536U ||
            bounds.archive_bytes >= static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max()) ||
            bounds.expanded_bytes >= static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
            invalid("invalid archive limits");
        }
        if (archive.empty() || archive.native().find('\0') != std::string::npos) invalid("invalid archive filename");
        if (options.draft_directory.empty() || options.draft_directory.native().find('\0') != std::string::npos) {
            invalid("invalid archive draft directory");
        }
        options.draft_directory = fs::absolute(options.draft_directory);
        file = local.normalize(fs::absolute(archive).string());
        const auto initial = local.stat(file.string(), cancellation);
        source_bytes = read_archive(local, file, initial, bounds.archive_bytes, cancellation);
        revision = initial.revision;
        entry_revision_base = initial.revision;
        compression = detect_compression(source_bytes);
        std::string expanded;
        if (compression != ArchiveCompression::none) {
            DraftDirectory snapshot(options.draft_directory, false);
            snapshot.write("snapshot", source_bytes, cancellation);
            expanded = convert_compression(compression, true, snapshot, "snapshot", bounds.expanded_bytes, cancellation);
        } else {
            if (source_bytes.size() > bounds.expanded_bytes) invalid("archive exceeds the expanded byte limit");
            expanded = source_bytes;
        }
        ParseState parsed;
        if (std::string_view(expanded).starts_with("070701") || std::string_view(expanded).starts_with("070702")) {
            parsed = parse_cpio(expanded, bounds, format, cancellation);
        } else {
            if (std::string_view(expanded).starts_with("070707")) invalid("old ASCII cpio is unsupported; use newc or CRC");
            format = ArchiveFormat::tar;
            parsed = parse_tar(expanded, bounds, cancellation);
            resolve_hardlinks(parsed, cancellation);
        }
        validate_model(parsed.model, bounds, cancellation);
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
            if (node.entry.kind == EntryKind::file) ++link_counts[node.object];
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
        draft.write("raw", raw, cancellation);
        std::string encoded;
        if (compression == ArchiveCompression::none) {
            if (raw.size() > options.limits.archive_bytes) invalid("new archive exceeds the physical byte limit; draft: " + last_draft->string());
            encoded = std::move(raw);
        } else {
            encoded = convert_compression(compression, false, draft, "raw", options.limits.archive_bytes, cancellation);
            draft.write("candidate", encoded, cancellation);
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
        if (read_archive(local, file, current, options.limits.archive_bytes, cancellation) != source_bytes) {
            invalid("archive content changed since opening; retained draft: " + last_draft->string());
        }
        std::istringstream input(encoded, std::ios::binary);
        local.write(file.string(), input, encoded.size(), true, revision, cancellation, {});
        const auto saved = local.stat(file.string(), {});
        if (read_archive(local, file, saved, options.limits.archive_bytes, {}) != encoded) {
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
    fs::path file;
    ArchiveFormat format{ArchiveFormat::tar};
    ArchiveCompression compression{ArchiveCompression::none};
    Model model;
    std::map<std::uint64_t, std::uint64_t> link_counts;
    std::uint64_t next_object{1};
    std::string revision;
    std::string entry_revision_base;
    std::string source_bytes;
    std::optional<fs::path> last_draft;
};

ArchiveProvider::ArchiveProvider(
    fs::path archive, ArchiveOpenOptions options, const Cancellation& cancellation
) : impl_(std::make_unique<Impl>(std::move(archive), std::move(options), cancellation)) {}

ArchiveProvider::~ArchiveProvider() = default;

fs::path ArchiveProvider::archive_path() const { return impl_->file; }
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
    if (node.entry.kind != EntryKind::file) path_error(key, std::errc::invalid_argument);
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
        if (impl_->file != destination_provider.impl_->file) return;
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

std::string ArchiveProvider::id() const { return "archive:" + impl_->file.string(); }
std::string ArchiveProvider::display_name() const { return impl_->file.string() + "!"; }
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
    return impl_->local.space(impl_->file.string());
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
    const auto& bytes = *snapshot;
    if (offset >= bytes.size()) return;
    const auto size = std::min<std::uint64_t>(length, bytes.size() - static_cast<std::size_t>(offset));
    std::uint64_t done = 0;
    while (done < size) {
        cancellation.throw_if_requested();
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(size - done, 64U * 1024U));
        output.write(bytes.data() + static_cast<std::size_t>(offset + done), static_cast<std::streamsize>(count));
        if (!output) invalid("failed writing archive member data to the output stream");
        done += count;
    }
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
    std::string bytes;
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
        if (size > impl_->options.limits.expanded_bytes - bytes.size()) invalid("member exceeds the expanded byte limit");
        bytes.append(buffer.data(), size);
        if (progress) {
            lock.unlock();
            progress({bytes.size(), length.value_or(0), 0, 1, key});
            lock.lock();
            if (impl_->revision != revision) invalid("archive view changed during the progress callback");
        }
        cancellation.throw_if_requested();
    }
    if (input.bad() || (!input.eof() && input.fail())) invalid("failed reading archive member input");
    if (length && *length != bytes.size()) invalid("member input length does not match its declared length");
    const auto replacement = std::make_shared<const std::string>(std::move(bytes));
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
    if (impl_->model.at(from).entry.kind != EntryKind::file) invalid("hardlinks require a regular archive file");
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

}  // namespace mcpp
