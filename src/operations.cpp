#include "mcpp/operations.hpp"
#include "mcpp/local_provider.hpp"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <unistd.h>
#include <utility>

namespace mcpp {
namespace {

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
    const std::function<void()>& validate = {}
) {
    for (;;) {
        cancellation.throw_if_requested();
        try {
            if (validate) {
                validate();
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
    TransferProgress& result
) {
    cancellation.throw_if_requested();
    if (entry.kind == EntryKind::directory) {
        for (const auto& child :
             provider.list(
                 entry.path,
                 {.show_hidden = true},
                 cancellation
             )) {
            accumulate_entry(provider, child, cancellation, result);
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
    TransferProgress& result
) {
    cancellation.throw_if_requested();
    ++result.files_total;
    if (entry.kind != EntryKind::directory) {
        result.bytes_total += entry.size;
        return;
    }
    for (const auto& child :
         provider.list(
             entry.path,
             {.show_hidden = true},
             cancellation
         )) {
        accumulate_removal(
            provider,
            child,
            cancellation,
            result
        );
    }
}

bool should_overwrite(
    Provider& source_provider,
    const Entry& source,
    Provider& destination_provider,
    const std::string& destination,
    const OperationOptions& options,
    const Cancellation& cancellation
) {
    if (!destination_provider.exists(destination)) {
        return false;
    }
    if (options.overwrite) {
        return true;
    }
    if (!options.collision) {
        throw std::runtime_error(
            "destination exists: " + destination
        );
    }
    const auto destination_entry =
        destination_provider.stat(destination, cancellation);
    switch (options.collision(source, destination_entry)) {
        case CollisionDecision::overwrite:
            return true;
        case CollisionDecision::skip:
            return false;
        case CollisionDecision::cancel:
            throw OperationCancelled{};
    }
    static_cast<void>(source_provider);
    return false;
}

class DescriptorBuffer final : public std::streambuf {
public:
    explicit DescriptorBuffer(const int descriptor)
        : descriptor_(descriptor) {}

protected:
    std::streamsize xsputn(
        const char* data,
        const std::streamsize count
    ) override {
        std::streamsize offset = 0;
        while (offset < count) {
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

TransferProgress OperationEngine::measure(
    Provider& provider,
    const std::vector<Entry>& entries,
    const Cancellation& cancellation
) {
    TransferProgress result;
    for (const auto& entry : entries) {
        accumulate_entry(provider, entry, cancellation, result);
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
    std::string& skip_reason
) {
    const auto before = state;
    for (;;) {
        try {
            return copy_entry_once(
                source_provider, source, destination_provider, destination,
                options, cancellation, state, progress, skip_reason
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
    std::string& skip_reason
) {
    cancellation.throw_if_requested();
    const auto destination_path = std::string(destination);
    if (source_provider.id() == destination_provider.id() &&
        source_provider.normalize(source.path) ==
            destination_provider.normalize(destination_path)) {
        skip_reason = "source and destination are the same";
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
    const bool destination_exists =
        destination_provider.exists(destination_path);
    const bool overwrite =
        destination_exists
            ? should_overwrite(
                  source_provider,
                  source,
                  destination_provider,
                  destination_path,
                  options,
                  cancellation
              )
            : false;
    if (destination_exists && !overwrite) {
        skip_reason = "destination exists: " + destination_path;
        return false;
    }

    state.current_path = source.path;
    emit_progress(progress, state);
    validate_directory_target(
        source_provider, source.path, destination_provider, destination_path
    );

    if (&source_provider == &destination_provider &&
        options.preserve_permissions &&
        options.preserve_symlinks &&
        (source.kind != EntryKind::directory || !options.error) &&
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

    auto effective_source = source;
    if (source.kind == EntryKind::symlink &&
        !options.preserve_symlinks) {
        effective_source =
            source_provider.stat_follow(source.path, cancellation);
    }

    if (effective_source.kind == EntryKind::directory) {
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
                    skip_reason
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
        DescriptorBuffer buffer(temporary.descriptor());
        std::ostream output(&buffer);
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
        destination_provider.write(
            destination_path,
            input,
            effective_source.size,
            overwrite,
            std::nullopt,
            cancellation,
            {}
        );
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
    state.bytes_done += effective_source.size;
    ++state.files_done;
    emit_completed_progress(progress, state);
    return true;
}

void OperationEngine::copy(
    Provider& source_provider,
    const std::vector<Entry>& sources,
    Provider& destination_provider,
    const std::string_view destination_directory,
    const OperationOptions& options,
    const Cancellation& cancellation,
    const ProgressCallback& progress,
    OperationResult* result
) const {
    auto state = measure(source_provider, sources, cancellation);
    for (const auto& source : sources) {
        try {
            std::string skip_reason;
            const auto copied = copy_entry(
                source_provider,
                source,
                destination_provider,
                destination_provider.join(
                    destination_directory,
                    source.name
                ),
                options,
                cancellation,
                state,
                progress,
                skip_reason
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
    auto state = measure(source_provider, {source}, cancellation);
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
    const std::vector<Entry>& sources,
    Provider& destination_provider,
    const std::string_view destination_directory,
    const OperationOptions& options,
    const Cancellation& cancellation,
    const ProgressCallback& progress,
    OperationResult* result
) const {
    if (&source_provider == &destination_provider &&
        has_capability(
            source_provider.capabilities(),
            ProviderCapability::rename
        )) {
        auto state = measure(source_provider, sources, cancellation);
        for (const auto& source : sources) {
            cancellation.throw_if_requested();
            state.current_path = source.path;
            try {
                emit_progress(progress, state);
            } catch (const SkipCurrent&) {
                record_skipped(result, source, "user requested skip");
                continue;
            }
            const auto destination = destination_provider.join(
                destination_directory,
                source.name
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
            bool overwrite = options.overwrite;
            if (destination_provider.exists(destination) &&
                !overwrite) {
                if (!options.collision) {
                    throw std::runtime_error(
                        "destination exists: " + destination
                    );
                }
                switch (options.collision(
                    source,
                    destination_provider.stat(destination, cancellation)
                )) {
                    case CollisionDecision::overwrite:
                        overwrite = true;
                        break;
                    case CollisionDecision::skip:
                        record_skipped(
                            result,
                            source,
                            "destination exists: " + destination
                        );
                        continue;
                    case CollisionDecision::cancel:
                        throw OperationCancelled{};
                }
            }
            std::string rename_skip;
            const auto renamed = rename_with_retry(
                source_provider, source, destination_provider, destination,
                overwrite, options, cancellation, rename_skip
            );
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
                accumulate_entry(
                    destination_provider,
                    destination_provider.stat(
                        destination,
                        cancellation
                    ),
                    cancellation,
                    completed
                );
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
                const auto copied = copy_entry(
                    source_provider,
                    source,
                    destination_provider,
                    destination,
                    {
                        .overwrite = overwrite,
                        .preserve_permissions =
                            options.preserve_permissions,
                        .preserve_symlinks =
                            options.preserve_symlinks,
                        .collision = options.collision,
                        .error = options.error,
                    },
                    cancellation,
                    state,
                    progress,
                    skip_reason
                );
                if (copied) {
                    if (remove_with_retry(
                            source_provider, source, cancellation, {}, options.error,
                            [&] {
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
    auto state = measure(source_provider, sources, cancellation);
    for (const auto& source : sources) {
        try {
            const auto destination = destination_provider.join(
                destination_directory, source.name
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
            const auto copied = copy_entry(
                source_provider,
                source,
                destination_provider,
                destination,
                options,
                cancellation,
                state,
                progress,
                skip_reason
            );
            if (copied) {
                if (remove_with_retry(
                        source_provider, source, cancellation, {}, options.error,
                        [&] {
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
    const OperationErrorResolver& error
) const {
    TransferProgress state;
    for (const auto& entry : entries) {
        accumulate_removal(provider, entry, cancellation, state);
    }
    for (const auto& entry : entries) {
        cancellation.throw_if_requested();
        bool reported = false;
        const auto removed = remove_with_retry(
            provider, entry, cancellation,
            [&](const TransferProgress& current) {
                reported = true;
                ++state.files_done;
                state.bytes_done += current.bytes_done;
                state.current_path = current.current_path;
                emit_progress(progress, state);
            },
            error
        );
        if (!removed) {
            record_skipped(result, entry, "user skipped deletion after error");
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
    bool overwrite = options.overwrite;
    if (destination_provider.exists(destination) && !overwrite) {
        if (!options.collision) {
            throw std::runtime_error(
                "destination exists: " + std::string(destination)
            );
        }
        switch (options.collision(
            source,
            destination_provider.stat(destination, cancellation)
        )) {
            case CollisionDecision::overwrite:
                overwrite = true;
                break;
            case CollisionDecision::skip:
                record_skipped(
                    result,
                    source,
                    "destination exists: " + std::string(destination)
                );
                return;
            case CollisionDecision::cancel:
                throw OperationCancelled{};
        }
    }
    auto state = measure(source_provider, {source}, cancellation);
    state.current_path = source.path;
    try {
        emit_progress(progress, state);
    } catch (const SkipCurrent&) {
        record_skipped(result, source, "user requested skip");
        return;
    }
    if (&source_provider == &destination_provider &&
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
    try {
        copied = copy_entry(
            source_provider,
            source,
            destination_provider,
            destination,
            {
                .overwrite = overwrite,
                .preserve_permissions = options.preserve_permissions,
                .preserve_symlinks = options.preserve_symlinks,
                .collision = options.collision,
                .error = options.error,
            },
            cancellation,
            state,
            progress,
            skip_reason
        );
    } catch (const SkipCurrent&) {
        record_skipped(result, source, "user requested skip");
        return;
    }
    if (copied) {
        if (remove_with_retry(
                source_provider, source, cancellation, {}, options.error,
                [&] {
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
