#include "mcpp/local_provider.hpp"
#include <iostream>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <fcntl.h>
#include <grp.h>
#include <linux/fs.h>
#include <limits>
#include <pwd.h>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

namespace mcpp {
namespace {

struct OutputMetadata {
    struct stat status{};
    std::vector<std::pair<std::string, std::vector<char>>> attributes;
};

std::optional<OutputMetadata> output_metadata(const std::filesystem::path& path) {
    OutputMetadata result;
    if (::lstat(path.c_str(), &result.status) != 0) {
        if (errno == ENOENT || errno == ENOTDIR) {
            return std::nullopt;
        }
        throw std::system_error(errno, std::generic_category(), "inspect output metadata");
    }
    if (!S_ISREG(result.status.st_mode)) {
        return std::nullopt;
    }
    const auto count = listxattr(path.c_str(), nullptr, 0);
    if (count < 0) {
        if (errno == ENOTSUP || errno == EOPNOTSUPP) {
            return result;
        }
        throw std::system_error(errno, std::generic_category(), "read output attribute list");
    }
    std::vector<char> names(static_cast<std::size_t>(count));
    if (count > 0 && listxattr(path.c_str(), names.data(), names.size()) != count) {
        throw std::runtime_error("output attributes changed while saving: " + path.string());
    }
    for (std::size_t offset = 0; offset < names.size();) {
        const std::string name(names.data() + offset);
        offset += name.size() + 1U;
        if (!name.starts_with("user.") && name != "system.posix_acl_access") {
            continue;
        }
        const auto size = getxattr(path.c_str(), name.c_str(), nullptr, 0);
        if (size < 0) {
            throw std::system_error(errno, std::generic_category(), "read output attribute");
        }
        std::vector<char> value(static_cast<std::size_t>(size));
        if (getxattr(path.c_str(), name.c_str(), value.data(), value.size()) != size) {
            throw std::runtime_error("output attribute changed while saving: " + name);
        }
        result.attributes.emplace_back(name, std::move(value));
    }
    return result;
}

mode_t creation_mode() {
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.starts_with("Umask:")) {
            std::istringstream input(line.substr(6));
            unsigned int mask = 0;
            input >> std::oct >> mask;
            if (input && mask <= 0777U) {
                return static_cast<mode_t>(0666U & ~mask);
            }
            break;
        }
    }
    throw std::runtime_error("cannot read the process umask from /proc/self/status");
}

bool path_is_within(
    const std::filesystem::path& root,
    const std::filesystem::path& candidate
) {
    auto root_it = root.begin();
    auto candidate_it = candidate.begin();
    for (; root_it != root.end() && candidate_it != candidate.end();
         ++root_it, ++candidate_it) {
        if (*root_it != *candidate_it) {
            return false;
        }
    }
    return root_it == root.end();
}

bool lstat_if_present(
    const std::filesystem::path& path,
    struct stat& status
) {
    if (::lstat(path.c_str(), &status) == 0) {
        return true;
    }
    const auto failure = errno;
    if (failure == ENOENT || failure == ENOTDIR) {
        return false;
    }
    throw std::filesystem::filesystem_error(
        "cannot inspect file identity",
        path,
        std::error_code(failure, std::generic_category())
    );
}

std::string revision_for(const struct stat& status) {
    return std::to_string(status.st_dev) + ":" +
           std::to_string(status.st_ino) + ":" +
           std::to_string(status.st_mtim.tv_sec) + ":" +
           std::to_string(status.st_mtim.tv_nsec) + ":" +
           std::to_string(status.st_size) + ":" +
           std::to_string(status.st_ctim.tv_sec) + ":" +
           std::to_string(status.st_ctim.tv_nsec);
}

std::optional<struct stat> destination_status(const std::filesystem::path& path) {
    struct stat status{};
    return lstat_if_present(path, status) ? std::optional{status} : std::nullopt;
}

void validate_copied_source(
    const std::filesystem::path& path, const struct stat& expected,
    const int descriptor = -1
) {
    const auto matches = [&](const struct stat& current) {
        return revision_for(current) == revision_for(expected) &&
               current.st_mode == expected.st_mode && current.st_rdev == expected.st_rdev;
    };
    struct stat current{};
    if (!lstat_if_present(path, current) || !matches(current) ||
        (descriptor >= 0 && (fstat(descriptor, &current) != 0 || !matches(current)))) {
        throw std::runtime_error("source changed while copying: " + path.string());
    }
}

std::chrono::system_clock::time_point modified_for(const struct stat& status) {
    return std::chrono::system_clock::time_point{
        std::chrono::seconds(status.st_mtim.tv_sec) +
        std::chrono::nanoseconds(status.st_mtim.tv_nsec)
    };
}

std::array<timespec, 2> modification_times(
    const std::chrono::system_clock::time_point modified
) {
    const auto seconds = std::chrono::floor<std::chrono::seconds>(modified.time_since_epoch());
    const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
        modified.time_since_epoch() - seconds
    );
    return {{{.tv_sec = 0, .tv_nsec = UTIME_OMIT},
             {.tv_sec = static_cast<time_t>(seconds.count()), .tv_nsec = static_cast<long>(nanos.count())}}};
}

class DirectorySync final {
public:
    explicit DirectorySync(const std::filesystem::path& path)
        : path_(path),
          descriptor_(open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)) {
        if (descriptor_ < 0) {
            throw std::system_error(errno, std::generic_category(), "open output directory: " + path.string());
        }
    }

    ~DirectorySync() { static_cast<void>(close(descriptor_)); }
    DirectorySync(const DirectorySync&) = delete;
    DirectorySync& operator=(const DirectorySync&) = delete;

    void sync() const {
        if (fsync(descriptor_) != 0) {
            throw std::system_error(errno, std::generic_category(), "sync output directory: " + path_.string());
        }
    }

    void sync_published(const std::filesystem::path& destination) const {
        try {
            sync();
        } catch (const std::system_error& error) {
            throw std::system_error(
                error.code(), "destination was published but directory sync failed: " + destination.string()
            );
        }
    }

private:
    std::filesystem::path path_;
    int descriptor_;
};

