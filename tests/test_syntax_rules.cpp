#include "mcpp/syntax.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void check(const bool condition, const std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

void write(const std::filesystem::path& path, const std::string_view text) {
    std::ofstream output(path);
    output << text;
    if (!output) throw std::runtime_error("cannot write fixture: " + path.string());
}

const mcpp::SyntaxColor& color_at(const std::vector<mcpp::SyntaxSpan>& spans, const std::size_t position) {
    for (const auto& span : spans) {
        if (position >= span.begin && position < span.end && span.color) return *span.color;
    }
    throw std::runtime_error("missing external syntax span");
}

void test_spelling_contexts(const std::filesystem::path& root) {
    const auto registry = root / "Syntax";
    write(registry, R"(file .*\\.l22$ Spelling\sL22
context default red black
 keyword whole code brightgreen
context exclusive [ ] red black
 spellcheck
 keyword whole typo brightcyan
context < > red black
 spellcheck
context { } red black
file .*\\.flag$ Default\sSpelling
context default red black
 spellcheck
context exclusive [ ] red black
file .*\\.none$ No\sSpelling
context default red black
)");
    mcpp::SyntaxHighlighter highlighter;
    highlighter.load_registry(registry, root / "syntax.d");
    const std::vector<std::string> lines{"é code [typo", "tag] gone <text> {raw}"};
    const auto text = lines[0] + "\n" + lines[1];
    const auto eligible = highlighter.spelling_ranges("sample.l22", lines);
    check(eligible && *eligible == std::vector<mcpp::SyntaxSpellingRange>{
        {9U, text.find(']')}, {text.find('<'), text.find('>') + 1U}
    }, "context spelling ranges use original UTF-8 byte offsets, including newlines");
    check(eligible->front().contains(text.find("tag"), 3U) &&
          !eligible->front().contains(text.find('['), 1U) &&
          !eligible->front().contains(text.find("code"), 4U),
          "exclusive delimiters and unmarked code stay outside eligible ranges");
    const auto spans = highlighter.highlight("sample.l22", lines);
    check(color_at(spans[0], 0U).foreground == color_at(spans[1], 0U).foreground &&
          color_at(spans[0], 9U).foreground == 14,
          "spelling eligibility is independent of context and keyword colors");
    const auto defaults = highlighter.spelling_ranges("sample.flag", {"a[raw]z"});
    check(defaults && *defaults == std::vector<mcpp::SyntaxSpellingRange>{{0U, 2U}, {5U, 7U}},
          "spellcheck belongs to its own context, including default-context exclusive delimiters");
    const auto disabled = highlighter.spelling_ranges("sample.none", {"word"});
    check(disabled && disabled->empty(), "an external definition without marked contexts checks no text");
    check(!highlighter.spelling_ranges("sample.l22", lines, "cpp") &&
          !highlighter.spelling_ranges("sample.l22", lines, "plain") &&
          !highlighter.spelling_ranges("sample.txt", lines),
          "builtin, plain and unmatched auto syntax preserve unrestricted spelling");
    std::cout << "PASS context spelling eligibility, byte offsets and builtin compatibility\n";
}

}  // namespace

