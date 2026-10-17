#include "mcpp/local_provider.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <fcntl.h>
#include <grp.h>
#include <linux/fs.h>
#include <pwd.h>
#include <sstream>
#include <stdexcept>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <unistd.h>

namespace mcpp {
namespace {

std::chrono::system_clock::time_point to_system_time(
    const std::filesystem::file_time_type value
) {
    return std::chrono::system_clock::now() +
           std::chrono::duration_cast<std::chrono::system_clock::duration>(
               value - std::filesystem::file_time_type::clock::now()
           );
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

std::string lowercase(std::string value) {
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](const unsigned char character) {
            return static_cast<char>(std::tolower(character));
        }
    );
    return value;
}

std::string extension_of(const std::string& name) {
    return lowercase(std::filesystem::path(name).extension().string());
}

std::string identity_name(
    const uid_t uid,
    const gid_t gid,
    const bool owner
) {
    if (owner) {
        if (const auto* entry = getpwuid(uid); entry != nullptr) {
            return entry->pw_name;
        }
        return std::to_string(uid);
    }
    if (const auto* entry = getgrgid(gid); entry != nullptr) {
        return entry->gr_name;
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
    return "local";
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

    const auto modified = directory_entry.last_write_time(error);
    if (!error) {
        result.modified = to_system_time(modified);
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
        result.revision =
            std::to_string(native_status.st_dev) + ":" +
            std::to_string(native_status.st_ino) + ":" +
            std::to_string(native_status.st_mtim.tv_sec) + ":" +
            std::to_string(native_status.st_mtim.tv_nsec) + ":" +
            std::to_string(native_status.st_size);
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
             std::filesystem::directory_options::skip_permission_denied,
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

    const auto compare = [&](const Entry& left, const Entry& right) {
        const auto left_directory =
            left.kind == EntryKind::directory ||
            left.target_is_directory;
        const auto right_directory =
            right.kind == EntryKind::directory ||
            right.target_is_directory;
        if (left_directory != right_directory) {
            return left_directory;
        }
        int comparison = 0;
        switch (request.sort) {
            case SortField::extension:
                comparison = extension_of(left.name).compare(
                    extension_of(right.name)
                );
                break;
            case SortField::size:
                comparison =
                    left.size < right.size
                        ? -1
                        : left.size > right.size ? 1 : 0;
                break;
            case SortField::modified:
                comparison =
                    left.modified < right.modified
                        ? -1
                        : left.modified > right.modified ? 1 : 0;
                break;
            case SortField::name:
                comparison = lowercase(left.name).compare(
                    lowercase(right.name)
                );
                break;
        }
        if (comparison == 0) {
            comparison = lowercase(left.name).compare(
                lowercase(right.name)
            );
        }
        return request.descending
                   ? comparison > 0
                   : comparison < 0;
    };
    std::stable_sort(entries.begin(), entries.end(), compare);

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
    std::ifstream input(resolve_follow(path), std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open file for reading");
    }
    input.seekg(static_cast<std::streamoff>(offset));
    std::array<char, 64U * 1024U> buffer{};
    auto remaining = length;
    while (remaining > 0U && input) {
        cancellation.throw_if_requested();
        const auto requested = static_cast<std::streamsize>(
            std::min<std::uint64_t>(remaining, buffer.size())
        );
        input.read(buffer.data(), requested);
        const auto count = input.gcount();
        if (count <= 0) {
            break;
        }
        output.write(buffer.data(), count);
        if (!output.good()) {
            throw std::runtime_error("failed while writing file data");
        }
        remaining -= static_cast<std::uint64_t>(count);
    }
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
    const auto destination = resolve(path);
    if (!overwrite && exists(destination.string())) {
        throw std::runtime_error(
            "destination exists: " + destination.string()
        );
    }
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
    auto pattern =
        (destination.parent_path() / ".mcpp-write-XXXXXX").string();
    std::vector<char> temporary_buffer(pattern.begin(), pattern.end());
    temporary_buffer.push_back('\0');
    int descriptor = mkstemp(temporary_buffer.data());
    if (descriptor < 0) {
        throw std::runtime_error(
            "cannot create temporary output: " +
            std::string(std::strerror(errno))
        );
    }
    const std::filesystem::path temporary(temporary_buffer.data());
    auto destination_permissions = std::filesystem::perms::owner_read |
                                   std::filesystem::perms::owner_write;
    if (exists(destination.string())) {
        destination_permissions =
            std::filesystem::symlink_status(destination).permissions();
    }
    if (fchmod(
            descriptor,
            static_cast<mode_t>(
                static_cast<unsigned int>(destination_permissions) &
                07777U
            )
        ) != 0) {
        const auto failure = errno;
        static_cast<void>(close(descriptor));
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw std::runtime_error(
            "cannot set temporary output permissions: " +
            std::string(std::strerror(failure))
        );
    }
    std::array<char, 64U * 1024U> buffer{};
    std::uint64_t written = 0;
    try {
        while (input) {
            cancellation.throw_if_requested();
            input.read(
                buffer.data(),
                static_cast<std::streamsize>(buffer.size())
            );
            const auto count = input.gcount();
            if (count <= 0) {
                break;
            }
            std::streamsize offset = 0;
            while (offset < count) {
                const auto result = ::write(
                    descriptor,
                    buffer.data() + offset,
                    static_cast<std::size_t>(count - offset)
                );
                if (result < 0 && errno == EINTR) {
                    continue;
                }
                if (result <= 0) {
                    throw std::runtime_error(
                        "failed while writing " +
                        destination.string()
                    );
                }
                offset += result;
            }
            written += static_cast<std::uint64_t>(count);
            if (progress) {
                progress({
                    .bytes_done = written,
                    .bytes_total = length.value_or(0),
                    .files_done = 0,
                    .files_total = 1,
                    .current_path = destination.string(),
                });
            }
        }
        if (fsync(descriptor) != 0) {
            throw std::runtime_error(
                "failed to commit " + temporary.string()
            );
        }
        if (close(descriptor) != 0) {
            descriptor = -1;
            throw std::runtime_error(
                "failed to close " + temporary.string()
            );
        }
        descriptor = -1;
        if (overwrite) {
            if (expected_revision) {
                const auto current = stat(
                    destination.string(),
                    cancellation
                );
                if (current.revision != *expected_revision) {
                    throw std::runtime_error(
                        "destination changed while saving: " +
                        destination.string()
                    );
                }
            }
            std::filesystem::rename(temporary, destination);
        } else if (syscall(
                       SYS_renameat2,
                       AT_FDCWD,
                       temporary.c_str(),
                       AT_FDCWD,
                       destination.c_str(),
                       RENAME_NOREPLACE
                   ) != 0) {
            throw std::runtime_error(
                errno == EEXIST
                    ? "destination exists: " + destination.string()
                    : "cannot commit destination: " +
                          std::string(std::strerror(errno))
            );
        }
    } catch (...) {
        if (descriptor >= 0) {
            static_cast<void>(close(descriptor));
        }
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
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
        count = std::filesystem::remove_all(target);
    } else {
        count = std::filesystem::remove(target) ? 1U : 0U;
    }
    if (count == 0U) {
        throw std::runtime_error(
            "nothing removed: " + target.string()
        );
    }
    if (progress) {
        progress({
            .files_done = count,
            .files_total = count,
            .current_path = target.string(),
        });
    }
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
    if (!overwrite && exists(destination_path.string())) {
        throw std::runtime_error(
            "destination exists: " + destination_path.string()
        );
    }
    if (overwrite) {
        std::optional<std::filesystem::path> backup;
        if (exists(destination_path.string())) {
            for (unsigned int attempt = 0; attempt < 100U; ++attempt) {
                const auto candidate =
                    destination_path.parent_path() /
                    (
                        ".mcpp-replaced-" +
                        std::to_string(getpid()) + "-" +
                        std::to_string(attempt)
                    );
                if (rename_no_replace(
                        destination_path,
                        candidate
                    )) {
                    backup = candidate;
                    break;
                }
            }
            if (!backup) {
                throw std::runtime_error(
                    "cannot allocate rename backup"
                );
            }
        }
        try {
            std::filesystem::rename(source_path, destination_path);
        } catch (...) {
            if (backup) {
                std::filesystem::rename(*backup, destination_path);
            }
            throw;
        }
        if (backup) {
            std::error_code ignored;
            std::filesystem::remove_all(*backup, ignored);
        }
        return;
    }
    std::filesystem::rename(source_path, destination_path);
}

void LocalProvider::copy_direct(
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
        const bool created = !exists(destination_path.string());
        create_directory(destination_path.string());
        try {
            for (const auto& child :
                 list(
                     source_path.string(),
                     {.show_hidden = true},
                     cancellation
                 )) {
                copy_direct(
                    child.path,
                    join(destination_path.string(), child.name),
                    overwrite,
                    cancellation,
                    progress
                );
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
        return;
    }
    if (source_entry.kind == EntryKind::symlink) {
        if (overwrite) {
            std::error_code ignored;
            std::filesystem::remove(destination_path, ignored);
        }
        create_symlink(source_entry.target, destination_path.string());
        if (progress) {
            progress({
                .files_done = 1,
                .files_total = 1,
                .current_path = destination_path.string(),
            });
        }
        return;
    }
    std::ifstream input(source_path, std::ios::binary);
    if (!input) {
        throw std::runtime_error(
            "cannot open copy source: " + source_path.string()
        );
    }
    write(
        destination_path.string(),
        input,
        source_entry.size,
        overwrite,
        std::nullopt,
        cancellation,
        progress
    );
    if (source_entry.permissions != std::filesystem::perms::unknown) {
        set_permissions(
            destination_path.string(),
            source_entry.permissions
        );
    }
}

void LocalProvider::set_permissions(
    const std::string_view path,
    const std::filesystem::perms permissions
) {
    std::filesystem::permissions(
        resolve(path),
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
    const std::string_view path
) {
    std::filesystem::create_symlink(
        std::filesystem::path(target),
        resolve(path)
    );
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
