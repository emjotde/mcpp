#include "viewer_data.hpp"
#include "editor_search_data.hpp"
#include "local_tools_stream.hpp"

#include "mcpp/local_tools.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iconv.h>
#include <limits>
#include <mutex>
#include <regex.h>
#include <sstream>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/random.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace mcpp::viewer_detail {
namespace {

constexpr std::size_t block_size = 64U * 1024U;

class ConversionError final : public std::system_error {
public:
    using std::system_error::system_error;
};

class Descriptor {
public:
    explicit Descriptor(const int value) : value_(value) {}
    ~Descriptor() { if (value_ >= 0) ::close(value_); }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    [[nodiscard]] int get() const noexcept { return value_; }
    [[nodiscard]] int release() noexcept { return std::exchange(value_, -1); }
private:
    int value_;
};

void check_range(const std::uint64_t offset, const std::uint64_t length) {
    constexpr auto maximum = static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
    if (offset > maximum || length > maximum - offset) {
        throw std::length_error("viewer data exceeds the seekable storage limit");
    }
}

template<class Record>
class Records {
public:
    [[nodiscard]] std::uint64_t size() const noexcept { return count_; }

    [[nodiscard]] Record get(const std::uint64_t index) const {
        if (index >= count_) throw std::out_of_range("viewer metadata index");
        load(index);
        return cache_[static_cast<std::size_t>(index - first_)];
    }

    void set(const std::uint64_t index, const Record& value) {
        if (index >= count_) throw std::out_of_range("viewer metadata index");
        load(index);
        cache_[static_cast<std::size_t>(index - first_)] = value;
        dirty_ = true;
    }

    void push(const Record& value) {
        check_range(count_ * sizeof(Record), sizeof(Record));
        load(count_);
        cache_[static_cast<std::size_t>(count_ - first_)] = value;
        ++count_;
        dirty_ = true;
    }

    void clear() {
        dirty_ = false;
        first_ = std::numeric_limits<std::uint64_t>::max();
        count_ = 0;
        data_.resize(0);
    }

    void finish() const { flush(); }

private:
    void flush() const {
        if (!dirty_) return;
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(cache_.size(), count_ - first_));
        data_.write(first_ * sizeof(Record),
                    {reinterpret_cast<const char*>(cache_.data()), count * sizeof(Record)});
        dirty_ = false;
    }

    void load(const std::uint64_t index) const {
        const auto first = index / cache_.size() * cache_.size();
        if (first == first_) return;
        flush();
        first_ = first;
        if (first < count_) {
            const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(cache_.size(), count_ - first));
            const auto bytes = data_.bytes(first * sizeof(Record), count * sizeof(Record));
            if (bytes.size() != count * sizeof(Record)) throw std::runtime_error("viewer metadata ended early");
            std::memcpy(cache_.data(), bytes.data(), bytes.size());
        }
    }

    mutable Data data_;
    mutable std::array<Record, 128> cache_{};
    mutable std::uint64_t first_{std::numeric_limits<std::uint64_t>::max()};
    mutable bool dirty_{false};
    std::uint64_t count_{0};
};

class Cursor {
public:
    Cursor(const Data& data, const Cancellation& cancellation)
        : data_(data), cancellation_(cancellation) {}

    [[nodiscard]] std::string_view at(const std::uint64_t position, const std::size_t minimum = 1U) {
        if (position >= data_.size()) return {};
        if (position < begin_ || position - begin_ >= bytes_.size() ||
            bytes_.size() - static_cast<std::size_t>(position - begin_) <
                std::min<std::uint64_t>(minimum, data_.size() - position)) {
            begin_ = position;
            bytes_ = data_.bytes(position, block_size, cancellation_);
        }
        return std::string_view(bytes_).substr(static_cast<std::size_t>(position - begin_));
    }

    [[nodiscard]] std::uint64_t next(const std::uint64_t position) {
        if (position >= data_.size()) return data_.size();
        auto end = position + 1U;
        while (end < data_.size()) {
            const auto bytes = at(end);
            const auto stop = std::find_if(bytes.begin(), bytes.end(), [](const unsigned char byte) {
                return (byte & 0xc0U) != 0x80U;
            });
            end += static_cast<std::uint64_t>(stop - bytes.begin());
            if (stop != bytes.end()) break;
        }
        return end;
    }

    void copy(std::uint64_t position, std::uint64_t length, std::ostream& output) {
        while (length > 0U) {
            const auto bytes = at(position);
            const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(length, bytes.size()));
            if (count == 0U) throw std::runtime_error("viewer transform input ended early");
            output.write(bytes.data(), static_cast<std::streamsize>(count));
            if (!output) throw std::runtime_error("cannot write formatted viewer data");
            position += count;
            length -= count;
        }
    }

