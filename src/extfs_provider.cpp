#include "mcpp/extfs_provider.hpp"

#include "extfs_helper.hpp"
#include "mcpp/archive_provider.hpp"
#include "mcpp/local_provider.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <ctime>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <streambuf>
#include <sys/stat.h>
#include <utility>

namespace mcpp {
namespace {
namespace fs = std::filesystem;
using extfs::fail;
using extfs::identity_part;
using extfs::Workspace;

[[noreturn]] void path_error(const std::string_view path, const std::errc error) {
    throw fs::filesystem_error("extfs member", fs::path(path), std::make_error_code(error));
}

[[noreturn]] void unsupported(const char* operation) {
    fail(std::string(operation) + " is not supported by the native extfs helper protocol");
}

void text_path(const std::string_view path, const ExtfsLimits& limits) {
    if (path.empty() || path.size() > limits.path_bytes ||
        path.find("://") != std::string_view::npos ||
        std::any_of(path.begin(), path.end(), [](const unsigned char byte) {
            return byte < 32U || byte == 127U || byte == '\\';
        })) {
        fail("unsafe or ambiguous member path");
    }
}

std::vector<std::string> components(const std::string_view path) {
    std::vector<std::string> result;
    std::size_t offset = path.starts_with('/') ? 1U : 0U;
    while (offset < path.size()) {
        const auto end = path.find('/', offset);
        result.emplace_back(path.substr(offset, end == std::string_view::npos ? end : end - offset));
        if (end == std::string_view::npos) break;
        offset = end + 1U;
    }
    return result;
}

std::string from_components(const std::vector<std::string>& parts) {
    std::string result;
    for (const auto& part : parts) result += "/" + part;
    return result.empty() ? "/" : result;
}

std::string virtual_path(std::string_view path, const ExtfsLimits& limits) {
    text_path(path, limits);
    if (path.find("//") != std::string_view::npos) fail("repeated slash in member path");
    if (path == "/") return "/";
    if (path.ends_with('/')) path.remove_suffix(1U);
    const auto parts = components(path);
    for (const auto& part : parts) {
        if (part.empty() || part == "." || part == ".." ||
            part.front() == ' ' || part.back() == ' ') {
            fail("dot, empty or ambiguous member path component");
        }
    }
    if (parts.empty()) fail("empty member path");
    const auto result = from_components(parts);
    if (result.size() > limits.path_bytes) fail("member path exceeds its byte limit");
    return result;
}

std::string parent_key(const std::string& path) {
    const auto slash = path.rfind('/');
    return slash == 0U ? "/" : path.substr(0, slash);
}

std::string base_key(const std::string& path) {
    return path == "/" ? "/" : path.substr(path.rfind('/') + 1U);
}

std::string link_destination(
    const std::string& path, std::string_view target, const ExtfsLimits& limits
) {
    text_path(target, limits);
    if (target.find(':') != std::string_view::npos) fail("links to host paths or other VFS providers are not enabled");
    if (target.find("//") != std::string_view::npos) fail("ambiguous link target");
    if (target.size() > 1U && target.ends_with('/')) target.remove_suffix(1U);
    auto parts = target.starts_with('/') ? std::vector<std::string>{} : components(parent_key(path));
    for (const auto& part : components(target)) {
        if (part.empty()) fail("ambiguous link target");
        if (part == ".") continue;
        if (part == "..") {
            if (parts.empty()) fail("link target escapes the virtual filesystem");
            parts.pop_back();
        } else parts.push_back(part);
    }
    return virtual_path(from_components(parts), limits);
}

struct Node {
    Entry entry;
    std::string hardlink;
    bool explicit_entry{true};
    std::shared_ptr<const std::string> bytes;
};
using Model = std::map<std::string, Node>;

std::string resolve(const Model& model, const std::string& path, const bool follow_leaf, const ExtfsLimits& limits) {
    std::deque<std::string> pending;
    for (auto& part : components(path)) pending.push_back(std::move(part));
    std::vector<std::string> found;
    unsigned int links = 0;
    while (!pending.empty()) {
        auto part = std::move(pending.front());
        pending.pop_front();
        found.push_back(std::move(part));
        const auto key = from_components(found);
        const auto node = model.find(key);
        if (node == model.end()) path_error(key, std::errc::no_such_file_or_directory);
        if (node->second.entry.kind == EntryKind::symlink && (follow_leaf || !pending.empty())) {
            if (++links > 32U) path_error(path, std::errc::too_many_symbolic_link_levels);
            const auto target = components(link_destination(key, node->second.entry.target, limits));
            for (auto item = target.rbegin(); item != target.rend(); ++item) pending.push_front(*item);
            found.clear();
        } else if (!pending.empty() && node->second.entry.kind != EntryKind::directory) {
            path_error(key, std::errc::not_a_directory);
        }
    }
    return from_components(found);
}

std::string file_key(const Model& model, const std::string& path, const ExtfsLimits& limits) {
    auto key = resolve(model, path, true, limits);
    std::set<std::string> visited;
    while (!model.at(key).hardlink.empty()) {
        if (!visited.insert(key).second) fail("hardlink cycle in helper listing");
        key = model.at(key).hardlink;
        const auto found = model.find(key);
        if (found == model.end() || found->second.entry.kind != EntryKind::file) {
            fail("hardlink target is not a listed regular file");
        }
    }
    if (model.at(key).entry.kind != EntryKind::file) path_error(path, std::errc::is_a_directory);
    return key;
}

class Row {
public:
    explicit Row(const std::string_view text) : text_(text) {}
    [[nodiscard]] std::string_view token() {
        const auto begin = text_.find_first_not_of(" \t", offset_);
        if (begin == std::string_view::npos) fail("incomplete helper listing line");
        const auto end = text_.find_first_of(" \t", begin);
        offset_ = end == std::string_view::npos ? text_.size() : end;
        return text_.substr(begin, offset_ - begin);
    }
    [[nodiscard]] std::string_view filename() const {
        if (offset_ + 1U >= text_.size() || text_[offset_] != ' ' ||
            text_[offset_ + 1U] == ' ' || text_[offset_ + 1U] == '\t') {
            fail("ambiguous filename delimiter in helper listing");
        }
        return text_.substr(offset_ + 1U);
    }
private:
    std::string_view text_;
    std::size_t offset_{0};
};

template <typename Value>
Value decimal(const std::string_view token, const char* description) {
    Value value{};
    const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value);
    if (token.empty() || parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size()) {
        fail(std::string("invalid ") + description + " in helper listing");
    }
    return value;
}

void numeric_identity(const std::string& value, std::uint32_t& output) {
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), output);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) output = 0;
}

