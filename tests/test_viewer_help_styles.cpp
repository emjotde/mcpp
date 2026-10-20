#include "mcpp/cli_display.hpp"
#include "mcpp/command_line.hpp"
#include "mcpp/help.hpp"
#include "mcpp/local_provider.hpp"
#include "mcpp/skin_glyphs_ui.hpp"
#include "viewer_help_styles.hpp"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/terminal.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>

namespace {

using mcpp::SkinPair;

constexpr unsigned all_attributes =
    SkinPair::bold | SkinPair::italic | SkinPair::underline | SkinPair::reverse | SkinPair::blink;
constexpr std::string_view nroff = "ORIGINAL PLAIN B\bB _\bU\n";

void check(const bool condition, const std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

void write(const std::filesystem::path& path, const std::string_view contents) {
    std::ofstream output(path, std::ios::binary);
    output << contents;
    check(static_cast<bool>(output), "write the original widget-style fixture");
}

std::string read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}

void pair(const ftxui::Cell& cell, const SkinPair& expected, const std::string_view message) {
    const auto attributes = (cell.bold ? SkinPair::bold : 0U) |
        (cell.italic ? SkinPair::italic : 0U) | (cell.underlined ? SkinPair::underline : 0U) |
        (cell.inverted ? SkinPair::reverse : 0U) | (cell.blink ? SkinPair::blink : 0U);
    check(cell.foreground_color == expected.foreground && cell.background_color == expected.background &&
          cell.bold == ((expected.attributes & SkinPair::bold) != 0U) &&
          cell.italic == ((expected.attributes & SkinPair::italic) != 0U) &&
          cell.underlined == ((expected.attributes & SkinPair::underline) != 0U) &&
          cell.inverted == ((expected.attributes & SkinPair::reverse) != 0U) &&
          cell.blink == ((expected.attributes & SkinPair::blink) != 0U) &&
          !cell.dim && !cell.underlined_double && !cell.strikethrough,
          std::string(message) + " (actual " + cell.foreground_color.Print(false) + "/" +
              cell.background_color.Print(true) + "/" + std::to_string(attributes) +
              "; expected " + expected.foreground.Print(false) + "/" +
              expected.background.Print(true) + "/" + std::to_string(expected.attributes) + ")");
}

ftxui::Screen render(ftxui::Element element, const SkinPair& normal) {
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(60), ftxui::Dimension::Fixed(1));
    ftxui::Render(screen, mcpp::detail::paint_skin_style(std::move(element), normal));
    return screen;
}

