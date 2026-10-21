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

constexpr std::size_t first_editor_item = 70U;
static_assert(static_cast<std::size_t>(UiCaption::delete_confirmation) == 0U);
static_assert(static_cast<std::size_t>(UiCaption::manager_panel_file_listing) == 1U);
static_assert(static_cast<std::size_t>(UiCaption::manager_options_save_setup) == 62U);
static_assert(static_cast<std::size_t>(UiCaption::editor_menu_file) == 63U);
static_assert(static_cast<std::size_t>(UiCaption::editor_menu_options) == 69U);
static_assert(static_cast<std::size_t>(UiCaption::editor_file_open_file) == first_editor_item);
static_assert(mcpp::ui_caption_count == 138U);
static_assert(!std::is_invocable_v<decltype(&UiTranslations::caption),
                                 const UiTranslations&, std::string_view>);
static_assert(!std::is_invocable_v<decltype(&UiTranslations::caption),
                                 const UiTranslations&, unsigned>);
static_assert(!std::is_invocable_v<decltype(&mcpp::english_ui_caption), std::string_view>);

constexpr auto editor_item_ids = std::to_array<UiCaption>({
    UiCaption::editor_file_open_file, UiCaption::editor_file_new,
    UiCaption::editor_file_close, UiCaption::editor_file_history,
    UiCaption::editor_file_save, UiCaption::editor_file_save_as,
    UiCaption::editor_file_insert_file, UiCaption::editor_file_copy_to_file,
    UiCaption::editor_file_user_menu, UiCaption::editor_file_about, UiCaption::editor_file_quit,
    UiCaption::editor_edit_undo, UiCaption::editor_edit_redo,
    UiCaption::editor_edit_toggle_ins_overw, UiCaption::editor_edit_toggle_mark,
    UiCaption::editor_edit_mark_columns, UiCaption::editor_edit_mark_all, UiCaption::editor_edit_unmark,
    UiCaption::editor_edit_copy, UiCaption::editor_edit_move, UiCaption::editor_edit_delete,
    UiCaption::editor_edit_copy_to_clipfile, UiCaption::editor_edit_cut_to_clipfile,
    UiCaption::editor_edit_paste_from_clipfile, UiCaption::editor_edit_beginning, UiCaption::editor_edit_end,
    UiCaption::editor_search_search, UiCaption::editor_search_search_again, UiCaption::editor_search_replace,
    UiCaption::editor_search_toggle_bookmark, UiCaption::editor_search_next_bookmark,
    UiCaption::editor_search_prev_bookmark, UiCaption::editor_search_flush_bookmarks,
    UiCaption::editor_command_go_to_line, UiCaption::editor_command_toggle_line_state,
    UiCaption::editor_command_go_to_matching_bracket, UiCaption::editor_command_toggle_syntax_highlighting,
    UiCaption::editor_command_find_declaration, UiCaption::editor_command_back_from_declaration,
    UiCaption::editor_command_forward_to_declaration, UiCaption::editor_command_encoding,
    UiCaption::editor_command_refresh_screen, UiCaption::editor_command_start_stop_record_macro,
    UiCaption::editor_command_delete_macro, UiCaption::editor_command_record_repeat_actions,
    UiCaption::editor_command_spell_check, UiCaption::editor_command_check_word,
    UiCaption::editor_command_change_spelling_language, UiCaption::editor_command_mail,
    UiCaption::editor_format_insert_literal, UiCaption::editor_format_insert_date_time,
    UiCaption::editor_format_format_paragraph, UiCaption::editor_format_sort,
    UiCaption::editor_format_paste_output_of, UiCaption::editor_format_external_formatter,
    UiCaption::editor_window_move, UiCaption::editor_window_resize,
    UiCaption::editor_window_toggle_fullscreen, UiCaption::editor_window_next,
    UiCaption::editor_window_previous, UiCaption::editor_window_list,
    UiCaption::editor_options_general, UiCaption::editor_options_save_mode,
    UiCaption::editor_options_learn_keys, UiCaption::editor_options_syntax_highlighting,
    UiCaption::editor_options_syntax_file, UiCaption::editor_options_menu_file,
    UiCaption::editor_options_save_setup,
});
static_assert(editor_item_ids.size() == 68U);

