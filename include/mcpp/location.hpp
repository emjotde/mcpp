#pragma once

#include "mcpp/provider.hpp"

#include <string>
#include <string_view>

namespace mcpp {

struct Location {
    ProviderPtr provider;
    std::string path;
};

[[nodiscard]] Location open_location(
    std::string_view value,
    const ProviderPtr& local_provider
);

}  // namespace mcpp
