#pragma once

#include <array>
#include <cstddef>
#include <filesystem>
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
};

struct TranslationDiagnostic {
    TranslationIssue issue;
    std::filesystem::path catalog;
    std::string message;
    std::size_t count{1};
};

// Only the labels above can be translated; filenames, resource text and messages
// assembled from user data are deliberately not accepted by this API.
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
    [[nodiscard]] std::span<const std::filesystem::path> loaded_catalogs() const noexcept;
    [[nodiscard]] std::span<const TranslationDiagnostic> diagnostics() const noexcept;

    // Revision-0 GNU .mo, either byte order. UTF-8 (or declared ASCII) only;
    // missing charset declarations are accepted only as valid UTF-8. Locale
    // aliases are not resolved. Plural-Forms is never evaluated: plural and
    // contextual entries are skipped with counted diagnostics, not reduced to
    // a possibly incorrect singular form. Empty translations retain fallback.
    // Invalid UTF-8, C0/C1 controls and overlong labels also retain fallback.
    // Diagnostics are returned to the caller; nothing is printed or installed.
private:
    std::array<std::string, ui_label_count> labels_;
    std::array<bool, ui_label_count> translated_{};
    std::vector<std::filesystem::path> catalogs_;
    std::vector<TranslationDiagnostic> diagnostics_;
};

}  // namespace mcpp
