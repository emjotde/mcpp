#include "mcpp/ext2_undelete_provider.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <iterator>
#include <limits>
#include <map>
#include <ostream>
#include <span>
#include <stdexcept>
#include <streambuf>
#include <sys/stat.h>
#include <utility>

namespace mcpp {
namespace {
namespace fs = std::filesystem;
using Bytes = std::vector<unsigned char>;

// Layout and allocation rules: https://www.nongnu.org/ext2-doc/ext2.html
// Only the documented ext2 structures are decoded, never native C structs.
constexpr std::uint32_t compat_features = 0x00000038U;
constexpr std::uint32_t incompat_features = 0x00000002U;
constexpr std::uint32_t readonly_features = 0x00000003U;
constexpr std::uint32_t ordinary_inode_flags = 0x000000FBU;
constexpr std::size_t read_chunk = 65536U;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error("ext2 undelete: " + message);
}

[[noreturn]] void readonly() {
    fail("the undelete view is read-only; copy recovered bytes to another filesystem");
}

[[noreturn]] void path_error(const std::string_view path, const std::errc error) {
    throw fs::filesystem_error(
        "ext2 undelete member", fs::path(path), std::make_error_code(error));
}

std::uint16_t u16(const std::span<const unsigned char> bytes, const std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 2U) fail("truncated metadata field");
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(bytes[offset]) |
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset + 1U]) << 8U));
}

std::uint32_t u32(const std::span<const unsigned char> bytes, const std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 4U) fail("truncated metadata field");
    std::uint32_t result = 0;
    for (unsigned int index = 0; index < 4U; ++index) {
        result |= static_cast<std::uint32_t>(bytes[offset + index]) << (index * 8U);
    }
    return result;
}

std::uint64_t divided_up(const std::uint64_t value, const std::uint64_t divisor) {
    return value / divisor + (value % divisor != 0U ? 1U : 0U);
}

bool bit(const Bytes& bytes, const std::uint32_t index) {
    if (index / 8U >= bytes.size()) fail("allocation bitmap index is out of bounds");
    return (bytes[index / 8U] & (1U << (index % 8U))) != 0U;
}

std::chrono::system_clock::time_point timestamp(const std::uint32_t seconds) {
    return std::chrono::system_clock::time_point{std::chrono::seconds(seconds)};
}

bool power_of(const std::uint32_t value, const std::uint32_t base) {
    if (value == 0U) return false;
    auto remaining = value;
    while (remaining % base == 0U) remaining /= base;
    return remaining == 1U;
}

std::string virtual_path(std::string_view path) {
    if (path.empty() || path == "/") return "/";
    if (path.starts_with('/')) path.remove_prefix(1U);
    if (path.empty() || path.size() > 32U) fail("invalid undelete member name");
    const auto colon = path.find(':');
    const auto decimal = [](const std::string_view value) {
        return !value.empty() && (value.size() == 1U || value.front() != '0') &&
            std::all_of(value.begin(), value.end(),
                [](const unsigned char byte) { return byte >= '0' && byte <= '9'; });
    };
    if (colon == std::string_view::npos || !decimal(path.substr(0, colon)) ||
        !decimal(path.substr(colon + 1U))) {
        fail("members must be <inode>:<free-data-blocks> in the undelete namespace");
    }
    return "/" + std::string(path);
}

class ExactBuffer final : public std::streambuf {
public:
    explicit ExactBuffer(Bytes& bytes) : bytes_(bytes) {}
    [[nodiscard]] std::size_t written() const noexcept { return written_; }

protected:
    std::streamsize xsputn(const char* data, const std::streamsize count) override {
        if (count <= 0) return 0;
        const auto copied = std::min<std::uint64_t>(
            static_cast<std::uint64_t>(count), bytes_.size() - written_);
        std::memcpy(bytes_.data() + written_, data, static_cast<std::size_t>(copied));
        written_ += static_cast<std::size_t>(copied);
        return static_cast<std::streamsize>(copied);
    }

    int_type overflow(const int_type value) override {
        if (traits_type::eq_int_type(value, traits_type::eof())) return traits_type::not_eof(value);
        if (written_ == bytes_.size()) return traits_type::eof();
        bytes_[written_++] = static_cast<unsigned char>(traits_type::to_char_type(value));
        return value;
    }

private:
    Bytes& bytes_;
    std::size_t written_{0};
};

struct Group {
    std::uint32_t begin{0};
    std::uint32_t end{0};
    std::uint32_t inode_table{0};
    std::uint32_t inode_count{0};
    Bytes block_bitmap;
    Bytes inode_bitmap;
};

