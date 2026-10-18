#include "mcpp/viewer.hpp"

#include <algorithm>
#include <cctype>
#include <deque>
#include <iomanip>
#include <sstream>
#include <stdexcept>

#include <ftxui/screen/string.hpp>

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

std::size_t next_utf8(
    const std::string_view value,
    const std::size_t offset
) {
    if (offset >= value.size()) {
        return value.size();
    }
    auto next = offset + 1U;
    while (next < value.size() &&
           (
               static_cast<unsigned char>(value[next]) &
               0xC0U
           ) == 0x80U) {
        ++next;
    }
    return next;
}

}  // namespace

void ViewerModel::open(
    ProviderPtr provider,
    std::string path,
    const Cancellation& cancellation
) {
    provider_ = std::move(provider);
    path_ = std::move(path);
    const auto entry = provider_->stat_follow(path_, cancellation);
    if (entry.kind != EntryKind::file) {
        throw std::runtime_error("viewer requires a regular file");
    }
    storage_path_ = entry.path;
    size_ = entry.size;
    offset_ = 0;
    view_end_ = 0;
    previous_offsets_.clear();
    horizontal_offset_ = 0;
    last_search_.clear();
    next_search_offset_ = 0;
    load_window(0, window_size, cancellation);
}

void ViewerModel::close() {
    provider_.reset();
    path_.clear();
    storage_path_.clear();
    window_.clear();
    size_ = 0;
    offset_ = 0;
    view_end_ = 0;
    previous_offsets_.clear();
    horizontal_offset_ = 0;
    last_search_.clear();
    next_search_offset_ = 0;
}

bool ViewerModel::open() const noexcept {
    return provider_ != nullptr;
}

void ViewerModel::go_to_start() {
    offset_ = 0;
    view_end_ = 0;
    previous_offsets_.clear();
    horizontal_offset_ = 0;
}

void ViewerModel::go_to_end(
    const std::size_t viewport_width,
    const std::size_t viewport_lines
) {
    if (!open()) {
        return;
    }
    previous_offsets_.clear();
    horizontal_offset_ = 0;
    const auto height = std::max<std::size_t>(1U, viewport_lines);
    if (hex_) {
        const auto rows = size_ == 0U ? 0U : (size_ - 1U) / 16U + 1U;
        offset_ = (rows - std::min<std::uint64_t>(rows, height)) * 16U;
    } else {
        const auto offsets = text_offsets_before(size_, viewport_width, height);
        offset_ = offsets.empty() ? 0U : offsets.front();
    }
    view_end_ = offset_;
}

void ViewerModel::scroll_lines(
    const int amount,
    const std::size_t viewport_width
) {
    if (!open()) {
        return;
    }
    if (offset_ < window_offset_ ||
        offset_ >= window_offset_ + window_.size()) {
        load_window(offset_, window_size, {});
    }
    if (amount > 0) {
        for (int line = 0;
             line < amount && offset_ < size_;
             ++line) {
            auto next_offset = offset_;
            if (hex_) {
                next_offset =
                    std::min<std::uint64_t>(size_, offset_ + 16U);
            } else {
                next_offset = read_text_row(
                    offset_,
                    viewport_width,
                    nullptr,
                    {}
                );
            }
            if (next_offset <= offset_) {
                break;
            }
            previous_offsets_.push_back(offset_);
            offset_ = next_offset;
        }
    } else {
        for (int line = 0; line > amount && offset_ > 0U; --line) {
            if (hex_) {
                offset_ =
                    offset_ >= 16U ? offset_ - 16U : 0U;
                continue;
            }
            if (previous_offsets_.empty()) {
                previous_offsets_ = text_offsets_before(
                    offset_,
                    viewport_width,
                    static_cast<std::size_t>(
                        static_cast<long long>(line) - amount
                    )
                );
            }
            if (previous_offsets_.empty()) {
                offset_ = 0;
                break;
            }
            offset_ = previous_offsets_.back();
            previous_offsets_.pop_back();
        }
    }
}

void ViewerModel::page(
    const int direction,
    const std::size_t viewport_width,
    const std::size_t viewport_lines
) {
    scroll_lines(
        direction * static_cast<int>(viewport_lines),
        viewport_width
    );
}

void ViewerModel::scroll_columns(const int amount) {
    if (wrap_ || hex_ || amount == 0) {
        return;
    }
    const auto next =
        static_cast<long long>(horizontal_offset_) + amount;
    horizontal_offset_ = static_cast<std::size_t>(
        std::max<long long>(0, next)
    );
}

