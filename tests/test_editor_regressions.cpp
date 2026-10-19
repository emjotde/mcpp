#include "mcpp/editor.hpp"
#include "mcpp/command_line.hpp"
#include "mcpp/diff.hpp"
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

void test_unnamed_editor_and_cli_positions(const std::filesystem::path& root) {
    const auto provider = std::make_shared<mcpp::LocalProvider>(root);
    mcpp::EditorModel editor;
    editor.create(provider, "");
    check(editor.open() && editor.path().empty() && !editor.dirty(), "unnamed buffer is open without a path");
    editor.insert("draft");
    bool rejected = false;
    try {
        editor.save(false);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    check(rejected && editor.dirty(), "unnamed Save requires a chosen path without discarding the draft");
    const auto named = root / "named-from-unnamed";
    editor.save_as(named.string(), false);
    check(read_file(named) == "draft" && !editor.dirty() && editor.path() == named.string(),
          "Save as publishes and names an unnamed buffer");
    editor.create(provider, "");
    check(editor.path().empty() && editor.text().empty(), "creating another unnamed buffer clears the old path");
    check(read_file(named) == "draft", "unnamed creation does not alter the old file");

    const auto target = mcpp::parse_editor_file((root / "not-created").string() + ":12:");
    check(target.path == (root / "not-created").string() && target.line == 12U,
          "parse per-file line with optional trailing colon");
    check(mcpp::parse_editor_file("name:0").line == 1U &&
          mcpp::parse_line_number("0") == 1U, "native zero line selects the first line");
    check(mcpp::parse_editor_file("name:word").path == "name:word", "nonnumeric colon suffix is a filename");
    const auto literal = root / "literal:12";
    { std::ofstream output(literal); output << "literal"; }
    check(mcpp::parse_editor_file(literal.string()).path == literal.string() &&
          mcpp::parse_editor_file(literal.string()).line == 0U, "an existing colon filename wins over position syntax");
    const auto dangling = root / "dangling:12";
    std::filesystem::create_symlink(root / "missing-target", dangling);
    check(mcpp::parse_editor_file(dangling.string()).path == dangling.string() &&
          mcpp::parse_editor_file(dangling.string()).line == 0U,
          "a dangling colon symlink is not silently retargeted to a different filename");
    for (const auto value : {"", "-1", "12x", "999999999999999999999999999999"}) {
        bool invalid = false;
        try {
            static_cast<void>(mcpp::parse_line_number(value));
        } catch (const std::invalid_argument&) {
            invalid = true;
        }
        check(invalid, "invalid and overflowing line numbers are rejected");
    }
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

void test_word_navigation(const std::filesystem::path& root) {
    auto editor = load(root / "word-navigation", "foo_bar-baz écho\nnext word\n");
    for (const auto offset : {7U, 8U, 12U, 17U, 18U, 23U, 27U}) {
        editor.move_word(1);
        check(editor.cursor_offset() == offset, "forward word stop agrees with MC");
    }
    for (const auto offset : {23U, 18U, 17U, 12U, 8U, 7U, 0U}) {
        editor.move_word(-1);
        check(editor.cursor_offset() == offset, "backward word stop agrees with MC");
    }
    editor.move_word(-1);
    check(editor.cursor_offset() == 0U, "word-left clamps at the start");
    editor.set_position({1, 9});
    editor.move_word(1);
    check(editor.row() == 2U && editor.column() == 0U, "word-right crosses only the LF");
    editor.move_word(1);
    check(editor.cursor_offset() == editor.byte_size(), "word-right clamps at EOF");
    editor.move_word(-1);
    check(editor.row() == 1U && editor.column() == 9U, "word-left stops at previous EOL");
}

void test_resume_mark(const std::filesystem::path& root) {
    auto editor = load(root / "resume-mark", "abcdef\nABCDEF\n123456\n");
    editor.set_position({0, 2});
    editor.begin_column_mark();
    editor.set_position({1, 4});
    editor.finish_mark();
    editor.set_position({1, 5});
    editor.resume_mark();
    editor.set_position({2, 5});
    editor.finish_mark();
    check(editor.column_selection() && editor.selected_text() ==
              std::optional<std::string>{"cde\nCDE\n345"},
          "resuming a rectangle preserves its original anchor and shape");
    editor.clear_selection();
    editor.set_position({2, 5});
    editor.begin_column_mark();
    editor.set_position({1, 3});
    editor.finish_mark();
    editor.set_position({0, 0});
    editor.resume_mark();
    editor.set_position({0, 1});
    editor.finish_mark();
    check(editor.selected_text() == std::optional<std::string>{"bcde\nBCDE\n2345"},
          "resuming a reverse rectangle retains the unsorted anchor");
    editor.clear_selection();
    editor.set_position({0, 1});
    editor.resume_mark();
    editor.set_position({0, 3});
    editor.finish_mark();
    check(!editor.column_selection() && editor.selected_text() ==
              std::optional<std::string>{"bc"},
          "Shift extension without a mark starts a linear block at the cursor");
}

void test_search_preserves_blocks(const std::filesystem::path& root) {
    for (const bool column : {false, true}) {
        const auto path = root / (column ? "search-column" : "search-linear");
        auto editor = load(path, column ? "KEEP xx NEEDLE\nkeep yy other\n"
                                        : "KEEP and NEEDLE and TAIL\n");
        if (column) {
            editor.begin_column_mark();
        } else {
            editor.begin_mark();
        }
        editor.move(0, 4);
        if (column) {
            editor.move(1, 0);
        }
        editor.finish_mark();
        const auto marked = editor.selected_text();
        editor.set_position({0, 0});
        check(editor.find("NEEDLE", true), "find independently of the persistent block");
        check(editor.selected_text() == marked && editor.column_selection() == column,
              "search preserves the original block and its shape");
        check(editor.search_selection().has_value(), "search has its own displayed range");
        editor.delete_block_or_line();
        editor.save(false);
        check(read_file(path) == (column ? " xx NEEDLE\n yy other\n"
                                        : " and NEEDLE and TAIL\n"),
              "F8 after search deletes the user block, not the search highlight");
    }
}

void test_cr_only_editing(const std::filesystem::path& root) {
    auto editor = load(root / "cr-source", "alpha\nbeta\ngamma\n");
    const auto path = root / "cr-output";
    editor.save_as(path.string(), false, mcpp::EditorLineEnding::cr);
    check(editor.lines().size() == 4U && editor.line_separator() == '\r' &&
              read_file(path) == "alpha\rbeta\rgamma\r",
          "CR Save As preserves logical editing lines");
    editor.goto_line(2);
    editor.home();
    editor.delete_block_or_line();
    editor.save(false);
    check(read_file(path) == "alpha\rgamma\r", "F8 removes one CR line only");
    check(editor.undo() && editor.lines().size() == 4U, "Undo restores the CR line");
    editor.set_position({1, 0});
    editor.paste("x\r\ny\n");
    editor.save(false);
    check(read_file(path) == "alpha\rx\ry\rbeta\rgamma\r",
          "pasted LF/CRLF text retains CR output format");
    editor.set_position({0, 0});
    check(editor.find("^beta$", {.mode=mcpp::EditorSearchMode::regular_expression}, false, false),
          "logical regex line anchors work in a CR document");
    editor.close();
    editor.open(std::make_shared<mcpp::LocalProvider>(root), path.string());
    check(editor.lines().size() == 6U && editor.lines()[3] == "beta",
          "reopening a CR file retains the same logical rows");
}

void test_codepage_view_and_original_bytes(const std::filesystem::path& root) {
    const auto path = root / "codepage";
    auto editor = load(path, std::string("caf\xe9\n", 5));
    editor.set_encoding("ISO-8859-1");
    check(!editor.dirty() && editor.lines().front() == "café" && editor.storage_byte_size() == 5U &&
              read_file(path) == std::string("caf\xe9\n", 5),
          "selecting a codepage changes interpretation, not the file or dirty state");
    editor.move(0, 4);
    check(editor.storage_cursor_offset() == 4U, "encoded cursor offsets remain physical file-byte offsets");
    editor.set_position({0, 0});
    editor.begin_mark();
    editor.move(0, 4);
    editor.finish_mark();
    check(editor.storage_selection_size() == 4U && editor.selected_bytes() == std::string("caf\xe9", 4),
          "block byte counts and file output retain the selected codepage");
    editor.clear_selection();
    editor.paste_bytes(std::string("\xe9", 1));
    editor.save(false);
    check(read_file(path) == std::string("caf\xe9\xe9\n", 6), "inserting encoded bytes saves in the original codepage");
    bool rejected = false;
    try { editor.insert("界"); }
    catch (const std::system_error&) { rejected = true; }
    check(rejected && !editor.dirty() && editor.storage_byte_size() == 6U,
          "unrepresentable input fails before changing the buffer");
    check(editor.undo(), "encoded edit remains undoable");
    editor.save(false);
    check(read_file(path) == std::string("caf\xe9\n", 5), "Undo does not change the file encoding");
    const auto converted = root / "codepage-crlf";
    editor.save_as(converted.string(), false, mcpp::EditorLineEnding::crlf);
    check(read_file(converted) == std::string("caf\xe9\r\n", 6) && editor.storage_byte_size() == 6U,
          "Save As converts line endings without converting the selected codepage");
}

void test_column_block_transform(const std::filesystem::path& root) {
    auto editor = load(root / "column-transform", "LbbR\nLaaR\nLccR\n");
    editor.set_position({0, 1});
    editor.begin_column_mark();
    editor.set_position({2, 3});
    editor.finish_mark();
    editor.replace_block("aa\nbb\ncc\n");
    check(editor.text() == "LaaR\nLbbR\nLccR\n", "column transforms preserve the surrounding bytes");
    check(editor.column_selection() && editor.selection_size() == 8U,
          "column transforms retain the rectangle, including its serialized line separators");
    check(editor.undo() && editor.text() == "LbbR\nLaaR\nLccR\n" && editor.column_selection(),
          "a column transform is one undoable edit");
}

void test_native_general_behavior(const std::filesystem::path& root) {
    auto editor = load(root / "native-general-tabs", "");
    mcpp::EditorBehavior options;
    options.fake_half_tabs = true;
    editor.set_behavior(options);
    editor.tab();
    check(editor.text() == "    ", "Fake half tabs inserts half a tab in indentation");
    editor.tab();
    check(editor.text() == "\t", "a second fake half tab collapses to a real tab");
    editor.backspace();
    check(editor.text().empty(), "backspace removes a real indentation tab");
    options.fill_tabs_with_spaces = true;
    editor.set_behavior(options);
    editor.tab(); editor.tab();
    check(editor.text() == "        ", "Fill tabs with spaces prevents collapse");
    options.backspace_through_tabs = true;
    editor.set_behavior(options);
    editor.backspace();
    check(editor.text().empty(), "Backspace through tabs removes the indentation prefix");
    check(editor.undo() && editor.text() == "        ", "indentation deletion is one undoable edit");

    editor = load(root / "native-general-selection", "keep\n");
    options = {};
    options.persistent_selection = false;
    editor.set_behavior(options);
    editor.select_all();
    editor.insert("replacement");
    check(editor.text() == "replacement", "nonpersistent selection is replaced by typing");
    check(editor.undo() && editor.text() == "keep\n", "selection replacement is one undoable edit");
    editor.select_all();
    editor.move(0, 1);
    check(!editor.selection(), "ordinary motion clears a nonpersistent selection");

    editor = load(root / "native-general-virtual", "a界\nx\n");
    options = {};
    options.cursor_beyond_eol = true;
    editor.set_behavior(options);
    editor.set_display_position(0, 7);
    check(editor.display_cursor_column() == 7U && editor.virtual_column() == 4U &&
              editor.storage_cursor_offset() == 4U, "virtual columns do not invent file bytes");
    editor.insert("Z");
    check(editor.text() == "a界    Z\nx\n", "typing materializes only the required virtual spaces");
    check(editor.undo() && editor.text() == "a界\nx\n" && editor.virtual_column() == 4U,
          "Undo restores both bytes and the virtual cursor");
    editor.move(1, 0);
    check(editor.display_cursor_column() == 7U && editor.virtual_column() == 6U,
          "vertical virtual motion preserves the display column");
    editor.set_display_position(0, 4);
    editor.begin_column_mark();
    editor.set_display_position(1, 6);
    editor.finish_mark();
    check(editor.column_mark_columns() == std::pair<std::size_t, std::size_t>{4,6},
          "column marks retain virtual display coordinates");

    editor = load(root / "native-general-undo", "");
    options = {};
    options.group_undo = true;
    editor.set_behavior(options);
    editor.insert('a'); editor.insert('b'); editor.insert('c');
    check(editor.undo() && editor.text().empty(), "Group undo combines consecutive insertion actions");
    check(editor.redo() && editor.text() == "abc", "grouped Redo restores the same insertion group");
    editor.home(); editor.insert('X');
    check(editor.undo() && editor.text() == "abc", "navigation separates undo groups");
    options.group_undo = false;
    editor.set_behavior(options);
    editor.insert('A'); editor.insert('B');
    check(editor.undo() && editor.text() == "Aabc", "Group undo off keeps individual insertions");

    editor = load(root / "native-general-typewriter", "one two");
    options = {};
    options.wrap = mcpp::EditorWrapMode::typewriter;
    options.wrap_length = 8;
    editor.set_behavior(options);
    editor.end();
    editor.insert(" three");
    check(editor.text() == "one two\nthree" && editor.row() == 1U && editor.column() == 5U,
          "Type writer wrap breaks words without moving the insertion point");
    check(editor.undo() && editor.text() == "one two", "automatic wrapping belongs to the typing edit");

    editor = load(root / "native-general-dynamic", "one two three four five");
    options.wrap = mcpp::EditorWrapMode::dynamic;
    options.wrap_length = 12;
    editor.set_behavior(options);
    editor.end(); editor.insert(" six");
    check(editor.text() == "one two\nthree four\nfive six" && editor.row() == 2U && editor.column() == 8U,
          "Dynamic paragraphing reflows while preserving the logical insertion point");
    check(editor.undo() && editor.text() == "one two three four five",
          "dynamic paragraphing is one undoable edit");
}

void test_local_diff_and_merge(const std::filesystem::path& root) {
    const auto first = root / "diff-first", second = root / "diff-second";
    auto left = load(first, "same\nLEFT\ncommon\nLEFT2\nlast\n");
    auto right = load(second, "same\nRIGHT\ncommon\nRIGHT2\nlast\n");
    mcpp::DiffModel comparison;
    comparison.compare(left, right);
    check(comparison.hunks().size() == 2U && comparison.rows().size() == 5U,
          "local diff aligns unchanged rows and distinct hunks");
    comparison.merge(0, true, left, right);
    check(right.text() == "same\nLEFT\ncommon\nRIGHT2\nlast\n" &&
              read_file(second) == "same\nRIGHT\ncommon\nRIGHT2\nlast\n",
          "directional merge stages only the selected hunk without publishing it");
    check(right.undo() && right.text() == "same\nRIGHT\ncommon\nRIGHT2\nlast\n",
          "a merged hunk is one undoable edit");
    comparison.compare(left, right);
    comparison.merge(1, false, left, right);
    check(left.text() == "same\nLEFT\ncommon\nRIGHT2\nlast\n", "reverse merge updates only its target side");
    left = load(first, "one\ntwo");
    right = load(second, "one\n");
    comparison.compare(left, right);
    check(comparison.hunks().size() == 1U && comparison.hunks()[0].right_count == 0U,
          "diff represents insertion at EOF explicitly");
    comparison.merge(0, true, left, right);
    check(right.text() == "one\ntwo", "EOF insertion preserves the absent final newline");
    left = load(first, "");
    right = load(second, "one\n");
    comparison.compare(left, right);
    comparison.merge(0, true, left, right);
    check(right.text().empty(), "merging an empty side removes the selected target lines");
    left = load(first, "Name\t \r\n");
    right = load(second, "name \n");
    comparison.compare(left, right, {.ignore_case=true, .ignore_all_space=true, .strip_trailing_cr=true});
    check(comparison.hunks().empty(), "native diff options affect actual comparison");
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
        {"Unnamed editor and CLI positions", test_unnamed_editor_and_cli_positions},
        {"E1 linear move", test_linear_move},
        {"E1 rectangular move", test_rectangular_move},
        {"E4 virtual source copy and mark width", test_virtual_source_copy},
        {"E4 real spaces", test_real_spaces_copy},
        {"E3 Unicode offsets", test_unicode_offsets},
        {"E3 embedded NUL", test_unicode_nul},
        {"F8 line and empty mark", test_delete_line_and_empty_mark},
        {"Document jump preferred column", test_jump_preferred_column},
        {"MC word and newline stops", test_word_navigation},
        {"Persistent mark extension", test_resume_mark},
        {"Search preserves persistent blocks", test_search_preserves_blocks},
        {"CR-only save and editing", test_cr_only_editing},
        {"Codepage view and byte-preserving saves", test_codepage_view_and_original_bytes},
        {"Rectangular block transforms", test_column_block_transform},
        {"Native General editing behavior", test_native_general_behavior},
        {"Local diff and staged merge", test_local_diff_and_merge},
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
