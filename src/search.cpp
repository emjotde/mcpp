#include "mcpp/search.hpp"
#include "mcpp/editor_search.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>

namespace mcpp {
namespace {

bool wildcard_match(
    const std::string_view pattern,
    const std::string_view value,
    const bool case_sensitive
) {
    return EditorSearch::filename_matches(value, pattern, case_sensitive);
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
    const auto& needle = options.content;
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
        if (EditorSearch::find(
                chunk, needle, 0, 0, chunk.size(),
                {.mode = EditorSearchMode::normal, .case_sensitive = options.case_sensitive},
                false, false
            )) {
            return true;
        }
        const auto keep = std::min(
            needle.size() > chunk.size() / 4U ? chunk.size() : needle.size() * 4U + 4U,
            chunk.size()
        );
        auto begin = chunk.size() - keep;
        while (begin > 0U &&
               (static_cast<unsigned char>(chunk[begin]) & 0xC0U) == 0x80U) {
            --begin;
        }
        overlap = chunk.substr(begin);
    }
    return false;
}

}  // namespace

std::vector<Entry> SearchEngine::search(
    Provider& provider,
    const std::string_view root,
    const SearchOptions& options,
    const Cancellation& cancellation,
    const ProgressCallback& progress,
    const SearchErrorCallback& error
) const {
    static_cast<void>(EditorSearch::filename_matches("", options.name_pattern, options.case_sensitive));
    std::vector<Entry> result;
    TransferProgress state;
    const auto normalized_root = provider.normalize(root);
    std::vector<std::string> pending{normalized_root};
    std::set<std::string, std::less<>> visited;
    const auto root_entry = provider.stat(
        normalized_root,
        cancellation
    );
    visited.insert(
        root_entry.revision.empty()
            ? normalized_root
            : root_entry.revision
    );
    while (!pending.empty()) {
        cancellation.throw_if_requested();
        auto path = std::move(pending.back());
        pending.pop_back();
        std::vector<Entry> entries;
        try {
            entries = provider.list(path, {.show_hidden = options.include_hidden}, cancellation);
        } catch (const OperationCancelled&) {
            throw;
        } catch (const std::exception& failure) {
            if (!error) {
                throw;
            }
            error(path, failure.what());
            continue;
        }
        std::vector<std::string> children;
        for (const auto& entry : entries) {
            cancellation.throw_if_requested();
            ++state.files_done;
            state.current_path = entry.path;
            if (progress) {
                progress(state);
            }
            bool matches_content = options.content.empty();
            if (!matches_content && entry.kind != EntryKind::directory) {
                try {
                    const auto source = entry.kind == EntryKind::symlink
                                            ? provider.stat_follow(entry.path, cancellation)
                                            : entry;
                    matches_content = source.kind == EntryKind::file &&
                                      content_matches(provider, source, options, cancellation);
                } catch (const OperationCancelled&) {
                    throw;
                } catch (const std::exception& failure) {
                    if (!error) {
                        throw;
                    }
                    error(entry.path, failure.what());
                }
            }
            if (wildcard_match(
                    options.name_pattern,
                    entry.name,
                    options.case_sensitive
                ) &&
                matches_content) {
                result.push_back(entry);
            }
            if (options.recursive &&
                entry.kind == EntryKind::directory) {
                const auto identity =
                    entry.revision.empty()
                        ? provider.normalize(entry.path)
                        : entry.revision;
                if (visited.insert(identity).second) {
                    children.push_back(entry.path);
                }
            }
        }
        for (auto child = children.rbegin();
             child != children.rend();
             ++child) {
            pending.push_back(std::move(*child));
        }
    }
    return result;
}

}  // namespace mcpp