std::string identity_name(
    const uid_t uid,
    const gid_t gid,
    const bool owner
) {
    const auto requested_size =
        sysconf(owner ? _SC_GETPW_R_SIZE_MAX : _SC_GETGR_R_SIZE_MAX);
    const auto buffer_size = static_cast<std::size_t>(
        requested_size > 0 ? requested_size : 16384
    );
    std::vector<char> buffer(buffer_size);
    if (owner) {
        passwd entry{};
        passwd* result = nullptr;
        if (getpwuid_r(
                uid,
                &entry,
                buffer.data(),
                buffer.size(),
                &result
            ) == 0 &&
            result != nullptr) {
            return entry.pw_name;
        }
        return std::to_string(uid);
    }
    group entry{};
    group* result = nullptr;
    if (getgrgid_r(
            gid,
            &entry,
            buffer.data(),
            buffer.size(),
            &result
        ) == 0 &&
        result != nullptr) {
        return entry.gr_name;
    }
    return std::to_string(gid);
}

bool rename_no_replace(
    const std::filesystem::path& source,
    const std::filesystem::path& destination
) {
    if (syscall(
            SYS_renameat2,
            AT_FDCWD,
            source.c_str(),
            AT_FDCWD,
            destination.c_str(),
            RENAME_NOREPLACE
        ) == 0) {
        return true;
    }
    if (errno == EEXIST) {
        return false;
    }
    throw std::filesystem::filesystem_error(
        "rename",
        source,
        destination,
        std::error_code(errno, std::generic_category())
    );
}

void publish_output(
    const std::filesystem::path& temporary,
    const std::filesystem::path& destination,
    const bool overwrite,
    const std::optional<struct stat>& expected
) {
    if (overwrite && expected) {
        struct stat current{};
        if (!lstat_if_present(destination, current) ||
            revision_for(current) != revision_for(*expected)) {
            throw std::runtime_error("destination changed while copying or saving: " + destination.string());
        }
        std::filesystem::rename(temporary, destination);
    } else if (!rename_no_replace(temporary, destination)) {
        throw std::runtime_error("destination exists: " + destination.string());
    }
}

int open_regular_file(const std::filesystem::path& path) {
    const int descriptor = open(
        path.c_str(),
        O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC
    );
    if (descriptor < 0) {
        throw std::runtime_error(
            "cannot open regular file: " + path.string() + ": " +
            std::string(std::strerror(errno))
        );
    }
    struct stat status {};
    if (fstat(descriptor, &status) != 0 ||
        !S_ISREG(status.st_mode)) {
        const auto failure = errno;
        close(descriptor);
        throw std::runtime_error(
            "not a regular file: " + path.string() +
            (failure == 0
                 ? ""
                 : ": " + std::string(std::strerror(failure)))
        );
    }
    return descriptor;
}

bool unsupported_extent_seek(const int error) {
    return error == EINVAL || error == ENOTSUP || error == EOPNOTSUPP || error == ENOSYS;
}

std::uint64_t copy_sparse_data(
    const int source, const int destination, const off_t size,
    const bool preallocate, const Cancellation& cancellation,
    const ProgressCallback& progress, const std::filesystem::path& destination_path
) {
    std::array<char, 64U * 1024U> buffer{};
    off_t position = 0;
    bool seek_extents = true;
    const auto report = [&] {
        if (progress) {
            progress({
                .bytes_done = static_cast<std::uint64_t>(position),
                .bytes_total = static_cast<std::uint64_t>(size),
                .files_total = 1,
                .current_path = destination_path.string(),
            });
        }
        cancellation.throw_if_requested();
    };
    while (position < size) {
        cancellation.throw_if_requested();
        off_t end = size;
        if (seek_extents) {
            const auto data = lseek(source, position, SEEK_DATA);
            if (data < 0) {
                const auto error = errno;
                if (error == EINTR) continue;
                if (error == ENXIO) {
                    position = size;
                    report();
                    break;
                }
                if (unsupported_extent_seek(error)) {
                    seek_extents = false;
                    continue;
                }
                throw std::system_error(error, std::generic_category(), "seek copied source data");
            }
            if (data < position || data > size) {
                throw std::runtime_error("source extent changed while copying");
            }
            if (data != position) {
                position = data;
                report();
            }
            if (position == size) break;
            end = lseek(source, position, SEEK_HOLE);
            if (end < 0) {
                const auto error = errno;
                if (error == EINTR) continue;
                if (!unsupported_extent_seek(error)) {
                    throw std::system_error(error, std::generic_category(), "seek copied source hole");
                }
                seek_extents = false;
                end = size;
            }
            if (end <= position) {
                throw std::runtime_error("source extent changed while copying");
            }
            end = std::min(end, size);
        }
        while (position < end) {
            cancellation.throw_if_requested();
            const auto requested = static_cast<std::size_t>(
                std::min<off_t>(end - position, static_cast<off_t>(buffer.size()))
            );
            const auto count = pread(source, buffer.data(), requested, position);
            if (count < 0 && errno == EINTR) continue;
            if (count < 0) {
                throw std::system_error(errno, std::generic_category(), "read copied source");
            }
            if (count == 0) {
                throw std::runtime_error("source ended before expected length while copying");
            }
            const auto bytes = static_cast<std::size_t>(count);
            // Also retain holes when SEEK_HOLE is unavailable or reports a dense extent.
            const auto zero = [&](const std::size_t begin, const std::size_t end_offset) {
                return std::all_of(buffer.data() + begin, buffer.data() + end_offset, [](const char byte) {
                    return byte == '\0';
                });
            };
            const auto block_end = [&](const std::size_t offset) {
                constexpr off_t block_size = 4096;
                const auto remainder = (position + static_cast<off_t>(offset)) % block_size;
                return std::min(bytes, offset + static_cast<std::size_t>(block_size - remainder));
            };
            std::size_t offset = 0;
            while (offset < bytes) {
                auto end_offset = block_end(offset);
                if (zero(offset, end_offset)) {
                    offset = end_offset;
                    continue;
                }
                const auto begin = offset;
                offset = end_offset;
                while (offset < bytes) {
                    end_offset = block_end(offset);
                    if (zero(offset, end_offset)) break;
                    offset = end_offset;
                }
                if (preallocate) {
                    const auto error = posix_fallocate(
                        destination, position + static_cast<off_t>(begin), static_cast<off_t>(offset - begin)
                    );
                    if (error != 0) {
                        throw std::system_error(error, std::generic_category(), "preallocate copied data");
                    }
                }
                auto written_offset = begin;
                while (written_offset < offset) {
                    cancellation.throw_if_requested();
                    const auto written = pwrite(
                        destination, buffer.data() + written_offset, offset - written_offset,
                        position + static_cast<off_t>(written_offset)
                    );
                    if (written < 0 && errno == EINTR) continue;
                    if (written <= 0) {
                        throw std::system_error(
                            written == 0 ? EIO : errno, std::generic_category(), "write copied data"
                        );
                    }
                    written_offset += static_cast<std::size_t>(written);
                }
            }
            position += count;
            report();
        }
    }
    cancellation.throw_if_requested();
    if (ftruncate(destination, size) != 0) {
        throw std::system_error(errno, std::generic_category(), "set copied file size");
    }
    return static_cast<std::uint64_t>(size);
}