struct SourceRow {
    std::string_view label;
    int action;
    char mnemonic{};
};

constexpr auto source_file_rows = std::to_array<SourceRow>({
    {"Open file...", 0, 'o'},
    {"New              C-n", 1, 'n'},
    {"Close", 2, 'c'},
    {"History...       M-E", 5, 'h'},
    {"", -2},
    {"Save             F2", 3, 's'},
    {"Save as...       F12", 4, 'a'},
    {"", -2},
    {"Insert file...   F15", 7, 'i'},
    {"Copy to file...  C-f", 8, 'y'},
    {"", -2},
    {"User menu...     F11", 9, 'u'},
    {"", -2},
    {"About...", 10, 'b'},
    {"", -2},
    {"Quit             F10", 6, 'q'},
});

constexpr auto source_edit_rows = std::to_array<SourceRow>({
    {"Undo                 C-u", 0, 'u'},
    {"Redo                 M-r", 1, 'r'},
    {"", -2},
    {"Toggle ins/overw     Ins", 2, 't'},
    {"", -2},
    {"Toggle mark          F3", 3, 'g'},
    {"Mark columns         F13", 4, 'm'},
    {"Mark all", 5, 'a'},
    {"Unmark", 6, 'k'},
    {"", -2},
    {"Copy                 F5", 7, 'y'},
    {"Move                 F6", 8, 'v'},
    {"Delete               F8", 9, 'd'},
    {"", -2},
    {"Copy to clipfile     C-Ins", 10, 'p'},
    {"Cut to clipfile      S-Del", 11, 'c'},
    {"Paste from clipfile  S-Ins", 12, 's'},
    {"", -2},
    {"Beginning            C-Home", 13, 'b'},
    {"End                  C-End", 14, 'e'},
});

constexpr auto source_search_rows = std::to_array<SourceRow>({
    {"Search...        F7", 0, 's'},
    {"Search again     F17", 1, 'a'},
    {"Replace...       F4", 2, 'r'},
    {"", -2},
    {"Toggle bookmark  M-k", 3, 't'},
    {"Next bookmark    M-j", 4, 'n'},
    {"Prev bookmark    M-i", 5, 'p'},
    {"Flush bookmarks  M-o", 6, 'f'},
});

constexpr auto source_command_rows = std::to_array<SourceRow>({
    {"Go to line...                M-l", 0, 'g'},
    {"Toggle line state            M-n", 2, 't'},
    {"Go to matching bracket       M-b", 1, 'b'},
    {"Toggle syntax highlighting   C-s", 3, 'y'},
    {"", -2},
    {"Find declaration             M-Enter", 4, 'f'},
    {"Back from declaration        M--", 5, 'd'},
    {"Forward to declaration       M-+", 6, 'w'},
    {"", -2},
    {"Encoding...                  M-e", 7, 'i'},
    {"", -2},
    {"Refresh screen               C-l", 8, 'r'},
    {"", -2},
    {"Start/Stop record macro      C-r", 9, 's'},
    {"Delete macro...", 10, 'o'},
    {"Record/Repeat actions", 11, 'a'},
    {"", -2},
    {"Spell check", 13, 'p'},
    {"Check word                   C-p", 14, 'h'},
    {"Change spelling language...", 15, 'l'},
    {"", -2},
    {"Mail...", 12, 'm'},
});

constexpr auto source_format_rows = std::to_array<SourceRow>({
    {"Insert literal...   C-q", 0, 'l'},
    {"Insert date/time", 1, 'd'},
    {"", -2},
    {"Format paragraph    M-p", 2, 'f'},
    {"Sort...             M-t", 3, 's'},
    {"Paste output of...  M-u", 4, 'p'},
    {"External formatter", 5, 'e'},
});

constexpr auto source_window_rows = std::to_array<SourceRow>({
    {"Move", 3, 'm'},
    {"Resize", 4, 'r'},
    {"Toggle fullscreen", 5, 't'},
    {"", -2},
    {"Next", 0, 'n'},
    {"Previous", 1, 'p'},
    {"List...", 2, 'l'},
});

constexpr auto source_options_rows = std::to_array<SourceRow>({
    {"General...", 0, 'g'},
    {"Save mode...", 2, 'm'},
    {"Learn keys...", 3, 'k'},
    {"Syntax highlighting...", 4, 'h'},
    {"", -2},
    {"Syntax file", 5, 'y'},
    {"Menu file", 6, 'm'},
    {"", -2},
    {"Save setup", 1, 's'},
});

