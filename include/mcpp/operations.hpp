#pragma once

#include "mcpp/provider.hpp"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mcpp {

enum class CollisionDecision {
    overwrite,
    append,
    reget,
    skip,
    cancel,
};

struct CollisionOptions {
    bool append{false};
    bool reget{false};
};

using CollisionResolver = std::function<CollisionDecision(
    const Entry& source,
    const Entry& destination
)>;

enum class OperationErrorDecision {
    retry,
    skip,
    cancel,
};

using OperationErrorResolver = std::function<OperationErrorDecision(
    const Entry& source,
    const std::string& message
)>;

enum class RecursiveDeleteDecision {
    yes,
    no,
    all,
    none,
    cancel,
};

using RecursiveDeleteResolver = std::function<RecursiveDeleteDecision(
    const Entry& directory
)>;

class CopyMask {
public:
    explicit CopyMask(std::string_view source = "*", bool shell_patterns = true);
    [[nodiscard]] bool matches(std::string_view name) const;
    [[nodiscard]] std::optional<std::string> transform(
        std::string_view name, std::string_view destination = "\\0"
    ) const;
    [[nodiscard]] static bool has_replacements(std::string_view destination);
    [[nodiscard]] static std::string unescape_path(std::string_view path);

private:
    struct Pattern;
    std::shared_ptr<const Pattern> pattern_;
};

struct OperationOptions {
    bool overwrite{false};
    bool preserve_permissions{true};
    bool preserve_symlinks{true};
    CollisionResolver collision;
    OperationErrorResolver error{};
    bool compute_totals{true};
    std::string source_mask{"*"};
    std::optional<std::string> destination_mask{};
    bool shell_patterns{true};
    bool stable_symlinks{false};
    bool dive_into_subdirectories{false};
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
    [[nodiscard]] static CollisionOptions collision_options(
        const Provider& source_provider,
        const Entry& source,
        const Provider& destination_provider,
        const Entry& destination
    );

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
        OperationResult* result = nullptr,
        const OperationErrorResolver& error = {},
        bool compute_totals = true,
        const RecursiveDeleteResolver& recursive_delete = {}
    ) const;

private:
    struct Collision {
        CollisionDecision decision{CollisionDecision::overwrite};
        std::optional<Entry> destination;
    };

    using LinkCache = std::map<std::string, std::pair<std::string, std::string>>;
    static Collision resolve_collision(
        Provider& source_provider,
        const Entry& source,
        Provider& destination_provider,
        std::string_view destination,
        const OperationOptions& options,
        const Cancellation& cancellation,
        std::optional<Collision> current = {}
    );
    static TransferProgress measure(
        Provider& provider,
        const std::vector<Entry>& entries,
        const Cancellation& cancellation,
        bool tolerate_errors = false
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
        std::string& skip_reason,
        std::optional<Collision> collision_current = {},
        std::vector<Entry> ancestors = {},
        std::shared_ptr<LinkCache> links = {},
        Entry* transferred_source = nullptr,
        CollisionDecision* transferred_decision = nullptr
    );
    static bool copy_entry_once(
        Provider& source_provider,
        const Entry& source,
        Provider& destination_provider,
        std::string_view destination,
        const OperationOptions& options,
        const Cancellation& cancellation,
        TransferProgress& state,
        const ProgressCallback& progress,
        std::string& skip_reason,
        std::optional<Collision> collision_current,
        std::vector<Entry> ancestors,
        const std::shared_ptr<LinkCache>& links,
        Entry* transferred_source,
        CollisionDecision* transferred_decision
    );
};

}  // namespace mcpp
