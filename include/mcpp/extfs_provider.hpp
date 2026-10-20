#pragma once

#include "mcpp/provider.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mcpp {

struct ExtfsLimits {
    std::size_t container_bytes{64U * 1024U * 1024U};
    std::size_t member_bytes{64U * 1024U * 1024U};
    std::size_t listing_bytes{16U * 1024U * 1024U};
    std::size_t members{100000U};
    std::size_t path_bytes{4096U};
    std::size_t memory_bytes{512U * 1024U * 1024U};
    unsigned int cpu_seconds{15U};
    std::chrono::milliseconds elapsed{30000};
};

struct ExtfsOpenOptions {
    ExtfsLimits limits{};
    // Empty uses private storage below XDG_CACHE_HOME or HOME, never /tmp.
    std::filesystem::path cache_directory;
    // Empty uses the native user mc/extfs.d, then system mc/extfs.d paths.
    // Helpers are trusted executable configuration, not sandboxed programs.
    std::vector<std::filesystem::path> helper_directories;
    // The protocol has no capability query. Unsupported helper commands fail
    // explicitly; changes are never published without checking their result.
    bool writable{true};
};

struct ExtfsHelper {
    std::string name;
    std::filesystem::path executable;
    std::string revision;
    bool requires_source{true};
};

[[nodiscard]] std::vector<ExtfsHelper> discover_extfs_helpers(
    const ExtfsOpenOptions& options = {}
);
[[nodiscard]] std::optional<ExtfsHelper> find_extfs_helper(
    std::string_view name, const ExtfsOpenOptions& options = {}
);

struct ExtfsSource {
    ProviderPtr provider;
    std::string path;
    std::string revision;
    std::string location;
};

// MC's native container/helper://member protocol: list, copyout, copyin, rm,
// mkdir and rmdir. Only local, source-based helpers are enabled. Source-less
// "+" helpers and the undocumented "run" operation are not invoked.
//
// Every helper receives a private local copy, never a virtual path or the
// original container. Listings and copied-out regular files are bounded and
// checked. Writes relist and verify a private candidate, then publish through
// the captured source provider's expected-revision write. Source snapshots,
// member data and source-provider ownership outlive the cache that opened them.
// Publication has the source provider's atomic-replacement/advisory-lock
// guarantees; it is not a filesystem compare-and-swap.
// Absolute symlink targets belong to this virtual root. Callers must not
// preserve them as absolute host links when copying to another filesystem.
class ExtfsProvider final : public Provider {
public:
    ~ExtfsProvider() override;
    ExtfsProvider(const ExtfsProvider&) = delete;
    ExtfsProvider& operator=(const ExtfsProvider&) = delete;

    [[nodiscard]] ExtfsSource source() const;
    [[nodiscard]] const std::string& helper() const noexcept;
    [[nodiscard]] std::string location(std::string_view path = "/") const;
    void ensure_writable(std::string_view path) const;
    void validate_directory_target(
        std::string_view source, const ExtfsProvider& destination_provider,
        std::string_view destination
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
    explicit ExtfsProvider(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
    friend class ExtfsCache;
};

class ExtfsCache {
public:
    explicit ExtfsCache(
        std::chrono::seconds timeout = std::chrono::seconds(60),
        ExtfsOpenOptions options = {}
    );
    ~ExtfsCache();
    ExtfsCache(const ExtfsCache&) = delete;
    ExtfsCache& operator=(const ExtfsCache&) = delete;
    [[nodiscard]] ProviderPtr open(
        const ProviderPtr& provider, std::string_view path,
        std::string_view source_location, std::string_view helper,
        const Cancellation& cancellation = {}
    );
    void set_timeout(std::chrono::seconds timeout);
    void prune();
    [[nodiscard]] std::vector<std::string> locations();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mcpp
