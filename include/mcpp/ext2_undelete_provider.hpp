#pragma once

#include "mcpp/provider.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace mcpp {

struct Ext2UndeleteLimits {
    std::uint64_t image_bytes{1ULL << 40U};
    std::uint32_t inodes{4U * 1024U * 1024U};
    std::uint32_t groups{65536U};
    std::uint64_t inode_table_bytes{512ULL * 1024U * 1024U};
    std::size_t deleted_files{100000U};
    std::size_t mapped_blocks{1000000U};
    std::size_t metadata_bytes{128U * 1024U * 1024U};
};

struct Ext2UndeleteOptions {
    Ext2UndeleteLimits limits{};
};

struct Ext2UndeleteSource {
    ProviderPtr provider;
    std::string path;
    std::string revision;
    std::string location;
};

struct Ext2UndeleteImageInfo {
    std::uint32_t block_size{0};
    std::uint32_t inode_size{0};
    std::uint32_t blocks{0};
    std::uint32_t inodes{0};
    std::uint32_t groups{0};
    std::size_t deleted_files{0};
    std::size_t skipped_nonregular_inodes{0};
};

struct Ext2RecoveryInfo {
    std::uint64_t inode{0};
    std::chrono::system_clock::time_point deleted{};
    std::uint64_t logical_blocks{0};
    std::uint64_t free_data_blocks{0};
    bool recoverable{false};
    std::string diagnostic;
};

// A read-only, flat view of deleted regular inodes in a regular ext2 image.
// This is not a mounted filesystem and never opens a block device. Image bytes
// are read only through the captured provider; every read checks its revision.
// A changed/removed source invalidates this view rather than mixing revisions.
//
// Members are named "<inode>:<free-data-blocks>", not their lost filenames; the
// count includes only validated, currently free data pointers, not tree blocks.
// stat() retains the inode's stored mode, uid/gid, size and atime/mtime/ctime;
// recovery_info() separately exposes deletion time and recovery failures. The
// inode's ownership and mode are metadata, not host access credentials. The
// virtual root is synthetic. All mutations fail, even when a stored mode has
// write bits. location() uses the captured source URI plus "/undel://". This
// source/member form is a regular-image extension, not native device-URI
// compatibility. Native device aliases must not be guessed or opened here.
//
// Supports Linux ext2 revision 0/1, 1/2/4 KiB blocks, traditional direct through
// triple-indirect block maps and sparse-super/large-file filesystem features.
// Journals, extents, unknown features, dirty filesystems, external inode xattrs
// and nonregular inode recovery are not supported. A missing data/tree pointer
// is an error: a deleted inode cannot distinguish a sparse hole from a pointer
// cleared during deletion. Reallocated, overlapping or invalid blocks never
// produce zero-filled or partially successful recovery. Incomplete candidates
// remain listed, but read() fails before emitting any bytes.
//
// Ext2 has no data checksums or history of blocks overwritten and freed again.
// A free allocation bit cannot prove that deleted bytes are still original.
// Use a stable image made without writing to the filesystem being recovered.
// Source providers must supply meaningful, nonempty revisions for checked
// reads. No pathname or source identity is recovered from a provider ID.
class Ext2UndeleteProvider final : public Provider {
public:
    Ext2UndeleteProvider(
        ProviderPtr source, std::string path, std::string source_location,
        Ext2UndeleteOptions options = {},
        const Cancellation& cancellation = {}
    );
    ~Ext2UndeleteProvider() override;
    Ext2UndeleteProvider(const Ext2UndeleteProvider&) = delete;
    Ext2UndeleteProvider& operator=(const Ext2UndeleteProvider&) = delete;

    [[nodiscard]] Ext2UndeleteSource source() const;
    [[nodiscard]] std::string location(std::string_view path = "/") const;
    [[nodiscard]] Ext2UndeleteImageInfo image_info() const noexcept;
    [[nodiscard]] Ext2RecoveryInfo recovery_info(
        std::string_view path, const Cancellation& cancellation = {}
    ) const;

    [[nodiscard]] std::string id() const override;
    [[nodiscard]] std::string display_name() const override;
    [[nodiscard]] ProviderCapability capabilities() const override;
    [[nodiscard]] std::string normalize(std::string_view path) const override;
    [[nodiscard]] std::string join(std::string_view directory, std::string_view name) const override;
    [[nodiscard]] std::string parent(std::string_view path) const override;
    [[nodiscard]] std::string basename(std::string_view path) const override;
    [[nodiscard]] std::vector<Entry> list(
        std::string_view path, const ListRequest& request,
        const Cancellation& cancellation
    ) const override;
    [[nodiscard]] Entry stat(std::string_view path, const Cancellation& cancellation) const override;
    [[nodiscard]] Entry stat_follow(std::string_view path, const Cancellation& cancellation) const override;
    [[nodiscard]] bool exists(std::string_view path) const override;
    [[nodiscard]] SpaceInfo space(std::string_view path) const override;
    void read(
        std::string_view path, std::uint64_t offset, std::uint64_t length,
        std::ostream& output, const Cancellation& cancellation
    ) const override;
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
    void rename(std::string_view source, std::string_view destination, bool overwrite) override;
    bool copy_direct(
        std::string_view source, std::string_view destination, bool overwrite,
        const Cancellation& cancellation, const ProgressCallback& progress
    ) override;
    void set_permissions(std::string_view path, std::filesystem::perms permissions) override;
    void set_owner(
        std::string_view path, std::optional<std::string> owner,
        std::optional<std::string> group
    ) override;
    void create_symlink(
        std::string_view target, std::string_view path, bool overwrite = false,
        const Cancellation& cancellation = {}
    ) override;
    void create_hardlink(std::string_view target, std::string_view path) override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] std::shared_ptr<Ext2UndeleteProvider> open_ext2_undelete_provider(
    ProviderPtr source, std::string path, std::string source_location,
    Ext2UndeleteOptions options = {},
    const Cancellation& cancellation = {}
);

}  // namespace mcpp
