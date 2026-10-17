#pragma once

#include "mcpp/cancellation.hpp"
#include "mcpp/entry.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mcpp {

enum class ProviderCapability : std::uint32_t {
    none = 0,
    read = 1U << 0U,
    write = 1U << 1U,
    remove = 1U << 2U,
    rename = 1U << 3U,
    direct_copy = 1U << 4U,
    links = 1U << 5U,
    permissions = 1U << 6U,
};

constexpr ProviderCapability operator|(
    const ProviderCapability left,
    const ProviderCapability right
) noexcept {
    return static_cast<ProviderCapability>(
        static_cast<std::uint32_t>(left) |
        static_cast<std::uint32_t>(right)
    );
}

constexpr bool has_capability(
    const ProviderCapability value,
    const ProviderCapability capability
) noexcept {
    return (
        static_cast<std::uint32_t>(value) &
        static_cast<std::uint32_t>(capability)
    ) != 0U;
}

struct TransferProgress {
    std::uint64_t bytes_done{0};
    std::uint64_t bytes_total{0};
    std::uint64_t files_done{0};
    std::uint64_t files_total{0};
    std::string current_path;
};

struct SpaceInfo {
    std::uint64_t capacity{0};
    std::uint64_t free{0};
    std::uint64_t available{0};
};

using ProgressCallback = std::function<void(const TransferProgress&)>;

class Provider {
public:
    virtual ~Provider() = default;

    [[nodiscard]] virtual std::string id() const = 0;
    [[nodiscard]] virtual std::string display_name() const = 0;
    [[nodiscard]] virtual ProviderCapability capabilities() const = 0;

    [[nodiscard]] virtual std::string normalize(
        std::string_view path
    ) const = 0;
    [[nodiscard]] virtual std::string join(
        std::string_view directory,
        std::string_view name
    ) const = 0;
    [[nodiscard]] virtual std::string parent(
        std::string_view path
    ) const = 0;
    [[nodiscard]] virtual std::string basename(
        std::string_view path
    ) const = 0;

    [[nodiscard]] virtual std::vector<Entry> list(
        std::string_view path,
        const ListRequest& request,
        const Cancellation& cancellation
    ) const = 0;
    [[nodiscard]] virtual Entry stat(
        std::string_view path,
        const Cancellation& cancellation
    ) const = 0;
    [[nodiscard]] virtual bool exists(std::string_view path) const = 0;
    [[nodiscard]] virtual SpaceInfo space(
        std::string_view path
    ) const = 0;

    virtual void read(
        std::string_view path,
        std::uint64_t offset,
        std::uint64_t length,
        std::ostream& output,
        const Cancellation& cancellation
    ) const = 0;
    virtual void write(
        std::string_view path,
        std::istream& input,
        std::optional<std::uint64_t> length,
        bool overwrite,
        std::optional<std::string> expected_revision,
        const Cancellation& cancellation,
        const ProgressCallback& progress
    ) = 0;

    virtual void create_directory(std::string_view path) = 0;
    virtual void remove(
        std::string_view path,
        bool recursive,
        const Cancellation& cancellation,
        const ProgressCallback& progress
    ) = 0;
    virtual void rename(
        std::string_view source,
        std::string_view destination,
        bool overwrite
    ) = 0;
    virtual void copy_direct(
        std::string_view source,
        std::string_view destination,
        bool overwrite,
        const Cancellation& cancellation,
        const ProgressCallback& progress
    ) = 0;
    virtual void set_permissions(
        std::string_view path,
        std::filesystem::perms permissions
    ) = 0;
    virtual void set_owner(
        std::string_view path,
        std::optional<std::string> owner,
        std::optional<std::string> group
    ) = 0;
    virtual void create_symlink(
        std::string_view target,
        std::string_view path
    ) = 0;
    virtual void create_hardlink(
        std::string_view target,
        std::string_view path
    ) = 0;
};

using ProviderPtr = std::shared_ptr<Provider>;

}  // namespace mcpp