std::uint32_t mode(const std::string_view value) {
    if (value.size() != 10U) fail("expected a ten-character ls permission string");
    std::uint32_t result = 0;
    if (value.front() == '-') result = S_IFREG;
    else if (value.front() == 'd') result = S_IFDIR;
    else if (value.front() == 'l') result = S_IFLNK;
    else fail("special files in extfs listings are not enabled");
    constexpr std::array<std::uint32_t, 9> bits{{0400U,0200U,0100U,0040U,0020U,0010U,0004U,0002U,0001U}};
    for (std::size_t index = 0; index < bits.size(); ++index) {
        const char byte = value[index + 1U];
        const auto position = index % 3U;
        if (byte == '-') continue;
        if ((position == 0U && byte == 'r') || (position == 1U && byte == 'w') ||
            (position == 2U && byte == 'x')) result |= bits[index];
        else if (position == 2U && index < 6U && (byte == 's' || byte == 'S')) {
            result |= index == 2U ? 04000U : 02000U;
            if (byte == 's') result |= bits[index];
        } else if (index == 8U && (byte == 't' || byte == 'T')) {
            result |= 01000U;
            if (byte == 't') result |= bits[index];
        } else fail("invalid ls permission string");
    }
    return result;
}

std::chrono::system_clock::time_point listing_time(Row& row) {
    constexpr std::array<std::string_view, 12> months{{
        "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"
    }};
    const auto first = row.token();
    std::tm value{};
    value.tm_isdst = -1;
    bool inferred_year = false;
    std::string_view clock;
    if (first.size() == 10U && first[2] == '-' && first[5] == '-') {
        value.tm_mon = decimal<int>(first.substr(0, 2), "month") - 1;
        value.tm_mday = decimal<int>(first.substr(3, 2), "day");
        value.tm_year = decimal<int>(first.substr(6), "year") - 1900;
        clock = row.token();
    } else {
        const auto month = std::find(months.begin(), months.end(), first);
        if (month == months.end()) fail("invalid English month in helper listing");
        value.tm_mon = static_cast<int>(month - months.begin());
        value.tm_mday = decimal<int>(row.token(), "day");
        const auto last = row.token();
        if (last.find(':') == std::string_view::npos) {
            if (last.size() != 4U) fail("invalid year in helper listing");
            value.tm_year = decimal<int>(last, "year") - 1900;
        } else {
            const auto now = std::time(nullptr);
            std::tm current{};
            if (::localtime_r(&now, &current) == nullptr) fail("cannot determine listing year");
            value.tm_year = current.tm_year;
            inferred_year = true;
            clock = last;
        }
    }
    if (!clock.empty()) {
        const auto colon = clock.find(':');
        if (colon == std::string_view::npos) fail("invalid time in helper listing");
        value.tm_hour = decimal<int>(clock.substr(0, colon), "hour");
        const auto second = clock.find(':', colon + 1U);
        value.tm_min = decimal<int>(clock.substr(colon + 1U,
            second == std::string_view::npos ? second : second - colon - 1U), "minute");
        if (second != std::string_view::npos) value.tm_sec = decimal<int>(clock.substr(second + 1U), "second");
    }
    if (value.tm_mon < 0 || value.tm_mon > 11 || value.tm_mday < 1 || value.tm_mday > 31 ||
        value.tm_year < 0 || value.tm_year > 300 || value.tm_hour < 0 || value.tm_hour > 23 ||
        value.tm_min < 0 || value.tm_min > 59 || value.tm_sec < 0 || value.tm_sec > 59) {
        fail("date or time is out of range in helper listing");
    }
    const auto before = value;
    auto timestamp = std::mktime(&value);
    if (value.tm_mon != before.tm_mon || value.tm_mday != before.tm_mday ||
        value.tm_year != before.tm_year || value.tm_hour != before.tm_hour ||
        value.tm_min != before.tm_min || value.tm_sec != before.tm_sec) {
        fail("invalid calendar date in helper listing");
    }
    if (inferred_year && timestamp > std::time(nullptr) + 24 * 60 * 60) {
        --value.tm_year;
        value.tm_isdst = -1;
        timestamp = std::mktime(&value);
    }
    return std::chrono::system_clock::from_time_t(timestamp);
}