int main(const int argc, const char* const* argv) {
    try {
        if (argc != 2 && !(argc == 3 && std::string_view(argv[2]) == "spellcheck")) {
            throw std::runtime_error("usage: syntax-model owned-fixture-directory [spellcheck]");
        }
        const auto root = std::filesystem::path(argv[1]);
        const auto definitions = root / "syntax.d";
        std::filesystem::create_directories(definitions);
        if (argc == 3) {
            test_spelling_contexts(root);
            return 0;
        }
        const auto registry = root / "Syntax";
        write(registry, R"(file .*\\.l22$ Original\sL22 ^#!original
include original.syntax
)");
        const std::string original = R"(caseinsensitive
define alert brightred black bold+underline
context default lightgray black
 keyword whole ready alert
 keyword whole go brightmagenta
 keyword x*y yellow
 keyword q+z brightcyan
 keyword \{0123456789\}\[0123456789\] rgb123 gray5
context exclusive [ ] blue
 keyword whole go brightgreen
context ! ! red
 keyword \\\n brightcyan
)";
        write(definitions / "original.syntax", original);
        mcpp::SyntaxHighlighter highlighter;
        highlighter.load_registry(registry, definitions);
        const std::vector<std::string> lines{
            "READY unready xone two y qonez qone two z 123", "[go] go GONE", "!a\\", "b!"
        };
        const auto spans = highlighter.highlight("sample.l22", lines);
        const auto& ready = color_at(spans[0], 0U);
        check(ready.foreground == 9 && ready.background == 0 &&
              (ready.attributes & mcpp::SyntaxColor::bold) != 0U &&
              (ready.attributes & mcpp::SyntaxColor::underline) != 0U,
              "case-insensitive whole keyword and native colors/attributes");
        check(color_at(spans[0], 6U).foreground == 7, "whole keyword excludes an embedded word");
        check(color_at(spans[0], lines[0].find("xone")).foreground == 11, "star pattern includes spaces");
        check(color_at(spans[0], lines[0].find("qonez")).foreground == 14 &&
              color_at(spans[0], lines[0].find("qone two")).foreground == 7,
              "plus pattern cannot include spaces");
        const auto digits = lines[0].find("123");
        check(color_at(spans[0], digits).foreground == 67 &&
              color_at(spans[0], digits).background == 237, "native repeated character sets and palette colors");
        check(color_at(spans[1], 0U).foreground == 7 &&
              color_at(spans[1], 1U).foreground == 10 &&
              color_at(spans[1], 1U).background == mcpp::SyntaxColor::base &&
              color_at(spans[1], 3U).foreground == 7 &&
              color_at(spans[1], 5U).foreground == 13,
              "exclusive context excludes delimiters and owns its keywords");
        check(color_at(spans[2], 2U).foreground == 14 &&
              color_at(spans[3], 0U).foreground == 1, "escaped newline keyword preserves multiline context");
        check(highlighter.language_name("sample.l22", lines) == "Original L22" &&
              highlighter.language_name("other.bin", {"#!original"}) == "Original L22" &&
              highlighter.language_name("other.bin", {}, "external:Original L22") == "Original L22",
              "registry filename, first-line and explicit selection are real");
        check(highlighter.definitions().size() == 1U &&
              highlighter.definitions().front().id == "external:Original L22" &&
              highlighter.language_name("a.py", {}) == "Python Program" &&
              highlighter.highlight("a.py", {"return 1"}, "plain")[0].empty(),
              "external chooser IDs preserve builtins, auto and plain");
        std::cout << "PASS external syntax patterns and registry selection\n";

        const auto rejects = [&](const std::string_view content, const std::string_view expected) {
            write(definitions / "original.syntax", content);
            bool rejected = false;
            try {
                highlighter.load_registry(registry, definitions);
            } catch (const std::exception& error) {
                rejected = std::string_view(error.what()).find(expected) != std::string_view::npos;
            }
            check(rejected, "invalid syntax resource reports its actual error");
            check(color_at(highlighter.highlight("sample.l22", lines)[0], 0U).foreground == 9,
                  "failed resource reload preserves last good highlighting");
        };
        rejects("include missing.syntax\n", "cannot include");
        rejects("include original.syntax\n", "cyclic syntax include");
        rejects("context default\nunsupported_rule red\n", "unsupported syntax directive");
        write(definitions / "original.syntax", original);
        check(highlighter.highlight("sample.l22", lines, "cpp")[0][0].style != mcpp::SyntaxStyle::normal,
              "explicit builtin remains available after a failed external reload");
        bool missing = false;
        try {
            static_cast<void>(highlighter.highlight("a.cpp", lines, "external:Unavailable"));
        } catch (const std::exception& error) {
            missing = std::string_view(error.what()).find("unavailable") != std::string_view::npos;
        }
        check(missing, "missing selected definition never becomes an unrelated builtin");
        std::cout << "PASS invalid-resource preservation and missing-selection diagnostics\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