struct MetadataRange {
    std::uint32_t begin{0};
    std::uint32_t end{0};
};

struct Node {
    Entry entry;
    Ext2RecoveryInfo recovery;
    std::vector<std::uint32_t> data;
    std::uint64_t referenced_blocks{0};
};

void incomplete(Node& node, const std::string& diagnostic) {
    if (node.recovery.diagnostic.empty()) node.recovery.diagnostic = diagnostic;
    node.recovery.recoverable = false;
}

std::atomic_uint64_t next_instance{0};

}  // namespace

class Ext2UndeleteProvider::Impl {
public:
    Impl(
        ProviderPtr provider, std::string path, std::string source_location,
        const Ext2UndeleteOptions& options, const Cancellation& cancellation
    ) : identity_("ext2-undelete:" + std::to_string(++next_instance)),
        limits_(options.limits) {
        cancellation.throw_if_requested();
        if (!provider) throw std::invalid_argument("ext2 undelete source provider is null");
        if (source_location.empty() || source_location.find('\0') != std::string::npos) {
            throw std::invalid_argument("ext2 undelete requires the actual source location");
        }
        if (limits_.image_bytes < 2048U || limits_.inodes == 0U || limits_.groups == 0U ||
            limits_.inode_table_bytes == 0U || limits_.deleted_files == 0U ||
            limits_.mapped_blocks == 0U || limits_.metadata_bytes < read_chunk + 1024U) {
            throw std::invalid_argument("invalid ext2 undelete limits");
        }
        source_.provider = std::move(provider);
        source_.path = source_.provider->normalize(path);
        source_.location = std::move(source_location);
        if (!has_capability(source_.provider->capabilities(), ProviderCapability::read)) {
            fail("source provider does not support image reads");
        }
        source_entry_ = source_.provider->stat_follow(source_.path, cancellation);
        if (source_entry_.kind != EntryKind::file ||
            (source_entry_.mode != 0U && (source_entry_.mode & S_IFMT) != S_IFREG)) {
            fail("a regular ext2 image is required; block devices and other special files "
                 "need a stable regular image snapshot");
        }
        if (source_entry_.revision.empty()) fail("source provider does not supply a checked image revision");
        if (source_entry_.size > limits_.image_bytes ||
            source_entry_.size > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            fail("source image exceeds its byte limit");
        }
        source_.revision = source_entry_.revision;
        charge(read_chunk + 1024U);
        const auto superblock = read_exact(1024U, 1024U, cancellation);
        geometry(superblock);
        load_groups(superblock, cancellation);
        scan_inodes(cancellation);
        check_source(cancellation);
        root_.name = "/";
        root_.path = "/";
        root_.kind = EntryKind::directory;
        root_.permissions = static_cast<fs::perms>(0555U);
        root_.mode = S_IFDIR | 0555U;
        root_.links = 2U;
        root_.revision = identity_ + ":root";
    }

    void check_source(const Cancellation& cancellation) const {
        cancellation.throw_if_requested();
        const auto current = source_.provider->stat_follow(source_.path, cancellation);
        if (current.revision != source_.revision || current.kind != source_entry_.kind ||
            current.mode != source_entry_.mode || current.size != source_entry_.size ||
            current.inode != source_entry_.inode) {
            fail("source image changed; reopen the undelete view");
        }
        cancellation.throw_if_requested();
    }

    [[nodiscard]] const Node& node(const std::string& path) const {
        const auto found = nodes_.find(path);
        if (found == nodes_.end()) path_error(path, std::errc::no_such_file_or_directory);
        return found->second;
    }

    [[nodiscard]] Bytes read_exact(
        const std::uint64_t offset, const std::size_t count, const Cancellation& cancellation
    ) const {
        cancellation.throw_if_requested();
        const auto limit = filesystem_bytes_ == 0U ? source_entry_.size : filesystem_bytes_;
        if (count > read_chunk || offset > limit || count > limit - offset) {
            fail("image read is out of bounds or truncated");
        }
        check_source(cancellation);
        Bytes bytes(count);
        ExactBuffer buffer(bytes);
        std::ostream output(&buffer);
        source_.provider->read(source_.path, offset, count, output, cancellation);
        if (!output.good() || buffer.written() != count) {
            fail("source provider returned a short or oversized image read");
        }
        check_source(cancellation);
        return bytes;
    }

