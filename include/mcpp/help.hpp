#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mcpp {

enum class HelpTopic {
    index,
    browser,
    manager,
    panels,
    selection,
    command_line,
    copy_move,
    delete_files,
    jobs,
    find_files,
    tree,
    hotlist,
    viewer,
    viewer_search,
    viewer_edit,
    quick_view,
    editor,
    editor_blocks,
    editor_search,
    editor_save,
    editor_windows,
    diff,
    dialogs,
    settings,
    keymaps,
    archives,
    permissions,
};

struct HelpBlock {
    std::string_view text;
    std::optional<HelpTopic> target;
    bool heading{false};
};

struct HelpPage {
    HelpTopic topic;
    std::string_view title;
    std::vector<HelpBlock> blocks;
};

[[nodiscard]] std::span<const HelpPage> help_pages();
[[nodiscard]] const HelpPage& help_page(HelpTopic topic);

struct HelpLine {
    std::string text;
    std::optional<std::size_t> link;
    bool heading{false};
};

struct HelpLink {
    HelpTopic target;
    std::size_t first_line;
    std::size_t last_line;
};

class HelpBrowser {
public:
    explicit HelpBrowser(HelpTopic topic = HelpTopic::index);

    void resize(std::size_t columns, std::size_t rows);
    void scroll(int direction, std::size_t lines = 1);
    void page(int direction);
    void top();
    void bottom();
    void next_link(bool backwards = false);
    bool follow();
    bool follow_link(std::size_t link);
    bool back();
    void open(HelpTopic topic);

    [[nodiscard]] HelpTopic topic() const noexcept;
    [[nodiscard]] std::span<const HelpLine> lines() const noexcept;
    [[nodiscard]] std::span<const HelpLink> links() const noexcept;
    [[nodiscard]] std::size_t first_line() const noexcept;
    [[nodiscard]] std::size_t page_rows() const noexcept;
    [[nodiscard]] std::optional<std::size_t> selected_link() const noexcept;
    [[nodiscard]] bool can_go_back() const noexcept;

private:
    struct Anchor {
        std::size_t block{0};
        std::size_t byte{0};
    };
    struct Position {
        HelpTopic topic;
        Anchor anchor;
        std::optional<std::size_t> selected;
    };

    [[nodiscard]] Position position() const;
    [[nodiscard]] std::size_t last_top() const noexcept;
    [[nodiscard]] bool visible(std::size_t link) const;
    void layout();
    void restore(const Position& position);
    void select_visible();
    void reveal_selected();

    HelpTopic topic_;
    std::size_t columns_{72};
    std::size_t rows_{16};
    std::size_t first_{0};
    std::optional<std::size_t> selected_;
    std::vector<HelpLine> lines_;
    std::vector<Anchor> anchors_;
    std::vector<HelpLink> links_;
    std::vector<Position> history_;
};

}  // namespace mcpp
