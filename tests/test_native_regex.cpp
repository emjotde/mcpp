#include "mcpp/extension.hpp"
#include "mcpp/file_highlight.hpp"
#include "mcpp/native_menu.hpp"
#include "mcpp/native_regex.hpp"
#include "mcpp/syntax.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {

namespace fs = std::filesystem;

void check(const bool value, const std::string_view message) {
    if (!value) throw std::runtime_error(std::string(message));
}

void write(const fs::path& path, const std::string_view value) {
    std::ofstream output(path, std::ios::binary);
    output.write(value.data(), static_cast<std::streamsize>(value.size()));
    output.close();
    if (!output) throw std::runtime_error("cannot write fixture: " + path.string());
}

template<class Function>
void rejects(Function function, const std::string_view message) {
    try {
        function();
    } catch (const std::exception& error) {
        check(std::string_view(error.what()).find(message) != std::string_view::npos, error.what());
        return;
    }
    throw std::runtime_error("expected an explicit regular-expression error");
}

void core_case() {
    const std::string pattern = "(?<=pre:)\\p{L}++[.]txt$";
    const std::string subject = "pre:" "\xc5\xbc" ".txt";
    const mcpp::NativeRegex expression(pattern);
    const auto match = expression.find(subject);
    check(match && match->offset == 4U && match->length == 6U,
          "lookbehind, Unicode properties and possessive quantifiers return byte ranges");
    check(mcpp::NativeRegex("^\\x{17c}$", true).search("\xc5\xbb"),
          "case folding handles non-ASCII letters");
    check(!mcpp::NativeRegex("(?>a|ab)c").search("abc"),
          "atomic groups retain PCRE semantics");
    const auto binary = mcpp::NativeRegex("b\\x00c").find(std::string("ab\0cd", 5));
    check(binary && binary->offset == 1U && binary->length == 3U,
          "explicit subject lengths retain NUL bytes");
    const auto invalid = mcpp::NativeRegex("[.]txt$").find(
        std::string(1, static_cast<char>(0xff)) + ".txt");
    check(invalid && invalid->offset == 1U, "invalid filename bytes do not reject a valid suffix");
    check(mcpp::NativeRegex("").search({}), "empty patterns and subjects remain valid");
    rejects([] { static_cast<void>(mcpp::NativeRegex("(?<")); }, "at byte");
    rejects([&] { static_cast<void>(expression.find(subject, subject.size() + 1U)); }, "exceeds subject");
    rejects([] {
        static_cast<void>(mcpp::NativeRegex("(*NO_AUTO_POSSESS)(*NO_START_OPT)(a+)+$")
            .search(std::string(256, 'a') + "!"));
    }, "matching failed");

    mcpp::NativeMenuContext context;
    context.active.file.emplace();
    context.active.file->kind = mcpp::EntryKind::file;
    context.active.file->name = subject;
    const auto menu = mcpp::parse_native_menu(
        "+ f " + pattern + "\np PCRE menu\n  printf original\n", "original-menu", false);
    check(mcpp::select_native_menu(menu, context).entries.size() == 1U,
          "non-shell menu conditions use the same PCRE engine");
}

void extension_case() {
    const auto rules = mcpp::ExtensionRules::parse(
        "[mc.ext.ini]\nVersion=4.0\n"
        "[Original]\nRegex=(?<=pre-)\\\\p{L}++[.]txt$\nRegexIgnoreCase=true\nOpen=original\n"
        "[Default]\nOpen=default\n", "original.ext.ini");
    mcpp::Entry entry;
    entry.kind = mcpp::EntryKind::file;
    entry.name = "pre-" "\xc5\xbb" ".TXT";
    const auto matched = rules.match(entry, mcpp::ExtensionAction::open, false);
    check(matched && matched->command == "original",
          "native extension PCRE and Unicode case folding match virtual filenames");
    entry.name = "other.TXT";
    check(rules.match(entry, mcpp::ExtensionAction::open, true)->command == "default",
          "unmatched expressions retain normal fallback ordering");
    rejects([] {
        static_cast<void>(mcpp::ExtensionRules::parse(
            "[mc.ext.ini]\nVersion=4.0\n[Broken]\nRegex=(?<\nOpen=bad\n", "broken.ext.ini"));
    }, "broken.ext.ini:4");
    const auto legacy = mcpp::ExtensionRules::parse("*.TXT=legacy %f\n");
    check(legacy.match(entry, mcpp::ExtensionAction::open, true)->legacy,
          "legacy shell-pattern actions are unchanged");
}

