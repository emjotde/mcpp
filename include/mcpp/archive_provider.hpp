#pragma once

#include "mcpp/provider.hpp"

#include <cstddef>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>

namespace mcpp {

class LocalProvider;

enum class ArchiveFormat { tar, cpio_newc, cpio_crc, cpio_odc, cpio_binary_le, cpio_binary_be };
enum class ArchiveCompression { none, gzip, bzip2, xz, zstd };

struct ArchiveLimits {
    std::uint64_t archive_bytes{static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())};
    std::uint64_t expanded_bytes{static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())};
    std::size_t members{100000U};
    std::size_t path_bytes{4096U};
    std::size_t metadata_bytes{16U * 1024U * 1024U};
    std::size_t sparse_extents{1000000U};
};

struct ArchiveOpenOptions {
    bool writable{false};
    ArchiveLimits limits{};
    // Created with mode 0700 if absent; an existing directory must be private
    // and owned by the current user. No system temporary directory is used.
    std::filesystem::path draft_directory{".mcpp-archive-drafts"};
};

// An indexed, privately spooled archive snapshot, not a host extraction directory.
// Supports regular files, directories, safe relative symlinks and hardlinks in
// tar (ustar, GNU/PAX including GNU sparse 0.0/0.1/1.0) and newc/CRC, odc and
// V7 binary cpio. Special entries retain their actual mode and device numbers;
// reading them as ordinary files is an error. Index/metadata limits stay bounded.
// Byte limits are configurable; data is never accumulated in a whole-file string.
//
// Writable operations publish one complete archive before returning, using
// LocalProvider's expected-revision/atomic-replacement guard. Other programs
// are not required to honor the advisory lock; this is not a filesystem CAS.
// Failed publication retains a private draft, exposed by last_draft(). The
// original snapshot remains readable; reopen after an external change.
//
// Regular-file Entry.inode values identify virtual hardlink groups within this
// provider instance; Entry.links counts names present in the archive, not links
// outside it. Inode numbers need not stay the same after reopening the archive.
// Unchanged members retain their Entry.revision across unrelated publications.
class ArchiveProvider final : public Provider {
public:
    explicit ArchiveProvider(
        std::filesystem::path archive,
        ArchiveOpenOptions options = {},
        const Cancellation& cancellation = {}
    );
    // Captures a regular member from its actual provider. A non-local source
    // is always read-only; no virtual member path is interpreted as a host path.
    ArchiveProvider(
        ProviderPtr source, std::string path,
        ArchiveOpenOptions options = {},
        const Cancellation& cancellation = {}
    );
    ~ArchiveProvider() override;
    ArchiveProvider(const ArchiveProvider&) = delete;
    ArchiveProvider& operator=(const ArchiveProvider&) = delete;

    // Empty for a non-local source. Use the captured source accessors for
    // nested VFS navigation/formatting, never a parsed provider ID.
    [[nodiscard]] std::filesystem::path archive_path() const;
    [[nodiscard]] ProviderPtr source_provider() const;
    [[nodiscard]] std::string source_path() const;
    [[nodiscard]] ArchiveFormat format() const;
    [[nodiscard]] ArchiveCompression compression() const;
    [[nodiscard]] std::optional<std::filesystem::path> last_draft() const;
    // Follows virtual symlinks; counts regular or special virtual hardlinks.
    [[nodiscard]] std::uint64_t hardlink_count(std::string_view path) const;
    // Sparse-aware archive-to-local copy through LocalProvider's normal atomic
    // publication and special-file rules. Returns false when the generic stream
    // path is needed (a regular member without owner-read permission).
    bool copy_to_local(
        std::string_view source, LocalProvider& destination_provider,
        std::string_view destination, bool overwrite, bool preserve_permissions,
        const Cancellation& cancellation, const ProgressCallback& progress,
        std::optional<std::string> expected_source_revision = {},
        std::optional<std::string> expected_destination_revision = {}
    ) const;
    // Requires a regular member, writable provider, host archive, and a member write
    // bit (0222). Archived uid/gid are metadata, not host access credentials.
    // Both file-save methods enforce this member check before consuming input.
    void ensure_writable(std::string_view path) const;
    // Replaces data for every name in the resolved file's hardlink group, in
    // one guarded publication. Keeps the virtual inode, ownership and mode;
    // updates mtime/ctime. expected_revision refers to stat_follow(path), not
    // to a symlink entry. Missing files are errors; no new member is created.
    void write_preserving_links(
        std::string_view path, std::istream& input,
        std::optional<std::uint64_t> length,
        std::optional<std::string> expected_revision,
        const Cancellation& cancellation = {}
    );
    // Call before OperationEngine's recursive directory copy/move, just as
    // its LocalProvider guard is called. Resolves virtual directory symlinks.
    void validate_directory_target(
        std::string_view source,
        const ArchiveProvider& destination_provider,
        std::string_view destination
    ) const;

