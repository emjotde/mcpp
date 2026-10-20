#include "mcpp/config.hpp"
#include "editor_display.hpp"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/terminal.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;

void check(const bool condition, const std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

void write(const fs::path& path, const std::string_view contents) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path);
    output << contents;
    check(static_cast<bool>(output), "write original display preference fixture");
}

std::string read(const fs::path& path) {
    std::ifstream input(path);
    check(static_cast<bool>(input), "read original display preference fixture");
    return {std::istreambuf_iterator<char>(input), {}};
}

void configuration(const fs::path& root) {
    const auto native = root / "native/ini";
    write(native,
        "# retained original\n[Midnight-Commander]\n"
        "editor_simple_statusbar=yes\neditor_show_right_margin=0\n"
        "editor_word_wrap_line_length=12\n[Future]\nkeep=raw\\q\n");
    auto config = mcpp::Config::load(native);
    check(config.editor_simple_statusbar == true && config.editor_show_right_margin == false &&
          config.editor_wrap_length == 12U && config.diagnostics.size() == 1U,
          "native display booleans and the existing native wrap column must apply");
    config.save();
    auto restored = mcpp::Config::load(native);
    check(restored.editor_simple_statusbar == true && restored.editor_show_right_margin == false &&
          read(native).find("# retained original") != std::string::npos &&
          read(native).find("[Future]\nkeep=raw\\q") != std::string::npos,
          "native saves must retain explicit false, comments and unknown data");
    const auto legacy = root / "legacy/config";
    write(legacy, "editor_simple_statusbar=false\neditor_show_right_margin=true\n");
    config = mcpp::Config::load(legacy);
    config.save();
    restored = mcpp::Config::load(legacy);
    check(restored.editor_simple_statusbar == false && restored.editor_show_right_margin == true,
          "flat display preferences must retain both boolean states");
    const auto absent = root / "absent/config";
    write(absent, "editor_wrap_length=17\n");
    config = mcpp::Config::load(absent);
    check(!config.editor_simple_statusbar && !config.editor_show_right_margin,
          "absent display preferences must remain disengaged");
    config.save();
    const auto saved = read(absent);
    check(saved.find("editor_simple_statusbar=") == std::string::npos &&
          saved.find("editor_show_right_margin=") == std::string::npos,
          "saving an old profile must not introduce display preferences");
    for (const auto& contents : {
             std::string("[Midnight-Commander]\neditor_show_right_margin=maybe\n"),
             std::string("editor_simple_statusbar=2\n")}) {
        const auto invalid = root / "invalid/config";
        write(invalid, contents);
        bool rejected = false;
        try {
            static_cast<void>(mcpp::Config::load(invalid));
        } catch (const std::exception& error) {
            rejected = std::string_view(error.what()).find("invalid boolean") != std::string_view::npos;
        }
        check(rejected && read(invalid) == contents &&
              !config.editor_simple_statusbar && !config.editor_show_right_margin,
              "invalid booleans must fail without modifying input or accepted preferences");
    }
}

void status() {
    using mcpp::detail::editor_simple_position;
    check(editor_simple_position(0, 1, 4, 0, 40, 48, false, 100) ==
              "   0     1/4      0/40 0048   0%",
          "simple status must use native zero-based visual column and compact byte groups");
    const auto newline = editor_simple_position(27, 1, 4, 27, 40, 10, false, 100);
    check(newline.find("0010  67%") != std::string::npos &&
          newline.find("L:[") == std::string::npos && newline.find("U+") == std::string::npos,
          "simple status must omit verbose selection and hexadecimal groups");
    const auto eof = editor_simple_position(0, 4, 4, 40, 40, 0, true, 100);
    check(eof.find("<EOF> 100%") != std::string::npos,
          "simple status must retain EOF and storage-byte percentage");
    check(editor_simple_position(27, 4, 4, 40, 40, 0, true, 12) == " 27 4/4 100%",
          "narrow status must drop complete groups rather than cut a label");
    check(editor_simple_position(0, 1, 1, 0, 0, 0, true, 100).find("0/0 <EOF>   0%") !=
              std::string::npos, "an empty buffer must not divide by zero");
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    check(editor_simple_position(0, 1, 1, maximum, maximum, 0x754CU, false, 200)
                  .find("30028 100%") != std::string::npos,
          "Unicode character values and byte percentages must not overflow");
}

void pair(const ftxui::Cell& cell, const mcpp::SkinPair& expected) {
    const auto attributes = (cell.bold ? mcpp::SkinPair::bold : 0U) |
        (cell.italic ? mcpp::SkinPair::italic : 0U) |
        (cell.underlined ? mcpp::SkinPair::underline : 0U) |
        (cell.inverted ? mcpp::SkinPair::reverse : 0U) |
        (cell.blink ? mcpp::SkinPair::blink : 0U);
    check(cell.foreground_color == expected.foreground && cell.background_color == expected.background &&
          attributes == expected.attributes && !cell.dim && !cell.underlined_double && !cell.strikethrough,
          "right-margin cells must use the exact configured pair and explicit attributes");
}

