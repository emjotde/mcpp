#include "mcpp/native_menu.hpp"
#include "mcpp/local_tools.hpp"
#include "mcpp/user_menu.hpp"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <utility>
#include <vector>

namespace {

void check(const bool value, const std::string_view message) {
    if (!value) throw std::runtime_error(std::string(message));
}

template <typename Function>
void rejects(Function&& function, const std::string_view message) {
    bool rejected = false;
    try {
        function();
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected, message);
}

mcpp::Entry file(std::string name, std::string path, const std::uint32_t mode = S_IFREG | 0644U) {
    mcpp::Entry result;
    result.name = std::move(name);
    result.path = std::move(path);
    result.kind = mcpp::EntryKind::file;
    result.mode = mode;
    return result;
}

mcpp::NativeMenuContext context() {
    mcpp::NativeMenuContext result;
    result.active.directory = "/review/cases/native-menus/active";
    result.active.file = file("odd ' $(printf SHOULD_NOT_RUN) `text`\n\"two\".txt",
                             "/review/cases/native-menus/active/selected");
    result.other.directory = "/review/cases/native-menus/other pane";
    result.other.file = file("other .tar", "/review/cases/native-menus/other pane/other .tar", S_IFREG | 0755U);
    result.active.tagged = {
        file("a space.txt", "/active/a space.txt"),
        file("b'\n.txt", "/active/b'\n.txt"),
    };
    result.other.tagged = {*result.other.file};
    result.menu_file = "/review/cases/native-menus/menu";
    result.executable = [](const std::string_view path) { return path == "/bin/sh"; };
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

void parser_and_conditions() {
    const auto entries = mcpp::parse_native_menu(
        "shell_patterns=1\n"
        "# Independently written menu fixture.\n"
        "  this is a comment continuation\n"
        "+= f *.txt | f *.tar & t d\n"
        "h Hidden by left-to-right conditions\n"
        "    :\n"
        "+ f *.txt & ! t d\n"
        "= t t & T x\n"
        "a Active entry\n"
        "    if true; then\n"
        "        printf '%%s' %f\n"
        "    fi\n"
        "+? x /bin/sh\n"
        "b Executable entry\n"
        "\t:\n"
        "c|Compatible entry|printf '%%s' %f\n"
        "invalid\n",
        "independent-menu"
    );
    check(entries.size() == 4U, "native and legacy entries parsed together");
    check(entries[1].command == "if true; then\n    printf '%%s' %f\nfi\n",
          "command blocks keep newlines and relative indentation");
    check(!entries.back().native, "legacy format keeps its original macro rules");
    const auto selected = mcpp::select_native_menu(entries, context());
    check(selected.entries.size() == 3U, "addition conditions hide unmatched entries");
    check(selected.entries.front().key == 'a' && selected.selected == 0U, "default condition selects a visible entry");
    check(selected.trace.size() == 1U, "debug conditions provide a trace");
    const auto regex = mcpp::parse_native_menu(
        "shell_patterns=0\n+ f \\.txt$\na Regex entry\n    :\n+ f ^missing$\nb Missing\n    :\n"
    );
    check(mcpp::select_native_menu(regex, context()).entries.size() == 1U, "regular expressions search the file name");
    auto archive = context();
    archive.active.local = false;
    archive.active.file->name = "member.txt";
    archive.active.file->path = "/member.txt";
    const auto provider_only = mcpp::parse_native_menu("+ t r & f *.txt\na Archive condition\n    :\n");
    check(mcpp::select_native_menu(provider_only, archive).entries.size() == 1U,
          "archive conditions use entry metadata, not host stat");
    rejects([] { static_cast<void>(mcpp::parse_native_menu("+ t z\na Invalid\n    :\n")); },
            "unknown condition types are explicit errors");
    rejects([] { static_cast<void>(mcpp::parse_native_menu("shell_patterns=0\n+ f [\na Invalid\n    :\n")); },
            "invalid regex is not treated as a false condition");
}

void macros_and_arguments() {
    const auto pane = context();
    const auto expansion = mcpp::expand_native_menu(
        "# Unmatched ' and %{not a prompt} remain shell comments.\n"
        "printf '%%s\\0' %f \"%f\" '%f' \"%0f\" pre%f/post \"$(printf '%%s' %f)\"",
        pane
    );
    const auto output = mcpp::capture_shell(expansion.command, std::filesystem::current_path());
    const auto& name = pane.active.file->name;
    check(output.status == 0 && output.output == nul_values(
        {name, name, name, name, "pre" + name + "/post", name}),
        "quoted, adjacent, and nested expansions preserve one exact argument");
    auto opposite = pane;
    opposite.other.file->name = "opposite ' $(printf SHOULD_NOT_RUN)\nname.dat";
    const auto opposite_command = mcpp::expand_native_menu("printf '%%s\\0' %p %P \"%P\" '%P'", opposite);
    const auto opposite_output = mcpp::capture_shell(opposite_command.command, std::filesystem::current_path());
    check(opposite_output.status == 0 && opposite_output.output == nul_values({
        name, opposite.other.file->name, opposite.other.file->name, opposite.other.file->name,
    }), "%P mirrors opposite-pane %p filenames, not full paths, with argument-safe quoting");
    const auto tagged = mcpp::expand_native_menu("printf '%%s\\0' %t %u %t %s %T %U %T %S", pane);
    const auto tags = mcpp::capture_shell(tagged.command, std::filesystem::current_path());
    check(tags.status == 0 && tags.output == nul_values({
        "a space.txt", "b'\n.txt", "a space.txt", "b'\n.txt", name,
        "other .tar", "other .tar", "other .tar",
    }), "tagged lists retain argument boundaries and untagging affects later macros");
    check(tagged.untag_active.size() == 2U && tagged.untag_other.size() == 1U,
          "untagging is returned for the UI to commit after successful execution");
    const auto requirements = mcpp::native_menu_requirements("printf '%%s' %{Argument} %var{MCPP_NATIVE_MENU_UNSET:fallback}", false);
    check(requirements.prompts == std::vector<std::string>{"Argument"}, "interactive prompt text is retained");
    const auto prompted = mcpp::expand_native_menu(
        "printf '%%s\\0' %{Argument} %var{MCPP_NATIVE_MENU_UNSET:fallback}", pane, {name}
    );
    const auto response = mcpp::capture_shell(prompted.command, std::filesystem::current_path());
    check(response.status == 0 && response.output == nul_values({name, "fallback"}),
          "prompt and environment values are not shell source");
    const auto legacy = mcpp::expand_user_menu_command("printf '%%s' %f %d %D", "/active/full file", "/active", "/other");
    check(legacy.find("'/active/full file'") != std::string::npos, "legacy %f remains a full path");
    for (const auto command : {
        "printf %z", "printf %0f", "printf \"%t\"", "printf `%f`",
        "cat <<EOF\n%f\nEOF", "printf $((%f))", "printf %"
    }) {
        rejects([&] { static_cast<void>(mcpp::expand_native_menu(command, pane)); },
                "unsafe or unsupported macro use is explicitly rejected");
    }
}

void editor_and_directives() {
    auto edit = context();
    edit.editor = true;
    edit.column = 3U;
    edit.syntax = "cpp";
    edit.active.file->name = "/review/cases/native-menus/edit.cpp";
    edit.block_file = "/review/cases/native-menus/block";
    edit.error_file = "/review/cases/native-menus/error";
    const auto requirements = mcpp::native_menu_requirements("printf '%%s' %b %k %e", true);
    check(requirements.block_file && requirements.error_file, "editor block and error files are requested explicitly");
    const auto expansion = mcpp::expand_native_menu("printf '%%s\\0' %f %n %b %k %e %c %i %y %m %x", edit);
    const auto result = mcpp::capture_shell(expansion.command, std::filesystem::current_path());
    check(result.status == 0 && result.output == nul_values({
        "/review/cases/native-menus/edit.cpp", "/review/cases/native-menus/edit",
        edit.block_file, edit.block_file, edit.error_file, "3", "   ", "cpp", edit.menu_file, "cpp",
    }), "editor macros use the edited file and cursor context");
    const auto view = mcpp::expand_native_menu("%view{hex,nroff} printf '%%s' %f", edit);
    check(view.action == mcpp::NativeMenuAction::view && view.view_hex == true && view.view_nroff == true,
          "documented viewer directives are returned to the UI");
    const auto unformatted = mcpp::expand_native_menu("%view{unform}", edit);
    check(unformatted.action == mcpp::NativeMenuAction::view && unformatted.view_nroff == false,
          "extension unform spelling disables nroff formatting");
    const auto cd = mcpp::expand_native_menu("%cd %d/child", edit);
    check(cd.action == mcpp::NativeMenuAction::change_directory &&
          cd.command == "'/review/cases/native-menus/active'/child",
          "cd is an internal action, not a shell command");
    auto archive = edit;
    archive.active.local = false;
    rejects([&] { static_cast<void>(mcpp::expand_native_menu("printf '%%s' %f", archive)); },
            "external commands cannot use archive-member paths as local files");
    check(mcpp::expand_native_menu("%view", archive).action == mcpp::NativeMenuAction::view,
          "internal viewing does not require an external archive command");
}

}  // namespace

int main() {
    try {
        parser_and_conditions();
        macros_and_arguments();
        editor_and_directives();
        std::cout << "PASS native menu parsing, conditions, arguments, and editor context\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
