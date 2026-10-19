#pragma once

#include "mcpp/provider.hpp"
#include "mcpp/editor_search.hpp"

#include <cstddef>
#include <compare>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mcpp {

struct EditorPosition {
    std::size_t row{0};
    std::size_t column{0};

    auto operator<=>(const EditorPosition&) const = default;
};

enum class EditorLineEnding { unchanged, lf, crlf, cr };
enum class EditorSaveMode { atomic, in_place, backup };

class EditorModel {
public:
    void open(
        ProviderPtr provider,
        std::string path,
        const Cancellation& cancellation = {}
    );
    void create(
        ProviderPtr provider,
        std::string path
    );
    void close();
    [[nodiscard]] bool open() const noexcept;
    [[nodiscard]] bool dirty() const noexcept;
    [[nodiscard]] const std::string& path() const noexcept;
    [[nodiscard]] const std::vector<std::string>& lines() const noexcept;
    [[nodiscard]] std::size_t row() const noexcept;
    [[nodiscard]] std::size_t column() const noexcept;
    [[nodiscard]] std::size_t byte_size() const noexcept;
    [[nodiscard]] std::size_t cursor_offset() const noexcept;
    [[nodiscard]] std::size_t storage_byte_size() const;
    [[nodiscard]] std::size_t storage_cursor_offset() const;
    [[nodiscard]] std::size_t storage_selection_size() const;
    [[nodiscard]] const std::string& encoding() const noexcept { return encoding_; }
    void set_encoding(std::string codepage);
    [[nodiscard]] std::size_t selection_size() const;
    [[nodiscard]] std::uint64_t change_id() const noexcept;
    [[nodiscard]] char line_separator() const noexcept { return line_separator_; }
    void set_save_mode(EditorSaveMode mode) noexcept { save_mode_ = mode; }
    [[nodiscard]] EditorSaveMode save_mode() const noexcept { return save_mode_; }
    void set_backup_extension(std::string value);
    [[nodiscard]] bool marking() const noexcept;
    [[nodiscard]] bool overwrite() const noexcept;
    [[nodiscard]] bool column_selection() const noexcept;
    [[nodiscard]] std::optional<std::pair<
        EditorPosition,
        EditorPosition
    >> selection() const noexcept;
    [[nodiscard]] std::optional<std::pair<EditorPosition, EditorPosition>>
    search_selection() const;
    [[nodiscard]] std::string text() const { return content(); }
    [[nodiscard]] std::string text_range(EditorPosition begin, EditorPosition end) const;
    void replace_text(EditorPosition begin, EditorPosition end, std::string_view value);
    void replace_block(std::string_view value);
    void set_text(std::string_view value);
    void toggle_bookmark();
    [[nodiscard]] bool jump_bookmark(int direction);
    void clear_bookmarks() noexcept;
    void set_bookmarks(std::set<std::size_t> rows);
    [[nodiscard]] const std::set<std::size_t>& bookmarks() const noexcept { return bookmarks_; }

