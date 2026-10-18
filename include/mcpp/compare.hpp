#pragma once

#include "mcpp/provider.hpp"

#include <string>
#include <vector>

namespace mcpp {

enum class DifferenceKind {
    left_only,
    right_only,
    type,
    size,
    modified,
    content,
};

enum class CompareMode { quick, size, thorough };

struct Difference {
    std::string relative_path;
    DifferenceKind kind{DifferenceKind::size};
    bool mark_left{false};
    bool mark_right{false};
};

class DirectoryComparator {
public:
    [[nodiscard]] std::vector<Difference> compare(
        Provider& left_provider,
        std::string_view left_root,
        Provider& right_provider,
        std::string_view right_root,
        bool recursive,
        const Cancellation& cancellation,
        CompareMode mode = CompareMode::quick
    ) const;
};

}  // namespace mcpp
