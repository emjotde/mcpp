#include "mcpp/translation.hpp"

#include <algorithm>
#include <array>
#include <clocale>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <unistd.h>

namespace {

namespace fs = std::filesystem;
using mcpp::TranslationEnvironment;
using mcpp::TranslationIssue;
using mcpp::UiLabel;
using mcpp::UiTranslations;
using Messages = std::vector<std::pair<std::string, std::string>>;
using Bytes = std::vector<unsigned char>;

static_assert(!std::is_invocable_v<decltype(&UiTranslations::text),
                                 UiTranslations&, std::string_view>);

void require(const bool condition, const std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

void word(Bytes& bytes, const std::size_t offset, const std::uint32_t value, const bool big) {
    for (std::size_t index = 0; index < 4U; ++index) {
        const auto shift = static_cast<unsigned>((big ? 3U - index : index) * 8U);
        bytes[offset + index] = static_cast<unsigned char>((value >> shift) & 0xffU);
    }
}

// Original tiny fixtures, generated directly from the public GNU .mo layout.
Bytes mo(
    Messages messages, const bool big = false, const std::string_view charset = "UTF-8",
    const std::string_view plural_forms = {}
) {
    auto header = "Content-Type: text/plain; charset=" + std::string(charset) + "\n";
    if (!plural_forms.empty()) header += "Plural-Forms: " + std::string(plural_forms) + "\n";
    messages.emplace_back("", std::move(header));
    std::sort(messages.begin(), messages.end());
    const auto count = messages.size();
    Bytes bytes(28U + count * 16U, 0U);
    word(bytes, 0U, 0x950412deU, big);
    word(bytes, 8U, static_cast<std::uint32_t>(count), big);
    word(bytes, 12U, 28U, big);
    word(bytes, 16U, static_cast<std::uint32_t>(28U + count * 8U), big);
    for (const bool original : {true, false}) {
        for (std::size_t index = 0; index < count; ++index) {
            const auto& text = original ? messages[index].first : messages[index].second;
            const auto row = 28U + (original ? 0U : count * 8U) + index * 8U;
            word(bytes, row, static_cast<std::uint32_t>(text.size()), big);
            word(bytes, row + 4U, static_cast<std::uint32_t>(bytes.size()), big);
            bytes.insert(bytes.end(), text.begin(), text.end());
            bytes.push_back(0U);
        }
    }
    return bytes;
}

void catalog(const fs::path& root, const std::string_view locale, const Bytes& bytes) {
    const auto path = root / locale / "LC_MESSAGES" / "mcpp.mo";
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    require(static_cast<bool>(output), "write original .mo fixture");
}

std::size_t issues(const UiTranslations& labels, const TranslationIssue issue) {
    std::size_t result = 0;
    for (const auto& diagnostic : labels.diagnostics()) {
        if (diagnostic.issue == issue) result += diagnostic.count;
    }
    return result;
}

struct RestoreEnvironment {
    static constexpr std::array<const char*, 4> names{"LC_ALL", "LC_MESSAGES", "LANG", "LANGUAGE"};
    std::array<std::optional<std::string>, names.size()> values;

    RestoreEnvironment() {
        for (std::size_t index = 0; index < names.size(); ++index) {
            if (const auto* value = std::getenv(names[index])) values[index] = value;
        }
    }
    ~RestoreEnvironment() {
        for (std::size_t index = 0; index < names.size(); ++index) {
            if (values[index]) ::setenv(names[index], values[index]->c_str(), 1);
            else ::unsetenv(names[index]);
        }
    }
};

void locale_case(const fs::path& root) {
    catalog(root, "fr_FR.utf8", mo({
        {"File", "Fichier"}, {"Command", "Commande"}, {"Right", "Droite"},
        {"OK", "Valider"}, {"Cancel", ""}, {"Quit", "Quitter"}, {"Yes", "Oui"}, {"No", "Non"},
    }));
    catalog(root, "fr", mo({{"Left", "Gauche"}, {"Options", "Options"}}));
    catalog(root, "de", mo({{"Cancel", "Abbrechen"}, {"Yes", "Ja"}}));
    catalog(root, "pl", mo({
        {"Left", "Lewy"}, {"File", "Plik"}, {"Command", "Polecenie"}, {"Options", "Opcje"},
        {"Right", "Prawy"}, {"OK", "OK"}, {"Cancel", "Anuluj"}, {"Yes", "Tak"}, {"No", "Nie"},
        {"Error", "Błąd"}, {"Warning", "Ostrzeżenie"}, {"Confirmation", "Potwierdzenie"},
        {"Confirm", "Potwierdź"}, {"Quit", "Wyjdź"},
    }));

    const auto all = UiTranslations::load(root, {"fr_FR.UTF-8@formal", "de", "pl", ""});
    require(all.text(UiLabel::file) == "Fichier" && all.text(UiLabel::left) == "Gauche",
            "LC_ALL wins; normalized codeset and locale-component fallbacks work");
    require(all.text(UiLabel::cancel) == "Cancel", "LC_MESSAGES is not an extra fallback");
    require(UiTranslations::load(root, {"", "de", "fr", ""}).text(UiLabel::cancel) == "Abbrechen",
            "LC_MESSAGES takes precedence over LANG");
    require(UiTranslations::load(root, {"", "", "fr", ""}).text(UiLabel::left) == "Gauche",
            "LANG supplies the message locale");
    const auto polish = UiTranslations::load(root, {"", "pl_PL.UTF-8", "", ""});
    require(polish.translated_labels() == mcpp::ui_label_count &&
            polish.text(UiLabel::quit) == "Wyjdź" && polish.diagnostics().empty(),
            "the complete bounded primary-label fixture is available for the reserved UI case");
    for (const auto* invariant : {"C", "POSIX", "C.UTF-8"}) {
        const auto english = UiTranslations::load(root, {invariant, "fr", "fr", "fr:de"});
        require(english.translated_labels() == 0U && english.loaded_catalogs().empty(),
                "C/POSIX locales suppress LANGUAGE, including C.UTF-8");
    }

    RestoreEnvironment restore;
    const std::string before = std::setlocale(LC_MESSAGES, nullptr);
    require(::setenv("LC_ALL", "pl_PL.UTF-8", 1) == 0 &&
            ::setenv("LANGUAGE", "missing:fr_FR.UTF-8:de:C:pl", 1) == 0,
            "set isolated process environment");
    const auto preferences = UiTranslations::load(root);
    require(preferences.text(UiLabel::file) == "Fichier" &&
            preferences.text(UiLabel::cancel) == "Abbrechen",
            "LANGUAGE is searched per label, with missing and empty-translation fallback");
    require(preferences.text(UiLabel::confirm) == "Confirm", "C in LANGUAGE stops later preferences");
    require(preferences.text(UiLabel::warning) == "Warning" && preferences.diagnostics().empty(),
            "missing catalogs and labels preserve English without noise");
    require(std::string(std::setlocale(LC_MESSAGES, nullptr)) == before &&
            std::string(std::getenv("LC_ALL")) == "pl_PL.UTF-8",
            "loading neither changes the global locale nor rewrites the environment");
    std::cout << "case 1 locale/environment/fallback: ok\n";
}

void singular_case(const fs::path& root) {
    catalog(root, "pl", mo({
        {"Error", "Błąd"}, {"OK", "Dobrze"}, {"Cancel", "Anuluj"},
        {std::string("File\0Files", 10U), std::string("plik\0pliki\0plików", 18U)},
        {"menu\004Right", "Prawy"}, {"Options", std::string("\xc0\xaf", 2U)},
        {"Warning", "\033[31m"}, {"notes.txt", "notatki.txt"},
    }, true, "UTF-8",
        "nplurals=3; plural=(n==1?0:(n%10>=2&&n%10<=4&&(n%100<12||n%100>14))?1:2);"));
    const auto labels = UiTranslations::load(root, {"", "pl_PL.UTF-8", "", ""});
    require(labels.text(UiLabel::error) == "Błąd" && labels.text(UiLabel::ok) == "Dobrze" &&
            labels.text(UiLabel::cancel) == "Anuluj",
            "big-endian catalog preserves UTF-8 singular labels");
    require(labels.translated_labels() == 3U && labels.loaded_catalogs().size() == 1U,
            "only supported built-in labels are retained");
    require(labels.text(UiLabel::file) == "File" && labels.text(UiLabel::right) == "Right",
            "plural and contextual entries are not misused as singular labels");
    require(issues(labels, TranslationIssue::skipped_plural_entries) == 1U &&
            issues(labels, TranslationIssue::skipped_context_entries) == 1U &&
            issues(labels, TranslationIssue::invalid_label) == 2U,
            "unsupported entries and invalid UTF-8/control labels have counted diagnostics");
    require(labels.text(UiLabel::options) == "Options" && labels.text(UiLabel::warning) == "Warning",
            "invalid translations keep their English fallback");
    std::cout << "case 2 singular/UTF-8/byte-order bounds: ok\n";
}

void rejection_case(const fs::path& root) {
    auto broken = mo({{"File", "Do not publish"}, {"Left", "Also invalid"}});
    word(broken, 12U, 0xfffffff0U, false);
    catalog(root, "broken", broken);
    catalog(root, "latin", mo({{"File", "Do not convert"}}, false, "ISO-8859-1"));
    auto future = mo({{"File", "Unsupported revision"}});
    word(future, 4U, 1U, false);
    catalog(root, "future", future);
    catalog(root, "de", mo({{"File", "Datei"}}));
    const auto labels = UiTranslations::load(root,
        {"", "de_DE.UTF-8", "", "../escape:broken:latin:future:de"});
    require(labels.text(UiLabel::file) == "Datei" && labels.text(UiLabel::left) == "Left",
            "rejected catalogs never publish partial labels and later languages remain usable");
    require(issues(labels, TranslationIssue::invalid_preference) == 1U &&
            issues(labels, TranslationIssue::invalid_catalog) == 1U &&
            issues(labels, TranslationIssue::unsupported_charset) == 1U &&
            issues(labels, TranslationIssue::unsupported_revision) == 1U,
            "unsafe locales, corrupt tables and unsupported catalog features are explicit");
    require(UiTranslations::load(root, {"de", "", "", ""}, "../mc").translated_labels() == 0U,
            "domain cannot escape LC_MESSAGES");
    require(issues(UiTranslations::load(root, {"de", "", "", ""}, "../mc"),
                   TranslationIssue::invalid_domain) == 1U,
            "invalid domains are diagnosed");
    std::cout << "case 3 rejected-catalog fallback: ok\n";
}

}  // namespace

int main(int argc, char** argv) {
    try {
        require(::getuid() == 1001U && ::geteuid() == 1001U &&
                fs::is_regular_file("/review/.operation-regressions-disposable"),
                "translation filesystem tests require the approved UID-1001 disposable container");
        const auto root = argc == 2 ? fs::path(argv[1]) :
            fs::path("/review/cases") / ("l26-translation-" + std::to_string(::getpid()));
        require(root.is_absolute() && root.lexically_normal().string().starts_with("/review/cases/"),
                "fixture directory must be under /review/cases");
        require(fs::create_directory(root), "use a fresh translation fixture directory");
        locale_case(root / "locale");
        singular_case(root / "singular");
        rejection_case(root / "rejection");
        std::cout << "translation fixtures: " << root << '\n';
    } catch (const std::exception& error) {
        std::cerr << "UI translations: " << error.what() << '\n';
        return 1;
    }
}
