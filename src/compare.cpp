#include "mcpp/compare.hpp"

#include <algorithm>
#include <map>
#include <sstream>

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

bool same_contents(
    Provider& first_provider, const Entry& first,
    Provider& second_provider, const Entry& second,
    const Cancellation& cancellation
) {
    const auto first_file = first_provider.stat_follow(first.path, cancellation);
    const auto second_file = second_provider.stat_follow(second.path, cancellation);
    if (first_file.kind != EntryKind::file || second_file.kind != EntryKind::file) {
        return first.kind == second.kind && first.target == second.target;
    }
    if (first_file.size != second_file.size) {
        return false;
    }
    constexpr std::uint64_t chunk_size = 64U * 1024U;
    for (std::uint64_t offset = 0; offset < first_file.size; offset += chunk_size) {
        cancellation.throw_if_requested();
        const auto count = std::min(chunk_size, first_file.size - offset);
        std::ostringstream first_bytes;
        std::ostringstream second_bytes;
        first_provider.read(first_file.path, offset, count, first_bytes, cancellation);
        second_provider.read(second_file.path, offset, count, second_bytes, cancellation);
        const auto first_value = std::move(first_bytes).str();
        const auto second_value = std::move(second_bytes).str();
        if (first_value.size() != count || second_value.size() != count) {
            throw std::runtime_error("file changed during directory comparison");
        }
        if (first_value != second_value) {
            return false;
        }
    }
    return true;
}

}  // namespace

std::vector<Difference> DirectoryComparator::compare(
    Provider& left_provider,
    const std::string_view left_root,
    Provider& right_provider,
    const std::string_view right_root,
    const bool recursive,
    const Cancellation& cancellation,
    const CompareMode mode
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
            if (left_entry.kind != EntryKind::directory) {
                differences.push_back({path, DifferenceKind::left_only, true, false});
            }
            continue;
        }
        const auto& right_entry = match->second;
        const bool left_file = left_entry.kind != EntryKind::directory;
        const bool right_file = right_entry.kind != EntryKind::directory;
        if (!left_file && !right_file) {
            continue;
        }
        const bool different_size = left_entry.size != right_entry.size;
        const bool different_type = left_entry.kind != right_entry.kind;
        const bool different_content =
            mode == CompareMode::thorough &&
            (different_type ||
             !same_contents(left_provider, left_entry, right_provider, right_entry, cancellation));
        const auto mark = [&](const Entry& source, const Entry& target, const bool file) {
            if (!file || (mode != CompareMode::size && source.modified < target.modified)) {
                return false;
            }
            return different_size || different_type ||
                   (mode == CompareMode::quick && source.modified > target.modified) ||
                   different_content;
        };
        const bool mark_left = mark(left_entry, right_entry, left_file);
        const bool mark_right = mark(right_entry, left_entry, right_file);
        if (mark_left || mark_right) {
            const auto kind = different_type ? DifferenceKind::type
                              : different_size ? DifferenceKind::size
                              : different_content ? DifferenceKind::content
                              : DifferenceKind::modified;
            differences.push_back({path, kind, mark_left, mark_right});
        }
    }
    for (const auto& [path, entry] : right) {
        static_cast<void>(entry);
        if (!left.contains(path) && entry.kind != EntryKind::directory) {
            differences.push_back({path, DifferenceKind::right_only, false, true});
        }
    }
    return differences;
}

}  // namespace mcpp
