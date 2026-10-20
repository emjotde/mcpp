#pragma once

#include "mcpp/cancellation.hpp"
#include "mcpp/entry.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace mcpp {

enum class ExtensionAction { open, view, edit };
enum class ExtensionType { description, mime };

struct ExtensionCommand {
    std::string command;
    std::string source;
    std::string section;
    std::size_t line{0};
    bool legacy{false};
};

class ExtensionRules {
public:
    [[nodiscard]] static ExtensionRules parse(
        std::string_view content, std::string_view source = {}
    );
    [[nodiscard]] static ExtensionRules load(const std::filesystem::path& path);
    [[nodiscard]] std::optional<ExtensionCommand> match(
        const Entry& entry, ExtensionAction action, bool local,
        const std::function<std::string(ExtensionType)>& probe = {},
        bool use_file_to_guess_type = true
    ) const;

private:
    struct Data;
    std::shared_ptr<const Data> data_;
};

[[nodiscard]] std::filesystem::path find_extension_file(
    const std::filesystem::path& config_directory
);
[[nodiscard]] std::string probe_extension_type(
    const std::filesystem::path& path, ExtensionType type,
    const Cancellation& cancellation = {}
);

}  // namespace mcpp
