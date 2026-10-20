#include "mcpp/local_provider.hpp"
#include "mcpp/skin_glyphs_ui.hpp"
#include "viewer_help_styles.hpp"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>

namespace {

void check(const bool condition, const std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

void write(const std::filesystem::path& path, const std::string_view bytes,
           const std::ios::openmode mode = std::ios::trunc) {
    std::ofstream output(path, std::ios::binary | mode);
    output << bytes;
    check(static_cast<bool>(output), "write the tiny original EOF fixture");
}

std::string read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}

ftxui::Elements data_rows(mcpp::ViewerModel& viewer, const std::size_t width,
                         const std::size_t height, const mcpp::Skin& skin = {}) {
    ftxui::Elements rows;
    const auto lines = viewer.lines(width, height);
    for (std::size_t index = 0; index < lines.size(); ++index) {
        rows.push_back(mcpp::detail::render_viewer_text(
            skin, lines[index], viewer.text_highlight(index), viewer.text_styles(index)));
    }
    return rows;
}

ftxui::Screen draw(ftxui::Elements rows, const int width, const int height) {
    auto screen = ftxui::Screen::Create(
        ftxui::Dimension::Fixed(width), ftxui::Dimension::Fixed(height));
    ftxui::Render(screen, ftxui::vbox(std::move(rows)));
    return screen;
}

std::string row_text(const ftxui::Screen& screen, const int row) {
    std::string result;
    for (int column = 0; column < screen.dimx(); ++column) {
        result += screen.CellAt(column, row).character;
    }
    while (!result.empty() && result.back() == ' ') result.pop_back();
    return result;
}

void rendering(const std::filesystem::path& root) {
    auto provider = std::make_shared<mcpp::LocalProvider>();
    const auto path = root / "original.txt";
    write(path, "alpha\n");
    mcpp::ViewerModel viewer;
    viewer.open(provider, path.string());
    for (const auto marker : {"", "0", "EOF!"}) {
        auto rows = data_rows(viewer, 12U, 4U);
        const auto actual = rows.front();
        mcpp::detail::append_viewer_eof_rows(rows, marker, viewer, 4U);
        check(rows.size() == (*marker == '\0' ? 1U : 4U) && rows.front() == actual,
              "empty disables padding; literal zero and strings append without replacing data");
        const auto screen = draw(std::move(rows), 12, 4);
        check(row_text(screen, 0) == "alpha", "the actual text row is unchanged");
        for (int row = 1; row < 4; ++row) {
            check(row_text(screen, row) == marker, "each unused text row starts with the literal marker");
        }
    }
    auto rows = data_rows(viewer, 12U, 4U);
    mcpp::detail::append_viewer_eof_rows(rows, "é界", viewer, 4U);
    auto screen = draw(std::move(rows), 12, 4);
    check(screen.CellAt(0, 1).character == "é" && screen.CellAt(1, 1).character == "界",
          "marker glyphs remain UTF-8 user data");
    rows = data_rows(viewer, 4U, 4U);
    mcpp::detail::append_viewer_eof_rows(rows, "123456789", viewer, 4U);
    screen = draw(std::move(rows), 4, 4);
    check(row_text(screen, 2) == "1234" && row_text(screen, 3) == "1234",
          "the viewport clips long markers without wrapping into extra rows");
    rows = data_rows(viewer, 3U, 1U);
    mcpp::detail::append_viewer_eof_rows(rows, "EOF!", viewer, 4U);
    check(rows.size() == 1U, "an unconsumed file is not treated as EOF");
    rows = data_rows(viewer, 12U, 4U);
    mcpp::detail::append_viewer_eof_rows(rows, "EOF!", viewer, 1U);
    check(rows.size() == 1U, "a full viewport is never enlarged or shortened");
    write(path, "");
    viewer.go_to_start();
    rows = data_rows(viewer, 24U, 3U);
    rows.push_back(mcpp::glyph_text("Waiting for input..."));
    mcpp::detail::append_viewer_eof_rows(rows, "EOF!", viewer, 3U);
    rows.push_back(mcpp::glyph_text("RULER"));
    screen = draw(std::move(rows), 24, 4);
    check(row_text(screen, 0) == "Waiting for input..." &&
          row_text(screen, 1) == "EOF!" && row_text(screen, 3) == "RULER",
          "existing status rows and the separately reserved ruler row are retained");
    viewer.close();
    rows.clear();
    mcpp::detail::append_viewer_eof_rows(rows, "EOF!", viewer, 4U);
    check(rows.empty(), "a closed viewer has no EOF presentation");
}

void state(const std::filesystem::path& root) {
    auto provider = std::make_shared<mcpp::LocalProvider>();
    const auto path = root / "original.nroff";
    constexpr std::string_view bytes = "B\bB _\bU needle\n";
    write(path, bytes);
    mcpp::ViewerModel viewer;
    viewer.open(provider, path.string());
    viewer.toggle_nroff();
    check(viewer.find("needle", true).has_value(), "find the actual nroff file text");
    auto rows = data_rows(viewer, 24U, 4U);
    const auto before = std::array{
        viewer.offset(), viewer.size(), viewer.view_end(), viewer.source_view_end(),
        viewer.data_size(), viewer.data_position(), viewer.hex_cursor()};
    const auto match = viewer.text_highlight(0);
    check(match.has_value() && viewer.text_styles(0).size() == 2U,
          "the actual model supplies search and nroff ranges");
    mcpp::detail::append_viewer_eof_rows(rows, "EOF!", viewer, 4U);
    const auto screen = draw(std::move(rows), 24, 4);
    check(screen.CellAt(0, 0).bold && screen.CellAt(2, 0).underlined &&
          screen.CellAt(static_cast<int>(match->first), 0).inverted &&
          !screen.CellAt(0, 1).bold && !screen.CellAt(0, 1).underlined &&
          !screen.CellAt(0, 1).inverted,
          "markers do not inherit actual-row nroff or search emphasis");
    check(before == std::array{
              viewer.offset(), viewer.size(), viewer.view_end(), viewer.source_view_end(),
              viewer.data_size(), viewer.data_position(), viewer.hex_cursor()} &&
          viewer.text_highlight(0) == match && viewer.text_styles(0).size() == 2U &&
          !viewer.dirty() && read(path) == bytes,
          "display-only padding preserves offsets, search, styles and original bytes");
    check(!viewer.find("EOF!", true).has_value(), "padding is not searchable file content");

    const auto latin = root / "original-latin1.txt";
    write(latin, "\xE9\n");
    mcpp::ViewerModel encoded;
    encoded.open(provider, latin.string());
    encoded.set_encoding("ISO-8859-1");
    rows = data_rows(encoded, 12U, 3U);
    const auto encoded_end = encoded.source_view_end();
    mcpp::detail::append_viewer_eof_rows(rows, "EOF!", encoded, 3U);
    check(row_text(draw(std::move(rows), 12, 3), 0) == "é" &&
          encoded.source_view_end() == encoded_end && encoded_end == 2U &&
          encoded.data_size() == 2U && read(latin) == "\xE9\n",
          "codepage presentation retains source-byte offsets");

    const auto growing = root / "original-growing.txt";
    write(growing, "one\n");
    mcpp::ViewerModel live;
    live.open(provider, growing.string());
    rows = data_rows(live, 12U, 4U);
    mcpp::detail::append_viewer_eof_rows(rows, "EOF!", live, 4U);
    check(live.data_size() == 4U && row_text(draw(std::move(rows), 12, 4), 1) == "EOF!",
          "the small growing file initially has presentation padding");
    write(growing, "two\n", std::ios::app);
    rows = data_rows(live, 12U, 4U);
    mcpp::detail::append_viewer_eof_rows(rows, "EOF!", live, 4U);
    auto refreshed = draw(std::move(rows), 12, 4);
    check(live.data_size() == 8U && live.source_view_end() == 8U &&
          row_text(refreshed, 1) == "two" && row_text(refreshed, 2) == "EOF!",
          "appended data replaces presentation padding through the existing refresh");
    write(growing, "Z\n");
    rows = data_rows(live, 12U, 4U);
    mcpp::detail::append_viewer_eof_rows(rows, "EOF!", live, 4U);
    refreshed = draw(std::move(rows), 12, 4);
    check(live.data_size() == 2U && live.source_view_end() == 2U &&
          row_text(refreshed, 0) == "Z" && row_text(refreshed, 1) == "EOF!",
          "truncation refresh remains independent of padding");
    live.toggle_hex();
    live.set_hex_cursor(1U, true);
    rows = data_rows(live, 12U, 4U);
    mcpp::detail::append_viewer_eof_rows(rows, "EOF!", live, 4U);
    check(rows.size() == 1U && live.hex_cursor() == 1U && !live.dirty(),
          "hex mode ignores the marker without moving the cursor or staging edits");

    struct Pipe {
        int input{-1};
        int output{-1};
        ~Pipe() {
            if (input >= 0) ::close(input);
            if (output >= 0) ::close(output);
        }
    } pipe;
    int descriptors[2]{};
    check(::pipe(descriptors) == 0, "create the tiny local stream");
    pipe.input = descriptors[0];
    pipe.output = descriptors[1];
    mcpp::ViewerModel stream;
    stream.open_stream(provider, pipe.input, "original pipe");
    rows = data_rows(stream, 24U, 4U);
    check(rows.empty() && stream.reading(), "an open empty stream remains in its reading state");
    rows.push_back(mcpp::glyph_text("Waiting for input..."));
    mcpp::detail::append_viewer_eof_rows(rows, "EOF!", stream, 4U);
    check(row_text(draw(std::move(rows), 24, 4), 0) == "Waiting for input...",
          "padding leaves the existing waiting status visible");
    check(::write(pipe.output, "live\n", 5U) == 5, "append a few bytes to the live stream");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    do {
        rows = data_rows(stream, 24U, 4U);
        if (stream.data_size() == 5U) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (std::chrono::steady_clock::now() < deadline);
    check(stream.data_size() == 5U && stream.reading(), "stream refresh receives the new bytes");
    mcpp::detail::append_viewer_eof_rows(rows, "EOF!", stream, 4U);
    check(row_text(draw(std::move(rows), 24, 4), 0) == "live" &&
          stream.source_view_end() == 5U && !stream.dirty(),
          "incoming stream bytes replace padding without changing byte accounting");
    stream.close();
}

}  // namespace

int main(const int argc, char** argv) {
    try {
        if (argc != 3 || ::geteuid() != 1001U || !std::filesystem::exists("/.dockerenv") ||
            !std::filesystem::exists("/review/.operation-regressions-disposable")) {
            throw std::runtime_error("EOF models require UID1001 in the owned review container");
        }
        const auto root = std::filesystem::absolute(argv[2]).lexically_normal();
        check(root.string().starts_with("/review/cases/viewer-eof/"),
              "use an owned viewer-eof case directory");
        std::filesystem::create_directories(root / "runtime");
        check(::setenv("TMPDIR", (root / "runtime").c_str(), 1) == 0,
              "keep any spool fallback in the owned case directory");
        const std::string_view selector(argv[1]);
        if (selector == "render") rendering(root);
        else if (selector == "state") state(root);
        else throw std::runtime_error("unknown EOF model selector");
        std::cout << selector << ": passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
