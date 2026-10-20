#pragma once

#include "mcpp/extfs_provider.hpp"

namespace mcpp::extfs {

[[noreturn]] void fail(const std::string& message);
void validate_limits(const ExtfsLimits& limits);
void validate_helper(const ExtfsHelper& helper);
[[nodiscard]] std::string identity_part(std::string_view value);

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
    const ExtfsLimits& limits, const Cancellation& cancellation
);

}  // namespace mcpp::extfs
