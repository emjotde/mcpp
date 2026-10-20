#include "mcpp/cli_display.hpp"
#include "mcpp/config.hpp"

#include <ftxui/screen/terminal.hpp>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace {

namespace fs = std::filesystem;

void require(const bool condition, const std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

void write(const fs::path& path, const std::string_view value) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path);
    output << value;
    require(static_cast<bool>(output), "write native color fixture");
}

std::string read(const fs::path& path) {
    std::ifstream input(path);
    require(static_cast<bool>(input), "read native color fixture");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

struct Environment {
    std::string name;
    std::optional<std::string> saved;

    explicit Environment(std::string key) : name(std::move(key)) {
        if (const auto* value = std::getenv(name.c_str())) saved = value;
    }

    ~Environment() {
        if (saved) ::setenv(name.c_str(), saved->c_str(), 1);
        else ::unsetenv(name.c_str());
    }
};

void native(const fs::path& root) {
    const auto path = root / "ini";
    write(path,
        "# retained native colors\n[Colors]\n"
        "base_color=normal=black,cyan,underline:editnormal=yellow,blue:input=yellow,blue,bold\n"
        "xterm-256color=normal=white,,bold:inputmark=black,cyan,underline\n"
        "unused-terminal=future-role=magenta\n"
        "color_terminals=vt100, xterm-256color\n[Future]\nkeep=raw\\q\n");
    auto config = mcpp::Config::load(path);
    require(config.color_tables.size() == 3U && config.diagnostics.size() == 1U,
            "native tables are conditional settings, while unknown sections remain diagnosed");
    const auto layers = mcpp::parse_config_colors(config, "xterm-256color");
    require(layers.size() == 2U && mcpp::parse_config_colors(config, "xterm").size() == 1U,
            "only the base table and exact TERM table are active");
    auto panels = mcpp::find_skin("grayscale");
    auto editor = panels;
    for (const auto& layer : layers) {
        mcpp::apply_cli_colors(panels, layer, mcpp::CliPaletteContext::panels);
        mcpp::apply_cli_colors(editor, layer, mcpp::CliPaletteContext::editor);
    }
    require(panels.normal.foreground == ftxui::Color::White &&
            panels.normal.background == ftxui::Color::Cyan &&
            panels.normal.attributes == mcpp::SkinPair::bold,
            "TERM overrides supplied fields and retains the base background");
    require(editor.normal.foreground == ftxui::Color::White &&
            editor.normal.background == ftxui::Color::Cyan,
            "higher-priority generic colors retain their own role's prior background");
    require(panels.colors.at("core").at("input").foreground == ftxui::Color::YellowLight &&
            panels.colors.at("core").at("inputmark").background == ftxui::Color::Cyan,
            "configuration uses the published input role bindings");
    require(config.color_terminal_listed("vt100") && !config.color_terminal_listed("vt") &&
            !config.color_terminal_listed("xterm"),
            "color_terminals matches complete terminal names");
    config.save();
    const auto saved = read(path);
    require(saved.find("# retained native colors") != std::string::npos &&
            saved.find("unused-terminal=future-role=magenta") != std::string::npos &&
            saved.find("[Future]\nkeep=raw\\q") != std::string::npos,
            "saving retains comments, inactive profiles and unknown data");
    struct stat status{};
    require(::stat(path.c_str(), &status) == 0 && (status.st_mode & 0777) == 0600,
            "native color settings retain private atomic saves");
    const auto restored = mcpp::Config::load(path);
    require(restored.color_tables.at("base_color").value == config.color_tables.at("base_color").value &&
            restored.color_terminals == config.color_terminals,
            "native color values survive restart");
}

void legacy(const fs::path& root) {
    const auto path = root / "config";
    write(path,
        "# retained legacy\neditor_tab_width=5\nlegacy_future=raw\\q\n"
        "color_terminals=vt100\n"
        "color_table=\"base_color\" \"normal=black,cyan,underline\"\n"
        "color_table=\"xterm-256color\" \"normal=white\"\n");
    auto config = mcpp::Config::load(path);
    const auto layers = mcpp::parse_config_colors(config, "xterm-256color", "normal=black,green");
    require(layers.size() == 3U, "environment follows both saved color layers");
    auto skin = mcpp::find_skin("grayscale");
    for (const auto& layer : layers) mcpp::apply_cli_colors(skin, layer, mcpp::CliPaletteContext::panels);
    const auto cli = mcpp::parse_cli_colors(std::array<std::string, 1>{"normal=white,,none"});
    mcpp::apply_cli_colors(skin, cli, mcpp::CliPaletteContext::panels);
    require(skin.normal.foreground == ftxui::Color::White &&
            skin.normal.background == ftxui::Color::Green && skin.normal.attributes == 0U,
            "precedence is base, TERM, environment, then CLI with partial-field inheritance");
    config.save();
    const auto saved = read(path);
    require(saved.find("normal=black,cyan,underline") != std::string::npos &&
            saved.find("green") == std::string::npos && saved.find(",,none") == std::string::npos &&
            saved.find("legacy_future=raw\\q") != std::string::npos,
            "launch-only color layers do not rewrite saved colors or unknown legacy settings");
    const auto restored = mcpp::Config::load(path);
    require(restored.editor_tab_width == 5U && restored.color_tables.size() == 2U,
            "flat preferences and saved color tables coexist");
    Environment term("TERM");
    Environment no_color("NO_COLOR");
    ::setenv("TERM", "vt100", 1);
    ::unsetenv("NO_COLOR");
    const auto forced = mcpp::configured_color_mode(config, {});
    require(forced == true && mcpp::configured_color_mode(config, false) == false,
            "listed terminals force colors without overriding explicit no-color");
    ftxui::Terminal::SetColorSupport(ftxui::Terminal::Palette1);
    mcpp::prepare_cli_terminal(forced, false);
    const auto forced_layers = mcpp::parse_config_colors(config, "vt100");
    require(ftxui::Terminal::ColorSupport() == ftxui::Terminal::Palette16 &&
            forced_layers.front().front().foreground == ftxui::Color::Black &&
            forced_layers.front().front().background == ftxui::Color::Cyan,
            "terminal forcing happens before color construction");
    ::setenv("NO_COLOR", "1", 1);
    require(!mcpp::configured_color_mode(config, {}).has_value() &&
            mcpp::configured_color_mode(config, true) == true,
            "NO_COLOR is retained unless launch flags explicitly force color");
    ::unsetenv("NO_COLOR");
    ::setenv("TERM", "vt", 1);
    require(!mcpp::configured_color_mode(config, {}).has_value(),
            "terminal forcing never uses prefix matching");
}

void invalid(const fs::path& root) {
    const auto good = root / "good.ini";
    const auto bad = root / "bad.ini";
    write(good, "[Colors]\nbase_color=normal=black,cyan\n");
    auto retained = mcpp::Config::load(good);
    auto retained_layers = mcpp::parse_config_colors(retained, "xterm-256color");
    const std::string bad_value = "[Colors]\nbase_color=normal=not-a-color\n";
    write(bad, bad_value);
    bool rejected = false;
    try {
        auto candidate = mcpp::Config::load(bad);
        auto candidate_layers = mcpp::parse_config_colors(candidate, "xterm-256color");
        retained = std::move(candidate);
        retained_layers = std::move(candidate_layers);
    } catch (const std::exception& error) {
        const std::string diagnostic(error.what());
        rejected = diagnostic.find(bad.string() + ":2: [Colors] base_color") != std::string::npos &&
                   diagnostic.find("not-a-color") != std::string::npos;
    }
    require(rejected && read(bad) == bad_value &&
            retained.color_tables.at("base_color").value == "normal=black,cyan" &&
            retained_layers.front().front().background == ftxui::Color::Cyan,
            "invalid active colors report their source without replacing accepted state or rewriting input");
    rejected = false;
    try {
        static_cast<void>(mcpp::parse_config_colors(retained, "xterm-256color", "commandlinemark=red"));
    } catch (const std::exception& error) {
        const std::string diagnostic(error.what());
        rejected = diagnostic.find("MC_COLOR_TABLE: unsupported color role: commandlinemark") != std::string::npos;
    }
    require(rejected, "unsupported environment roles are diagnosed rather than accepted as no-ops");
    write(bad, "[Colors]\ncolor_terminals=vt100,,xterm\n");
    rejected = false;
    try {
        retained = mcpp::Config::load(bad);
    } catch (const std::exception& error) {
        rejected = std::string(error.what()).find(":2: [Colors] color_terminals") != std::string::npos;
    }
    require(rejected && retained.color_tables.at("base_color").value == "normal=black,cyan",
            "malformed terminal lists reject transactionally with native key context");
}

}  // namespace

int main(const int argc, char** argv) {
    try {
        require(argc == 3, "usage: mcpp_native_color_tests CASE WORKDIR");
        require(::geteuid() == 1001U && fs::exists("/.dockerenv") &&
                fs::exists("/review/.operation-regressions-disposable"),
                "native colors require UID1001 in the owned review container");
        const auto root = fs::absolute(argv[2]).lexically_normal();
        require(root.string().starts_with("/review/cases/mc-config/colors/"),
                "use an owned mc-config/colors case directory");
        ::umask(0077);
        ftxui::Terminal::SetColorSupport(ftxui::Terminal::TrueColor);
        const std::string_view selected(argv[1]);
        if (selected == "native") native(root);
        else if (selected == "legacy") legacy(root);
        else if (selected == "invalid") invalid(root);
        else throw std::runtime_error("unknown native colors case");
        std::cout << "native colors " << selected << ": passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
