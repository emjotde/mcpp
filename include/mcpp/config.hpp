#pragma once

#include "mcpp/entry.hpp"

#include <filesystem>
#include <string>

namespace mcpp {

struct Config {
    bool show_hidden{false};
    bool confirm_delete{true};
    bool confirm_overwrite{true};
    bool directories_first{true};
    SortField sort{SortField::name};
    std::string theme{"classic"};
    std::filesystem::path left_path;
    std::filesystem::path right_path;

    static Config load(const std::filesystem::path& path);
    void save(const std::filesystem::path& path) const;
};

[[nodiscard]] std::filesystem::path default_config_path();

}  // namespace mcpp
