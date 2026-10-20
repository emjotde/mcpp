#include "mcpp/translation.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;
using mcpp::TranslationIssue;
using mcpp::UiCaption;
using mcpp::UiCountMessage;
using mcpp::UiLabel;
using mcpp::UiTranslations;
using Bytes = std::vector<unsigned char>;
using Messages = std::vector<std::pair<std::string, std::string>>;

constexpr auto caption = UiCaption::delete_confirmation;
constexpr auto question = UiCountMessage::delete_selected_items;
constexpr std::string_view context = "delete-dialog";
constexpr std::string_view singular = "Delete {count} selected item?";
constexpr std::string_view plural = "Delete {count} selected items?";
constexpr std::string_view three_forms =
    "nplurals=3; plural=(n==1?0:n%10>=2&&n%10<=4&&(n%100<12||n%100>14)?1:2);";
constexpr std::string_view two_forms = "nplurals=2; plural=(n!=1);";

static_assert(!std::is_invocable_v<decltype(&UiTranslations::caption),
                                 const UiTranslations&, std::string_view>);
static_assert(!std::is_invocable_v<decltype(&UiTranslations::count_text),
                                 const UiTranslations&, std::string_view, std::uint64_t>);
static_assert(!std::is_convertible_v<std::uint64_t, UiCountMessage>);

void require(const bool value, const std::string_view message) {
    if (!value) throw std::runtime_error(std::string(message));
}

std::string contextual(const std::string_view message, const std::string_view prefix = context) {
    return std::string(prefix) + '\004' + std::string(message);
}

std::string plural_key(
    const std::string_view prefix = context, const std::string_view second = plural
) {
    return contextual(singular, prefix) + '\0' + std::string(second);
}

std::string join(const std::vector<std::string>& forms) {
    std::string result;
    for (std::size_t index = 0U; index < forms.size(); ++index) {
        if (index != 0U) result += '\0';
        result += forms[index];
    }
    return result;
}

void word(Bytes& data, const std::size_t offset, const std::uint32_t value, const bool big) {
    for (std::size_t index = 0U; index < 4U; ++index) {
        const auto shift = static_cast<unsigned>((big ? 3U - index : index) * 8U);
        data[offset + index] = static_cast<unsigned char>((value >> shift) & 0xffU);
    }
}

Bytes mo(
    Messages messages, const std::string_view forms = {}, const bool big = false,
    const std::string_view charset = "UTF-8", const std::string_view extra_header = {}
) {
    auto header = "Content-Type: text/plain; charset=" + std::string(charset) + "\n";
    if (!forms.empty()) header += "Plural-Forms: " + std::string(forms) + "\n";
    header += extra_header;
    messages.emplace_back("", std::move(header));
    std::sort(messages.begin(), messages.end());
    const auto count = messages.size();
    Bytes data(28U + count * 16U, 0U);
    word(data, 0U, 0x950412deU, big);
    word(data, 8U, static_cast<std::uint32_t>(count), big);
    word(data, 12U, 28U, big);
    word(data, 16U, static_cast<std::uint32_t>(28U + count * 8U), big);
    for (const bool original : {true, false}) {
        for (std::size_t index = 0U; index < count; ++index) {
            const auto& text = original ? messages[index].first : messages[index].second;
            const auto row = 28U + (original ? 0U : count * 8U) + index * 8U;
            word(data, row, static_cast<std::uint32_t>(text.size()), big);
            word(data, row + 4U, static_cast<std::uint32_t>(data.size()), big);
            data.insert(data.end(), text.begin(), text.end());
            data.push_back(0U);
        }
    }
    return data;
}

fs::path catalog(const fs::path& root, const std::string_view locale, const Bytes& bytes) {
    const auto path = root / locale / "LC_MESSAGES/mcpp.mo";
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    require(static_cast<bool>(output), "write original tiny .mo fixture");
    return path;
}

UiTranslations load(const fs::path& root, const std::string_view locale) {
    return UiTranslations::load(root, {std::string(locale), {}, {}, {}});
}

