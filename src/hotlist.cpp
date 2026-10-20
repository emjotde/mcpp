#include "mcpp/hotlist.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <fcntl.h>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace mcpp {
namespace {

constexpr std::string_view imported_comment = " mcpp: legacy bookmarks imported";
constexpr std::size_t maximum_depth = 128;
constexpr std::size_t maximum_size = 8U * 1024U * 1024U;

struct Descriptor {
    int value{-1};
    ~Descriptor() { if (value >= 0) static_cast<void>(close(value)); }
};

std::optional<std::string> read_hotlist(const std::filesystem::path& path) {
    Descriptor descriptor{open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
    if (descriptor.value < 0) {
        if (errno == ENOENT) return {};
        throw std::system_error(errno, std::generic_category(), "open directory hotlist");
    }
    struct stat status{};
    if (fstat(descriptor.value, &status) != 0) {
        throw std::system_error(errno, std::generic_category(), "stat directory hotlist");
    }
    if (!S_ISREG(status.st_mode)) throw std::runtime_error("directory hotlist is not a regular file");
    std::string result;
    std::array<char, 8192> buffer{};
    for (;;) {
        const auto count = read(descriptor.value, buffer.data(), buffer.size());
        if (count == 0) return result;
        if (count < 0) {
            if (errno == EINTR) continue;
            throw std::system_error(errno, std::generic_category(), "read directory hotlist");
        }
        if (static_cast<std::size_t>(count) > maximum_size - result.size()) {
            throw std::runtime_error("directory hotlist exceeds 8 MiB");
        }
        result.append(buffer.data(), static_cast<std::size_t>(count));
    }
}

class Parser {
public:
    explicit Parser(const std::string_view input) : input_(input) {}

    std::vector<HotlistItem> items(const bool nested, const std::size_t depth, bool& imported) {
        if (depth > maximum_depth) fail("groups are nested too deeply");
        std::vector<HotlistItem> result;
        for (;;) {
            whitespace();
            if (position_ == input_.size()) {
                if (nested) fail("missing ENDGROUP");
                return result;
            }
            if (input_[position_] == '#') {
                const auto start = ++position_;
                while (position_ < input_.size() && input_[position_] != '\n') ++position_;
                auto comment = std::string(input_.substr(start, position_ - start));
                if (!comment.empty() && comment.back() == '\r') comment.pop_back();
                if (comment == imported_comment) imported = true;
                else result.push_back({HotlistKind::comment, std::move(comment), {}, {}});
                continue;
            }
            const auto keyword = word();
            if (keyword == "ENDGROUP") {
                if (!nested) fail("unexpected ENDGROUP");
                return result;
            }
            if (keyword == "GROUP") {
                auto label = quoted();
                auto children = items(true, depth + 1U, imported);
                result.push_back({HotlistKind::group, std::move(label), {}, std::move(children)});
            } else if (keyword == "ENTRY") {
                auto label = quoted();
                if (word() != "URL") fail("expected URL after an entry label");
                auto directory = quoted();
                result.push_back({HotlistKind::entry, std::move(label), std::move(directory), {}});
            } else {
                fail("expected ENTRY, GROUP or ENDGROUP");
            }
        }
    }

private:
    std::string_view input_;
    std::size_t position_{0};
    std::size_t line_{1};

    [[noreturn]] void fail(const std::string& reason) const {
        throw std::runtime_error("invalid directory hotlist at line " +
                                 std::to_string(line_) + ": " + reason);
    }

    void whitespace() {
        while (position_ < input_.size() &&
               std::isspace(static_cast<unsigned char>(input_[position_])) != 0) {
            if (input_[position_] == '\n') ++line_;
            ++position_;
        }
    }

    std::string word() {
        whitespace();
        std::string result;
        while (position_ < input_.size() &&
               std::isalpha(static_cast<unsigned char>(input_[position_])) != 0) {
            result += static_cast<char>(std::toupper(static_cast<unsigned char>(input_[position_++])));
        }
        if (result.empty()) fail("expected a keyword");
        return result;
    }

    std::string quoted() {
        whitespace();
        if (position_ == input_.size() || input_[position_++] != '"') fail("expected a quoted string");
        std::string result;
        while (position_ < input_.size()) {
            auto character = input_[position_++];
            if (character == '"') return result;
            if (character == '\\') {
                if (position_ == input_.size()) fail("incomplete string escape");
                character = input_[position_++];
            }
            if (character == '\0') fail("NUL in a string");
            if (character == '\n') ++line_;
            result += character;
        }
        fail("unterminated quoted string");
    }
};

void write_items(
    std::ostream& output, const std::vector<HotlistItem>& items, const std::size_t depth
) {
    if (depth > maximum_depth) throw std::runtime_error("hotlist groups are nested too deeply");
    const std::string indent(depth * 2U, ' ');
    for (const auto& item : items) {
        if (item.label.find('\0') != std::string::npos || item.directory.find('\0') != std::string::npos) {
            throw std::invalid_argument("hotlist labels and paths cannot contain NUL");
        }
        switch (item.kind) {
            case HotlistKind::comment:
                if (item.label.find_first_of("\r\n") != std::string::npos) {
                    throw std::invalid_argument("a hotlist comment must fit on one line");
                }
                output << indent << '#' << item.label << '\n';
                break;
            case HotlistKind::entry:
                output << indent << "ENTRY " << std::quoted(item.label)
                       << " URL " << std::quoted(item.directory) << '\n';
                break;
            case HotlistKind::group:
                output << indent << "GROUP " << std::quoted(item.label) << '\n';
                write_items(output, item.children, depth + 1U);
                output << indent << "ENDGROUP\n";
                break;
        }
    }
}

void collect_bookmarks(const std::vector<HotlistItem>& items, std::vector<std::string>& result) {
    for (const auto& item : items) {
        if (item.kind == HotlistKind::group) collect_bookmarks(item.children, result);
        else if (item.kind == HotlistKind::entry) result.push_back(item.directory);
    }
}

bool begins_with(const HotlistPath& path, const HotlistPath& prefix) {
    return path.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), path.begin());
}

}  // namespace

