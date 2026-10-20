#include "mcpp/location.hpp"
#include "mcpp/archive_provider.hpp"
#include "mcpp/config.hpp"
#include "mcpp/ext2_undelete_provider.hpp"
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

ArchiveCache::ArchiveCache(
    ProviderPtr local_provider, const std::chrono::seconds timeout,
    TransformOpenOptions transformations
) : local_(std::move(local_provider)), timeout_(timeout),
    transformations_(timeout, std::move(transformations)), external_filesystems_(timeout) {
    if (!local_) throw std::invalid_argument("local provider is null");
    if (timeout.count() < 0) throw std::invalid_argument("VFS timeout cannot be negative");
}

ProviderPtr ArchiveCache::open_transform(
    const Location& source, const std::string_view converter,
    const std::string_view parameter, const Cancellation& cancellation
) {
    return transformations_.open(
        source.provider, source.path, format(source.provider, source.path),
        converter, parameter, cancellation);
}

ProviderPtr ArchiveCache::open(const std::string_view archive_path, const Cancellation& cancellation) {
    return open(Location{local_, local_->normalize(archive_path)}, cancellation);
}

ProviderPtr ArchiveCache::open_extfs(
    const Location& source, const std::string_view helper, const Cancellation& cancellation
) {
    return external_filesystems_.open(source.provider, source.path,
        format(source.provider, source.path), helper, cancellation);
}

ProviderPtr ArchiveCache::open_extfs_source_less(
    ExtfsOrigin origin, const std::string_view helper, const Cancellation& cancellation
) {
    return external_filesystems_.open_source_less(std::move(origin), helper, cancellation);
}

ProviderPtr ArchiveCache::open(const Location& source, const Cancellation& cancellation) {
    return open_cached(source, CacheKind::archive, cancellation);
}

ProviderPtr ArchiveCache::open_undelete(const Location& source, const Cancellation& cancellation) {
    return open_cached(source, CacheKind::ext2_undelete, cancellation);
}

ProviderPtr ArchiveCache::open_cached(
    const Location& source, const CacheKind kind, const Cancellation& cancellation
) {
    prune();
    const std::string kind_name = kind == CacheKind::archive ? "archive" : "ext2 image";
    if (!source.provider) throw std::invalid_argument(kind_name + " source provider is null");
    const auto path = source.provider->normalize(source.path);
    const auto entry = source.provider->stat_follow(path, cancellation);
    if (entry.kind != EntryKind::file) throw std::runtime_error(kind_name + " is not a regular file: " + path);
    const auto key = std::tuple{kind, source.provider, path, entry.revision};
    {
        const std::lock_guard lock(mutex_);
        const auto found = records_.find(key);
        if (found != records_.end()) {
            found->second.last_used = std::chrono::steady_clock::now();
            return found->second.provider;
        }
    }
    const auto label = format(source.provider, path);
    ProviderPtr provider;
    if (kind == CacheKind::archive) {
        provider = open_archive_provider(source.provider, path,
            {.draft_directory=archive_draft_directory()}, cancellation);
    } else {
        provider = open_ext2_undelete_provider(source.provider, path, label, {}, cancellation);
    }
    if (source.provider->stat_follow(path, cancellation).revision != entry.revision) {
        throw std::runtime_error(kind_name + " changed while it was being opened: " + path);
    }
    const std::lock_guard lock(mutex_);
    const auto [found, inserted] = records_.try_emplace(
        key, Record{provider, label, std::chrono::steady_clock::now()}
    );
    if (!inserted) found->second.last_used = std::chrono::steady_clock::now();
    return found->second.provider;
}

std::string ArchiveCache::format(const ProviderPtr& provider, const std::string_view path) const {
    const auto* archive = dynamic_cast<const ArchiveProvider*>(provider.get());
    if (archive == nullptr) return format_location(provider, path);
    {
        const std::lock_guard lock(mutex_);
        for (const auto& [key, record] : records_) {
            static_cast<void>(key);
            if (record.provider == provider) return record.source_location + archive_suffix(*archive, path);
        }
    }
    return format_location(provider, path);
}

void ArchiveCache::prune() {
    transformations_.prune();
    external_filesystems_.prune();
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
    transformations_.set_timeout(timeout);
    external_filesystems_.set_timeout(timeout);
    prune();
}

std::vector<std::string> ArchiveCache::locations() {
    prune();
    const auto transformed = transformations_.locations();
    const auto external = external_filesystems_.locations();
    const std::lock_guard lock(mutex_);
    std::set<std::string> paths(transformed.begin(), transformed.end());
    paths.insert(external.begin(), external.end());
    for (const auto& [key, record] : records_) {
        static_cast<void>(key);
        const auto* archive = dynamic_cast<const ArchiveProvider*>(record.provider.get());
        paths.insert(archive != nullptr
            ? record.source_location + archive_suffix(*archive, "/")
            : format_location(record.provider, "/"));
    }
    return {paths.begin(), paths.end()};
}