private:
    const Data& data_;
    const Cancellation& cancellation_;
    std::uint64_t begin_{0};
    std::string bytes_;
};

bool equal(Cursor& left_reader, Cursor& right_reader, std::uint64_t left, std::uint64_t right,
           std::uint64_t length, const Cancellation& cancellation) {
    while (length > 0U) {
        cancellation.throw_if_requested();
        const auto left_bytes = left_reader.at(left);
        const auto right_bytes = right_reader.at(right);
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(
            length, std::min(left_bytes.size(), right_bytes.size())));
        if (count == 0U) throw std::runtime_error("viewer transform input ended early");
        if (left_bytes.substr(0, count) != right_bytes.substr(0, count)) return false;
        left += count;
        right += count;
        length -= count;
    }
    return true;
}

std::uint64_t previous_character(Cursor& reader, std::uint64_t position) {
    std::size_t span = 4U;
    while (position > 0U) {
        const auto begin = position > span ? position - span : 0U;
        const auto bytes = reader.at(begin, static_cast<std::size_t>(position - begin))
                              .substr(0, static_cast<std::size_t>(position - begin));
        for (auto index = bytes.size(); index > 0U; --index) {
            if ((static_cast<unsigned char>(bytes[index - 1U]) & 0xc0U) != 0x80U) return begin + index - 1U;
        }
        position = begin;
        span = block_size;
    }
    return 0;
}

bool whole_word(const Data& input, Cursor& reader, const EditorSearchMatch& match) {
    const auto word = [&](const std::uint64_t position) {
        return position < input.size() &&
            search_detail::word_character(reader.at(position, 4U).substr(0, 4U));
    };
    return !(match.begin > 0U && word(previous_character(reader, match.begin))) &&
           !word(match.end);
}

std::optional<EditorSearchMatch> literal_range(
    const Data& input, const std::string_view pattern, const std::uint64_t begin,
    const std::uint64_t end, const EditorSearchOptions& options,
    const Cancellation& cancellation
) {
    if (pattern.size() > (std::numeric_limits<std::size_t>::max() - block_size - 8U) / 4U) {
        throw std::length_error("search pattern is too large");
    }
    // A case-folded UTF-8 literal consumes at most four bytes per pattern byte.
    // Regex and wildcard searches never use these finite literal windows.
    const auto maximum = options.mode == EditorSearchMode::hexadecimal || options.case_sensitive
        ? pattern.size() : pattern.size() * 4U;
    auto local = options;
    local.backwards = false;
    local.whole_words = false;
    const auto unicode_case = options.mode == EditorSearchMode::normal && !options.case_sensitive &&
        (input.non_ascii() || std::any_of(pattern.begin(), pattern.end(), [](const unsigned char byte) {
            return byte >= 0x80U;
        }));
    std::optional<EditorSearchMatch> last;
    Cursor neighbors(input, cancellation);
    for (auto cursor = begin; cursor < end;) {
        cancellation.throw_if_requested();
        const auto first = cursor > 3U ? cursor - 3U : 0U;
        const auto core_end = cursor + std::min<std::uint64_t>(block_size, end - cursor);
        const auto match_end = core_end + std::min<std::uint64_t>(maximum, end - core_end);
        const auto read_end = match_end + std::min<std::uint64_t>(4U, input.size() - match_end);
        const auto bytes = input.bytes(first, static_cast<std::size_t>(read_end - first), cancellation);
        const auto found = unicode_case
            ? search_detail::casefolded_literals(bytes, pattern, static_cast<std::size_t>(cursor - first),
                                                   static_cast<std::size_t>(match_end - first))
            : EditorSearch::find_all(bytes, pattern, static_cast<std::size_t>(cursor - first),
                                      static_cast<std::size_t>(match_end - first), local);
        auto consumed = cursor;
        for (const auto& match : found) {
            if (first + match.begin >= core_end) break;
            cancellation.throw_if_requested();
            EditorSearchMatch candidate{
                static_cast<std::size_t>(first + match.begin),
                static_cast<std::size_t>(first + match.end)
            };
            consumed = candidate.end;
            if (!options.whole_words || whole_word(input, neighbors, candidate)) {
                if (!options.backwards) return candidate;
                last = std::move(candidate);
            }
        }
        cursor = std::max(core_end, consumed);
    }
    return last;
}

