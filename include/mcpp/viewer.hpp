#pragma once

#include "mcpp/provider.hpp"
#include "mcpp/editor_search.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mcpp {

namespace viewer_detail {
class Data;
class OffsetMap;
class Styles;
}

enum class ViewerLinkedSave { require_choice, detach, preserve_links };

struct ViewerTextStyle {
    std::size_t begin;
    std::size_t end;
    bool bold;
    bool underline;
};

class ViewerModel {
public:
    void open(
        ProviderPtr provider,
        std::string path,
        const Cancellation& cancellation = {}
    );
    // Duplicates the descriptor; the caller retains ownership.
    void open_stream(
        ProviderPtr provider,
        int descriptor,
        std::string name,
        const Cancellation& cancellation = {}
    );
    void close();
    [[nodiscard]] bool refresh(const Cancellation& cancellation = {});
    [[nodiscard]] bool streamed() const noexcept { return stream_ != nullptr; }
    [[nodiscard]] bool reading() const noexcept;
    [[nodiscard]] const std::string& source_error() const noexcept { return source_error_; }
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
    void toggle_magic(const Cancellation& cancellation = {});
    void toggle_nroff(const Cancellation& cancellation = {});
    void set_encoding(std::string codepage, const Cancellation& cancellation = {});
    [[nodiscard]] bool magic() const noexcept { return magic_; }
    [[nodiscard]] bool nroff() const noexcept { return nroff_; }
    [[nodiscard]] bool parsed() const noexcept { return magic_content_ != nullptr; }
    [[nodiscard]] const std::string& encoding() const noexcept { return encoding_; }
    void select_bookmark(std::size_t slot);
    void set_bookmark();
    void goto_bookmark();
    [[nodiscard]] std::size_t bookmark_slot() const noexcept { return bookmark_slot_; }
    void goto_offset(std::uint64_t offset);
    void goto_data_offset(std::uint64_t offset);
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
    [[nodiscard]] std::uint64_t source_view_end() const;
    [[nodiscard]] std::uint64_t data_size() const noexcept;
    [[nodiscard]] std::size_t horizontal_offset() const noexcept;
    [[nodiscard]] const std::string& path() const noexcept;
    [[nodiscard]] const ProviderPtr& provider() const noexcept { return provider_; }
    [[nodiscard]] std::vector<std::string> lines(
        std::size_t width,
        std::size_t height,
        const Cancellation& cancellation = {}
    );
    // Byte range in the last rendered row, after tab expansion and clipping.
    [[nodiscard]] std::optional<std::pair<std::size_t, std::size_t>>
    text_highlight(std::size_t row) const noexcept;
    [[nodiscard]] const std::vector<ViewerTextStyle>& text_styles(std::size_t row) const noexcept;
    [[nodiscard]] bool search_can_wrap() const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> find(
        std::string_view needle,
        bool case_sensitive,
        const Cancellation& cancellation = {},
        bool wrap = true
    );
    [[nodiscard]] std::optional<std::uint64_t> find(
        std::string_view needle,
        const EditorSearchOptions& options,
        const Cancellation& cancellation = {},
        bool wrap = true,
        bool all_charsets = false
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
        std::optional<std::pair<std::size_t, std::size_t>>* highlight = nullptr,
        std::vector<ViewerTextStyle>* styles = nullptr
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
    void read_view(std::uint64_t offset, std::uint64_t length, std::ostream& output,
                   const Cancellation& cancellation);
    void read_source(std::uint64_t offset, std::uint64_t length, std::ostream& output,
                     const Cancellation& cancellation);
    void validate_edit_source(const Cancellation& cancellation) const;
    void validate_snapshot_source(const Cancellation& cancellation) const;
    [[nodiscard]] std::shared_ptr<viewer_detail::Data> capture_source(const Cancellation& cancellation);
    [[nodiscard]] std::shared_ptr<viewer_detail::Data> search_data(const Cancellation& cancellation);
    void refresh_presentation(bool magic, bool nroff, std::string codepage, const Cancellation& cancellation);
    void select_presentation();
    [[nodiscard]] const viewer_detail::Data* presentation() const noexcept;
    [[nodiscard]] std::uint64_t source_position(std::uint64_t position) const;
    void set_view_end(std::uint64_t position);
    [[nodiscard]] std::optional<std::uint64_t> reveal_search_match(
        std::pair<std::uint64_t, std::uint64_t> match, const Cancellation& cancellation);

    struct LastTextPage {
        std::size_t width;
        std::size_t rows;
        bool wrap;
        std::uint64_t offset;
    };

    struct StreamSource;
    ProviderPtr provider_;
    std::shared_ptr<StreamSource> stream_;
    std::string path_;
    std::string storage_path_;
    std::uint64_t size_{0};
    std::uint64_t source_size_{0};
    bool magic_{true};
    bool nroff_{false};
    std::string encoding_;
    std::shared_ptr<viewer_detail::Data> magic_content_;
    std::shared_ptr<viewer_detail::Data> text_content_;
    bool presentation_dirty_{false};
    std::shared_ptr<viewer_detail::Styles> presentation_styles_;
    std::shared_ptr<viewer_detail::OffsetMap> nroff_offsets_;
    std::shared_ptr<viewer_detail::OffsetMap> codepage_offsets_;
    std::shared_ptr<viewer_detail::Data> search_content_;
    std::vector<std::vector<ViewerTextStyle>> text_style_rows_;
    std::array<std::uint64_t, 10> bookmarks_{};
    std::size_t bookmark_slot_{0};
    std::uint64_t offset_{0};
    std::uint64_t view_end_{0};
    std::uint64_t source_view_end_{0};
    std::uint64_t window_offset_{0};
    std::string window_;
    std::vector<std::uint64_t> previous_offsets_;
    std::size_t horizontal_offset_{0};
    bool hex_{false};
    bool wrap_{true};
    std::string last_search_;
    bool last_search_case_sensitive_{false};
    EditorSearchOptions last_search_options_;
    std::string advanced_search_pattern_;
    bool search_boundary_exhausted_{false};
    bool last_search_all_charsets_{false};
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
    bool source_conflict_{false};
    std::string source_error_;
};

}  // namespace mcpp