Node directory_node(const bool explicit_entry = false) {
    Node result;
    result.explicit_entry = explicit_entry;
    result.entry.kind = EntryKind::directory;
    result.entry.mode = S_IFDIR | 0755U;
    result.entry.permissions = static_cast<fs::perms>(0755U);
    return result;
}

Model parse_listing(const std::string_view listing, const ExtfsLimits& limits, const Cancellation& cancellation) {
    if (std::any_of(listing.begin(), listing.end(), [](const unsigned char byte) {
        return (byte < 32U && byte != '\n' && byte != '\t') || byte == 127U;
    })) fail("control character in helper listing");
    Model model;
    model.emplace("/", directory_node());
    std::istringstream input{std::string(listing)};
    std::string line;
    std::size_t number = 0;
    std::size_t count = 0;
    while (std::getline(input, line)) {
        cancellation.throw_if_requested();
        ++number;
        if (line.empty()) continue;
        if (line.size() > limits.path_bytes * 3U + 1024U) fail("helper listing line exceeds its byte limit");
        try {
            Row row(line);
            Node node;
            auto& entry = node.entry;
            entry.mode = mode(row.token());
            entry.permissions = static_cast<fs::perms>(entry.mode & 07777U);
            entry.kind = S_ISDIR(entry.mode) ? EntryKind::directory :
                         S_ISLNK(entry.mode) ? EntryKind::symlink : EntryKind::file;
            entry.links = decimal<std::uint64_t>(row.token(), "link count");
            if (entry.links == 0U) fail("zero link count in helper listing");
            entry.owner = std::string(row.token());
            entry.group = std::string(row.token());
            if (entry.owner.size() > 255U || entry.group.size() > 255U) fail("owner or group is too long");
            numeric_identity(entry.owner, entry.uid);
            numeric_identity(entry.group, entry.gid);
            entry.size = decimal<std::uint64_t>(row.token(), "size");
            if (entry.size > limits.member_bytes) fail("member exceeds its byte limit");
            entry.modified = listing_time(row);
            entry.accessed = entry.modified;
            entry.changed = entry.modified;
            auto name = row.filename();
            const auto arrow = name.find(" -> ");
            if (arrow != std::string_view::npos) {
                if (name.find(" -> ", arrow + 4U) != std::string_view::npos) fail("ambiguous link output");
                const auto target = name.substr(arrow + 4U);
                name = name.substr(0, arrow);
                if (entry.kind == EntryKind::symlink) entry.target = std::string(target);
                else if (entry.kind == EntryKind::file && entry.links > 1U) {
                    if (target.starts_with('/')) fail("hardlink targets must be relative to the container root");
                    node.hardlink = virtual_path(target, limits);
                } else fail("ambiguous arrow in a non-link filename");
            } else if (entry.kind == EntryKind::symlink) fail("symlink listing has no target");
            if (name.starts_with('/')) fail("absolute member in helper listing");
            const auto key = virtual_path(name, limits);
            if (key == "/") fail("helper listing must not include the root");
            if (entry.kind != EntryKind::directory && name.ends_with('/')) fail("non-directory has a trailing slash");
            entry.path = key;
            entry.name = base_key(key);
            entry.hidden = entry.name.starts_with('.');
            if (!entry.target.empty()) static_cast<void>(link_destination(key, entry.target, limits));
            if (++count > limits.members || !model.emplace(key, std::move(node)).second) {
                fail("duplicate member or too many members in helper listing");
            }
        } catch (const std::runtime_error& error) {
            fail("listing line " + std::to_string(number) + ": " + error.what());
        }
    }
    std::vector<std::string> paths;
    for (const auto& [key, node] : model) {
        static_cast<void>(node);
        paths.push_back(key);
    }
    for (const auto& key : paths) {
        auto parent = parent_key(key);
        while (parent != "/") {
            auto [found, inserted] = model.try_emplace(parent, directory_node());
            if (!inserted && found->second.entry.kind != EntryKind::directory) {
                fail("member has a non-directory ancestor: " + key);
            }
            if (model.size() > limits.members + 1U) fail("too many explicit or implicit member directories");
            parent = parent_key(parent);
        }
    }
    std::map<std::string, std::uint64_t> inodes;
    std::map<std::string, std::uint64_t> link_counts;
    std::uint64_t next_inode = 1;
    for (auto& [key, node] : model) {
        cancellation.throw_if_requested();
        auto anchor = key;
        if (node.entry.kind == EntryKind::file) {
            anchor = file_key(model, key, limits);
            if (model.at(anchor).entry.size != node.entry.size) fail("inconsistent hardlink sizes in helper listing");
            ++link_counts[anchor];
        } else if (node.entry.kind == EntryKind::symlink) {
            try { static_cast<void>(resolve(model, key, true, limits)); }
            catch (const fs::filesystem_error& error) {
                if (error.code() != std::errc::no_such_file_or_directory) throw;
            }
        }
        const auto [found, inserted] = inodes.try_emplace(anchor, next_inode);
        if (inserted) ++next_inode;
        node.entry.inode = found->second;
    }
    for (auto& [key, node] : model) {
        if (node.entry.kind == EntryKind::file) {
            node.entry.links = std::max(node.entry.links, link_counts.at(file_key(model, key, limits)));
        }
    }
    return model;
}

