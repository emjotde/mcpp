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

void test_native_format(const std::filesystem::path& root) {
    const auto registry = root / "Syntax";
    const auto definition = root / "syntax.d" / "original-format.syntax";
    write(registry, "file . Original\\sNative\\sFormat\ninclude original-format.syntax\n");
    write(definition, R"(define accent brightgreen black
context default red black
 keyword whole whole\[\] accent
 keyword plain
context [ ] brightmagenta black
 keyword reset
)");
    mcpp::SyntaxHighlighter highlighter;
    highlighter.load_registry(registry, root / "syntax.d");
    const std::vector<std::string> lines{"whole plain [reset] wholeword"};
    const auto spans = highlighter.highlight("original.fmt", lines);
    for (std::size_t byte = 0; byte < lines[0].size(); ++byte) {
        const auto expected = byte < 5U ? 10 : (byte >= 12U && byte < 19U ? 13 : 1);
        check(color_at(spans[0], byte).foreground == expected &&
              color_at(spans[0], byte).background == 0,
              "native empty repeated sets and colorless keywords inherit their own context");
    }
    write(definition, "context default red black\nunsupported_rule\n");
    bool rejected = false;
    try {
        highlighter.load_registry(registry, root / "syntax.d");
    } catch (const std::exception& error) {
        rejected = std::string_view(error.what()).find("unsupported syntax directive") !=
                   std::string_view::npos;
    }
    const auto preserved = highlighter.highlight("original.fmt", lines);
    check(rejected && preserved.size() == spans.size() &&
          preserved[0].size() == spans[0].size(),
          "unknown directives still fail without replacing native syntax rules");
    for (std::size_t index = 0; index < spans[0].size(); ++index) {
        check(preserved[0][index].begin == spans[0][index].begin &&
              preserved[0][index].end == spans[0][index].end &&
              preserved[0][index].color == spans[0][index].color,
              "failed native-format reload preserves byte spans and colors");
    }
    std::cout << "PASS MC-native empty repeated sets, omitted keyword colors and transactional reload\n";
}

void test_native_consumption(const std::filesystem::path& root) {
    const auto registry = root / "Syntax";
    write(registry, R"(file . Original\sConsuming\sPatterns
context default lightgray black
 spellcheck
 keyword whole sentinel yellow
 keyword \[ab\] brightgreen
 keyword * brightred
)");
    mcpp::SyntaxHighlighter highlighter;
    highlighter.load_registry(registry, root / "syntax.d");
    const std::vector<std::string> lines{"sentinel", "abba! aba", ""};
    const auto spans = highlighter.highlight("original.consume", lines);
    check(spans.size() == lines.size() && spans.back().empty(),
          "repetition-only keywords preserve empty rows");
    for (std::size_t row = 0; row < 2U; ++row) {
        for (std::size_t byte = 0; byte < lines[row].size(); ++byte) {
            check(color_at(spans[row], byte).foreground == (row == 0U ? 11 : 10),
                  "repetition-only keywords match cached native colors at every byte");
        }
        for (const auto& span : spans[row]) {
            check(span.begin < span.end && span.end <= lines[row].size(),
                  "empty keyword matches always make bounded byte progress");
        }
    }
    const auto spelling = highlighter.spelling_ranges("original.consume", lines);
    check(spelling && *spelling == std::vector<mcpp::SyntaxSpellingRange>{{0U, 19U}},
          "empty keyword matches preserve original spelling byte offsets and newlines");
    std::cout << "PASS cached MC-native repetition-only colors, byte progress and spelling offsets\n";
}

