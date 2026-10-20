#include "extension_view_checkout.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace mcpp {

ExtensionViewCheckout::ExtensionViewCheckout(
    ProviderPtr provider, const Entry& source, const Cancellation& cancellation
) : provider_(std::move(provider)), source_(source) {
    cancellation.throw_if_requested();
    if (!provider_ || source_.path.empty() || source_.path.find('\0') != std::string::npos) {
        throw std::invalid_argument("extension view checkout needs a captured provider and source path");
    }
    if (source_.kind != EntryKind::file) {
        throw std::runtime_error("extension view checkout requires a regular source file");
    }
    if (source_.revision.empty()) {
        throw std::runtime_error("extension view checkout requires a captured source revision");
    }
    if (!has_capability(provider_->capabilities(), ProviderCapability::read)) {
        throw std::runtime_error("extension view checkout source is not readable");
    }
    const auto name = provider_->basename(source_.path);
    if (name.empty() || name == "." || name == ".." ||
        name.find('/') != std::string::npos || name.find('\0') != std::string::npos) {
        throw std::invalid_argument("extension view checkout needs a regular basename");
    }
    validate_source(cancellation);

    auto pattern = (std::filesystem::absolute(std::filesystem::temp_directory_path()) /
                    "mcpp-view-XXXXXX").string();
    std::vector<char> directory_name(pattern.begin(), pattern.end());
    directory_name.push_back('\0');
    try {
        cancellation.throw_if_requested();
        if (mkdtemp(directory_name.data()) == nullptr) {
            throw std::system_error(errno, std::generic_category(), "create extension view checkout directory");
        }
        directory_ = directory_name.data();
        if (chmod(directory_.c_str(), 0700) != 0) {
            throw std::system_error(errno, std::generic_category(), "protect extension view checkout directory");
        }
        path_ = directory_ / name;
        const int descriptor = open(path_.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (descriptor < 0) {
            throw std::system_error(errno, std::generic_category(), "create extension view checkout file");
        }
        const auto protected_file = fchmod(descriptor, 0600);
        const auto permissions_error = errno;
        const auto closed = close(descriptor);
        const auto close_error = errno;
        if (protected_file != 0) {
            throw std::system_error(permissions_error, std::generic_category(), "protect extension view checkout file");
        }
        if (closed != 0) {
            throw std::system_error(close_error, std::generic_category(), "close extension view checkout file");
        }
        std::ofstream output;
        output.exceptions(std::ios::badbit | std::ios::failbit);
        output.open(path_, std::ios::binary | std::ios::trunc);
        constexpr std::uint64_t chunk_size = 64U * 1024U;
        for (std::uint64_t offset = 0; offset < source_.size;) {
            cancellation.throw_if_requested();
            const auto length = std::min(chunk_size, source_.size - offset);
            provider_->read(source_.path, offset, length, output, cancellation);
            cancellation.throw_if_requested();
            offset += length;
            const auto position = static_cast<std::streamoff>(output.tellp());
            if (position < 0 || static_cast<std::uint64_t>(position) != offset) {
                throw std::runtime_error("extension view checkout source returned an incomplete or oversized read");
            }
        }
        output.close();
        if (std::filesystem::file_size(path_) != source_.size) {
            throw std::runtime_error("extension view checkout size does not match the captured source");
        }
        validate_source(cancellation);
    } catch (...) {
        cleanup();
        throw;
    }
}

ExtensionViewCheckout::~ExtensionViewCheckout() {
    cleanup();
}

void ExtensionViewCheckout::validate_source(const Cancellation& cancellation) const {
    cancellation.throw_if_requested();
    const auto current = provider_->stat(source_.path, cancellation);
    cancellation.throw_if_requested();
    if (current.path != source_.path || current.kind != EntryKind::file ||
        current.revision != source_.revision || current.size != source_.size) {
        throw std::runtime_error("extension view checkout source changed since it was captured");
    }
}

void ExtensionViewCheckout::cleanup() noexcept {
    if (directory_.empty()) return;
    std::error_code error;
    std::filesystem::remove_all(directory_, error);
    if (error) std::cerr << "mcpp: extension view checkout cleanup failed: " << error.message() << '\n';
}

}  // namespace mcpp
