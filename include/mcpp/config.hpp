#pragma once

#include "mcpp/entry.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace mcpp {

struct Config {
    bool show_hidden{true};
    bool confirm_delete{true};
    bool confirm_overwrite{true};
    bool directories_first{true};
    SortField sort{SortField::name};
    bool sort_descending{false};
    std::string theme{"grayscale"};
    std::vector<std::string> bookmarks;

    static Config load(const std::filesystem::path& path);
    void save(const std::filesystem::path& path) const;
};

[[nodiscard]] std::filesystem::path default_config_path();

}  // namespace mcpp