    Ext2UndeleteSource source_;
    Ext2UndeleteImageInfo info_;
    std::string identity_;
    Entry root_;
    std::map<std::string, Node> nodes_;
    std::uint64_t filesystem_bytes_{0};
    std::uint64_t free_bytes_{0};

private:
    void charge(const std::uint64_t bytes) {
        if (bytes > limits_.metadata_bytes - metadata_used_) fail("metadata exceeds its byte limit");
        metadata_used_ += static_cast<std::size_t>(bytes);
    }

    void geometry(const Bytes& bytes) {
        if (u16(bytes, 56U) != 0xEF53U) fail("source is not an ext2 filesystem image");
        const auto revision = u32(bytes, 76U);
        if (revision > 1U) fail("unsupported ext2 revision");
        if (u32(bytes, 72U) != 0U) fail("only the Linux ext2 inode layout is supported");
        const auto compatible = u32(bytes, 92U);
        const auto incompatible = u32(bytes, 96U);
        const auto readonly_compatible = u32(bytes, 100U);
        if ((compatible & 0x00000004U) != 0U || (incompatible & 0x0000000CU) != 0U) {
            fail("journaling/ext3/ext4 undelete is not supported");
        }
        if ((compatible & ~compat_features) != 0U ||
            (incompatible & ~incompat_features) != 0U ||
            (readonly_compatible & ~readonly_features) != 0U ||
            (revision == 0U && (compatible | incompatible | readonly_compatible) != 0U)) {
            fail("unsupported ext2 filesystem feature bits");
        }
        if (u16(bytes, 58U) != 1U || u32(bytes, 232U) != 0U) {
            fail("filesystem is not cleanly unmounted or has pending orphan inodes");
        }
        const auto logarithm = u32(bytes, 24U);
        if (logarithm > 2U) fail("only 1, 2 and 4 KiB ext2 blocks are supported");
        if (u32(bytes, 28U) != logarithm || u32(bytes, 36U) != u32(bytes, 32U)) {
            fail("ext2 fragments are not supported");
        }
        info_.block_size = 1024U << logarithm;
        info_.blocks = u32(bytes, 4U);
        info_.inodes = u32(bytes, 0U);
        info_.inode_size = revision == 0U ? 128U : u16(bytes, 88U);
        first_inode_ = revision == 0U ? 11U : u32(bytes, 84U);
        first_data_ = u32(bytes, 20U);
        blocks_per_group_ = u32(bytes, 32U);
        inodes_per_group_ = u32(bytes, 40U);
        if (first_data_ != (info_.block_size == 1024U ? 1U : 0U) ||
            info_.blocks <= first_data_ || blocks_per_group_ == 0U ||
            blocks_per_group_ > info_.block_size * 8U || inodes_per_group_ == 0U ||
            inodes_per_group_ > info_.block_size * 8U ||
            info_.inode_size < 128U || info_.inode_size > info_.block_size ||
            (info_.inode_size & (info_.inode_size - 1U)) != 0U ||
            inodes_per_group_ % (info_.block_size / info_.inode_size) != 0U ||
            first_inode_ < 11U || first_inode_ > info_.inodes) {
            fail("invalid ext2 block/inode geometry");
        }
        const auto group_count = divided_up(info_.blocks - first_data_, blocks_per_group_);
        if (group_count > limits_.groups || info_.inodes > limits_.inodes) {
            fail("ext2 group or inode count exceeds its limit");
        }
        if (group_count == 0U ||
            info_.inodes <= (group_count - 1U) * inodes_per_group_ ||
            info_.inodes > group_count * inodes_per_group_) {
            fail("inode count does not match the ext2 block groups");
        }
        if (static_cast<std::uint64_t>(info_.inodes) * info_.inode_size > limits_.inode_table_bytes) {
            fail("ext2 inode table scan exceeds its byte limit");
        }
        info_.groups = static_cast<std::uint32_t>(group_count);
        filesystem_bytes_ = static_cast<std::uint64_t>(info_.blocks) * info_.block_size;
        if (filesystem_bytes_ > source_entry_.size) fail("ext2 filesystem extends beyond the source image");
        if (u32(bytes, 8U) > info_.blocks || u32(bytes, 12U) > info_.blocks ||
            u32(bytes, 16U) > info_.inodes) {
            fail("invalid ext2 free/reserved counts");
        }
        free_bytes_ = static_cast<std::uint64_t>(u32(bytes, 12U)) * info_.block_size;
        sparse_super_ = (readonly_compatible & 1U) != 0U;
        large_files_ = (readonly_compatible & 2U) != 0U;
        descriptor_blocks_ = static_cast<std::uint32_t>(
            divided_up(group_count * 32U, info_.block_size));
        reserved_descriptors_ = u16(bytes, 206U);
        if ((compatible & 0x10U) == 0U && reserved_descriptors_ != 0U) {
            fail("reserved descriptor blocks without the resize-inode feature");
        }
    }

