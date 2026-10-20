#pragma once

#include "mcpp/provider.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace mcpp {

struct TransformLimits {
    std::size_t input_bytes{64U * 1024U * 1024U};
    std::size_t output_bytes{64U * 1024U * 1024U};
    std::size_t registry_bytes{1024U * 1024U};
    std::size_t rules{256U};
    std::size_t path_bytes{4096U};
    std::size_t memory_bytes{512U * 1024U * 1024U};
    unsigned int cpu_seconds{15U};
    std::chrono::milliseconds elapsed{30000};
};

struct TransformOpenOptions {
    TransformLimits limits{};
    // Empty selects private storage below XDG_CACHE_HOME or HOME, never /tmp.
    std::filesystem::path cache_directory;
    // An explicit file must exist. Otherwise MC_DATADIR/sfs.ini is selected
    // when set, then /etc/mcpp/sfs.ini or /etc/mc/sfs.ini when present.
    // Rules are trusted shell commands, not sandboxed plugins.
    std::optional<std::filesystem::path> registry_file;
};

struct TransformSource {
    ProviderPtr provider;
    std::string path;
    std::string revision;
    std::string location;
};

// A native source/name://parameter location denotes one file, not a directory.
// Its sole internal path is "/". Source paths are never treated as host paths;
// helpers receive a private, revision-checked local copy through %1, the literal
// suffix through %2, and a pre-created 0600 output through %3.
class TransformProvider final : public Provider {
public:
    ~TransformProvider() override;
    TransformProvider(const TransformProvider&) = delete;
    TransformProvider& operator=(const TransformProvider&) = delete;

    [[nodiscard]] const TransformSource& source() const noexcept;
    [[nodiscard]] const std::string& converter() const noexcept;
    [[nodiscard]] const std::string& parameter() const noexcept;
    [[nodiscard]] std::string location() const;
    [[nodiscard]] static constexpr std::string_view output_path() noexcept { return "/"; }

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
    explicit TransformProvider(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
    friend class TransformCache;
};

class TransformCache {
public:
    explicit TransformCache(
        std::chrono::seconds timeout = std::chrono::seconds(60),
        TransformOpenOptions options = {}
    );
    ~TransformCache();
    TransformCache(const TransformCache&) = delete;
    TransformCache& operator=(const TransformCache&) = delete;
    [[nodiscard]] ProviderPtr open(
        const ProviderPtr& provider, std::string_view path,
        std::string_view source_location, std::string_view converter,
        std::string_view parameter = {}, const Cancellation& cancellation = {}
    );
    void set_timeout(std::chrono::seconds timeout);
    void prune();
    [[nodiscard]] std::vector<std::string> locations();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// Shared by parsed viewing and explicit SFS paths. Detection is byte-based;
// callers still decide whether a compressed tar/cpio should be an archive.
[[nodiscard]] std::optional<std::string_view> compression_decoder(std::string_view header) noexcept;
[[nodiscard]] std::string decompress_bytes(
    std::string_view bytes, std::string_view decoder,
    const Cancellation& cancellation = {}, TransformOpenOptions options = {}
);

}  // namespace mcpp
