#pragma once

#include "mcpp/provider.hpp"

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
        std::chrono::seconds timeout = std::chrono::seconds(60)
    );
    [[nodiscard]] ProviderPtr open(
        std::string_view archive_path,
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
};

[[nodiscard]] Location open_location(
    std::string_view value,
    const ProviderPtr& local_provider,
    ArchiveCache* archives = nullptr,
    const Cancellation& cancellation = {}
);

[[nodiscard]] std::string format_location(const ProviderPtr& provider, std::string_view path);
[[nodiscard]] bool is_archive_location(std::string_view value) noexcept;

}  // namespace mcpp
