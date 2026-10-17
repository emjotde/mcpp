#include "mcpp/location.hpp"

#include <stdexcept>

namespace mcpp {

Location open_location(
    const std::string_view value,
    const ProviderPtr& local_provider
) {
    if (!local_provider) {
        throw std::invalid_argument("local provider is null");
    }
    if (value.find("://") != std::string_view::npos) {
        throw std::invalid_argument(
            "remote providers are not enabled yet: " +
            std::string(value)
        );
    }
    return {
        .provider = local_provider,
        .path = local_provider->normalize(value),
    };
}

}  // namespace mcpp
