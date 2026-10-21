#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mcpp {

enum class UiLabel {
    left,
    file,
    command,
    options,
    right,
    ok,
    cancel,
    yes,
    no,
    error,
    warning,
    confirmation,
    confirm,
    quit,
    count,
};

inline constexpr auto ui_label_count = static_cast<std::size_t>(UiLabel::count);

[[nodiscard]] std::string_view english_ui_label(UiLabel label) noexcept;

enum class UiCaption {
    delete_confirmation,
    manager_panel_file_listing,
    manager_panel_quick_view,
    manager_panel_info,
    manager_panel_tree,
    manager_panel_listing_format,
    manager_panel_sort_order,
    manager_panel_filter,
    manager_panel_encoding,
    manager_panel_ftp_link,
    manager_panel_shell_link,
    manager_panel_sftp_link,
    manager_panel_panelize,
    manager_panel_rescan,
    manager_file_view,
    manager_file_view_file,
    manager_file_filtered_view,
    manager_file_edit,
    manager_file_copy,
    manager_file_chmod,
    manager_file_link,
    manager_file_symlink,
    manager_file_relative_symlink,
    manager_file_edit_symlink,
    manager_file_chown,
    manager_file_advanced_chown,
    manager_file_chattr,
    manager_file_rename_move,
    manager_file_mkdir,
    manager_file_delete,
    manager_file_quick_cd,
    manager_file_select_group,
    manager_file_unselect_group,
    manager_file_invert_selection,
    manager_file_exit,
    manager_command_user_menu,
    manager_command_directory_tree,
    manager_command_find_file,
    manager_command_swap_panels,
    manager_command_switch_panels,
    manager_command_compare_directories,
    manager_command_compare_files,
    manager_command_external_panelize,
    manager_command_show_directory_sizes,
    manager_command_history,
    manager_command_file_history,
    manager_command_directory_hotlist,
    manager_command_vfs_list,
    manager_command_background_jobs,
    manager_command_screen_list,
    manager_command_undelete_files,
    manager_command_edit_extension_file,
    manager_command_edit_menu_file,
    manager_command_edit_highlighting_file,
    manager_options_configuration,
    manager_options_layout,
    manager_options_panel_options,
    manager_options_confirmation,
    manager_options_appearance,
    manager_options_display_bits,
    manager_options_learn_keys,
    manager_options_virtual_fs,
    manager_options_save_setup,
    editor_menu_file,
    editor_menu_edit,
    editor_menu_search,
    editor_menu_command,
    editor_menu_format,
    editor_menu_window,
    editor_menu_options,
    count,
};

[[nodiscard]] std::string_view english_ui_caption(UiCaption caption) noexcept;

enum class UiCountMessage {
    delete_selected_items,
    count,
};

inline constexpr auto ui_caption_count = static_cast<std::size_t>(UiCaption::count);
inline constexpr auto ui_count_message_count = static_cast<std::size_t>(UiCountMessage::count);

struct TranslationEnvironment {
    std::string lc_all;
    std::string lc_messages;
    std::string lang;
    std::string language;
};

enum class TranslationIssue {
    invalid_preference,
    invalid_domain,
    limit_exceeded,
    unreadable_catalog,
    invalid_catalog,
    unsupported_revision,
    unsupported_charset,
    skipped_plural_entries,
    skipped_context_entries,
    invalid_label,
    invalid_plural_forms,
    invalid_count_format,
    plural_arithmetic_overflow,
    plural_division_by_zero,
    plural_index_out_of_range,
};

struct TranslationDiagnostic {
    TranslationIssue issue;
    std::filesystem::path catalog;
    std::string message;
    std::size_t count{1};
};

struct UiCountText {
    std::string text;
    bool translated{};
    std::optional<TranslationDiagnostic> diagnostic;
};