constexpr std::array<std::span<const SourceRow>, 7> source_editor_rows{
    source_file_rows, source_edit_rows, source_search_rows, source_command_rows,
    source_format_rows, source_window_rows, source_options_rows,
};
constexpr std::array<std::string_view, 7> contexts{
    "editor-file-menu", "editor-edit-menu", "editor-search-menu", "editor-command-menu",
    "editor-format-menu", "editor-window-menu", "editor-options-menu",
};

consteval bool matches_source_rows() {
    if (detail::builtin_editor_dropdown_captions.size() != source_editor_rows.size()) return false;
    auto next = first_editor_item;
    for (std::size_t category = 0U; category < source_editor_rows.size(); ++category) {
        const auto source = source_editor_rows[category];
        const auto captions = detail::builtin_editor_dropdown_captions[category];
        if (source.size() != captions.size()) return false;
        for (std::size_t row = 0U; row < source.size(); ++row) {
            const auto& caption = captions[row];
            if (source[row].action == -2) {
                if (caption.id != UiCaption::count || !caption.shortcut.empty() ||
                    !source[row].label.empty() || source[row].mnemonic != 0) return false;
                continue;
            }
            if (next >= mcpp::ui_caption_count ||
                caption.id != editor_item_ids[next - first_editor_item] ||
                static_cast<std::size_t>(caption.id) != next++) return false;
            const auto& message = detail::builtin_caption_messages[
                static_cast<std::size_t>(caption.id)];
            const auto separator = message.key.find('\004');
            const auto gap = source[row].label.find("  ");
            if (message.id != caption.id || message.key.substr(0U, separator) != contexts[category] ||
                message.english() != source[row].label.substr(0U, gap) ||
                caption.shortcut != (gap == std::string_view::npos
                    ? std::string_view{} : source[row].label.substr(gap))) return false;
        }
    }
    return next == mcpp::ui_caption_count;
}

static_assert(matches_source_rows());

void require(const bool condition, const std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

void word(Bytes& bytes, const std::size_t offset, const std::uint32_t value, const bool big) {
    for (std::size_t index = 0U; index < 4U; ++index) {
        const auto shift = static_cast<unsigned>((big ? 3U - index : index) * 8U);
        bytes[offset + index] = static_cast<unsigned char>((value >> shift) & 0xffU);
    }
}

// Original fixtures use the public revision-0 GNU .mo layout, not installed catalogs.
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
    require(static_cast<bool>(output), "write original editor-caption catalog");
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
    for (auto index = first_editor_item; index < mcpp::ui_caption_count; ++index) {
        const auto& message = detail::builtin_caption_messages[index];
        require(translations.caption(message.id) == message.english() &&
                !translations.translated(message.id), "editor dropdown retains exact English fallback");
    }
}

std::string fixture_translation(const UiCaption caption) {
    switch (caption) {
        case UiCaption::editor_file_open_file: return "Ouvrir café";
        case UiCaption::editor_file_save: return "Żółć é";
        case UiCaption::editor_edit_move: return "Déplacer bloc";
        case UiCaption::editor_window_move: return "Fenêtre 移動";
        case UiCaption::editor_search_search: return "検索";
        case UiCaption::editor_command_encoding: return "编码";
        case UiCaption::editor_format_insert_literal: return "Wstaw 🧪";
        case UiCaption::editor_options_save_setup: return "Save setup";
        default: return "Élément " + std::to_string(static_cast<std::size_t>(caption));
    }
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
    require(static_cast<bool>(output), "write std-Python gettext oracle");
}

void source_oracle(const fs::path& root) {
    std::ofstream output(root / "source-editor-rows.tsv");
    for (std::size_t category = 0U; category < source_editor_rows.size(); ++category) {
        const auto rows = source_editor_rows[category];
        for (std::size_t row = 0U; row < rows.size(); ++row) {
            output << category << '\t' << row << '\t' << rows[row].label << '\t'
                   << rows[row].action << '\t'
                   << static_cast<unsigned>(static_cast<unsigned char>(rows[row].mnemonic)) << '\n';
        }
    }
    require(static_cast<bool>(output), "write immutable English editor-row oracle");
}

