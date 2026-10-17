#pragma once

#include "mcpp/provider.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mcpp {

class ViewerModel {
public:
    void open(
        ProviderPtr provider,
        std::string path,
        const Cancellation& cancellation = {}
    );
    void close();
    void scroll_lines(int amount, std::size_t viewport_width);
    void page(
        int direction,
        std::size_t viewport_width,
        std::size_t viewport_lines
    );
    void scroll_columns(int amount);
    void toggle_hex();
    void set_wrap(bool value);
    [[nodiscard]] bool wrap() const noexcept;
    [[nodiscard]] bool hex() const noexcept;
    [[nodiscard]] bool open() const noexcept;
    [[nodiscard]] std::uint64_t size() const noexcept;
    [[nodiscard]] std::uint64_t offset() const noexcept;
    [[nodiscard]] std::uint64_t view_end() const noexcept;
    [[nodiscard]] std::size_t horizontal_offset() const noexcept;
    [[nodiscard]] const std::string& path() const noexcept;
    [[nodiscard]] std::vector<std::string> lines(
        std::size_t width,
        std::size_t height,
        const Cancellation& cancellation = {}
    );
    [[nodiscard]] std::optional<std::uint64_t> find(
        std::string_view needle,
        bool case_sensitive,
        const Cancellation& cancellation = {}
    );

private:
    void load_window(
        std::uint64_t offset,
        std::size_t size,
        const Cancellation& cancellation
    );

    ProviderPtr provider_;
    std::string path_;
    std::string storage_path_;
    std::uint64_t size_{0};
    std::uint64_t offset_{0};
    std::uint64_t view_end_{0};
    std::uint64_t window_offset_{0};
    std::string window_;
    std::vector<std::uint64_t> previous_offsets_;
    std::size_t horizontal_offset_{0};
    bool hex_{false};
    bool wrap_{true};
    std::string last_search_;
    bool last_search_case_sensitive_{false};
    std::uint64_t next_search_offset_{0};
};

}  // namespace mcpp