    void metadata_range(
        const Group& group, const std::uint32_t begin, const std::uint64_t count
    ) {
        if (count == 0U || begin < group.begin || begin >= group.end || count > group.end - begin) {
            fail("ext2 metadata lies outside its block group");
        }
        charge(sizeof(MetadataRange) * 2U);
        metadata_.push_back({begin, static_cast<std::uint32_t>(begin + count)});
    }

    std::uint32_t free_bits(
        const Bytes& bitmap, const std::uint32_t count, const Cancellation& cancellation
    ) const {
        std::uint32_t free = 0;
        for (std::uint32_t index = 0; index < count; ++index) {
            if (index % 256U == 0U) cancellation.throw_if_requested();
            if (!bit(bitmap, index)) ++free;
        }
        return free;
    }

    void load_groups(const Bytes& superblock, const Cancellation& cancellation) {
        const auto descriptor_bytes = static_cast<std::uint64_t>(info_.groups) * 32U;
        charge(descriptor_bytes + static_cast<std::uint64_t>(info_.groups) * sizeof(Group));
        groups_.reserve(info_.groups);
        Bytes descriptors;
        descriptors.reserve(static_cast<std::size_t>(descriptor_bytes));
        const auto descriptor_offset = static_cast<std::uint64_t>(first_data_ + 1U) * info_.block_size;
        for (std::uint64_t offset = 0; offset < descriptor_bytes;) {
            const auto count = static_cast<std::size_t>(
                std::min<std::uint64_t>(read_chunk, descriptor_bytes - offset));
            const auto bytes = read_exact(descriptor_offset + offset, count, cancellation);
            descriptors.insert(descriptors.end(), bytes.begin(), bytes.end());
            offset += count;
        }
        std::uint64_t free_blocks = 0;
        std::uint64_t free_inodes = 0;
        const auto table_blocks = divided_up(
            static_cast<std::uint64_t>(inodes_per_group_) * info_.inode_size, info_.block_size);
        for (std::uint32_t index = 0; index < info_.groups; ++index) {
            cancellation.throw_if_requested();
            const auto record = std::span<const unsigned char>(descriptors).subspan(
                static_cast<std::size_t>(index) * 32U, 32U);
            const auto begin = static_cast<std::uint64_t>(first_data_) +
                static_cast<std::uint64_t>(index) * blocks_per_group_;
            Group group;
            group.begin = static_cast<std::uint32_t>(begin);
            group.end = static_cast<std::uint32_t>(std::min<std::uint64_t>(
                info_.blocks, begin + blocks_per_group_));
            group.inode_table = u32(record, 8U);
            group.inode_count = static_cast<std::uint32_t>(std::min<std::uint64_t>(
                inodes_per_group_, info_.inodes - static_cast<std::uint64_t>(index) * inodes_per_group_));
            // mke2fs also records INODE_ZEROED on ext2; it changes no layout.
            // https://www.kernel.org/doc/html/latest/filesystems/ext4/group_descr.html
            if ((u16(record, 18U) & ~0x0004U) != 0U) fail("unsupported ext2 block-group flags");
            if (!sparse_super_ || index <= 1U || power_of(index, 3U) ||
                power_of(index, 5U) || power_of(index, 7U)) {
                metadata_range(group, group.begin, 1ULL + descriptor_blocks_ + reserved_descriptors_);
            }
            metadata_range(group, u32(record, 0U), 1U);
            metadata_range(group, u32(record, 4U), 1U);
            metadata_range(group, group.inode_table, table_blocks);
            charge(static_cast<std::uint64_t>(info_.block_size) * 2U);
            group.block_bitmap = read_exact(
                static_cast<std::uint64_t>(u32(record, 0U)) * info_.block_size, info_.block_size, cancellation);
            group.inode_bitmap = read_exact(
                static_cast<std::uint64_t>(u32(record, 4U)) * info_.block_size, info_.block_size, cancellation);
            const auto blocks = free_bits(group.block_bitmap, group.end - group.begin, cancellation);
            const auto inodes = free_bits(group.inode_bitmap, group.inode_count, cancellation);
            if (blocks != u16(record, 12U) || inodes != u16(record, 14U)) {
                fail("ext2 allocation bitmaps disagree with their free counts");
            }
            free_blocks += blocks;
            free_inodes += inodes;
            groups_.push_back(std::move(group));
        }
        if (free_blocks != u32(superblock, 12U) || free_inodes != u32(superblock, 16U)) {
            fail("ext2 group free counts disagree with the superblock");
        }
        std::sort(metadata_.begin(), metadata_.end(),
            [](const auto& left, const auto& right) { return left.begin < right.begin; });
        std::uint32_t previous_end = 0;
        for (const auto& range : metadata_) {
            if (range.begin < previous_end) fail("overlapping ext2 metadata blocks");
            previous_end = range.end;
            for (auto block = range.begin; block < range.end; ++block) {
                if (block % 256U == 0U) cancellation.throw_if_requested();
                if (!allocated(block)) fail("ext2 metadata block is marked free");
            }
        }
        for (std::uint32_t inode = 1U; inode < first_inode_; ++inode) {
            if (inode % 256U == 0U) cancellation.throw_if_requested();
            const auto& group = groups_.at((inode - 1U) / inodes_per_group_);
            if (!bit(group.inode_bitmap, (inode - 1U) % inodes_per_group_)) {
                fail("reserved ext2 inode is marked free");
            }
        }
    }

