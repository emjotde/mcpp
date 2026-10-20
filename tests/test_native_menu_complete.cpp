#include "mcpp/local_tools.hpp"
#include "mcpp/native_menu.hpp"
#include "mcpp/user_menu.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {

void check(const bool value, const std::string_view message) {
    if (!value) throw std::runtime_error(std::string(message));
}

template <typename Function>
void rejects(Function&& function, const std::string_view message) {
    try {
        function();
    } catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error(std::string(message));
}

mcpp::Entry file(const std::string& name) {
    mcpp::Entry result;
    result.name = name;
    result.path = "/captured/" + name;
    result.kind = mcpp::EntryKind::file;
    return result;
}

mcpp::NativeMenuContext context(const std::filesystem::path& root) {
    mcpp::NativeMenuContext result;
    result.active.directory = root.string();
    result.active.file = file("odd ' $(printf BAD) `value`\\tail\n\"*.txt");
    result.active.tagged = {file("a space"), file("b'$(printf BAD)\\\n`tag`")};
    result.other.directory = "/other";
    result.other.file = file("opposite");
    return result;
}

std::string nul_values(const std::vector<std::string>& values) {
    std::string result;
    for (const auto& value : values) {
        result += value;
        result += '\0';
    }
    return result;
}

void output(
    const std::string_view command,
    const mcpp::NativeMenuContext& captured,
    const std::string& expected
) {
    const auto expanded = mcpp::expand_native_menu(command, captured);
    const auto result = mcpp::capture_shell(expanded.command, captured.active.directory);
    check(result.status == 0, "expanded shell command failed: " + result.error + "\n" + expanded.command);
    check(result.output == expected, "menu argument bytes changed\n" + expanded.command);
}

void words(const std::filesystem::path& root) {
    const auto captured = context(root);
    const auto joined = captured.active.tagged[0].name + " " + captured.active.tagged[1].name + " ";
    output("printf '%%s\\0' \"%t\" '%t' $'%t' \"pre%t/post\" %t", captured,
           nul_values({joined, joined, joined, "pre" + joined + "/post",
                       captured.active.tagged[0].name, captured.active.tagged[1].name}));
    const auto untagged = mcpp::expand_native_menu("printf '%%s\\0' \"%u\" \"%t\" \"%s\"", captured);
    const auto result = mcpp::capture_shell(untagged.command, root);
    check(result.status == 0 && result.output == nul_values({joined, "", captured.active.file->name}),
          "quoted untagging must affect subsequent tagged/selected macros");
    check(untagged.untag_active == std::vector<std::string>{
              captured.active.tagged[0].path, captured.active.tagged[1].path},
          "untagging must retain captured paths");
    auto raw = captured;
    raw.active.file = file("alpha beta");
    output("printf '%%s\\0' %0f \"%0f\"", raw, nul_values({"alpha", "beta", "alpha beta"}));
    raw.active.file = file("printf 'RAW-COMMAND'");
    output("%0f", raw, "RAW-COMMAND");
    check(mcpp::expand_user_menu_command("printf '%%s' %f", "/legacy/full name", "", "") ==
              "printf '%s' '/legacy/full name'",
          "legacy commands must keep full-path quoting");
    auto archive = captured;
    archive.active.local = false;
    rejects([&] { static_cast<void>(mcpp::expand_native_menu("%0f", archive)); },
            "raw opt-in must not bypass the archive command guard");
    archive = captured;
    archive.other.local = false;
    rejects([&] { static_cast<void>(mcpp::expand_native_menu("printf %0F", archive)); },
            "raw opposite-pane macros must retain their provider guard");
}

void contexts(const std::filesystem::path& root) {
    auto captured = context(root);
    const auto name = captured.active.file->name;
    output("printf '%%s\\0' \"`printf '%%s' %f`\" "
           "\"`printf '%%s' \"%f\"`\" $'left%f/right' \"$(printf '%%s' $'%f')\"",
           captured, nul_values({name, name, "left" + name + "/right", name}));
    output("cat <<'L20_LITERAL'\n%f\nL20_LITERAL\n"
           "cat <<L20_EXPANDING\n%f\nL20_EXPANDING\n"
           "printf '%%s' %f", captured, name + "\n" + name + "\n" + name);
    captured.active.file = file("008");
    output("printf '%%s\\0' $((%f+2)) $((%f %% 3))", captured, nul_values({"10", "2"}));
    captured.active.file = file("3+4");
    output("printf '%%s' $((%0f))", captured, "7");
    rejects([&] { static_cast<void>(mcpp::expand_native_menu("printf '%%s' $((%f))", captured)); },
            "ordinary arithmetic macros must not become shell expressions");
    captured.active.file = file("L20_LITERAL");
    rejects([&] { static_cast<void>(mcpp::expand_native_menu("cat <<'L20_LITERAL'\n%f\nL20_LITERAL", captured)); },
            "ordinary macro data must not terminate a here-document");
    rejects([&] { static_cast<void>(mcpp::expand_native_menu("cat <<%f\nbody\nL20_LITERAL", captured)); },
            "macro delimiters must fail explicitly");
    rejects([&] { static_cast<void>(mcpp::expand_native_menu("printf $'%f", captured)); },
            "unfinished ANSI quotes must fail explicitly");
    const auto requirements = mcpp::native_menu_requirements(
        "cat <<'L20_LITERAL'\n%{literal prompt} %b\nL20_LITERAL\n"
        "printf `printf '%%s' %e`", true);
    check(requirements.prompts == std::vector<std::string>{"literal prompt"} &&
              requirements.block_file && requirements.error_file,
          "requirements must use the same shell contexts as expansion");
}