void positive_case(const fs::path& root) {
    Messages entries;
    for (auto index = first_editor_item; index < mcpp::ui_caption_count; ++index) {
        const auto& message = detail::builtin_caption_messages[index];
        entries.emplace_back(message.key, fixture_translation(message.id));
    }
    for (const bool big : {false, true}) {
        const std::string_view locale = big ? "editor-be" : "editor-le";
        catalog(root, locale, mo(entries, big));
        const auto translations = load(root, locale);
        require(translations.loaded_catalogs().size() == 1U && translations.diagnostics().empty(),
                "all editor dropdown keys load without skipped contexts in either byte order");
        for (auto index = first_editor_item; index < mcpp::ui_caption_count; ++index) {
            const auto& message = detail::builtin_caption_messages[index];
            const auto expected = fixture_translation(message.id);
            require(translations.translated(message.id) && translations.caption(message.id) == expected &&
                    mcpp::english_ui_caption(message.id) == message.english(),
                    "typed editor item selects its exact contextual UTF-8 caption");
            oracle(root, locale, "-", message.id, expected);
        }
    }
    source_oracle(root);
    std::cout << "positive: 68 typed dropdown captions, seven row maps, UTF-8 and byte orders: ok\n";
}

void fallback_case(const fs::path& root) {
    catalog(root, "first", mo({
        {key(UiCaption::editor_edit_move), ""},
        {key(UiCaption::editor_window_move), "Fenêtre"},
        {key(UiCaption::editor_file_save), "Enregistrer"},
        {"Move", "Sans contexte"},
        {"other-editor-menu\004Move", "Mauvais contexte"},
    }));
    catalog(root, "later", mo({
        {key(UiCaption::editor_edit_move), "Déplacer bloc"},
        {key(UiCaption::editor_window_move), "Autre fenêtre"},
        {key(UiCaption::editor_file_copy_to_file), "Copier dans un fichier"},
    }));
    const auto first = load(root, "first");
    require(first.caption(UiCaption::editor_edit_move) == "Move" &&
            !first.translated(UiCaption::editor_edit_move) &&
            first.caption(UiCaption::editor_window_move) == "Fenêtre",
            "an empty editor entry never borrows the same English text from another context");
    oracle(root, "first", "-", UiCaption::editor_edit_move, "Move");
    oracle(root, "first", "-", UiCaption::editor_window_move, "Fenêtre");
    const auto chain = UiTranslations::load(root, {"fixture", {}, {}, "first:later:C"});
    require(chain.caption(UiCaption::editor_edit_move) == "Déplacer bloc" &&
            chain.caption(UiCaption::editor_window_move) == "Fenêtre" &&
            chain.caption(UiCaption::editor_file_copy_to_file) == "Copier dans un fichier" &&
            chain.caption(UiCaption::editor_file_save) == "Enregistrer",
            "missing and empty items use later languages without replacing earlier items");
    oracle(root, "first", "later", UiCaption::editor_edit_move, "Déplacer bloc");
    oracle(root, "first", "later", UiCaption::editor_window_move, "Fenêtre");
    oracle(root, "first", "later", UiCaption::editor_file_copy_to_file, "Copier dans un fichier");
    const auto invariant = UiTranslations::load(root, {"C.UTF-8", {}, {}, "first:later"});
    require_english(invariant);
    require(invariant.loaded_catalogs().empty() && invariant.diagnostics().empty(),
            "the invariant locale suppresses editor dropdown translations");
    const auto missing = load(root / "missing", "fixture");
    require_english(missing);
    require(missing.diagnostics().empty(), "a missing editor catalog is quiet");
    require_english(UiTranslations{});
    std::cout << "fallback: exact contexts, empty/missing entries and invariant locale: ok\n";
}