class BoundedBuffer final : public std::streambuf {
public:
    explicit BoundedBuffer(const std::size_t limit) : limit_(limit) {}
    [[nodiscard]] std::string take() { return std::move(bytes_); }
protected:
    std::streamsize xsputn(const char* bytes, const std::streamsize count) override {
        if (count < 0 || static_cast<std::uint64_t>(count) > limit_ - bytes_.size()) {
            fail("container exceeds its byte limit");
        }
        bytes_.append(bytes, static_cast<std::size_t>(count));
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
    const ExtfsSource& source, const Entry& expected, const ExtfsLimits& limits,
    const Cancellation& cancellation
) {
    if (expected.kind != EntryKind::file) fail("helper source is not a regular file");
    if (expected.size > limits.container_bytes) fail("container exceeds its byte limit");
    BoundedBuffer buffer(limits.container_bytes);
    std::ostream output(&buffer);
    output.exceptions(std::ios::badbit | std::ios::failbit);
    source.provider->read(source.path, 0, expected.size + 1U, output, cancellation);
    auto bytes = buffer.take();
    const auto after = source.provider->stat_follow(source.path, cancellation);
    if (bytes.size() != expected.size || after.kind != EntryKind::file ||
        after.path != expected.path || after.revision != expected.revision) {
        fail("source changed while its private local copy was being prepared");
    }
    return bytes;
}

bool same_member(const Node& left, const Node& right) {
    return left.entry.kind == right.entry.kind && left.entry.size == right.entry.size &&
        left.entry.target == right.entry.target && left.hardlink == right.hardlink;
}

void validate_unaffected(const Model& before, const Model& after, const std::set<std::string>& changed) {
    for (const auto& [key, node] : before) {
        if (!node.explicit_entry || changed.contains(key)) continue;
        const auto found = after.find(key);
        if (found == after.end() || !same_member(node, found->second)) {
            fail("helper unexpectedly changed another member: " + key);
        }
    }
    for (const auto& [key, node] : after) {
        if (node.explicit_entry && !before.contains(key) && !changed.contains(key)) {
            fail("helper unexpectedly added another member: " + key);
        }
    }
}

std::string read_input(
    std::istream& input, const std::optional<std::uint64_t> length,
    const std::size_t limit, const Cancellation& cancellation
) {
    if (length && *length > limit) fail("new member exceeds its byte limit");
    std::string bytes;
    std::array<char, 65536> buffer{};
    while (input) {
        cancellation.throw_if_requested();
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count < 0 || static_cast<std::uint64_t>(count) > limit - bytes.size()) fail("new member exceeds its byte limit");
        bytes.append(buffer.data(), static_cast<std::size_t>(count));
    }
    if (input.bad() || (!input.eof() && input.fail())) fail("failed while reading new member data");
    if (length && bytes.size() != *length) fail("new member length does not match its expected size");
    return bytes;
}

}  // namespace

class ExtfsProvider::Impl {
public:
    ExtfsOpenOptions options;
    ExtfsSource source;
    Entry source_entry;
    ExtfsHelper helper;
    std::string identity;
    std::string container;
    Model model;
    bool writable{false};
    std::string readonly_reason;
    std::uint64_t generation{1};
    std::size_t cached_bytes{0};
    mutable std::mutex mutex;

    [[nodiscard]] Entry entry(const std::string& key) const {
        auto result = model.at(key).entry;
        result.path = key;
        result.name = base_key(key);
        result.hidden = result.name.starts_with('.');
        result.revision = identity + ":" + std::to_string(generation) + ":" + identity_part(key);
        if (result.kind == EntryKind::symlink) {
            try {
                result.target_is_directory = model.at(resolve(model, key, true, options.limits)).entry.kind == EntryKind::directory;
            } catch (const fs::filesystem_error& error) {
                if (error.code() != std::errc::no_such_file_or_directory) throw;
            }
        }
        return result;
    }

    void unchanged(const Cancellation& cancellation) const {
        const auto current = source.provider->stat_follow(source.path, cancellation);
        if (current.kind != EntryKind::file || current.path != source_entry.path ||
            current.revision != source.revision || current.size != container.size()) {
            fail("source container changed since it was opened; reopen it before making changes");
        }
    }

    void source_writable(const Cancellation& cancellation) const {
        if (!options.writable) fail("filesystem was opened read-only");
        if (!has_capability(source.provider->capabilities(), ProviderCapability::write)) {
            fail("source provider is read-only");
        }
        unchanged(cancellation);
        const auto leaf = source.provider->stat(source.path, cancellation);
        if (leaf.kind != EntryKind::file || leaf.revision != source.revision) {
            fail("writing through a source symlink is not enabled");
        }
        if (leaf.links != 1U) fail("atomic helper publication cannot preserve source hardlinks");
        if (const auto* local = dynamic_cast<const LocalProvider*>(source.provider.get())) {
            local->ensure_writable(source.path);
        } else if (const auto* archive = dynamic_cast<const ArchiveProvider*>(source.provider.get())) {
            archive->ensure_writable(source.path);
        } else if (const auto* external = dynamic_cast<const ExtfsProvider*>(source.provider.get())) {
            external->ensure_writable(source.path);
        } else fail("source provider does not offer a supported guarded publication");
    }

