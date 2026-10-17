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
};

struct Difference {
    std::string relative_path;
    DifferenceKind kind{DifferenceKind::size};
};

class DirectoryComparator {
public:
    [[nodiscard]] std::vector<Difference> compare(
        Provider& left_provider,
        std::string_view left_root,
        Provider& right_provider,
        std::string_view right_root,
        bool recursive,
        const Cancellation& cancellation
    ) const;
};

}  // namespace mcpp