std::size_t issues(const UiTranslations& translations, const TranslationIssue issue) {
    std::size_t count = 0U;
    for (const auto& diagnostic : translations.diagnostics()) {
        if (diagnostic.issue == issue) count += diagnostic.count;
    }
    return count;
}

void oracle(
    const fs::path& root, const std::string_view mode, const std::string_view locale,
    const std::string_view fallback, const std::uint64_t count, const std::string_view expected
) {
    std::ofstream output(root / "oracle.tsv", std::ios::app);
    output << mode << '\t' << locale << '\t' << fallback << '\t' << count << '\t' << expected << '\n';
    require(static_cast<bool>(output), "write Python gettext cross-check expectations");
}

void expect_count(
    const UiTranslations& translations, const std::uint64_t count, const std::string_view expected
) {
    const auto text = translations.count_text(question, count);
    require(text.translated && !text.diagnostic && text.text == expected, "translated count text");
}

void context_case(const fs::path& root) {
    catalog(root, "first", mo({
        {"Delete", "WRONG context-free caption"}, {"File", "Original file caption"},
        {contextual("Delete", "other-dialog"), "WRONG other-context caption"},
    }));
    catalog(root, "empty-caption", mo({
        {"Delete", "WRONG context-free caption"}, {contextual("Delete"), ""},
    }));
    catalog(root, "later", mo({{contextual("Delete"), "Delete in its own context"}}));
    catalog(root, "fixture_A.utf8", mo({{contextual("Delete"), "Usuwanie próbne"}}, {}, true));
    const auto first = load(root, "first");
    require(first.caption(caption) == "Delete" && !first.translated(caption) &&
            first.text(UiLabel::file) == "Original file caption",
            "missing/empty context never falls back to a context-free translation");
    require(issues(first, TranslationIssue::skipped_context_entries) == 1U,
            "only the unsupported context is counted as skipped");
    require(load(root, "empty-caption").caption(caption) == "Delete",
            "empty contextual captions preserve the loader's empty-translation fallback");
    const auto fallback = UiTranslations::load(root, {"fixture", {}, {}, "first:later:C:ignored"});
    require(fallback.caption(caption) == "Delete in its own context" && fallback.translated(caption),
            "contextual lookup falls through LANGUAGE using the same contextual key");
    const auto codeset = UiTranslations::load(root, {"fixture_A.UTF-8@formal", "first", "first", {}});
    require(codeset.caption(caption) == "Usuwanie próbne" && codeset.diagnostics().empty(),
            "LC_ALL, locale components, normalized UTF-8 codeset and big-endian caption");
    for (const auto* invariant : {"C.UTF-8", "POSIX"}) {
        const auto english = UiTranslations::load(root, {invariant, "later", "later", "later"});
        require(english.loaded_catalogs().empty() && english.caption(caption) == "Delete" &&
                !english.count_text(question, 2U).translated,
                "C/POSIX suppress catalog and LANGUAGE lookup for the new messages");
    }
    require(first.caption(UiCaption::count).empty(), "invalid caption enum is not a lookup key");
    oracle(root, "caption", "first", "-", 0U, "Delete");
    oracle(root, "caption", "first", "later", 0U, "Delete in its own context");
    oracle(root, "caption", "fixture_A.utf8", "-", 0U, "Usuwanie próbne");
    std::cout << "context: exact GNU context/fallback, UTF-8, byte order, C suppression: ok\n";
}

