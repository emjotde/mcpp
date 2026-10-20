#pragma once

#include "viewer_data.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace mcpp::archive_detail {

using Data = viewer_detail::Data;

struct Extent {
    std::uint64_t offset;
    std::uint64_t length;
    std::uint64_t stored;
};

inline void storage_ranges(
    const int descriptor, const std::uint64_t offset, const std::uint64_t length,
    const std::function<void(std::uint64_t, std::uint64_t)>& range,
    const Cancellation& cancellation
) {
    const auto end = offset + length;
    auto position = offset;
    while (position < end) {
        cancellation.throw_if_requested();
        const auto data = ::lseek(descriptor, static_cast<off_t>(position), SEEK_DATA);
        if (data < 0) {
            if (errno == ENXIO) return;
            if (errno == EINVAL || errno == EOPNOTSUPP || errno == ENOTSUP) {
                range(position, end - position);
                return;
            }
            throw std::system_error(errno, std::generic_category(), "find archive snapshot data");
        }
        const auto begin = static_cast<std::uint64_t>(data);
        if (begin >= end) return;
        const auto hole = ::lseek(descriptor, data, SEEK_HOLE);
        if (hole < 0) {
            if (errno == EINVAL || errno == EOPNOTSUPP || errno == ENOTSUP) {
                range(begin, end - begin);
                return;
            }
            throw std::system_error(errno, std::generic_category(), "find archive snapshot hole");
        }
        const auto stop = std::min(end, static_cast<std::uint64_t>(hole));
        if (begin < position || stop <= begin) throw std::runtime_error("invalid archive snapshot extent");
        range(begin, stop - begin);
        position = stop;
    }
}

inline void copy_storage(
    const int descriptor, Data& output, const std::uint64_t offset,
    const std::uint64_t length, const std::uint64_t destination,
    const Cancellation& cancellation
) {
    std::array<char, 64U * 1024U> buffer{};
    storage_ranges(descriptor, offset, length, [&](auto position, auto remaining) {
        while (remaining > 0U) {
            cancellation.throw_if_requested();
            const auto count = ::pread(descriptor, buffer.data(),
                static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), remaining)),
                static_cast<off_t>(position));
            if (count < 0 && errno == EINTR) continue;
            if (count < 0) throw std::system_error(errno, std::generic_category(), "read archive snapshot");
            if (count == 0) throw std::runtime_error("archive snapshot ended early");
            output.write(destination + position - offset, {buffer.data(), static_cast<std::size_t>(count)});
            position += static_cast<std::uint64_t>(count);
            remaining -= static_cast<std::uint64_t>(count);
        }
    }, cancellation);
    if (output.size() < destination + length) output.resize(destination + length);
}

inline void write_storage(
    const Data& input, const int output, const std::uint64_t offset,
    const std::uint64_t length, const std::uint64_t destination,
    const Cancellation& cancellation
) {
    storage_ranges(input.descriptor(), offset, length, [&](auto position, auto remaining) {
        while (remaining > 0U) {
            cancellation.throw_if_requested();
            const auto bytes = input.bytes(position,
                static_cast<std::size_t>(std::min<std::uint64_t>(remaining, 64U * 1024U)), cancellation);
            if (bytes.empty()) throw std::runtime_error("archive snapshot ended early");
            for (std::size_t done = 0; done < bytes.size();) {
                const auto count = ::pwrite(output, bytes.data() + done, bytes.size() - done,
                    static_cast<off_t>(destination + position - offset + done));
                if (count < 0 && errno == EINTR) continue;
                if (count <= 0) throw std::system_error(errno, std::generic_category(), "write archive snapshot");
                done += static_cast<std::size_t>(count);
            }
            position += bytes.size();
            remaining -= bytes.size();
        }
    }, cancellation);
}

inline bool zero_range(
    const Data& input, const std::uint64_t offset, const std::uint64_t length,
    const Cancellation& cancellation
) {
    bool zero = true;
    storage_ranges(input.descriptor(), offset, length, [&](auto position, auto remaining) {
        while (zero && remaining > 0U) {
            const auto bytes = input.bytes(position,
                static_cast<std::size_t>(std::min<std::uint64_t>(remaining, 64U * 1024U)), cancellation);
            if (bytes.empty()) throw std::runtime_error("archive snapshot ended early");
            zero = std::all_of(bytes.begin(), bytes.end(), [](const char byte) { return byte == '\0'; });
            position += bytes.size();
            remaining -= bytes.size();
        }
    }, cancellation);
    return zero;
}

