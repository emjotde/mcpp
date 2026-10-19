#include "mcpp/entry.hpp"
#include "mcpp/editor_search.hpp"

#include <algorithm>
#include <cstring>

namespace mcpp {

void sort_entries(std::vector<Entry>& entries, const ListRequest& request) {
    const auto directory = [](const Entry& entry) {
        return entry.kind == EntryKind::directory || entry.target_is_directory;
    };
    const auto executable = [](const Entry& entry) {
        return entry.kind == EntryKind::file &&
               entry.permissions != std::filesystem::perms::unknown &&
               (entry.permissions & (std::filesystem::perms::owner_exec |
                                     std::filesystem::perms::group_exec |
                                     std::filesystem::perms::others_exec)) != std::filesystem::perms::none;
    };
    const auto name = [&](const std::string& value) {
        return request.case_sensitive ? value : EditorSearch::fold_case(value);
    };
    const auto extension = [&](const std::string& value) {
        return name(std::filesystem::path(value).extension().string());
    };
    const auto order = [](const auto& first, const auto& second) {
        return first < second ? -1 : first > second ? 1 : 0;
    };
    std::stable_sort(entries.begin(), entries.end(), [&](const Entry& first, const Entry& second) {
        if ((first.revision == "mcpp:parent") != (second.revision == "mcpp:parent")) {
            return first.revision == "mcpp:parent";
        }
        if (request.directories_first && directory(first) != directory(second)) return directory(first);
        if (request.executables_first && executable(first) != executable(second)) return executable(first);
        int comparison = 0;
        switch (request.sort) {
            case SortField::unsorted: return false;
            case SortField::name: comparison = name(first.name).compare(name(second.name)); break;
            case SortField::version:
                comparison = strverscmp(name(first.name).c_str(), name(second.name).c_str());
                break;
            case SortField::extension: comparison = extension(first.name).compare(extension(second.name)); break;
            case SortField::size: comparison = order(first.size, second.size); break;
            case SortField::modified: comparison = order(first.modified, second.modified); break;
            case SortField::accessed: comparison = order(first.accessed, second.accessed); break;
            case SortField::changed: comparison = order(first.changed, second.changed); break;
            case SortField::inode: comparison = order(first.inode, second.inode); break;
        }
        if (comparison == 0) comparison = name(first.name).compare(name(second.name));
        const bool reverse = request.descending &&
                             !(request.directories_first && request.reverse_files_only &&
                               directory(first) && directory(second));
        return reverse ? comparison > 0 : comparison < 0;
    });
}

}  // namespace mcpp