DirectoryHotlist DirectoryHotlist::parse(const std::string_view content) {
    if (content.size() > maximum_size) throw std::runtime_error("directory hotlist exceeds 8 MiB");
    DirectoryHotlist result;
    result.items_ = Parser(content).items(false, 0, result.legacy_imported_);
    return result;
}

DirectoryHotlist DirectoryHotlist::load(
    const std::filesystem::path& path, const std::vector<std::string>& legacy_bookmarks
) {
    const auto content = read_hotlist(path);
    auto result = content ? parse(*content) : DirectoryHotlist{};
    result.source_ = path.lexically_normal();
    result.saved_content_ = content;
    if (!result.legacy_imported_) {
        auto existing = result.bookmarks();
        for (const auto& directory : legacy_bookmarks) {
            if (std::ranges::find(existing, directory) == existing.end()) {
                result.items_.push_back({HotlistKind::entry, directory, directory, {}});
                existing.push_back(directory);
            }
        }
        result.legacy_imported_ = true;
    }
    return result;
}

std::string DirectoryHotlist::serialize() const {
    std::ostringstream output;
    if (legacy_imported_) output << '#' << imported_comment << '\n';
    write_items(output, items_, 0);
    auto content = output.str();
    if (content.size() > maximum_size) throw std::runtime_error("directory hotlist exceeds 8 MiB");
    return content;
}