bool token_is_green(const std::vector<std::vector<mcpp::SyntaxSpan>>& spans) {
    if (spans.empty()) return false;
    for (const auto& span : spans.front()) {
        if (span.begin == 0U && span.end >= 5U && span.color && span.color->foreground == 10) return true;
    }
    return false;
}

void syntax_case(const fs::path& root) {
    const auto registry = root / "Syntax";
    fs::create_directories(root / "syntax.d");
    write(registry, R"(file (?i)(?<=pre-)\\p{L}++[.]txt$ PCRE\sOriginal ^#!(?=original)
context default red black
 keyword whole token brightgreen
)");
    mcpp::SyntaxHighlighter highlighter;
    highlighter.load_registry(registry, root / "syntax.d");
    const std::string filename = "pre-" "\xc5\xbb" ".TXT";
    check(token_is_green(highlighter.highlight(filename, {"token"})),
          "filename registry rules accept PCRE and Unicode");
    const auto first_line = highlighter.highlight("unmatched", {"#!original", "token"});
    check(first_line.size() == 2U && token_is_green({first_line[1]}),
          "first-line registry rules use PCRE lookahead");
    write(registry, "file (?< Broken\ncontext default red black\n");
    rejects([&] { highlighter.load_registry(registry, root / "syntax.d"); }, "at byte");
    check(token_is_green(highlighter.highlight(filename, {"token"})),
          "failed registry replacement retains accepted rules");
}

void highlight_case(const fs::path& root) {
    const auto resource = root / "filehighlight.ini";
    write(resource, R"([original]
regexp=(?i)(?<=pre-)\\p{L}++[.]txt$
)");
    const auto rules = mcpp::FileHighlightRules::load_file(resource);
    mcpp::Skin skin;
    skin.resource = root / "original.ini";
    skin.colors["filehighlight"]["original"] = {ftxui::Color::Red, ftxui::Color::Blue};
    mcpp::Entry entry;
    entry.kind = mcpp::EntryKind::file;
    entry.name = "pre-" "\xc5\xbb" ".TXT";
    const auto color = rules.color_for(entry, skin);
    check(color && color->foreground == ftxui::Color::Red && color->background == ftxui::Color::Blue,
          "native filename highlighting applies the PCRE-selected skin group");
    entry.name = "other.TXT";
    check(!rules.color_for(entry, skin), "unmatched names preserve builtin type colors");
    write(resource, "[original]\nregexp=(?<\n");
    rejects([&] { static_cast<void>(mcpp::FileHighlightRules::load_file(resource)); }, "invalid filename regexp");
    entry.name = "pre-" "\xc5\xbb" ".TXT";
    check(rules.color_for(entry, skin).has_value(), "invalid replacement does not invalidate accepted rules");
    skin.resource.clear();
    check(!rules.color_for(entry, skin), "builtin palettes retain their original type colors");
}

}  // namespace

int main(const int argc, char** argv) {
    try {
        if (argc != 3 || ::geteuid() != 1001U || !fs::is_regular_file("/.dockerenv") ||
            !fs::is_regular_file("/review/.operation-regressions-disposable")) {
            throw std::runtime_error("native regex checks require the owned UID1001 Docker fixture");
        }
        const auto base = fs::absolute(argv[2]).lexically_normal();
        check(base.string().starts_with("/review/cases/native-regex-"), "invalid regex fixture directory");
        fs::create_directories(base);
        const auto root = base / ("run-" + std::to_string(::getpid()));
        check(fs::create_directory(root), "regex fixture directory already exists");
        const std::string_view selected(argv[1]);
        if (selected == "core") core_case();
        else if (selected == "extensions") extension_case();
        else if (selected == "syntax") syntax_case(root);
        else if (selected == "highlight") highlight_case(root);
        else throw std::runtime_error("unknown regex check selector");
        std::cout << selected << ": passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
