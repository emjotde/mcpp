#include "mcpp/editor.hpp"
#include "mcpp/editor_search.hpp"
#include "mcpp/local_provider.hpp"

#include <clocale>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

void check(const bool value, const std::string_view message) {
    if (!value) {
        throw std::runtime_error(std::string(message));
    }
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    check(static_cast<bool>(input), "open saved fixture");
    return {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>(),
    };
}

mcpp::EditorModel load(
    const std::filesystem::path& path,
    const std::string_view content
) {
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(content.data(), static_cast<std::streamsize>(content.size()));
        check(static_cast<bool>(output), "write fixture");
    }
    mcpp::EditorModel editor;
    editor.open(
        std::make_shared<mcpp::LocalProvider>(path.parent_path()),
        path.string()
    );
    return editor;
}

void erase_block(mcpp::EditorModel& editor) {
    editor.delete_block_or_line();
}

void test_linear_move(const std::filesystem::path& root) {
    for (const bool forward : {true, false}) {
        const auto path = root / (forward ? "linear-forward" : "linear-backward");
        const auto original = forward ? "abc|def\nend\n" : "|defabc\nend\n";
        auto editor = load(path, original);
        if (!forward) {
            editor.move(0, 4);
        }
        editor.toggle_mark();
        editor.move(0, 3);
        editor.toggle_mark();
        if (forward) {
            editor.end();
        } else {
            editor.home();
        }
        check(editor.move_selection(), "move linear block");
        check(editor.undo(), "one undo reverses the linear move");
        check(editor.lines()[0] == std::string(original).substr(0, 7),
              "linear move undo restores original bytes");
        check(!editor.undo(), "linear move is exactly one undo step");
        check(editor.redo(), "redo linear move");
        const auto marked = editor.selected_text();
        erase_block(editor);
        editor.save(false);
        check(read_file(path) == "|def\nend\n", "F8 deletes the moved linear block, not a newline");
        check(marked == std::optional<std::string>{"abc"}, "linear destination stays marked");
        check(editor.undo() && editor.selected_text() == marked,
              "undo deletion restores the moved linear mark");
    }
}

void test_rectangular_move(const std::filesystem::path& root) {
    const auto path = root / "rectangular-move";
    const std::string original = "aabbcc\nAABBCC\n--\nzz\n";
    auto editor = load(path, original);
    editor.move(0, 2);
    editor.toggle_column_mark();
    editor.move(0, 2);
    editor.move(1, 0);
    editor.toggle_column_mark();
    editor.set_position({.row = 2, .column = 0});
    check(editor.move_selection(), "move rectangular block");
    check(editor.undo(), "one undo reverses the rectangular move");
    check(editor.lines()[0] == "aabbcc" && editor.lines()[2] == "--",
          "rectangular move undo restores source and destination");
    check(!editor.undo(), "rectangular move is exactly one undo step");
    check(editor.redo(), "redo rectangular move");
    const auto marked = editor.selected_text();
    erase_block(editor);
    editor.save(false);
    check(read_file(path) == "aacc\nAACC\n--\nzz\n",
          "F8 deletes the moved rectangular block only");
    check(editor.undo() && editor.column_selection() &&
              editor.selected_text() == std::optional<std::string>{"bb\nBB"} &&
              editor.selected_text() == marked,
          "undo deletion restores the rectangular destination mark");
}

void test_virtual_source_copy(const std::filesystem::path& root) {
    const auto path = root / "virtual-source-copy";
    auto editor = load(path, "abcdef\nx\nab\n012345\nq\n");
    editor.set_position({.row = 2, .column = 1});
    editor.toggle_column_mark();
    editor.move(-2, 0);
    editor.move(0, 3);
    editor.toggle_column_mark();
    check(editor.selected_text() == std::optional<std::string>{"bcd\n\nb"},
          "rectangle serializes real bytes, not virtual source spaces");
    editor.set_position({.row = 3, .column = 4});
    check(editor.copy_selection(), "copy to short destination rows");
    check(editor.selection_on_line(3) ==
              std::optional<std::pair<std::size_t, std::size_t>>{{4, 7}},
          "rectangle keeps its original width when its last row is short");
    check(editor.copy_selection(), "repeat rectangular copy");
    check(editor.undo(), "undo repeated rectangular copy");
    check(editor.copy_selection(), "copy after undo restores rectangle width");
    editor.save(false);
    check(read_file(path) == "abcdef\nx\nab\n0123bcdbcd45\nq   \n    bb",
          "copy preserves destination padding without padding virtual source cells");
}

