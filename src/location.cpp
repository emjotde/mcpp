#include "mcpp/location.hpp"
#include "mcpp/archive_provider.hpp"
#include "mcpp/config.hpp"
#include "mcpp/local_provider.hpp"

#include <algorithm>
#include <cstdlib>
#include <set>
#include <stdexcept>

namespace mcpp {
namespace {

std::string archive_suffix(const ArchiveProvider& provider, const std::string_view path) {
    return std::string(provider.format() == ArchiveFormat::tar ? "/utar://" : "/ucpio://") +
        std::string(path.starts_with('/') ? path.substr(1) : path);
}

std::filesystem::path archive_draft_directory() {
    std::filesystem::path directory;
    if (const auto* cache = std::getenv("XDG_CACHE_HOME");
        cache != nullptr && std::filesystem::path(cache).is_absolute()) {
        directory = std::filesystem::path(cache) / "mcpp" / "archive-drafts";
    } else if (const auto* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        directory = std::filesystem::path(home) / ".cache" / "mcpp" / "archive-drafts";
    } else {
        directory = default_config_path().parent_path() / "archive-drafts";
    }
    if (std::filesystem::create_directories(directory.parent_path())) {
        std::filesystem::permissions(directory.parent_path(), std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace);
    }
    return directory;
}

}  // namespace

ArchiveCache::ArchiveCache(ProviderPtr local_provider, const std::chrono::seconds timeout)
    : local_(std::move(local_provider)), timeout_(timeout) {
    if (!local_) throw std::invalid_argument("local provider is null");
    if (timeout.count() < 0) throw std::invalid_argument("VFS timeout cannot be negative");
}

ProviderPtr ArchiveCache::open(const std::string_view archive_path, const Cancellation& cancellation) {
    prune();
    const auto* local = dynamic_cast<const LocalProvider*>(local_.get());
    if (local == nullptr) throw std::invalid_argument("archives require a local filesystem path");
    const auto path = local->normalize(archive_path);
    const auto entry = local->stat_follow(path, cancellation);
    if (entry.kind != EntryKind::file) throw std::runtime_error("archive is not a regular file: " + path);
    const auto native = local->native_path(path);
    const auto key = std::pair{path, entry.revision};
    {
        const std::lock_guard lock(mutex_);
        const auto found = records_.find(key);
        if (found != records_.end()) {
            found->second.last_used = std::chrono::steady_clock::now();
            return found->second.provider;
        }
    }
    auto provider = open_archive_provider(native, {.draft_directory=archive_draft_directory()}, cancellation);
    if (local->stat_follow(path, cancellation).revision != entry.revision) {
        throw std::runtime_error("archive changed while it was being opened: " + path);
    }
    const std::lock_guard lock(mutex_);
    const auto [found, inserted] = records_.try_emplace(
        key, Record{provider, path, std::chrono::steady_clock::now()}
    );
    if (!inserted) found->second.last_used = std::chrono::steady_clock::now();
    return found->second.provider;
}

std::string ArchiveCache::format(const ProviderPtr& provider, const std::string_view path) const {
    const auto* archive = dynamic_cast<const ArchiveProvider*>(provider.get());
    if (archive == nullptr) return format_location(provider, path);
    const std::lock_guard lock(mutex_);
    for (const auto& [key, record] : records_) {
        static_cast<void>(key);
        if (record.provider == provider) return record.archive_path + archive_suffix(*archive, path);
    }
    return format_location(provider, path);
}

void ArchiveCache::prune() {
    const auto now = std::chrono::steady_clock::now();
    const std::lock_guard lock(mutex_);
    for (auto iterator = records_.begin(); iterator != records_.end();) {
        auto& record = iterator->second;
        if (record.provider.use_count() > 1) {
            record.last_used = now;
            ++iterator;
        } else if (now - record.last_used >= timeout_) iterator = records_.erase(iterator);
        else ++iterator;
    }
}

void ArchiveCache::set_timeout(const std::chrono::seconds timeout) {
    if (timeout.count() < 0) throw std::invalid_argument("VFS timeout cannot be negative");
    {
        const std::lock_guard lock(mutex_);
        timeout_ = timeout;
    }
    prune();
}

std::vector<std::string> ArchiveCache::locations() {
    prune();
    const std::lock_guard lock(mutex_);
    std::set<std::string> paths;
    for (const auto& [key, record] : records_) {
        static_cast<void>(key);
        const auto* archive = dynamic_cast<const ArchiveProvider*>(record.provider.get());
        paths.insert(record.archive_path + archive_suffix(*archive, "/"));
    }
    return {paths.begin(), paths.end()};
}

std::string format_location(const ProviderPtr& provider, const std::string_view path) {
    if (!provider) throw std::invalid_argument("location provider is null");
    if (const auto* archive = dynamic_cast<const ArchiveProvider*>(provider.get())) {
        return archive->archive_path().string() + archive_suffix(*archive, path);
    }
    return std::string(path);
}

bool is_archive_location(const std::string_view value) noexcept {
    return value.find("/utar://") != std::string_view::npos ||
           value.find("/ucpio://") != std::string_view::npos;
}

Location open_location(
    const std::string_view value,
    const ProviderPtr& local_provider,
    ArchiveCache* archives,
    const Cancellation& cancellation
) {
    if (!local_provider) {
        throw std::invalid_argument("local provider is null");
    }
    const auto tar = value.rfind("/utar://");
    const auto cpio = value.rfind("/ucpio://");
    if (tar != std::string_view::npos || cpio != std::string_view::npos) {
        if (local_provider->exists(value)) {
            return {.provider=local_provider, .path=local_provider->normalize(value)};
        }
        const bool is_tar = tar != std::string_view::npos &&
            (cpio == std::string_view::npos || tar > cpio);
        const auto split = is_tar ? tar : cpio;
        const auto outer = value.substr(0, split);
        if (outer.find("://") != std::string_view::npos) {
            throw std::invalid_argument("nested archive locations are not supported yet");
        }
        ArchiveCache temporary(local_provider);
        auto provider = (archives == nullptr ? temporary : *archives).open(outer, cancellation);
        const auto* archive = dynamic_cast<const ArchiveProvider*>(provider.get());
        if ((archive->format() == ArchiveFormat::tar) != is_tar) {
            throw std::invalid_argument("archive format does not match the VFS path");
        }
        const auto offset = split + (is_tar ? 8U : 9U);
        const auto member = std::filesystem::path("/" + std::string(value.substr(offset)))
                                .lexically_normal().generic_string();
        return {.provider=std::move(provider), .path=member};
    }
    if (value.find("://") != std::string_view::npos) {
        throw std::invalid_argument(
            "remote providers are not enabled yet: " +
            std::string(value)
        );
    }
    return {
        .provider = local_provider,
        .path = local_provider->normalize(value),
    };
}

}  // namespace mcpp