    bool allocated(const std::uint32_t block) const {
        if (block < first_data_ || block >= info_.blocks) fail("allocation lookup outside ext2 image");
        const auto index = (block - first_data_) / blocks_per_group_;
        const auto& group = groups_.at(index);
        return bit(group.block_bitmap, block - group.begin);
    }

    bool is_metadata(const std::uint32_t block) const {
        const auto found = std::upper_bound(metadata_.begin(), metadata_.end(), block,
            [](const auto value, const auto& range) { return value < range.begin; });
        return found != metadata_.begin() && block < std::prev(found)->end;
    }

    bool retained_block(
        Node& node, const std::uint32_t block, const std::size_t node_index, const bool data
    ) {
        if (block == 0U) {
            incomplete(node, "missing block pointer; sparse holes and cleared deleted pointers cannot be distinguished");
            return false;
        }
        if (block < first_data_ || block >= info_.blocks) {
            incomplete(node, "block pointer is outside the ext2 filesystem");
            return false;
        }
        if (is_metadata(block)) {
            incomplete(node, "block pointer refers to filesystem metadata");
            return false;
        }
        if (allocated(block)) {
            incomplete(node, "block has been reallocated and cannot be recovered");
            return false;
        }
        charge(sizeof(std::pair<std::uint32_t, std::size_t>) * 2U);
        references_.emplace_back(block, node_index);
        ++node.referenced_blocks;
        if (data) ++node.recovery.free_data_blocks;
        return true;
    }

    void missing(Node& node, const std::uint64_t count) const {
        node.data.insert(node.data.end(), static_cast<std::size_t>(count), 0U);
    }

    void map_tree(
        Node& node, const std::size_t node_index, const std::uint32_t block,
        const unsigned int depth, const std::uint64_t count,
        const Cancellation& cancellation
    ) {
        cancellation.throw_if_requested();
        if (count == 0U) {
            if (block != 0U) incomplete(node, "retained block pointers extend beyond the stored file size");
            return;
        }
        if (!retained_block(node, block, node_index, depth == 0U)) {
            missing(node, count);
            return;
        }
        if (depth == 0U) {
            node.data.push_back(block);
            return;
        }
        if (mapped_blocks_ == limits_.mapped_blocks) fail("block map exceeds its block limit");
        ++mapped_blocks_;
        const auto bytes = read_exact(static_cast<std::uint64_t>(block) * info_.block_size,
                                      info_.block_size, cancellation);
        const auto pointers = info_.block_size / 4U;
        std::uint64_t child_capacity = 1U;
        for (unsigned int level = 1U; level < depth; ++level) child_capacity *= pointers;
        auto remaining = count;
        for (std::uint32_t index = 0; index < pointers; ++index) {
            const auto child_count = std::min(remaining, child_capacity);
            map_tree(node, node_index, u32(bytes, static_cast<std::size_t>(index) * 4U),
                     depth - 1U, child_count, cancellation);
            remaining -= child_count;
        }
        if (remaining != 0U) fail("ext2 indirect block capacity is inconsistent");
    }