void plural_case(const fs::path& root) {
    const auto forms = join({
        "Usunąć {count} element?", "Usunąć {count} elementy?", "Usunąć {count} elementów?",
    });
    const Messages messages{
        {contextual("Delete"), "Usuwanie próbne"}, {plural_key(), forms},
    };
    catalog(root, "three-le", mo(messages, three_forms));
    catalog(root, "three-be", mo(messages, three_forms, true));
    const std::array<std::pair<std::uint64_t, std::string_view>, 7> samples{{
        {0U, "Usunąć 0 elementów?"}, {1U, "Usunąć 1 element?"},
        {2U, "Usunąć 2 elementy?"}, {5U, "Usunąć 5 elementów?"},
        {12U, "Usunąć 12 elementów?"}, {22U, "Usunąć 22 elementy?"},
        {std::numeric_limits<std::uint64_t>::max(), "Usunąć 18446744073709551615 elementów?"},
    }};
    for (const auto* locale : {"three-le", "three-be"}) {
        const auto translations = load(root, locale);
        require(translations.diagnostics().empty(), "supported plural/context entries are not skipped");
        for (const auto& [count, expected] : samples) {
            expect_count(translations, count, expected);
            oracle(root, "count", locale, "-", count, expected);
        }
    }

    constexpr std::string_view operators =
        "nplurals=2; plural=n==0?0:!(n!=1)?(0&&(1/0)):n==2?(1||(1%0)):"
        "n>=3&&n<=4&&n>2?((n*2+4-2)/2%2):n<6?0:1;";
    catalog(root, "operators", mo({{plural_key(), join({"zero {count}", "one {count}"})}}, operators));
    const auto arithmetic = load(root, "operators");
    require(arithmetic.diagnostics().empty(), "all GNU plural operators parse eagerly");
    for (const auto& [count, expected] :
         std::array<std::pair<std::uint64_t, std::string_view>, 7>{{
             {0U, "zero 0"}, {1U, "zero 1"}, {2U, "one 2"}, {3U, "zero 3"},
             {4U, "one 4"}, {5U, "zero 5"}, {6U, "one 6"},
         }}) {
        expect_count(arithmetic, count, expected);
        oracle(root, "count", "operators", "-", count, expected);
    }
    catalog(root, "empty", mo({{plural_key(), join({"", "First {count}"})}}, two_forms));
    const auto fallback = UiTranslations::load(root, {"fixture", {}, {}, "empty:three-le"});
    expect_count(fallback, 1U, "Usunąć 1 element?");
    expect_count(fallback, 2U, "First 2");
    catalog(root, "key", mo({{plural_key(context, "Different plural original"),
                             join({"Singular {count}", "Plural {count}"})}}, two_forms));
    const auto key = load(root, "key");
    expect_count(key, 2U, "Plural 2");
    oracle(root, "count", "key", "-", 2U, "Plural 2");
    catalog(root, "wrong-context", mo({
        {std::string(singular) + '\0' + std::string(plural), join({"WRONG {count}", "WRONG {count}"})},
        {plural_key("other-dialog"), join({"WRONG {count}", "WRONG {count}"})},
    }, two_forms));
    const auto missing = load(root, "wrong-context").count_text(question, 2U);
    require(!missing.translated && !missing.diagnostic && missing.text == "Delete 2 selected items?",
            "plural contextual fallback returns the caller's English original, not another context");
    oracle(root, "count", "wrong-context", "-", 2U, "Delete 2 selected items?");
    std::cout << "plural: three forms, uint64 count, both byte orders, operators, short-circuit: ok\n";
}

std::string balanced_sum(const std::size_t terms) {
    if (terms == 1U) return "1";
    return "(" + balanced_sum(terms / 2U) + "+" + balanced_sum(terms - terms / 2U) + ")";
}

std::string nested_ternary(const std::size_t levels) {
    std::string expression;
    for (std::size_t index = 0U; index < levels; ++index) expression += "n?0:";
    return expression + "0";
}

void rejected(
    const fs::path& root, const std::string_view locale, const Bytes& bytes,
    const TranslationIssue issue
) {
    catalog(root, locale, bytes);
    const auto translations = load(root, locale);
    require(issues(translations, issue) == 1U, "rejected catalog has its precise eager diagnostic");
    require(translations.loaded_catalogs().empty() && translations.caption(caption) == "Delete" &&
            translations.text(UiLabel::file) == "File" &&
            !translations.count_text(question, 1U).translated,
            "invalid catalog never publishes a partial built-in label/caption/count message");
}