void DirectoryHotlist::save(const std::filesystem::path& path) {
    const auto content = serialize();
    const auto expected = source_ && *source_ == path.lexically_normal() ? saved_content_
                                                                      : std::optional<std::string>{};
    const auto check_destination = [&] {
        if (read_hotlist(path) != expected) {
            throw std::runtime_error("directory hotlist changed on disk; close and reopen the hotlist");
        }
        struct stat status{};
        if (lstat(path.c_str(), &status) == 0) {
            if (!S_ISREG(status.st_mode) || status.st_uid != geteuid()) {
                throw std::runtime_error("refusing to replace an unowned or non-regular directory hotlist");
            }
        } else if (errno != ENOENT) {
            throw std::system_error(errno, std::generic_category(), "stat directory hotlist");
        }
    };
    check_destination();
    auto parent = path.parent_path();
    if (parent.empty()) parent = ".";
    if (std::filesystem::create_directories(parent) && chmod(parent.c_str(), 0700) != 0) {
        throw std::system_error(errno, std::generic_category(), "protect directory hotlist directory");
    }
    static std::atomic_uint64_t sequence{0};
    std::filesystem::path staging;
    Descriptor descriptor;
    for (unsigned int attempt = 0; attempt < 32U; ++attempt) {
        staging = parent / (".hotlist-write-" + std::to_string(getpid()) + "-" +
                            std::to_string(sequence.fetch_add(1)));
        descriptor.value = open(staging.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (descriptor.value >= 0) break;
        if (errno != EEXIST) throw std::system_error(errno, std::generic_category(), "create directory hotlist file");
    }
    if (descriptor.value < 0) throw std::runtime_error("cannot create a directory hotlist staging file");
    try {
        std::size_t offset = 0;
        while (offset < content.size()) {
            const auto count = write(descriptor.value, content.data() + offset, content.size() - offset);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) throw std::system_error(errno ? errno : EIO, std::generic_category(), "write directory hotlist");
            offset += static_cast<std::size_t>(count);
        }
        if (fsync(descriptor.value) != 0) {
            throw std::system_error(errno, std::generic_category(), "commit directory hotlist");
        }
        const auto value = std::exchange(descriptor.value, -1);
        if (close(value) != 0) throw std::system_error(errno, std::generic_category(), "close directory hotlist");
        check_destination();
        std::filesystem::rename(staging, path);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(staging, ignored);
        throw;
    }
    source_ = path.lexically_normal();
    saved_content_ = content;
}

const std::vector<HotlistItem>& DirectoryHotlist::group(const HotlistPath& path) const {
    const auto* items = &items_;
    for (const auto index : path) {
        const auto& item = items->at(index);
        if (item.kind != HotlistKind::group) throw std::invalid_argument("hotlist destination is not a group");
        items = &item.children;
    }
    return *items;
}

std::vector<HotlistItem>& DirectoryHotlist::group(const HotlistPath& path) {
    return const_cast<std::vector<HotlistItem>&>(std::as_const(*this).group(path));
}

std::vector<std::string> DirectoryHotlist::bookmarks() const {
    std::vector<std::string> result;
    collect_bookmarks(items_, result);
    return result;
}

std::size_t DirectoryHotlist::insert(
    const HotlistPath& path, HotlistItem item, const std::optional<std::size_t> before
) {
    auto& items = group(path);
    const auto index = before.value_or(items.size());
    if (index > items.size()) throw std::out_of_range("hotlist insertion position is out of range");
    items.insert(items.begin() + static_cast<std::ptrdiff_t>(index), std::move(item));
    return index;
}

void DirectoryHotlist::erase(const HotlistPath& path) {
    if (path.empty()) throw std::invalid_argument("cannot remove the top level hotlist group");
    HotlistPath parent(path.begin(), path.end() - 1);
    auto& items = group(parent);
    if (path.back() >= items.size()) throw std::out_of_range("hotlist entry is out of range");
    items.erase(items.begin() + static_cast<std::ptrdiff_t>(path.back()));
}

HotlistPath DirectoryHotlist::move(
    const HotlistPath& path, HotlistPath destination, const std::optional<std::size_t> before
) {
    if (path.empty()) throw std::invalid_argument("cannot move the top level hotlist group");
    if (begins_with(destination, path)) throw std::invalid_argument("cannot move a group into itself");
    const HotlistPath parent(path.begin(), path.end() - 1);
    const auto& items = group(parent);
    const auto& target = group(destination);
    const auto source_index = path.back();
    const auto position = before.value_or(target.size());
    if (position > target.size()) throw std::out_of_range("hotlist insertion position is out of range");
    auto moving = items.at(source_index);
    auto candidate = *this;
    candidate.erase(path);
    auto adjusted = position;
    if (parent == destination && source_index < position) --adjusted;
    if (destination.size() > parent.size() && begins_with(destination, parent) &&
        destination[parent.size()] > source_index) {
        --destination[parent.size()];
    }
    candidate.insert(destination, std::move(moving), adjusted);
    *this = std::move(candidate);
    destination.push_back(adjusted);
    return destination;
}

}  // namespace mcpp