void failure_case(const fs::path& root) {
    catalog(root, "invalid", mo({
        {key(UiCaption::editor_file_open_file), std::string("\xc0\xaf", 2U)},
        {key(UiCaption::editor_edit_copy), "\033"},
        {key(UiCaption::editor_search_replace), std::string(UiTranslations::max_label_bytes + 1U, 'A')},
        {key(UiCaption::editor_window_move), std::string("\xc2\x85", 2U)},
        {key(UiCaption::editor_format_insert_literal), "Wstaw 🧪"},
        {"unlisted-editor-menu\004Copy", "Copier"},
    }));
    const auto invalid = load(root, "invalid");
    for (const auto id : {UiCaption::editor_file_open_file, UiCaption::editor_edit_copy,
                          UiCaption::editor_search_replace, UiCaption::editor_window_move}) {
        require(invalid.caption(id) == mcpp::english_ui_caption(id) && !invalid.translated(id),
                "invalid editor bytes retain the original English caption");
    }
    require(invalid.caption(UiCaption::editor_format_insert_literal) == "Wstaw 🧪" &&
            issues(invalid, TranslationIssue::invalid_label) == 4U &&
            issues(invalid, TranslationIssue::skipped_context_entries) == 1U,
            "invalid editor labels are counted without hiding a valid sibling");
    // GNUTranslations rejects invalid UTF-8 at load time; only the valid catalog is an oracle.
    catalog(root, "duplicate", mo({
        {key(UiCaption::editor_edit_copy), "Copier"},
        {key(UiCaption::editor_edit_copy), "Copie"},
    }));
    const auto duplicate = load(root, "duplicate");
    require_english(duplicate);
    require(duplicate.loaded_catalogs().empty() &&
            issues(duplicate, TranslationIssue::invalid_catalog) == 1U,
            "duplicate supported editor keys reject the catalog without partial publication");
    catalog(root, "later", mo({{key(UiCaption::editor_edit_copy), "Copier"}}));
    const auto chain = UiTranslations::load(root, {"fixture", {}, {}, "duplicate:later"});
    require(chain.caption(UiCaption::editor_edit_copy) == "Copier" &&
            issues(chain, TranslationIssue::invalid_catalog) == 1U,
            "a rejected editor catalog does not block a later valid catalog");
    oracle(root, "later", "-", UiCaption::editor_edit_copy, "Copier");
    for (const auto id : {UiCaption::count, static_cast<UiCaption>(-1),
                          static_cast<UiCaption>(2147483647)}) {
        require(chain.caption(id).empty() && !chain.translated(id) &&
                mcpp::english_ui_caption(id).empty(), "invalid enum values are not lookup keys");
    }
    std::cout << "failure: invalid UTF-8/control/length, duplicate keys and enum bounds: ok\n";
}

}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2 || argc == 3,
                "usage: mcpp_editor_menu_caption_tests positive|fallback|failure [ROOT]");
        const std::string_view selector(argv[1]);
        require(selector == "positive" || selector == "fallback" || selector == "failure",
                "only three bounded editor-caption selectors are supported");
        require(::getuid() == 1001U && ::geteuid() == 1001U && fs::is_regular_file("/.dockerenv") &&
                fs::is_regular_file("/review/.operation-regressions-disposable"),
                "requires the approved UID-1001 disposable Docker container");
        fs::path root;
        if (argc == 3) {
            root = argv[2];
        } else {
            const fs::path parent("/review/cases/editor-menu-caption-ctest");
            ::umask(0077);
            fs::create_directories(parent);
            root = parent / (std::string(selector) + "-" + std::to_string(::getpid()));
        }
        require(root.is_absolute() && root.lexically_normal() == root &&
                root.parent_path().string().starts_with("/review/cases/") &&
                fs::canonical(root.parent_path()) == root.parent_path(),
                "fixture root must have a real parent below /review/cases");
        struct stat parent {};
        require(::stat(root.parent_path().c_str(), &parent) == 0 && S_ISDIR(parent.st_mode) &&
                parent.st_uid == 1001U && (parent.st_mode & 0077U) == 0U,
                "fixture parent must be an owned private directory");
        ::umask(0077);
        require(fs::create_directory(root), "editor-caption fixture root must be fresh");
        if (selector == "positive") positive_case(root);
        else if (selector == "fallback") fallback_case(root);
        else failure_case(root);
        std::cout << "UiTranslations bytes=" << sizeof(UiTranslations)
                  << " alignment=" << alignof(UiTranslations)
                  << " captions=" << mcpp::ui_caption_count << '\n';
        std::cout << "editor-caption fixtures: " << root << '\n';
    } catch (const std::exception& error) {
        std::cerr << "editor menu captions: " << error.what() << '\n';
        return 1;
    }
}
