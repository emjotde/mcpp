#include "mcpp/config.hpp"
#include "panel_reload_warning.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;
using mcpp::detail::maybe_show_fast_reload_warning;

void check(const bool condition, const std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

void write(const fs::path& path, const std::string_view contents) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path);
    output << contents;
    check(static_cast<bool>(output), "write original warning preference fixture");
}

std::string read(const fs::path& path) {
    std::ifstream input(path);
    check(static_cast<bool>(input), "read original warning preference fixture");
    return {std::istreambuf_iterator<char>(input), {}};
}

void native(const fs::path& root) {
    const auto path = root / "ini";
    write(path,
        "# original warning state\n[Midnight-Commander]\n"
        "fast_refresh=true\nmax_dirt_limit=3\n"
        "[Panels]\nfast_reload=true\nfast_reload_msg_shown=false\n");
    auto config = mcpp::Config::load(path);
    check(config.fast_reload_msg_shown == false && config.diagnostics.size() == 2U,
          "only the two backend-specific keys remain unsupported");
    config.save();
    check(mcpp::Config::load(path).fast_reload_msg_shown == false,
          "loading and saving without applying options must not acknowledge the warning");

    int warnings = 0;
    bool legacy_shown = true;
    maybe_show_fast_reload_warning(true, false, config.fast_reload_msg_shown, legacy_shown,
        [&] { ++warnings; });
    check(warnings == 0 && config.fast_reload_msg_shown == false,
          "disabled fast reload must not show or acknowledge a warning");
    bool failed = false;
    try {
        maybe_show_fast_reload_warning(true, true, config.fast_reload_msg_shown, legacy_shown,
            [] { throw std::runtime_error("original failed warning"); });
    } catch (const std::runtime_error&) {
        failed = true;
    }
    check(failed && config.fast_reload_msg_shown == false,
          "a failed warning must leave its saved acknowledgement false");
    maybe_show_fast_reload_warning(true, config.fast_reload, config.fast_reload_msg_shown, legacy_shown,
        [&] {
            check(config.fast_reload_msg_shown == false,
                  "the warning must be shown before its acknowledgement changes");
            ++warnings;
        });
    check(warnings == 1 && config.fast_reload_msg_shown == true,
          "explicit false must warn even when already enabled or legacy state is true");
    config.save();
    config = mcpp::Config::load(path);
    legacy_shown = false;
    maybe_show_fast_reload_warning(false, true, config.fast_reload_msg_shown, legacy_shown,
        [&] { ++warnings; });
    const auto saved = read(path);
    check(warnings == 1 && config.fast_reload_msg_shown == true &&
          saved.find("# original warning state") != std::string::npos &&
          saved.find("fast_refresh=true") != std::string::npos &&
          saved.find("max_dirt_limit=3") != std::string::npos &&
          config.diagnostics.size() == 2U,
          "saved true must suppress later warnings while unsupported keys remain diagnosed and preserved");

    const auto invalid = root / "invalid/ini";
    const std::string original = "[Panels]\nfast_reload_msg_shown=2\n";
    write(invalid, original);
    bool rejected = false;
    try {
        static_cast<void>(mcpp::Config::load(invalid));
    } catch (const std::runtime_error& error) {
        rejected = std::string_view(error.what()).find("invalid boolean") != std::string_view::npos;
    }
    check(rejected && read(invalid) == original,
          "an invalid saved warning flag must be rejected without rewriting its profile");
}

void legacy(const fs::path& root) {
    const auto path = root / "ini";
    write(path, "[Panels]\nfast_reload=false\n");
    auto config = mcpp::Config::load(path);
    check(!config.fast_reload_msg_shown.has_value(), "an absent warning flag must remain absent");
    bool legacy_shown = false;
    int warnings = 0;
    const auto warn = [&] { ++warnings; };
    maybe_show_fast_reload_warning(false, false, config.fast_reload_msg_shown, legacy_shown, warn);
    config.fast_reload = true;
    maybe_show_fast_reload_warning(false, true, config.fast_reload_msg_shown, legacy_shown, warn);
    maybe_show_fast_reload_warning(true, true, config.fast_reload_msg_shown, legacy_shown, warn);
    maybe_show_fast_reload_warning(false, true, config.fast_reload_msg_shown, legacy_shown, warn);
    config.save();
    check(warnings == 1 && legacy_shown && !config.fast_reload_msg_shown.has_value() &&
          read(path).find("fast_reload_msg_shown") == std::string::npos,
          "legacy toggles must warn once per launch without emitting a new key");

    config = mcpp::Config::load(path);
    legacy_shown = false;
    maybe_show_fast_reload_warning(true, true, config.fast_reload_msg_shown, legacy_shown, warn);
    check(warnings == 1 && !legacy_shown,
          "a pre-enabled legacy profile must not gain a new apply-time warning");
    maybe_show_fast_reload_warning(false, true, config.fast_reload_msg_shown, legacy_shown, warn);
    check(warnings == 2 && legacy_shown && !config.fast_reload_msg_shown.has_value(),
          "an absent flag retains its per-launch legacy warning after restart");

    const auto flat = root / "flat/config";
    write(flat, "fast_reload=false\n");
    config = mcpp::Config::load(flat);
    legacy_shown = false;
    config.fast_reload = true;
    maybe_show_fast_reload_warning(false, true, config.fast_reload_msg_shown, legacy_shown, warn);
    config.save();
    check(warnings == 3 && legacy_shown && !config.fast_reload_msg_shown.has_value() &&
          read(flat).find("fast_reload_msg_shown") == std::string::npos,
          "flat profiles must also preserve absence after a real warning");
}

}  // namespace

int main(const int argc, char** argv) {
    try {
        check(argc == 3 && ::geteuid() == 1001U && fs::exists("/.dockerenv") &&
              fs::exists("/review/.operation-regressions-disposable"),
              "run MODE CASE_ROOT only in the owned UID1001 disposable container");
        const auto root = fs::absolute(argv[2]).lexically_normal();
        check(root.string().starts_with("/review/cases/") && !fs::exists(root),
              "use a fresh fixture root beneath /review/cases");
        const std::string_view mode(argv[1]);
        if (mode == "native") native(root);
        else if (mode == "legacy") legacy(root);
        else throw std::runtime_error("expected native or legacy warning group");
        std::cout << "panel reload warning " << mode << ": passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
