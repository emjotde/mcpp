#include "mcpp/viewer.hpp"
#include "mcpp/editor_search.hpp"
#include "mcpp/local_provider.hpp"
#include "mcpp/transform_provider.hpp"
#include "viewer_data.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <iomanip>
#include <limits>
#include <linux/magic.h>
#include <mutex>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/vfs.h>
#include <system_error>
#include <thread>
#include <unistd.h>

#include <ftxui/screen/string.hpp>

namespace mcpp {
namespace {

constexpr std::size_t window_size = 256U * 1024U;

class Descriptor {
public:
    explicit Descriptor(const int value = -1) : value_(value) {}
    ~Descriptor() { if (value_ >= 0) ::close(value_); }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    Descriptor(Descriptor&& other) noexcept : value_(other.release()) {}
    [[nodiscard]] int get() const noexcept { return value_; }
    int release() noexcept { return std::exchange(value_, -1); }
private:
    int value_;
};

bool unreported_size(const int descriptor, const struct stat& status) {
    struct statfs filesystem{};
    if (::fstatfs(descriptor, &filesystem) == 0 &&
        (filesystem.f_type == PROC_SUPER_MAGIC || filesystem.f_type == SYSFS_MAGIC ||
         filesystem.f_type == DEBUGFS_MAGIC || filesystem.f_type == TRACEFS_MAGIC)) return true;
    if (status.st_size != 0) return false;
    return ::lseek(descriptor, 0, SEEK_END) < 0;
}

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

struct ViewerModel::StreamSource {
    Descriptor input;
    Descriptor storage;
    std::atomic<std::uint64_t> available{0};
    std::atomic<bool> finished{false};
    bool reported_finished{false};
    std::mutex error_mutex;
    std::string error;
    std::jthread worker;

    StreamSource(Descriptor descriptor, Cancellation cancellation)
        : input(std::move(descriptor)), storage(viewer_detail::private_spool()),
          worker([this, cancellation = std::move(cancellation)](const std::stop_token stop) {
              try {
                  std::array<char, 64U * 1024U> buffer{};
                  while (!stop.stop_requested() && !cancellation.requested()) {
                      pollfd item{input.get(), POLLIN, 0};
                      const auto ready = ::poll(&item, 1, 100);
                      if (ready < 0 && errno == EINTR) continue;
                      if (ready < 0) throw std::system_error(errno, std::generic_category(), "poll viewer input");
                      if (ready == 0) continue;
                      if (stop.stop_requested() || cancellation.requested()) break;
                      if ((item.revents & POLLNVAL) != 0) throw std::runtime_error("viewer input descriptor is no longer valid");
                      const auto count = ::read(input.get(), buffer.data(), buffer.size());
                      if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
                      if (count < 0) throw std::system_error(errno, std::generic_category(), "read viewer input");
                      if (count == 0) break;
                      std::size_t position = 0;
                      while (position < static_cast<std::size_t>(count)) {
                          if (stop.stop_requested() || cancellation.requested()) break;
                          const auto offset = available.load(std::memory_order_relaxed);
                          if (offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) -
                                           (static_cast<std::size_t>(count) - position)) {
                              throw std::runtime_error("viewer input exceeds the seekable storage limit");
                          }
                          const auto written = ::pwrite(storage.get(), buffer.data() + position,
                              static_cast<std::size_t>(count) - position, static_cast<off_t>(offset));
                          if (written < 0 && errno == EINTR) continue;
                          if (written <= 0) throw std::system_error(errno, std::generic_category(), "write viewer storage");
                          position += static_cast<std::size_t>(written);
                          available.store(offset + static_cast<std::uint64_t>(written), std::memory_order_release);
                      }
                  }
                  if (cancellation.requested() && !stop.stop_requested()) throw OperationCancelled{};
              } catch (const std::exception& failure) {
                  std::lock_guard lock(error_mutex);
                  error = failure.what();
              }
              finished.store(true, std::memory_order_release);
          }) {}

    ~StreamSource() {
        worker.request_stop();
        if (worker.joinable()) worker.join();
    }

