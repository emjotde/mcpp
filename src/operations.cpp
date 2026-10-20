#include "mcpp/operations.hpp"
#include "mcpp/archive_provider.hpp"
#include "mcpp/local_provider.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <fnmatch.h>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <unistd.h>
#include <utility>

namespace mcpp {
namespace {

std::size_t mask_character_length(const std::string_view value) {
    if (value.empty()) return 0U;
    std::mbstate_t state{};
    const auto count = std::mbrlen(value.data(), value.size(), &state);
    return count == 0U || count == static_cast<std::size_t>(-1) ||
           count == static_cast<std::size_t>(-2) ? 1U : count;
}

std::size_t shell_class_end(const std::string_view pattern, const std::size_t begin) {
    auto index = begin + 1U;
    if (index < pattern.size() && (pattern[index] == '!' || pattern[index] == '^')) ++index;
    if (index < pattern.size() && pattern[index] == ']') ++index;
    for (; index < pattern.size(); ++index) {
        if (pattern[index] == '\\' && index + 1U < pattern.size()) {
            ++index;
        } else if (pattern[index] == '[' && index + 1U < pattern.size() &&
                   std::string_view(":.=").find(pattern[index + 1U]) != std::string_view::npos) {
            const auto end = pattern.find(std::string{pattern[index + 1U], ']'}, index + 2U);
            if (end != std::string_view::npos) index = end + 1U;
        } else if (pattern[index] == ']') {
            return index + 1U;
        }
    }
    return begin + 1U;
}

std::vector<std::string> shell_mask_captures(
    const std::string& pattern, const std::string& name
) {
    std::vector<std::string> captures{name};
    std::vector<std::size_t> boundaries{0U};
    while (boundaries.back() < name.size()) {
        boundaries.push_back(boundaries.back() +
            mask_character_length(std::string_view(name).substr(boundaries.back())));
    }
    std::size_t position = 0U;
    for (std::size_t index = 0U; index < pattern.size();) {
        const auto character = pattern[index++];
        if (character == '*') {
            auto boundary = boundaries.end() - 1;
            while (*boundary > position &&
                   fnmatch(pattern.c_str() + index, name.c_str() + *boundary, 0) != 0) {
                --boundary;
            }
            const auto end = *boundary;
            captures.push_back(name.substr(position, end - position));
            position = end;
        } else {
            const auto length = mask_character_length(std::string_view(name).substr(position));
            if (character == '?') {
                captures.push_back(name.substr(position, length));
            } else if (character == '[') {
                index = shell_class_end(pattern, index - 1U);
            } else {
                if (character == '\\' && index < pattern.size()) ++index;
                while (index < pattern.size() &&
                       (static_cast<unsigned char>(pattern[index]) & 0xc0U) == 0x80U) ++index;
            }
            position += length;
        }
    }
    return captures;
}

void append_mask_text(
    std::string& result, const std::string_view value, const char conversion,
    char& next_conversion
) {
    if (conversion == '\0' && next_conversion == '\0') {
        result += value;
        return;
    }
    std::mbstate_t input_state{};
    for (std::size_t offset = 0; offset < value.size();) {
        wchar_t character{};
        auto count = std::mbrtowc(&character, value.data() + offset, value.size() - offset, &input_state);
        const auto mode = next_conversion != '\0' ? next_conversion : conversion;
        next_conversion = '\0';
        if (count == static_cast<std::size_t>(-1) || count == static_cast<std::size_t>(-2)) {
            input_state = {};
            result += value[offset++];
            continue;
        }
        if (count == 0U) count = 1U;
        if (mode == 'u' || mode == 'U') character = static_cast<wchar_t>(std::towupper(static_cast<wint_t>(character)));
        else if (mode == 'l' || mode == 'L') character = static_cast<wchar_t>(std::towlower(static_cast<wint_t>(character)));
        char encoded[MB_LEN_MAX];
        std::mbstate_t output_state{};
        const auto length = std::wcrtomb(encoded, character, &output_state);
        if (length == static_cast<std::size_t>(-1)) result.append(value.substr(offset, count));
        else result.append(encoded, length);
        offset += count;
    }
}

std::vector<Entry> matching_sources(
    const std::vector<Entry>& sources, const CopyMask& mask, const OperationOptions& options
) {
    std::vector<Entry> result;
    for (const auto& source : sources) {
        if (!mask.matches(source.name)) continue;
        if (options.destination_mask) {
            static_cast<void>(mask.transform(source.name, *options.destination_mask));
        }
        result.push_back(source);
    }
    return result;
}

std::string masked_destination(
    Provider& provider, const std::string_view directory, const Entry& source,
    const CopyMask& mask, const OperationOptions& options, const Cancellation& cancellation
) {
    const auto name = options.destination_mask
        ? *mask.transform(source.name, *options.destination_mask) : source.name;
    auto destination = provider.join(directory, name);
    if (options.dive_into_subdirectories &&
        (source.kind == EntryKind::directory ||
         (!options.preserve_symlinks && source.target_is_directory)) &&
        provider.exists(destination)) {
        const auto target = provider.stat(destination, cancellation);
        if (target.kind == EntryKind::directory || target.target_is_directory) {
            destination = provider.join(destination, source.name);
        }
    }
    return destination;
}

void require_capability(
    const Provider& provider, const ProviderCapability capability,
    const std::string_view operation
) {
    if (!has_capability(provider.capabilities(), capability)) {
        throw std::runtime_error(
            std::string(operation) + " is not supported by this read-only provider: " + provider.display_name()
        );
    }
}

void emit_progress(
    const ProgressCallback& callback,
    const TransferProgress& progress
) {
    if (callback) {
        callback(progress);
    }
}

void emit_completed_progress(
    const ProgressCallback& callback,
    const TransferProgress& progress
) {
    try {
        emit_progress(callback, progress);
    } catch (const SkipCurrent&) {
        // A skip after publication cannot undo the completed entry.
    }
}

void preserve_time(
    Provider& destination_provider, const std::string& destination,
    const Entry& source
) {
    if (auto* local = dynamic_cast<LocalProvider*>(&destination_provider)) {
        local->set_modified(destination, source.modified);
    } else if (auto* archive = dynamic_cast<ArchiveProvider*>(&destination_provider)) {
        const auto modified = archive->format() == ArchiveFormat::tar ? source.modified :
            std::chrono::system_clock::time_point{std::chrono::floor<std::chrono::seconds>(source.modified.time_since_epoch())};
        archive->set_modified(destination, modified);
    }
}

void record_completed(
    OperationResult* result,
    const Entry& source
) {
    if (result != nullptr) {
        result->completed_sources.push_back(source.path);
    }
}

void record_skipped(
    OperationResult* result,
    const Entry& source,
    std::string reason
) {
    if (result != nullptr) {
        result->skipped.push_back({source.path, std::move(reason)});
    }
}

bool directory_destination(
    Provider& provider,
    const std::string_view destination,
    const Cancellation& cancellation
) {
    if (destination.ends_with('/')) {
        return true;
    }
    if (!provider.exists(destination)) {
        return false;
    }
    const auto entry = provider.stat(destination, cancellation);
    return entry.kind == EntryKind::directory ||
           entry.target_is_directory;
}

void validate_directory_target(
    Provider& source_provider,
    const std::string_view source,
    Provider& destination_provider,
    const std::string_view destination
) {
    const auto* local_source =
        dynamic_cast<const LocalProvider*>(&source_provider);
    const auto* local_destination =
        dynamic_cast<const LocalProvider*>(&destination_provider);
    if (local_source != nullptr && local_destination != nullptr) {
        local_source->validate_directory_target(
            source, *local_destination, destination
        );
        return;
    }
    const auto* archive_source = dynamic_cast<const ArchiveProvider*>(&source_provider);
    const auto* archive_destination = dynamic_cast<const ArchiveProvider*>(&destination_provider);
    if (archive_source != nullptr && archive_destination != nullptr &&
        archive_source->stat(source, {}).kind == EntryKind::directory) {
        archive_source->validate_directory_target(source, *archive_destination, destination);
    } else if (local_source != nullptr && archive_destination != nullptr) {
        const LocalProvider filesystem;
        local_source->validate_directory_target(
            source, filesystem, archive_destination->archive_path().string()
        );
    }
}

bool same_move_file(
    Provider& source_provider,
    const std::string_view source,
    Provider& destination_provider,
    const std::string_view destination
) {
    const auto* local_source =
        dynamic_cast<const LocalProvider*>(&source_provider);
    const auto* local_destination =
        dynamic_cast<const LocalProvider*>(&destination_provider);
    if (local_source != nullptr && local_destination != nullptr) {
        return local_source->normalize(source) ==
                   local_destination->normalize(destination) ||
               local_source->same_file(
                   source, *local_destination, destination
               );
    }
    if (dynamic_cast<const ArchiveProvider*>(&source_provider) != nullptr &&
        dynamic_cast<const ArchiveProvider*>(&destination_provider) != nullptr &&
        source_provider.id() == destination_provider.id() &&
        source_provider.exists(source) && destination_provider.exists(destination)) {
        const auto first = source_provider.stat(source, {});
        const auto second = destination_provider.stat(destination, {});
        if (first.kind == EntryKind::file && second.kind == EntryKind::file &&
            first.inode == second.inode && first.revision == second.revision) {
            return true;
        }
    }
    return source_provider.id() == destination_provider.id() &&
           source_provider.normalize(source) ==
               destination_provider.normalize(destination);
}

void validate_move_target(
    Provider& source_provider,
    const Entry& source,
    Provider& destination_provider,
    const std::string_view destination,
    const Cancellation& cancellation
) {
    validate_directory_target(
        source_provider, source.path, destination_provider, destination
    );
    if (destination_provider.exists(destination)) {
        const auto target = destination_provider.stat(destination, cancellation);
        if ((source.kind == EntryKind::directory) !=
            (target.kind == EntryKind::directory)) {
            throw std::invalid_argument(
                "cannot move between a directory and a non-directory"
            );
        }
    }
}

void validate_move_source(
    Provider& provider,
    const Entry& source,
    const Cancellation& cancellation
) {
    if (dynamic_cast<LocalProvider*>(&provider) != nullptr &&
        !source.revision.empty() &&
        provider.stat(source.path, cancellation).revision != source.revision) {
        throw std::runtime_error("source changed after copying; source was not removed: " + source.path);
    }
}

bool rename_fallback_allowed(
    const std::filesystem::filesystem_error& error,
    Provider& source_provider,
    const Entry& source,
    Provider& destination_provider,
    const std::string_view destination,
    const Cancellation& cancellation
) {
    const auto code = error.code();
    if (code != std::errc::cross_device_link &&
        code != std::errc::directory_not_empty &&
        code != std::errc::file_exists) {
        return false;
    }
    const auto current_source = source_provider.stat(source.path, cancellation);
    if (!destination_provider.exists(destination)) {
        return code == std::errc::cross_device_link;
    }
    const auto target = destination_provider.stat(destination, cancellation);
    if (code == std::errc::cross_device_link) {
        return (current_source.kind == EntryKind::directory) ==
               (target.kind == EntryKind::directory);
    }
    return current_source.kind == EntryKind::directory &&
           target.kind == EntryKind::directory;
}

enum class RenameOutcome {
    renamed,
    fallback,
    skipped,
};

RenameOutcome rename_with_retry(
    Provider& source_provider,
    const Entry& source,
    Provider& destination_provider,
    const std::string_view destination,
    const bool overwrite,
    const OperationOptions& options,
    const Cancellation& cancellation,
    std::string& skip_reason
) {
    for (;;) {
        cancellation.throw_if_requested();
        try {
            source_provider.rename(source.path, destination, overwrite);
            return RenameOutcome::renamed;
        } catch (const std::filesystem::filesystem_error& error) {
            if (rename_fallback_allowed(
                    error, source_provider, source,
                    destination_provider, destination, cancellation
                )) {
                return RenameOutcome::fallback;
            }
            if (!options.error) {
                throw;
            }
            switch (options.error(source, error.what())) {
                case OperationErrorDecision::retry:
                    continue;
                case OperationErrorDecision::skip:
                    skip_reason = error.what();
                    return RenameOutcome::skipped;
                case OperationErrorDecision::cancel:
                    throw OperationCancelled{};
            }
        }
    }
}

bool remove_with_retry(
    Provider& provider,
    const Entry& entry,
    const Cancellation& cancellation,
    const ProgressCallback& progress,
    const OperationErrorResolver& resolve,
    const std::function<void()>& validate = {},
    const std::function<bool()>& perform = {}
) {
    for (;;) {
        cancellation.throw_if_requested();
        try {
            require_capability(provider, ProviderCapability::remove, "deletion");
            if (validate) {
                validate();
            }
            if (perform) {
                return perform();
            }
            provider.remove(
                entry.path, entry.kind == EntryKind::directory,
                cancellation, progress
            );
            return true;
        } catch (const OperationCancelled&) {
            throw;
        } catch (const SkipCurrent&) {
            throw;
        } catch (const std::exception& error) {
            if (!resolve) {
                throw;
            }
            switch (resolve(entry, error.what())) {
                case OperationErrorDecision::retry:
                    continue;
                case OperationErrorDecision::skip:
                    return false;
                case OperationErrorDecision::cancel:
                    throw OperationCancelled{};
            }
        }
    }
}

bool is_descendant_path(
    Provider& provider,
    const std::string_view parent,
    const std::string_view candidate
) {
    const auto parent_path = std::filesystem::path(
        provider.normalize(parent)
    );
    const auto candidate_path = std::filesystem::path(
        provider.normalize(candidate)
    );
    auto parent_part = parent_path.begin();
    auto candidate_part = candidate_path.begin();
    while (parent_part != parent_path.end() &&
           candidate_part != candidate_path.end() &&
           *parent_part == *candidate_part) {
        ++parent_part;
        ++candidate_part;
    }
    return parent_part == parent_path.end() &&
           candidate_part != candidate_path.end();
}

void accumulate_entry(
    Provider& provider,
    const Entry& entry,
    const Cancellation& cancellation,
    TransferProgress& result,
    const bool tolerate_errors = false
) {
    cancellation.throw_if_requested();
    if (entry.kind == EntryKind::directory) {
        std::vector<Entry> children;
        try {
            children = provider.list(entry.path, {.show_hidden = true}, cancellation);
        } catch (const OperationCancelled&) {
            throw;
        } catch (const std::exception&) {
            if (!tolerate_errors) {
                throw;
            }
            // Counting is an estimate; the real traversal will invoke the error resolver.
            return;
        }
        for (const auto& child : children) {
            accumulate_entry(provider, child, cancellation, result, tolerate_errors);
        }
        return;
    }
    ++result.files_total;
    result.bytes_total += entry.size;
}

void accumulate_removal(
    Provider& provider,
    const Entry& entry,
    const Cancellation& cancellation,
    TransferProgress& result,
    const bool tolerate_errors = false
) {
    cancellation.throw_if_requested();
    ++result.files_total;
    if (entry.kind != EntryKind::directory) {
        result.bytes_total += entry.size;
        return;
    }
    std::vector<Entry> children;
    try {
        children = provider.list(entry.path, {.show_hidden = true}, cancellation);
    } catch (const OperationCancelled&) {
        throw;
    } catch (const std::exception&) {
        if (!tolerate_errors) {
            throw;
        }
        return;
    }
    for (const auto& child : children) {
        accumulate_removal(
            provider,
            child,
            cancellation,
            result,
            tolerate_errors
        );
    }
}

class DescriptorBuffer final : public std::streambuf {
public:
    explicit DescriptorBuffer(
        const int descriptor,
        std::function<void()> checkpoint = {}
    ) : descriptor_(descriptor), checkpoint_(std::move(checkpoint)) {}

protected:
    std::streamsize xsputn(
        const char* data,
        const std::streamsize count
    ) override {
        std::streamsize offset = 0;
        while (offset < count) {
            if (checkpoint_) {
                checkpoint_();
            }
            const auto result = ::write(
                descriptor_,
                data + offset,
                static_cast<std::size_t>(count - offset)
            );
            if (result < 0 && errno == EINTR) {
                continue;
            }
            if (result <= 0) {
                return offset;
            }
            offset += result;
        }
        return count;
    }