void failure_case(const fs::path& root) {
    const Messages base{{"File", "MUST NOT LEAK"}, {contextual("Delete"), "MUST NOT LEAK"},
                        {plural_key(), join({"one {count}", "many {count}"})}};
    rejected(root, "syntax", mo(base, "nplurals=2; plural=n?0;"), TranslationIssue::invalid_plural_forms);
    rejected(root, "missing", mo(base), TranslationIssue::invalid_plural_forms);
    rejected(root, "zero-forms", mo(base, "nplurals=0; plural=0;"), TranslationIssue::invalid_plural_forms);
    rejected(root, "too-many-forms", mo(base, "nplurals=17; plural=0;"), TranslationIssue::limit_exceeded);
    rejected(root, "form-count", mo(base, "nplurals=3; plural=n!=1;"), TranslationIssue::invalid_plural_forms);
    rejected(root, "extra-forms", mo(base, "nplurals=1; plural=0;"), TranslationIssue::invalid_plural_forms);
    rejected(root, "duplicate-header", mo(base, two_forms, false, "UTF-8",
             "Plural-Forms: nplurals=2; plural=n!=1;\n"), TranslationIssue::invalid_plural_forms);
    rejected(root, "literal-overflow", mo(base, "nplurals=2; plural=18446744073709551616;"),
             TranslationIssue::plural_arithmetic_overflow);
    rejected(root, "constant-overflow", mo(base, "nplurals=2; plural=18446744073709551615+1;"),
             TranslationIssue::plural_arithmetic_overflow);
    rejected(root, "constant-zero", mo(base, "nplurals=2; plural=1/0;"),
             TranslationIssue::plural_division_by_zero);
    rejected(root, "constant-index", mo(base, "nplurals=2; plural=2;"),
             TranslationIssue::plural_index_out_of_range);
    rejected(root, "bytes", mo(base, "nplurals=2; plural=n" +
             std::string(UiTranslations::max_plural_expression_bytes, ' ') + "!=1;"),
             TranslationIssue::limit_exceeded);
    rejected(root, "depth", mo(base, "nplurals=2; plural=" +
             std::string(UiTranslations::max_plural_expression_depth, '!') + "n;"),
             TranslationIssue::limit_exceeded);
    rejected(root, "ternary-depth", mo(base, "nplurals=2; plural=" +
             nested_ternary(UiTranslations::max_plural_expression_depth) + ";"),
             TranslationIssue::limit_exceeded);
    rejected(root, "nodes", mo(base, "nplurals=2; plural=!" + balanced_sum(129U) + ";"),
             TranslationIssue::limit_exceeded);
    auto wrong_format = base;
    wrong_format.back().second = join({"{count} {count}", "many {count}"});
    rejected(root, "format", mo(wrong_format, two_forms), TranslationIssue::invalid_count_format);
    auto bad_extent = mo(base, two_forms);
    word(bad_extent, 12U, 0xfffffff0U, false);
    rejected(root, "extent", bad_extent, TranslationIssue::invalid_catalog);

    constexpr std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();
    struct RuntimeFailure {
        std::string_view locale;
        std::string_view expression;
        std::uint64_t count;
        TranslationIssue issue;
    };
    constexpr std::array<RuntimeFailure, 6> failures{{
        {"divide", "n?0:1/0", 0U, TranslationIssue::plural_division_by_zero},
        {"remainder", "n?0:1%0", 0U, TranslationIssue::plural_division_by_zero},
        {"add", "n+1", maximum, TranslationIssue::plural_arithmetic_overflow},
        {"multiply", "n*2", maximum, TranslationIssue::plural_arithmetic_overflow},
        {"subtract", "n-1", 0U, TranslationIssue::plural_arithmetic_overflow},
        {"index", "n", 2U, TranslationIssue::plural_index_out_of_range},
    }};
    for (const auto& failure : failures) {
        const auto path = catalog(root, failure.locale, mo(base,
            "nplurals=2; plural=" + std::string(failure.expression) + ";"));
        const auto translations = load(root, failure.locale);
        require(translations.diagnostics().empty(), "valid count-dependent formula is not guessed at load");
        const auto result = translations.count_text(question, failure.count);
        require(!result.translated && result.diagnostic && result.diagnostic->issue == failure.issue &&
                result.diagnostic->catalog == path,
                "runtime arithmetic/index errors return a catalog-specific diagnostic");
        const auto again = translations.count_text(question, failure.count);
        require(again.diagnostic && translations.diagnostics().empty() && again.text == result.text,
                "count lookup neither appends diagnostics nor prints on repeated renders");
    }

    const std::array<std::pair<std::string, std::uint64_t>, 5> boundaries{{
        {"1?0:1/0", 2U},
        {std::string(UiTranslations::max_plural_expression_depth - 1U, '!') + "n", 1U},
        {"!" + balanced_sum(128U), 1U},
        {"n" + std::string(UiTranslations::max_plural_expression_bytes - 4U, ' ') + "!=1", 1U},
        {nested_ternary(UiTranslations::max_plural_expression_depth - 1U), 0U},
    }};
    for (std::size_t index = 0U; index < boundaries.size(); ++index) {
        const auto locale = "boundary-" + std::to_string(index);
        const auto& [expression, count] = boundaries[index];
        catalog(root, locale, mo(base, "nplurals=2; plural=" + expression + ";"));
        const auto translations = load(root, locale);
        require(translations.diagnostics().empty(), "exact formula limits and dead constant branch are accepted");
        expect_count(translations, count, "one " + std::to_string(count));
    }
    std::vector<std::string> forms;
    for (std::size_t index = 0U; index < UiTranslations::max_plural_forms; ++index) {
        forms.push_back("form" + std::to_string(index) + " {count}");
    }
    catalog(root, "form-boundary", mo({{plural_key(), join(forms)}}, "nplurals=16; plural=n%16;"));
    expect_count(load(root, "form-boundary"), 15U, "form15 15");
    const auto fallback = UiTranslations::load(root, {"fixture", {}, {}, "syntax:form-boundary"});
    expect_count(fallback, 15U, "form15 15");
    require(fallback.text(UiLabel::file) == "File", "bad formula cannot leak earlier labels during fallback");

    auto invalid_utf8 = base;
    invalid_utf8.back().second = join({std::string("\xc0\xaf", 2U) + " {count}", "many {count}"});
    catalog(root, "utf8", mo(invalid_utf8, two_forms));
    const auto utf8 = load(root, "utf8");
    require(issues(utf8, TranslationIssue::invalid_label) == 1U &&
            !utf8.count_text(question, 2U).translated && utf8.translated(caption),
            "one invalid UTF-8 form skips the complete count message, not unrelated valid captions");
    std::cout << "failure: eager atomic rejection, exact limits, checked runtime arithmetic/index: ok\n";
}

}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2 || argc == 3,
                "usage: mcpp_translation_plural_tests context|plural|failure [/review/cases/FRESH]");
        require(::getuid() == 1001U && ::geteuid() == 1001U &&
                fs::is_regular_file("/.dockerenv") &&
                fs::is_regular_file("/review/.operation-regressions-disposable"),
                "requires the approved UID-1001 disposable Docker container");
        const std::string_view selected = argv[1];
        require(selected == "context" || selected == "plural" || selected == "failure",
                "select exactly one of the three bounded model groups");
        const fs::path root = argc == 3 ? fs::path(argv[2]) :
            fs::path("/review/cases") / ("l26-next-" + std::string(selected) + "-" + std::to_string(::getpid()));
        require(root.is_absolute() && root.lexically_normal().string().starts_with("/review/cases/"),
                "fixtures must be under /review/cases");
        ::umask(0077);
        require(fs::create_directory(root), "fixture root must be fresh");
        if (selected == "context") context_case(root);
        else if (selected == "plural") plural_case(root);
        else failure_case(root);
        std::cout << "original fixtures: " << root << '\n';
    } catch (const std::exception& error) {
        std::cerr << "plural/context translations: " << error.what() << '\n';
        return 1;
    }
}
