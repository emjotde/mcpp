#include "mcpp/extension.hpp"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <unistd.h>

namespace {

void check(const bool value, const std::string_view message) {
    if (!value) throw std::runtime_error(std::string(message));
}

void matching_case() {
    using mcpp::ExtensionAction;
    using mcpp::ExtensionRules;
    mcpp::Entry entry;
    entry.kind = mcpp::EntryKind::file;
    entry.name = "sample.TXT";
    const auto rules = ExtensionRules::parse(
        "[mc.ext.ini]\nVersion=4.0\n"
        "[By name]\nShell=.txt\nShellIgnoreCase=true\nInclude=text\nOpen=ignored\n"
        "[By contents]\nRegex=\\\\.TXT$\nType=^ASCII\nView=%view\n"
        "[Include/text]\nOpen=first\nEdit=\n"
        "[By contents]\nView=%view{unform}\n"
        "[By MIME]\nMime=^text/\nEdit=wrong\n"
        "[Directory]\nDirectory=^sample\nShell=no-match\nType=unavailable\nOpen=%cd %f\n"
        "[Default]\nOpen=default\nView=default\nEdit=default\n",
        "independent-extension.ini"
    );
    int probes = 0;
    const auto probe = [&probes](const mcpp::ExtensionType type) {
        ++probes;
        return type == mcpp::ExtensionType::description ? "ASCII text" : "text/plain";
    };
    check(rules.match(entry, ExtensionAction::open, true, probe)->command == "first",
          "first match and Include override referring actions");
    check(probes == 0, "filename-only action does not probe file types");
    check(rules.match(entry, ExtensionAction::view, true, probe)->command == "%view{unform}",
          "missing actions continue; duplicate sections keep their order and final key");
    check(rules.match(entry, ExtensionAction::edit, true, probe)->command.empty(),
          "an explicitly empty action blocks fallback");
    entry.kind = mcpp::EntryKind::directory;
    check(rules.match(entry, ExtensionAction::open, false)->command == "%cd %f",
          "Directory takes precedence without reading host metadata");
    entry.kind = mcpp::EntryKind::file;
    entry.name = "unmatched";
    check(rules.match(entry, ExtensionAction::edit, true, probe)->section == "By MIME",
          "MIME condition uses the requested probe");
    check(rules.match(entry, ExtensionAction::open, true, probe)->command == "default",
          "Default is used only after normal sections");

    const auto legacy = ExtensionRules::parse(
        "shell/i/.txt\n\tView=%view\n"
        "regex/\\.TXT$\n\tOpen=legacy-native\n"
        "*.TXT=legacy-simple %f\n"
    );
    entry.name = "sample.TXT";
    check(legacy.match(entry, ExtensionAction::view, true)->command == "%view",
          "legacy native extension conditions and actions remain usable");
    check(!legacy.match(entry, ExtensionAction::open, true)->legacy,
          "native legacy action precedes simple pattern format");
    entry.name = "other.TXT";
    const auto simple = ExtensionRules::parse("*.TXT=legacy-simple %f\n");
    check(simple.match(entry, ExtensionAction::open, true)->legacy,
          "mcpp pattern=command format retains full-path expansion mode");
    bool rejected = false;
    try {
        static_cast<void>(ExtensionRules::parse(
            "[mc.ext.ini]\nVersion=4.0\n[bad]\nShell=.txt\nInclude=missing\n"
        ));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected, "missing includes are invalid resources, not unmatched rules");
}

}  // namespace

int main() {
    if (geteuid() != 1001U || !std::filesystem::exists("/.dockerenv") ||
        !std::filesystem::exists("/review/.operation-regressions-disposable")) {
        std::cerr << "Extension checks require uid1001 in the approved disposable Docker container\n";
        return 1;
    }
    try {
        matching_case();
        std::cout << "PASS one native and legacy extension matching case\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL extension matching: " << error.what() << '\n';
        return 1;
    }
}