void styles(const std::filesystem::path& root) {
    using mcpp::detail::HelpTextStyle;
    const auto document = root / "original.nroff";
    const auto resource = root / "original.ini";
    write(document, nroff);
    write(resource, R"([core]
_default_=lightgray;black;bold+italic+underline+reverse+blink
[viewer]
_default_=white;blue;bold+italic+underline+reverse+blink
viewbold=#ffee44;#402050;none
viewunderline=#80ccff;#103040;none
[help]
_default_=white;blue;bold+italic+underline+reverse+blink
helpbold=#ccee88;#234527;none
helpitalic=#ffaa88;#4a2535;none
)");
    auto skin = mcpp::load_skin(resource.string(), root);
    const auto resource_bytes = read(resource);
    auto viewer = mcpp::ViewerModel{};
    viewer.open(std::make_shared<mcpp::LocalProvider>(), document.string());
    viewer.toggle_nroff();
    const auto lines = viewer.lines(60, 1);
    check(lines == std::vector<std::string>{"ORIGINAL PLAIN B U"} &&
          viewer.text_styles(0).size() == 2U &&
          viewer.text_styles(0)[0].bold && viewer.text_styles(0)[1].underline,
          "the actual viewer supplies nroff text and independent style ranges");
    const auto bold = lines[0].find("B");
    const auto underlined = lines[0].find("U");
    const auto draw = [&](const std::optional<std::pair<std::size_t, std::size_t>> match = {}) {
        return render(mcpp::detail::render_viewer_text(skin, lines[0], match, viewer.text_styles(0)),
                      skin.normal);
    };
    auto screen = draw();
    pair(screen.CellAt(static_cast<int>(bold), 0), skin.colors.at("viewer").at("viewbold"),
         "native viewbold replaces foreground, background and inherited attributes");
    pair(screen.CellAt(static_cast<int>(underlined), 0), skin.colors.at("viewer").at("viewunderline"),
         "native viewunderline honors explicit none without forced underline");
    pair(screen.CellAt(0, 0), skin.normal, "plain file text keeps its ambient pair");
    screen = render(mcpp::detail::render_viewer_text(
        skin, "C", {}, {{0U, 1U, true, true}}), skin.normal);
    pair(screen.CellAt(0, 0), skin.colors.at("viewer").at("viewunderline"),
         "combined overstrike styles use the native control's underline priority");

    mcpp::HelpBrowser browser(mcpp::HelpTopic::browser);
    browser.resize(60, 8);
    const auto heading = std::ranges::find_if(browser.lines(), [](const auto& line) {
        return line.heading;
    });
    check(heading != browser.lines().end() && !heading->text.empty(),
          "the independently written help has a real bold heading");
    auto help = render(mcpp::detail::help_text_style(
        mcpp::glyph_text(heading->text), skin, HelpTextStyle::bold), skin.normal);
    pair(help.CellAt(0, 0), skin.colors.at("help").at("helpbold"),
         "native helpbold honors explicit none on the original help heading");
    help = render(mcpp::detail::help_text_style(
        mcpp::glyph_text("Tab: links"), skin, HelpTextStyle::italic), skin.normal);
    pair(help.CellAt(0, 0), skin.colors.at("help").at("helpitalic"),
         "native helpitalic honors explicit none on the original navigation hint");
    auto selected = skin.selected;
    selected.attributes = 0U;
    help = render(mcpp::detail::paint_skin_style(
        mcpp::glyph_text("original link"), selected) | ftxui::underlined | ftxui::bold,
        skin.normal);
    auto selected_expected = selected;
    selected_expected.attributes = SkinPair::underline;
    pair(help.CellAt(0, 0), selected_expected,
         "the extracted painter preserves the existing selected-link decorators");
    for (const auto role : {"viewbold", "viewunderline"}) {
        skin.colors["viewer"][role].attributes = all_attributes;
    }
    for (const auto role : {"helpbold", "helpitalic"}) {
        skin.colors["help"][role].attributes = all_attributes;
    }
    screen = draw();
    pair(screen.CellAt(static_cast<int>(bold), 0), skin.colors.at("viewer").at("viewbold"),
         "formatted viewer roles support all five native attributes");
    pair(screen.CellAt(static_cast<int>(underlined), 0), skin.colors.at("viewer").at("viewunderline"),
         "underlined viewer roles support all five native attributes");
    for (const auto emphasis : {HelpTextStyle::bold, HelpTextStyle::italic}) {
        help = render(mcpp::detail::help_text_style(mcpp::glyph_text("original"), skin, emphasis),
                      skin.normal);
        pair(help.CellAt(0, 0), skin.colors.at("help").at(
            emphasis == HelpTextStyle::bold ? "helpbold" : "helpitalic"),
            "both help roles support all five native attributes");
    }
    skin.normal.attributes = 0U;
    screen = draw(std::pair{bold, bold + 1U});
    auto expected = skin.normal;
    expected.attributes = SkinPair::reverse | SkinPair::bold;
    pair(screen.CellAt(static_cast<int>(bold), 0), expected,
         "search emphasis has priority over a formatted native color role");

    skin.colors["viewer"].erase("viewbold");
    skin.colors["viewer"].erase("viewunderline");
    skin.colors["help"].erase("helpbold");
    skin.colors["help"].erase("helpitalic");
    screen = draw();
    expected = skin.normal;
    expected.attributes = SkinPair::bold;
    pair(screen.CellAt(static_cast<int>(bold), 0), expected,
         "a missing native bold key keeps legacy bold despite an explicit section default");
    expected.attributes = SkinPair::underline;
    pair(screen.CellAt(static_cast<int>(underlined), 0), expected,
         "a missing native underline key keeps legacy underline");
    for (const auto emphasis : {HelpTextStyle::bold, HelpTextStyle::italic}) {
        help = render(mcpp::detail::help_text_style(mcpp::glyph_text("original"), skin, emphasis),
                      skin.normal);
        expected.attributes = emphasis == HelpTextStyle::bold ? SkinPair::bold : SkinPair::italic;
        pair(help.CellAt(0, 0), expected, "a missing help role keeps its fallback emphasis");
    }
    viewer.close();
    check(read(document) == nroff && read(resource) == resource_bytes,
          "formatting and role rendering never rewrite source or skin bytes");
}