    void inode_map(
        Node& node, const std::size_t node_index, const std::span<const unsigned char> bytes,
        const Cancellation& cancellation
    ) {
        const auto count = node.recovery.logical_blocks;
        if (count > limits_.mapped_blocks) {
            incomplete(node, "file block map exceeds the configured recovery limit");
            return;
        }
        if ((u32(bytes, 32U) & ~ordinary_inode_flags) != 0U) {
            incomplete(node, "unsupported inode flags (compression, extents or another non-ext2 layout)");
            return;
        }
        if (u32(bytes, 104U) != 0U) {
            incomplete(node, "recovery of an inode with external extended attributes is not supported");
            return;
        }
        if (u32(bytes, 112U) != 0U || bytes[116U] != 0U || bytes[117U] != 0U) {
            incomplete(node, "inode fragments are not supported");
            return;
        }
        if (count > limits_.mapped_blocks - mapped_blocks_) fail("block map exceeds its block limit");
        mapped_blocks_ += static_cast<std::size_t>(count);
        charge(count * sizeof(std::uint32_t));
        node.data.reserve(static_cast<std::size_t>(count));
        auto remaining = count;
        for (std::uint32_t index = 0; index < 12U; ++index) {
            const auto used = std::min<std::uint64_t>(remaining, 1U);
            map_tree(node, node_index, u32(bytes, 40U + index * 4U), 0U, used, cancellation);
            remaining -= used;
        }
        std::uint64_t capacity = 1U;
        for (unsigned int depth = 1U; depth <= 3U; ++depth) {
            capacity *= info_.block_size / 4U;
            const auto used = std::min(remaining, capacity);
            map_tree(node, node_index, u32(bytes, 84U + depth * 4U), depth, used, cancellation);
            remaining -= used;
        }
        if (remaining != 0U || node.data.size() != count) {
            incomplete(node, "file size exceeds the traditional ext2 block map");
        }
        if (node.referenced_blocks * (info_.block_size / 512U) != u32(bytes, 28U)) {
            incomplete(node, "i_blocks does not match the retained data and indirect blocks");
        }
        node.recovery.recoverable = node.recovery.diagnostic.empty();
    }

    Node deleted_inode(
        const std::uint32_t number, const std::span<const unsigned char> bytes,
        const std::size_t node_index, const Cancellation& cancellation
    ) {
        Node node;
        node.entry.kind = EntryKind::file;
        node.entry.inode = number;
        node.entry.mode = u16(bytes, 0U);
        node.entry.permissions = static_cast<fs::perms>(node.entry.mode & 07777U);
        node.entry.uid = u16(bytes, 2U) | (static_cast<std::uint32_t>(u16(bytes, 120U)) << 16U);
        node.entry.gid = u16(bytes, 24U) | (static_cast<std::uint32_t>(u16(bytes, 122U)) << 16U);
        node.entry.owner = std::to_string(node.entry.uid);
        node.entry.group = std::to_string(node.entry.gid);
        node.entry.links = u16(bytes, 26U);
        node.entry.size = u32(bytes, 4U) |
            (static_cast<std::uint64_t>(u32(bytes, 108U)) << 32U);
        node.entry.accessed = timestamp(u32(bytes, 8U));
        node.entry.changed = timestamp(u32(bytes, 12U));
        node.entry.modified = timestamp(u32(bytes, 16U));
        node.entry.revision = identity_ + ":" + std::to_string(number) + ":" +
            std::to_string(u32(bytes, 100U)) + ":" + std::to_string(u32(bytes, 20U));
        node.recovery.inode = number;
        node.recovery.deleted = timestamp(u32(bytes, 20U));
        node.recovery.logical_blocks = divided_up(node.entry.size, info_.block_size);
        if (node.entry.links != 0U) incomplete(node, "a free deleted inode still has a nonzero link count");
        if (!large_files_ && u32(bytes, 108U) != 0U) {
            incomplete(node, "large inode size without the ext2 large-file feature");
        }
        inode_map(node, node_index, bytes, cancellation);
        return node;
    }

