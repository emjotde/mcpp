#include "mcpp/ext2_undelete_provider.hpp"
#include "mcpp/local_provider.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {
namespace fs = std::filesystem;

void check(const bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename Function>
void rejects(Function function, const std::string_view diagnostic) {
    try {
        function();
    } catch (const std::exception& error) {
        if (std::string_view(error.what()).find(diagnostic) != std::string_view::npos) return;
        throw std::runtime_error(std::string("unexpected rejection: ") + error.what());
    }
    throw std::runtime_error("expected rejection: " + std::string(diagnostic));
}

std::string get(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    check(input.good(), "open original payload");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

class ImageSource final : public mcpp::Provider {
public:
    explicit ImageSource(const fs::path& image)
        : local_(std::make_shared<mcpp::LocalProvider>(image.parent_path())),
          image_(image.string()) {}

    std::uint64_t revision_epoch{0};
    bool short_reads{false};
    std::size_t writes{0};

    std::string id() const override { return "opaque-test-source-with-no-filesystem-path"; }
    std::string display_name() const override { return "original image source"; }
    mcpp::ProviderCapability capabilities() const override { return mcpp::ProviderCapability::read; }
    std::string normalize(const std::string_view path) const override {
        if (path == "/file" || path == "file") return "/file";
        throw std::runtime_error("test source has only the actual /file image");
    }
    std::string join(std::string_view, std::string_view name) const override { return normalize(name); }
    std::string parent(std::string_view) const override { return "/"; }
    std::string basename(std::string_view path) const override {
        static_cast<void>(normalize(path));
        return "file";
    }
    std::vector<mcpp::Entry> list(
        std::string_view, const mcpp::ListRequest&, const mcpp::Cancellation&
    ) const override { throw std::runtime_error("not a directory"); }
    mcpp::Entry stat(const std::string_view path, const mcpp::Cancellation& cancellation) const override {
        static_cast<void>(normalize(path));
        auto entry = local_->stat_follow(image_, cancellation);
        entry.name = "file";
        entry.path = "/file";
        entry.revision += ":" + std::to_string(revision_epoch);
        return entry;
    }
    mcpp::Entry stat_follow(const std::string_view path, const mcpp::Cancellation& cancellation) const override {
        return stat(path, cancellation);
    }
    bool exists(const std::string_view path) const override { return path == "/file"; }
    mcpp::SpaceInfo space(std::string_view) const override { return {}; }
    void read(
        const std::string_view path, const std::uint64_t offset, const std::uint64_t length,
        std::ostream& output, const mcpp::Cancellation& cancellation
    ) const override {
        static_cast<void>(normalize(path));
        local_->read(image_, offset, short_reads && length != 0U ? length - 1U : length, output, cancellation);
    }
    void write(
        std::string_view, std::istream&, std::optional<std::uint64_t>, bool,
        std::optional<std::string>, const mcpp::Cancellation&, const mcpp::ProgressCallback&
    ) override { mutation(); }
    void create_directory(std::string_view) override { mutation(); }
    void remove(
        std::string_view, bool, const mcpp::Cancellation&, const mcpp::ProgressCallback&
    ) override { mutation(); }
    void rename(std::string_view, std::string_view, bool) override { mutation(); }
    bool copy_direct(
        std::string_view, std::string_view, bool, const mcpp::Cancellation&, const mcpp::ProgressCallback&
    ) override { mutation(); }
    void set_permissions(std::string_view, fs::perms) override { mutation(); }
    void set_owner(
        std::string_view, std::optional<std::string>, std::optional<std::string>
    ) override { mutation(); }
    void create_symlink(
        std::string_view, std::string_view, bool, const mcpp::Cancellation&
    ) override { mutation(); }
    void create_hardlink(std::string_view, std::string_view) override { mutation(); }

private:
    [[noreturn]] void mutation() {
        ++writes;
        throw std::runtime_error("production code called a source mutation");
    }
    std::shared_ptr<mcpp::LocalProvider> local_;
    std::string image_;
};

std::string contents(const mcpp::Provider& provider, const std::string_view path,
                     const std::uint64_t offset = 0U,
                     const std::uint64_t length = std::numeric_limits<std::uint64_t>::max()) {
    std::ostringstream output;
    provider.read(path, offset, length, output, {});
    return output.str();
}

std::string member(const mcpp::Provider& provider, const std::uint64_t inode) {
    for (const auto& entry : provider.list("/", {}, {})) {
        if (entry.inode == inode) return entry.path;
    }
    throw std::runtime_error("deleted fixture inode is missing");
}

void recovery_case(const fs::path& image, const std::uint64_t direct_inode,
                   const std::uint64_t long_inode, const std::string& direct_bytes,
                   const std::string& long_bytes) {
    auto source = std::make_shared<ImageSource>(image);
    std::weak_ptr<ImageSource> held_source = source;
    const auto revision = source->stat("/file", {}).revision;
    auto provider = mcpp::open_ext2_undelete_provider(source, "/file", "original-image://file");
    check(provider->source().provider == source && provider->source().path == "/file" &&
          provider->source().revision == revision, "capture the real virtual source, not an ID-derived host path");
    check(provider->location() == "original-image://file/undel://", "format the captured source URI");
    const auto info = provider->image_info();
    check(info.block_size == 1024U && info.inode_size == 128U && info.groups == 2U &&
          info.deleted_files == 2U, "decode the actual two-group ext2 geometry");
    check(provider->stat("/", {}).kind == mcpp::EntryKind::directory &&
          provider->stat("/", {}).mode == (S_IFDIR | 0555U), "synthetic read-only virtual root");
    check(provider->capabilities() == mcpp::ProviderCapability::read, "advertise read only");
    const auto direct = member(*provider, direct_inode);
    const auto longer = member(*provider, long_inode);
    const auto entry = provider->stat(longer, {});
    check(entry.size == long_bytes.size() && entry.uid == 1001U && entry.gid == 1001U &&
          entry.mode == (S_IFREG | 0640U) && entry.links == 0U,
          "retain deleted inode metadata rather than invented host metadata");
    check(entry.modified == std::chrono::system_clock::time_point{std::chrono::seconds(1600000100)} &&
          entry.accessed == std::chrono::system_clock::time_point{std::chrono::seconds(1600000000)} &&
          entry.changed == std::chrono::system_clock::time_point{std::chrono::seconds(1600000200)},
          "retain the original inode timestamps");
    const auto recovery = provider->recovery_info(longer);
    check(recovery.recoverable && recovery.diagnostic.empty() &&
          recovery.logical_blocks == 16U && recovery.free_data_blocks == 16U &&
          recovery.deleted > entry.modified && longer == "/" + std::to_string(long_inode) + ":16",
          "separate deletion time and free data blocks from inode timestamps and indirect blocks");
    source.reset();
    check(!held_source.expired(), "the view strongly owns its actual image provider");
    check(contents(*provider, direct) == direct_bytes && contents(*provider, longer) == long_bytes,
          "recover original direct and singly-indirect bytes across two inode groups");
    constexpr std::uint64_t boundary = 12U * 1024U - 7U;
    check(contents(*provider, longer, boundary, 1031U) == long_bytes.substr(boundary, 1031U),
          "read a bounded range across the direct-to-indirect boundary");
    check(contents(*provider, longer, long_bytes.size(), 100U).empty(), "EOF range is empty");
    const auto page = provider->list("/", {.sort=mcpp::SortField::inode, .offset=1U, .limit=1U}, {});
    check(page.size() == 1U && page.front().inode == long_inode, "sort and paginate the root listing");
    check(provider->parent(longer) == "/" && provider->join("/", entry.name) == longer &&
          provider->basename(longer) == entry.name &&
          provider->location(longer) == "original-image://file/undel://" + entry.name,
          "member path operations stay in the captured image namespace");
    rejects([&] { static_cast<void>(provider->stat("/file", {})); }, "undelete namespace");
    rejects([&] { static_cast<void>(provider->normalize("/../file")); }, "undelete namespace");
    std::istringstream input("must not be consumed");
    rejects([&] { provider->write(longer, input, {}, true, {}, {}, {}); }, "read-only");
    check(input.tellg() == 0, "reject writes before reading caller input");
    rejects([&] { provider->remove(longer, false, {}, {}); }, "read-only");
    rejects([&] { provider->create_directory("/new"); }, "read-only");
    mcpp::Cancellation cancelled;
    cancelled.request();
    std::ostringstream cancelled_output;
    rejects([&] { provider->read(longer, 0U, 1U, cancelled_output, cancelled); }, "operation cancelled");
    check(cancelled_output.str().empty(), "cancellation emits no recovered bytes");
    source = held_source.lock();
    source->short_reads = true;
    std::ostringstream short_output;
    rejects([&] { provider->read(longer, 0U, 16U, short_output, {}); }, "short or oversized image read");
    check(short_output.str().empty(), "a short source read never leaks zero padding or succeeds");
    source->short_reads = false;
    ++source->revision_epoch;
    std::ostringstream changed_output;
    rejects([&] { provider->read(longer, 0U, 16U, changed_output, {}); }, "source image changed");
    check(changed_output.str().empty() && source->writes == 0U, "a changed source is never read or written");
    provider.reset();
    source.reset();
    check(held_source.expired(), "source lifetime ends with the final owner");
}

void guard_case(const fs::path& image, const std::uint64_t direct_inode,
                const std::uint64_t long_inode) {
    const auto source = std::make_shared<ImageSource>(image);
    const auto provider = mcpp::open_ext2_undelete_provider(source, "/file", "damaged-image://file");
    const auto reused = member(*provider, direct_inode);
    const auto cleared = member(*provider, long_inode);
    const auto first = provider->recovery_info(reused);
    const auto second = provider->recovery_info(cleared);
    check(!first.recoverable && first.free_data_blocks == 0U &&
          first.diagnostic.find("reallocated") != std::string::npos,
          "an allocated data pointer is not recoverable");
    check(!second.recoverable && second.free_data_blocks == 15U &&
          second.diagnostic.find("missing block pointer") != std::string::npos,
          "a cleared pointer is not silently treated as a sparse zero block");
    std::ostringstream output;
    rejects([&] { provider->read(reused, 0U, 1U, output, {}); }, "reallocated");
    rejects([&] { provider->read(cleared, 1024U, 1U, output, {}); }, "missing block pointer");
    rejects([&] { provider->read(cleared, 0U, 0U, output, {}); }, "missing block pointer");
    check(output.str().empty() && source->writes == 0U,
          "incomplete files fail before even an otherwise readable range is emitted");
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (geteuid() != 1001U || !fs::is_regular_file("/.dockerenv") ||
            !fs::is_regular_file("/review/.operation-regressions-disposable")) {
            throw std::runtime_error("ext2 tests require uid1001 in the owned disposable container");
        }
        if (argc != 7) throw std::runtime_error("usage: test_ext2_undelete recovery|guard image direct_inode long_inode direct_payload long_payload");
        const auto image = fs::canonical(argv[2]);
        if (!image.string().starts_with("/review/cases/") || !fs::is_regular_file(image)) {
            throw std::runtime_error("ext2 test images must be regular owned disposable case files");
        }
        struct stat status {};
        check(::stat(image.c_str(), &status) == 0 && status.st_uid == geteuid(), "fixture is owned by uid1001");
        const auto direct_inode = std::stoull(argv[3]);
        const auto long_inode = std::stoull(argv[4]);
        if (std::string_view(argv[1]) == "recovery") {
            recovery_case(image, direct_inode, long_inode, get(argv[5]), get(argv[6]));
        } else if (std::string_view(argv[1]) == "guard") {
            guard_case(image, direct_inode, long_inode);
        } else throw std::runtime_error("unknown bounded ext2 test case");
        std::cout << "PASS ext2 undelete " << argv[1] << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
