#pragma once

#include "mcpp/extfs_provider.hpp"

namespace mcpp::extfs {

[[noreturn]] void fail(const std::string& message);
void validate_limits(const ExtfsLimits& limits);
void validate_helper(const ExtfsHelper& helper);
[[nodiscard]] std::string identity_part(std::string_view value);

class DirectoryContext {
public:
    explicit DirectoryContext(ExtfsDirectory directory, const Cancellation& cancellation);
    ~DirectoryContext();
    DirectoryContext(const DirectoryContext&) = delete;
    DirectoryContext& operator=(const DirectoryContext&) = delete;
    [[nodiscard]] const ExtfsDirectory& directory() const noexcept { return directory_; }
    [[nodiscard]] const std::filesystem::path& native_path() const noexcept { return native_; }
    [[nodiscard]] std::filesystem::path working_path() const;
    [[nodiscard]] std::string identity() const;
    void verify(const Cancellation& cancellation) const;
private:
    ExtfsDirectory directory_;
    std::filesystem::path native_;
    int descriptor_{-1};
    std::uintmax_t device_{0};
    std::uintmax_t inode_{0};
};

class Workspace {
public:
    explicit Workspace(const ExtfsOpenOptions& options);
    ~Workspace();
    Workspace(const Workspace&) = delete;
    Workspace& operator=(const Workspace&) = delete;
    [[nodiscard]] std::filesystem::path path() const;
    void put(std::string_view name, std::string_view bytes, const Cancellation& cancellation) const;
    [[nodiscard]] std::string get(
        std::string_view name, std::size_t limit, const Cancellation& cancellation
    ) const;
private:
    std::filesystem::path base_path_;
    std::string name_;
    int base_{-1};
    int directory_{-1};
};

[[nodiscard]] std::string invoke(
    const ExtfsHelper& helper, const Workspace& workspace,
    std::string_view command, const std::vector<std::string>& arguments,
    const ExtfsLimits& limits, const Cancellation& cancellation,
    const std::filesystem::path& working_directory = {}
);

}  // namespace mcpp::extfs
