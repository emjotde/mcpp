#pragma once

#include "mcpp/entry.hpp"
#include "mcpp/skin.hpp"

#include <filesystem>
#include <memory>
#include <optional>

namespace mcpp {

namespace detail {
struct FileHighlightData;
}

class FileHighlightRules {
public:
    [[nodiscard]] static FileHighlightRules load(
        const std::filesystem::path& config_directory,
        const std::filesystem::path& previous_resource = {}
    );
    [[nodiscard]] static FileHighlightRules load_file(
        const std::filesystem::path& path, bool legacy = false
    );
    [[nodiscard]] std::optional<SkinPair> color_for(
        const Entry& entry, const Skin& skin, bool local_entry = false
    ) const;
    [[nodiscard]] std::filesystem::path resource() const;

private:
    std::shared_ptr<const detail::FileHighlightData> data_;
};

[[nodiscard]] std::filesystem::path find_file_highlight_file(
    const std::filesystem::path& config_directory
);

}  // namespace mcpp