void remove_tree_at(
    const int parent_descriptor,
    const std::string& name,
    const std::filesystem::path& display_path,
    const Cancellation& cancellation,
    std::uint64_t& removed,
    const ProgressCallback& progress
) {
    cancellation.throw_if_requested();
    struct stat status {};
    if (fstatat(
            parent_descriptor,
            name.c_str(),
            &status,
            AT_SYMLINK_NOFOLLOW
        ) != 0) {
        throw std::runtime_error(
            "cannot inspect before removal: " +
            display_path.string() + ": " +
            std::string(std::strerror(errno))
        );
    }
    if (S_ISDIR(status.st_mode)) {
        const int child_descriptor = openat(
            parent_descriptor,
            name.c_str(),
            O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC
        );
        if (child_descriptor < 0) {
            throw std::runtime_error(
                "cannot open directory for removal: " +
                display_path.string() + ": " +
                std::string(std::strerror(errno))
            );
        }
        DIR* directory = fdopendir(child_descriptor);
        if (directory == nullptr) {
            const auto failure = errno;
            close(child_descriptor);
            throw std::runtime_error(
                "cannot enumerate directory for removal: " +
                display_path.string() + ": " +
                std::string(std::strerror(failure))
            );
        }
        try {
            while (true) {
                errno = 0;
                const auto* item = readdir(directory);
                if (item == nullptr) {
                    if (errno != 0) {
                        throw std::runtime_error(
                            "cannot enumerate directory for removal: " +
                            display_path.string()
                        );
                    }
                    break;
                }
                const std::string child_name(item->d_name);
                if (child_name == "." || child_name == "..") {
                    continue;
                }
                remove_tree_at(
                    dirfd(directory),
                    child_name,
                    display_path / child_name,
                    cancellation,
                    removed,
                    progress
                );
            }
        } catch (...) {
            closedir(directory);
            throw;
        }
        closedir(directory);
    }
    cancellation.throw_if_requested();
    if (unlinkat(
            parent_descriptor,
            name.c_str(),
            S_ISDIR(status.st_mode) ? AT_REMOVEDIR : 0
        ) != 0) {
        throw std::runtime_error(
            "cannot remove " + display_path.string() + ": " +
            std::string(std::strerror(errno))
        );
    }
    ++removed;
    if (progress) {
        progress({
            .files_done = removed,
            .current_path = display_path.string(),
        });
    }
}

}  // namespace

LocalProvider::LocalProvider(std::filesystem::path root) {
    std::error_code error;
    root_ = std::filesystem::weakly_canonical(
        std::filesystem::absolute(std::move(root)),
        error
    );
    if (error || !std::filesystem::is_directory(root_)) {
        throw std::invalid_argument(
            "local provider root is not a directory"
        );
    }
}

std::string LocalProvider::id() const {
    return "local:" + root_.string();
}

std::string LocalProvider::display_name() const {
    return "Local";
}

ProviderCapability LocalProvider::capabilities() const {
    return ProviderCapability::read |
           ProviderCapability::write |
           ProviderCapability::remove |
           ProviderCapability::rename |
           ProviderCapability::direct_copy |
           ProviderCapability::links |
           ProviderCapability::permissions;
}

std::filesystem::path LocalProvider::resolve(
    const std::string_view path
) const {
    std::filesystem::path requested(path);
    auto candidate = requested.is_absolute()
                         ? requested
                         : root_ / requested;
    candidate = candidate.lexically_normal();

    if (candidate == root_) {
        return root_;
    }
    std::error_code error;
    const auto parent = std::filesystem::weakly_canonical(
        candidate.parent_path(),
        error
    );
    if (error) {
        throw std::runtime_error(
            "cannot resolve local path: " + candidate.string()
        );
    }
    const auto resolved =
        (parent / candidate.filename()).lexically_normal();
    if (!path_is_within(root_, resolved)) {
        throw std::invalid_argument(
            "path escapes provider root: " + candidate.string()
        );
    }
    return resolved;
}

std::string LocalProvider::normalize(const std::string_view path) const {
    return resolve(path).string();
}

std::filesystem::path LocalProvider::native_path(const std::string_view path) const {
    return resolve_follow(path);
}

std::filesystem::path LocalProvider::resolve_follow(
    const std::string_view path
) const {
    const auto lexical = resolve(path);
    std::error_code error;
    const auto canonical =
        std::filesystem::weakly_canonical(lexical, error);
    if (error) {
        throw std::runtime_error(
            "cannot resolve local path: " + lexical.string()
        );
    }
    if (!path_is_within(root_, canonical)) {
        throw std::invalid_argument(
            "path resolves outside provider root: " +
            lexical.string()
        );
    }
    return canonical;
}

std::string LocalProvider::join(
    const std::string_view directory,
    const std::string_view name
) const {
    return resolve(
        (std::filesystem::path(directory) /
         std::filesystem::path(name)).string()
    ).string();
}

std::string LocalProvider::parent(const std::string_view path) const {
    const auto current = resolve(path);
    if (current == root_) {
        return root_.string();
    }
    const auto result = current.parent_path();
    return path_is_within(root_, result)
               ? result.string()
               : root_.string();
}

std::string LocalProvider::basename(const std::string_view path) const {
    return resolve(path).filename().string();
}

