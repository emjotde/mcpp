#pragma once

#include <filesystem>
#include <set>
#include <string>

namespace mcpp {

[[nodiscard]] std::filesystem::path tree_cache_path();
[[nodiscard]] std::set<std::string> load_tree_cache(const std::filesystem::path& path);
void save_tree_cache(const std::filesystem::path& path, const std::set<std::string>& directories);

}  // namespace mcpp