class Mapping {
public:
    explicit Mapping(const Data& input) : size_(input.size()) {
        if (size_ > static_cast<std::uint64_t>(std::numeric_limits<regoff_t>::max())) {
            throw std::length_error("this libc's POSIX regex offsets cannot address subjects above " +
                std::to_string(std::numeric_limits<regoff_t>::max()) + " bytes; literal and hex searches remain available");
        }
        if (size_ == 0U) return;
        bytes_ = ::mmap(nullptr, static_cast<std::size_t>(size_), PROT_READ, MAP_PRIVATE,
                        input.descriptor(), 0);
        if (bytes_ == MAP_FAILED) throw std::system_error(errno, std::generic_category(), "map private regex subject");
        static_cast<void>(::madvise(bytes_, static_cast<std::size_t>(size_), MADV_SEQUENTIAL));
    }
    ~Mapping() { if (bytes_ != MAP_FAILED) ::munmap(bytes_, static_cast<std::size_t>(size_)); }
    Mapping(const Mapping&) = delete;
    Mapping& operator=(const Mapping&) = delete;
    [[nodiscard]] std::string_view view() const noexcept {
        static constexpr char empty = '\0';
        return {size_ == 0U ? &empty : static_cast<const char*>(bytes_), static_cast<std::size_t>(size_)};
    }
private:
    void* bytes_{MAP_FAILED};
    std::uint64_t size_;
};

std::optional<EditorSearchMatch> find_one(
    const Data& input, const std::string_view pattern, const std::uint64_t start,
    const EditorSearchOptions& options, const bool wrap, const Cancellation& cancellation
) {
    cancellation.throw_if_requested();
    const auto anchor = std::min(start, input.size());
    if (options.mode == EditorSearchMode::regular_expression || options.mode == EditorSearchMode::wildcard) {
        const Mapping mapping(input);
        auto found = EditorSearch::find(mapping.view(), pattern, static_cast<std::size_t>(anchor),
                                        0, mapping.view().size(), options, wrap, false);
        cancellation.throw_if_requested();
        return found;
    }
    auto found = options.backwards ? literal_range(input, pattern, 0, anchor, options, cancellation)
                                   : literal_range(input, pattern, anchor, input.size(), options, cancellation);
    if (!found && wrap) {
        found = options.backwards ? literal_range(input, pattern, anchor, input.size(), options, cancellation)
                                  : literal_range(input, pattern, 0, anchor, options, cancellation);
    }
    return found;
}

std::string utf8(const std::uint32_t character) {
    std::string result;
    if (character < 0x80U) result += static_cast<char>(character);
    else if (character < 0x800U) {
        result += static_cast<char>(0xc0U | (character >> 6U));
        result += static_cast<char>(0x80U | (character & 63U));
    } else if (character < 0x10000U) {
        result += static_cast<char>(0xe0U | (character >> 12U));
        result += static_cast<char>(0x80U | ((character >> 6U) & 63U));
        result += static_cast<char>(0x80U | (character & 63U));
    } else if (character <= 0x10ffffU) {
        result += static_cast<char>(0xf0U | (character >> 18U));
        result += static_cast<char>(0x80U | ((character >> 12U) & 63U));
        result += static_cast<char>(0x80U | ((character >> 6U) & 63U));
        result += static_cast<char>(0x80U | (character & 63U));
    } else throw std::runtime_error("encoding conversion returned an invalid Unicode character");
    return result;
}

}  // namespace

