#include "mcpp/compare.hpp"

#include <map>

namespace mcpp {
namespace {

using Inventory = std::map<std::string, Entry, std::less<>>;

void inventory(
    Provider& provider,
    const std::string& directory,
    const std::string& relative_directory,
    const bool recursive,
    const Cancellation& cancellation,
    Inventory& result
) {
    for (const auto& entry :
         provider.list(
             directory,
             {.show_hidden = true},
             cancellation
         )) {
        cancellation.throw_if_requested();
        const auto relative =
            relative_directory.empty()
                ? entry.name
                : relative_directory + "/" + entry.name;
        result.emplace(relative, entry);
        if (recursive &&
            entry.kind == EntryKind::directory) {
            inventory(
                provider,
                entry.path,
                relative,
                true,
                cancellation,
                result
            );
        }
    }
}

}  // namespace

std::vector<Difference> DirectoryComparator::compare(
    Provider& left_provider,
    const std::string_view left_root,
    Provider& right_provider,
    const std::string_view right_root,
    const bool recursive,
    const Cancellation& cancellation
) const {
    const auto normalized_left = left_provider.normalize(left_root);
    const auto normalized_right = right_provider.normalize(right_root);
    Inventory left;
    Inventory right;
    inventory(
        left_provider,
        normalized_left,
        {},
        recursive,
        cancellation,
        left
    );
    inventory(
        right_provider,
        normalized_right,
        {},
        recursive,
        cancellation,
        right
    );

    std::vector<Difference> differences;
    for (const auto& [path, left_entry] : left) {
        const auto match = right.find(path);
        if (match == right.end()) {
            differences.push_back({path, DifferenceKind::left_only});
            continue;
        }
        const auto& right_entry = match->second;
        if (left_entry.kind != right_entry.kind) {
            differences.push_back({path, DifferenceKind::type});
        } else if (left_entry.size != right_entry.size) {
            differences.push_back({path, DifferenceKind::size});
        } else if (left_entry.modified != right_entry.modified) {
            differences.push_back({path, DifferenceKind::modified});
        }
    }
    for (const auto& [path, entry] : right) {
        static_cast<void>(entry);
        if (!left.contains(path)) {
            differences.push_back({path, DifferenceKind::right_only});
        }
    }
    return differences;
}

}  // namespace mcpp