    void require_writable(const Cancellation& cancellation) const {
        if (!writable) fail(readonly_reason);
        source_writable(cancellation);
    }

    void member_writable(const std::string& key) const {
        const auto& item = model.at(key).entry;
        constexpr auto bits = fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write;
        if (item.permissions == fs::perms::unknown || (item.permissions & bits) == fs::perms::none) {
            path_error(key, std::errc::permission_denied);
        }
        if (item.kind == EntryKind::file && item.links != 1U) {
            fail("copyin cannot safely preserve linked members; copy to a new name instead");
        }
    }

    [[nodiscard]] std::string destination(const std::string_view path) const {
        const auto requested = virtual_path(path, options.limits);
        if (requested == "/") fail("cannot replace or remove the extfs root");
        const auto parent = resolve(model, parent_key(requested), true, options.limits);
        if (model.at(parent).entry.kind != EntryKind::directory) path_error(parent, std::errc::not_a_directory);
        return (parent == "/" ? parent : parent + "/") + base_key(requested);
    }

    [[nodiscard]] Model listing(const Workspace& workspace, const Cancellation& cancellation) const {
        const auto before = workspace.get("container", options.limits.container_bytes, cancellation);
        const auto text = extfs::invoke(helper, workspace, "list",
            {(workspace.path() / "container").string()}, options.limits, cancellation);
        if (workspace.get("container", options.limits.container_bytes, cancellation) != before) {
            fail("list helper unexpectedly modified its input");
        }
        return parse_listing(text, options.limits, cancellation);
    }

    [[nodiscard]] std::string copyout(
        const Workspace& workspace, const Model& in, const std::string& key,
        const Cancellation& cancellation
    ) const {
        const auto anchor = file_key(in, key, options.limits);
        const auto before = workspace.get("container", options.limits.container_bytes, cancellation);
        workspace.put("output", {}, cancellation);
        static_cast<void>(extfs::invoke(helper, workspace, "copyout",
            {(workspace.path() / "container").string(), anchor.substr(1U),
             (workspace.path() / "output").string()}, options.limits, cancellation));
        const auto result = workspace.get("output", options.limits.member_bytes, cancellation);
        if (result.size() != in.at(anchor).entry.size) fail("copyout size differs from the helper listing");
        if (workspace.get("container", options.limits.container_bytes, cancellation) != before) {
            fail("copyout helper unexpectedly modified its input");
        }
        return result;
    }

    [[nodiscard]] std::shared_ptr<const std::string> data(
        const std::string& key, const Cancellation& cancellation
    ) {
        const auto anchor = file_key(model, key, options.limits);
        auto& node = model.at(anchor);
        if (node.bytes) return node.bytes;
        Workspace workspace(options);
        workspace.put("container", container, cancellation);
        auto bytes = std::make_shared<const std::string>(copyout(workspace, model, anchor, cancellation));
        if (bytes->size() <= options.limits.container_bytes - cached_bytes) {
            node.bytes = bytes;
            cached_bytes += bytes->size();
        }
        return bytes;
    }

    void publish(const Workspace& workspace, Model next, const Cancellation& cancellation) {
        const auto bytes = workspace.get("container", options.limits.container_bytes, cancellation);
        require_writable(cancellation);
        std::istringstream input(bytes);
        source.provider->write(source.path, input, bytes.size(), true, source.revision, cancellation, {});
        const Cancellation published;
        const auto current = source.provider->stat_follow(source.path, published);
        auto next_source = source;
        next_source.revision = current.revision;
        if (current.revision.empty() || snapshot(next_source, current, options.limits, published) != bytes) {
            fail("source changed immediately after helper publication; reopen it");
        }
        source = std::move(next_source);
        source_entry = current;
        container = bytes;
        model = std::move(next);
        cached_bytes = 0;
        ++generation;
    }
};

class ExtfsCache::Impl {
public:
    struct Record {
        std::shared_ptr<ExtfsProvider> provider;
        std::chrono::steady_clock::time_point last_used;
    };
    Impl(const std::chrono::seconds lifetime, ExtfsOpenOptions settings)
        : timeout(lifetime), options(std::move(settings)) {}
    std::chrono::seconds timeout;
    ExtfsOpenOptions options;
    std::mutex mutex;
    std::vector<Record> records;
};

ExtfsCache::ExtfsCache(const std::chrono::seconds timeout, ExtfsOpenOptions options)
    : impl_(std::make_unique<Impl>(timeout, std::move(options))) {
    if (timeout.count() < 0) throw std::invalid_argument("VFS timeout cannot be negative");
    extfs::validate_limits(impl_->options.limits);
}
ExtfsCache::~ExtfsCache() = default;

