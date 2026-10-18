#pragma once

#include "mcpp/provider.hpp"

#include <string>
#include <vector>

namespace mcpp {

using SearchErrorCallback = std::function<void(const std::string&, const std::string&)>;

struct SearchOptions {
    std::string name_pattern{"*"};
    std::string content;
    bool case_sensitive{false};
    bool recursive{true};
    bool include_hidden{false};
};

class SearchEngine {
public:
    [[nodiscard]] std::vector<Entry> search(
        Provider& provider,
        std::string_view root,
        const SearchOptions& options,
        const Cancellation& cancellation,
        const ProgressCallback& progress = {},
        const SearchErrorCallback& error = {}
    ) const;
};

}  // namespace mcpp