int private_spool() {
    const auto* cache = std::getenv("XDG_CACHE_HOME");
    const auto* home = std::getenv("HOME");
    std::filesystem::path directory;
    if (cache != nullptr && std::filesystem::path(cache).is_absolute()) directory = cache;
    else if (home != nullptr && std::filesystem::path(home).is_absolute()) directory = std::filesystem::path(home) / ".cache";
    else throw std::runtime_error("viewer needs an absolute HOME or XDG_CACHE_HOME for private storage");
    directory /= "mcpp";
    std::filesystem::create_directories(directory);
    Descriptor parent(::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (parent.get() < 0) throw std::system_error(errno, std::generic_category(), "open viewer storage directory");
    const auto anonymous = ::openat(parent.get(), ".", O_TMPFILE | O_RDWR | O_CLOEXEC, 0600);
    if (anonymous >= 0) return anonymous;
    const auto failure = errno;
    if (failure != EOPNOTSUPP && failure != EINVAL && failure != EISDIR && failure != ENOSYS) {
        throw std::system_error(failure, std::generic_category(), "create private viewer storage");
    }
    std::array<unsigned char, 16> random{};
    std::size_t obtained = 0;
    while (obtained < random.size()) {
        const auto count = ::getrandom(random.data() + obtained, random.size() - obtained, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) throw std::system_error(errno, std::generic_category(), "name private viewer storage");
        obtained += static_cast<std::size_t>(count);
    }
    std::string name = ".viewer-";
    constexpr char digits[] = "0123456789abcdef";
    for (const auto byte : random) { name += digits[byte >> 4U]; name += digits[byte & 15U]; }
    Descriptor result(::openat(parent.get(), name.c_str(),
        O_CREAT | O_EXCL | O_NOFOLLOW | O_RDWR | O_CLOEXEC, 0600));
    if (result.get() < 0) throw std::system_error(errno, std::generic_category(), "create private viewer storage");
    if (::unlinkat(parent.get(), name.c_str(), 0) != 0) {
        throw std::system_error(errno, std::generic_category(), "unlink private viewer storage");
    }
    return result.release();
}

Data::Data() : descriptor_(private_spool()) {}
Data::~Data() { ::close(descriptor_); }

std::string Data::path() const {
    return "/proc/" + std::to_string(::getpid()) + "/fd/" + std::to_string(descriptor_);
}

void Data::append(const std::string_view bytes) {
    write(size_, bytes);
}

void Data::write(const std::uint64_t offset, const std::string_view bytes) {
    check_range(offset, bytes.size());
    if (bytes.empty()) {
        if (offset > size_) resize(offset);
        return;
    }
    const auto end = offset + bytes.size();
    if (!bytes.empty() && offset >= size_ &&
        std::all_of(bytes.begin(), bytes.end(), [](const char byte) { return byte == '\0'; })) {
        resize(end);
        return;
    }
    if (!non_ascii_) {
        non_ascii_ = std::any_of(bytes.begin(), bytes.end(), [](const unsigned char byte) { return byte >= 0x80U; });
    }
    for (std::size_t position = 0; position < bytes.size();) {
        const auto count = ::pwrite(descriptor_, bytes.data() + position, bytes.size() - position,
                                    static_cast<off_t>(offset + position));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) throw std::system_error(errno, std::generic_category(), "write private viewer storage");
        position += static_cast<std::size_t>(count);
    }
    size_ = std::max(size_, end);
}

void Data::resize(const std::uint64_t size) {
    check_range(size, 0);
    if (::ftruncate(descriptor_, static_cast<off_t>(size)) != 0) {
        throw std::system_error(errno, std::generic_category(), "resize private viewer storage");
    }
    size_ = size;
    if (size == 0U) non_ascii_ = false;
}

void Data::read(std::uint64_t offset, const std::uint64_t length, std::ostream& output,
                const Cancellation& cancellation) const {
    if (offset > size_) throw std::out_of_range("read outside private viewer storage");
    auto remaining = std::min(length, size_ - offset);
    std::array<char, block_size> buffer;
    while (remaining > 0U) {
        cancellation.throw_if_requested();
        const auto count = ::pread(descriptor_, buffer.data(),
            static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), remaining)), static_cast<off_t>(offset));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) throw std::system_error(errno, std::generic_category(), "read private viewer storage");
        if (count == 0) throw std::runtime_error("private viewer storage ended early");
        output.write(buffer.data(), count);
        if (!output) throw std::runtime_error("cannot write viewer data");
        offset += static_cast<std::uint64_t>(count);
        remaining -= static_cast<std::uint64_t>(count);
    }
}

std::string Data::bytes(const std::uint64_t offset, const std::size_t length,
                        const Cancellation& cancellation) const {
    std::ostringstream output;
    read(offset, length, output, cancellation);
    return std::move(output).str();
}

DataOutput::Buffer::Buffer(Data& data) : data_(data) {
    setp(buffer_.data(), buffer_.data() + buffer_.size());
}

std::uint64_t DataOutput::Buffer::size() const noexcept {
    return data_.size() + static_cast<std::uint64_t>(pptr() - pbase());
}

int DataOutput::Buffer::sync() {
    data_.append({pbase(), static_cast<std::size_t>(pptr() - pbase())});
    setp(buffer_.data(), buffer_.data() + buffer_.size());
    return 0;
}

DataOutput::Buffer::int_type DataOutput::Buffer::overflow(const int_type value) {
    sync();
    if (!traits_type::eq_int_type(value, traits_type::eof())) {
        *pptr() = traits_type::to_char_type(value);
        pbump(1);
    }
    return traits_type::not_eof(value);
}

