#pragma once

#include "mcpp/provider.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mcpp {

enum class ViewerLinkedSave { require_choice, detach, preserve_links };

class ViewerModel {
public:
    void open(
        ProviderPtr provider,
        std::string path,
        const Cancellation& cancellation = {}
    );
    void close();
    void go_to_start();
    void go_to_end(std::size_t viewport_width, std::size_t viewport_lines);
    void scroll_lines(int amount, std::size_t viewport_width);
    void page(
        int direction,
        std::size_t viewport_width,
        std::size_t viewport_lines
    );
    void scroll_columns(int amount);
    void toggle_hex();
    void goto_offset(std::uint64_t offset);
    void goto_line(std::uint64_t line, const Cancellation& cancellation = {});
    void goto_percent(unsigned int percent);
    [[nodiscard]] std::uint64_t line_number(const Cancellation& cancellation = {});
    [[nodiscard]] unsigned int position_percent() const noexcept;
    void toggle_hex_editing();
    [[nodiscard]] bool hex_editing() const noexcept;
    [[nodiscard]] bool hex_ascii() const noexcept;
    void toggle_hex_navigation();
    [[nodiscard]] std::uint64_t hex_cursor() const noexcept;
    [[nodiscard]] bool hex_low_nibble() const noexcept;
    void set_hex_cursor(std::uint64_t offset, bool ascii, bool low_nibble = false);
    void move_hex_cursor(long long delta);
    void scroll_hex_lines(int amount);
    void edit_hex_digit(char digit);
    void edit_ascii_byte(char byte);
    [[nodiscard]] bool dirty() const noexcept;
    [[nodiscard]] std::uintmax_t hardlink_count() const;
    void save_changes(
        const Cancellation& cancellation = {},
        ViewerLinkedSave linked = ViewerLinkedSave::require_choice
    );
    void discard_changes();
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
    // Byte range in the last rendered row, after tab expansion and clipping.
    [[nodiscard]] std::optional<std::pair<std::size_t, std::size_t>>
    text_highlight(std::size_t row) const noexcept;
    [[nodiscard]] bool search_can_wrap() const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> find(
        std::string_view needle,
        bool case_sensitive,
        const Cancellation& cancellation = {},
        bool wrap = true
    );

private:
    [[nodiscard]] std::vector<std::uint64_t> text_offsets_before(
        std::uint64_t end,
        std::size_t width,
        std::size_t count
    );
    [[nodiscard]] std::string_view window_at(
        std::uint64_t offset,
        std::size_t minimum,
        const Cancellation& cancellation
    );
    [[nodiscard]] std::uint64_t read_text_row(
        std::uint64_t offset,
        std::size_t width,
        std::string* output,
        const Cancellation& cancellation,
        std::optional<std::pair<std::size_t, std::size_t>>* highlight = nullptr
    );
    void load_window(
        std::uint64_t offset,
        std::size_t size,
        const Cancellation& cancellation
    );
    void set_byte(std::uint64_t offset, unsigned char value);
    void keep_hex_cursor_visible();
    void navigation_changed();
    [[nodiscard]] std::uint64_t last_text_page(std::size_t width, std::size_t rows);

    struct LastTextPage {
        std::size_t width;
        std::size_t rows;
        bool wrap;
        std::uint64_t offset;
    };

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
    std::optional<std::pair<std::uint64_t, std::uint64_t>> search_match_;
    std::vector<std::optional<std::pair<std::size_t, std::size_t>>> text_highlights_;
    std::uint64_t next_search_offset_{0};
    std::uint64_t search_begin_{0};
    bool hex_editing_{false};
    bool hex_ascii_{false};
    std::uint64_t hex_cursor_{0};
    bool hex_low_nibble_{false};
    std::size_t viewport_width_{80};
    std::size_t viewport_rows_{1};
    std::optional<LastTextPage> last_text_page_;
    std::map<std::uint64_t, unsigned char> edits_;
    std::string revision_;
};

}  // namespace mcpp