std::string format_location(const ProviderPtr& provider, const std::string_view path) {
    if (!provider) throw std::invalid_argument("location provider is null");
    if (const auto* archive = dynamic_cast<const ArchiveProvider*>(provider.get())) {
        return format_location(archive->source_provider(), archive->source_path()) + archive_suffix(*archive, path);
    }
    if (const auto* undelete = dynamic_cast<const Ext2UndeleteProvider*>(provider.get())) {
        return undelete->location(path);
    }
    if (const auto* external = dynamic_cast<const ExtfsProvider*>(provider.get())) {
        return external->location(path);
    }
    if (const auto* transformed = dynamic_cast<const TransformProvider*>(provider.get())) {
        static_cast<void>(transformed->normalize(path));
        return transformed->location();
    }
    return std::string(path);
}

std::optional<Location> source_location(const Provider& provider) {
    if (const auto* archive = dynamic_cast<const ArchiveProvider*>(&provider)) {
        return Location{archive->source_provider(), archive->source_path()};
    }
    if (const auto* undelete = dynamic_cast<const Ext2UndeleteProvider*>(&provider)) {
        const auto source = undelete->source();
        return Location{source.provider, source.path};
    }
    if (const auto* external = dynamic_cast<const ExtfsProvider*>(&provider)) {
        if (external->is_source_less()) return std::nullopt;
        const auto source = external->source();
        return Location{source.provider, source.path};
    }
    if (const auto* transformed = dynamic_cast<const TransformProvider*>(&provider)) {
        const auto& source = transformed->source();
        return Location{source.provider, source.path};
    }
    return std::nullopt;
}

Location local_execution_directory(Location location) {
    for (unsigned depth = 0; depth < 16U; ++depth) {
        if (!location.provider) throw std::invalid_argument("execution provider is null");
        if (dynamic_cast<const LocalProvider*>(location.provider.get()) != nullptr) {
            location.path = location.provider->normalize(location.path.empty() ? "." : location.path);
            return location;
        }
        if (const auto* external = dynamic_cast<const ExtfsProvider*>(location.provider.get());
            external != nullptr && external->is_source_less()) {
            const auto origin = external->origin();
            if (!origin) throw std::logic_error("source-less helper has no captured origin");
            location = {origin->working.provider, origin->working.path};
            continue;
        }
        const auto source = source_location(*location.provider);
        if (!source || !source->provider) {
            throw std::runtime_error("virtual location has no local execution directory");
        }
        location = {source->provider, source->provider->parent(source->path)};
    }
    throw std::runtime_error("too many nested execution directories");
}

bool is_archive_location(const std::string_view value) noexcept {
    return value.find("/utar://") != std::string_view::npos ||
           value.find("/ucpio://") != std::string_view::npos;
}

bool is_virtual_location(const std::string_view value) noexcept {
    return value.find("://") != std::string_view::npos;
}

bool is_transform_location(const std::string_view value) noexcept {
    const auto marker = value.rfind("://");
    if (marker == std::string_view::npos) return false;
    const auto slash = value.rfind('/', marker);
    if (slash == std::string_view::npos || slash == 0U || slash + 1U == marker) return false;
    const auto name = value.substr(slash + 1U, marker - slash - 1U);
    return name != "utar" && name != "ucpio" && name != "undel";
}