std::streamsize DataOutput::Buffer::xsputn(const char* bytes, const std::streamsize length) {
    auto remaining = length;
    while (remaining > 0) {
        const auto count = std::min(remaining, static_cast<std::streamsize>(epptr() - pptr()));
        if (count == 0) { sync(); continue; }
        std::memcpy(pptr(), bytes, static_cast<std::size_t>(count));
        pbump(static_cast<int>(count));
        bytes += count;
        remaining -= count;
    }
    return length;
}

DataOutput::DataOutput(Data& data) : std::ostream(nullptr), buffer_(data) {
    rdbuf(&buffer_);
    exceptions(std::ios::badbit | std::ios::failbit);
}
void DataOutput::finish() { flush(); }
std::uint64_t DataOutput::size() const noexcept { return buffer_.size(); }

std::shared_ptr<Data> capture(const std::uint64_t size, const ReadRange& read,
                              const Cancellation& cancellation) {
    check_range(size, 0);
    auto result = std::make_shared<Data>();
    DataOutput output(*result);
    for (std::uint64_t position = 0; position < size;) {
        cancellation.throw_if_requested();
        const auto count = std::min<std::uint64_t>(size - position, block_size);
        const auto before = output.size();
        read(position, count, output, cancellation);
        if (output.size() - before != count) throw std::runtime_error("file changed while preparing the viewer");
        position += count;
    }
    output.finish();
    return result;
}

class OffsetMap::Impl {
public:
    struct Point { std::uint64_t view; std::uint64_t source; };
    Records<Point> points;
    mutable std::mutex mutex;
    bool characters;
    std::shared_ptr<const Data> decoded;
    explicit Impl(const bool value) : characters(value) {}
};

OffsetMap::OffsetMap(const bool characters) : impl_(std::make_unique<Impl>(characters)) {}
OffsetMap::OffsetMap(std::shared_ptr<const Data> decoded) : impl_(std::make_unique<Impl>(true)) {
    impl_->decoded = std::move(decoded);
}
OffsetMap::~OffsetMap() = default;

void OffsetMap::add(const std::uint64_t view, const std::uint64_t source, const bool force) {
    const std::lock_guard lock(impl_->mutex);
    auto& points = impl_->points;
    if (points.size() != 0U) {
        const auto last = points.get(points.size() - 1U);
        if (view < last.view) throw std::logic_error("viewer offset map is not ordered");
        if (view == last.view) { points.set(points.size() - 1U, {view, source}); return; }
        if (!force && source >= last.source && source - last.source == view - last.view) return;
    }
    points.push({view, source});
}

void OffsetMap::finish() {
    const std::lock_guard lock(impl_->mutex);
    impl_->points.finish();
}

std::uint64_t OffsetMap::source_position(const std::uint64_t view) const {
    const std::lock_guard lock(impl_->mutex);
    const auto& points = impl_->points;
    if (points.size() == 0U) return view;
    std::uint64_t begin = 0, end = points.size();
    while (begin < end) {
        const auto middle = begin + (end - begin) / 2U;
        if (points.get(middle).view <= view) begin = middle + 1U;
        else end = middle;
    }
    const auto index = begin == 0U ? 0U : begin - 1U;
    const auto point = points.get(index);
    if (impl_->decoded) {
        Cancellation cancellation;
        Cursor reader(*impl_->decoded, cancellation);
        auto source = point.source;
        for (auto position = point.view; position < view && position < impl_->decoded->size();) {
            position = reader.next(position);
            ++source;
        }
        return source;
    }
    if (impl_->characters && index + 1U < points.size() && point.view < view) {
        const auto next = points.get(index + 1U);
        if (next.source - point.source != next.view - point.view) return next.source;
    }
    return point.source + view - std::min(view, point.view);
}

std::uint64_t OffsetMap::view_position(const std::uint64_t source) const {
    const std::lock_guard lock(impl_->mutex);
    const auto& points = impl_->points;
    if (points.size() == 0U) return source;
    if (impl_->characters) {
        std::uint64_t begin = 0, end = points.size();
        while (begin < end) {
            const auto middle = begin + (end - begin) / 2U;
            if (points.get(middle).source <= source) begin = middle + 1U;
            else end = middle;
        }
        const auto index = begin == 0U ? 0U : begin - 1U;
        const auto point = points.get(index);
        if (impl_->decoded) {
            Cancellation cancellation;
            Cursor reader(*impl_->decoded, cancellation);
            auto view = point.view;
            for (auto position = point.source; position < source && view < impl_->decoded->size(); ++position) {
                view = reader.next(view);
            }
            return view;
        }
        if (index + 1U < points.size()) {
            const auto next = points.get(index + 1U);
            if (next.source - point.source != next.view - point.view) return point.view;
        }
        return point.view + source - std::min(source, point.source);
    }
    std::uint64_t view = 0, best = 0;
    for (std::uint64_t index = 0; index < points.size(); ++index) {
        const auto point = points.get(index);
        if (point.source > source || point.source < best) continue;
        best = point.source;
        view = point.view + source - point.source;
        if (index + 1U < points.size()) {
            const auto next = points.get(index + 1U);
            if (source < next.source && next.source - point.source != next.view - point.view) view = point.view;
            else view = std::min(view, next.view);
        }
    }
    return view;
}