void test_native_bytes(const std::filesystem::path& root) {
    const auto registry = root / "Syntax";
    write(registry, R"(file . Original\sNative\sBytes
caseinsensitive
context default lightgray black
 keyword ascii brightred
 keyword café brightgreen
 keyword \{é\}X yellow
 keyword R\[éλ\]Y brightcyan
context <é> </é> brightmagenta black
 spellcheck
)");
    mcpp::SyntaxHighlighter highlighter;
    highlighter.load_registry(registry, root / "syntax.d");
    const std::vector<std::string> lines{
        "ASCII café CAFé CAFÉ éX ΩX RéλY RûΩY", "<é>inside</é> <É>outside</É>"
    };
    const auto spans = highlighter.highlight("sample.l22", lines);
    std::vector<int> expected(lines[0].size(), 7);
    const auto paint = [&](const std::string_view word, const std::size_t skip, const int color) {
        const auto start = lines[0].find(word);
        check(start != std::string::npos, "expected syntax fixture word exists");
        for (auto byte = start + skip; byte < start + word.size(); ++byte) expected[byte] = color;
    };
    paint("ASCII", 0U, 9);
    paint("café", 0U, 10);
    paint("CAFé", 0U, 10);
    paint("éX", 1U, 11);
    paint("ΩX", 1U, 11);
    paint("RéλY", 0U, 14);
    paint("RûΩY", 0U, 14);
    for (std::size_t byte = 0; byte < expected.size(); ++byte) {
        check(color_at(spans[0], byte).foreground == expected[byte],
              "native folding and character sets preserve every original UTF-8 byte offset");
    }
    const std::string context = "<é>inside</é>";
    for (std::size_t byte = 0; byte < lines[1].size(); ++byte) {
        check(color_at(spans[1], byte).foreground == (byte < context.size() ? 13 : 7),
              "native context delimiters fold ASCII only, not UTF-8 codepoints");
    }
    const auto context_begin = lines[0].size() + 1U;
    const auto spelling = highlighter.spelling_ranges("sample.l22", lines);
    check(spelling && *spelling == std::vector<mcpp::SyntaxSpellingRange>{
        {context_begin, context_begin + context.size()}
    }, "native byte contexts preserve color-independent spelling eligibility");
    write(registry, "file . Broken\ncontext default\nunsupported_rule\n");
    bool rejected = false;
    try {
        highlighter.load_registry(registry, root / "syntax.d");
    } catch (const std::exception& error) {
        rejected = std::string_view(error.what()).find("unsupported syntax directive") !=
                   std::string_view::npos;
    }
    check(rejected && highlighter.spelling_ranges("sample.l22", lines) == spelling,
          "failed native reload preserves spelling eligibility");
    const auto preserved = highlighter.highlight("sample.l22", lines);
    check(preserved.size() == spans.size(), "failed native reload preserves highlighted rows");
    for (std::size_t row = 0; row < lines.size(); ++row) {
        for (std::size_t byte = 0; byte < lines[row].size(); ++byte) {
            check(color_at(preserved[row], byte) == color_at(spans[row], byte),
                  "failed native reload preserves every byte color");
        }
    }
    const auto builtin = highlighter.highlight("a.cpp", {"return 1"}, "cpp");
    check(!builtin[0].empty() && builtin[0][0].style == mcpp::SyntaxStyle::keyword &&
          !builtin[0][0].color &&
          !highlighter.spelling_ranges("a.cpp", {"return 1"}, "cpp") &&
          highlighter.highlight("sample.l22", lines, "plain")[0].empty(),
          "native byte rules do not change builtin or plain syntax");
    std::cout << "PASS MC-equivalent ASCII folding, byte sets, spelling, reload and builtin rules\n";
}