void test_real_spaces_copy(const std::filesystem::path& root) {
    const auto path = root / "real-spaces-copy";
    auto editor = load(path, "ab  ef\nx\nAB  EF\n012345\nq\n");
    editor.move(0, 2);
    editor.toggle_column_mark();
    editor.move(2, 0);
    editor.move(0, 2);
    editor.toggle_column_mark();
    check(editor.selected_text() == std::optional<std::string>{"  \n\n  "},
          "rectangle preserves real source spaces and empty rows");
    editor.set_position({.row = 3, .column = 4});
    check(editor.copy_selection(), "copy real spaces");
    editor.save(false);
    check(read_file(path) == "ab  ef\nx\nAB  EF\n0123  45\nq   \n      ",
          "real spaces and destination padding survive rectangular copy");
}

class LocaleGuard {
public:
    LocaleGuard() : previous_(std::setlocale(LC_CTYPE, nullptr)) {
        check(std::setlocale(LC_CTYPE, "C.UTF-8") != nullptr, "C.UTF-8 is available");
    }

    ~LocaleGuard() {
        std::setlocale(LC_CTYPE, previous_.c_str());
    }

private:
    std::string previous_;
};

void test_unicode_offsets(const std::filesystem::path&) {
    const LocaleGuard locale;
    const std::string text = "é K x k";
    const auto matches = mcpp::EditorSearch::find_all(text, "k", 0, text.size(), {});
    check(matches.size() == 2 && matches[0].begin == 3 && matches[0].end == 6 &&
              matches[1].begin == 9 && matches[1].end == 10,
          "UTF-8 folding returns original byte offsets even when encoded lengths differ");
    const auto upper = mcpp::EditorSearch::find_all("É x é", "é", 0, 7, {});
    check(upper.size() == 2 && upper[0].begin == 0 && upper[0].end == 2 &&
              upper[1].begin == 5 && upper[1].end == 7,
          "normal case-insensitive search matches uppercase UTF-8");
    const auto clipped = mcpp::EditorSearch::find(text, "k", 0, 0, 5, {}, false, false);
    check(!clipped, "range ending inside a UTF-8 character cannot return a partial match");
    const auto reverse = mcpp::EditorSearch::find(
        text, "k", text.size(), 0, 8, {.backwards = true}, false, false
    );
    check(reverse && reverse->begin == 3 && reverse->end == 6,
          "backwards scoped search preserves original byte bounds");
}

void test_unicode_nul(const std::filesystem::path& root) {
    const LocaleGuard locale;
    std::string text = "É";
    text.push_back('\0');
    text += "É é";
    std::string needle = "é";
    needle.push_back('\0');
    needle += "é";
    const auto spanning = mcpp::EditorSearch::find_all(text, needle, 0, text.size(), {});
    check(spanning.size() == 1 && spanning[0].begin == 0 && spanning[0].end == 5,
          "UTF-8 normal search can span an embedded NUL in text and query");
    const auto matches = mcpp::EditorSearch::find_all(text, "é", 0, text.size(), {});
    check(matches.size() == 3 && matches[1].begin == 3 && matches[1].end == 5 &&
              matches[2].begin == 6 && matches[2].end == 8,
          "UTF-8 search continues past embedded NULs");
    const auto hex = mcpp::EditorSearch::find_all(
        text, "00 c3 89", 0, text.size(),
        {.mode = mcpp::EditorSearchMode::hexadecimal}
    );
    check(hex.size() == 1 && hex[0].begin == 2 && hex[0].end == 5,
          "hexadecimal search remains byte-exact across NULs");
    const auto path = root / "unicode-nul";
    auto editor = load(path, text);
    check(editor.find("é", false), "select the first replacement match");
    check(editor.replace_all("é", "X", false) == 3,
          "replace all uses folded matches with original byte offsets");
    editor.save(false);
    check(read_file(path) == std::string("X\0X X", 5),
          "UTF-8 replacement preserves NUL and adjacent bytes");
    const std::string invalid("A\xff" "B", 3);
    const auto invalid_match = mcpp::EditorSearch::find_all(
        invalid, std::string("a\xff" "b", 3), 0, invalid.size(), {}
    );
    check(invalid_match.size() == 1 && invalid_match[0].end == 3,
          "invalid UTF-8 bytes retain exact identity during normal search");
}