class Styles::Impl {
public:
    Records<TextStyle> styles;
    mutable std::mutex mutex;
};
Styles::Styles() : impl_(std::make_unique<Impl>()) {}
Styles::~Styles() = default;

void Styles::add(const TextStyle style) {
    if (!style.bold && !style.underline) return;
    const std::lock_guard lock(impl_->mutex);
    auto& styles = impl_->styles;
    if (styles.size() != 0U) {
        auto last = styles.get(styles.size() - 1U);
        if (last.end == style.begin && last.bold == style.bold && last.underline == style.underline) {
            last.end = style.end;
            styles.set(styles.size() - 1U, last);
            return;
        }
    }
    styles.push(style);
}

void Styles::finish() {
    const std::lock_guard lock(impl_->mutex);
    impl_->styles.finish();
}

std::optional<TextStyle> Styles::at(const std::uint64_t position, const std::uint64_t length) const {
    const std::lock_guard lock(impl_->mutex);
    const auto& styles = impl_->styles;
    std::uint64_t begin = 0, end = styles.size();
    while (begin < end) {
        const auto middle = begin + (end - begin) / 2U;
        if (styles.get(middle).end <= position) begin = middle + 1U;
        else end = middle;
    }
    if (begin == styles.size()) return std::nullopt;
    const auto result = styles.get(begin);
    return result.begin < position + length ? std::optional<TextStyle>(result) : std::nullopt;
}