Entry LocalProvider::make_entry(
    const std::filesystem::directory_entry& directory_entry
) const {
    Entry result;
    result.name = directory_entry.path().filename().string();
    result.path = directory_entry.path().string();
    result.hidden = result.name.starts_with('.');

    std::error_code error;
    const auto status = directory_entry.symlink_status(error);
    if (error) {
        throw std::runtime_error(
            "cannot inspect " + result.path + ": " + error.message()
        );
    }
    result.permissions = status.permissions();
    if (std::filesystem::is_symlink(status)) {
        result.kind = EntryKind::symlink;
        result.target =
            std::filesystem::read_symlink(directory_entry.path(), error)
                .string();
        if (error) {
            result.target.clear();
        }
        error.clear();
        result.target_is_directory =
            directory_entry.is_directory(error) && !error;
    } else if (std::filesystem::is_directory(status)) {
        result.kind = EntryKind::directory;
    } else if (std::filesystem::is_regular_file(status)) {
        result.kind = EntryKind::file;
        result.size = directory_entry.file_size(error);
        if (error) {
            result.size = 0;
        }
    } else {
        result.kind = EntryKind::other;
    }

    struct stat native_status {};
    if (lstat(directory_entry.path().c_str(), &native_status) == 0) {
        result.size = static_cast<std::uint64_t>(
            std::max<off_t>(native_status.st_size, 0)
        );
        result.owner = identity_name(
            native_status.st_uid,
            native_status.st_gid,
            true
        );
        result.group = identity_name(
            native_status.st_uid,
            native_status.st_gid,
            false
        );
        result.modified =
            std::chrono::system_clock::time_point{
                std::chrono::seconds(native_status.st_mtim.tv_sec) +
                std::chrono::nanoseconds(native_status.st_mtim.tv_nsec)
            };
        result.accessed = std::chrono::system_clock::time_point{
            std::chrono::seconds(native_status.st_atim.tv_sec) +
            std::chrono::nanoseconds(native_status.st_atim.tv_nsec)};
        result.changed = std::chrono::system_clock::time_point{
            std::chrono::seconds(native_status.st_ctim.tv_sec) +
            std::chrono::nanoseconds(native_status.st_ctim.tv_nsec)};
        result.inode = static_cast<std::uint64_t>(native_status.st_ino);
        result.links = static_cast<std::uint64_t>(native_status.st_nlink);
        result.uid = static_cast<std::uint32_t>(native_status.st_uid);
        result.gid = static_cast<std::uint32_t>(native_status.st_gid);
        result.mode = static_cast<std::uint32_t>(native_status.st_mode);
        result.revision = revision_for(native_status);
    }
    return result;
}

std::vector<Entry> LocalProvider::list(
    const std::string_view path,
    const ListRequest& request,
    const Cancellation& cancellation
) const {
    const auto directory = resolve_follow(path);
    std::vector<Entry> entries;
    std::error_code error;
    for (std::filesystem::directory_iterator iterator(
             directory,
             std::filesystem::directory_options::none,
             error
         ),
         end;
         iterator != end;
         iterator.increment(error)) {
        cancellation.throw_if_requested();
        if (error) {
            throw std::runtime_error(
                "cannot list " + directory.string() + ": " +
                error.message()
            );
        }
        auto entry = make_entry(*iterator);
        if (request.show_hidden || !entry.hidden) {
            entries.push_back(std::move(entry));
        }
    }
    if (error) {
        throw std::runtime_error(
            "cannot list " + directory.string() + ": " +
            error.message()
        );
    }

    sort_entries(entries, request);

    const auto begin = std::min(request.offset, entries.size());
    const auto end =
        request.limit == 0U
            ? entries.size()
            : std::min(entries.size(), begin + request.limit);
    return {
        std::make_move_iterator(entries.begin() +
                                static_cast<std::ptrdiff_t>(begin)),
        std::make_move_iterator(entries.begin() +
                                static_cast<std::ptrdiff_t>(end)),
    };
}

Entry LocalProvider::stat(
    const std::string_view path,
    const Cancellation& cancellation
) const {
    cancellation.throw_if_requested();
    const auto resolved = resolve(path);
    if (!std::filesystem::exists(resolved) &&
        !std::filesystem::is_symlink(
            std::filesystem::symlink_status(resolved)
        )) {
        throw std::runtime_error(
            "path does not exist: " + resolved.string()
        );
    }
    return make_entry(std::filesystem::directory_entry(resolved));
}

Entry LocalProvider::stat_follow(
    const std::string_view path,
    const Cancellation& cancellation
) const {
    cancellation.throw_if_requested();
    return make_entry(
        std::filesystem::directory_entry(resolve_follow(path))
    );
}

bool LocalProvider::exists(const std::string_view path) const {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(
        resolve(path),
        error
    );
    return !error &&
           status.type() != std::filesystem::file_type::not_found;
}

SpaceInfo LocalProvider::space(const std::string_view path) const {
    const auto information = std::filesystem::space(
        resolve_follow(path)
    );
    return {
        .capacity = information.capacity,
        .free = information.free,
        .available = information.available,
    };
}

void LocalProvider::read(
    const std::string_view path,
    const std::uint64_t offset,
    const std::uint64_t length,
    std::ostream& output,
    const Cancellation& cancellation
) const {
    const auto source = resolve_follow(path);
    const int descriptor = open_regular_file(source);
    std::array<char, 64U * 1024U> buffer{};
    auto remaining = length;
    std::uint64_t position = offset;
    try {
        while (remaining > 0U) {
            cancellation.throw_if_requested();
            const auto requested = static_cast<std::size_t>(
                std::min<std::uint64_t>(remaining, buffer.size())
            );
            const auto count = pread(
                descriptor,
                buffer.data(),
                requested,
                static_cast<off_t>(position)
            );
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count < 0) {
                throw std::runtime_error(
                    "failed while reading " + source.string()
                );
            }
            if (count == 0) {
                break;
            }
            output.write(buffer.data(), count);
            if (!output.good()) {
                throw std::runtime_error("failed while writing file data");
            }
            remaining -= static_cast<std::uint64_t>(count);
            position += static_cast<std::uint64_t>(count);
        }
    } catch (...) {
        close(descriptor);
        throw;
    }
    close(descriptor);
}

void LocalProvider::write(
    const std::string_view path,
    std::istream& input,
    const std::optional<std::uint64_t> length,
    const bool overwrite,
    const std::optional<std::string> expected_revision,
    const Cancellation& cancellation,
    const ProgressCallback& progress
) {
    write_impl(path, input, length, overwrite, expected_revision, cancellation, progress, std::nullopt);
}

void LocalProvider::write_with_permissions(
    const std::string_view path, std::istream& input,
    const std::optional<std::uint64_t> length, const bool overwrite,
    const Cancellation& cancellation, const ProgressCallback& progress,
    const std::filesystem::perms permissions
) {
    if (permissions == std::filesystem::perms::unknown) {
        throw std::invalid_argument("copy permissions are unknown");
    }
    write_impl(path, input, length, overwrite, std::nullopt, cancellation, progress, permissions);
}