void test_native_contexts(const std::filesystem::path& root) {
    struct Paint {
        std::size_t begin;
        std::size_t end;
        int color;
    };
    struct Fixture {
        std::string_view name;
        std::string_view rules;
        std::vector<std::string> lines;
        std::vector<Paint> paint;
        std::vector<mcpp::SyntaxSpellingRange> spelling;
    };
    const std::vector<Fixture> fixtures{
        {"left", R"(context \[a\] ] brightmagenta black
 spellcheck
 keyword whole sentinel yellow
)", {"sentinel", "qaa]z a] tail", "b aa] ] a]", ""}, {}, {}},
        {"right", R"(context [ \[b\] brightmagenta black
 spellcheck
 keyword whole sentinel yellow
)", {"sentinel", "q[bbz a[b] tail", "q[]z q[b]z", ""},
            {{10U, 36U, 13}}, {{10U, 36U}}},
        {"both", R"(context \[a\] \[b\] brightmagenta black
 spellcheck
 keyword whole sentinel yellow
)", {"sentinel", "qaa]z abb tail", "b aa] ] a]", ""}, {}, {}},
        {"neighbors", R"(context \[a\]L R\[b\] brightmagenta black
 spellcheck
 keyword whole sentinel yellow
context M\[a\] \[b\]N brightgreen black
 spellcheck
 keyword whole sentinel yellow
)", {"sentinel", "qLRz aaLRbbz LaRz", "qMNz MaaNbbz MbN", ""},
            {{28U, 44U, 10}}, {{28U, 44U}}},
        {"literal-positive", R"(context L\[a\] R\[b\] brightmagenta black
 spellcheck
)", {"sentinel", "qLRz LaaRbbz LwordRz", ""},
            {{10U, 12U, 13}, {14U, 20U, 13}, {22U, 28U, 13}},
            {{10U, 12U}, {14U, 20U}, {22U, 28U}}},
        {"other-prefixes", R"(context \{LQ\} R brightmagenta black
context + R brightcyan black
context < > brightgreen black
 spellcheck
)", {"sentinel", "qLRz <yes> tail", ""}, {{14U, 19U, 10}}, {{14U, 19U}}},
        {"empty-markers", R"(context \[\] \[\] brightmagenta black
 spellcheck
context [ \[\] brightgreen black
 spellcheck
)", {"sentinel", "qLRz [teh] outside", ""}, {{14U, 28U, 10}}, {{14U, 28U}}},
        {"empty-neighbors", R"(context \[\]L R brightmagenta black
 spellcheck
context L\[\] R\[\] brightgreen black
 spellcheck
)", {"sentinel", "qLRz LR tail", ""},
            {{10U, 12U, 10}, {14U, 16U, 10}}, {{10U, 12U}, {14U, 16U}}},
    };
    const auto registry = root / "Syntax";
    mcpp::SyntaxHighlighter highlighter;
    std::vector<std::vector<mcpp::SyntaxSpan>> last_spans;
    const auto same_spans = [](const auto& left, const auto& right) {
        if (left.size() != right.size()) return false;
        for (std::size_t row = 0; row < left.size(); ++row) {
            if (left[row].size() != right[row].size()) return false;
            for (std::size_t index = 0; index < left[row].size(); ++index) {
                const auto& a = left[row][index];
                const auto& b = right[row][index];
                if (a.begin != b.begin || a.end != b.end ||
                    a.style != b.style || a.color != b.color) return false;
            }
        }
        return true;
    };
    for (const auto& fixture : fixtures) {
        write(registry, "file . Original\\sContext\\s" + std::string(fixture.name) +
            "\ncontext default lightgray black\n keyword whole sentinel yellow\n" +
            std::string(fixture.rules));
        highlighter.load_registry(registry, root / "syntax.d");
        std::size_t size = fixture.lines.size() - 1U;
        for (const auto& line : fixture.lines) size += line.size();
        std::vector<int> expected(size, 7);
        for (std::size_t byte = 0; byte < 8U; ++byte) expected[byte] = 11;
        for (const auto& paint : fixture.paint) {
            check(paint.begin < paint.end && paint.end <= expected.size(),
                  "native context fixture has a bounded positive paint range");
            for (auto byte = paint.begin; byte < paint.end; ++byte) expected[byte] = paint.color;
        }
        const auto spans = highlighter.highlight("original.ctx", fixture.lines);
        check(spans.size() == fixture.lines.size(), "native contexts preserve every row");
        std::size_t offset = 0;
        for (std::size_t row = 0; row < fixture.lines.size(); ++row) {
            std::size_t next = 0;
            for (const auto& span : spans[row]) {
                check(span.begin == next && span.begin < span.end &&
                      span.end <= fixture.lines[row].size(),
                      "context scanning emits positive, contiguous, bounded visible spans");
                next = span.end;
            }
            check(next == fixture.lines[row].size(), "native context spans cover every visible byte");
            for (std::size_t byte = 0; byte < fixture.lines[row].size(); ++byte) {
                const auto& color = color_at(spans[row], byte);
                check(color.foreground == expected[offset + byte] && color.background == 0,
                      "cached native context colors differ for " + std::string(fixture.name));
            }
            offset += fixture.lines[row].size() + 1U;
        }
        // These ranges preserve mcpp's context filter; native manual Spell check is a separate control.
        const auto spelling = highlighter.spelling_ranges("original.ctx", fixture.lines);
        check(spelling && *spelling == fixture.spelling,
              "context classification preserves mcpp spelling ranges at original byte offsets");
        for (const auto& range : *spelling) {
            check(range.begin < range.end && range.end <= size,
                  "ignored context transitions never emit empty or out-of-bounds spelling ranges");
        }
        const auto outside = highlighter.highlight("original.ctx", {"outside"});
        const auto outside_spelling = highlighter.spelling_ranges("original.ctx", {"outside"});
        check(color_at(outside.front(), 0U).foreground == 7 &&
              outside_spelling && outside_spelling->empty(),
              "active contexts do not leak between separate scans");
        check(same_spans(highlighter.highlight("original.ctx", fixture.lines), spans) &&
              highlighter.spelling_ranges("original.ctx", fixture.lines) == spelling,
              "repeated context scans do not retain a stale transition cache");
        last_spans = spans;
    }
    write(registry, "file . Broken\ncontext default lightgray black\nunsupported_rule\n");
    bool rejected = false;
    try {
        highlighter.load_registry(registry, root / "syntax.d");
    } catch (const std::exception& error) {
        rejected = std::string_view(error.what()).find("unsupported syntax directive") !=
                   std::string_view::npos;
    }
    check(rejected &&
          same_spans(highlighter.highlight("original.ctx", fixtures.back().lines), last_spans) &&
          highlighter.spelling_ranges("original.ctx", fixtures.back().lines) ==
              std::optional(fixtures.back().spelling),
          "unknown directives preserve the prior context registry, colors and spelling ranges");
    std::cout << "PASS cached native context classification, positive progress and mcpp spelling ranges\n";
}