    void scan_inodes(const Cancellation& cancellation) {
        std::vector<Node> nodes;
        for (std::uint32_t group_index = 0; group_index < info_.groups; ++group_index) {
            const auto& group = groups_[group_index];
            for (std::uint32_t first = 0; first < group.inode_count;) {
                cancellation.throw_if_requested();
                const auto count = std::min<std::uint32_t>(
                    group.inode_count - first, static_cast<std::uint32_t>(read_chunk / info_.inode_size));
                const auto offset = static_cast<std::uint64_t>(group.inode_table) * info_.block_size +
                    static_cast<std::uint64_t>(first) * info_.inode_size;
                const auto bytes = read_exact(offset, static_cast<std::size_t>(count) * info_.inode_size,
                                              cancellation);
                for (std::uint32_t index = 0; index < count; ++index) {
                    cancellation.throw_if_requested();
                    const auto slot = first + index;
                    const auto number = group_index * inodes_per_group_ + slot + 1U;
                    if (number < first_inode_ || bit(group.inode_bitmap, slot)) continue;
                    const auto record = std::span<const unsigned char>(bytes).subspan(
                        static_cast<std::size_t>(index) * info_.inode_size, info_.inode_size);
                    if (u32(record, 20U) == 0U) continue;
                    if ((u16(record, 0U) & S_IFMT) != S_IFREG) {
                        ++info_.skipped_nonregular_inodes;
                        continue;
                    }
                    if (nodes.size() == limits_.deleted_files) fail("deleted inode listing exceeds its entry limit");
                    charge(sizeof(Node) * 4U + 1024U);
                    nodes.push_back(deleted_inode(number, record, nodes.size(), cancellation));
                }
                first += count;
            }
        }
        cancellation.throw_if_requested();
        std::sort(references_.begin(), references_.end());
        for (std::size_t first = 0; first < references_.size();) {
            cancellation.throw_if_requested();
            auto end = first + 1U;
            while (end < references_.size() && references_[end].first == references_[first].first) ++end;
            if (end - first > 1U) {
                for (auto index = first; index < end; ++index) {
                    incomplete(nodes.at(references_[index].second),
                        "block is referenced more than once by deleted data/indirect mappings");
                }
            }
            first = end;
        }
        info_.deleted_files = nodes.size();
        for (auto& node : nodes) {
            cancellation.throw_if_requested();
            node.entry.name = std::to_string(node.entry.inode) + ":" +
                std::to_string(node.recovery.free_data_blocks);
            node.entry.path = "/" + node.entry.name;
            auto key = node.entry.path;
            nodes_.emplace(std::move(key), std::move(node));
        }
    }

    Ext2UndeleteLimits limits_;
    Entry source_entry_;
    std::size_t metadata_used_{0};
    std::size_t mapped_blocks_{0};
    std::uint32_t first_inode_{0};
    std::uint32_t first_data_{0};
    std::uint32_t blocks_per_group_{0};
    std::uint32_t inodes_per_group_{0};
    std::uint32_t descriptor_blocks_{0};
    std::uint32_t reserved_descriptors_{0};
    bool sparse_super_{false};
    bool large_files_{false};
    std::vector<Group> groups_;
    std::vector<MetadataRange> metadata_;
    std::vector<std::pair<std::uint32_t, std::size_t>> references_;
};

Ext2UndeleteProvider::Ext2UndeleteProvider(
    ProviderPtr source, std::string path, std::string source_location,
    const Ext2UndeleteOptions options, const Cancellation& cancellation
) : impl_(std::make_unique<Impl>(std::move(source), std::move(path),
                               std::move(source_location), options, cancellation)) {}

Ext2UndeleteProvider::~Ext2UndeleteProvider() = default;

Ext2UndeleteSource Ext2UndeleteProvider::source() const { return impl_->source_; }

std::string Ext2UndeleteProvider::location(const std::string_view path) const {
    const auto member = normalize(path);
    return impl_->source_.location + "/undel://" + (member == "/" ? "" : member.substr(1U));
}

Ext2UndeleteImageInfo Ext2UndeleteProvider::image_info() const noexcept { return impl_->info_; }

Ext2RecoveryInfo Ext2UndeleteProvider::recovery_info(
    const std::string_view path, const Cancellation& cancellation
) const {
    const auto key = normalize(path);
    impl_->check_source(cancellation);
    if (key == "/") path_error(key, std::errc::is_a_directory);
    return impl_->node(key).recovery;
}

std::string Ext2UndeleteProvider::id() const { return impl_->identity_; }
std::string Ext2UndeleteProvider::display_name() const { return "ext2 undelete: " + impl_->source_.location; }
ProviderCapability Ext2UndeleteProvider::capabilities() const { return ProviderCapability::read; }
std::string Ext2UndeleteProvider::normalize(const std::string_view path) const { return virtual_path(path); }

std::string Ext2UndeleteProvider::join(
    const std::string_view directory, const std::string_view name
) const {
    if (normalize(directory) != "/") path_error(directory, std::errc::not_a_directory);
    if (name.empty() || name.starts_with('/')) fail("join requires a relative undelete member name");
    return normalize(name);
}

std::string Ext2UndeleteProvider::parent(const std::string_view path) const {
    static_cast<void>(normalize(path));
    return "/";
}

std::string Ext2UndeleteProvider::basename(const std::string_view path) const {
    const auto key = normalize(path);
    return key == "/" ? key : key.substr(1U);
}

