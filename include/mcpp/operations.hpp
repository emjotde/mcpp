#pragma once

#include "mcpp/provider.hpp"

#include <functional>
#include <string>
#include <vector>

namespace mcpp {

enum class CollisionDecision {
    overwrite,
    skip,
    cancel,
};

using CollisionResolver = std::function<CollisionDecision(
    const Entry& source,
    const Entry& destination
)>;

struct OperationOptions {
    bool overwrite{false};
    bool preserve_permissions{true};
    bool preserve_symlinks{true};
    CollisionResolver collision;
};

struct SkippedSource {
    std::string path;
    std::string reason;
};

struct OperationResult {
    // Completed sources remain available if a later source fails or cancels.
    std::vector<std::string> completed_sources;
    std::vector<SkippedSource> skipped;
};

class OperationEngine {
public:
    void copy(
        Provider& source_provider,
        const std::vector<Entry>& sources,
        Provider& destination_provider,
        std::string_view destination_directory,
        const OperationOptions& options,
        const Cancellation& cancellation,
        const ProgressCallback& progress = {},
        OperationResult* result = nullptr
    ) const;

    void copy_exact(
        Provider& source_provider,
        const Entry& source,
        Provider& destination_provider,
        std::string_view destination,
        const OperationOptions& options,
        const Cancellation& cancellation,
        const ProgressCallback& progress = {},
        OperationResult* result = nullptr
    ) const;

    void move(
        Provider& source_provider,
        const std::vector<Entry>& sources,
        Provider& destination_provider,
        std::string_view destination_directory,
        const OperationOptions& options,
        const Cancellation& cancellation,
        const ProgressCallback& progress = {},
        OperationResult* result = nullptr
    ) const;

    void move_exact(
        Provider& source_provider,
        const Entry& source,
        Provider& destination_provider,
        std::string_view destination,
        const OperationOptions& options,
        const Cancellation& cancellation,
        const ProgressCallback& progress = {},
        OperationResult* result = nullptr
    ) const;

    void remove(
        Provider& provider,
        const std::vector<Entry>& entries,
        const Cancellation& cancellation,
        const ProgressCallback& progress = {},
        OperationResult* result = nullptr
    ) const;

private:
    static TransferProgress measure(
        Provider& provider,
        const std::vector<Entry>& entries,
        const Cancellation& cancellation
    );

    static bool copy_entry(
        Provider& source_provider,
        const Entry& source,
        Provider& destination_provider,
        std::string_view destination,
        const OperationOptions& options,
        const Cancellation& cancellation,
        TransferProgress& state,
        const ProgressCallback& progress,
        std::string& skip_reason
    );
};

}  // namespace mcpp