void cli(const std::filesystem::path& root) {
    const auto options = mcpp::parse_cli(std::array<std::string_view, 4>{
        "-C", "viewbold=#abc,,none:helpbold=white,blue,none",
        "--colors=viewunderline=brightgreen,color17,italic+blink:helpitalic=yellow,red,underline",
        "literal:17"
    });
    check(options.paths == std::vector<std::string>{"literal:17"} && options.colors.size() == 2U,
          "-C and --colors collect the original role specifications without rewriting operands");
    const auto overrides = mcpp::parse_cli_colors(options.colors);
    auto source = mcpp::find_skin("gray");
    source.colors["viewer"]["_default_"] = {ftxui::Color::White, ftxui::Color::Blue, 0U, true};
    source.colors["help"]["_default_"] = {ftxui::Color::Cyan, ftxui::Color::Black, 0U, true};
    for (const auto context : {mcpp::CliPaletteContext::panels, mcpp::CliPaletteContext::editor,
                              mcpp::CliPaletteContext::viewer, mcpp::CliPaletteContext::help}) {
        auto skin = source;
        const auto normal = skin.normal;
        mcpp::apply_cli_colors(skin, overrides, context);
        check(skin.normal.foreground == normal.foreground &&
              skin.normal.background == normal.background && skin.normal.attributes == normal.attributes,
              "map-only emphasis roles never replace the ambient body pair");
        check((mcpp::find_skin_color(skin, "viewer", "viewbold") != nullptr) ==
                  (context == mcpp::CliPaletteContext::viewer) &&
              (mcpp::find_skin_color(skin, "help", "helpbold") != nullptr) ==
                  (context == mcpp::CliPaletteContext::help),
              "viewer and help role overrides stay in their own palettes");
        if (context == mcpp::CliPaletteContext::viewer) {
            const auto& bold = skin.colors.at("viewer").at("viewbold");
            check(bold.foreground == ftxui::Color::RGB(170, 187, 204) &&
                  bold.background == ftxui::Color::Blue && bold.attributes == 0U && bold.explicit_style,
                  "CLI viewbold resolves the native canonical map and honors explicit none");
            const auto& underlined = skin.colors.at("viewer").at("viewunderline");
            check(underlined.foreground == ftxui::Color::GreenLight &&
                  underlined.background == ftxui::Color(static_cast<ftxui::Color::Palette256>(17)) &&
                  underlined.attributes == (SkinPair::italic | SkinPair::blink),
                  "CLI viewunderline accepts its independent full color pair and attributes");
        }
        if (context == mcpp::CliPaletteContext::help) {
            check(skin.colors.at("help").at("helpbold").attributes == 0U &&
                  skin.colors.at("help").at("helpitalic").attributes == SkinPair::underline,
                  "CLI help emphasis attributes do not force bold or italic");
        }
    }
    const auto partial = mcpp::parse_cli_colors(std::array<std::string, 1>{
        "viewbold=red:viewunderline=green:helpbold=yellow:helpitalic=cyan"
    });
    for (const auto context : {mcpp::CliPaletteContext::viewer, mcpp::CliPaletteContext::help}) {
        auto skin = source;
        mcpp::apply_cli_colors(skin, partial, context);
        const auto section = context == mcpp::CliPaletteContext::viewer ? "viewer" : "help";
        for (const auto& [role, attributes] : std::array{
                 std::pair{"viewbold", SkinPair::bold},
                 std::pair{"viewunderline", SkinPair::underline},
                 std::pair{"helpbold", SkinPair::bold},
                 std::pair{"helpitalic", SkinPair::italic}}) {
            if (const auto* colors = mcpp::find_skin_color(skin, section, role)) {
                check(colors->attributes == attributes && colors->explicit_style,
                      "omitted CLI attributes keep the missing native role's fallback emphasis");
            }
        }
    }
    auto skin = source;
    skin.colors["viewer"]["viewbold"] = {ftxui::Color::White, ftxui::Color::Magenta, 0U, true};
    mcpp::apply_cli_colors(skin, partial, mcpp::CliPaletteContext::viewer);
    check(skin.colors.at("viewer").at("viewbold").attributes == 0U &&
          skin.colors.at("viewer").at("viewbold").background == ftxui::Color::Magenta,
          "omitted CLI fields preserve an existing native role's explicit none and background");
    for (const auto role : {"viewbold", "viewunderline", "helpbold", "helpitalic"}) {
        check(mcpp::cli_color_roles().find(role) != std::string_view::npos,
              "CLI help advertises exactly these independently consumed emphasis roles");
    }
    bool rejected = false;
    try {
        static_cast<void>(mcpp::parse_cli_colors(std::array<std::string, 1>{"input=red"}));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected, "unimplemented independent input roles remain rejected");
    check(std::filesystem::is_directory(root), "the CLI model stays in its owned case directory");
}

}  // namespace

int main(const int argc, char** argv) {
    try {
        if (argc != 3 || ::geteuid() != 1001U || !std::filesystem::exists("/.dockerenv") ||
            !std::filesystem::exists("/review/.operation-regressions-disposable")) {
            throw std::runtime_error("widget-style models require UID1001 in the owned review container");
        }
        const auto root = std::filesystem::absolute(argv[2]).lexically_normal();
        check(root.string().starts_with("/review/cases/viewer-help-colors/"),
              "use an owned viewer-help-colors case directory");
        std::filesystem::create_directories(root);
        ftxui::Terminal::SetColorSupport(ftxui::Terminal::TrueColor);
        const std::string_view selector(argv[1]);
        if (selector == "styles") styles(root);
        else if (selector == "cli") cli(root);
        else throw std::runtime_error("unknown widget-style model selector");
        std::cout << selector << ": passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