    [[nodiscard]] std::string failure() {
        std::lock_guard lock(error_mutex);
        return error;
    }

};

void ViewerModel::open(
    ProviderPtr provider,
    std::string path,
    const Cancellation& cancellation
) {
    if (!provider) {
        throw std::invalid_argument("viewer provider is null");
    }
    const auto entry = provider->stat_follow(path, cancellation);
    const auto* local = dynamic_cast<LocalProvider*>(provider.get());
    if (local != nullptr) {
        Descriptor input(::open(local->native_path(entry.path).c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC));
        if (input.get() < 0) throw std::system_error(errno, std::generic_category(), "open viewer source");
        struct stat status{};
        if (::fstat(input.get(), &status) != 0) throw std::system_error(errno, std::generic_category(), "inspect viewer source");
        if (S_ISFIFO(status.st_mode) || (S_ISREG(status.st_mode) && unreported_size(input.get(), status))) {
            open_stream(std::move(provider), input.get(), std::move(path), cancellation);
            return;
        }
        if (!S_ISREG(status.st_mode)) throw std::runtime_error("viewer requires a regular file or a readable pipe");
    } else if (entry.kind != EntryKind::file) {
        throw std::runtime_error("viewer requires a regular file");
    }
    ViewerModel next;
    next.provider_ = std::move(provider);
    next.path_ = std::move(path);
    next.storage_path_ = entry.path;
    next.source_size_ = next.size_ = entry.size;
    next.revision_ = entry.revision;
    next.hex_ = hex_;
    next.wrap_ = wrap_;
    next.refresh_presentation(magic_, nroff_, encoding_, cancellation);
    *this = std::move(next);
}

void ViewerModel::open_stream(
    ProviderPtr provider, const int descriptor, std::string name,
    const Cancellation& cancellation
) {
    cancellation.throw_if_requested();
    if (!provider || dynamic_cast<LocalProvider*>(provider.get()) == nullptr) {
        throw std::invalid_argument("stream viewing requires a local provider");
    }
    if (::isatty(descriptor) != 0) throw std::runtime_error("viewer data input must not be the terminal keyboard");
    struct stat status{};
    if (::fstat(descriptor, &status) != 0) throw std::system_error(errno, std::generic_category(), "inspect viewer input");
    if (!S_ISREG(status.st_mode) && !S_ISFIFO(status.st_mode) &&
        !(S_ISCHR(status.st_mode) && status.st_rdev == makedev(1, 3))) {
        throw std::runtime_error("viewer input must be a regular file, pipe, or /dev/null");
    }
    Descriptor input(::fcntl(descriptor, F_DUPFD_CLOEXEC, 3));
    if (input.get() < 0) throw std::system_error(errno, std::generic_category(), "duplicate viewer input");
    ViewerModel next;
    next.provider_ = std::move(provider);
    next.path_ = std::move(name);
    next.stream_ = std::make_shared<StreamSource>(std::move(input), cancellation);
    next.hex_ = hex_;
    next.wrap_ = wrap_;
    next.magic_ = magic_;
    *this = std::move(next);
}

bool ViewerModel::reading() const noexcept {
    return stream_ != nullptr && !stream_->finished.load(std::memory_order_acquire);
}

bool ViewerModel::refresh(const Cancellation& cancellation) {
    cancellation.throw_if_requested();
    if (!open()) return false;
    std::uint64_t next_size = source_size_;
    std::string next_revision = revision_;
    std::string next_path = storage_path_;
    std::string error;
    bool completed = false;
    if (stream_) {
        next_size = stream_->available.load(std::memory_order_acquire);
        error = stream_->failure();
        const auto finished = stream_->finished.load(std::memory_order_acquire);
        completed = finished != stream_->reported_finished;
        stream_->reported_finished = finished;
    } else if (dynamic_cast<LocalProvider*>(provider_.get()) != nullptr) {
        try {
            const auto entry = provider_->stat_follow(path_, cancellation);
            if (entry.kind != EntryKind::file) throw std::runtime_error("viewer source is no longer a regular file");
            next_size = entry.size;
            next_revision = entry.revision;
            next_path = entry.path;
        } catch (const OperationCancelled&) {
            throw;
        } catch (const std::exception& failure) {
            error = failure.what();
        }
    } else return false;
    if (dirty() && (next_revision != revision_ || !error.empty())) {
        const auto first_conflict = !source_conflict_;
        source_conflict_ = true;
        source_error_ = "file changed on disk; discard the staged hex edits before reloading or saving";
        return first_conflict;
    }
    if (error.empty() && source_error_.starts_with("source changed;")) error = source_error_;
    const auto changed = completed || next_size != source_size_ || next_revision != revision_ || error != source_error_;
    if (changed) search_content_.reset();
    source_error_ = std::move(error);
    if (source_conflict_) return changed;
    if (next_size == source_size_ && next_revision == revision_) return changed;
    const auto old_size = source_size_;
    source_size_ = next_size;
    revision_ = std::move(next_revision);
    storage_path_ = std::move(next_path);
    previous_offsets_.clear();
    if (presentation() != nullptr) {
        source_error_ = "source changed; select Raw/Unformat to refresh the formatted snapshot";
    } else {
        select_presentation();
        if (hex_) {
            offset_ = std::min(offset_, hex_cursor_) / 16U * 16U;
            keep_hex_cursor_visible();
        }
    }
    set_view_end(std::min(view_end_, size_));
    next_search_offset_ = std::min(next_search_offset_, size_);
    search_begin_ = std::min(search_begin_, size_);
    if (source_size_ < old_size) {
        for (auto& bookmark : bookmarks_) bookmark = std::min(bookmark, size_);
    }
    if (search_match_ && (search_match_->second > size_ || !stream_)) search_match_.reset();
    return true;
}

void ViewerModel::close() {
    stream_.reset();
    provider_.reset();
    path_.clear();
    storage_path_.clear();
    window_.clear();
    size_ = 0;
    source_size_ = 0;
    magic_content_.reset();
    text_content_.reset();
    presentation_dirty_ = false;
    presentation_styles_.reset();
    nroff_offsets_.reset();
    codepage_offsets_.reset();
    search_content_.reset();
    text_style_rows_.clear();
    bookmarks_.fill(0);
    bookmark_slot_ = 0;
    offset_ = 0;
    view_end_ = 0;
    source_view_end_ = 0;
    previous_offsets_.clear();
    horizontal_offset_ = 0;
    last_search_.clear();
    search_match_.reset();
    text_highlights_.clear();
    last_text_page_.reset();
    next_search_offset_ = 0;
    search_begin_ = 0;
    hex_cursor_ = 0;
    hex_editing_ = false;
    hex_ascii_ = false;
    hex_low_nibble_ = false;
    edits_.clear();
    revision_.clear();
    source_conflict_ = false;
    source_error_.clear();
}

bool ViewerModel::open() const noexcept {
    return provider_ != nullptr;
}

void ViewerModel::go_to_start() {
    offset_ = 0;
    set_view_end(0);
    previous_offsets_.clear();
    horizontal_offset_ = 0;
    last_search_.clear();
    search_match_.reset();
    text_highlights_.clear();
    next_search_offset_ = 0;
    search_begin_ = 0;
    hex_cursor_ = 0;
    hex_low_nibble_ = false;
}

void ViewerModel::go_to_end(
    const std::size_t viewport_width,
    const std::size_t viewport_lines
) {
    if (!open()) {
        return;
    }
    static_cast<void>(refresh());
    if (presentation_dirty_) refresh_presentation(magic_, nroff_, encoding_, {});
    previous_offsets_.clear();
    horizontal_offset_ = 0;
    const auto height = std::max<std::size_t>(1U, viewport_lines);
    viewport_width_ = std::max<std::size_t>(1U, viewport_width);
    viewport_rows_ = height;
    if (hex_) {
        const auto rows = size_ == 0U ? 0U : (size_ - 1U) / 16U + 1U;
        offset_ = (rows - std::min<std::uint64_t>(rows, height)) * 16U;
    } else {
        offset_ = last_text_page(viewport_width_, height);
    }
    set_view_end(offset_);
    navigation_changed();
}

void ViewerModel::navigation_changed() {
    last_search_.clear();
    advanced_search_pattern_.clear();
    search_boundary_exhausted_ = false;
    next_search_offset_ = hex_ ? hex_cursor_ : offset_;
}

std::uint64_t ViewerModel::last_text_page(
    const std::size_t width, const std::size_t rows
) {
    if (!last_text_page_ || last_text_page_->width != width ||
        last_text_page_->rows != rows || last_text_page_->wrap != wrap_) {
        const auto offsets = text_offsets_before(size_, width, rows);
        last_text_page_ = LastTextPage{
            width, rows, wrap_, offsets.empty() ? 0U : offsets.front()
        };
    }
    return last_text_page_->offset;
}

void ViewerModel::scroll_lines(
    const int amount,
    const std::size_t viewport_width
) {
    if (!open()) {
        return;
    }
    static_cast<void>(refresh());
    if (presentation_dirty_) refresh_presentation(magic_, nroff_, encoding_, {});
    const auto original_offset = offset_;
    viewport_width_ = std::max<std::size_t>(1U, viewport_width);
    const auto maximum = !hex_ && amount > 0
                             ? last_text_page(viewport_width_, viewport_rows_)
                             : size_;
    if (offset_ < window_offset_ ||
        offset_ >= window_offset_ + window_.size()) {
        load_window(offset_, window_size, {});
    }
    if (amount > 0) {
        for (int line = 0;
             line < amount && offset_ < maximum;
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
            next_offset = std::min(next_offset, maximum);
            if (next_offset <= offset_) {
                break;
            }
            if (previous_offsets_.size() >= 4096U) {
                previous_offsets_.erase(previous_offsets_.begin(), previous_offsets_.begin() + 2048);
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
    if (offset_ != original_offset) {
        navigation_changed();
    }
}

void ViewerModel::page(
    const int direction,
    const std::size_t viewport_width,
    const std::size_t viewport_lines
) {
    viewport_rows_ = std::max<std::size_t>(1U, viewport_lines);
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
    navigation_changed();
}

void ViewerModel::toggle_hex() {
    const auto anchor = offset_;
    const auto line = text_content_ || nroff_ || (!encoding_.empty() && encoding_ != "UTF-8") ? line_number() : 0U;
    if (hex_ && (nroff_ || (!encoding_.empty() && encoding_ != "UTF-8"))) {
        refresh_presentation(magic_, nroff_, encoding_, {});
    }
    hex_ = !hex_;
    select_presentation();
    if (!hex_) {
        hex_editing_ = false;
    } else {
        hex_cursor_ = size_ == 0U ? 0U : std::min(anchor, size_ - 1U);
        offset_ = hex_cursor_ / 16U * 16U;
    }
    previous_offsets_.clear();
    horizontal_offset_ = 0;
    if (line != 0U) goto_line(line);
    navigation_changed();
}

const viewer_detail::Data* ViewerModel::presentation() const noexcept {
    if (!hex_ && text_content_) return text_content_.get();
    return magic_content_.get();
}

void ViewerModel::select_presentation() {
    const auto* content = presentation();
    size_ = content != nullptr ? content->size() : source_size_;
    offset_ = std::min(offset_, size_);
    hex_cursor_ = size_ == 0U ? 0U : std::min(hex_cursor_, size_ - 1U);
    set_view_end(std::min(view_end_, size_));
    search_content_.reset();
    window_.clear();
    window_offset_ = 0;
    last_text_page_.reset();
    text_highlights_.clear();
    text_style_rows_.clear();
}

void ViewerModel::read_view(const std::uint64_t offset, const std::uint64_t length,
                            std::ostream& output, const Cancellation& cancellation) {
    cancellation.throw_if_requested();
    if (const auto* content = presentation()) {
        content->read(offset, length, output, cancellation);
    } else read_source(offset, length, output, cancellation);
}

void ViewerModel::read_source(
    const std::uint64_t offset, const std::uint64_t length,
    std::ostream& output, const Cancellation& cancellation
) {
    if (!stream_) {
        if (dirty()) validate_edit_source(cancellation);
        provider_->read(storage_path_, offset, length, output, cancellation);
        if (dirty()) validate_edit_source(cancellation);
        return;
    }
    if (offset > source_size_) throw std::out_of_range("viewer read is outside the received input");
    auto remaining = std::min(length, source_size_ - offset);
    auto position = offset;
    std::array<char, 64U * 1024U> buffer{};
    while (remaining > 0U) {
        cancellation.throw_if_requested();
        const auto count = ::pread(stream_->storage.get(), buffer.data(),
            static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size())),
            static_cast<off_t>(position));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) throw std::system_error(errno, std::generic_category(), "read viewer storage");
        if (count == 0) throw std::runtime_error("viewer storage ended before the received input");
        output.write(buffer.data(), count);
        if (!output) throw std::runtime_error("cannot write viewer data");
        position += static_cast<std::uint64_t>(count);
        remaining -= static_cast<std::uint64_t>(count);
    }
}

void ViewerModel::validate_edit_source(const Cancellation& cancellation) const {
    if (stream_) throw std::runtime_error("streamed viewer data is read-only");
    if (source_conflict_) throw std::runtime_error(source_error_);
    const auto entry = provider_->stat_follow(path_, cancellation);
    if (entry.kind != EntryKind::file || entry.path != storage_path_ || entry.revision != revision_) {
        throw std::runtime_error("file changed on disk; refresh before editing or discard the staged hex edits");
    }
}

void ViewerModel::validate_snapshot_source(const Cancellation& cancellation) const {
    cancellation.throw_if_requested();
    if (dirty()) validate_edit_source(cancellation);
    if (stream_) return;
    const auto entry = provider_->stat_follow(path_, cancellation);
    if (entry.kind != EntryKind::file || entry.path != storage_path_ ||
        entry.size != source_size_ || entry.revision != revision_) {
        throw std::runtime_error("file changed while preparing the viewer; refresh and try again");
    }
}

std::shared_ptr<viewer_detail::Data> ViewerModel::capture_source(const Cancellation& cancellation) {
    validate_snapshot_source(cancellation);
    auto data = viewer_detail::capture(source_size_,
        [&](const std::uint64_t offset, const std::uint64_t length, std::ostream& output,
            const Cancellation& stopped) {
            if (edits_.empty()) {
                read_source(offset, length, output, stopped);
                return;
            }
            std::ostringstream bytes;
            read_source(offset, length, bytes, stopped);
            auto value = std::move(bytes).str();
            for (auto edit = edits_.lower_bound(offset);
                 edit != edits_.end() && edit->first - offset < value.size(); ++edit) {
                value[static_cast<std::size_t>(edit->first - offset)] = static_cast<char>(edit->second);
            }
            output.write(value.data(), static_cast<std::streamsize>(value.size()));
        }, cancellation);
    validate_snapshot_source(cancellation);
    return data;
}

std::shared_ptr<viewer_detail::Data> ViewerModel::search_data(const Cancellation& cancellation) {
    if (!hex_ && text_content_) return text_content_;
    if (magic_content_) return magic_content_;
    validate_snapshot_source(cancellation);
    if (!search_content_) search_content_ = capture_source(cancellation);
    return search_content_;
}

std::uint64_t ViewerModel::data_size() const noexcept {
    return magic_content_ ? magic_content_->size() : source_size_;
}

std::uint64_t ViewerModel::source_position(std::uint64_t position) const {
    if (hex_ || !text_content_) return std::min(position, data_size());
    if (nroff_ && nroff_offsets_) position = nroff_offsets_->source_position(position);
    if (codepage_offsets_) position = codepage_offsets_->source_position(position);
    return std::min(position, data_size());
}

void ViewerModel::set_view_end(const std::uint64_t position) {
    const auto source = source_position(position);
    view_end_ = position;
    source_view_end_ = source;
}

std::uint64_t ViewerModel::source_view_end() const { return source_view_end_; }

void ViewerModel::refresh_presentation(const bool magic, const bool nroff, std::string codepage,
                                       const Cancellation& cancellation) {
    static_cast<void>(refresh(cancellation));
    if (reading() && (nroff || (!codepage.empty() && codepage != "UTF-8"))) {
        throw std::runtime_error("wait for end of input before formatting or changing the stream codepage");
    }
    std::shared_ptr<viewer_detail::Data> parsed;
    if (magic && !reading()) {
        std::ostringstream initial;
        read_source(0, std::min<std::uint64_t>(12, source_size_), initial, cancellation);
        const auto header = std::move(initial).str();
        if (const auto decoder = compression_decoder(header)) {
            const auto encoded = capture_source(cancellation);
            parsed = viewer_detail::decompress(*encoded, *decoder, cancellation);
        }
    }
    std::shared_ptr<viewer_detail::Data> text;
    std::shared_ptr<viewer_detail::Styles> styles;
    std::shared_ptr<viewer_detail::OffsetMap> nroff_offsets, codepage_offsets;
    if (nroff || (!codepage.empty() && codepage != "UTF-8")) {
        auto input = parsed ? parsed : capture_source(cancellation);
        if (!codepage.empty() && codepage != "UTF-8") {
            auto decoded = viewer_detail::decode(*input, codepage, cancellation);
            input = std::move(decoded.data);
            codepage_offsets = std::move(decoded.offsets);
        }
        if (nroff) {
            auto formatted = viewer_detail::format_nroff(*input, cancellation);
            text = std::move(formatted.data);
            styles = std::move(formatted.styles);
            nroff_offsets = std::move(formatted.offsets);
        } else text = std::move(input);
    }
    magic_ = magic;
    nroff_ = nroff;
    encoding_ = std::move(codepage);
    magic_content_ = std::move(parsed);
    if (magic_content_) hex_editing_ = false;
    text_content_ = std::move(text);
    presentation_styles_ = std::move(styles);
    nroff_offsets_ = std::move(nroff_offsets);
    codepage_offsets_ = std::move(codepage_offsets);
    presentation_dirty_ = false;
    if (source_error_.starts_with("source changed;")) source_error_.clear();
    select_presentation();
}

void ViewerModel::toggle_magic(const Cancellation& cancellation) {
    if (dirty()) throw std::runtime_error("save or discard hex edits before changing parse mode");
    if (reading() && !magic_) throw std::runtime_error("wait for end of input before parsing the stream");
    refresh_presentation(!magic_, nroff_, encoding_, cancellation);
    goto_offset(0);
}

void ViewerModel::toggle_nroff(const Cancellation& cancellation) {
    if (dirty()) throw std::runtime_error("save or discard hex edits before changing formatting");
    const auto line = line_number(cancellation);
    refresh_presentation(magic_, !nroff_, encoding_, cancellation);
    goto_line(line, cancellation);
}

void ViewerModel::set_encoding(std::string codepage, const Cancellation& cancellation) {
    if (dirty()) throw std::runtime_error("save or discard hex edits before changing codepage");
    const auto line = line_number(cancellation);
    refresh_presentation(magic_, nroff_, std::move(codepage), cancellation);
    goto_line(line, cancellation);
}

void ViewerModel::select_bookmark(const std::size_t slot) {
    if (slot >= bookmarks_.size()) throw std::out_of_range("viewer bookmark must be 0 through 9");
    bookmark_slot_ = slot;
}

void ViewerModel::set_bookmark() { bookmarks_[bookmark_slot_] = offset_; }
void ViewerModel::goto_bookmark() { goto_offset(bookmarks_[bookmark_slot_]); }

void ViewerModel::goto_offset(const std::uint64_t requested) {
    if (!open()) {
        throw std::runtime_error("viewer is not open");
    }
    if (presentation_dirty_) refresh_presentation(magic_, nroff_, encoding_, {});
    hex_cursor_ = size_ == 0U ? 0U : std::min(requested, size_ - 1U);
    offset_ = hex_ ? hex_cursor_ / 16U * 16U : std::min(requested, size_);
    previous_offsets_.clear();
    horizontal_offset_ = 0;
    hex_low_nibble_ = false;
    next_search_offset_ = offset_;
    navigation_changed();
}

void ViewerModel::goto_data_offset(const std::uint64_t requested) {
    if (presentation_dirty_) refresh_presentation(magic_, nroff_, encoding_, {});
    if (requested > data_size()) throw std::out_of_range("offset is outside the file data");
    if (hex_ || !text_content_) { goto_offset(requested); return; }
    auto view = codepage_offsets_ ? codepage_offsets_->view_position(requested) : requested;
    if (nroff_ && nroff_offsets_) view = nroff_offsets_->view_position(view);
    goto_offset(std::min<std::uint64_t>(view, size_));
}

void ViewerModel::goto_line(
    const std::uint64_t requested,
    const Cancellation& cancellation
) {
    if (!open()) {
        throw std::runtime_error("viewer is not open");
    }
    static_cast<void>(refresh(cancellation));
    if (presentation_dirty_) refresh_presentation(magic_, nroff_, encoding_, cancellation);
    if (requested == 0U) {
        throw std::invalid_argument("line numbers start at 1");
    }
    std::uint64_t position = 0;
    std::uint64_t line = 1;
    while (line < requested && position < size_) {
        cancellation.throw_if_requested();
        const auto bytes = window_at(position, 1U, cancellation);
        if (bytes.empty()) {
            throw std::runtime_error("file changed while locating the line");
        }
        const auto newline = bytes.find('\n');
        if (newline == std::string_view::npos) {
            position += bytes.size();
        } else {
            position += newline + 1U;
            ++line;
        }
    }
    if (line != requested || (position == size_ && requested != 1U)) {
        throw std::out_of_range("line number is outside the file");
    }
    goto_offset(position);
}

void ViewerModel::goto_percent(const unsigned int percent) {
    if (percent > 100U) {
        throw std::out_of_range("percentage must be between 0 and 100");
    }
    goto_data_offset(data_size() / 100U * percent + data_size() % 100U * percent / 100U);
}

std::uint64_t ViewerModel::line_number(const Cancellation& cancellation) {
    if (!open()) {
        throw std::runtime_error("viewer is not open");
    }
    if (presentation_dirty_) refresh_presentation(magic_, nroff_, encoding_, cancellation);
    const auto end = hex_ ? hex_cursor_ : offset_;
    std::uint64_t line = 1;
    for (std::uint64_t position = 0; position < end;) {
        cancellation.throw_if_requested();
        const auto bytes = window_at(position, 1U, cancellation);
        if (bytes.empty()) {
            throw std::runtime_error("file changed while locating the line");
        }
        const auto count = static_cast<std::size_t>(
            std::min<std::uint64_t>(bytes.size(), end - position)
        );
        line += static_cast<std::uint64_t>(std::count(
            bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(count), '\n'
        ));
        position += count;
    }
    return line;
}

unsigned int ViewerModel::position_percent() const noexcept {
    if (reading() && size_ == 0U) return 0U;
    const auto position = hex_ ? hex_cursor_ : source_view_end_;
    const auto total = hex_ && size_ > 0U ? size_ - 1U : data_size();
    return total == 0U ? 100U : static_cast<unsigned int>(std::min(
        100.0L, static_cast<long double>(position) * 100.0L / total
    ));
}

void ViewerModel::toggle_hex_editing() {
    if (!open()) {
        throw std::runtime_error("viewer is not open");
    }
    if (!hex_editing_) {
        static_cast<void>(refresh());
        validate_edit_source({});
    }
    if (!hex_editing_ && !has_capability(provider_->capabilities(), ProviderCapability::write)) {
        throw std::runtime_error("this filesystem is read-only");
    }
    if (parsed()) throw std::runtime_error("parsed data cannot be hex-edited; select Raw first");
    if (!hex_) toggle_hex();
    hex_editing_ = !hex_editing_;
    hex_cursor_ = size_ == 0U ? 0U : std::min(std::max(hex_cursor_, offset_), size_ - 1U);
    hex_low_nibble_ = false;
}

bool ViewerModel::hex_editing() const noexcept { return hex_editing_; }
bool ViewerModel::hex_ascii() const noexcept { return hex_ascii_; }
std::uint64_t ViewerModel::hex_cursor() const noexcept { return hex_cursor_; }
bool ViewerModel::hex_low_nibble() const noexcept { return hex_low_nibble_; }
bool ViewerModel::dirty() const noexcept { return !edits_.empty(); }
bool ViewerModel::search_can_wrap() const noexcept {
    return last_search_options_.backwards ? search_begin_ < size_ : search_begin_ > 0U;
}

void ViewerModel::toggle_hex_navigation() {
    hex_ascii_ = !hex_ascii_;
    hex_low_nibble_ = false;
}

void ViewerModel::set_hex_cursor(
    const std::uint64_t offset,
    const bool ascii,
    const bool low_nibble
) {
    if (!open() || !hex_ || offset >= size_) {
        throw std::out_of_range("hex cursor is outside the file");
    }
    hex_cursor_ = offset;
    hex_ascii_ = ascii;
    hex_low_nibble_ = !ascii && low_nibble;
    keep_hex_cursor_visible();
    navigation_changed();
}

void ViewerModel::move_hex_cursor(const long long delta) {
    if (size_ == 0U) {
        return;
    }
    const auto previous = hex_cursor_;
    if (delta < 0) {
        const auto distance = static_cast<std::uint64_t>(-(delta + 1LL)) + 1U;
        hex_cursor_ -= std::min(hex_cursor_, distance);
    } else {
        hex_cursor_ += std::min(size_ - 1U - hex_cursor_, static_cast<std::uint64_t>(delta));
    }
    keep_hex_cursor_visible();
    hex_low_nibble_ = false;
    if (hex_cursor_ != previous) {
        navigation_changed();
    }
}

void ViewerModel::keep_hex_cursor_visible() {
    const auto row = hex_cursor_ / 16U;
    if (hex_cursor_ < offset_) {
        offset_ = row * 16U;
    } else if (row >= offset_ / 16U + viewport_rows_) {
        offset_ = (row - viewport_rows_ + 1U) * 16U;
    }
}

void ViewerModel::scroll_hex_lines(const int amount) {
    if (!open() || !hex_) {
        throw std::logic_error("hex scrolling requires an open hex viewer");
    }
    if (size_ == 0U || amount == 0) {
        return;
    }
    const auto relative = hex_cursor_ >= offset_ ? hex_cursor_ - offset_ : 0U;
    auto row = std::min(offset_ / 16U, (size_ - 1U) / 16U);
    if (amount < 0) {
        row -= std::min(row, static_cast<std::uint64_t>(-static_cast<long long>(amount)));
    } else {
        row += std::min((size_ - 1U) / 16U - row, static_cast<std::uint64_t>(amount));
    }
    offset_ = row * 16U;
    hex_cursor_ = offset_ + std::min(relative, size_ - 1U - offset_);
    hex_low_nibble_ = false;
    previous_offsets_.clear();
    navigation_changed();
}

void ViewerModel::set_byte(const std::uint64_t offset, const unsigned char value) {
    if (!hex_editing_ || offset >= size_) {
        throw std::runtime_error("no editable byte at the hex cursor");
    }
    validate_edit_source({});
    std::ostringstream bytes;
    provider_->read(storage_path_, offset, 1U, bytes, {});
    const auto original = std::move(bytes).str();
    if (original.size() != 1U) {
        throw std::runtime_error("file changed while editing hex data");
    }
    validate_edit_source({});
    search_match_.reset();
    search_content_.reset();
    last_text_page_.reset();
    navigation_changed();
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
    validate_edit_source({});
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

std::uintmax_t ViewerModel::hardlink_count() const {
    if (const auto* local = dynamic_cast<const LocalProvider*>(provider_.get())) {
        return local->hardlink_count(storage_path_);
    }
    throw std::runtime_error("hardlink inspection is unavailable for this provider");
}

void ViewerModel::save_changes(
    const Cancellation& cancellation,
    const ViewerLinkedSave linked
) {
    if (!dirty()) {
        return;
    }
    validate_edit_source(cancellation);
    if (parsed()) throw std::runtime_error("parsed data cannot be saved as raw hex edits");
    if (const auto* local = dynamic_cast<const LocalProvider*>(provider_.get())) {
        local->ensure_writable(storage_path_);
        if (linked == ViewerLinkedSave::require_choice &&
            local->hardlink_count(storage_path_) > 1U) {
            throw std::runtime_error("linked file requires a detach or preserve-links choice");
        }
    }
    EditedInput buffer(*provider_, storage_path_, source_size_, edits_, cancellation);
    std::istream input(&buffer);
    input.exceptions(std::ios::badbit);
    if (linked == ViewerLinkedSave::preserve_links) {
        auto* local = dynamic_cast<LocalProvider*>(provider_.get());
        if (local == nullptr) {
            throw std::runtime_error("linked saves are unavailable for this provider");
        }
        local->write_in_place(storage_path_, input, source_size_, revision_, cancellation);
    } else {
        provider_->write(storage_path_, input, source_size_, true, revision_, cancellation, {});
    }
    revision_ = provider_->stat(storage_path_, cancellation).revision;
    edits_.clear();
    window_.clear();
    source_conflict_ = false;
    source_error_.clear();
}

void ViewerModel::discard_changes() {
    edits_.clear();
    window_.clear();
    search_match_.reset();
    last_text_page_.reset();
    hex_low_nibble_ = false;
    text_content_.reset();
    presentation_styles_.reset();
    nroff_offsets_.reset();
    codepage_offsets_.reset();
    search_content_.reset();
    presentation_dirty_ = !hex_ && (nroff_ || (!encoding_.empty() && encoding_ != "UTF-8"));
    source_conflict_ = false;
    source_error_.clear();
    select_presentation();
    static_cast<void>(refresh());
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
    const Cancellation& cancellation,
    std::optional<std::pair<std::size_t, std::size_t>>* highlight,
    std::vector<ViewerTextStyle>* styles
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
        const auto byte_begin = offset;
        const bool matched = search_match_ &&
            offset < search_match_->second && offset + length > search_match_->first;
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
        const auto begin = output->size();
        if (tab) {
            output->append(glyph_width, ' ');
        } else if (glyph_width > 0U || !output->empty()) {
            output->append(glyph);
        }
        if (highlight != nullptr && matched && output->size() > begin) {
            if (!*highlight) {
                *highlight = std::pair{begin, output->size()};
            } else {
                (*highlight)->second = output->size();
            }
        }
        if (styles != nullptr && output->size() > begin && nroff_ && presentation_styles_) {
            const auto style = presentation_styles_->at(byte_begin, length);
            if (style) {
                styles->push_back({begin, output->size(), style->bold, style->underline});
            }
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
    if (offset >= size_) {
        window_.clear();
        window_offset_ = offset;
        return;
    }
    std::ostringstream output;
    read_view(
        offset,
        std::min<std::uint64_t>(size, size_ - offset),
        output,
        cancellation
    );
    window_ = std::move(output).str();
    window_offset_ = offset;
    if (presentation() == nullptr) {
        for (auto edit = edits_.lower_bound(offset);
             edit != edits_.end() && edit->first - offset < window_.size(); ++edit) {
            window_[static_cast<std::size_t>(edit->first - offset)] = static_cast<char>(edit->second);
        }
    }
}

std::vector<std::string> ViewerModel::lines(
    const std::size_t width,
    const std::size_t height,
    const Cancellation& cancellation
) {
    static_cast<void>(refresh(cancellation));
    if (presentation_dirty_) refresh_presentation(magic_, nroff_, encoding_, cancellation);
    text_highlights_.clear();
    text_style_rows_.clear();
    viewport_width_ = std::max<std::size_t>(1U, width);
    viewport_rows_ = std::max<std::size_t>(1U, height);
    if (!open()) {
        set_view_end(0);
        return {};
    }
    std::vector<std::string> result;
    result.reserve(height);
    set_view_end(offset_);
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
            set_view_end(view_end_ + count);
        }
        return result;
    }

    while (result.size() < height && view_end_ < size_) {
        std::string line;
        std::optional<std::pair<std::size_t, std::size_t>> highlight;
        std::vector<ViewerTextStyle> styles;
        const auto next = read_text_row(
            view_end_,
            width,
            &line,
            cancellation,
            &highlight,
            &styles
        );
        if (next <= view_end_) {
            break;
        }
        result.push_back(std::move(line));
        text_highlights_.push_back(highlight);
        text_style_rows_.push_back(std::move(styles));
        set_view_end(next);
    }
    return result;
}

std::optional<std::pair<std::size_t, std::size_t>>
ViewerModel::text_highlight(const std::size_t row) const noexcept {
    return row < text_highlights_.size() ? text_highlights_[row] : std::nullopt;
}

const std::vector<ViewerTextStyle>& ViewerModel::text_styles(const std::size_t row) const noexcept {
    static const std::vector<ViewerTextStyle> empty;
    return row < text_style_rows_.size() ? text_style_rows_[row] : empty;
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
    static_cast<void>(refresh(cancellation));
    if (presentation_dirty_) refresh_presentation(magic_, nroff_, encoding_, cancellation);
    const auto requested = std::string(needle);
    last_search_options_ = {.mode=EditorSearchMode::normal, .case_sensitive=case_sensitive};
    const bool repeat =
        requested == last_search_ &&
        case_sensitive == last_search_case_sensitive_;
    const auto search_begin =
        repeat ? std::min(next_search_offset_, size_) : hex_ ? hex_cursor_ : offset_;
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
            read_view(
                cursor,
                std::min(chunk_size, end - cursor),
                output,
                cancellation
            );
            auto value = std::move(output).str();
            if (presentation() == nullptr) {
                for (auto edit = edits_.lower_bound(cursor);
                     edit != edits_.end() && edit->first - cursor < value.size(); ++edit) {
                    value[static_cast<std::size_t>(edit->first - cursor)] = static_cast<char>(edit->second);
                }
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
        if (!repeat) {
            search_match_.reset();
        }
        last_search_ = requested;
        last_search_case_sensitive_ = case_sensitive;
        next_search_offset_ = search_begin;
        return std::nullopt;
    }
    last_search_ = requested;
    last_search_case_sensitive_ = case_sensitive;
    next_search_offset_ = found->second;
    return reveal_search_match(*found, cancellation);
}

std::optional<std::uint64_t> ViewerModel::find(
    const std::string_view needle, const EditorSearchOptions& options,
    const Cancellation& cancellation, const bool wrap, const bool all_charsets
) {
    if (!open() || needle.empty()) return std::nullopt;
    static_cast<void>(refresh(cancellation));
    if (presentation_dirty_) refresh_presentation(magic_, nroff_, encoding_, cancellation);
    const bool continuing = advanced_search_pattern_ == needle && last_search_options_.mode == options.mode &&
        last_search_options_.case_sensitive == options.case_sensitive &&
        last_search_options_.whole_words == options.whole_words && last_search_all_charsets_ == all_charsets && !last_search_.empty();
    const bool repeat = continuing && last_search_options_.backwards == options.backwards;
    if (!all_charsets && !options.backwards && !options.whole_words &&
        (options.mode == EditorSearchMode::normal || options.mode == EditorSearchMode::hexadecimal)) {
        const auto bytes = options.mode == EditorSearchMode::hexadecimal ? EditorSearch::decode_hexadecimal(needle) : std::string(needle);
        if (!repeat) {
            if (continuing && search_match_) {
                last_search_ = bytes;
                last_search_case_sensitive_ = options.mode == EditorSearchMode::hexadecimal || options.case_sensitive;
                next_search_offset_ = search_match_->second;
            } else last_search_.clear();
        }
        const auto found = find(bytes, options.mode == EditorSearchMode::hexadecimal || options.case_sensitive, cancellation, wrap);
        advanced_search_pattern_ = needle;
        last_search_options_ = options;
        last_search_all_charsets_ = false;
        search_boundary_exhausted_ = false;
        return found;
    }
    const auto content = search_data(cancellation);
    auto start = repeat ? std::min<std::uint64_t>(next_search_offset_, size_)
               : continuing && search_match_ ? options.backwards ? search_match_->first : search_match_->second
               : hex_ ? hex_cursor_ : offset_;
    search_begin_ = start;
    if (repeat && search_boundary_exhausted_) {
        if (!wrap) return std::nullopt;
        start = options.backwards ? size_ : 0U;
    }
    const auto found = viewer_detail::find(*content, needle, start, options, wrap, all_charsets, cancellation);
    if (presentation() == nullptr) validate_snapshot_source(cancellation);
    advanced_search_pattern_ = needle;
    last_search_ = needle;
    last_search_options_ = options;
    last_search_all_charsets_ = all_charsets;
    last_search_case_sensitive_ = options.case_sensitive;
    if (!found) {
        if (!repeat) search_match_.reset();
        next_search_offset_ = start;
        return std::nullopt;
    }
    search_boundary_exhausted_ = false;
    if (options.backwards) next_search_offset_ = found->begin;
    else if (found->end > found->begin) next_search_offset_ = found->end;
    else {
        next_search_offset_ = viewer_detail::next_character(*content, found->end, cancellation);
        search_boundary_exhausted_ = found->end == content->size();
    }
    return reveal_search_match({found->begin, found->end}, cancellation);
}

std::optional<std::uint64_t> ViewerModel::reveal_search_match(
    const std::pair<std::uint64_t, std::uint64_t> found, const Cancellation& cancellation
) {
    constexpr std::uint64_t chunk_size = 256U * 1024U;
    const auto prefix_begin =
        found.first > chunk_size ? found.first - chunk_size : 0U;
    std::ostringstream prefix_output;
    read_view(
        prefix_begin,
        found.first - prefix_begin,
        prefix_output,
        cancellation
    );
    const auto prefix = std::move(prefix_output).str();
    const auto newline = prefix.rfind('\n');
    auto next_offset =
        newline == std::string::npos
            ? prefix_begin
            : prefix_begin + newline + 1U;
    std::size_t next_column = 0;
    if (hex_) {
        hex_cursor_ = found.first;
        next_offset = found.first / 16U * 16U;
    } else if (wrap_) {
        while (next_offset < found.first) {
            const auto next = read_text_row(next_offset, viewport_width_, nullptr, cancellation);
            if (next <= next_offset || next > found.first) {
                break;
            }
            next_offset = next;
        }
    } else {
        const auto begin = newline == std::string::npos ? 0U : newline + 1U;
        std::size_t column = 0;
        for (auto index = begin; index < prefix.size();) {
            const auto next = next_utf8(prefix, index);
            const auto glyph = std::string_view(prefix).substr(index, next - index);
            if (glyph == "\t") {
                column += 8U - column % 8U;
            } else if (static_cast<unsigned char>(glyph.front()) >= 32U &&
                       glyph.front() != '\x7f') {
                column += static_cast<std::size_t>(std::max(0, ftxui::string_width(glyph)));
            }
            index = next;
        }
        if (column >= viewport_width_) {
            next_column = column - viewport_width_ / 2U;
        }
    }
    offset_ = next_offset;
    previous_offsets_.clear();
    horizontal_offset_ = next_column;
    search_match_ = found;
    return found.first;
}

}  // namespace mcpp