void directories(const std::filesystem::path& root) {
    const auto base = root / "files";
    const auto search = root / "search";
    const auto home = root / "home";
    for (const auto& path : {base / "local", base / "~mcpp", base / "-",
                            search / "local", search / "sought", home / "find/found"}) {
        std::filesystem::create_directories(path);
    }
    auto captured = context(base);
    check(setenv("HOME", home.c_str(), 1) == 0 && setenv("CDPATH", search.c_str(), 1) == 0,
          "set isolated directory environment");
    check(mcpp::native_menu_directory("local", captured) == (base / "local").string(),
          "the captured current directory must take precedence over CDPATH");
    captured.active.file = file("sought");
    auto expansion = mcpp::expand_native_menu("%cd %f", captured);
    check(mcpp::native_menu_directory(expansion.command, captured) == (search / "sought").string(),
          "CDPATH must resolve a macro-generated relative directory");
    check(mcpp::native_menu_directory("~mcpp", captured) == "/home/mcpp" &&
              mcpp::native_menu_directory("~", captured) == home.string(),
          "named homes use the account database; bare tilde uses HOME");
    captured.active.file = file("~mcpp");
    expansion = mcpp::expand_native_menu("%cd %f", captured);
    check(mcpp::native_menu_directory(expansion.command, captured) == (base / "~mcpp").string(),
          "a quoted macro filename must not become a named-home request");
    check(mcpp::native_menu_directory("-", captured, "/captured.tar/utar://folder") ==
              "/captured.tar/utar://folder" &&
              mcpp::native_menu_directory("'-'", captured) == (base / "-").string(),
          "previous-directory requests must differ from literal dash filenames");
    check(setenv("CDPATH", ":~/find", 1) == 0, "set tilde CDPATH");
    check(mcpp::native_menu_directory("found", captured) == (home / "find/found").string(),
          "CDPATH entries must support tilde without running a shell");
    rejects([&] { static_cast<void>(mcpp::native_menu_directory("missing", captured)); },
            "an unresolved directory must fail explicitly");
    rejects([&] { static_cast<void>(mcpp::native_menu_directory("~mcpp_no_such_user_a639", captured)); },
            "an unknown named home must fail explicitly");
    rejects([&] { static_cast<void>(mcpp::native_menu_directory("$(printf injected)", captured)); },
            "directory handling must never run shell substitutions");
    captured.active.local = false;
    captured.active.directory = "/captured.tar/utar://folder";
    check(mcpp::native_menu_directory("found", captured) == "found" &&
              mcpp::native_menu_directory("'~mcpp'", captured) == "~mcpp",
          "archive-relative directories must never search the host CDPATH or account database");
}

}  // namespace

int main(int argc, char** argv) {
    if (geteuid() != 1001U || !std::filesystem::exists("/.dockerenv") ||
        !std::filesystem::exists("/review/.operation-regressions-disposable")) {
        std::cerr << "Menu checks require uid1001 in the approved disposable Docker container\n";
        return 1;
    }
    try {
        check(argc == 3, "usage: native-menu-complete words|contexts|directories /review/cases/menu-complete-*");
        const auto root = std::filesystem::path(argv[2]).lexically_normal();
        check(root.string().starts_with("/review/cases/menu-complete-"), "invalid disposable menu fixture root");
        std::filesystem::create_directories(root);
        const std::string_view selected = argv[1];
        if (selected == "words") words(root);
        else if (selected == "contexts") contexts(root);
        else if (selected == "directories") directories(root);
        else throw std::invalid_argument("unknown focused menu case");
        std::cout << "PASS native menu " << selected << '\n';
    } catch (const std::exception& error) {
        std::cerr << "FAIL native menu: " << error.what() << '\n';
        return 1;
    }
}
