#include "mcpp/viewer.hpp"
#include "mcpp/editor_search.hpp"
#include "mcpp/local_provider.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <deque>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <streambuf>

#include <ftxui/screen/string.hpp>

namespace mcpp {
namespace {

constexpr std::size_t window_size = 256U * 1024U;

class EditedInput final : public std::streambuf {
public:
    EditedInput(
        Provider& provider, std::string path, const std::uint64_t size,
        const std::map<std::uint64_t, unsigned char>& edits,
        const Cancellation& cancellation
    ) : provider_(provider), path_(std::move(path)), size_(size),
        edits_(edits), cancellation_(cancellation) {}

protected:
    int_type underflow() override {
        if (gptr() != nullptr && gptr() < egptr()) {
            return traits_type::to_int_type(*gptr());
        }
        if (offset_ >= size_) {
            return traits_type::eof();
        }
        cancellation_.throw_if_requested();
        const auto count = std::min<std::uint64_t>(data_.size(), size_ - offset_);
        std::ostringstream bytes;
        provider_.read(path_, offset_, count, bytes, cancellation_);
        const auto value = std::move(bytes).str();
        if (value.size() != count) {
            throw std::runtime_error("file changed while saving hex edits");
        }
        std::memcpy(data_.data(), value.data(), value.size());
        for (auto edit = edits_.lower_bound(offset_);
             edit != edits_.end() && edit->first < offset_ + count; ++edit) {
            data_[static_cast<std::size_t>(edit->first - offset_)] =
                static_cast<char>(edit->second);
        }
        offset_ += count;
        setg(data_.data(), data_.data(), data_.data() + value.size());
        return traits_type::to_int_type(*gptr());
    }

private:
    Provider& provider_;
    std::string path_;
    std::uint64_t size_;
    const std::map<std::uint64_t, unsigned char>& edits_;
    Cancellation cancellation_;
    std::array<char, 64U * 1024U> data_{};
    std::uint64_t offset_{0};
};

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
    if (!provider) {
        throw std::invalid_argument("viewer provider is null");
    }
    const auto entry = provider->stat_follow(path, cancellation);
    if (entry.kind != EntryKind::file) {
        throw std::runtime_error("viewer requires a regular file");
    }
    std::ostringstream initial;
    provider->read(entry.path, 0, std::min<std::uint64_t>(window_size, entry.size), initial, cancellation);
    provider_ = std::move(provider);
    path_ = std::move(path);
    storage_path_ = entry.path;
    size_ = entry.size;
    offset_ = 0;
    view_end_ = 0;
    previous_offsets_.clear();
    horizontal_offset_ = 0;
    last_search_.clear();
    next_search_offset_ = 0;
    search_begin_ = 0;
    hex_cursor_ = 0;
    hex_editing_ = false;
    hex_ascii_ = false;
    hex_low_nibble_ = false;
    edits_.clear();
    revision_ = entry.revision;
    window_ = std::move(initial).str();
    window_offset_ = 0;
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
    search_begin_ = 0;
    hex_cursor_ = 0;
    hex_editing_ = false;
    hex_ascii_ = false;
    hex_low_nibble_ = false;
    edits_.clear();
    revision_.clear();
}

bool ViewerModel::open() const noexcept {
    return provider_ != nullptr;
}

void ViewerModel::go_to_start() {
    offset_ = 0;
    view_end_ = 0;
    previous_offsets_.clear();
    horizontal_offset_ = 0;
    last_search_.clear();
    next_search_offset_ = 0;
    search_begin_ = 0;
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
    const auto anchor = offset_;
    hex_ = !hex_;
    if (!hex_) {
        hex_editing_ = false;
    } else {
        hex_cursor_ = size_ == 0U ? 0U : std::min(anchor, size_ - 1U);
        offset_ = hex_cursor_ / 16U * 16U;
    }
    previous_offsets_.clear();
    horizontal_offset_ = 0;
}

void ViewerModel::goto_offset(const std::uint64_t requested) {
    if (!open()) {
        throw std::runtime_error("viewer is not open");
    }
    hex_cursor_ = size_ == 0U ? 0U : std::min(requested, size_ - 1U);
    offset_ = hex_ ? hex_cursor_ / 16U * 16U : std::min(requested, size_);
    previous_offsets_.clear();
    horizontal_offset_ = 0;
    hex_low_nibble_ = false;
    next_search_offset_ = offset_;
}

void ViewerModel::toggle_hex_editing() {
    if (!open()) {
        throw std::runtime_error("viewer is not open");
    }
    hex_ = true;
    hex_editing_ = !hex_editing_;
    hex_cursor_ = size_ == 0U ? 0U : std::min(std::max(hex_cursor_, offset_), size_ - 1U);
    hex_low_nibble_ = false;
}

bool ViewerModel::hex_editing() const noexcept { return hex_editing_; }
bool ViewerModel::hex_ascii() const noexcept { return hex_ascii_; }
std::uint64_t ViewerModel::hex_cursor() const noexcept { return hex_cursor_; }
bool ViewerModel::dirty() const noexcept { return !edits_.empty(); }
bool ViewerModel::search_can_wrap() const noexcept { return search_begin_ > 0U; }

void ViewerModel::toggle_hex_navigation() {
    hex_ascii_ = !hex_ascii_;
    hex_low_nibble_ = false;
}

void ViewerModel::move_hex_cursor(const long long delta) {
    if (size_ == 0U) {
        return;
    }
    if (delta < 0) {
        const auto distance = static_cast<std::uint64_t>(-(delta + 1LL)) + 1U;
        hex_cursor_ -= std::min(hex_cursor_, distance);
    } else {
        hex_cursor_ += std::min(size_ - 1U - hex_cursor_, static_cast<std::uint64_t>(delta));
    }
    const auto row = hex_cursor_ / 16U;
    if (hex_cursor_ < offset_) {
        offset_ = row * 16U;
    } else if (row >= offset_ / 16U + hex_view_rows_) {
        offset_ = (row - hex_view_rows_ + 1U) * 16U;
    }
    hex_low_nibble_ = false;
}

void ViewerModel::set_byte(const std::uint64_t offset, const unsigned char value) {
    if (!hex_editing_ || offset >= size_) {
        throw std::runtime_error("no editable byte at the hex cursor");
    }
    std::ostringstream bytes;
    provider_->read(storage_path_, offset, 1U, bytes, {});
    const auto original = std::move(bytes).str();
    if (original.size() != 1U) {
        throw std::runtime_error("file changed while editing hex data");
    }
    if (static_cast<unsigned char>(original[0]) == value) {
        edits_.erase(offset);
    } else {
        edits_[offset] = value;
    }
    if (offset >= window_offset_ && offset - window_offset_ < window_.size()) {
        window_[static_cast<std::size_t>(offset - window_offset_)] = static_cast<char>(value);
    }
}

void ViewerModel::edit_hex_digit(const char digit) {
    const auto lowered = static_cast<char>(std::tolower(static_cast<unsigned char>(digit)));
    if (!((lowered >= '0' && lowered <= '9') || (lowered >= 'a' && lowered <= 'f'))) {
        throw std::invalid_argument("hex editing requires a hexadecimal digit");
    }
    if (size_ == 0U) {
        throw std::runtime_error("cannot overwrite bytes in an empty file");
    }
    const auto bytes = window_at(hex_cursor_, 1U, {});
    if (bytes.empty()) {
        throw std::runtime_error("hex cursor is outside the file");
    }
    const auto current = static_cast<unsigned char>(bytes[0]);
    const auto nibble = static_cast<unsigned int>(
        lowered <= '9' ? lowered - '0' : lowered - 'a' + 10
    );
    const auto value = static_cast<unsigned char>(
        hex_low_nibble_ ? (current & 0xF0U) | nibble
                        : (current & 0x0FU) | (nibble << 4U)
    );
    set_byte(hex_cursor_, value);
    if (hex_low_nibble_) {
        move_hex_cursor(1);
    } else {
        hex_low_nibble_ = true;
    }
}

void ViewerModel::edit_ascii_byte(const char byte) {
    set_byte(hex_cursor_, static_cast<unsigned char>(byte));
    move_hex_cursor(1);
}

void ViewerModel::save_changes(const Cancellation& cancellation) {
    if (!dirty()) {
        return;
    }
    if (const auto* local = dynamic_cast<const LocalProvider*>(provider_.get())) {
        local->ensure_writable(storage_path_);
    }
    EditedInput buffer(*provider_, storage_path_, size_, edits_, cancellation);
    std::istream input(&buffer);
    input.exceptions(std::ios::badbit);
    provider_->write(storage_path_, input, size_, true, revision_, cancellation, {});
    revision_ = provider_->stat(storage_path_, cancellation).revision;
    edits_.clear();
    window_.clear();
}

void ViewerModel::discard_changes() {
    edits_.clear();
    window_.clear();
    hex_low_nibble_ = false;
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
    for (auto edit = edits_.lower_bound(offset);
         edit != edits_.end() && edit->first - offset < window_.size(); ++edit) {
        window_[static_cast<std::size_t>(edit->first - offset)] = static_cast<char>(edit->second);
    }
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
    hex_view_rows_ = std::max<std::size_t>(1U, height);
    view_end_ = offset_;
    if (hex_) {
        hex_view_rows_ = std::max<std::size_t>(1U, height);
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
    const Cancellation& cancellation,
    const bool wrap
) {
    if (!open() || needle.empty()) {
        return std::nullopt;
    }
    const auto requested = std::string(needle);
    const bool repeat =
        requested == last_search_ &&
        case_sensitive == last_search_case_sensitive_;
    const auto search_begin =
        repeat ? std::min(next_search_offset_, size_) : offset_;
    search_begin_ = search_begin;
    constexpr std::uint64_t chunk_size = 256U * 1024U;
    const auto search_range = [&](const std::uint64_t begin,
                                  const std::uint64_t end)
        -> std::optional<std::pair<std::uint64_t, std::uint64_t>> {
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
            auto value = std::move(output).str();
            for (auto edit = edits_.lower_bound(cursor);
                 edit != edits_.end() && edit->first - cursor < value.size(); ++edit) {
                value[static_cast<std::size_t>(edit->first - cursor)] = static_cast<char>(edit->second);
            }
            auto chunk = overlap + value;
            const auto found = EditorSearch::find(
                chunk, requested, 0, 0, chunk.size(),
                {.mode = EditorSearchMode::normal, .case_sensitive = case_sensitive},
                false, false
            );
            if (found) {
                return std::pair{
                    cursor - overlap.size() + found->begin,
                    cursor - overlap.size() + found->end
                };
            }
            const auto keep = std::min(
                requested.size() > chunk.size() / 4U ? chunk.size() : requested.size() * 4U + 4U,
                chunk.size()
            );
            auto begin_overlap = chunk.size() - keep;
            while (begin_overlap > 0U &&
                   (static_cast<unsigned char>(chunk[begin_overlap]) & 0xC0U) == 0x80U) {
                --begin_overlap;
            }
            overlap = chunk.substr(begin_overlap);
        }
        return std::nullopt;
    };
    auto found = search_range(search_begin, size_);
    if (!found && wrap && search_begin > 0U) {
        found = search_range(0U, search_begin);
    }
    if (!found) {
        last_search_ = requested;
        last_search_case_sensitive_ = case_sensitive;
        next_search_offset_ = search_begin;
        return std::nullopt;
    }
    last_search_ = requested;
    last_search_case_sensitive_ = case_sensitive;
    next_search_offset_ = found->second;
    const auto prefix_begin =
        found->first > chunk_size ? found->first - chunk_size : 0U;
    std::ostringstream prefix_output;
    provider_->read(
        storage_path_,
        prefix_begin,
        found->first - prefix_begin,
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
        hex_cursor_ = found->first;
        offset_ = found->first / 16U * 16U;
    }
    previous_offsets_.clear();
    horizontal_offset_ = 0;
    return found->first;
}

}  // namespace mcpp