void LocalProvider::write_impl(
    const std::string_view path, std::istream& input,
    const std::optional<std::uint64_t> length, const bool overwrite,
    const std::optional<std::string> expected_revision,
    const Cancellation& cancellation, const ProgressCallback& progress,
    const std::optional<std::filesystem::perms> permissions
) {
    cancellation.throw_if_requested();
    const auto destination = normalize(path);
    write_staged(
        destination, length, overwrite, expected_revision, cancellation, progress, permissions,
        [&](const int descriptor) {
            std::array<char, 64U * 1024U> buffer{};
            std::uint64_t written = 0;
            if (preallocate_space_.load(std::memory_order_relaxed) && length && *length > 0U) {
                cancellation.throw_if_requested();
                if (*length > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
                    throw std::runtime_error("output is too large to preallocate");
                }
                const auto error = posix_fallocate(descriptor, 0, static_cast<off_t>(*length));
                if (error != 0) throw std::system_error(error, std::generic_category(), "preallocate output");
            }
            while (input) {
                cancellation.throw_if_requested();
                input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                const auto count = input.gcount();
                if (count <= 0) {
                    if (input.bad() || (!input.eof() && input.fail())) {
                        throw std::runtime_error("failed while reading source data");
                    }
                    break;
                }
                std::streamsize offset = 0;
                while (offset < count) {
                    cancellation.throw_if_requested();
                    const auto result = ::write(
                        descriptor, buffer.data() + offset, static_cast<std::size_t>(count - offset)
                    );
                    if (result < 0 && errno == EINTR) continue;
                    if (result <= 0) {
                        throw std::runtime_error("failed while writing " + destination);
                    }
                    offset += result;
                }
                written += static_cast<std::uint64_t>(count);
                if (progress) {
                    progress({
                        .bytes_done = written,
                        .bytes_total = length.value_or(0),
                        .files_total = 1,
                        .current_path = destination,
                    });
                }
                cancellation.throw_if_requested();
            }
            return written;
        }
    );
}

