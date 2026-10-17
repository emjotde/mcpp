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

class OperationEngine {
public:
    void copy(
        Provider& source_provider,
        const std::vector<Entry>& sources,
        Provider& destination_provider,
        std::string_view destination_directory,
        const OperationOptions& options,
        const Cancellation& cancellation,
        const ProgressCallback& progress = {}
    ) const;

    void move(
        Provider& source_provider,
        const std::vector<Entry>& sources,
        Provider& destination_provider,
        std::string_view destination_directory,
        const OperationOptions& options,
        const Cancellation& cancellation,
        const ProgressCallback& progress = {}
    ) const;

    void remove(
        Provider& provider,
        const std::vector<Entry>& entries,
        const Cancellation& cancellation,
        const ProgressCallback& progress = {}
    ) const;

private:
    static TransferProgress measure(
        Provider& provider,
        const std::vector<Entry>& entries,
        const Cancellation& cancellation
    );

    static void copy_entry(
        Provider& source_provider,
        const Entry& source,
        Provider& destination_provider,
        std::string_view destination,
        const OperationOptions& options,
        const Cancellation& cancellation,
        TransferProgress& state,
        const ProgressCallback& progress
    );
};

}  // namespace mcpp
