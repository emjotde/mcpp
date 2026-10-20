#pragma once

#include "mcpp/provider.hpp"

#include <filesystem>

namespace mcpp {

// A private read-only checkout operation, never a writable view of the source.
class ExtensionViewCheckout final {
public:
    ExtensionViewCheckout(
        ProviderPtr provider, const Entry& source, const Cancellation& cancellation
    );
    ~ExtensionViewCheckout();
    ExtensionViewCheckout(const ExtensionViewCheckout&) = delete;
    ExtensionViewCheckout& operator=(const ExtensionViewCheckout&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    [[nodiscard]] const std::filesystem::path& directory() const noexcept { return directory_; }
    void validate_source(const Cancellation& cancellation) const;

private:
    void cleanup() noexcept;

    ProviderPtr provider_;
    Entry source_;
    std::filesystem::path directory_;
    std::filesystem::path path_;
};

}  // namespace mcpp
