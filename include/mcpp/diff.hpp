#pragma once

#include "mcpp/editor.hpp"

#include <cstddef>
#include <optional>
#include <vector>

namespace mcpp {

struct DiffOptions {
    std::size_t quality{0};
    bool ignore_case{false};
    bool ignore_tab_expansion{false};
    bool ignore_space_change{false};
    bool ignore_all_space{false};
    bool strip_trailing_cr{false};
};

struct DiffHunk {
    std::size_t left_begin;
    std::size_t left_count;
    std::size_t right_begin;
    std::size_t right_count;
    std::size_t display_row{0};
};

struct DiffRow {
    std::optional<std::size_t> left;
    std::optional<std::size_t> right;
    std::optional<std::size_t> hunk;
};

class DiffModel {
public:
    void compare(const EditorModel& left, const EditorModel& right, const DiffOptions& options = {},
                 const Cancellation& cancellation = {});
    void merge(std::size_t hunk, bool left_to_right, EditorModel& left, EditorModel& right) const;
    [[nodiscard]] const std::vector<DiffHunk>& hunks() const noexcept { return hunks_; }
    [[nodiscard]] const std::vector<DiffRow>& rows() const noexcept { return rows_; }
    [[nodiscard]] static std::size_t line_count(const EditorModel& model);

private:
    std::vector<DiffHunk> hunks_;
    std::vector<DiffRow> rows_;
};

}  // namespace mcpp