void ViewerModel::toggle_hex() {
    hex_ = !hex_;
    previous_offsets_.clear();
    horizontal_offset_ = 0;
}

void ViewerModel::set_wrap(const bool value) {
    wrap_ = value;
    previous_offsets_.clear();
    if (wrap_) {
        horizontal_offset_ = 0;
    }
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

std::uint64_t ViewerModel::view_end() const noexcept {
    return view_end_;
}

std::size_t ViewerModel::horizontal_offset() const noexcept {
    return horizontal_offset_;
}

const std::string& ViewerModel::path() const noexcept {
    return path_;
}

std::vector<std::uint64_t> ViewerModel::text_offsets_before(
    const std::uint64_t end,
    const std::size_t width,
    const std::size_t count
) {
    if (end == 0U || count == 0U) {
        return {};
    }
    std::uint64_t begin = 0;
    std::size_t newlines = 0;
    for (auto cursor = end; cursor > 0U;) {
        const auto chunk_begin = cursor > window_size ? cursor - window_size : 0U;
        load_window(
            chunk_begin,
            static_cast<std::size_t>(cursor - chunk_begin),
            {}
        );
        for (auto local = window_.size(); local > 0U; --local) {
            const auto position = chunk_begin + local - 1U;
            if (window_[local - 1U] == '\n' && position + 1U < end &&
                ++newlines == count) {
                begin = position + 1U;
                break;
            }
        }
        if (newlines == count) {
            break;
        }
        cursor = chunk_begin;
    }
    // Only the last viewport's row offsets are retained, even for huge lines.
    std::deque<std::uint64_t> offsets;
    for (auto cursor = begin; cursor < end;) {
        const auto next = read_text_row(cursor, width, nullptr, {});
        if (next <= cursor) {
            break;
        }
        offsets.push_back(cursor);
        if (offsets.size() > count) {
            offsets.pop_front();
        }
        cursor = std::min(next, end);
    }
    return {offsets.begin(), offsets.end()};
}

std::string_view ViewerModel::window_at(
    const std::uint64_t offset,
    const std::size_t minimum,
    const Cancellation& cancellation
) {
    if (offset >= size_) {
        return {};
    }
    const auto end = window_offset_ + window_.size();
    if (offset < window_offset_ || offset >= end ||
        end - offset <
            std::min<std::uint64_t>(minimum, size_ - offset)) {
        load_window(offset, window_size, cancellation);
    }
    if (offset < window_offset_ ||
        offset >= window_offset_ + window_.size()) {
        return {};
    }
    return std::string_view(window_).substr(
        static_cast<std::size_t>(offset - window_offset_)
    );
}

std::uint64_t ViewerModel::read_text_row(
    std::uint64_t offset,
    const std::size_t viewport_width,
    std::string* output,
    const Cancellation& cancellation
) {
    const auto width = std::max<std::size_t>(1U, viewport_width);
    std::size_t column = 0;
    std::size_t displayed_width = 0;
    bool clipped = false;
    while (offset < size_) {
        cancellation.throw_if_requested();
        // Keep a complete UTF-8 code point when refilling the bounded window.
        const auto bytes = window_at(offset, 4U, cancellation);
        if (bytes.empty()) {
            break;
        }
        if (!wrap_ && (output == nullptr || clipped)) {
            const auto newline = bytes.find('\n');
            if (newline != std::string_view::npos) {
                return offset + newline + 1U;
            }
            offset += bytes.size();
            continue;
        }
        if (bytes.front() == '\n') {
            return offset + 1U;
        }
        const auto length = next_utf8(bytes, 0);
        const auto glyph = bytes.substr(0, length);
        const bool tab = glyph == "\t";
        const auto first = static_cast<unsigned char>(glyph.front());
        if (!tab && (first < 32U || first == 127U)) {
            offset += length;
            continue;
        }
        auto glyph_width = static_cast<std::size_t>(
            std::max(0, ftxui::string_width(glyph))
        );
        if (tab) {
            constexpr std::size_t tab_width = 8U;
            glyph_width = tab_width - column % tab_width;
        }
        if (wrap_ && glyph_width > 0U && column > 0U &&
            column + glyph_width > width) {
            return offset;
        }
        const auto glyph_begin = column;
        column += glyph_width;
        offset += length;
        if (output == nullptr) {
            continue;
        }
        if (!wrap_) {
            if (column <= horizontal_offset_) {
                continue;
            }
            if (glyph_begin < horizontal_offset_) {
                if (!tab) {
                    continue;
                }
                glyph_width = column - horizontal_offset_;
            }
            if (displayed_width + glyph_width > width) {
                clipped = true;
                continue;
            }
        }
        if (tab) {
            output->append(glyph_width, ' ');
        } else if (glyph_width > 0U || !output->empty()) {
            output->append(glyph);
        }
        displayed_width += glyph_width;
    }
    return offset;
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
        storage_path_,
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
        view_end_ = 0;
        return {};
    }
    std::vector<std::string> result;
    result.reserve(height);
    view_end_ = offset_;
    if (hex_) {
        constexpr std::size_t bytes_per_line = 16U;
        for (std::size_t row = 0; row < height; ++row) {
            const auto bytes = window_at(
                view_end_,
                bytes_per_line,
                cancellation
            );
            if (bytes.empty()) {
                break;
            }
            const auto count = std::min(bytes_per_line, bytes.size());
            std::ostringstream line;
            line << std::hex << std::setfill('0') << std::setw(8)
                 << view_end_ << "  ";
            for (std::size_t index = 0; index < bytes_per_line; ++index) {
                if (index < count) {
                    line << std::setw(2)
                         << static_cast<unsigned int>(
                                static_cast<unsigned char>(
                                    bytes[index]
                                )
                            )
                         << ' ';
                } else {
                    line << "   ";
                }
            }
            line << " ";
            for (std::size_t index = 0; index < count; ++index) {
                const auto character = static_cast<unsigned char>(
                    bytes[index]
                );
                line << (std::isprint(character)
                             ? static_cast<char>(character)
                             : '.');
            }
            result.push_back(line.str());
            view_end_ += count;
        }
        return result;
    }

    while (result.size() < height && view_end_ < size_) {
        std::string line;
        const auto next = read_text_row(
            view_end_,
            width,
            &line,
            cancellation
        );
        if (next <= view_end_) {
            break;
        }
        result.push_back(std::move(line));
        view_end_ = next;
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
    const auto requested = std::string(needle);
    const auto sought =
        case_sensitive ? requested : lowercase(requested);
    const bool repeat =
        requested == last_search_ &&
        case_sensitive == last_search_case_sensitive_;
    const auto search_begin =
        repeat ? std::min(next_search_offset_, size_) : offset_;
    constexpr std::uint64_t chunk_size = 256U * 1024U;
    const auto search_range = [&](const std::uint64_t begin,
                                  const std::uint64_t end)
        -> std::optional<std::uint64_t> {
        std::string overlap;
        for (auto cursor = begin; cursor < end;
             cursor += chunk_size) {
            cancellation.throw_if_requested();
            std::ostringstream output;
            provider_->read(
                storage_path_,
                cursor,
                std::min(chunk_size, end - cursor),
                output,
                cancellation
            );
            auto chunk = overlap + std::move(output).str();
            const auto haystack =
                case_sensitive
                    ? chunk
                    : lowercase(std::move(chunk));
            const auto found = haystack.find(sought);
            if (found != std::string::npos) {
                return cursor - overlap.size() + found;
            }
            const auto keep = std::min(
                sought.size() - 1U,
                haystack.size()
            );
            overlap = haystack.substr(haystack.size() - keep);
        }
        return std::nullopt;
    };
    auto found = search_range(search_begin, size_);
    if (!found && search_begin > 0U) {
        found = search_range(0U, search_begin);
    }
    if (!found) {
        last_search_ = requested;
        last_search_case_sensitive_ = case_sensitive;
        next_search_offset_ = 0;
        return std::nullopt;
    }
    last_search_ = requested;
    last_search_case_sensitive_ = case_sensitive;
    next_search_offset_ = std::min<std::uint64_t>(
        size_,
        *found + std::max<std::size_t>(1U, requested.size())
    );
    const auto prefix_begin =
        *found > chunk_size ? *found - chunk_size : 0U;
    std::ostringstream prefix_output;
    provider_->read(
        storage_path_,
        prefix_begin,
        *found - prefix_begin,
        prefix_output,
        cancellation
    );
    const auto prefix = std::move(prefix_output).str();
    const auto newline = prefix.rfind('\n');
    offset_ =
        newline == std::string::npos
            ? prefix_begin
            : prefix_begin + newline + 1U;
    if (hex_) {
        offset_ = *found;
    }
    previous_offsets_.clear();
    horizontal_offset_ = 0;
    return *found;
}

}  // namespace mcpp