ProviderPtr ExtfsCache::open(
    const ProviderPtr& provider, const std::string_view path, const std::string_view source_location,
    const std::string_view name, const Cancellation& cancellation
) {
    if (!provider) throw std::invalid_argument("extfs source provider is null");
    if (!has_capability(provider->capabilities(), ProviderCapability::read)) fail("source provider does not allow reading");
    if (source_location.empty() || source_location.size() > 65536U ||
        source_location.find('\0') != std::string_view::npos) fail("invalid logical source location");
    if (path.empty() || path.size() > 65536U || path.find('\0') != std::string_view::npos) fail("invalid source path");
    const auto selected = find_extfs_helper(name, impl_->options);
    if (!selected) fail("executable helper is not installed: " + std::string(name));
    if (!selected->requires_source) fail("source-less '+' helpers are not enabled; only local containers are supported");
    prune();
    ExtfsSource source{provider, provider->normalize(path), {}, std::string(source_location)};
    const auto entry = provider->stat_follow(source.path, cancellation);
    if (entry.kind != EntryKind::file || entry.revision.empty()) fail("source must be a revisioned regular file");
    source.revision = entry.revision;
    const auto reusable = [&](const Impl::Record& record) {
        const auto captured = record.provider->source();
        return captured.provider == provider && captured.path == source.path &&
            captured.revision == source.revision && captured.location == source.location &&
            record.provider->impl_->helper.executable == selected->executable &&
            record.provider->impl_->helper.revision == selected->revision;
    };
    {
        const std::lock_guard lock(impl_->mutex);
        for (auto& record : impl_->records) {
            if (reusable(record)) {
                record.last_used = std::chrono::steady_clock::now();
                return record.provider;
            }
        }
    }
    auto next = std::make_unique<ExtfsProvider::Impl>();
    next->options = impl_->options;
    next->source = source;
    next->source_entry = entry;
    next->helper = *selected;
    next->container = snapshot(source, entry, next->options.limits, cancellation);
    Workspace workspace(next->options);
    workspace.put("container", next->container, cancellation);
    next->model = next->listing(workspace, cancellation);
    next->unchanged(cancellation);
    try {
        next->source_writable(cancellation);
        next->writable = true;
    } catch (const OperationCancelled&) { throw; }
    catch (const std::exception& error) { next->readonly_reason = error.what(); }
    static std::atomic_uint64_t sequence{1};
    const auto serial = sequence.fetch_add(1, std::memory_order_relaxed);
    next->identity = "extfs:" + identity_part(provider->id()) + identity_part(source.path) +
        identity_part(source.revision) + identity_part(source.location) +
        identity_part(selected->executable.string()) + identity_part(selected->revision) +
        ":" + std::to_string(serial);
    std::shared_ptr<ExtfsProvider> opened(new ExtfsProvider(std::move(next)));
    const std::lock_guard lock(impl_->mutex);
    for (auto& record : impl_->records) {
        if (reusable(record)) {
            record.last_used = std::chrono::steady_clock::now();
            return record.provider;
        }
    }
    impl_->records.push_back({opened, std::chrono::steady_clock::now()});
    return opened;
}

void ExtfsCache::set_timeout(const std::chrono::seconds timeout) {
    if (timeout.count() < 0) throw std::invalid_argument("VFS timeout cannot be negative");
    {
        const std::lock_guard lock(impl_->mutex);
        impl_->timeout = timeout;
    }
    prune();
}

void ExtfsCache::prune() {
    const auto now = std::chrono::steady_clock::now();
    const std::lock_guard lock(impl_->mutex);
    std::erase_if(impl_->records, [&](Impl::Record& record) {
        if (record.provider.use_count() > 1) {
            record.last_used = now;
            return false;
        }
        return now - record.last_used >= impl_->timeout;
    });
}

std::vector<std::string> ExtfsCache::locations() {
    prune();
    const std::lock_guard lock(impl_->mutex);
    std::set<std::string> found;
    for (const auto& record : impl_->records) found.insert(record.provider->location());
    return {found.begin(), found.end()};
}