void test_native_guards(const std::filesystem::path& root) {
    struct Paint {
        std::size_t first_row;
        std::size_t first_byte;
        std::size_t last_row;
        std::size_t last_byte;
        int color;
        bool spelling{false};
    };
    struct Fixture {
        std::string_view name;
        std::string_view syntax;
        std::vector<std::string> lines;
        std::vector<Paint> paint;
        bool default_spelling{false};
    };
    const std::vector<Fixture> fixtures{
        {"empty-keyword", R"(file . Original\sEmpty\sKeyword
context default lightgray black
 spellcheck
 keyword whole sentinel yellow
 keyword \[\] brightmagenta
context < > brightgreen black
)", {"za <tag> sentinel", "éa <tag> sentinel", "sentinel <tag>", ""},
            {{0U, 1U, 2U, 0U, 13}, {2U, 0U, 2U, 8U, 11}, {2U, 8U, 3U, 0U, 13}}, true},
        {"empty-options", R"(file . Original\sEmpty\sOptions
context default lightgray black
 keyword whole sentinel yellow
 keyword whole \[\] brightmagenta
 keyword linestart \[\] brightcyan
context < > brightgreen black
 spellcheck
)", {"za  <tag> sentinel", "sentinel  <tag>", "z <tag>", "!", ""},
            {{0U, 3U, 1U, 0U, 13}, {1U, 0U, 1U, 8U, 11}, {1U, 9U, 2U, 0U, 13},
             {2U, 0U, 3U, 0U, 14}, {3U, 0U, 4U, 0U, 13}}},
        {"edge-bytes", R"(file . Original\sWildcard\sBytes
context default lightgray black
 keyword whole sentinel yellow
context *A B brightmagenta black
 spellcheck
context C* D brightgreen black
 spellcheck
context E *F brightcyan black
 spellcheck
context G H* brightred black
 spellcheck
context < > yellow black
)", {"sentinel", "q AoneB \x01" "AtwoB tail", "CwordD <ok> tail",
            std::string("Craw\0 Dtail", 11U), "EinsideF \x01" "finishF tail",
            "GinsideH tail", std::string("before Hraw\0 after", 18U), "outside", ""},
            {{0U, 0U, 0U, 8U, 11}, {1U, 8U, 1U, 14U, 13, true},
             {2U, 7U, 2U, 11U, 11}, {3U, 0U, 3U, 7U, 10, true},
             {4U, 0U, 4U, 17U, 14, true}, {5U, 0U, 6U, 12U, 9, true}}},
        {"edge-whole", R"(file . Original\sWildcard\sBoundaries
context default lightgray black
 keyword whole sentinel yellow
context wholeright I* J brightmagenta black
 spellcheck
context wholeright K L* brightcyan black
 spellcheck
)", {"sentinel", "Iword J  outside", "Iword  J  after", "K body L outside",
            "K body L  after", "outside", "Iwrap", " after", "J  outside", ""},
            {{0U, 0U, 0U, 8U, 11}, {2U, 0U, 2U, 8U, 13, true},
             {3U, 0U, 4U, 9U, 14, true}, {6U, 0U, 8U, 1U, 13, true}}},
    };
    const auto same_spans = [](const auto& left, const auto& right) {
        if (left.size() != right.size()) return false;
        for (std::size_t row = 0; row < left.size(); ++row) {
            if (left[row].size() != right[row].size()) return false;
            for (std::size_t index = 0; index < left[row].size(); ++index) {
                const auto& a = left[row][index];
                const auto& b = right[row][index];
                if (a.begin != b.begin || a.end != b.end ||
                    a.style != b.style || a.color != b.color) return false;
            }
        }
        return true;
    };
    const auto registry = root / "Syntax";
    mcpp::SyntaxHighlighter highlighter;
    std::vector<std::vector<mcpp::SyntaxSpan>> last_spans;
    std::vector<mcpp::SyntaxSpellingRange> last_spelling;
    for (const auto& fixture : fixtures) {
        write(registry, fixture.syntax);
        highlighter.load_registry(registry, root / "syntax.d");
        std::vector<std::size_t> offsets;
        std::string text;
        for (const auto& line : fixture.lines) {
            if (!offsets.empty()) text += '\n';
            offsets.push_back(text.size());
            text += line;
        }
        std::vector<int> expected(text.size(), 7);
        std::vector<mcpp::SyntaxSpellingRange> eligible;
        if (fixture.default_spelling) eligible.push_back({0U, text.size()});
        for (const auto& paint : fixture.paint) {
            const auto begin = offsets.at(paint.first_row) + paint.first_byte;
            const auto end = offsets.at(paint.last_row) + paint.last_byte;
            check(begin < end && end <= text.size(), "guard fixture has a positive byte range");
            for (auto byte = begin; byte < end; ++byte) expected[byte] = paint.color;
            if (paint.spelling) eligible.push_back({begin, end});
        }
        const auto spans = highlighter.highlight("original.guard", fixture.lines);
        check(spans.size() == fixture.lines.size(), "guard syntax preserves all rows");
        for (std::size_t row = 0; row < fixture.lines.size(); ++row) {
            std::size_t next = 0;
            for (const auto& span : spans[row]) {
                check(span.begin == next && span.begin < span.end &&
                      span.end <= fixture.lines[row].size(),
                      "guard syntax emits contiguous positive spans within original bytes");
                next = span.end;
            }
            check(next == fixture.lines[row].size(), "guard syntax covers every visible byte");
            for (std::size_t byte = 0; byte < fixture.lines[row].size(); ++byte) {
                const auto& color = color_at(spans[row], byte);
                check(color.foreground == expected[offsets[row] + byte] && color.background == 0,
                      "cached native guard colors differ for " + std::string(fixture.name) +
                      " at row " + std::to_string(row) + " byte " + std::to_string(byte));
            }
        }
        // Native manual spelling is separate; retain mcpp's existing context filter.
        const auto spelling = highlighter.spelling_ranges("original.guard", fixture.lines);
        check(spelling && *spelling == eligible,
              "guard syntax preserves context spelling at original UTF-8 and control-byte offsets");
        const auto fresh = highlighter.highlight("original.guard", {"x"});
        const auto fresh_spelling = highlighter.spelling_ranges("original.guard", {"x"});
        check(color_at(fresh.front(), 0U).foreground == 7 && fresh_spelling &&
              *fresh_spelling == (fixture.default_spelling
                  ? std::vector<mcpp::SyntaxSpellingRange>{{0U, 1U}}
                  : std::vector<mcpp::SyntaxSpellingRange>{}),
              "empty keywords and wildcard contexts do not retain state between scans");
        check(same_spans(highlighter.highlight("original.guard", fixture.lines), spans) &&
              highlighter.spelling_ranges("original.guard", fixture.lines) == spelling,
              "guard syntax remains deterministic after a separate scan");
        last_spans = spans;
        last_spelling = *spelling;
    }
    const auto broken = root / "syntax.d" / "broken.syntax";
    write(registry, "file . Original\\sBroken\ninclude broken.syntax\n");
    write(broken, R"(context default lightgray black
 keyword \[\] brightmagenta
context wholeright I* J brightmagenta black
unsupported_rule
)");
    bool rejected = false;
    try {
        highlighter.load_registry(registry, root / "syntax.d");
    } catch (const std::exception& error) {
        const std::string_view message(error.what());
        rejected = message.find(broken.string() + ":4:") != std::string_view::npos &&
                   message.find("unsupported syntax directive") != std::string_view::npos;
    }
    check(rejected &&
          same_spans(highlighter.highlight("original.guard", fixtures.back().lines), last_spans) &&
          highlighter.spelling_ranges("original.guard", fixtures.back().lines) ==
              std::optional(last_spelling),
          "failed guard reload preserves prior rules and reports the included source and line");
    std::cout << "PASS cached native empty keywords and wildcard-edge contexts, byte progress and reload\n";
}

}  // namespace

