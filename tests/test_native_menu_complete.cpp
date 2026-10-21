#include "mcpp/local_tools.hpp"
#include "mcpp/native_menu.hpp"
#include "mcpp/user_menu.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
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
    const std::string& expected,
    const std::vector<std::string>& answers = {},
    const std::string_view executable = "/bin/bash"
) {
    const auto expanded = mcpp::expand_native_menu(command, captured, answers);
    const auto result = mcpp::capture_command(
        {std::string(executable), "-c", expanded.command}, captured.active.directory);
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

void ansi_heredocs(const std::filesystem::path& root) {
    auto captured = context(root);
    captured.active.file = file("Original ' \" \\ $(printf DATA) `printf DATA`\n* %f");
    captured.active.tagged = {
        file("tag one ' \" $(printf DATA)"),
        file("tag two \\ `printf DATA`\n? %f"),
    };
    captured.editor = true;
    captured.block_file = "block ' \" \\ $(printf DATA)";
    captured.error_file = "error `printf DATA`\n%f";
    const std::string answer = "Original answer ' \" \\ $(printf DATA) `printf DATA`\n%f";
    const auto name = captured.active.file->name;
    const auto tagged = captured.active.tagged[0].name + " " + captured.active.tagged[1].name + " ";
    const std::string command = R"MENU(cat <<$'L20_\x41NSI'
Original $HOME $(printf DATA) `printf DATA` \
%f
%{ANSI bytes}
%t
%b
%e
%0f
L20_ANSI
cat <<"pre"$'\105OF\\\'\"\?'"post"
%f
preEOF\'"?post
cat <<-$'L20_TABS'
	%f
	L20_TABS
printf '%%s\0' "$(cat <<$'L20_NESTED'
%f
L20_NESTED
)"
)MENU";
    const auto requirements = mcpp::native_menu_requirements(command, true);
    check(requirements.prompts == std::vector<std::string>{"ANSI bytes"} &&
              requirements.block_file && requirements.error_file,
          "ANSI delimiter requirements must share literal-body and editor macro contexts");
    output(command, captured,
           "Original $HOME $(printf DATA) `printf DATA` \\\n" +
               name + "\n" + answer + "\n" + tagged + "\n" +
               captured.block_file + "\n" + captured.error_file + "\n" + name + "\n" +
               name + "\n" + name + "\n" + name + '\0',
           {answer});
    const auto parsed = mcpp::parse_native_menu(
        "a ANSI source\n    cat <<$'L20_END'\n    %f\n    L20_END\n", "ansi-source.menu");
    check(parsed.size() == 1U && parsed[0].source == "ansi-source.menu" && parsed[0].line == 1U,
          "ANSI menu parsing must retain its error source");
    output(parsed[0].command, captured, name + "\n");

    const auto reason = [](auto&& function, const std::string_view expected) {
        try {
            function();
        } catch (const std::invalid_argument& error) {
            check(std::string_view(error.what()).find(expected) != std::string_view::npos,
                  "ANSI delimiter failed for an unexpected reason");
            return;
        }
        throw std::runtime_error("an invalid ANSI delimiter was accepted");
    };
    for (const auto& [source, expected] : std::vector<std::pair<std::string, std::string>>{
             {"cat <<$'EOF", "unfinished ANSI-C"},
             {"cat <<$'EOF\\", "unfinished escape"},
             {"cat <<$'EOF\\x'", "hexadecimal escape needs a digit"},
             {"cat <<$'EOF\\q'", "unsupported escape"},
             {"cat <<$'EOF\\u0041'", "unsupported escape"},
             {"cat <<$'EOF\\0'", "NUL and line breaks"},
             {"cat <<$'EOF\\400'", "NUL and line breaks"},
             {"cat <<$'EOF\\n'", "NUL and line breaks"},
             {"cat <<$'EOF\\r'", "NUL and line breaks"},
             {"cat <<$'EOF%f'", "menu macros in here-document delimiters"},
             {"cat <<$'EOF%var{DELIMITER:END}'", "menu macros in here-document delimiters"},
             {"cat <<$\"EOF\"", "locale quoted here-document delimiters"},
         }) {
        reason([&] { static_cast<void>(mcpp::native_menu_requirements(source, true)); }, expected);
        reason([&] { static_cast<void>(mcpp::expand_native_menu(source, captured)); }, expected);
    }
    reason([&] { static_cast<void>(mcpp::expand_native_menu(command, captured)); },
           "prompt has not been answered");
    captured.active.file = file("L20_END");
    reason([&] {
        static_cast<void>(mcpp::expand_native_menu("cat <<$'L20\\x5fEND'\n%f\nL20_END\n", captured));
    }, "would terminate a here-document");
    captured.active.file = file("\tOriginal");
    reason([&] {
        static_cast<void>(mcpp::expand_native_menu("cat <<-$'L20_TABS'\n%f\nL20_TABS\n", captured));
    }, "would lose leading tabs");
}