Decoded decode(const Data& input, const std::string& codepage, const Cancellation& cancellation) {
    Decoded result{std::make_shared<Data>(), {}};
    DataOutput output(*result.data);
    const auto& pages = supported_codepages();
    const auto single_byte = codepage != "UTF-8" &&
        std::any_of(pages.begin(), pages.end(), [&](const Codepage& page) { return page.id == codepage; });
    result.offsets = single_byte ? std::make_shared<OffsetMap>(result.data) : std::make_shared<OffsetMap>(true);
    result.offsets->add(0, 0);
    if (single_byte) {
        std::array<std::optional<std::string>, 256> table;
        std::array<bool, 256> identity{};
        for (std::size_t index = 0; index < table.size(); ++index) {
            try {
                table[index] = convert_encoding(std::string(1, static_cast<char>(index)), codepage, "UTF-8");
                identity[index] = *table[index] == std::string(1, static_cast<char>(index));
            }
            catch (const std::system_error& error) {
                if (error.code().value() != EILSEQ && error.code().value() != EINVAL) throw;
            }
        }
        for (std::uint64_t position = 0; position < input.size();) {
            cancellation.throw_if_requested();
            const auto bytes = input.bytes(position, block_size, cancellation);
            for (std::size_t index = 0; index < bytes.size();) {
                const auto begin = bytes.begin() + static_cast<std::ptrdiff_t>(index);
                const auto stop = std::find_if(begin, bytes.end(), [&](const unsigned char byte) {
                    return !identity[byte];
                });
                const auto count = static_cast<std::size_t>(stop - begin);
                if (count > 0U) {
                    for (std::size_t remaining = count; remaining > 0U;) {
                        const auto piece = static_cast<std::size_t>(std::min<std::uint64_t>(
                            remaining, 4096U - position % 4096U));
                        output.write(bytes.data() + index, static_cast<std::streamsize>(piece));
                        position += piece;
                        index += piece;
                        remaining -= piece;
                        if (position % 4096U == 0U) result.offsets->add(output.size(), position, true);
                    }
                    if (index == bytes.size()) break;
                }
                const auto& value = table[static_cast<unsigned char>(bytes[index])];
                if (!value) throw ConversionError(EILSEQ, std::generic_category(), "convert text from " + codepage);
                output.write(value->data(), static_cast<std::streamsize>(value->size()));
                ++position;
                ++index;
                if (position % 4096U == 0U) result.offsets->add(output.size(), position, true);
            }
        }
        result.offsets->add(output.size(), input.size(), true);
    } else {
        struct Conversion {
            iconv_t descriptor;
            explicit Conversion(const std::string& source) : descriptor(::iconv_open("UTF-32LE", source.c_str())) {
                if (descriptor == reinterpret_cast<iconv_t>(-1)) {
                    throw ConversionError(errno, std::generic_category(), "open viewer encoding conversion");
                }
            }
            ~Conversion() { ::iconv_close(descriptor); }
        } conversion(codepage);
        std::string pending;
        std::size_t first = 0;
        bool refill = true;
        std::uint64_t read_position = 0, consumed = 0, character_begin = 0;
        while (read_position < input.size() || first < pending.size()) {
            cancellation.throw_if_requested();
            if (refill && read_position < input.size()) {
                pending.erase(0, first);
                first = 0;
                auto bytes = input.bytes(read_position, block_size - pending.size(), cancellation);
                read_position += bytes.size();
                pending += bytes;
            }
            auto* source = pending.data() + first;
            auto available = pending.size() - first;
            const auto before = available;
            std::array<unsigned char, 4> character{};
            auto* target = reinterpret_cast<char*>(character.data());
            auto space = character.size();
            const auto status = ::iconv(conversion.descriptor, &source, &available, &target, &space);
            const auto failure = errno;
            const auto used = before - available;
            consumed += used;
            first += used;
            refill = first == pending.size();
            if (space == 0U) {
                const auto value = static_cast<std::uint32_t>(character[0]) |
                    (static_cast<std::uint32_t>(character[1]) << 8U) |
                    (static_cast<std::uint32_t>(character[2]) << 16U) |
                    (static_cast<std::uint32_t>(character[3]) << 24U);
                const auto encoded = utf8(value);
                const auto changed = encoded.size() != consumed - character_begin;
                result.offsets->add(output.size(), character_begin, changed);
                output.write(encoded.data(), static_cast<std::streamsize>(encoded.size()));
                result.offsets->add(output.size(), consumed, changed);
                character_begin = consumed;
            }
            if (status != static_cast<std::size_t>(-1)) continue;
            if (failure == E2BIG && space == 0U) continue;
            if (failure == EINVAL && read_position < input.size() && available < block_size) {
                refill = true;
                continue;
            }
            throw ConversionError(failure, std::generic_category(), "convert viewer text from " + codepage);
        }
        std::array<char, 16> final{};
        auto* target = final.data();
        auto available = final.size();
        if (::iconv(conversion.descriptor, nullptr, nullptr, &target, &available) == static_cast<std::size_t>(-1) ||
            available != final.size()) throw std::runtime_error("cannot finish viewer encoding conversion");
        result.offsets->add(output.size(), input.size(), true);
    }
    output.finish();
    result.offsets->finish();
    return result;
}