struct Content {
    std::shared_ptr<const Data> data;
    std::uint64_t length{0};
    std::vector<Extent> extents;

    [[nodiscard]] std::uint64_t size() const noexcept { return length; }
    [[nodiscard]] bool sparse() const noexcept {
        return length != 0U &&
            (extents.size() != 1U || extents.front().offset != 0U || extents.front().length != length);
    }
    [[nodiscard]] std::uint64_t stored_size() const noexcept {
        std::uint64_t total = 0;
        for (const auto& extent : extents) total += extent.length;
        return total;
    }
    void read(
        const std::uint64_t offset, const std::uint64_t count, std::ostream& output,
        const Cancellation& cancellation
    ) const {
        if (offset >= length) return;
        auto position = offset;
        const auto end = offset + std::min(count, length - offset);
        auto extent = std::lower_bound(extents.begin(), extents.end(), offset,
            [](const Extent& item, const std::uint64_t value) { return item.offset + item.length <= value; });
        const std::array<char, 64U * 1024U> zeros{};
        while (position < end) {
            cancellation.throw_if_requested();
            if (extent == extents.end() || position < extent->offset) {
                const auto stop = extent == extents.end() ? end : std::min(end, extent->offset);
                const auto size = static_cast<std::size_t>(std::min<std::uint64_t>(stop - position, zeros.size()));
                output.write(zeros.data(), static_cast<std::streamsize>(size));
                if (!output) throw std::runtime_error("cannot write archive member data");
                position += size;
            } else {
                const auto size = std::min(end - position, extent->offset + extent->length - position);
                data->read(extent->stored + position - extent->offset, size, output, cancellation);
                position += size;
                if (position == extent->offset + extent->length) ++extent;
            }
        }
    }
    void write_to(const int output, const Cancellation& cancellation) const {
        for (const auto& extent : extents) {
            write_storage(*data, output, extent.stored, extent.length, extent.offset, cancellation);
        }
        if (::ftruncate(output, static_cast<off_t>(length)) != 0) {
            throw std::system_error(errno, std::generic_category(), "size archive member snapshot");
        }
    }
};

inline std::shared_ptr<const Content> content_range(
    std::shared_ptr<const Data> input, const std::uint64_t offset, const std::uint64_t length,
    const std::size_t maximum_extents, const Cancellation& cancellation
) {
    if (offset > input->size() || length > input->size() - offset) {
        throw std::runtime_error("archive member exceeds its snapshot");
    }
    auto content = std::make_shared<Content>();
    content->data = std::move(input);
    content->length = length;
    storage_ranges(content->data->descriptor(), offset, length, [&](const auto begin, const auto count) {
        if (content->extents.size() >= maximum_extents) throw std::runtime_error("archive exceeds the extent limit");
        content->extents.push_back({begin - offset, count, begin});
    }, cancellation);
    return content;
}

inline bool equal(
    const Content& left, const Content& right, const Cancellation& cancellation
) {
    if (left.size() != right.size()) return false;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
    for (const auto* content : {&left, &right}) {
        for (const auto& extent : content->extents) {
            ranges.emplace_back(extent.offset, extent.offset + extent.length);
        }
    }
    std::sort(ranges.begin(), ranges.end());
    std::uint64_t compared = 0;
    for (const auto& [begin, end] : ranges) {
        for (auto position = std::max(begin, compared); position < end;) {
            cancellation.throw_if_requested();
            const auto count = std::min<std::uint64_t>(end - position, 64U * 1024U);
            std::ostringstream left_bytes;
            std::ostringstream right_bytes;
            left.read(position, count, left_bytes, cancellation);
            right.read(position, count, right_bytes, cancellation);
            if (left_bytes.str() != right_bytes.str()) return false;
            position += count;
        }
        compared = std::max(compared, end);
    }
    return true;
}

inline bool equal(
    const Data& left, const Data& right, const Cancellation& cancellation
) {
    if (left.size() != right.size()) return false;
    const auto alias = [](const Data& data) {
        return std::shared_ptr<const Data>(&data, [](const Data*) {});
    };
    return equal(*content_range(alias(left), 0, left.size(), std::numeric_limits<std::size_t>::max(), cancellation),
                 *content_range(alias(right), 0, right.size(), std::numeric_limits<std::size_t>::max(), cancellation),
                 cancellation);
}

}  // namespace mcpp::archive_detail