std::vector<Entry> Ext2UndeleteProvider::list(
    const std::string_view path, const ListRequest& request, const Cancellation& cancellation
) const {
    const auto key = normalize(path);
    impl_->check_source(cancellation);
    if (key != "/") {
        static_cast<void>(impl_->node(key));
        path_error(key, std::errc::not_a_directory);
    }
    std::vector<Entry> entries;
    entries.reserve(impl_->nodes_.size());
    for (const auto& [name, node] : impl_->nodes_) {
        static_cast<void>(name);
        cancellation.throw_if_requested();
        entries.push_back(node.entry);
    }
    sort_entries(entries, request);
    impl_->check_source(cancellation);
    if (request.offset >= entries.size()) return {};
    const auto end = request.limit == 0U ? entries.size() :
        request.offset + std::min(request.limit, entries.size() - request.offset);
    return {entries.begin() + static_cast<std::ptrdiff_t>(request.offset),
            entries.begin() + static_cast<std::ptrdiff_t>(end)};
}

Entry Ext2UndeleteProvider::stat(const std::string_view path, const Cancellation& cancellation) const {
    const auto key = normalize(path);
    impl_->check_source(cancellation);
    return key == "/" ? impl_->root_ : impl_->node(key).entry;
}

Entry Ext2UndeleteProvider::stat_follow(const std::string_view path, const Cancellation& cancellation) const {
    return stat(path, cancellation);
}

bool Ext2UndeleteProvider::exists(const std::string_view path) const {
    const auto key = normalize(path);
    impl_->check_source({});
    return key == "/" || impl_->nodes_.contains(key);
}

SpaceInfo Ext2UndeleteProvider::space(const std::string_view path) const {
    static_cast<void>(stat(path, {}));
    return {.capacity=impl_->filesystem_bytes_, .free=impl_->free_bytes_, .available=0U};
}

void Ext2UndeleteProvider::read(
    const std::string_view path, const std::uint64_t offset, const std::uint64_t length,
    std::ostream& output, const Cancellation& cancellation
) const {
    const auto key = normalize(path);
    impl_->check_source(cancellation);
    if (key == "/") path_error(key, std::errc::is_a_directory);
    const auto& node = impl_->node(key);
    if (!node.recovery.recoverable) {
        fail("cannot recover inode " + std::to_string(node.entry.inode) + ": " + node.recovery.diagnostic);
    }
    if (offset >= node.entry.size || length == 0U) return;
    auto remaining = std::min(length, node.entry.size - offset);
    auto position = offset;
    while (remaining != 0U) {
        cancellation.throw_if_requested();
        const auto logical = position / impl_->info_.block_size;
        const auto within = position % impl_->info_.block_size;
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(
            remaining, impl_->info_.block_size - within));
        const auto block = node.data.at(static_cast<std::size_t>(logical));
        if (block == 0U) fail("missing retained data block");
        const auto bytes = impl_->read_exact(
            static_cast<std::uint64_t>(block) * impl_->info_.block_size + within, count, cancellation);
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!output.good()) fail("failed while writing recovered bytes");
        position += count;
        remaining -= count;
    }
    impl_->check_source(cancellation);
}

void Ext2UndeleteProvider::write(
    std::string_view, std::istream&, std::optional<std::uint64_t>, bool,
    std::optional<std::string>, const Cancellation&, const ProgressCallback&
) { readonly(); }
void Ext2UndeleteProvider::create_directory(std::string_view) { readonly(); }
void Ext2UndeleteProvider::remove(
    std::string_view, bool, const Cancellation&, const ProgressCallback&
) { readonly(); }
void Ext2UndeleteProvider::rename(std::string_view, std::string_view, bool) { readonly(); }
bool Ext2UndeleteProvider::copy_direct(
    std::string_view, std::string_view, bool, const Cancellation&, const ProgressCallback&
) { readonly(); }
void Ext2UndeleteProvider::set_permissions(std::string_view, fs::perms) { readonly(); }
void Ext2UndeleteProvider::set_owner(
    std::string_view, std::optional<std::string>, std::optional<std::string>
) { readonly(); }
void Ext2UndeleteProvider::create_symlink(
    std::string_view, std::string_view, bool, const Cancellation&
) { readonly(); }
void Ext2UndeleteProvider::create_hardlink(std::string_view, std::string_view) { readonly(); }

std::shared_ptr<Ext2UndeleteProvider> open_ext2_undelete_provider(
    ProviderPtr source, std::string path, std::string source_location,
    const Ext2UndeleteOptions options, const Cancellation& cancellation
) {
    return std::make_shared<Ext2UndeleteProvider>(
        std::move(source), std::move(path), std::move(source_location), options, cancellation);
}

}  // namespace mcpp