// Only the built-in enums can be translated. Filenames, resource text and
// messages assembled from user data are deliberately not accepted by this API.
class UiTranslations {
public:
    static constexpr std::size_t max_catalog_bytes = 4U * 1024U * 1024U;
    static constexpr std::size_t max_catalog_entries = 65536U;
    static constexpr std::size_t max_referenced_string_bytes = 8U * 1024U * 1024U;
    static constexpr std::size_t max_metadata_bytes = 65536U;
    static constexpr std::size_t max_label_bytes = 512U;
    static constexpr std::size_t max_locale_bytes = 128U;
    static constexpr std::size_t max_environment_bytes = 1024U;
    static constexpr std::size_t max_language_preferences = 16U;
    static constexpr std::size_t max_catalog_candidates = 64U;
    static constexpr std::size_t max_plural_forms = 16U;
    static constexpr std::size_t max_plural_expression_bytes = 1024U;
    static constexpr std::size_t max_plural_expression_nodes = 256U;
    static constexpr std::size_t max_plural_expression_depth = 64U;

    UiTranslations();

    // Searches root/<locale>/LC_MESSAGES/<domain>.mo. Reads but never changes
    // LC_ALL, LC_MESSAGES, LANG or LANGUAGE; it does not call setlocale().
    // Requested non-C locale names need not be installed on the operating
    // system. LC_ALL > LC_MESSAGES > LANG; C/POSIX (including codeset variants)
    // keep English. Otherwise a nonempty LANGUAGE supplies the preference list
    // instead of that locale, with C/POSIX stopping further fallback.
    [[nodiscard]] static UiTranslations load(
        const std::filesystem::path& root, std::string_view domain = "mcpp");
    [[nodiscard]] static UiTranslations load(
        const std::filesystem::path& root, const TranslationEnvironment& environment,
        std::string_view domain = "mcpp");

    [[nodiscard]] const std::string& text(UiLabel label) const noexcept;
    [[nodiscard]] bool translated(UiLabel label) const noexcept;
    [[nodiscard]] std::size_t translated_labels() const noexcept;
    [[nodiscard]] const std::string& caption(UiCaption caption) const noexcept;
    [[nodiscard]] bool translated(UiCaption caption) const noexcept;

    // These messages own their context and singular/plural originals. Only the
    // decimal count replaces the single literal {count}; no format strings or
    // arbitrary lookup keys are accepted. A missing contextual entry never
    // falls back to a context-free translation. English fallback uses n == 1.
    // Runtime evaluation errors return English plus a diagnostic; callers can
    // report it once per catalog/issue rather than once per render.
    [[nodiscard]] UiCountText count_text(UiCountMessage message, std::uint64_t count) const;
    [[nodiscard]] std::span<const std::filesystem::path> loaded_catalogs() const noexcept;
    [[nodiscard]] std::span<const TranslationDiagnostic> diagnostics() const noexcept;

    // Revision-0 GNU .mo, either byte order. UTF-8 (or declared ASCII) only;
    // missing charset declarations are accepted only as valid UTF-8. Locale
    // aliases are not resolved. Supported contextual/plural keys are fixed
    // built-ins; other keys are skipped with counted diagnostics. Plural-Forms
    // supports GNU decimal constants, n, !, arithmetic, comparisons, &&, || and
    // ?: with short-circuit evaluation. Counts/constants are uint64_t; arithmetic
    // that would wrap (including subtraction below zero) is rejected, not
    // guessed or executed with undefined behavior. Formula byte/node/depth and
    // form-count limits above are enforced. Malformed formulas/form counts
    // reject the entire catalog. Empty translations retain fallback.
    // Invalid UTF-8, C0/C1 controls and overlong labels also retain fallback.
    // Diagnostics are returned to the caller; nothing is printed or installed.
private:
    struct CountData;

    std::array<std::string, ui_label_count> labels_;
    std::array<bool, ui_label_count> translated_{};
    std::array<std::string, ui_caption_count> captions_;
    std::array<bool, ui_caption_count> captions_translated_{};
    std::shared_ptr<const CountData> count_data_;
    std::vector<std::filesystem::path> catalogs_;
    std::vector<TranslationDiagnostic> diagnostics_;
};

}  // namespace mcpp