int main(const int argc, const char* const* argv) {
    try {
        if (argc != 2 && !(argc == 3 && (std::string_view(argv[2]) == "spellcheck" ||
                                       std::string_view(argv[2]) == "native-bytes" ||
                                       std::string_view(argv[2]) == "native-format" ||
                                       std::string_view(argv[2]) == "native-consumption" ||
                                       std::string_view(argv[2]) == "native-context" ||
                                       std::string_view(argv[2]) == "native-guards"))) {
            throw std::runtime_error(
                "usage: syntax-model owned-fixture-directory "
                "[spellcheck|native-bytes|native-format|native-consumption|native-context|native-guards]"
            );
        }
        const auto root = std::filesystem::path(argv[1]);
        const auto definitions = root / "syntax.d";
        std::filesystem::create_directories(definitions);
        if (argc == 3) {
            if (std::string_view(argv[2]) == "native-guards") test_native_guards(root);
            else if (std::string_view(argv[2]) == "native-context") test_native_contexts(root);
            else if (std::string_view(argv[2]) == "native-consumption") test_native_consumption(root);
            else if (std::string_view(argv[2]) == "native-format") test_native_format(root);
            else if (std::string_view(argv[2]) == "native-bytes") test_native_bytes(root);
            else test_spelling_contexts(root);
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