void test_delete_line_and_empty_mark(const std::filesystem::path& root) {
    const auto path = root / "delete-actions";
    constexpr auto original = "first line\nsecond line\nthird line\n";
    auto editor = load(path, original);
    editor.set_position({1, 2});
    editor.delete_block_or_line();
    check(editor.row() == 1 && editor.column() == 0, "F8 moves to the start of the next line");
    editor.save(false);
    check(read_file(path) == "first line\nthird line\n", "unmarked F8 deletes the whole current line");
    check(editor.undo(), "undo F8 line deletion");
    editor.save(false);
    check(read_file(path) == original, "one undo restores the deleted line exactly");
    editor.set_position({0, 0});
    editor.toggle_mark();
    editor.delete_block_or_line();
    check(!editor.marking() && !editor.dirty(), "F8 clears an empty mark without deleting text");
    editor.move(0, 2);
    editor.delete_block_or_line();
    editor.save(false);
    check(read_file(path) == "second line\nthird line\n", "F8 after an empty mark deletes the line");
    editor.goto_line(editor.lines().size());
    editor.delete_block_or_line();
    check(!editor.dirty(), "F8 at the empty EOF line is a no-op");
}

void test_jump_preferred_column(const std::filesystem::path& root) {
    auto editor = load(root / "jump-column", "\talpha\nx\n\tbeta\n");
    editor.set_position({0, 2});
    editor.goto_line(2);
    check(editor.column() == 1, "jump clamps the cursor to a short line");
    editor.goto_line(3);
    check(editor.column() == 2, "jump restores the preferred display column after a short line");
    editor.goto_line(4);
    check(editor.column() == 0, "jump to the empty EOF line");
    editor.goto_line(1);
    check(editor.column() == 2, "jump back from EOF retains the preferred display column");
}

}  // namespace

int main(const int argc, char** argv) {
    const auto parent =
        argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::current_path();
    std::filesystem::create_directories(parent);
    const auto root = std::filesystem::absolute(
        parent / ("mcpp-editor-regressions-" + std::to_string(getpid()))
    );
    if (!std::filesystem::create_directory(root)) {
        std::cerr << "Regression state directory already exists: " << root << '\n';
        return 1;
    }
    const std::vector<std::pair<std::string_view, void (*)(const std::filesystem::path&)>> tests{
        {"E1 linear move", test_linear_move},
        {"E1 rectangular move", test_rectangular_move},
        {"E4 virtual source copy and mark width", test_virtual_source_copy},
        {"E4 real spaces", test_real_spaces_copy},
        {"E3 Unicode offsets", test_unicode_offsets},
        {"E3 embedded NUL", test_unicode_nul},
        {"F8 line and empty mark", test_delete_line_and_empty_mark},
        {"Document jump preferred column", test_jump_preferred_column},
    };
    int failures = 0;
    for (const auto& [name, test] : tests) {
        try {
            test(root);
            std::cout << "[PASS] " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[FAIL] " << name << ": " << error.what() << '\n';
        }
    }
    if (failures == 0) {
        std::filesystem::remove_all(root);
    } else {
        std::cerr << "Regression artifacts: " << root << '\n';
    }
    return failures == 0 ? 0 : 1;
}