void backticks(const std::filesystem::path& root) {
    const auto legacy = [](const std::string_view body) {
        std::string result = "`";
        for (const auto character : body) {
            if (character == '\\' || character == '$' || character == '`') result += '\\';
            result += character;
        }
        return result + '`';
    };
    auto captured = context(root);
    captured.editor = true;
    captured.block_file = "/captured/block ' \" `printf DATA`\\tail\nbytes";
    captured.error_file = "/captured/error $(printf DATA)\\bytes";
    const auto name = captured.active.file->name;
    const auto names = name + name + name;
    const auto joined = captured.active.tagged[0].name + " " + captured.active.tagged[1].name + " ";
    const auto nested = legacy(
        "printf '%%s' \"" + legacy("printf '%%s' %f '%f' \"%f\"") + "\"");
    const auto triple = legacy("printf '%%s' \"" + nested + "\"");
    const auto mixed = legacy(
        "printf '%%s' \"$(printf '%%s' \"" +
        legacy("printf '%%s' %{Legacy bytes} %b %e") + "\")\"");
    const auto tagged = "$(printf '%%s' \"" +
        legacy("printf '%%s' \"" + legacy("printf '<%%s>' %t \"%t\"") + "\"") + "\")";
    const auto literal = legacy(
        "printf '%%s' \"" + legacy("printf '%%s' 'literal ` dollar $ slash \\ quote \"'") + "\"");
    const auto quotes = legacy("printf '%%s' \"" + legacy("printf '%%s' \\\"%f\\\"") + "\"");
    const auto comment = legacy(
        "printf '%%s' \"" + legacy("printf '%%s' %f # %{Ignored} %b %e") + "\"");
    const auto ignored = mcpp::native_menu_requirements(comment, true);
    check(ignored.prompts.empty() && !ignored.block_file && !ignored.error_file,
          "legacy delimiters must end comments without collecting their macros");
    const std::string label =
        R"MENU(`printf '%%s' "\`printf '%%s' %{Label ` \ $}\`"`)MENU";
    const std::string command =
        "printf '%%s\\0' \"" + nested + "\" \"" + triple + "\" \"" + mixed +
        "\" \"" + tagged + "\" \"" + literal + "\" \"" + quotes + "\" \"" +
        comment + "\" \"" + label + "\"";
    const auto requirements = mcpp::native_menu_requirements(command, true);
    check(requirements.prompts == std::vector<std::string>{"Legacy bytes", "Label ` \\ $"} &&
              requirements.block_file && requirements.error_file,
          "nested legacy requirements must retain authored macro arguments and editor files");
    const std::string answer = "reply ' \" \\ $(printf DATA) `printf DATA` %f\ncafé";
    for (const auto* executable : {"/bin/bash", "/bin/dash"}) {
        output(command, captured,
               nul_values({names, names, answer + captured.block_file + captured.error_file,
                           "<" + captured.active.tagged[0].name + "><" +
                               captured.active.tagged[1].name + "><" + joined + ">",
                           "literal ` dollar $ slash \\ quote \"", "\"" + name + "\"",
                           name, answer}),
               {answer, answer}, executable);
    }
    auto trusted = captured;
    trusted.active.file = file("printf 'trusted %f'");
    const auto raw = legacy("printf '%%s' \"" + legacy("%0f") + "\"");
    auto expected = raw;
    expected.replace(expected.find("%%"), 2U, "%");
    expected.replace(expected.find("%0f"), 3U, trusted.active.file->name);
    check(mcpp::expand_native_menu(raw, trusted).command == expected,
          "raw legacy macros remain trusted source without rescanning or added escape layers");
    for (const auto malformed : {
             R"MENU(printf '%%s' "`printf '%%s' \`printf '%%s' %f`")MENU",
             R"MENU(printf '%%s' "`printf '%%s' \`printf '%%s' %f\\\``")MENU",
             R"MENU(printf '%%s' "`printf '%%s' \`printf '%%s' '%f\``")MENU",
             R"MENU(printf '%%s' "`printf '%%s' $(printf '%%s' %f`")MENU"}) {
        rejects([&] { static_cast<void>(mcpp::expand_native_menu(malformed, captured)); },
                "malformed nested legacy quoting must fail during expansion");
        rejects([&] { static_cast<void>(mcpp::native_menu_requirements(malformed, true)); },
                "requirements must reject the same malformed nested legacy quoting");
    }
}

