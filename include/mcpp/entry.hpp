#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

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
    bool target_is_directory{false};
    std::chrono::system_clock::time_point accessed{};
    std::chrono::system_clock::time_point changed{};
    std::uint64_t inode{0};
    std::uint64_t links{1};
    std::uint32_t uid{0};
    std::uint32_t gid{0};
    std::uint32_t mode{0};
};

enum class SortField {
    name,
    extension,
    size,
    modified,
    unsorted,
    version,
    accessed,
    changed,
    inode,
};

enum class ListingFormat { full, brief, long_list, custom };

struct ListRequest {
    bool show_hidden{false};
    bool directories_first{true};
    SortField sort{SortField::name};
    bool descending{false};
    bool case_sensitive{true};
    bool executables_first{false};
    bool reverse_files_only{true};
    std::size_t offset{0};
    std::size_t limit{0};
};

void sort_entries(std::vector<Entry>& entries, const ListRequest& request);

}  // namespace mcpp
