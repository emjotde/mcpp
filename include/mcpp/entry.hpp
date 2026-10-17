#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>

namespace mcpp {

enum class EntryKind {
    file,
    directory,
    symlink,
    other,
};

struct Entry {
    std::string name;
    std::string path;
    EntryKind kind{EntryKind::other};
    std::uint64_t size{0};
    std::chrono::system_clock::time_point modified{};
    std::filesystem::perms permissions{std::filesystem::perms::unknown};
    std::string owner;
    std::string group;
    std::string target;
    std::string revision;
    bool hidden{false};
};

enum class SortField {
    name,
    extension,
    size,
    modified,
};

struct ListRequest {
    bool show_hidden{false};
    SortField sort{SortField::name};
    bool descending{false};
    std::size_t offset{0};
    std::size_t limit{0};
};

}  // namespace mcpp