void numbers(const std::filesystem::path& root) {
    auto captured = context(root);
    for (const auto* executable : {"/bin/bash", "/bin/dash"}) {
        for (const auto& [value, expected] : std::vector<std::pair<std::string, std::string>>{
                 {"008", "10"}, {"-007", "-5"}, {"000", "2"}, {"-000", "2"},
                 {"+0009", "11"}, {"0x2a", "44"}, {"0Xf", "17"},
                 {"+0x00Af", "177"}, {"-0Xf", "-13"}}) {
            captured.active.file = file(value);
            output("printf '%%s' $((%f+2))", captured, expected, {}, executable);
            output("printf '%%s' $((%{Integer}+2))", captured, expected, {value}, executable);
        }
        captured.active.tagged = {file("0x2a")};
        output("printf '%%s' $((%t+2))", captured, "44", {}, executable);
    }
    for (const auto value : {"0x", "-0x", "0x1g", "0x1+2", "0x$(printf BAD)", "16#f", "3+4"}) {
        captured.active.file = file(value);
        rejects([&] { static_cast<void>(mcpp::expand_native_menu("printf '%%s' $((%f))", captured)); },
                "ordinary arithmetic values must remain integer data, not shell source");
    }
    captured.active.file = file("3+4");
    output("printf '%%s' $((%0f))", captured, "7");
}

void cases(const std::filesystem::path& root) {
    auto captured = context(root);
    const auto name = captured.active.file->name;
    output("printf '%%s\\0' \"$(case %f in (%f|other) printf '%%s' %f;; "
           "(*) printf miss;; esac)\" \"%f\" "
           "\"$( (case x in x) printf '%%s' %f;; esac); printf '%%s' \"%f\")\" "
           "\"$(time -p case x in x) printf '%%s' %f;; esac)\" "
           "\"$(function l20_case { case x in x) printf '%%s' %f;; esac; }; l20_case)\" "
           "\"$(coproc l20_case case x in x) printf '%%s' %f > case-coproc;; esac; "
           "wait; cat case-coproc)\" "
           "\"$(coproc printf '%%s' case; wait; printf '%%s' ordinary)\" "
           "\"$(if [[ literal == other ||\ncase == other ]]; then printf miss; else "
           "case x in x) printf '%%s' %f;; esac; fi)\"",
           captured, nul_values({name, name, name + name, name, name, name, "ordinary", name}));
    output("printf '%%s\\0' \"$(case ')' in ([\\)]|\\)|'case in esac)') "
           "printf '%%s' %f;; *) printf miss;; esac)\" "
           "\"$(printf '%%s' case in esac)\" "
           "\"$(printf '%%s' 'case in esac ) ;; ;& ;;&')\"",
           captured, nul_values({name, "caseinesac", "case in esac ) ;; ;& ;;&"}));

    const std::string nested =
        "printf '%%s\\0' \"$(case \"$(printf '%%s' %f)\"\nin\n"
        "# case ignored in ) ;; esac %{ignored}\n"
        "%f) case x in (x) printf '%%s' case in esac "
        "\"$(case inner in inner) printf '%%s' '%{Pick}';;esac)\";; "
        "esac;; *) printf '%%s' %b %e %{Other};; esac)\" \"%t\"";
    const std::string answer = "prompt ' \" ) ;; ;& ;;& esac\n$(printf literal)`literal`\\tail";
    const auto joined = captured.active.tagged[0].name + " " + captured.active.tagged[1].name + " ";
    captured.editor = true;
    captured.block_file = "/captured/block ) esac";
    captured.error_file = "/captured/error ;;";
    const auto requirements = mcpp::native_menu_requirements(nested, true);
    check(requirements.prompts == std::vector<std::string>{"Pick", "Other"} &&
              requirements.block_file && requirements.error_file,
          "case requirements must share the expansion scanner and ignore comments");
    output(nested, captured, nul_values({"caseinesac" + answer, joined}), {answer, "unused"});

    captured.active.file = file("3+4");
    output("printf '%%s\\0' \"$(case chosen in chosen) printf A ;& "
           "ignored) printf '%%s' $((%0f)) ;;& chosen) printf C ;; "
           "*) printf miss ;; esac)\" \"%f$(case empty in esac)%f\" "
           "\"$(> case; ca\\\nse x i\\\nn x) printf '%%s' %f;; es\\\nac)\" "
           "\"$(case x in x) cat <<'L20_CASE_LITERAL'\n%f\n"
           "esac ) ;; ;& ;;&\nL20_CASE_LITERAL\n;; esac)\"",
           captured, nul_values({"A7C", "3+43+4", "3+4", "3+4\nesac ) ;; ;& ;;&"}));

    for (const auto command : {
             "printf '%%s' \"$(case x in x) printf %f;;)\"",
             "printf '%%s' \"$(case x x) printf %f;; esac)\"",
             "printf '%%s' \"$(case x in (x|) printf %f;; esac)\"",
             "printf '%%s' \"$(case x in x) case y in y) printf %f;; esac)\"",
             "printf '%%s' \"$(case x in (x printf %f;; esac)\"",
             "printf '%%s' \"$(case x in x) printf 'esac';)\""}) {
        rejects([&] { static_cast<void>(mcpp::expand_native_menu(command, captured)); },
                "malformed case grammar must fail during expansion");
        rejects([&] { static_cast<void>(mcpp::native_menu_requirements(command, true)); },
                "requirements must reject the same malformed case grammar");
    }
    rejects([&] {
        static_cast<void>(mcpp::expand_native_menu(
            "printf '%%s' \"$(case x in x) printf '%%s' $((%f));; esac)\"", captured));
    }, "case scanning must not turn ordinary arithmetic macro data into an expression");
}

