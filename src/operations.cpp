#include "mcpp/operations.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unistd.h>

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

std::filesystem::path temporary_transfer_path() {
    const auto base = std::filesystem::temp_directory_path();
    for (unsigned int attempt = 0; attempt < 100U; ++attempt) {
        const auto candidate =
            base /
            (
                "mcpp-transfer-" + std::to_string(getpid()) + "-" +
                std::to_string(attempt)
            );
        std::error_code error;
        if (!std::filesystem::exists(candidate, error)) {
            return candidate;
        }
    }
    throw std::runtime_error("cannot allocate transfer temporary file");
}

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

void OperationEngine::copy_entry(
    Provider& source_provider,
    const Entry& source,
    Provider& destination_provider,
    const std::string_view destination,
    const OperationOptions& options,
    const Cancellation& cancellation,
    TransferProgress& state,
    const ProgressCallback& progress
) {
    cancellation.throw_if_requested();
    const auto destination_path = std::string(destination);
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
        return;
    }

    state.current_path = source.path;
    emit_progress(progress, state);

    if (&source_provider == &destination_provider &&
        has_capability(
            source_provider.capabilities(),
            ProviderCapability::direct_copy
        )) {
        source_provider.copy_direct(
            source.path,
            destination_path,
            overwrite,
            cancellation,
            {}
        );
        if (source.kind != EntryKind::directory) {
            state.bytes_done += source.size;
            ++state.files_done;
        }
        emit_progress(progress, state);
        return;
    }

    if (source.kind == EntryKind::directory) {
        if (!destination_exists) {
            destination_provider.create_directory(destination_path);
        }
        for (const auto& child :
             source_provider.list(
                 source.path,
                 {.show_hidden = true},
                 cancellation
             )) {
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
                progress
            );
        }
        return;
    }

    if (source.kind == EntryKind::symlink &&
        options.preserve_symlinks &&
        has_capability(
            destination_provider.capabilities(),
            ProviderCapability::links
        )) {
        destination_provider.create_symlink(
            source.target,
            destination_path
        );
    } else {
        const auto temporary = temporary_transfer_path();
        try {
            {
                std::ofstream output(
                    temporary,
                    std::ios::binary | std::ios::trunc
                );
                if (!output) {
                    throw std::runtime_error(
                        "cannot create transfer temporary file"
                    );
                }
                source_provider.read(
                    source.path,
                    0,
                    source.size,
                    output,
                    cancellation
                );
            }
            std::ifstream input(temporary, std::ios::binary);
            destination_provider.write(
                destination_path,
                input,
                source.size,
                overwrite,
                std::nullopt,
                cancellation,
                {}
            );
            std::filesystem::remove(temporary);
        } catch (...) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            throw;
        }
    }

    if (options.preserve_permissions &&
        has_capability(
            destination_provider.capabilities(),
            ProviderCapability::permissions
        ) &&
        source.permissions != std::filesystem::perms::unknown) {
        destination_provider.set_permissions(
            destination_path,
            source.permissions
        );
    }
    state.bytes_done += source.size;
    ++state.files_done;
    emit_progress(progress, state);
}

void OperationEngine::copy(
    Provider& source_provider,
    const std::vector<Entry>& sources,
    Provider& destination_provider,
    const std::string_view destination_directory,
    const OperationOptions& options,
    const Cancellation& cancellation,
    const ProgressCallback& progress
) const {
    auto state = measure(source_provider, sources, cancellation);
    emit_progress(progress, state);
    for (const auto& source : sources) {
        copy_entry(
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
            progress
        );
    }
}

void OperationEngine::move(
    Provider& source_provider,
    const std::vector<Entry>& sources,
    Provider& destination_provider,
    const std::string_view destination_directory,
    const OperationOptions& options,
    const Cancellation& cancellation,
    const ProgressCallback& progress
) const {
    if (&source_provider == &destination_provider &&
        has_capability(
            source_provider.capabilities(),
            ProviderCapability::rename
        )) {
        for (const auto& source : sources) {
            cancellation.throw_if_requested();
            source_provider.rename(
                source.path,
                destination_provider.join(
                    destination_directory,
                    source.name
                ),
                options.overwrite
            );
        }
        return;
    }
    copy(
        source_provider,
        sources,
        destination_provider,
        destination_directory,
        options,
        cancellation,
        progress
    );
    remove(
        source_provider,
        sources,
        cancellation,
        progress
    );
}

void OperationEngine::remove(
    Provider& provider,
    const std::vector<Entry>& entries,
    const Cancellation& cancellation,
    const ProgressCallback& progress
) const {
    TransferProgress state;
    state.files_total = entries.size();
    for (const auto& entry : entries) {
        cancellation.throw_if_requested();
        provider.remove(
            entry.path,
            entry.kind == EntryKind::directory,
            cancellation,
            {}
        );
        ++state.files_done;
        state.current_path = entry.path;
        emit_progress(progress, state);
    }
}

}  // namespace mcpp
