#include "mcpp/editor.hpp"
#include "mcpp/local_provider.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;

void check(const bool condition, const std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

std::string read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    check(static_cast<bool>(input), "read fixture");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

mcpp::EditorModel load(const fs::path& path, const std::string_view bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    check(static_cast<bool>(output), "write fixture");
    mcpp::EditorModel editor;
    editor.open(std::make_shared<mcpp::LocalProvider>(path.parent_path()), path.string());
    return editor;
}

void history(const fs::path& root) {
    const std::string original = "é\tZ\r\nABCDE\tR\r\n";
    auto editor = load(root / "history", original);
    editor.set_behavior({.group_undo=true});
    editor.set_position({0, 2});
    editor.insert("x");
    editor.insert("y");
    editor.set_position({0, 4});
    editor.begin_column_mark();
    editor.set_display_position(1, 8);
    editor.toggle_bookmark();
    const auto cursor = editor.storage_cursor_offset();
    const auto marked = editor.selected_bytes();
    editor.set_encoding("ISO-8859-1");
    check(editor.dirty() && editor.marking() && editor.column_selection() &&
              editor.storage_cursor_offset() == cursor && editor.selected_bytes() == marked &&
              editor.bookmarks().contains(1) &&
              editor.column_mark_columns() == std::pair<std::size_t, std::size_t>{3, 8},
          "dirty codepage changes retain encoded cursor, active rectangle and bookmarks");
    editor.set_encoding("UTF-8");
    check(editor.undo() && editor.text() == original, "one undo retains the original insertion group");
    editor.set_encoding("ISO-8859-1");
    check(editor.redo(), "codepage changes retain the redo stack");
    const auto destination = root / "destination";
    fs::create_directories(destination);
    const auto provider = std::make_shared<mcpp::LocalProvider>(destination);
    const auto path = destination / "saved";
    const std::string draft = "éxy\tZ\r\nABCDE\tR\r\n";
    editor.save_as(provider, path.string(), true, mcpp::EditorLineEnding::unchanged);
    check(editor.provider() == provider && read(path) == draft && !editor.dirty(),
          "cross-provider Save As retains draft bytes and CRLF");
    editor.set_encoding("UTF-8");
    check(!editor.dirty() && editor.undo() && editor.text() == original,
          "a saved buffer retains its history across a codepage change");
    const auto change = editor.change_id();
    const auto position = mcpp::EditorPosition{editor.row(), editor.column()};
    bool rejected = false;
    try { editor.set_encoding("ASCII"); }
    catch (const std::system_error&) { rejected = true; }
    check(rejected && editor.encoding() == "UTF-8" && editor.text() == original &&
              editor.change_id() == change && editor.dirty() &&
              mcpp::EditorPosition{editor.row(), editor.column()} == position &&
              read(path) == draft && editor.redo() && editor.text() == draft,
          "invalid interpretation retains the document, provider, cursor, disk and redo history");
    std::cout << "PASS dirty/clean codepage history, marks and Save As\n";
}

void raw_search(const fs::path& root) {
    const std::string original = "é é\rx\r";
    const auto path = root / "raw";
    auto editor = load(path, original);
    editor.set_encoding("ISO-8859-1");
    editor.begin_mark();
    editor.set_position({0, 4});
    editor.finish_mark();
    const auto marked = editor.selected_bytes();
    const mcpp::EditorSearchOptions hex{.mode=mcpp::EditorSearchMode::hexadecimal};
    check(editor.count_matches("C3 A9", hex) == 2U && editor.count_matches("C3 83", hex) == 0U &&
              editor.count_matches("C3 A9", hex, editor.selection()) == 1U,
          "hex counts encoded bytes rather than the UTF-8 display");
    editor.set_position({0, 0});
    check(editor.find("C3 A9", hex, false, false) &&
              editor.search_selection() == std::pair{mcpp::EditorPosition{0, 0}, mcpp::EditorPosition{0, 4}} &&
              editor.find("C3 A9", hex, false, false) && editor.storage_cursor_offset() == 3U &&
              editor.selected_bytes() == marked,
          "raw next-search maps decoded positions without changing the marked block");
    editor.set_position({0, 0});
    check(editor.find("A9 0D 78", hex, false, false) &&
              editor.search_selection()->second == mcpp::EditorPosition{1, 1},
          "raw search crosses the stored CR separator");
    editor.set_position({0, 0});
    check(editor.find("C3 A9", hex, false, false) &&
              editor.replace_all("C3 A9", "C3 A8", hex) == 2U && editor.undo(),
          "encoded replace-all is undoable");
    editor.set_encoding("UTF-8");
    editor.set_position({0, 0});
    check(editor.find("A9", hex, false, false) &&
              editor.search_selection() == std::pair{mcpp::EditorPosition{0, 0}, mcpp::EditorPosition{0, 2}} &&
              editor.find("A9", hex, false, false) && editor.storage_cursor_offset() == 3U &&
              !editor.find("A9", hex, false, false),
          "partial UTF-8 byte hits highlight complete characters and advance by exact raw ranges");
    const auto change = editor.change_id();
    bool rejected = false;
    try { static_cast<void>(editor.replacement_for_selection("A9", "FF", hex)); }
    catch (const std::system_error&) { rejected = true; }
    check(rejected && editor.text() == original && editor.change_id() == change &&
              editor.selected_bytes() == marked && read(path) == original,
          "invalid raw replacement is transactional");
    check(editor.replace_selection(editor.replacement_for_selection("A9", "A8", hex)) &&
              editor.text() == "é è\rx\r" && editor.undo(),
          "single-byte replacement preserves the other bytes of its decoded character");
    editor.set_position({0, 0});
    check(editor.find("A9", hex, false, false) && editor.replace_all("A9", "A8", hex) == 2U,
          "partial-byte replace-all operates on raw ranges");
    editor.save(false);
    check(read(path) == "è è\rx\r" && editor.undo() && editor.text() == original,
          "raw replacements save exact bytes and retain CR endings and undo");
    std::cout << "PASS encoded hex search, valid position mapping and transactional replacement\n";
}

}  // namespace

int main(const int argc, char** argv) try {
    check(argc == 2 && ::getuid() == 1001 &&
              fs::exists("/review/.operation-regressions-disposable"),
          "run only as UID 1001 in the existing disposable parity container, with a case directory");
    const auto root = fs::absolute(argv[1]).lexically_normal();
    check(root.string().starts_with("/review/cases/editor-codepages/"), "use the owned case directory");
    fs::create_directories(root);
    history(root);
    raw_search(root);
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
