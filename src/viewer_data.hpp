#pragma once

#include "mcpp/cancellation.hpp"
#include "mcpp/editor_search.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <ostream>
#include <streambuf>
#include <string>
#include <string_view>

namespace mcpp::viewer_detail {

[[nodiscard]] int private_spool();

class Data {
public:
    Data();
    ~Data();
    Data(const Data&) = delete;
    Data& operator=(const Data&) = delete;
    [[nodiscard]] int descriptor() const noexcept { return descriptor_; }
    [[nodiscard]] std::uint64_t size() const noexcept { return size_; }
    [[nodiscard]] bool non_ascii() const noexcept { return non_ascii_; }
    [[nodiscard]] std::string path() const;
    void append(std::string_view bytes);
    void write(std::uint64_t offset, std::string_view bytes);
    void resize(std::uint64_t size);
    void read(std::uint64_t offset, std::uint64_t length, std::ostream& output,
              const Cancellation& cancellation = {}) const;
    [[nodiscard]] std::string bytes(std::uint64_t offset, std::size_t length,
                                    const Cancellation& cancellation = {}) const;
private:
    int descriptor_;
    std::uint64_t size_{0};
    bool non_ascii_{false};
};

class DataOutput final : public std::ostream {
public:
    explicit DataOutput(Data& data);
    void finish();
    [[nodiscard]] std::uint64_t size() const noexcept;
private:
    class Buffer final : public std::streambuf {
    public:
        explicit Buffer(Data& data);
        [[nodiscard]] std::uint64_t size() const noexcept;
    protected:
        int sync() override;
        int_type overflow(int_type value) override;
        std::streamsize xsputn(const char* bytes, std::streamsize length) override;
    private:
        Data& data_;
        std::array<char, 64U * 1024U> buffer_{};
    };
    Buffer buffer_;
};

using ReadRange = std::function<void(
    std::uint64_t, std::uint64_t, std::ostream&, const Cancellation&
)>;

[[nodiscard]] std::shared_ptr<Data> capture(
    std::uint64_t size, const ReadRange& read, const Cancellation& cancellation
);

class OffsetMap {
public:
    explicit OffsetMap(bool characters = false);
    explicit OffsetMap(std::shared_ptr<const Data> decoded);
    ~OffsetMap();
    OffsetMap(const OffsetMap&) = delete;
    OffsetMap& operator=(const OffsetMap&) = delete;
    void add(std::uint64_t view, std::uint64_t source, bool force = false);
    void finish();
    [[nodiscard]] std::uint64_t source_position(std::uint64_t view) const;
    [[nodiscard]] std::uint64_t view_position(std::uint64_t source) const;
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

struct TextStyle {
    std::uint64_t begin;
    std::uint64_t end;
    bool bold;
    bool underline;
};

class Styles {
public:
    Styles();
    ~Styles();
    Styles(const Styles&) = delete;
    Styles& operator=(const Styles&) = delete;
    void add(TextStyle style);
    void finish();
    [[nodiscard]] std::optional<TextStyle> at(std::uint64_t position, std::uint64_t length) const;
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

struct Decoded {
    std::shared_ptr<Data> data;
    std::shared_ptr<OffsetMap> offsets;
};

struct Formatted {
    std::shared_ptr<Data> data;
    std::shared_ptr<OffsetMap> offsets;
    std::shared_ptr<Styles> styles;
};

[[nodiscard]] Decoded decode(
    const Data& input, const std::string& codepage, const Cancellation& cancellation
);
[[nodiscard]] Formatted format_nroff(const Data& input, const Cancellation& cancellation);
[[nodiscard]] std::shared_ptr<Data> decompress(
    const Data& input, std::string_view decoder, const Cancellation& cancellation
);
[[nodiscard]] std::optional<EditorSearchMatch> find(
    const Data& input, std::string_view pattern, std::uint64_t start,
    const EditorSearchOptions& options, bool wrap, bool all_charsets,
    const Cancellation& cancellation
);
[[nodiscard]] std::uint64_t next_character(
    const Data& input, std::uint64_t offset, const Cancellation& cancellation
);

}  // namespace mcpp::viewer_detail