    int overflow(const int character) override {
        if (character == traits_type::eof()) {
            return traits_type::not_eof(character);
        }
        const auto byte = static_cast<char>(character);
        return xsputn(&byte, 1) == 1
                   ? character
                   : traits_type::eof();
    }

private:
    int descriptor_;
    std::function<void()> checkpoint_;
};

class TemporaryTransferFile final {
public:
    TemporaryTransferFile() {
        auto pattern =
            (
                std::filesystem::temp_directory_path() /
                "mcpp-transfer-XXXXXX"
            ).string();
        auto buffer = std::vector<char>(
            pattern.begin(),
            pattern.end()
        );
        buffer.push_back('\0');
        descriptor_ = mkostemp(buffer.data(), O_CLOEXEC);
        if (descriptor_ < 0) {
            throw std::runtime_error(
                "cannot create transfer temporary file: " +
                std::string(std::strerror(errno))
            );
        }
        path_ = buffer.data();
    }

    ~TemporaryTransferFile() {
        if (descriptor_ >= 0) {
            static_cast<void>(close(descriptor_));
        }
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    TemporaryTransferFile(const TemporaryTransferFile&) = delete;
    TemporaryTransferFile& operator=(
        const TemporaryTransferFile&
    ) = delete;

    [[nodiscard]] int descriptor() const noexcept {
        return descriptor_;
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

    void close_output() {
        if (descriptor_ >= 0) {
            if (fsync(descriptor_) != 0) {
                throw std::runtime_error(
                    "cannot commit transfer temporary file"
                );
            }
            if (close(descriptor_) != 0) {
                descriptor_ = -1;
                throw std::runtime_error(
                    "cannot commit transfer temporary file"
                );
            }
            descriptor_ = -1;
        }
    }

private:
    std::filesystem::path path_;
    int descriptor_{-1};
};

}  // namespace

struct CopyMask::Pattern {
    Pattern(std::string value, const bool shell)
        : source(std::move(value)), shell_patterns(shell) {
        if (!shell_patterns) expression.emplace(source);
    }
    std::string source;
    bool shell_patterns;
    std::optional<std::regex> expression;
};

CopyMask::CopyMask(const std::string_view source, const bool shell_patterns) {
    if (source.find('\0') != std::string_view::npos) {
        throw std::invalid_argument("source mask contains NUL");
    }
    try {
        pattern_ = std::make_shared<Pattern>(std::string(source), shell_patterns);
    } catch (const std::regex_error& error) {
        throw std::invalid_argument("invalid source mask: " + std::string(error.what()));
    }
}

bool CopyMask::matches(const std::string_view name) const {
    return pattern_->shell_patterns
        ? fnmatch(pattern_->source.c_str(), std::string(name).c_str(), 0) == 0
        : std::regex_search(name.begin(), name.end(), *pattern_->expression);
}

std::optional<std::string> CopyMask::transform(
    const std::string_view name, const std::string_view destination
) const {
    std::vector<std::string> captures;
    if (pattern_->shell_patterns) {
        if (!matches(name)) return std::nullopt;
        captures = shell_mask_captures(pattern_->source, std::string(name));
    } else {
        std::match_results<std::string_view::const_iterator> match;
        if (!std::regex_search(name.begin(), name.end(), match, *pattern_->expression)) {
            return std::nullopt;
        }
        for (const auto& capture : match) captures.push_back(capture.str());
        captures[0] = name;
    }
    std::string result;
    std::size_t next_group = 1U;
    char conversion = '\0';
    char next_conversion = '\0';
    const auto append = [&](const std::string_view value) {
        append_mask_text(result, value, conversion, next_conversion);
    };
    const auto group = [&](const std::size_t index) {
        if (index == 0U) {
            append(name);
        } else {
            if (index >= captures.size()) {
                throw std::invalid_argument("destination mask refers to missing source group " + std::to_string(index));
            }
            append(captures[index]);
        }
    };
    for (std::size_t index = 0; index < destination.size();) {
        const auto character = destination[index++];
        if (character == '*') {
            group(next_group++);
        } else if (character == '\\') {
            if (index == destination.size()) {
                throw std::invalid_argument("unfinished escape in destination mask");
            }
            const auto escaped = destination[index++];
            if (escaped >= '0' && escaped <= '9') {
                group(static_cast<std::size_t>(escaped - '0'));
            } else if (escaped == 'u' || escaped == 'l') {
                next_conversion = escaped;
            } else if (escaped == 'U' || escaped == 'L' || escaped == 'E') {
                conversion = escaped == 'E' ? '\0' : escaped;
            } else {
                append(destination.substr(index - 1U, 1U));
            }
        } else {
            const auto begin = index - 1U;
            while (index < destination.size() && destination[index] != '*' && destination[index] != '\\') ++index;
            append(destination.substr(begin, index - begin));
        }
    }
    if (result.empty() || result == "." || result == ".." ||
        result.find('/') != std::string::npos || result.find('\0') != std::string::npos) {
        throw std::invalid_argument("destination mask must produce a nonempty file name, not a path");
    }
    return result;
}

bool CopyMask::has_replacements(const std::string_view destination) {
    for (std::size_t index = 0; index < destination.size(); ++index) {
        if (destination[index] == '*') return true;
        if (destination[index] == '\\' && index + 1U < destination.size()) {
            const auto escaped = destination[++index];
            if ((escaped >= '0' && escaped <= '9') ||
                std::string_view("uUlLE").find(escaped) != std::string_view::npos) {
                return true;
            }
        }
    }
    return false;
}

std::string CopyMask::unescape_path(const std::string_view path) {
    if (path.find('\0') != std::string_view::npos) {
        throw std::invalid_argument("destination path contains NUL");
    }
    std::string result;
    for (std::size_t index = 0; index < path.size(); ++index) {
        if (path[index] == '\\' && ++index == path.size()) {
            throw std::invalid_argument("unfinished escape in destination path");
        }
        result += path[index];
    }
    return result;
}

CollisionOptions OperationEngine::collision_options(
    const Provider& source_provider,
    const Entry& source,
    const Provider& destination_provider,
    const Entry& destination
) {
    const bool regular_local =
        dynamic_cast<const LocalProvider*>(&source_provider) != nullptr &&
        dynamic_cast<const LocalProvider*>(&destination_provider) != nullptr &&
        source.kind == EntryKind::file && destination.kind == EntryKind::file;
    return {
        .append = regular_local,
        .reget = regular_local && destination.size < source.size,
    };
}

OperationEngine::Collision OperationEngine::resolve_collision(
    Provider& source_provider,
    const Entry& source,
    Provider& destination_provider,
    const std::string_view destination,
    const OperationOptions& options,
    const Cancellation& cancellation,
    std::optional<Collision> current
) {
    cancellation.throw_if_requested();
    auto collision = current ? std::move(*current) : Collision{};
    if (!current && destination_provider.exists(destination)) {
        collision.destination = destination_provider.stat(destination, cancellation);
        if (!options.overwrite) {
            if (!options.collision) {
                throw std::runtime_error("destination exists: " + std::string(destination));
            }
            collision.decision = options.collision(source, *collision.destination);
        }
    }
    if (collision.decision == CollisionDecision::cancel) {
        throw OperationCancelled{};
    }
    cancellation.throw_if_requested();
    if (collision.decision == CollisionDecision::skip) {
        return collision;
    }
    if (collision.decision == CollisionDecision::append ||
        collision.decision == CollisionDecision::reget) {
        const auto available = collision.destination
            ? collision_options(source_provider, source, destination_provider, *collision.destination)
            : CollisionOptions{};
        if (collision.decision == CollisionDecision::append && !available.append) {
            throw std::invalid_argument("append requires existing regular local files");
        }
        if (collision.decision == CollisionDecision::reget && !available.reget) {
            throw std::invalid_argument("reget requires a smaller existing regular local destination");
        }
    }
    if (collision.destination) {
        if (!destination_provider.exists(destination) ||
            destination_provider.stat(destination, cancellation).revision != collision.destination->revision) {
            throw std::runtime_error("destination changed since the collision decision: " + std::string(destination));
        }
    } else if (current && destination_provider.exists(destination)) {
        throw std::runtime_error("destination appeared before copying: " + std::string(destination));
    }
    return collision;
}

TransferProgress OperationEngine::measure(
    Provider& provider,
    const std::vector<Entry>& entries,
    const Cancellation& cancellation,
    const bool tolerate_errors
) {
    TransferProgress result;
    for (const auto& entry : entries) {
        accumulate_entry(provider, entry, cancellation, result, tolerate_errors);
    }
    return result;
}

bool OperationEngine::copy_entry(
    Provider& source_provider,
    const Entry& source,
    Provider& destination_provider,
    const std::string_view destination,
    const OperationOptions& options,
    const Cancellation& cancellation,
    TransferProgress& state,
    const ProgressCallback& progress,
    std::string& skip_reason,
    std::optional<Collision> collision_current,
    std::vector<Entry> ancestors,
    std::shared_ptr<LinkCache> links,
    Entry* transferred_source,
    CollisionDecision* transferred_decision
) {
    if (!links) {
        links = std::make_shared<LinkCache>();
    }
    const auto before = state;
    const bool existed_before = destination_provider.exists(destination);
    auto current_source = source;
    bool refresh_source = false;
    for (;;) {
        try {
            if (refresh_source) {
                current_source = source_provider.stat(source.path, cancellation);
                refresh_source = false;
            }
            if (transferred_source != nullptr) *transferred_source = current_source;
            return copy_entry_once(
                source_provider, current_source, destination_provider, destination,
                options, cancellation, state, progress, skip_reason,
                collision_current, ancestors, links, transferred_source, transferred_decision
            );
        } catch (const OperationCancelled&) {
            throw;
        } catch (const SkipCurrent&) {
            throw;
        } catch (const std::exception& error) {
            if (!options.error) {
                throw;
            }
            switch (options.error(source, error.what())) {
                case OperationErrorDecision::retry:
                    cancellation.throw_if_requested();
                    state = before;
                    skip_reason.clear();
                    refresh_source = true;
                    collision_current.reset();
                    if (!existed_before &&
                        (source.kind == EntryKind::directory ||
                         (!options.preserve_symlinks && source.target_is_directory)) &&
                        destination_provider.exists(destination) &&
                        destination_provider.stat(destination, cancellation).kind == EntryKind::directory) {
                        collision_current = Collision{
                            .decision = CollisionDecision::overwrite,
                            .destination = destination_provider.stat(destination, cancellation),
                        };
                    }
                    continue;
                case OperationErrorDecision::skip:
                    skip_reason = error.what();
                    return false;
                case OperationErrorDecision::cancel:
                    throw OperationCancelled{};
            }
        }
    }
}

bool OperationEngine::copy_entry_once(
    Provider& source_provider,
    const Entry& source,
    Provider& destination_provider,
    const std::string_view destination,
    const OperationOptions& options,
    const Cancellation& cancellation,
    TransferProgress& state,
    const ProgressCallback& progress,
    std::string& skip_reason,
    std::optional<Collision> collision_current,
    std::vector<Entry> ancestors,
    const std::shared_ptr<LinkCache>& links,
    Entry* transferred_source,
    CollisionDecision* transferred_decision
) {
    cancellation.throw_if_requested();
    require_capability(destination_provider, ProviderCapability::write, "copy destination");
    const auto destination_path = std::string(destination);
    if (same_move_file(
            source_provider, source.path, destination_provider, destination_path
        )) {
        skip_reason = "source and destination are the same file";
        return false;
    }
    if (source.kind == EntryKind::directory &&
        source_provider.id() == destination_provider.id() &&
        is_descendant_path(
            source_provider,
            source.path,
            destination_path
        )) {
        throw std::invalid_argument(
            "cannot copy a directory into itself"
        );
    }
    const auto collision = resolve_collision(
        source_provider, source, destination_provider, destination_path,
        options, cancellation, std::move(collision_current)
    );
    if (collision.decision == CollisionDecision::skip) {
        skip_reason = "destination exists: " + destination_path;
        return false;
    }
    const bool destination_exists = collision.destination.has_value();
    const bool overwrite = destination_exists;
    if (transferred_decision != nullptr) *transferred_decision = collision.decision;

    state.current_path = source.path;
    emit_progress(progress, state);
    auto effective_source = source;
    if (collision.decision != CollisionDecision::overwrite) {
        effective_source = source_provider.stat(source.path, cancellation);
        if (effective_source.kind != EntryKind::file) {
            throw std::runtime_error("append or reget source is no longer a regular file: " + source.path);
        }
        if (transferred_source != nullptr) *transferred_source = effective_source;
    }
    if (source.kind == EntryKind::symlink && !options.preserve_symlinks) {
        effective_source = source_provider.stat_follow(source.path, cancellation);
        if (same_move_file(source_provider, effective_source.path,
                           destination_provider, destination_path)) {
            skip_reason = "source and destination are the same file";
            return false;
        }
    }
    if (collision.destination &&
        (effective_source.kind == EntryKind::directory) !=
            (collision.destination->kind == EntryKind::directory)) {
        throw std::invalid_argument("cannot copy between a directory and a non-directory");
    }
    if (effective_source.kind != EntryKind::directory) {
        state.bytes_total = std::max(state.bytes_total, state.bytes_done + effective_source.size);
        state.files_total = std::max(state.files_total, state.files_done + 1U);
    }
    auto* local_source = dynamic_cast<LocalProvider*>(&source_provider);
    auto* local_destination = dynamic_cast<LocalProvider*>(&destination_provider);
    auto* archive_source = dynamic_cast<ArchiveProvider*>(&source_provider);
    auto* archive_destination = dynamic_cast<ArchiveProvider*>(&destination_provider);
    const auto expected_destination_revision = collision.destination
        ? std::optional{collision.destination->revision} : std::nullopt;
    // Local revisions include st_dev as well as st_ino. Archive revisions
    // identify the virtual object and its version, not the whole repack.
    const auto link_key = source_provider.id() + "|" + source.revision;
    const bool track_links =
        collision.decision == CollisionDecision::overwrite &&
        (local_source != nullptr || archive_source != nullptr) &&
        (local_destination != nullptr || archive_destination != nullptr) &&
        options.preserve_symlinks &&
        (source.kind == EntryKind::file ||
         (source.kind == EntryKind::other && local_source != nullptr && local_destination != nullptr)) &&
        (links->contains(link_key) || (local_source != nullptr
            ? local_source->hardlink_count(source.path)
            : archive_source->hardlink_count(source.path)) > 1U);
    if (track_links) {
        const auto cached = links->find(link_key);
        if (cached != links->end() && destination_provider.exists(cached->second.first) &&
            destination_provider.stat(cached->second.first, cancellation).revision == cached->second.second) {
            if (local_destination != nullptr) {
                local_destination->copy_hardlink(
                    cached->second.first, destination_path, overwrite, cancellation,
                    expected_destination_revision
                );
            } else {
                archive_destination->copy_hardlink(cached->second.first, destination_path, overwrite, cancellation);
            }
            cached->second.second = destination_provider.stat(cached->second.first, cancellation).revision;
            state.bytes_done += effective_source.size;
            ++state.files_done;
            emit_completed_progress(progress, state);
            return true;
        }
    }
    const auto remember_link = [&] {
        if (track_links) {
            (*links)[link_key] = {
                destination_path, destination_provider.stat(destination_path, cancellation).revision
            };
        }
    };

    if (local_source != nullptr && local_destination != nullptr &&
        (effective_source.kind == EntryKind::file || effective_source.kind == EntryKind::other)) {
        const auto copy_mode = collision.decision == CollisionDecision::append
            ? LocalProvider::CopyMode::append
            : collision.decision == CollisionDecision::reget
                ? LocalProvider::CopyMode::reget : LocalProvider::CopyMode::replace;
        local_destination->copy_file_from(
            *local_source, effective_source.path, destination_path, overwrite,
            options.preserve_permissions, cancellation,
            [&](const TransferProgress& value) {
                auto overall = state;
                overall.bytes_done += value.bytes_done;
                overall.current_path = value.current_path.empty() ? source.path : value.current_path;
                emit_progress(progress, overall);
            },
            copy_mode,
            copy_mode == LocalProvider::CopyMode::replace || effective_source.revision.empty()
                ? std::nullopt : std::optional{effective_source.revision},
            expected_destination_revision
        );
        remember_link();
        state.bytes_done += effective_source.size;
        ++state.files_done;
        emit_completed_progress(progress, state);
        return true;
    }
    if (local_source != nullptr && local_destination != nullptr &&
        source.kind == EntryKind::symlink && options.preserve_symlinks) {
        local_destination->copy_symlink_from(
            *local_source, source.path, destination_path,
            options.stable_symlinks, overwrite, cancellation,
            source.revision.empty() ? std::nullopt : std::optional{source.revision},
            expected_destination_revision
        );
        if (options.preserve_permissions) {
            preserve_time(destination_provider, destination_path, source);
        }
        state.bytes_done += source.size;
        ++state.files_done;
        emit_completed_progress(progress, state);
        return true;
    }
    if (effective_source.kind == EntryKind::other) {
        throw std::runtime_error("unsupported special file copy between providers: " + source.path);
    }

    if (&source_provider == &destination_provider &&
        options.preserve_permissions &&
        options.preserve_symlinks &&
        source.kind != EntryKind::directory &&
        has_capability(
            source_provider.capabilities(),
            ProviderCapability::direct_copy
        )) {
        const bool copied_all = source_provider.copy_direct(
            source.path,
            destination_path,
            overwrite,
            cancellation,
            [&](const TransferProgress& direct) {
                auto overall = state;
                overall.bytes_done += direct.bytes_done;
                overall.files_done += direct.files_done;
                overall.current_path =
                    direct.current_path.empty()
                        ? source.path
                        : direct.current_path;
                emit_progress(progress, overall);
            }
        );
        if (copied_all) {
            if (options.preserve_permissions) {
                preserve_time(destination_provider, destination_path, source);
            }
            remember_link();
            TransferProgress completed;
            accumulate_entry(
                source_provider,
                source,
                cancellation,
                completed
            );
            state.bytes_done += completed.bytes_total;
            state.files_done += completed.files_total;
            emit_completed_progress(progress, state);
        } else if (skip_reason.empty()) {
            skip_reason = "one or more descendants were skipped";
        }
        return copied_all;
    }

    if (effective_source.kind == EntryKind::directory) {
        if (std::ranges::any_of(ancestors, [&](const Entry& ancestor) {
                return same_move_file(
                    source_provider, effective_source.path,
                    source_provider, ancestor.path
                );
            })) {
            throw std::runtime_error(
                "cannot copy cyclic symbolic link: " + source.path
            );
        }
        ancestors.push_back(effective_source);
        validate_directory_target(
            source_provider,
            effective_source.path,
            destination_provider,
            destination_path
        );
        if (!destination_exists) {
            destination_provider.create_directory(destination_path);
        }
        bool copied_all = true;
        for (const auto& child :
             source_provider.list(
                 source.path,
                 {.show_hidden = true},
                 cancellation
             )) {
            try {
                copied_all =
                    copy_entry(
                    source_provider,
                    child,
                    destination_provider,
                    destination_provider.join(
                        destination_path,
                        child.name
                    ),
                    options,
                    cancellation,
                    state,
                    progress,
                    skip_reason,
                    {},
                    ancestors,
                    links
                ) && copied_all;
            } catch (const SkipCurrent&) {
                copied_all = false;
                if (skip_reason.empty()) {
                    skip_reason = "user skipped: " + child.path;
                }
            }
        }
        if (options.preserve_permissions &&
            has_capability(
                destination_provider.capabilities(),
                ProviderCapability::permissions
            ) &&
            effective_source.permissions !=
                std::filesystem::perms::unknown) {
            destination_provider.set_permissions(
                destination_path,
                effective_source.permissions
            );
        }
        if (copied_all && options.preserve_permissions) {
            preserve_time(destination_provider, destination_path, effective_source);
        }
        return copied_all;
    }

    if (source.kind == EntryKind::symlink &&
        options.preserve_symlinks &&
        has_capability(
            destination_provider.capabilities(),
            ProviderCapability::links
        )) {
        destination_provider.create_symlink(
            source.target,
            destination_path,
            overwrite,
            cancellation
        );
    } else {
        TemporaryTransferFile temporary;
        DescriptorBuffer buffer(temporary.descriptor(), [&] {
            cancellation.throw_if_requested();
            emit_progress(progress, state);
        });
        std::ostream output(&buffer);
        output.exceptions(std::ios::badbit | std::ios::failbit);
        source_provider.read(
            source.path,
            0,
            effective_source.size,
            output,
            cancellation
        );
        output.flush();
        if (!output.good()) {
            throw std::runtime_error(
                "failed while staging transfer data"
            );
        }
        temporary.close_output();
        std::ifstream input(temporary.path(), std::ios::binary);
        const auto callback = [&](const TransferProgress& value) {
            auto overall = state;
            overall.bytes_done += value.bytes_done;
            overall.current_path = source.path;
            emit_progress(progress, overall);
        };
        if (local_destination != nullptr && options.preserve_permissions &&
            effective_source.permissions != std::filesystem::perms::unknown) {
            local_destination->write_with_permissions(
                destination_path, input, effective_source.size, overwrite,
                cancellation, callback, effective_source.permissions
            );
        } else {
            destination_provider.write(
                destination_path, input, effective_source.size, overwrite,
                std::nullopt, cancellation, callback
            );
        }
    }

    if (effective_source.kind != EntryKind::symlink &&
        options.preserve_permissions &&
        has_capability(
            destination_provider.capabilities(),
            ProviderCapability::permissions
        ) &&
        effective_source.permissions !=
            std::filesystem::perms::unknown) {
        destination_provider.set_permissions(
            destination_path,
            effective_source.permissions
        );
    }
    if (options.preserve_permissions) {
        preserve_time(destination_provider, destination_path, effective_source);
    }
    remember_link();
    state.bytes_done += effective_source.size;
    ++state.files_done;
    emit_completed_progress(progress, state);
    return true;
}

void OperationEngine::copy(
    Provider& source_provider,
    const std::vector<Entry>& requested_sources,
    Provider& destination_provider,
    const std::string_view destination_directory,
    const OperationOptions& options,
    const Cancellation& cancellation,
    const ProgressCallback& progress,
    OperationResult* result
) const {
    const CopyMask mask(options.source_mask, options.shell_patterns);
    const auto sources = matching_sources(requested_sources, mask, options);
    auto state = options.compute_totals
                     ? measure(source_provider, sources, cancellation, static_cast<bool>(options.error))
                     : TransferProgress{};
    auto links = std::make_shared<LinkCache>();
    for (const auto& source : sources) {
        try {
            std::string skip_reason;
            const auto copied = copy_entry(
                source_provider,
                source,
                destination_provider,
                masked_destination(
                    destination_provider, destination_directory, source, mask, options, cancellation
                ),
                options,
                cancellation,
                state,
                progress,
                skip_reason,
                {},
                {},
                links
            );
            if (copied) {
                record_completed(result, source);
            } else {
                record_skipped(result, source, std::move(skip_reason));
            }
        } catch (const SkipCurrent&) {
            record_skipped(result, source, "user requested skip");
            continue;
        }
    }
}

void OperationEngine::copy_exact(
    Provider& source_provider,
    const Entry& source,
    Provider& destination_provider,
    const std::string_view destination,
    const OperationOptions& options,
    const Cancellation& cancellation,
    const ProgressCallback& progress,
    OperationResult* result
) const {
    cancellation.throw_if_requested();
    if (!CopyMask(options.source_mask, options.shell_patterns).matches(source.name)) return;
    if (directory_destination(
            destination_provider,
            destination,
            cancellation
        )) {
        copy(
            source_provider,
            {source},
            destination_provider,
            destination,
            options,
            cancellation,
            progress,
            result
        );
        return;
    }
    auto state = options.compute_totals
                     ? measure(source_provider, {source}, cancellation, static_cast<bool>(options.error))
                     : TransferProgress{};
    try {
        std::string skip_reason;
        if (copy_entry(
                source_provider,
                source,
                destination_provider,
                destination,
                options,
                cancellation,
                state,
                progress,
                skip_reason
            )) {
            record_completed(result, source);
        } else {
            record_skipped(result, source, std::move(skip_reason));
        }
    } catch (const SkipCurrent&) {
        record_skipped(result, source, "user requested skip");
    }
}

void OperationEngine::move(
    Provider& source_provider,
    const std::vector<Entry>& requested_sources,
    Provider& destination_provider,
    const std::string_view destination_directory,
    const OperationOptions& options,
    const Cancellation& cancellation,
    const ProgressCallback& progress,
    OperationResult* result
) const {
    cancellation.throw_if_requested();
    require_capability(source_provider, ProviderCapability::remove, "move source removal");
    require_capability(destination_provider, ProviderCapability::write, "move destination");
    const CopyMask mask(options.source_mask, options.shell_patterns);
    const auto sources = matching_sources(requested_sources, mask, options);
    auto links = std::make_shared<LinkCache>();
    if (&source_provider == &destination_provider &&
        has_capability(
            source_provider.capabilities(),
            ProviderCapability::rename
        )) {
        auto state = options.compute_totals
                         ? measure(source_provider, sources, cancellation, static_cast<bool>(options.error))
                         : TransferProgress{};
        for (const auto& source : sources) {
            cancellation.throw_if_requested();
            state.current_path = source.path;
            try {
                emit_progress(progress, state);
            } catch (const SkipCurrent&) {
                record_skipped(result, source, "user requested skip");
                continue;
            }
            const auto destination = masked_destination(
                destination_provider, destination_directory, source, mask, options, cancellation
            );
            validate_move_target(
                source_provider, source, destination_provider,
                destination, cancellation
            );
            if (same_move_file(
                    source_provider, source.path,
                    destination_provider, destination
                )) {
                record_skipped(
                    result, source,
                    "source and destination are the same file"
                );
                continue;
            }
            const auto collision = resolve_collision(
                source_provider, source, destination_provider, destination,
                options, cancellation
            );
            if (collision.decision == CollisionDecision::skip) {
                record_skipped(result, source, "destination exists: " + destination);
                continue;
            }
            const bool overwrite = collision.destination.has_value();
            std::string rename_skip;
            const auto renamed = collision.decision == CollisionDecision::overwrite &&
                                 !(options.stable_symlinks && options.preserve_symlinks &&
                                   source.kind == EntryKind::symlink)
                ? rename_with_retry(
                      source_provider, source, destination_provider, destination,
                      overwrite, options, cancellation, rename_skip
                  )
                : RenameOutcome::fallback;
            if (renamed == RenameOutcome::skipped) {
                record_skipped(result, source, std::move(rename_skip));
                continue;
            }
            if (renamed == RenameOutcome::renamed) {
                if (same_move_file(
                        source_provider, source.path,
                        destination_provider, destination
                    )) {
                    record_skipped(
                        result, source,
                        "source and destination are the same file"
                    );
                    continue;
                }
                record_completed(result, source);
                TransferProgress completed;
                if (options.compute_totals) accumulate_entry(
                    destination_provider,
                    destination_provider.stat(
                        destination,
                        cancellation
                    ),
                    cancellation,
                    completed,
                    true
                );
                else {
                    completed.files_total = 1;
                    completed.bytes_total = source.kind == EntryKind::file ? source.size : 0U;
                }
                state.bytes_done += completed.bytes_total;
                state.files_done += completed.files_total;
                state.current_path = source.path;
                try {
                    emit_progress(progress, state);
                } catch (const SkipCurrent&) {
                }
                continue;
            }
            try {
                std::string skip_reason;
                auto transferred_source = source;
                auto transferred_decision = CollisionDecision::overwrite;
                const auto copied = copy_entry(
                    source_provider,
                    source,
                    destination_provider,
                    destination,
                    options,
                    cancellation,
                    state,
                    progress,
                    skip_reason,
                    collision,
                    {},
                    links,
                    &transferred_source,
                    &transferred_decision
                );
                if (copied) {
                    if (remove_with_retry(
                            source_provider, source, cancellation, {}, options.error,
                            [&] {
                                if (transferred_decision != CollisionDecision::overwrite ||
                                    (options.stable_symlinks && source.kind == EntryKind::symlink)) {
                                    validate_move_source(source_provider, transferred_source, cancellation);
                                }
                                validate_move_target(
                                    source_provider, source, destination_provider,
                                    destination, cancellation
                                );
                            }
                        )) {
                        record_completed(result, source);
                    } else {
                        record_skipped(result, source, "source removal skipped after copy");
                    }
                } else {
                    record_skipped(
                        result,
                        source,
                        std::move(skip_reason)
                    );
                }
            } catch (const SkipCurrent&) {
                record_skipped(result, source, "user requested skip");
                continue;
            }
        }
        return;
    }
    auto state = options.compute_totals
                     ? measure(source_provider, sources, cancellation, static_cast<bool>(options.error))
                     : TransferProgress{};
    for (const auto& source : sources) {
        try {
            const auto destination = masked_destination(
                destination_provider, destination_directory, source, mask, options, cancellation
            );
            validate_move_target(
                source_provider, source, destination_provider,
                destination, cancellation
            );
            if (same_move_file(
                    source_provider, source.path,
                    destination_provider, destination
                )) {
                record_skipped(
                    result, source,
                    "source and destination are the same file"
                );
                continue;
            }
            std::string skip_reason;
            auto transferred_source = source;
            auto transferred_decision = CollisionDecision::overwrite;
            const auto copied = copy_entry(
                source_provider,
                source,
                destination_provider,
                destination,
                options,
                cancellation,
                state,
                progress,
                skip_reason,
                {},
                {},
                links,
                &transferred_source,
                &transferred_decision
            );
            if (copied) {
                if (remove_with_retry(
                        source_provider, source, cancellation, {}, options.error,
                        [&] {
                            if (transferred_decision != CollisionDecision::overwrite ||
                                (options.stable_symlinks && source.kind == EntryKind::symlink)) {
                                validate_move_source(source_provider, transferred_source, cancellation);
                            }
                            validate_move_target(
                                source_provider, source, destination_provider,
                                destination, cancellation
                            );
                        }
                    )) {
                    record_completed(result, source);
                } else {
                    record_skipped(result, source, "source removal skipped after copy");
                }
            } else {
                record_skipped(result, source, std::move(skip_reason));
            }
        } catch (const SkipCurrent&) {
            record_skipped(result, source, "user requested skip");
            continue;
        }
    }
}

void OperationEngine::remove(
    Provider& provider,
    const std::vector<Entry>& entries,
    const Cancellation& cancellation,
    const ProgressCallback& progress,
    OperationResult* result,
    const OperationErrorResolver& error,
    const bool compute_totals,
    const RecursiveDeleteResolver& recursive_delete
) const {
    TransferProgress state;
    if (compute_totals) {
        if (recursive_delete) {
            state.files_total = entries.size();
            for (const auto& entry : entries) {
                if (entry.kind != EntryKind::directory) state.bytes_total += entry.size;
            }
        } else {
            for (const auto& entry : entries) {
                accumulate_removal(provider, entry, cancellation, state, static_cast<bool>(error));
            }
        }
    }
    std::optional<bool> all_recursive;
    for (const auto& entry : entries) {
        cancellation.throw_if_requested();
        bool reported = false;
        bool approved = !recursive_delete;
        bool measured = !recursive_delete || !compute_totals;
        std::string skip_reason = "user skipped deletion after error";
        const std::function<bool()> confirm_recursive = [&] {
            if (!approved) {
                bool accept = false;
                if (all_recursive.has_value()) {
                    accept = *all_recursive;
                } else {
                    switch (recursive_delete(entry)) {
                        case RecursiveDeleteDecision::yes:
                            accept = true;
                            break;
                        case RecursiveDeleteDecision::no:
                            break;
                        case RecursiveDeleteDecision::all:
                            accept = true;
                            all_recursive = true;
                            break;
                        case RecursiveDeleteDecision::none:
                            all_recursive = false;
                            break;
                        case RecursiveDeleteDecision::cancel:
                            throw OperationCancelled{};
                    }
                }
                cancellation.throw_if_requested();
                if (!accept) {
                    skip_reason = "recursive deletion declined";
                    return false;
                }
                approved = true;
            }
            if (!measured) {
                // Do not walk a directory for totals until its deletion is approved.
                TransferProgress total;
                accumulate_removal(provider, entry, cancellation, total, true);
                state.files_total += total.files_total - 1U;
                state.bytes_total += total.bytes_total;
                measured = true;
            }
            return true;
        };
        const ProgressCallback update = [&](const TransferProgress& current) {
            reported = true;
            ++state.files_done;
            state.bytes_done += current.bytes_done;
            state.current_path = current.current_path;
            emit_progress(progress, state);
        };
        const auto removed = remove_with_retry(
            provider, entry, cancellation, update, error, {},
            [&] {
                if (auto* local = dynamic_cast<LocalProvider*>(&provider)) {
                    return local->remove_checked(
                        entry.path, entry.kind == EntryKind::directory,
                        cancellation, update, confirm_recursive
                    );
                }
                if (recursive_delete && entry.kind == EntryKind::directory) {
                    const auto before = provider.stat(entry.path, cancellation);
                    if (!provider.list(entry.path, {.show_hidden = true, .limit = 1U}, cancellation).empty()) {
                        if (!confirm_recursive()) return false;
                        const auto after = provider.stat(entry.path, cancellation);
                        if (after.kind != before.kind ||
                            (!before.revision.empty() && after.revision != before.revision)) {
                            throw std::runtime_error(
                                "directory changed before removal: " + entry.path
                            );
                        }
                    }
                }
                provider.remove(
                    entry.path, entry.kind == EntryKind::directory,
                    cancellation, update
                );
                return true;
            }
        );
        if (!removed) {
            record_skipped(result, entry, std::move(skip_reason));
            continue;
        }
        record_completed(result, entry);
        if (!reported) {
            ++state.files_done;
            state.current_path = entry.path;
            emit_progress(progress, state);
        }
    }
}

void OperationEngine::move_exact(
    Provider& source_provider,
    const Entry& source,
    Provider& destination_provider,
    const std::string_view destination,
    const OperationOptions& options,
    const Cancellation& cancellation,
    const ProgressCallback& progress,
    OperationResult* result
) const {
    cancellation.throw_if_requested();
    require_capability(source_provider, ProviderCapability::remove, "move source removal");
    require_capability(destination_provider, ProviderCapability::write, "move destination");
    if (!CopyMask(options.source_mask, options.shell_patterns).matches(source.name)) return;
    if (source_provider.id() == destination_provider.id() &&
        source_provider.normalize(source.path) ==
            destination_provider.normalize(destination)) {
        record_skipped(result, source, "source and destination are the same");
        return;
    }
    if (directory_destination(
            destination_provider,
            destination,
            cancellation
        )) {
        move(
            source_provider,
            {source},
            destination_provider,
            destination,
            options,
            cancellation,
            progress,
            result
        );
        return;
    }
    validate_move_target(
        source_provider, source, destination_provider,
        destination, cancellation
    );
    if (same_move_file(
            source_provider, source.path,
            destination_provider, destination
        )) {
        record_skipped(
            result, source,
            "source and destination are the same file"
        );
        return;
    }
    const auto collision = resolve_collision(
        source_provider, source, destination_provider, destination,
        options, cancellation
    );
    if (collision.decision == CollisionDecision::skip) {
        record_skipped(result, source, "destination exists: " + std::string(destination));
        return;
    }
    const bool overwrite = collision.destination.has_value();
    auto state = options.compute_totals
                     ? measure(source_provider, {source}, cancellation, static_cast<bool>(options.error))
                     : TransferProgress{};
    state.current_path = source.path;
    try {
        emit_progress(progress, state);
    } catch (const SkipCurrent&) {
        record_skipped(result, source, "user requested skip");
        return;
    }
    if (&source_provider == &destination_provider &&
        collision.decision == CollisionDecision::overwrite &&
        !(options.stable_symlinks && options.preserve_symlinks && source.kind == EntryKind::symlink) &&
        has_capability(
            source_provider.capabilities(),
            ProviderCapability::rename
        )) {
        std::string rename_skip;
        const auto renamed = rename_with_retry(
            source_provider, source, destination_provider, destination,
            overwrite, options, cancellation, rename_skip
        );
        if (renamed == RenameOutcome::skipped) {
            record_skipped(result, source, std::move(rename_skip));
            return;
        }
        if (renamed == RenameOutcome::renamed) {
            if (same_move_file(
                    source_provider, source.path,
                    destination_provider, destination
                )) {
                record_skipped(
                    result, source,
                    "source and destination are the same file"
                );
                return;
            }
            record_completed(result, source);
            state.bytes_done = state.bytes_total;
            state.files_done = state.files_total;
            try {
                emit_progress(progress, state);
            } catch (const SkipCurrent&) {
            }
            return;
        }
    }
    validate_move_target(
        source_provider, source, destination_provider,
        destination, cancellation
    );
    if (same_move_file(
            source_provider, source.path,
            destination_provider, destination
        )) {
        record_skipped(
            result, source,
            "source and destination are the same file"
        );
        return;
    }
    bool copied = false;
    std::string skip_reason;
    auto transferred_source = source;
    auto transferred_decision = CollisionDecision::overwrite;
    try {
        copied = copy_entry(
            source_provider,
            source,
            destination_provider,
            destination,
            options,
            cancellation,
            state,
            progress,
            skip_reason,
            collision,
            {},
            {},
            &transferred_source,
            &transferred_decision
        );
    } catch (const SkipCurrent&) {
        record_skipped(result, source, "user requested skip");
        return;
    }
    if (copied) {
        if (remove_with_retry(
                source_provider, source, cancellation, {}, options.error,
                [&] {
                    if (transferred_decision != CollisionDecision::overwrite ||
                        (options.stable_symlinks && source.kind == EntryKind::symlink)) {
                        validate_move_source(source_provider, transferred_source, cancellation);
                    }
                    validate_move_target(
                        source_provider, source, destination_provider,
                        destination, cancellation
                    );
                }
            )) {
            record_completed(result, source);
        } else {
            record_skipped(result, source, "source removal skipped after copy");
        }
    } else {
        record_skipped(result, source, std::move(skip_reason));
    }
}

}  // namespace mcpp