void LocalProvider::write_staged(
    const std::string_view path, const std::optional<std::uint64_t> length,
    const bool overwrite, const std::optional<std::string> expected_revision,
    const Cancellation& cancellation, const ProgressCallback& progress,
    const std::optional<std::filesystem::perms> permissions,
    const std::function<std::uint64_t(int)>& transfer,
    const std::function<void()>& validate_source,
    const std::optional<std::chrono::system_clock::time_point> modified
) {
    cancellation.throw_if_requested();
    const auto destination = resolve(path);
    const auto original = destination_status(destination);
    if (!overwrite && original) {
        throw std::runtime_error("destination exists: " + destination.string());
    }
    const auto metadata = output_metadata(destination);
    if (metadata && (!original || revision_for(metadata->status) != revision_for(*original))) {
        throw std::runtime_error("destination changed before copying or saving: " + destination.string());
    }
    const auto destination_permissions =
        permissions ? static_cast<mode_t>(static_cast<unsigned int>(*permissions) & 07777U)
                    : metadata ? metadata->status.st_mode & 07777U : creation_mode();
    if (expected_revision) {
        const auto current = stat(destination.string(), cancellation);
        if (current.revision != *expected_revision) {
            throw std::runtime_error(
                "destination changed since it was opened: " +
                destination.string()
            );
        }
    }
    std::filesystem::create_directories(destination.parent_path());
    const DirectorySync parent_directory(destination.parent_path());
    auto pattern =
        (destination.parent_path() / ".mcpp-write-XXXXXX").string();
    std::vector<char> temporary_buffer(pattern.begin(), pattern.end());
    temporary_buffer.push_back('\0');
    int descriptor = mkostemp(temporary_buffer.data(), O_CLOEXEC);
    if (descriptor < 0) {
        throw std::runtime_error(
            "cannot create temporary output: " +
            std::string(std::strerror(errno))
        );
    }
    const std::filesystem::path temporary(temporary_buffer.data());
    try {
        if (metadata &&
            (metadata->status.st_uid != geteuid() || metadata->status.st_gid != getegid()) &&
            fchown(descriptor, metadata->status.st_uid, metadata->status.st_gid) != 0) {
            throw std::system_error(errno, std::generic_category(), "preserve output ownership");
        }
        if (metadata && fremovexattr(descriptor, "system.posix_acl_access") != 0 &&
            errno != ENODATA && errno != ENOTSUP && errno != EOPNOTSUPP) {
            throw std::system_error(errno, std::generic_category(), "clear inherited output ACL");
        }
        if (metadata) {
            for (const auto& [name, value] : metadata->attributes) {
                if (fsetxattr(descriptor, name.c_str(), value.data(), value.size(), 0) != 0) {
                    throw std::system_error(
                        errno, std::generic_category(), "preserve output attribute " + name
                    );
                }
            }
        }
        if (fchmod(descriptor, static_cast<mode_t>(destination_permissions)) != 0) {
            throw std::system_error(errno, std::generic_category(), "set temporary output permissions");
        }
        const auto written = transfer(descriptor);
        if (length && written != *length) {
            throw std::runtime_error(
                "source ended before expected length"
            );
        }
        cancellation.throw_if_requested();
        if (validate_source) validate_source();
        if (fchmod(descriptor, static_cast<mode_t>(destination_permissions)) != 0) {
            throw std::system_error(errno, std::generic_category(), "restore output permissions after writing");
        }
        if (modified) {
            const auto times = modification_times(*modified);
            if (futimens(descriptor, times.data()) != 0) {
                throw std::system_error(errno, std::generic_category(), "preserve copied modification time");
            }
        }
        if (fsync(descriptor) != 0) {
            throw std::system_error(
                errno, std::generic_category(), "failed to commit " + temporary.string()
            );
        }
        cancellation.throw_if_requested();
        if (close(descriptor) != 0) {
            descriptor = -1;
            throw std::runtime_error(
                "failed to close " + temporary.string()
            );
        }
        descriptor = -1;
        cancellation.throw_if_requested();
        if (progress) {
            progress({
                .bytes_done = written,
                .bytes_total = length.value_or(0),
                .files_done = 0,
                .files_total = 1,
                .current_path = destination.string(),
            });
        }
        cancellation.throw_if_requested();
        if (validate_source) validate_source();
        if (overwrite && expected_revision) {
            const auto current = stat(destination.string(), cancellation);
            if (current.revision != *expected_revision) {
                throw std::runtime_error("destination changed while saving: " + destination.string());
            }
        }
        publish_output(temporary, destination, overwrite, original);
        parent_directory.sync_published(destination);
    } catch (...) {
        if (descriptor >= 0) {
            static_cast<void>(close(descriptor));
        }
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}

std::uintmax_t LocalProvider::hardlink_count(const std::string_view path) const {
    return std::filesystem::hard_link_count(resolve_follow(path));
}

void LocalProvider::ensure_writable(const std::string_view path) const {
    const auto target = resolve_follow(path);
    if (faccessat(AT_FDCWD, target.c_str(), W_OK, AT_EACCESS) != 0) {
        throw std::system_error(
            errno, std::generic_category(), "file is not writable: " + target.string()
        );
    }
}

void LocalProvider::replace_symlink(
    const std::string_view path, const std::string_view target,
    const std::string& expected_revision
) {
    if (target.empty() || target.find('\0') != std::string_view::npos) {
        throw std::invalid_argument("symlink target is empty or contains NUL");
    }
    const auto original = resolve(path);
    const auto entry = stat(path, {});
    if (entry.kind != EntryKind::symlink || entry.revision != expected_revision) {
        throw std::runtime_error("symlink changed before editing: " + original.string());
    }
    auto pattern = (original.parent_path() / ".mcpp-link-XXXXXX").string();
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back('\0');
    const auto descriptor = mkstemp(name.data());
    if (descriptor < 0) throw std::system_error(errno, std::generic_category(), "create replacement symlink");
    const std::filesystem::path temporary(name.data());
    const auto closed = close(descriptor);
    try {
        if (closed != 0) throw std::system_error(errno, std::generic_category(), "close symlink temporary");
        std::filesystem::remove(temporary);
        std::filesystem::create_symlink(std::string(target), temporary);
        if (stat(path, {}).revision != expected_revision) {
            throw std::runtime_error("symlink changed while awaiting replacement");
        }
        std::filesystem::rename(temporary, original);
    } catch (...) {
        std::error_code error;
        std::filesystem::remove(temporary, error);
        if (error) std::cerr << "mcpp: symlink temporary cleanup failed: " << error.message() << '\n';
        throw;
    }
}

void LocalProvider::set_modified(
    const std::string_view path,
    const std::chrono::system_clock::time_point modified
) {
    const auto target = resolve(path);
    const auto times = modification_times(modified);
    if (utimensat(AT_FDCWD, target.c_str(), times.data(), AT_SYMLINK_NOFOLLOW) != 0) {
        throw std::system_error(errno, std::generic_category(), "preserve modification time");
    }
}

void LocalProvider::write_in_place(
    const std::string_view path,
    std::istream& input,
    const std::uint64_t length,
    const std::optional<std::string>& expected_revision,
    const Cancellation& cancellation
) {
    const auto target = resolve_follow(path);
    ensure_writable(path);
    cancellation.throw_if_requested();
    if (expected_revision && stat(target.string(), cancellation).revision != *expected_revision) {
        throw std::runtime_error("destination changed since it was opened: " + target.string());
    }
    const int descriptor = open(target.c_str(), O_WRONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (descriptor < 0) {
        throw std::system_error(errno, std::generic_category(), "open linked file for writing");
    }
    try {
        struct stat current{};
        if (fstat(descriptor, &current) != 0 || !S_ISREG(current.st_mode)) {
            throw std::runtime_error("linked save requires a regular file");
        }
        if (expected_revision && revision_for(current) != *expected_revision) {
            throw std::runtime_error("destination changed while opening linked save: " + target.string());
        }
        std::array<char, 64U * 1024U> data{};
        std::uint64_t written = 0;
        while (written < length) {
            cancellation.throw_if_requested();
            const auto count = static_cast<std::streamsize>(
                std::min<std::uint64_t>(data.size(), length - written)
            );
            input.read(data.data(), count);
            if (input.gcount() != count) {
                throw std::runtime_error("linked save input ended unexpectedly");
            }
            std::streamsize offset = 0;
            while (offset < count) {
                const auto result = pwrite(
                    descriptor, data.data() + offset, static_cast<std::size_t>(count - offset),
                    static_cast<off_t>(written + static_cast<std::uint64_t>(offset))
                );
                if (result < 0 && errno == EINTR) {
                    continue;
                }
                if (result <= 0) {
                    throw std::system_error(errno, std::generic_category(), "write linked file");
                }
                offset += result;
            }
            written += static_cast<std::uint64_t>(count);
        }
        if (ftruncate(descriptor, static_cast<off_t>(length)) != 0 || fsync(descriptor) != 0) {
            throw std::system_error(errno, std::generic_category(), "commit linked save");
        }
    } catch (...) {
        close(descriptor);
        throw;
    }
    if (close(descriptor) != 0) {
        throw std::system_error(errno, std::generic_category(), "close linked save");
    }
}

void LocalProvider::create_directory(const std::string_view path) {
    std::error_code error;
    if (!std::filesystem::create_directories(resolve(path), error) &&
        error) {
        throw std::runtime_error(
            "cannot create directory: " + error.message()
        );
    }
}

void LocalProvider::copy_hardlink(
    const std::string_view source, const std::string_view destination,
    const bool overwrite, const Cancellation& cancellation
) {
    const auto from = resolve(source);
    const auto to = resolve(destination);
    cancellation.throw_if_requested();
    if (same_file(from.string(), *this, to.string())) {
        return;
    }
    struct stat original{};
    if (!lstat_if_present(from, original) ||
        (!S_ISREG(original.st_mode) && !S_ISFIFO(original.st_mode) &&
         !S_ISCHR(original.st_mode) && !S_ISBLK(original.st_mode))) {
        throw std::runtime_error("copied hardlink source has an unsupported file type");
    }
    const auto previous = destination_status(to);
    if (!overwrite && previous) throw std::runtime_error("destination exists: " + to.string());
    std::filesystem::create_directories(to.parent_path());
    const DirectorySync parent_directory(to.parent_path());
    auto pattern = (to.parent_path() / ".mcpp-hardlink-XXXXXX").string();
    if (mkdtemp(pattern.data()) == nullptr) {
        throw std::system_error(errno, std::generic_category(), "stage copied hardlink");
    }
    const std::filesystem::path directory(pattern);
    const auto temporary = directory / "link";
    try {
        std::filesystem::create_hard_link(from, temporary);
        struct stat staged{};
        if (!lstat_if_present(temporary, staged) ||
            staged.st_dev != original.st_dev || staged.st_ino != original.st_ino) {
            throw std::runtime_error("copied hardlink source changed during publication");
        }
        const DirectorySync staged_directory(directory);
        staged_directory.sync();
        cancellation.throw_if_requested();
        publish_output(temporary, to, overwrite, previous);
        std::filesystem::remove(directory);
        parent_directory.sync_published(to);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        std::filesystem::remove(directory, ignored);
        throw;
    }
}

void LocalProvider::remove(
    const std::string_view path,
    const bool recursive,
    const Cancellation& cancellation,
    const ProgressCallback& progress
) {
    cancellation.throw_if_requested();
    const auto target = resolve(path);
    if (target == root_) {
        throw std::invalid_argument(
            "refusing to remove provider root"
        );
    }
    std::uint64_t count = 0;
    if (recursive) {
        const auto parent = target.parent_path();
        const int parent_descriptor = open(
            parent.c_str(),
            O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC
        );
        if (parent_descriptor < 0) {
            throw std::runtime_error(
                "cannot open removal parent: " + parent.string()
            );
        }
        try {
            remove_tree_at(
                parent_descriptor,
                target.filename().string(),
                target,
                cancellation,
                count,
                progress
            );
        } catch (...) {
            close(parent_descriptor);
            throw;
        }
        close(parent_descriptor);
    } else {
        count = std::filesystem::remove(target) ? 1U : 0U;
    }
    if (count == 0U) {
        throw std::runtime_error(
            "nothing removed: " + target.string()
        );
    }
    if (progress && !recursive) {
        progress({
            .files_done = count,
            .files_total = count,
            .current_path = target.string(),
        });
    }
}

void LocalProvider::validate_directory_target(
    const std::string_view source,
    const LocalProvider& destination_provider,
    const std::string_view destination
) const {
    if (!std::filesystem::is_directory(
            std::filesystem::symlink_status(resolve(source))
        )) {
        return;
    }
    if (path_is_within(
            resolve_follow(source),
            destination_provider.resolve_follow(destination)
        )) {
        throw std::invalid_argument(
            "cannot move or copy a directory into itself"
        );
    }
}

bool LocalProvider::same_file(
    const std::string_view source,
    const LocalProvider& destination_provider,
    const std::string_view destination
) const {
    struct stat source_status {};
    struct stat destination_status {};
    if (!lstat_if_present(resolve(source), source_status) ||
        !lstat_if_present(
            destination_provider.resolve(destination),
            destination_status
        )) {
        return false;
    }
    return source_status.st_dev == destination_status.st_dev &&
           source_status.st_ino == destination_status.st_ino;
}

void LocalProvider::rename(
    const std::string_view source,
    const std::string_view destination,
    const bool overwrite
) {
    const auto source_path = resolve(source);
    const auto destination_path = resolve(destination);
    if (source_path == root_ || destination_path == root_) {
        throw std::invalid_argument(
            "refusing to rename provider root"
        );
    }
    if (source_path == destination_path) {
        return;
    }
    validate_directory_target(source, *this, destination);
    if (same_file(source, *this, destination)) {
        throw std::invalid_argument(
            "source and destination are the same file"
        );
    }
    if (!overwrite) {
        if (!rename_no_replace(source_path, destination_path)) {
            throw std::runtime_error(
                "destination exists: " + destination_path.string()
            );
        }
        return;
    }
    std::filesystem::rename(source_path, destination_path);
}

void LocalProvider::copy_file_from(
    const LocalProvider& source_provider,
    const std::string_view source, const std::string_view destination,
    const bool overwrite, const bool preserve_permissions,
    const Cancellation& cancellation, const ProgressCallback& progress
) {
    cancellation.throw_if_requested();
    const auto from = source_provider.resolve(source);
    const auto to = resolve(destination);
    if (to == root_) {
        throw std::invalid_argument("refusing to replace provider root with a copied file");
    }
    struct stat original{};
    if (!lstat_if_present(from, original)) {
        throw std::runtime_error("copy source does not exist: " + from.string());
    }
    if (!S_ISREG(original.st_mode) && !S_ISFIFO(original.st_mode) &&
        !S_ISCHR(original.st_mode) && !S_ISBLK(original.st_mode)) {
        throw std::runtime_error(
            std::string(S_ISSOCK(original.st_mode) ? "cannot copy Unix socket: " : "unsupported file type: ") +
            from.string()
        );
    }
    if (source_provider.same_file(from.string(), *this, to.string())) {
        throw std::invalid_argument("source and destination are the same file");
    }
    const auto permissions = preserve_permissions
        ? std::optional{static_cast<std::filesystem::perms>(original.st_mode & 07777U)}
        : std::nullopt;
    if (S_ISREG(original.st_mode)) {
        if (original.st_size < 0) throw std::runtime_error("copy source has a negative size");
        const int descriptor = open_regular_file(from);
        try {
            validate_copied_source(from, original, descriptor);
            write_staged(
                to.string(), static_cast<std::uint64_t>(original.st_size),
                overwrite, std::nullopt, cancellation, progress, permissions,
                [&](const int output) {
                    return copy_sparse_data(
                        descriptor, output, original.st_size,
                        preallocate_space_.load(std::memory_order_relaxed),
                        cancellation, progress, to
                    );
                },
                [&] { validate_copied_source(from, original, descriptor); },
                modified_for(original)
            );
        } catch (...) {
            static_cast<void>(close(descriptor));
            throw;
        }
        static_cast<void>(close(descriptor));
        return;
    }

    const auto previous = destination_status(to);
    if (!overwrite && previous) {
        throw std::runtime_error("destination exists: " + to.string());
    }
    const auto mode = permissions ? static_cast<mode_t>(*permissions)
        : previous && !S_ISLNK(previous->st_mode) ? previous->st_mode & 07777U : creation_mode();
    std::filesystem::create_directories(to.parent_path());
    const DirectorySync parent_directory(to.parent_path());
    auto pattern = (to.parent_path() / ".mcpp-special-XXXXXX").string();
    if (mkdtemp(pattern.data()) == nullptr) {
        throw std::system_error(errno, std::generic_category(), "stage copied special file");
    }
    const std::filesystem::path directory(pattern);
    const auto temporary = directory / "node";
    try {
        if (S_ISFIFO(original.st_mode)) {
            if (mkfifo(temporary.c_str(), 0600) != 0) {
                throw std::system_error(errno, std::generic_category(), "recreate FIFO: " + from.string());
            }
        } else if (mknod(temporary.c_str(), (original.st_mode & S_IFMT) | 0600U, original.st_rdev) != 0) {
            throw std::system_error(
                errno, std::generic_category(),
                "cannot recreate device " + from.string() + " (mknod requires permission, normally CAP_MKNOD)"
            );
        }
        if (previous && !S_ISLNK(previous->st_mode) &&
            (previous->st_uid != geteuid() || previous->st_gid != getegid()) &&
            lchown(temporary.c_str(), previous->st_uid, previous->st_gid) != 0) {
            throw std::system_error(errno, std::generic_category(), "preserve special output ownership");
        }
        if (chmod(temporary.c_str(), static_cast<mode_t>(mode)) != 0) {
            throw std::system_error(errno, std::generic_category(), "set copied special file permissions");
        }
        const auto times = modification_times(modified_for(original));
        if (utimensat(AT_FDCWD, temporary.c_str(), times.data(), AT_SYMLINK_NOFOLLOW) != 0) {
            throw std::system_error(errno, std::generic_category(), "preserve special file modification time");
        }
        const DirectorySync staged_directory(directory);
        staged_directory.sync();
        if (progress) {
            progress({.files_total = 1, .current_path = to.string()});
        }
        cancellation.throw_if_requested();
        validate_copied_source(from, original);
        publish_output(temporary, to, overwrite, previous);
        std::filesystem::remove(directory);
        parent_directory.sync_published(to);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        std::filesystem::remove(directory, ignored);
        throw;
    }
}

bool LocalProvider::copy_direct(
    const std::string_view source,
    const std::string_view destination,
    const bool overwrite,
    const Cancellation& cancellation,
    const ProgressCallback& progress
) {
    cancellation.throw_if_requested();
    const auto source_path = resolve(source);
    const auto destination_path = resolve(destination);
    if (!overwrite && exists(destination_path.string())) {
        throw std::runtime_error(
            "destination exists: " + destination_path.string()
        );
    }
    const auto source_entry = stat(source_path.string(), cancellation);
    if (source_entry.kind == EntryKind::directory) {
        validate_directory_target(source, *this, destination);
        const bool created = !exists(destination_path.string());
        bool copied_all = true;
        create_directory(destination_path.string());
        try {
            for (const auto& child :
                 list(
                     source_path.string(),
                     {.show_hidden = true},
                     cancellation
                 )) {
                try {
                    copied_all =
                        copy_direct(
                            child.path,
                            join(destination_path.string(), child.name),
                            overwrite,
                            cancellation,
                            progress
                        ) && copied_all;
                } catch (const SkipCurrent&) {
                    copied_all = false;
                    continue;
                }
            }
        } catch (...) {
            if (created) {
                std::error_code ignored;
                std::filesystem::remove_all(
                    destination_path,
                    ignored
                );
            }
            throw;
        }
        if (source_entry.permissions !=
            std::filesystem::perms::unknown) {
            set_permissions(
                destination_path.string(),
                source_entry.permissions
            );
        }
        return copied_all;
    }
    if (source_entry.kind == EntryKind::symlink) {
        if (progress) {
            progress({
                .files_done = 0,
                .files_total = 1,
                .current_path = destination_path.string(),
            });
        }
        create_symlink(
            source_entry.target,
            destination_path.string(),
            overwrite,
            cancellation
        );
        return true;
    }
    copy_file_from(*this, source_path.string(), destination_path.string(), overwrite, true, cancellation, progress);
    return true;
}

void LocalProvider::set_permissions(
    const std::string_view path,
    const std::filesystem::perms permissions
) {
    std::filesystem::permissions(
        resolve_follow(path),
        permissions,
        std::filesystem::perm_options::replace
    );
}

void LocalProvider::set_owner(
    const std::string_view path,
    const std::optional<std::string> owner,
    const std::optional<std::string> group
) {
    uid_t uid = static_cast<uid_t>(-1);
    gid_t gid = static_cast<gid_t>(-1);
    if (owner && !owner->empty()) {
        const auto* entry = getpwnam(owner->c_str());
        if (entry == nullptr) {
            throw std::invalid_argument(
                "unknown user: " + *owner
            );
        }
        uid = entry->pw_uid;
    }
    if (group && !group->empty()) {
        const auto* entry = getgrnam(group->c_str());
        if (entry == nullptr) {
            throw std::invalid_argument(
                "unknown group: " + *group
            );
        }
        gid = entry->gr_gid;
    }
    const auto target = resolve(path);
    if (lchown(target.c_str(), uid, gid) != 0) {
        throw std::runtime_error(
            "cannot change owner for " + target.string() + ": " +
            std::string(std::strerror(errno))
        );
    }
}

void LocalProvider::create_symlink(
    const std::string_view target,
    const std::string_view path,
    const bool overwrite,
    const Cancellation& cancellation
) {
    cancellation.throw_if_requested();
    const auto destination = resolve(path);
    if (destination == root_) {
        throw std::invalid_argument(
            "refusing to replace provider root with a symbolic link"
        );
    }
    std::filesystem::create_directories(destination.parent_path());
    auto pattern = (
        destination.parent_path() / ".mcpp-link-XXXXXX"
    ).string();
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    // Reserve the staging link name inside a private directory.
    if (mkdtemp(buffer.data()) == nullptr) {
        throw std::runtime_error(
            "cannot stage symbolic link: " +
            std::string(std::strerror(errno))
        );
    }
    const std::filesystem::path directory(buffer.data());
    const auto temporary = directory / "link";
    try {
        std::filesystem::create_symlink(
            std::filesystem::path(target),
            temporary
        );
        cancellation.throw_if_requested();
        rename(
            temporary.string(),
            destination.string(),
            overwrite
        );
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        std::filesystem::remove(directory, ignored);
        throw;
    }
    std::error_code ignored;
    std::filesystem::remove(directory, ignored);
}

void LocalProvider::create_hardlink(
    const std::string_view target,
    const std::string_view path
) {
    std::filesystem::create_hard_link(
        resolve(target),
        resolve(path)
    );
}

}  // namespace mcpp
