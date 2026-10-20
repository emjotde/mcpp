#pragma once

#include "mcpp/local_tools.hpp"

#include <iosfwd>

namespace mcpp {

// Streams stdout without a materialized-output cap; stderr remains bounded.
[[nodiscard]] CommandOutput capture_command_to(
    std::ostream& output,
    const std::vector<std::string>& arguments,
    const std::filesystem::path& directory,
    const Cancellation& cancellation = {},
    std::size_t error_limit = 64U * 1024U,
    std::optional<std::filesystem::path> input = std::nullopt
);

[[nodiscard]] CommandOutput capture_shell_to(
    std::ostream& output,
    std::string_view command,
    const std::filesystem::path& directory,
    const Cancellation& cancellation = {},
    std::size_t error_limit = 64U * 1024U
);

}  // namespace mcpp