Formatted format_nroff(const Data& input, const Cancellation& cancellation) {
    struct Cell {
        std::uint64_t text;
        std::uint64_t length;
        std::uint64_t begin;
        std::uint64_t end;
        bool bold{false};
        bool underline{false};
    };
    Formatted result{
        std::make_shared<Data>(), std::make_shared<OffsetMap>(), std::make_shared<Styles>()
    };
    DataOutput output(*result.data);
    Records<Cell> cells;
    Cursor reader(input, cancellation);
    Cursor previous_reader(input, cancellation);
    for (std::uint64_t line = 0; line < input.size();) {
        cancellation.throw_if_requested();
        auto end = line;
        auto control = std::numeric_limits<std::uint64_t>::max();
        while (end < input.size()) {
            const auto bytes = reader.at(end);
            const auto newline = bytes.find('\n');
            const auto count = newline == std::string_view::npos ? bytes.size() : newline;
            if (control == std::numeric_limits<std::uint64_t>::max()) {
                const auto found = bytes.substr(0, count).find_first_of("\b\r");
                if (found != std::string_view::npos) control = end + found;
            }
            end += count;
            if (newline != std::string_view::npos) break;
        }
        if (control == std::numeric_limits<std::uint64_t>::max() ||
            (control + 1U == end && reader.at(control).front() == '\r')) {
            result.offsets->add(output.size(), line);
            const auto length = end - line - (control == std::numeric_limits<std::uint64_t>::max() ? 0U : 1U);
            input.read(line, length, output, cancellation);
            result.offsets->add(output.size(), end);
        } else {
            cells.clear();
            std::uint64_t cursor = 0;
            for (auto position = line; position < end;) {
                cancellation.throw_if_requested();
                const auto byte = reader.at(position).front();
                if (byte == '\r') { cursor = 0; ++position; continue; }
                if (byte == '\b') { if (cursor > 0U) --cursor; ++position; continue; }
                const auto next = reader.next(position);
                const auto length = next - position;
                Cell cell{position, length, position, next};
                if (cursor >= cells.size()) cells.push(cell);
                else {
                    auto previous = cells.get(cursor);
                    if (previous.length == length &&
                        equal(previous_reader, reader, previous.text, position, length, cancellation)) {
                        previous.bold = true;
                    } else if ((previous.length == 1U && previous_reader.at(previous.text).front() == '_') ||
                               (length == 1U && byte == '_')) {
                        if (!(length == 1U && byte == '_')) { previous.text = position; previous.length = length; }
                        previous.underline = true;
                    } else previous = cell;
                    previous.end = next;
                    cells.set(cursor, previous);
                }
                ++cursor;
                position = next;
            }
            for (std::uint64_t index = 0; index < cells.size(); ++index) {
                cancellation.throw_if_requested();
                const auto cell = cells.get(index);
                const auto begin = output.size();
                result.offsets->add(begin, cell.begin, cell.end - cell.begin != cell.length);
                reader.copy(cell.text, cell.length, output);
                result.offsets->add(output.size(), cell.end);
                result.styles->add({begin, output.size(), cell.bold, cell.underline});
            }
        }
        if (end < input.size()) {
            result.offsets->add(output.size(), end);
            output.put('\n');
            result.offsets->add(output.size(), end + 1U);
            line = end + 1U;
        } else line = end;
    }
    result.offsets->add(output.size(), input.size());
    output.finish();
    result.offsets->finish();
    result.styles->finish();
    return result;
}

std::shared_ptr<Data> decompress(
    const Data& input, const std::string_view decoder, const Cancellation& cancellation
) {
    std::string tool;
    if (decoder == "ugz") tool = "gzip";
    else if (decoder == "ubz2") tool = "bzip2";
    else if (decoder == "uxz") tool = "xz";
    else if (decoder == "uzst") tool = "zstd";
    else throw std::invalid_argument("unknown parsed-view decoder");
    auto result = std::make_shared<Data>();
    DataOutput output(*result);
    const auto command = capture_command_to(output, {tool, "-cd", "--", input.path()},
                                             ".", cancellation, 64U * 1024U);
    if (command.status != 0) {
        throw std::runtime_error(tool + " exited " + std::to_string(command.status) + ": " + command.error);
    }
    output.finish();
    return result;
}

std::optional<EditorSearchMatch> find(
    const Data& input, const std::string_view pattern, const std::uint64_t start,
    const EditorSearchOptions& options, const bool wrap, const bool all_charsets,
    const Cancellation& cancellation
) {
    auto best = find_one(input, pattern, start, options, wrap, cancellation);
    if (!all_charsets || options.mode == EditorSearchMode::hexadecimal) return best;
    if (!input.non_ascii()) return best;
    const auto anchor = std::min(start, input.size());
    const auto distance = [&](const EditorSearchMatch& match) {
        if (options.backwards) return match.end <= anchor ? anchor - match.end : anchor + input.size() - match.end;
        return match.begin >= anchor ? match.begin - anchor : input.size() - anchor + match.begin;
    };
    for (const auto& page : supported_codepages()) {
        if (page.id == "UTF-8") continue;
        cancellation.throw_if_requested();
        try {
            const auto decoded = decode(input, page.id, cancellation);
            auto candidate = find_one(*decoded.data, pattern, decoded.offsets->view_position(anchor),
                                       options, wrap, cancellation);
            if (!candidate) continue;
            const auto source = [&](const std::size_t position) {
                return position == std::string::npos ? std::string::npos
                    : static_cast<std::size_t>(decoded.offsets->source_position(position));
            };
            candidate->begin = source(candidate->begin);
            candidate->end = source(candidate->end);
            for (auto& [begin, end] : candidate->captures) { begin = source(begin); end = source(end); }
            if (!best || distance(*candidate) < distance(*best)) best = std::move(candidate);
        } catch (const ConversionError& error) {
            if (error.code().value() != EILSEQ && error.code().value() != EINVAL) throw;
        }
    }
    return best;
}

std::uint64_t next_character(
    const Data& input, const std::uint64_t offset, const Cancellation& cancellation
) {
    if (offset >= input.size()) return input.size() + 1U;
    Cursor cursor(input, cancellation);
    return cursor.next(offset);
}

}  // namespace mcpp::viewer_detail