ftxui::Element original_line() {
    using namespace ftxui;
    return hbox({
        text("A界é  ") | color(Color::White) | bgcolor(Color::Blue) | bold | italic,
        text("$") | color(Color::Black) | bgcolor(Color::Cyan) | underlined,
        text("Z") | color(Color::Black) | bgcolor(Color::White) | bold | focusCursorBlock,
    });
}

void margin() {
    using namespace ftxui;
    const mcpp::SkinPair style{Color::RedLight, Color::Green, 0U, true};
    auto plain = Screen::Create(Dimension::Fixed(24), Dimension::Fixed(1));
    Render(plain, original_line());
    auto screen = Screen::Create(Dimension::Fixed(24), Dimension::Fixed(1));
    Render(screen, mcpp::detail::editor_right_margin(
        original_line(), 0, 24, 4, style, {{6, 7}, {7, 8}}));
    for (int column = 0; column < 24; ++column) {
        const auto& before = plain.CellAt(column, 0);
        const auto& after = screen.CellAt(column, 0);
        check(before.character == after.character, "a margin must never replace a displayed glyph");
        if (column >= 4 && column != 6 && column != 7) pair(after, style);
        else check(before.foreground_color == after.foreground_color &&
                   before.background_color == after.background_color &&
                   before.bold == after.bold && before.italic == after.italic &&
                   before.underlined == after.underlined,
                   "normal syntax, selection and cursor styles must survive outside the margin or protection");
    }
    check(screen.CellAt(1, 0).character == "界" && screen.CellAt(3, 0).character == "é" &&
          screen.cursor().x == plain.cursor().x && screen.cursor().y == plain.cursor().y,
          "wide and combining glyphs and the cursor position must remain unchanged");

    auto decorated = style;
    decorated.attributes = mcpp::SkinPair::bold | mcpp::SkinPair::italic |
        mcpp::SkinPair::underline | mcpp::SkinPair::reverse | mcpp::SkinPair::blink;
    screen = Screen::Create(Dimension::Fixed(12), Dimension::Fixed(1));
    Render(screen, mcpp::detail::editor_right_margin(text("0123456789AB"), 7, 12, 12, decorated));
    pair(screen.CellAt(5, 0), decorated);
    check(screen.CellAt(4, 0).background_color != decorated.background,
          "horizontal scrolling must place the margin in document display columns");
    for (int column = 0; column < 12; ++column) {
        check(screen.CellAt(column, 0).character == std::string(1, "0123456789AB"[column]),
              "scrolling the margin must not insert a separator glyph");
    }

    screen = Screen::Create(Dimension::Fixed(30), Dimension::Fixed(1));
    Render(screen, hbox({
        text("   1  ") | color(Color::Yellow) | bgcolor(Color::Blue),
        mcpp::detail::editor_right_margin(text("0123456789AB"), 7, 24, 12, style),
    }));
    check(screen.CellAt(5, 0).background_color == Color::Blue &&
          screen.CellAt(10, 0).background_color != style.background,
          "the line-number gutter must not count toward the document margin");
    pair(screen.CellAt(11, 0), style);
    screen = Screen::Create(Dimension::Fixed(24), Dimension::Fixed(3));
    Render(screen, mcpp::detail::editor_right_margin(filler(), 0, 24, 12, style));
    for (int row = 0; row < 3; ++row) {
        check(screen.CellAt(11, row).background_color != style.background,
              "blank rows must retain normal cells before the margin");
        pair(screen.CellAt(12, row), style);
        pair(screen.CellAt(23, row), style);
    }
    screen = Screen::Create(Dimension::Fixed(12), Dimension::Fixed(1));
    Render(screen, mcpp::detail::editor_right_margin(text("beyond"), 20, 12, 12, style));
    pair(screen.CellAt(0, 0), style);
    pair(screen.CellAt(11, 0), style);
    screen = Screen::Create(Dimension::Fixed(12), Dimension::Fixed(1));
    Render(screen, mcpp::detail::editor_right_margin(text("before"), 0, 12, 20, style));
    check(screen.CellAt(11, 0).background_color != style.background,
          "an offscreen margin must not recolor the viewport");
}

}  // namespace

int main(const int argc, char** argv) {
    try {
        check(argc == 2 && ::geteuid() == 1001U && fs::exists("/.dockerenv") &&
              fs::exists("/review/.operation-regressions-disposable"),
              "display preference tests require UID1001 in the owned review container");
        const auto root = fs::absolute(argv[1]).lexically_normal();
        check(root.string().starts_with("/review/cases/mc-config/editor-display-500191ad/model/"),
              "use an owned editor display model directory");
        ::umask(0077);
        fs::create_directories(root);
        check(!fs::is_symlink(root), "model directory must not be a symlink");
        ftxui::Terminal::SetColorSupport(ftxui::Terminal::TrueColor);
        configuration(root);
        status();
        margin();
        std::cout << "native/flat/absent/invalid preferences, compact status and cell-only margin: passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