void extglobs(const std::filesystem::path& root) {
    const auto runner = root / "bash-extglob";
    {
        std::ofstream script(runner);
        script << "#!/bin/sh\nexec /bin/bash -O extglob \"$@\"\n";
        check(static_cast<bool>(script), "write the explicit extglob-enabled Bash runner");
    }
    std::filesystem::permissions(runner, std::filesystem::perms::owner_all);
    auto captured = context(root);
    captured.active.file = file("extglob ' \" @(?|*) ) | \\ café\n*.case");
    captured.editor = true;
    captured.block_file = "/captured/block @(|)";
    captured.error_file = "/captured/error !()";
    const auto name = captured.active.file->name;
    const std::string command =
        "printf '%%s\\0' \"$(case abbz-tail in "
        "(@(%f|[()]|literal\\)|'quoted|)'|?(@(a|x))*(b)+(z|q)!(blocked))) "
        "printf '%%s' %{Extglob bytes};; *) printf miss;; esac)\" "
        "\"$(case %f in @('%f'|\"%{Pattern bytes}\"|%b|%e)) "
        "printf '%%s' %f;; *) printf miss;; esac)\" "
        "\"%f\" '%f' %0var{L20_EXTGLOB_F1DEC888:raw words} '@(?|*)'";
    const auto requirements = mcpp::native_menu_requirements(command, true);
    check(requirements.prompts == std::vector<std::string>{"Extglob bytes", "Pattern bytes"} &&
              requirements.block_file && requirements.error_file,
          "extended patterns must share the requirements and expansion scanner");
    const std::string answer = "literal ' \" @(?|*) ) | \\ $(printf DATA)";
    output(command, captured,
           nul_values({answer, name, name, name, "raw", "words", "@(?|*)"}),
           {answer, answer}, runner.string());
    auto trusted = captured;
    trusted.active.file = file("@(x|y)");
    check(mcpp::expand_native_menu(
              "printf '%%s' \"$(case x in %0f) printf trusted;; esac)\"", trusted).command ==
              "printf '%s' \"$(case x in @(x|y)) printf trusted;; esac)\"",
          "raw opt-in must retain trusted pattern source without rescanning macro bytes");
    for (const auto malformed : {
             "printf '%%s' \"$(case x in @(?(@(x|y))) printf bad;; esac)\"",
             "printf '%%s' \"$(case x in @([)]|x)) printf bad;; esac)\""}) {
        rejects([&] { static_cast<void>(mcpp::expand_native_menu(malformed, captured)); },
                "extended patterns must not consume or invent the case-arm close");
        rejects([&] { static_cast<void>(mcpp::native_menu_requirements(malformed, true)); },
                "requirements must reject the same malformed extended patterns");
    }
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
        check(argc == 3, "usage: native-menu-complete words|contexts|ansi-heredocs|backticks|numbers|cases|extglobs|directories /review/cases/menu-complete-*");
        const auto root = std::filesystem::path(argv[2]).lexically_normal();
        check(root.string().starts_with("/review/cases/menu-complete-"), "invalid disposable menu fixture root");
        std::filesystem::create_directories(root);
        const std::string_view selected = argv[1];
        if (selected == "words") words(root);
        else if (selected == "contexts") contexts(root);
        else if (selected == "ansi-heredocs") ansi_heredocs(root);
        else if (selected == "backticks") backticks(root);
        else if (selected == "numbers") numbers(root);
        else if (selected == "cases") cases(root);
        else if (selected == "extglobs") extglobs(root);
        else if (selected == "directories") directories(root);
        else throw std::invalid_argument("unknown focused menu case");
        std::cout << "PASS native menu " << selected << '\n';
    } catch (const std::exception& error) {
        std::cerr << "FAIL native menu: " << error.what() << '\n';
        return 1;
    }
}
