#pragma once

#include "mcpp/provider.hpp"
#include "mcpp/transform_provider.hpp"

#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <string_view>

namespace mcpp {

struct Location {
    ProviderPtr provider;
    std::string path;
};

class ArchiveCache {
public:
    explicit ArchiveCache(
        ProviderPtr local_provider,
        std::chrono::seconds timeout = std::chrono::seconds(60),
        TransformOpenOptions transformations = {}
    );
    [[nodiscard]] ProviderPtr open(
        std::string_view archive_path,
        const Cancellation& cancellation = {}
    );
    [[nodiscard]] ProviderPtr open_transform(
        const Location& source, std::string_view converter,
        std::string_view parameter = {},
        const Cancellation& cancellation = {}
    );
    [[nodiscard]] std::string format(const ProviderPtr& provider, std::string_view path) const;
    [[nodiscard]] std::vector<std::string> locations();
    void set_timeout(std::chrono::seconds timeout);
    void prune();

private:
    struct Record {
        ProviderPtr provider;
        std::string archive_path;
        std::chrono::steady_clock::time_point last_used;
    };
    ProviderPtr local_;
    std::chrono::seconds timeout_;
    mutable std::mutex mutex_;
    std::map<std::pair<std::string, std::string>, Record> records_;
    TransformCache transformations_;
};

[[nodiscard]] Location open_location(
    std::string_view value,
    const ProviderPtr& local_provider,
    ArchiveCache* archives = nullptr,
    const Cancellation& cancellation = {}
);

// A leading "/" is rooted in relative_to.provider, not in the host filesystem.
// In particular /file in an archive can never name the host's /file.
[[nodiscard]] Location open_location(
    std::string_view value,
    const Location& relative_to,
    ArchiveCache* archives = nullptr,
    const Cancellation& cancellation = {}
);

[[nodiscard]] std::string format_location(const ProviderPtr& provider, std::string_view path);
[[nodiscard]] bool is_archive_location(std::string_view value) noexcept;
[[nodiscard]] bool is_transform_location(std::string_view value) noexcept;
// Includes unsupported/remote schemes so callers do not mangle them with host
// filesystem normalization before open_location reports the actual error.
[[nodiscard]] bool is_virtual_location(std::string_view value) noexcept;

}  // namespace mcpp