    [[nodiscard]] std::string id() const override;
    [[nodiscard]] std::string display_name() const override;
    [[nodiscard]] ProviderCapability capabilities() const override;
    // Paths are virtual, rooted at "/". Dot components and repeated slashes
    // are errors, rather than aliases silently collapsed by normalization.
    [[nodiscard]] std::string normalize(std::string_view path) const override;
    [[nodiscard]] std::string join(
        std::string_view directory, std::string_view name
    ) const override;
    [[nodiscard]] std::string parent(std::string_view path) const override;
    [[nodiscard]] std::string basename(std::string_view path) const override;

    [[nodiscard]] std::vector<Entry> list(
        std::string_view path, const ListRequest& request,
        const Cancellation& cancellation
    ) const override;
    [[nodiscard]] Entry stat(
        std::string_view path, const Cancellation& cancellation
    ) const override;
    [[nodiscard]] Entry stat_follow(
        std::string_view path, const Cancellation& cancellation
    ) const override;
    [[nodiscard]] bool exists(std::string_view path) const override;
    [[nodiscard]] SpaceInfo space(std::string_view path) const override;
    void read(
        std::string_view path, std::uint64_t offset, std::uint64_t length,
        std::ostream& output, const Cancellation& cancellation
    ) const override;
    // Unlike write_preserving_links, ordinary write replaces only this member
    // and detaches it from hardlinks. A leaf symlink is replaced, not followed.
    void write(
        std::string_view path, std::istream& input,
        std::optional<std::uint64_t> length, bool overwrite,
        std::optional<std::string> expected_revision,
        const Cancellation& cancellation, const ProgressCallback& progress
    ) override;
    void create_directory(std::string_view path) override;
    void remove(
        std::string_view path, bool recursive,
        const Cancellation& cancellation, const ProgressCallback& progress
    ) override;
    void rename(
        std::string_view source, std::string_view destination, bool overwrite
    ) override;
    bool copy_direct(
        std::string_view source, std::string_view destination, bool overwrite,
        const Cancellation& cancellation, const ProgressCallback& progress
    ) override;
    void set_permissions(
        std::string_view path, std::filesystem::perms permissions
    ) override;
    void set_owner(
        std::string_view path, std::optional<std::string> owner,
        std::optional<std::string> group
    ) override;
    void create_symlink(
        std::string_view target, std::string_view path, bool overwrite = false,
        const Cancellation& cancellation = {}
    ) override;
    void create_hardlink(
        std::string_view target, std::string_view path
    ) override;
    // Creates/replaces one linked name in a single guarded publication.
    void copy_hardlink(
        std::string_view source, std::string_view destination, bool overwrite,
        const Cancellation& cancellation
    );
    void set_modified(
        std::string_view path, std::chrono::system_clock::time_point modified
    );

private:
    void write_impl(
        std::string_view path, std::istream& input,
        std::optional<std::uint64_t> length, bool overwrite,
        std::optional<std::string> expected_revision,
        const Cancellation& cancellation, const ProgressCallback& progress,
        bool preserve_links
    );
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// Filename-only UI hint. Opening always checks the actual archive bytes.
[[nodiscard]] bool is_archive_filename(std::string_view name) noexcept;
[[nodiscard]] std::shared_ptr<ArchiveProvider> open_archive_provider(
    const std::filesystem::path& archive, ArchiveOpenOptions options = {},
    const Cancellation& cancellation = {}
);
[[nodiscard]] std::shared_ptr<ArchiveProvider> open_archive_provider(
    ProviderPtr source, std::string path, ArchiveOpenOptions options = {},
    const Cancellation& cancellation = {}
);

}  // namespace mcpp
