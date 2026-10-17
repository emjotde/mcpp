#include "mcpp/viewer.hpp"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>

namespace mcpp {
namespace {

constexpr std::size_t window_size = 256U * 1024U;

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

}  // namespace

void ViewerModel::open(
    ProviderPtr provider,
    std::string path,
    const Cancellation& cancellation
) {
    provider_ = std::move(provider);
    path_ = std::move(path);
    const auto entry = provider_->stat(path_, cancellation);
    size_ = entry.size;
    offset_ = 0;
    load_window(0, window_size, cancellation);
}

void ViewerModel::close() {
    provider_.reset();
    path_.clear();
    window_.clear();
    size_ = 0;
    offset_ = 0;
}

bool ViewerModel::open() const noexcept {
    return provider_ != nullptr;
}

void ViewerModel::scroll_lines(
    const int amount,
    const std::size_t viewport_lines
) {
    if (!open()) {
        return;
    }
    if (hex_) {
        const auto next =
            static_cast<std::int64_t>(offset_) +
            static_cast<std::int64_t>(amount) * 16;
        offset_ = static_cast<std::uint64_t>(
            std::clamp<std::int64_t>(
                next,
                0,
                static_cast<std::int64_t>(size_)
            )
        );
        return;
    }
    if (offset_ < window_offset_ ||
        offset_ > window_offset_ + window_.size()) {
        load_window(offset_, window_size, {});
    }
    if (amount > 0) {
        for (int line = 0; line < amount && offset_ < size_; ++line) {
            const auto local = static_cast<std::size_t>(
                offset_ - window_offset_
            );
            const auto next = window_.find('\n', local);
            if (next == std::string::npos) {
                const auto new_offset =
                    std::min<std::uint64_t>(
                        size_,
                        window_offset_ + window_.size()
                    );
                if (new_offset == offset_) {
                    break;
                }
                offset_ = new_offset;
                load_window(offset_, window_size, {});
            } else {
                offset_ = window_offset_ + next + 1U;
            }
        }
    } else {
        for (int line = 0; line > amount && offset_ > 0U; --line) {
            if (offset_ <= window_offset_ + 1U) {
                const auto begin =
                    offset_ > window_size
                        ? offset_ - window_size
                        : 0U;
                load_window(begin, window_size, {});
            }
            const auto local = static_cast<std::size_t>(
                offset_ - window_offset_
            );
            const auto before =
                local > 1U
                    ? window_.rfind('\n', local - 2U)
                    : std::string::npos;
            offset_ =
                before == std::string::npos
                    ? window_offset_
                    : window_offset_ + before + 1U;
        }
    }
    static_cast<void>(viewport_lines);
}

void ViewerModel::page(
    const int direction,
    const std::size_t viewport_lines
) {
    scroll_lines(
        direction * static_cast<int>(viewport_lines),
        viewport_lines
    );
}

void ViewerModel::toggle_hex() {
    hex_ = !hex_;
}

void ViewerModel::set_wrap(const bool value) {
    wrap_ = value;
}

bool ViewerModel::wrap() const noexcept {
    return wrap_;
}

bool ViewerModel::hex() const noexcept {
    return hex_;
}

std::uint64_t ViewerModel::size() const noexcept {
    return size_;
}

std::uint64_t ViewerModel::offset() const noexcept {
    return offset_;
}

const std::string& ViewerModel::path() const noexcept {
    return path_;
}

void ViewerModel::load_window(
    const std::uint64_t offset,
    const std::size_t size,
    const Cancellation& cancellation
) {
    if (!provider_) {
        return;
    }
    std::ostringstream output;
    provider_->read(
        path_,
        offset,
        std::min<std::uint64_t>(size, size_ - offset),
        output,
        cancellation
    );
    window_ = std::move(output).str();
    window_offset_ = offset;
}

std::vector<std::string> ViewerModel::lines(
    const std::size_t width,
    const std::size_t height,
    const Cancellation& cancellation
) {
    if (!open()) {
        return {};
    }
    if (offset_ < window_offset_ ||
        offset_ >= window_offset_ + window_.size()) {
        load_window(offset_, window_size, cancellation);
    }
    const auto local = static_cast<std::size_t>(
        offset_ - window_offset_
    );
    std::vector<std::string> result;
    result.reserve(height);
    if (hex_) {
        constexpr std::size_t bytes_per_line = 16U;
        for (std::size_t row = 0; row < height; ++row) {
            const auto begin = local + row * bytes_per_line;
            if (begin >= window_.size()) {
                break;
            }
            std::ostringstream line;
            line << std::hex << std::setfill('0') << std::setw(8)
                 << (window_offset_ + begin) << "  ";
            for (std::size_t index = 0; index < bytes_per_line; ++index) {
                if (begin + index < window_.size()) {
                    line << std::setw(2)
                         << static_cast<unsigned int>(
                                static_cast<unsigned char>(
                                    window_[begin + index]
                                )
                            )
                         << ' ';
                } else {
                    line << "   ";
                }
            }
            line << " ";
            for (std::size_t index = 0; index < bytes_per_line &&
                                        begin + index < window_.size();
                 ++index) {
                const auto character = static_cast<unsigned char>(
                    window_[begin + index]
                );
                line << (std::isprint(character)
                             ? static_cast<char>(character)
                             : '.');
            }
            result.push_back(line.str());
        }
        return result;
    }

    std::istringstream input(window_.substr(local));
    std::string line;
    while (result.size() < height && std::getline(input, line)) {
        if (!wrap_ || width == 0U || line.size() <= width) {
            result.push_back(
                width == 0U ? line : line.substr(0, width)
            );
            continue;
        }
        for (std::size_t begin = 0; begin < line.size() &&
                                     result.size() < height;
             begin += width) {
            result.push_back(line.substr(begin, width));
        }
    }
    return result;
}

std::optional<std::uint64_t> ViewerModel::find(
    const std::string_view needle,
    const bool case_sensitive,
    const Cancellation& cancellation
) {
    if (!open() || needle.empty()) {
        return std::nullopt;
    }
    const auto sought =
        case_sensitive ? std::string(needle)
                       : lowercase(std::string(needle));
    constexpr std::uint64_t chunk_size = 256U * 1024U;
    std::string overlap;
    for (auto cursor = offset_; cursor < size_;
         cursor += chunk_size) {
        cancellation.throw_if_requested();
        std::ostringstream output;
        provider_->read(
            path_,
            cursor,
            std::min(chunk_size, size_ - cursor),
            output,
            cancellation
        );
        auto chunk = overlap + std::move(output).str();
        const auto haystack =
            case_sensitive ? chunk : lowercase(std::move(chunk));
        const auto found = haystack.find(sought);
        if (found != std::string::npos) {
            offset_ =
                cursor - overlap.size() + found;
            return offset_;
        }
        const auto keep = std::min(
            sought.size() > 0U ? sought.size() - 1U : 0U,
            haystack.size()
        );
        overlap = haystack.substr(haystack.size() - keep);
    }
    return std::nullopt;
}

}  // namespace mcpp
