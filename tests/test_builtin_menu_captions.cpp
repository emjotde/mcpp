#include "mcpp/translation.hpp"

#include "builtin_menu_captions.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
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
namespace detail = mcpp::detail;
using mcpp::TranslationIssue;
using mcpp::UiCaption;
using mcpp::UiTranslations;
using Bytes = std::vector<unsigned char>;
using Messages = std::vector<std::pair<std::string, std::string>>;

static_assert(static_cast<std::size_t>(UiCaption::delete_confirmation) == 0U);
static_assert(static_cast<std::size_t>(UiCaption::editor_menu_options) == 69U);
static_assert(!std::is_invocable_v<decltype(&UiTranslations::caption),
                                 const UiTranslations&, std::string_view>);
static_assert(!std::is_invocable_v<decltype(&UiTranslations::caption),
                                 const UiTranslations&, unsigned>);
static_assert(!std::is_invocable_v<decltype(&mcpp::english_ui_caption), std::string_view>);
static_assert(!std::is_convertible_v<unsigned, UiCaption>);
static_assert(!std::is_convertible_v<std::string_view, UiCaption>);

struct SourceRow {
    std::string_view label;
    int action;
    char mnemonic{};
};

constexpr auto source_panel_rows = std::to_array<SourceRow>({
    {"File listing", 12, 'g'},
    {"Quick view         C-x q", 10, 'q'},
    {"Info               C-x i", 11, 'i'},
    {"Tree", 14, 't'},
    {"", -2},
    {"Listing format...", 16, 'l'},
    {"Sort order...", 17, 's'},
    {"Filter...", 13, 'f'},
    {"Encoding...        M-e", 18, 'e'},
    {"", -2},
    {"FTP link...", -1},
    {"Shell link...", -1},
    {"SFTP link...", -1},
    {"Panelize", 15, 'z'},
    {"", -2},
    {"Rescan             C-r", 7, 'r'},
});

constexpr auto source_file_rows = std::to_array<SourceRow>({
    {"View              F3", 0, 'v'},
    {"View file...", 18, 'w'},
    {"Filtered view     M-!", 22, 'f'},
    {"Edit              F4", 1, 'e'},
    {"Copy              F5", 2, 'c'},
    {"Chmod             C-x c", 8, 'h'},
    {"Link              C-x l", 11, 'l'},
    {"Symlink           C-x s", 12, 's'},
    {"Relative symlink  C-x v", 13, 'k'},
    {"Edit symlink      C-x C-s", 19, 'y'},
    {"Chown             C-x o", 9, 'o'},
    {"Advanced chown", 20, 'a'},
    {"Chattr            C-x e", 21, 't'},
    {"Rename/Move       F6", 4, 'r'},
    {"Mkdir             F7", 5, 'm'},
    {"Delete            F8", 6, 'd'},
    {"Quick cd          M-c", 14, 'q'},
    {"", -2},
    {"Select group      +", 15, 'g'},
    {"Unselect group    -", 16, 'n'},
    {"Invert selection  *", 17, 'i'},
    {"", -2},
    {"Exit              F10", 10, 'x'},
});

constexpr auto source_command_rows = std::to_array<SourceRow>({
    {"User menu                     F2", 8, 'u'},
    {"Directory tree", 11, 'd'},
    {"Find file                     M-?", 0, 'f'},
    {"Swap panels                   C-u", 5, 'w'},
    {"Switch panels on/off          C-o", 3, 'p'},
    {"Compare directories           C-x d", 1, 'c'},
    {"Compare files                 C-x C-d", 12, 'o'},
    {"External panelize             C-x !", 13, 'x'},
    {"Show directory sizes          C-Space", 14, 'i'},
    {"", -2},
    {"Command history               M-h", 15, 'h'},
    {"Viewed/edited files history   M-E", 16, 's'},
    {"Directory hotlist             C-\\", 7, 'r'},
    {"Active VFS list               C-x a", 9},
    {"Background jobs               C-x j", 10, 'b'},
    {"Screen list                   M-`", 17, 't'},
    {"", -2},
    {"Undelete files (ext2fs only)", -1},
    {"", -2},
    {"Edit extension file", 18, 'e'},
    {"Edit menu file", 19, 'm'},
    {"Edit highlighting group file", 20, 'g'},
});