ExtfsProvider::ExtfsProvider(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
ExtfsProvider::~ExtfsProvider() = default;
ExtfsSource ExtfsProvider::source() const {
    const std::lock_guard lock(impl_->mutex);
    return impl_->source;
}
const std::string& ExtfsProvider::helper() const noexcept { return impl_->helper.name; }
std::string ExtfsProvider::location(const std::string_view path) const {
    const auto key = normalize(path);
    const std::lock_guard lock(impl_->mutex);
    return impl_->source.location + "/" + impl_->helper.name + "://" + key.substr(1U);
}
std::string ExtfsProvider::id() const { return impl_->identity; }
std::string ExtfsProvider::display_name() const { return location(); }
ProviderCapability ExtfsProvider::capabilities() const {
    return impl_->writable ? ProviderCapability::read | ProviderCapability::write | ProviderCapability::remove
                          : ProviderCapability::read;
}
std::string ExtfsProvider::normalize(const std::string_view path) const { return virtual_path(path, impl_->options.limits); }
std::string ExtfsProvider::parent(const std::string_view path) const { return parent_key(normalize(path)); }
std::string ExtfsProvider::basename(const std::string_view path) const { return base_key(normalize(path)); }
std::string ExtfsProvider::join(const std::string_view directory, const std::string_view name) const {
    if (name.empty() || name.find('/') != std::string_view::npos) fail("join requires one literal member name");
    const auto base = normalize(directory);
    return normalize((base == "/" ? base : base + "/") + std::string(name));
}

void ExtfsProvider::ensure_writable(const std::string_view path) const {
    const std::lock_guard lock(impl_->mutex);
    impl_->require_writable({});
    const auto key = resolve(impl_->model, normalize(path), true, impl_->options.limits);
    if (impl_->model.at(key).entry.kind != EntryKind::file) path_error(key, std::errc::invalid_argument);
    impl_->member_writable(key);
}

void ExtfsProvider::validate_directory_target(
    const std::string_view source, const ExtfsProvider& destination_provider,
    const std::string_view destination
) const {
    const auto validate = [&] {
        const auto& from_source = impl_->source;
        const auto& to_source = destination_provider.impl_->source;
        if (from_source.provider != to_source.provider || from_source.path != to_source.path ||
            impl_->helper.executable != destination_provider.impl_->helper.executable) return;
        const auto from = resolve(impl_->model, normalize(source), true, impl_->options.limits);
        const auto requested = destination_provider.normalize(destination);
        std::string to;
        try {
            to = resolve(destination_provider.impl_->model, requested, true, destination_provider.impl_->options.limits);
        } catch (const fs::filesystem_error& error) {
            if (error.code() != std::errc::no_such_file_or_directory) throw;
            to = destination_provider.impl_->destination(requested);
        }
        if (impl_->model.at(from).entry.kind == EntryKind::directory &&
            (from == "/" || from == to || to.starts_with(from + "/"))) {
            fail("cannot copy or move an extfs directory into itself");
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

std::vector<Entry> ExtfsProvider::list(
    const std::string_view path, const ListRequest& request, const Cancellation& cancellation
) const {
    cancellation.throw_if_requested();
    const std::lock_guard lock(impl_->mutex);
    const auto key = resolve(impl_->model, normalize(path), true, impl_->options.limits);
    if (impl_->model.at(key).entry.kind != EntryKind::directory) path_error(key, std::errc::not_a_directory);
    std::vector<Entry> entries;
    for (const auto& [candidate, node] : impl_->model) {
        static_cast<void>(node);
        cancellation.throw_if_requested();
        if (candidate == "/" || parent_key(candidate) != key) continue;
        auto item = impl_->entry(candidate);
        if (request.show_hidden || !item.hidden) entries.push_back(std::move(item));
    }
    sort_entries(entries, request);
    const auto begin = std::min(entries.size(), request.offset);
    const auto count = request.limit == 0U ? entries.size() - begin : std::min(request.limit, entries.size() - begin);
    return {std::make_move_iterator(entries.begin() + static_cast<std::ptrdiff_t>(begin)),
            std::make_move_iterator(entries.begin() + static_cast<std::ptrdiff_t>(begin + count))};
}

Entry ExtfsProvider::stat(const std::string_view path, const Cancellation& cancellation) const {
    cancellation.throw_if_requested();
    const std::lock_guard lock(impl_->mutex);
    return impl_->entry(resolve(impl_->model, normalize(path), false, impl_->options.limits));
}
Entry ExtfsProvider::stat_follow(const std::string_view path, const Cancellation& cancellation) const {
    cancellation.throw_if_requested();
    const std::lock_guard lock(impl_->mutex);
    return impl_->entry(resolve(impl_->model, normalize(path), true, impl_->options.limits));
}
bool ExtfsProvider::exists(const std::string_view path) const {
    try { static_cast<void>(stat(path, {})); return true; }
    catch (const fs::filesystem_error& error) {
        if (error.code() == std::errc::no_such_file_or_directory || error.code() == std::errc::not_a_directory) return false;
        throw;
    }
}
SpaceInfo ExtfsProvider::space(const std::string_view) const { unsupported("free-space reporting"); }

void ExtfsProvider::read(
    const std::string_view path, const std::uint64_t offset, const std::uint64_t length,
    std::ostream& output, const Cancellation& cancellation
) const {
    cancellation.throw_if_requested();
    std::shared_ptr<const std::string> bytes;
    {
        const std::lock_guard lock(impl_->mutex);
        bytes = impl_->data(normalize(path), cancellation);
    }
    if (offset >= bytes->size()) return;
    auto remaining = std::min<std::uint64_t>(length, bytes->size() - offset);
    auto position = static_cast<std::size_t>(offset);
    while (remaining > 0U) {
        cancellation.throw_if_requested();
        const auto count = std::min<std::uint64_t>(remaining, 65536U);
        output.write(bytes->data() + position, static_cast<std::streamsize>(count));
        if (!output) fail("cannot write copied-out member data");
        position += static_cast<std::size_t>(count);
        remaining -= count;
    }
}

void ExtfsProvider::write(
    const std::string_view path, std::istream& input, const std::optional<std::uint64_t> length,
    const bool overwrite, const std::optional<std::string> expected_revision,
    const Cancellation& cancellation, const ProgressCallback& progress
) {
    const std::lock_guard lock(impl_->mutex);
    impl_->require_writable(cancellation);
    const auto key = impl_->destination(path);
    const auto found = impl_->model.find(key);
    if (found != impl_->model.end()) {
        if (!overwrite) path_error(key, std::errc::file_exists);
        if (found->second.entry.kind == EntryKind::directory) path_error(key, std::errc::is_a_directory);
        impl_->member_writable(key);
    }
    impl_->member_writable(parent_key(key));
    if (expected_revision && (found == impl_->model.end() || impl_->entry(key).revision != *expected_revision)) {
        fail("member changed since it was opened");
    }
    const auto bytes = read_input(input, length, impl_->options.limits.member_bytes, cancellation);
    Workspace workspace(impl_->options);
    workspace.put("container", impl_->container, cancellation);
    workspace.put("input", bytes, cancellation);
    if (found != impl_->model.end() && found->second.entry.kind == EntryKind::symlink) {
        static_cast<void>(extfs::invoke(impl_->helper, workspace, "rm",
            {(workspace.path() / "container").string(), key.substr(1U)}, impl_->options.limits, cancellation));
    }
    static_cast<void>(extfs::invoke(impl_->helper, workspace, "copyin",
        {(workspace.path() / "container").string(), key.substr(1U), (workspace.path() / "input").string()},
        impl_->options.limits, cancellation));
    auto next = impl_->listing(workspace, cancellation);
    const auto created = next.find(key);
    if (created == next.end() || created->second.entry.kind != EntryKind::file ||
        !created->second.hardlink.empty() || created->second.entry.links != 1U ||
        impl_->copyout(workspace, next, key, cancellation) != bytes) {
        fail("copyin did not produce the requested regular-file contents");
    }
    validate_unaffected(impl_->model, next, {key});
    impl_->publish(workspace, std::move(next), cancellation);
    if (progress) progress({.bytes_done=bytes.size(), .bytes_total=bytes.size(), .files_done=1,
                            .files_total=1, .current_path=key});
}

void ExtfsProvider::create_directory(const std::string_view path) {
    const std::lock_guard lock(impl_->mutex);
    const Cancellation cancellation;
    impl_->require_writable(cancellation);
    const auto key = impl_->destination(path);
    if (impl_->model.contains(key)) path_error(key, std::errc::file_exists);
    impl_->member_writable(parent_key(key));
    Workspace workspace(impl_->options);
    workspace.put("container", impl_->container, cancellation);
    static_cast<void>(extfs::invoke(impl_->helper, workspace, "mkdir",
        {(workspace.path() / "container").string(), key.substr(1U)}, impl_->options.limits, cancellation));
    auto next = impl_->listing(workspace, cancellation);
    const auto created = next.find(key);
    if (created == next.end() || created->second.entry.kind != EntryKind::directory) {
        fail("mkdir helper did not create the requested directory");
    }
    validate_unaffected(impl_->model, next, {key});
    impl_->publish(workspace, std::move(next), cancellation);
}

void ExtfsProvider::remove(
    const std::string_view path, const bool recursive, const Cancellation& cancellation,
    const ProgressCallback& progress
) {
    const std::lock_guard lock(impl_->mutex);
    impl_->require_writable(cancellation);
    const auto key = resolve(impl_->model, normalize(path), false, impl_->options.limits);
    if (key == "/") fail("cannot remove the extfs root");
    impl_->member_writable(parent_key(key));
    std::set<std::string> removed{key};
    if (impl_->model.at(key).entry.kind == EntryKind::directory) {
        for (const auto& [candidate, node] : impl_->model) {
            static_cast<void>(node);
            if (candidate.starts_with(key + "/")) removed.insert(candidate);
        }
        if (!recursive && removed.size() > 1U) path_error(key, std::errc::directory_not_empty);
    }
    std::vector<std::string> ordered(removed.begin(), removed.end());
    std::sort(ordered.begin(), ordered.end(), [](const auto& left, const auto& right) {
        return left.size() > right.size();
    });
    Workspace workspace(impl_->options);
    workspace.put("container", impl_->container, cancellation);
    for (const auto& member : ordered) {
        cancellation.throw_if_requested();
        const auto& node = impl_->model.at(member);
        if (node.entry.kind == EntryKind::directory && !node.explicit_entry) continue;
        static_cast<void>(extfs::invoke(impl_->helper, workspace,
            node.entry.kind == EntryKind::directory ? "rmdir" : "rm",
            {(workspace.path() / "container").string(), member.substr(1U)}, impl_->options.limits, cancellation));
    }
    auto next = impl_->listing(workspace, cancellation);
    for (const auto& member : removed) if (next.contains(member)) fail("remove helper left the requested member: " + member);
    validate_unaffected(impl_->model, next, removed);
    impl_->publish(workspace, std::move(next), cancellation);
    if (progress) progress({.files_done=removed.size(), .files_total=removed.size(), .current_path=key});
}

void ExtfsProvider::rename(const std::string_view, const std::string_view, const bool) { unsupported("rename"); }
bool ExtfsProvider::copy_direct(
    const std::string_view, const std::string_view, const bool,
    const Cancellation&, const ProgressCallback&
) { return false; }
void ExtfsProvider::set_permissions(const std::string_view, const fs::perms) { unsupported("chmod"); }
void ExtfsProvider::set_owner(
    const std::string_view, const std::optional<std::string>, const std::optional<std::string>
) { unsupported("chown"); }
void ExtfsProvider::create_symlink(
    const std::string_view, const std::string_view, const bool, const Cancellation&
) { unsupported("symlink creation"); }
void ExtfsProvider::create_hardlink(const std::string_view, const std::string_view) { unsupported("hardlink creation"); }

}  // namespace mcpp
