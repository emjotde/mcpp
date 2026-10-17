#pragma once

#include "mcpp/provider.hpp"

#include <filesystem>

namespace mcpp {

class LocalProvider final : public Provider {
public:
    explicit LocalProvider(
        std::filesystem::path root = std::filesystem::path("/")
    );

    [[nodiscard]] std::string id() const override;
    [[nodiscard]] std::string display_name() const override;
    [[nodiscard]] ProviderCapability capabilities() const override;
    [[nodiscard]] std::string normalize(std::string_view path) const override;
    [[nodiscard]] std::string join(
        std::string_view directory,
        std::string_view name
    ) const override;
    [[nodiscard]] std::string parent(std::string_view path) const override;
    [[nodiscard]] std::string basename(std::string_view path) const override;
    [[nodiscard]] std::vector<Entry> list(
        std::string_view path,
        const ListRequest& request,
        const Cancellation& cancellation
    ) const override;
    [[nodiscard]] Entry stat(
        std::string_view path,
        const Cancellation& cancellation
    ) const override;
    [[nodiscard]] bool exists(std::string_view path) const override;
    [[nodiscard]] SpaceInfo space(
        std::string_view path
    ) const override;
    void read(
        std::string_view path,
        std::uint64_t offset,
        std::uint64_t length,
        std::ostream& output,
        const Cancellation& cancellation
    ) const override;
    void write(
        std::string_view path,
        std::istream& input,
        std::optional<std::uint64_t> length,
        bool overwrite,
        std::optional<std::string> expected_revision,
        const Cancellation& cancellation,
        const ProgressCallback& progress
    ) override;
    void create_directory(std::string_view path) override;
    void remove(
        std::string_view path,
        bool recursive,
        const Cancellation& cancellation,
        const ProgressCallback& progress
    ) override;
    void rename(
        std::string_view source,
        std::string_view destination,
        bool overwrite
    ) override;
    void copy_direct(
        std::string_view source,
        std::string_view destination,
        bool overwrite,
        const Cancellation& cancellation,
        const ProgressCallback& progress
    ) override;
    void set_permissions(
        std::string_view path,
        std::filesystem::perms permissions
    ) override;
    void set_owner(
        std::string_view path,
        std::optional<std::string> owner,
        std::optional<std::string> group
    ) override;
    void create_symlink(
        std::string_view target,
        std::string_view path
    ) override;
    void create_hardlink(
        std::string_view target,
        std::string_view path
    ) override;

private:
    [[nodiscard]] std::filesystem::path resolve(
        std::string_view path
    ) const;
    [[nodiscard]] std::filesystem::path resolve_follow(
        std::string_view path
    ) const;
    [[nodiscard]] Entry make_entry(
        const std::filesystem::directory_entry& entry
    ) const;

    std::filesystem::path root_;
};

}  // namespace mcpp