constexpr auto source_options_rows = std::to_array<SourceRow>({
    {"Configuration...", 20, 'c'},
    {"Layout...", 21, 'l'},
    {"Panel options...", 22, 'p'},
    {"Confirmation...", 23, 'o'},
    {"Appearance...", 24, 'a'},
    {"Display bits...", 25, 'd'},
    {"Learn keys...", 26, 'k'},
    {"Virtual FS...", 27},
    {"", -2},
    {"Save setup", 3, 's'},
});

constexpr std::array<std::span<const SourceRow>, 5> source_manager_rows{
    source_panel_rows, source_file_rows, source_command_rows, source_options_rows, source_panel_rows,
};

constexpr std::array<std::string_view, 7> source_editor_names{
    "File     ", "Edit     ", "Search     ", "Command     ",
    "Format     ", "Window     ", "Options",
};
constexpr std::array<std::size_t, 7> source_editor_mnemonics{0U, 0U, 0U, 0U, 3U, 0U, 0U};

consteval bool matches_source_rows(
    const std::span<const detail::BuiltinMenuCaption> captions,
    const std::span<const SourceRow> source
) {
    if (captions.size() != source.size()) return false;
    for (std::size_t index = 0U; index < source.size(); ++index) {
        if (source[index].action == -2) {
            if (captions[index].id != UiCaption::count || !captions[index].shortcut.empty() ||
                !source[index].label.empty() || source[index].mnemonic != 0) return false;
            continue;
        }
        const auto english = detail::builtin_english_caption(captions[index].id);
        if (english.empty() || !source[index].label.starts_with(english) ||
            source[index].label.substr(english.size()) != captions[index].shortcut) return false;
    }
    return true;
}

consteval bool matches_editor_names() {
    if (detail::builtin_editor_menu_caption_ids.size() != source_editor_names.size()) return false;
    for (std::size_t index = 0U; index < source_editor_names.size(); ++index) {
        const auto english = detail::builtin_english_caption(detail::builtin_editor_menu_caption_ids[index]);
        if (english.empty() || !source_editor_names[index].starts_with(english) ||
            source_editor_names[index].substr(english.size()) !=
                (index + 1U == source_editor_names.size() ? "" : "     ")) return false;
    }
    return true;
}

static_assert(matches_source_rows(detail::builtin_panel_menu_captions, source_panel_rows));
static_assert(matches_source_rows(detail::builtin_file_menu_captions, source_file_rows));
static_assert(matches_source_rows(detail::builtin_command_menu_captions, source_command_rows));
static_assert(matches_source_rows(detail::builtin_options_menu_captions, source_options_rows));
static_assert(matches_editor_names());
static_assert(detail::builtin_manager_menu_captions[0].data() ==
              detail::builtin_manager_menu_captions[4].data());

