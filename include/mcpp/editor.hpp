#pragma once

#include "mcpp/provider.hpp"
#include "mcpp/editor_search.hpp"

#include <cstddef>
#include <compare>
#include <cstdint>
#include <optional>
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

class EditorModel {
public:
    void open(
        ProviderPtr provider,
        std::string path,
        const Cancellation& cancellation = {}
    );
    void close();
    [[nodiscard]] bool open() const noexcept;
    [[nodiscard]] bool dirty() const noexcept;
    [[nodiscard]] const std::string& path() const noexcept;
    [[nodiscard]] const std::vector<std::string>& lines() const noexcept;
    [[nodiscard]] std::size_t row() const noexcept;
    [[nodiscard]] std::size_t column() const noexcept;
    [[nodiscard]] std::size_t byte_size() const noexcept;
    [[nodiscard]] std::uint64_t change_id() const noexcept;
    [[nodiscard]] bool marking() const noexcept;
    [[nodiscard]] bool overwrite() const noexcept;
    [[nodiscard]] std::optional<std::pair<
        EditorPosition,
        EditorPosition
    >> selection() const noexcept;

    void move(int rows, int columns);
    void move_word(int direction);
    void page(int rows);
    void home();
    void end();
    void goto_line(std::size_t line);
    void set_position(EditorPosition position);
    void toggle_mark();
    void begin_mark();
    void finish_mark();
    void clear_selection();
    void toggle_overwrite() noexcept;
    void insert(char character);
    void insert(std::string_view text);
    void paste(std::string_view text);
    void newline();
    void backspace();
    void erase();
    void delete_line();
    void delete_to_end();
    [[nodiscard]] bool copy_selection();
    [[nodiscard]] bool move_selection();
    [[nodiscard]] bool delete_selection();
    [[nodiscard]] std::optional<std::string> selected_text() const;
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
    void save_as(
        std::string path,
        bool force,
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
    struct Snapshot {
        std::vector<std::string> lines;
        std::size_t row{0};
        std::size_t column{0};
        std::optional<EditorPosition> selection_begin;
        std::optional<EditorPosition> selection_end;
        bool marking{false};
        std::size_t byte_size{0};
    };

    [[nodiscard]] std::string content() const;
    void set_content(std::string_view content);
    [[nodiscard]] std::size_t offset(EditorPosition position) const;
    [[nodiscard]] EditorPosition position(std::size_t offset) const;
    void update_mark();
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
    void record_undo();
    [[nodiscard]] Snapshot snapshot() const;
    void trim_undo();
    void restore(Snapshot snapshot);

    ProviderPtr provider_;
    std::string path_;
    std::string storage_path_;
    std::string revision_;
    std::vector<std::string> lines_;
    std::size_t row_{0};
    std::size_t column_{0};
    bool dirty_{false};
    std::uint64_t change_id_{0};
    std::optional<EditorPosition> selection_begin_;
    std::optional<EditorPosition> selection_end_;
    bool marking_{false};
    bool overwrite_{false};
    std::string clipboard_;
    std::vector<Snapshot> undo_;
    std::vector<Snapshot> redo_;
    std::size_t undo_bytes_{0};
    std::size_t redo_bytes_{0};
};

}  // namespace mcpp
