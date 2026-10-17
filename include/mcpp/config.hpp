#pragma once

#include "mcpp/entry.hpp"

#include <cstddef>
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
    bool editor_syntax{true};
    bool editor_line_numbers{false};
    std::size_t editor_tab_width{8};
    std::size_t editor_indent_width{4};
    bool editor_auto_indent{true};
    bool editor_show_whitespace{false};
    std::vector<std::string> bookmarks;
    std::vector<std::string> editor_history;

    static Config load(const std::filesystem::path& path);
    void save(const std::filesystem::path& path) const;
};

[[nodiscard]] std::filesystem::path default_config_path();

}  // namespace mcpp
