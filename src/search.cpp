#include "mcpp/search.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace mcpp {
namespace {

std::string lowercase(std::string value) {
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](const unsigned char character) {
            return static_cast<char>(std::tolower(character));
        }
    );
    return value;
}

bool wildcard_match(
    const std::string_view pattern,
    const std::string_view value,
    const bool case_sensitive
) {
    std::size_t pattern_index = 0;
    std::size_t value_index = 0;
    std::size_t star = std::string_view::npos;
    std::size_t retry = 0;
    const auto equal = [&](const char left, const char right) {
        if (case_sensitive) {
            return left == right;
        }
        return std::tolower(static_cast<unsigned char>(left)) ==
               std::tolower(static_cast<unsigned char>(right));
    };
    while (value_index < value.size()) {
        if (pattern_index < pattern.size() &&
            (pattern[pattern_index] == '?' ||
             equal(pattern[pattern_index], value[value_index]))) {
            ++pattern_index;
            ++value_index;
        } else if (pattern_index < pattern.size() &&
                   pattern[pattern_index] == '*') {
            star = pattern_index++;
            retry = value_index;
        } else if (star != std::string_view::npos) {
            pattern_index = star + 1U;
            value_index = ++retry;
        } else {
            return false;
        }
    }
    while (pattern_index < pattern.size() &&
           pattern[pattern_index] == '*') {
        ++pattern_index;
    }
    return pattern_index == pattern.size();
}

bool content_matches(
    Provider& provider,
    const Entry& entry,
    const SearchOptions& options,
    const Cancellation& cancellation
) {
    if (options.content.empty()) {
        return true;
    }
    constexpr std::uint64_t chunk_size = 256U * 1024U;
    const auto needle =
        options.case_sensitive
            ? options.content
            : lowercase(options.content);
    std::string overlap;
    for (std::uint64_t offset = 0; offset < entry.size;
         offset += chunk_size) {
        cancellation.throw_if_requested();
        std::ostringstream output;
        provider.read(
            entry.path,
            offset,
            std::min(chunk_size, entry.size - offset),
            output,
            cancellation
        );
        auto chunk = overlap + std::move(output).str();
        const auto searchable =
            options.case_sensitive ? chunk : lowercase(std::move(chunk));
        if (searchable.find(needle) != std::string::npos) {
            return true;
        }
        const auto keep = std::min(
            needle.empty() ? 0U : needle.size() - 1U,
            searchable.size()
        );
        overlap = searchable.substr(searchable.size() - keep);
    }
    return false;
}

void search_directory(
    Provider& provider,
    const std::string& path,
    const SearchOptions& options,
    const Cancellation& cancellation,
    const ProgressCallback& progress,
    TransferProgress& state,
    std::vector<Entry>& result
) {
    for (const auto& entry :
         provider.list(
             path,
             {.show_hidden = options.include_hidden},
             cancellation
         )) {
        cancellation.throw_if_requested();
        ++state.files_done;
        state.current_path = entry.path;
        if (progress) {
            progress(state);
        }
        if (wildcard_match(
                options.name_pattern,
                entry.name,
                options.case_sensitive
            ) &&
            (entry.kind != EntryKind::file ||
             content_matches(
                 provider,
                 entry,
                 options,
                 cancellation
             ))) {
            result.push_back(entry);
        }
        if (options.recursive &&
            entry.kind == EntryKind::directory) {
            search_directory(
                provider,
                entry.path,
                options,
                cancellation,
                progress,
                state,
                result
            );
        }
    }
}

}  // namespace

std::vector<Entry> SearchEngine::search(
    Provider& provider,
    const std::string_view root,
    const SearchOptions& options,
    const Cancellation& cancellation,
    const ProgressCallback& progress
) const {
    std::vector<Entry> result;
    TransferProgress state;
    search_directory(
        provider,
        provider.normalize(root),
        options,
        cancellation,
        progress,
        state,
        result
    );
    return result;
}

}  // namespace mcpp
