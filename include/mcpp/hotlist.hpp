#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mcpp {

enum class HotlistKind { entry, group, comment };

struct HotlistItem {
    HotlistKind kind{HotlistKind::entry};
    std::string label;
    std::string directory;
    std::vector<HotlistItem> children;

    bool operator==(const HotlistItem&) const = default;
};

using HotlistPath = std::vector<std::size_t>;

class DirectoryHotlist {
public:
    [[nodiscard]] static DirectoryHotlist parse(std::string_view content);
    [[nodiscard]] static DirectoryHotlist load(
        const std::filesystem::path& path, const std::vector<std::string>& legacy_bookmarks = {}
    );
    [[nodiscard]] std::string serialize() const;
    void save(const std::filesystem::path& path);

    [[nodiscard]] const std::vector<HotlistItem>& group(const HotlistPath& path) const;
    [[nodiscard]] std::vector<HotlistItem>& group(const HotlistPath& path);
    [[nodiscard]] std::vector<std::string> bookmarks() const;
    std::size_t insert(
        const HotlistPath& group, HotlistItem item, std::optional<std::size_t> before = {}
    );
    void erase(const HotlistPath& item);
    HotlistPath move(
        const HotlistPath& item, HotlistPath destination, std::optional<std::size_t> before = {}
    );

private:
    std::vector<HotlistItem> items_;
    bool legacy_imported_{false};
    std::optional<std::filesystem::path> source_;
    std::optional<std::string> saved_content_;
};

}  // namespace mcpp