    void move(int rows, int columns);
    void move_word(int direction);
    void page(int rows);
    void home();
    void end();
    void goto_line(std::size_t line);
    void set_position(EditorPosition position);
    void toggle_mark();
    void toggle_column_mark();
    void begin_mark();
    void begin_column_mark();
    void resume_mark();
    void finish_mark();
    void clear_selection();
    void select_all();
    [[nodiscard]] bool match_bracket();
    void toggle_overwrite() noexcept;
    void set_tab_width(std::size_t width);
    [[nodiscard]] std::size_t tab_width() const noexcept;
    void insert(char character);
    void insert(std::string_view text);
    void paste(std::string_view text);
    void paste_bytes(std::string_view bytes);
    void newline(bool auto_indent = false);
    void tab(std::size_t indent_width, bool fill_spaces = false);
    void backspace(std::size_t indent_width = 0);
    void erase();
    void delete_block_or_line();
    void delete_line();
    void delete_to_end();
    [[nodiscard]] bool copy_selection();
    [[nodiscard]] bool move_selection();
    [[nodiscard]] bool delete_selection();
    [[nodiscard]] std::optional<std::string> selected_text() const;
    [[nodiscard]] std::optional<std::string> selected_bytes() const;
    [[nodiscard]] std::optional<std::pair<
        std::size_t,
        std::size_t
    >> selection_on_line(std::size_t row) const;
    void paste_column(std::string_view text);
    [[nodiscard]] bool replace_selection(
        std::string_view replacement,
        std::optional<std::pair<
            EditorPosition,
            EditorPosition
        >>* scope = nullptr
    );
    bool undo();
    bool redo();
    void save(
        bool force,
        const Cancellation& cancellation = {}
    );
    [[nodiscard]] std::uintmax_t hardlink_count() const;
    void save_preserving_links(
        bool force = false,
        const Cancellation& cancellation = {}
    );
    void save_as(
        std::string path,
        bool force,
        const Cancellation& cancellation = {}
    );
    void save_as(
        std::string path,
        bool force,
        EditorLineEnding ending,
        const Cancellation& cancellation = {}
    );
    void save_as_preserving_links(
        std::string path,
        bool force,
        EditorLineEnding ending,
        const Cancellation& cancellation = {}
    );
    [[nodiscard]] bool find(
        std::string_view needle,
        bool case_sensitive,
        bool wrap = true,
        bool overlap = false
    );
    [[nodiscard]] bool find(
        std::string_view needle,
        const EditorSearchOptions& options,
        bool wrap,
        bool overlap,
        std::optional<std::pair<
            EditorPosition,
            EditorPosition
        >> scope = std::nullopt
    );
    [[nodiscard]] std::size_t replace_all(
        std::string_view needle,
        std::string_view replacement,
        bool case_sensitive
    );
    [[nodiscard]] std::size_t replace_all(
        std::string_view needle,
        std::string_view replacement,
        const EditorSearchOptions& options,
        std::optional<std::pair<
            EditorPosition,
            EditorPosition
        >> scope = std::nullopt
    );
    [[nodiscard]] std::string replacement_for_selection(
        std::string_view needle,
        std::string_view replacement,
        const EditorSearchOptions& options
    ) const;
    [[nodiscard]] std::size_t count_matches(
        std::string_view needle,
        const EditorSearchOptions& options,
        std::optional<std::pair<
            EditorPosition,
            EditorPosition
        >> scope = std::nullopt
    ) const;
    [[nodiscard]] bool replace(
        std::string_view needle,
        std::string_view replacement,
        bool case_sensitive
    );

private:
    void save_as_impl(
        std::string path, bool force, EditorLineEnding ending,
        bool preserve_links, const Cancellation& cancellation
    );
    void backup_target(
        const std::string& storage, const std::string& path,
        const std::optional<std::string>& expected,
        const Cancellation& cancellation
    );
    struct Snapshot {
        std::size_t row{0};
        std::size_t column{0};
        std::optional<EditorPosition> selection_begin;
        std::optional<EditorPosition> selection_end;
        bool marking{false};
        bool column_selection{false};
        std::optional<std::size_t> column_selection_width;
        std::size_t byte_size{0};
        char line_separator{'\n'};
    };
    struct UndoRecord {
        Snapshot before;
        Snapshot after;
        std::size_t offset{0};
        std::string removed;
        std::string inserted;
        [[nodiscard]] std::size_t bytes() const noexcept {
            return removed.size() + inserted.size();
        }
    };

    [[nodiscard]] std::string content() const;
    [[nodiscard]] std::string encode_content(std::string_view value) const;
    void update_storage_offsets() const;
    [[nodiscard]] std::string search_content() const;
    void validate_search_options(const EditorSearchOptions& options) const;
    [[nodiscard]] std::string normalize_insert(std::string_view value) const;
    void set_content(std::string_view content);
    [[nodiscard]] std::size_t offset(EditorPosition position) const;
    [[nodiscard]] EditorPosition position(std::size_t offset) const;
    void update_mark();
    [[nodiscard]] std::pair<std::size_t, std::size_t>
    column_selection_columns() const;
    void mark_column_block(
        std::size_t row,
        std::size_t height,
        std::size_t left,
        std::size_t width
    );
    void replace_range(
        EditorPosition begin,
        EditorPosition end,
        std::string_view replacement
    );
    void insert_text(
        std::string_view text,
        bool overwrite,
        bool include_selection_end = false
    );
    void erase_text(EditorPosition begin, EditorPosition end);
    void edit_text(EditorPosition begin, EditorPosition end, std::string_view replacement);
    void record_undo();
    void finalize_undo();
    [[nodiscard]] std::optional<EditorSearchMatch> selected_match() const;
    [[nodiscard]] Snapshot snapshot() const;
    void trim_undo();
    void restore(Snapshot snapshot);

    ProviderPtr provider_;
    std::string path_;
    std::string storage_path_;
    std::string revision_;
    std::string encoding_;
    mutable std::uint64_t storage_change_id_{0};
    mutable std::vector<std::size_t> storage_offsets_;
    std::vector<std::string> lines_;
    std::set<std::size_t> bookmarks_;
    std::size_t row_{0};
    std::size_t column_{0};
    std::optional<std::size_t> preferred_display_column_;
    bool dirty_{false};
    std::uint64_t change_id_{0};
    std::optional<EditorPosition> selection_begin_;
    std::optional<EditorPosition> selection_end_;
    bool marking_{false};
    bool column_selection_{false};
    std::optional<std::size_t> column_selection_width_;
    bool overwrite_{false};
    bool new_file_{false};
    EditorSaveMode save_mode_{EditorSaveMode::atomic};
    std::string backup_extension_{"~"};
    char line_separator_{'\n'};
    std::size_t tab_width_{8};
    std::string clipboard_;
    std::vector<UndoRecord> undo_;
    std::vector<UndoRecord> redo_;
    std::optional<Snapshot> pending_undo_;
    std::string pending_content_;
    std::optional<EditorSearchMatch> search_match_;
    std::string search_match_pattern_;
    EditorSearchOptions search_match_options_;
    std::uint64_t search_match_change_id_{0};
    std::optional<std::size_t> search_resume_;
    std::size_t undo_bytes_{0};
    std::size_t redo_bytes_{0};
};

}  // namespace mcpp