void require(const bool condition, const std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

void word(Bytes& bytes, const std::size_t offset, const std::uint32_t value, const bool big) {
    for (std::size_t index = 0U; index < 4U; ++index) {
        const auto shift = static_cast<unsigned>((big ? 3U - index : index) * 8U);
        bytes[offset + index] = static_cast<unsigned char>((value >> shift) & 0xffU);
    }
}

// Original tiny fixtures, written from the public GNU .mo layout without external resources.
Bytes mo(Messages messages, const bool big = false) {
    messages.emplace_back("", "Content-Type: text/plain; charset=UTF-8\n");
    std::sort(messages.begin(), messages.end());
    const auto count = messages.size();
    Bytes bytes(28U + count * 16U, 0U);
    word(bytes, 0U, 0x950412deU, big);
    word(bytes, 8U, static_cast<std::uint32_t>(count), big);
    word(bytes, 12U, 28U, big);
    word(bytes, 16U, static_cast<std::uint32_t>(28U + count * 8U), big);
    for (const bool original : {true, false}) {
        for (std::size_t index = 0U; index < count; ++index) {
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

std::string key(const UiCaption caption) {
    return std::string(detail::builtin_caption_messages[static_cast<std::size_t>(caption)].key);
}

void catalog(const fs::path& root, const std::string_view locale, const Bytes& bytes) {
    const auto path = root / locale / "LC_MESSAGES/mcpp.mo";
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    require(static_cast<bool>(output), "write original menu-caption .mo fixture");
}

UiTranslations load(const fs::path& root, const std::string_view locale) {
    return UiTranslations::load(root, {std::string(locale), {}, {}, {}});
}

std::size_t issues(const UiTranslations& translations, const TranslationIssue issue) {
    std::size_t result = 0U;
    for (const auto& diagnostic : translations.diagnostics()) {
        if (diagnostic.issue == issue) result += diagnostic.count;
    }
    return result;
}

void require_english(const UiTranslations& translations) {
    for (std::size_t index = 1U; index < detail::builtin_caption_messages.size(); ++index) {
        const auto& message = detail::builtin_caption_messages[index];
        require(translations.caption(message.id) == message.english() &&
                !translations.translated(message.id), "new captions retain their exact English fallback");
    }
}

std::string_view fixture_translation(const UiCaption caption) {
    constexpr std::array overrides{
        std::pair{UiCaption::manager_panel_tree, std::string_view("Drzewo")},
        std::pair{UiCaption::manager_file_view, std::string_view("Widok")},
        std::pair{UiCaption::manager_file_edit, std::string_view("Edytuj")},
        std::pair{UiCaption::manager_file_copy, std::string_view("Kopiuj")},
        std::pair{UiCaption::manager_file_delete, std::string_view("Usuń")},
        std::pair{UiCaption::manager_options_save_setup, std::string_view("Save setup")},
        std::pair{UiCaption::editor_menu_file, std::string_view("Fichier")},
        std::pair{UiCaption::editor_menu_edit, std::string_view("Éditer")},
    };
    for (const auto& [id, translation] : overrides) {
        if (id == caption) return translation;
    }
    return "Menu";
}

void oracle(
    const fs::path& root, const std::string_view locale, const std::string_view fallback,
    const UiCaption caption, const std::string_view expected
) {
    const auto& message = detail::builtin_caption_messages[static_cast<std::size_t>(caption)];
    const auto separator = message.key.find('\004');
    std::ofstream output(root / "oracle.tsv", std::ios::app);
    output << locale << '\t' << fallback << '\t' << message.key.substr(0U, separator)
           << '\t' << message.english() << '\t' << expected << '\n';
    require(static_cast<bool>(output), "write std-Python gettext cross-check");
}

void source_oracle(const fs::path& root) {
    std::ofstream manager(root / "source-manager.tsv");
    for (std::size_t category = 0U; category < source_manager_rows.size(); ++category) {
        const auto rows = source_manager_rows[category];
        for (std::size_t index = 0U; index < rows.size(); ++index) {
            manager << category << '\t' << index << '\t' << rows[index].label << '\t'
                    << rows[index].action << '\t'
                    << static_cast<unsigned>(static_cast<unsigned char>(rows[index].mnemonic)) << '\n';
        }
    }
    require(static_cast<bool>(manager), "write frozen manager source-row oracle");
    std::ofstream editor(root / "source-editor.tsv");
    for (std::size_t index = 0U; index < source_editor_names.size(); ++index) {
        editor << index << '\t' << source_editor_names[index] << '\t' << source_editor_mnemonics[index] << '\n';
    }
    require(static_cast<bool>(editor), "write frozen editor source-row oracle");
}

void positive_case(const fs::path& root) {
    Messages entries;
    for (std::size_t index = 1U; index < detail::builtin_caption_messages.size(); ++index) {
        const auto& message = detail::builtin_caption_messages[index];
        entries.emplace_back(message.key, fixture_translation(message.id));
    }
    for (const bool big : {false, true}) {
        const std::string_view locale = big ? "menus-be" : "menus-le";
        catalog(root, locale, mo(entries, big));
        const auto translations = load(root, locale);
        require(translations.loaded_catalogs().size() == 1U && translations.diagnostics().empty(),
                "complete bounded menu catalog is accepted in either byte order");
        for (std::size_t index = 1U; index < detail::builtin_caption_messages.size(); ++index) {
            const auto& message = detail::builtin_caption_messages[index];
            const auto expected = fixture_translation(message.id);
            require(translations.translated(message.id) && translations.caption(message.id) == expected &&
                    mcpp::english_ui_caption(message.id) == message.english(),
                    "all 69 new typed IDs resolve only their exact contextual entry");
            oracle(root, locale, "-", message.id, expected);
        }
        require(translations.caption(UiCaption::manager_file_edit) == "Edytuj" &&
                translations.caption(UiCaption::editor_menu_edit) == "Éditer",
                "equal English captions remain separate in manager/editor contexts");
        require(translations.translated(UiCaption::manager_options_save_setup) &&
                translations.caption(UiCaption::manager_options_save_setup) ==
                    mcpp::english_ui_caption(UiCaption::manager_options_save_setup),
                "an identity translation remains available for the unchanged English rendering branch");
    }
    source_oracle(root);
    std::cout << "positive: all 69 captions, both byte orders, context isolation and UTF-8: ok\n";
}

void fallback_case(const fs::path& root) {
    catalog(root, "first", mo({
        {key(UiCaption::manager_file_view), ""},
        {key(UiCaption::manager_file_copy), "Kopiuj"},
        {key(UiCaption::manager_file_edit), "Edytuj"},
        {key(UiCaption::editor_menu_edit), ""},
        {"View", "Podgląd"},
        {"other-menu\004View", "Podgląd"},
    }));
    catalog(root, "later", mo({
        {key(UiCaption::manager_file_view), "Widok"},
        {key(UiCaption::manager_file_copy), "Kopia"},
        {key(UiCaption::editor_menu_edit), "Éditer"},
    }));
    const auto first = load(root, "first");
    require(first.caption(UiCaption::manager_file_view) == "View" &&
            !first.translated(UiCaption::manager_file_view),
            "an empty contextual entry never uses a context-free or unrelated-context caption");
    require(first.caption(UiCaption::editor_menu_edit) == "Edit" &&
            !first.translated(UiCaption::editor_menu_edit) &&
            first.caption(UiCaption::manager_file_edit) == "Edytuj",
            "manager captions do not fill missing editor captions");
    oracle(root, "first", "-", UiCaption::manager_file_view, "View");
    oracle(root, "first", "-", UiCaption::editor_menu_edit, "Edit");

    const auto chain = UiTranslations::load(root, {"fixture", {}, {}, "first:later:C:ignored"});
    require(chain.caption(UiCaption::manager_file_view) == "Widok" &&
            chain.caption(UiCaption::editor_menu_edit) == "Éditer" &&
            chain.caption(UiCaption::manager_file_copy) == "Kopiuj",
            "missing and empty new entries use later languages without replacing earlier translations");
    oracle(root, "first", "later", UiCaption::manager_file_view, "Widok");
    oracle(root, "first", "later", UiCaption::editor_menu_edit, "Éditer");
    oracle(root, "first", "later", UiCaption::manager_file_copy, "Kopiuj");
    for (const auto* locale : {"C", "POSIX", "C.UTF-8"}) {
        const auto invariant = UiTranslations::load(root, {locale, "later", "later", "first:later"});
        require_english(invariant);
        require(invariant.loaded_catalogs().empty() && invariant.diagnostics().empty(),
                "C/POSIX suppress all 69 new caption translations");
    }
    const auto missing = load(root / "missing", "fixture");
    require_english(missing);
    require(missing.diagnostics().empty(), "missing new-caption catalogs are quiet");
    require_english(UiTranslations{});
    std::cout << "fallback: contextual, missing, empty, language-chain and C/POSIX fallback: ok\n";
}

void failure_case(const fs::path& root) {
    catalog(root, "invalid", mo({
        {key(UiCaption::manager_file_view), std::string("\xc0\xaf", 2U)},
        {key(UiCaption::manager_file_copy), "\033"},
        {key(UiCaption::editor_menu_edit), std::string(UiTranslations::max_label_bytes + 1U, 'A')},
        {key(UiCaption::manager_file_delete), std::string("\xc2\x85", 2U)},
        {key(UiCaption::manager_panel_tree), "Drzewo"},
        {"unlisted-menu\004Copy", "Kopia"},
    }));
    const auto invalid = load(root, "invalid");
    for (const auto id : {UiCaption::manager_file_view, UiCaption::manager_file_copy,
                          UiCaption::editor_menu_edit, UiCaption::manager_file_delete}) {
        require(invalid.caption(id) == mcpp::english_ui_caption(id) && !invalid.translated(id),
                "invalid new captions retain English without publishing their bytes");
    }
    require(invalid.caption(UiCaption::manager_panel_tree) == "Drzewo" &&
            issues(invalid, TranslationIssue::invalid_label) == 4U &&
            issues(invalid, TranslationIssue::skipped_context_entries) == 1U,
            "UTF-8, C0, C1 and length failures are counted while valid siblings remain usable");

    catalog(root, "duplicate", mo({
        {key(UiCaption::manager_file_copy), "Kopia"},
        {key(UiCaption::manager_file_copy), "Kopiuj"},
        {key(UiCaption::manager_panel_tree), "Drzewo"},
    }));
    const auto duplicate = load(root, "duplicate");
    require_english(duplicate);
    require(duplicate.loaded_catalogs().empty() &&
            issues(duplicate, TranslationIssue::invalid_catalog) == 1U,
            "a duplicate supported new key rejects the entire catalog without partial publication");
    catalog(root, "later", mo({{key(UiCaption::manager_file_copy), "Kopiuj"}}));
    const auto chain = UiTranslations::load(root, {"fixture", {}, {}, "duplicate:later"});
    require(chain.caption(UiCaption::manager_file_copy) == "Kopiuj" &&
            chain.caption(UiCaption::manager_panel_tree) == "Tree" &&
            issues(chain, TranslationIssue::invalid_catalog) == 1U,
            "a rejected catalog does not block a later valid new-caption catalog");
    oracle(root, "later", "-", UiCaption::manager_file_copy, "Kopiuj");
    for (const auto id : {UiCaption::count, static_cast<UiCaption>(-1),
                          static_cast<UiCaption>(2147483647)}) {
        require(chain.caption(id).empty() && !chain.translated(id) &&
                mcpp::english_ui_caption(id).empty(), "invalid caption enum values are bounded, not lookup keys");
    }
    std::cout << "failure: invalid new captions, duplicate rejection and enum bounds: ok\n";
}

}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2 || argc == 3, "usage: mcpp_builtin_menu_caption_tests positive|fallback|failure [ROOT]");
        const std::string_view selector(argv[1]);
        require(selector == "positive" || selector == "fallback" || selector == "failure",
                "only the three new bounded model selectors are supported");
        require(::getuid() == 1001U && ::geteuid() == 1001U && fs::is_regular_file("/.dockerenv") &&
                fs::is_regular_file("/review/.operation-regressions-disposable"),
                "new menu-caption fixtures require the approved UID-1001 disposable Docker container");
        fs::path root;
        if (argc == 3) {
            root = argv[2];
        } else {
            const fs::path parent("/review/cases/builtin-menu-caption-ctest");
            ::umask(0077);
            fs::create_directories(parent);
            root = parent / (std::string(selector) + "-" + std::to_string(::getpid()));
        }
        require(root.is_absolute() && root.lexically_normal() == root &&
                root.parent_path().string().starts_with("/review/cases/") &&
                fs::canonical(root.parent_path()) == root.parent_path(),
                "fresh fixture root must have a real parent below /review/cases");
        struct stat parent {};
        require(::stat(root.parent_path().c_str(), &parent) == 0 && S_ISDIR(parent.st_mode) &&
                parent.st_uid == 1001U && (parent.st_mode & 0077U) == 0U,
                "fixture parent must be an owned private directory");
        ::umask(0077);
        require(fs::create_directory(root), "new model fixture root must not already exist");
        if (selector == "positive") positive_case(root);
        else if (selector == "fallback") fallback_case(root);
        else failure_case(root);
        std::cout << "UiTranslations bytes=" << sizeof(UiTranslations)
                  << " alignment=" << alignof(UiTranslations)
                  << " captions=" << mcpp::ui_caption_count << '\n';
        std::cout << "new menu-caption fixtures: " << root << '\n';
    } catch (const std::exception& error) {
        std::cerr << "built-in menu captions: " << error.what() << '\n';
        return 1;
    }
}