namespace {

Location plain_location(const std::string_view value, const Location& base) {
    if (!base.provider) throw std::invalid_argument("location provider is null");
    const std::filesystem::path requested(value);
    const auto path = base.path.empty() || requested.is_absolute()
        ? requested : std::filesystem::path(base.path) / requested;
    return {base.provider, base.provider->normalize(path.lexically_normal().generic_string())};
}

Location open_location_impl(
    const std::string_view value, const Location& base,
    ArchiveCache* archives, const Cancellation& cancellation, const std::size_t depth
) {
    cancellation.throw_if_requested();
    if (!base.provider) throw std::invalid_argument("location provider is null");
    if (value.size() > 65536U || value.find('\0') != std::string_view::npos) {
        throw std::invalid_argument("invalid location path");
    }
    if (depth > 8U) throw std::invalid_argument("too many nested single-file transformations");
    const auto marker = value.rfind("://");
    if (marker == std::string_view::npos) return plain_location(value, base);
    const auto slash = value.rfind('/', marker);
    const auto name = slash == std::string_view::npos ? value.substr(0, marker)
        : value.substr(slash + 1U, marker - slash - 1U);
    constexpr std::string_view remote_schemes[]{
        "az", "azure", "azblob", "blob", "ftp", "sftp", "fish", "ssh",
        "sh", "smb", "smbfs", "http", "https"
    };
    if (std::ranges::any_of(remote_schemes, [name](const auto scheme) { return name == scheme; })) {
        throw std::invalid_argument("remote providers are not enabled: " + std::string(value));
    }
    if (name == "undel" && (slash == std::string_view::npos || slash == 0U)) {
        throw std::invalid_argument(
            "block-device undelete is not enabled; use a regular image/undel://member location");
    }
    const auto helper = name == "utar" || name == "ucpio" || name == "undel"
        ? std::optional<ExtfsHelper>{} : find_extfs_helper(name);
    const auto parameter = value.substr(marker + 3U);
    if (helper && !helper->requires_source) {
        const auto outer = slash == std::string_view::npos ? std::string_view(".")
            : slash == 0U ? std::string_view("/") : value.substr(0, slash);
        const auto working = local_execution_directory(base);
        const auto mount = slash == std::string_view::npos ? working
            : open_location_impl(outer, base, archives, cancellation, depth + 1U);
        ExtfsOrigin origin{
            {mount.provider, mount.path, format_location(mount.provider, mount.path)},
            {working.provider, working.path, format_location(working.provider, working.path)}
        };
        ProviderPtr provider;
        if (archives != nullptr) {
            provider = archives->open_extfs_source_less(std::move(origin), name, cancellation);
        } else {
            ExtfsCache temporary;
            provider = temporary.open_source_less(std::move(origin), name, cancellation);
        }
        const auto member = provider->normalize("/" + std::string(parameter));
        return {.provider=std::move(provider), .path=member};
    }
    if (slash == std::string_view::npos || slash == 0U) {
        throw std::invalid_argument("non-local providers are not enabled: " + std::string(value));
    }
    const auto outer = value.substr(0, slash);
    if (name == "utar" || name == "ucpio") {
        if (dynamic_cast<const LocalProvider*>(base.provider.get()) != nullptr &&
            !is_virtual_location(outer)) {
            const auto literal = plain_location(value, base);
            if (literal.provider->exists(literal.path)) return literal;
        }
        const auto source = open_location_impl(outer, base, archives, cancellation, depth + 1U);
        ArchiveCache temporary(base.provider);
        auto provider = (archives == nullptr ? temporary : *archives).open(source, cancellation);
        const auto* archive = dynamic_cast<const ArchiveProvider*>(provider.get());
        if ((archive->format() == ArchiveFormat::tar) != (name == "utar")) {
            throw std::invalid_argument("archive format does not match the VFS path");
        }
        const auto member = std::filesystem::path("/" + std::string(parameter))
                                .lexically_normal().generic_string();
        return {.provider=std::move(provider), .path=member};
    }
    const auto source = open_location_impl(outer, base, archives, cancellation, depth + 1U);
    if (name == "undel") {
        ArchiveCache temporary(base.provider);
        auto provider = (archives == nullptr ? temporary : *archives).open_undelete(source, cancellation);
        const auto member = provider->normalize("/" + std::string(parameter));
        return {.provider=std::move(provider), .path=member};
    }
    if (helper) {
        ProviderPtr provider;
        if (archives != nullptr) provider = archives->open_extfs(source, name, cancellation);
        else {
            ExtfsCache temporary;
            provider = temporary.open(source.provider, source.path,
                format_location(source.provider, source.path), name, cancellation);
        }
        const auto member = std::filesystem::path("/" + std::string(parameter))
                                .lexically_normal().generic_string();
        return {.provider=std::move(provider), .path=member};
    }
    ProviderPtr provider;
    if (archives != nullptr) {
        provider = archives->open_transform(source, name, parameter, cancellation);
    } else {
        TransformCache temporary;
        provider = temporary.open(source.provider, source.path,
            format_location(source.provider, source.path), name, parameter, cancellation);
    }
    return {.provider=std::move(provider), .path=std::string(TransformProvider::output_path())};
}

}  // namespace

Location open_location(
    const std::string_view value, const ProviderPtr& local_provider,
    ArchiveCache* archives, const Cancellation& cancellation
) {
    return open_location_impl(value, {local_provider, {}}, archives, cancellation, 0);
}

Location open_location(
    const std::string_view value, const Location& relative_to,
    ArchiveCache* archives, const Cancellation& cancellation
) {
    return open_location_impl(value, relative_to, archives, cancellation, 0);
}

}  // namespace mcpp
