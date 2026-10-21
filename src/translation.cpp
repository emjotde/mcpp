#include "mcpp/translation.hpp"

#include "builtin_menu_captions.hpp"
#include "translation_plural.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace mcpp {
namespace {

constexpr std::array<std::string_view, ui_label_count> english_labels{
    "Left", "File", "Command", "Options", "Right", "OK", "Cancel",
    "Yes", "No", "Error", "Warning", "Confirmation", "Confirm", "Quit",
};

struct CountMessage {
    std::string_view key;
    std::string_view singular;
    std::string_view plural;
};

constexpr std::array<CountMessage, ui_count_message_count> count_messages{{
    {"delete-dialog\004Delete {count} selected item?",
     "Delete {count} selected item?", "Delete {count} selected items?"},
}};

using Diagnostics = std::vector<TranslationDiagnostic>;

void diagnose(
    Diagnostics& diagnostics, const TranslationIssue issue,
    const std::filesystem::path& catalog, const std::string_view message,
    const std::size_t count = 1U
) {
    diagnostics.push_back({issue, catalog, std::string(message), count});
}

bool ascii_alnum(const char value) {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9');
}

std::string lowercase(const std::string_view text) {
    std::string result(text);
    for (auto& value : result) {
        if (value >= 'A' && value <= 'Z') value = static_cast<char>(value - 'A' + 'a');
    }
    return result;
}

std::string_view trim(std::string_view text) {
    constexpr std::string_view whitespace = " \t\r";
    const auto first = text.find_first_not_of(whitespace);
    if (first == std::string_view::npos) return {};
    text.remove_prefix(first);
    text.remove_suffix(text.size() - text.find_last_not_of(whitespace) - 1U);
    return text;
}

bool component(const std::string_view value) {
    return !value.empty() && std::all_of(value.begin(), value.end(), [](const char ch) {
        return ascii_alnum(ch) || ch == '-' || ch == '_';
    });
}

struct Locale {
    std::string language;
    std::string territory;
    std::string codeset;
    std::string modifier;
};

std::optional<Locale> parse_locale(std::string_view value) {
    if (value.empty() || value.size() > UiTranslations::max_locale_bytes) return std::nullopt;
    Locale result;
    const auto modifier = value.find('@');
    if (modifier != std::string_view::npos) {
        result.modifier = value.substr(modifier + 1U);
        if (!component(result.modifier)) return std::nullopt;
        value = value.substr(0, modifier);
    }
    const auto codeset = value.find('.');
    if (codeset != std::string_view::npos) {
        result.codeset = value.substr(codeset + 1U);
        if (!component(result.codeset)) return std::nullopt;
        value = value.substr(0, codeset);
    }
    const auto territory = value.find('_');
    result.language = value.substr(0, territory);
    if (!component(result.language)) return std::nullopt;
    if (territory != std::string_view::npos) {
        result.territory = value.substr(territory + 1U);
        if (!component(result.territory)) return std::nullopt;
    }
    return result;
}

bool invariant_locale(const Locale& locale) {
    return locale.language == "C" || locale.language == "POSIX";
}

std::string normalized_codeset(const std::string_view codeset) {
    std::string result;
    bool only_digits = true;
    for (const auto ch : lowercase(codeset)) {
        if (!ascii_alnum(ch)) continue;
        result += ch;
        only_digits = only_digits && ch >= '0' && ch <= '9';
    }
    if (only_digits && !result.empty()) result.insert(0, "iso");
    return result;
}

std::vector<std::string> locale_candidates(
    const TranslationEnvironment& environment, Diagnostics& diagnostics
) {
    const std::string_view selected = !environment.lc_all.empty() ? environment.lc_all :
        !environment.lc_messages.empty() ? environment.lc_messages :
        !environment.lang.empty() ? std::string_view(environment.lang) : std::string_view("C");
    const auto active = parse_locale(selected);
    if (!active) {
        diagnose(diagnostics, TranslationIssue::invalid_preference, {},
                 "Invalid message locale; keeping English labels.");
        return {};
    }
    if (invariant_locale(*active)) return {};
    if (environment.language.size() > UiTranslations::max_environment_bytes) {
        diagnose(diagnostics, TranslationIssue::limit_exceeded, {},
                 "LANGUAGE exceeds the supported byte limit; keeping English labels.");
        return {};
    }

    std::vector<std::string> result;
    const auto append = [&result, &diagnostics](std::string candidate) {
        if (std::find(result.begin(), result.end(), candidate) != result.end()) return true;
        if (result.size() == UiTranslations::max_catalog_candidates) {
            diagnose(diagnostics, TranslationIssue::limit_exceeded, {},
                     "Locale candidate limit reached; later fallbacks were not searched.");
            return false;
        }
        result.push_back(std::move(candidate));
        return true;
    };
    const auto expand = [&append](const Locale& locale) {
        std::vector<std::string> codesets;
        if (!locale.codeset.empty()) {
            codesets.push_back("." + locale.codeset);
            const auto normalized = "." + normalized_codeset(locale.codeset);
            if (normalized != codesets.front()) codesets.push_back(normalized);
        }
        codesets.emplace_back();
        for (const bool with_modifier : {true, false}) {
            if (with_modifier && locale.modifier.empty()) continue;
            for (const bool with_territory : {true, false}) {
                if (with_territory && locale.territory.empty()) continue;
                for (const auto& codeset : codesets) {
                    auto name = locale.language;
                    if (with_territory) name += "_" + locale.territory;
                    name += codeset;
                    if (with_modifier) name += "@" + locale.modifier;
                    if (!append(std::move(name))) return false;
                }
            }
        }
        return true;
    };

    if (environment.language.empty()) {
        expand(*active);
        return result;
    }
    std::string_view remaining = environment.language;
    std::size_t preferences = 0;
    while (!remaining.empty()) {
        const auto separator = remaining.find(':');
        const auto name = remaining.substr(0, separator);
        if (!name.empty()) {
            if (++preferences > UiTranslations::max_language_preferences) {
                diagnose(diagnostics, TranslationIssue::limit_exceeded, {},
                         "LANGUAGE preference limit reached; later fallbacks were not searched.");
                break;
            }
            const auto locale = parse_locale(name);
            if (!locale) {
                diagnose(diagnostics, TranslationIssue::invalid_preference, {},
                         "Ignored an invalid LANGUAGE locale name.");
            } else {
                if (invariant_locale(*locale)) break;
                if (!expand(*locale)) break;
            }
        }
        if (separator == std::string_view::npos) break;
        remaining.remove_prefix(separator + 1U);
    }
    return result;
}

std::string environment_value(const char* name) {
    const auto* value = std::getenv(name);
    if (value == nullptr) return {};
    std::size_t length = 0;
    while (length <= UiTranslations::max_environment_bytes && value[length] != '\0') ++length;
    return std::string(value, length);
}

std::optional<std::vector<unsigned char>> read_catalog(
    const std::filesystem::path& path, Diagnostics& diagnostics
) {
    const auto fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        const auto error = errno;
        if (error != ENOENT && error != ENOTDIR) {
            diagnose(diagnostics, TranslationIssue::unreadable_catalog, path,
                     std::error_code(error, std::generic_category()).message());
        }
        return std::nullopt;
    }
    struct File {
        int descriptor;
        ~File() { ::close(descriptor); }
    } file{fd};
    struct stat status {};
    if (::fstat(fd, &status) != 0 || !S_ISREG(status.st_mode) || status.st_size < 0) {
        diagnose(diagnostics, TranslationIssue::unreadable_catalog, path,
                 "Catalog must be a readable regular file.");
        return std::nullopt;
    }
    if (static_cast<std::uintmax_t>(status.st_size) > UiTranslations::max_catalog_bytes) {
        diagnose(diagnostics, TranslationIssue::limit_exceeded, path,
                 "Catalog exceeds the supported byte limit.");
        return std::nullopt;
    }
    std::vector<unsigned char> data(static_cast<std::size_t>(status.st_size));
    std::size_t offset = 0;
    while (offset < data.size()) {
        const auto received = ::read(fd, data.data() + offset, data.size() - offset);
        if (received < 0 && errno == EINTR) continue;
        if (received <= 0) {
            diagnose(diagnostics, TranslationIssue::unreadable_catalog, path,
                     "Could not read the complete catalog.");
            return std::nullopt;
        }
        offset += static_cast<std::size_t>(received);
    }
    return data;
}

class MoView {
public:
    MoView(const std::vector<unsigned char>& bytes, const bool big_endian)
        : bytes_(bytes), big_endian_(big_endian) {}

    std::uint32_t word(const std::size_t offset) const {
        std::uint32_t result = 0;
        for (std::size_t index = 0; index < 4U; ++index) {
            const auto shift = static_cast<unsigned>((big_endian_ ? 3U - index : index) * 8U);
            result |= static_cast<std::uint32_t>(bytes_[offset + index]) << shift;
        }
        return result;
    }

    bool table(const std::size_t offset, const std::size_t count) const {
        return offset <= bytes_.size() && count <= (bytes_.size() - offset) / 8U;
    }

    std::optional<std::string_view> entry(const std::size_t row) const {
        const auto size = static_cast<std::size_t>(word(row));
        const auto offset = static_cast<std::size_t>(word(row + 4U));
        if (offset >= bytes_.size() || size >= bytes_.size() - offset ||
            bytes_[offset + size] != 0U) return std::nullopt;
        return std::string_view(reinterpret_cast<const char*>(bytes_.data() + offset), size);
    }

private:
    const std::vector<unsigned char>& bytes_;
    bool big_endian_;
};

enum class Encoding { utf8, ascii, unsupported };

Encoding encoding(const std::string_view header) {
    std::string_view remaining = header;
    while (!remaining.empty()) {
        const auto newline = remaining.find('\n');
        auto line = remaining.substr(0, newline);
        const auto colon = line.find(':');
        if (colon != std::string_view::npos &&
            lowercase(trim(line.substr(0, colon))) == "content-type") {
            line.remove_prefix(colon + 1U);
            while (!line.empty()) {
                const auto semicolon = line.find(';');
                const auto field = line.substr(0, semicolon);
                const auto equals = field.find('=');
                if (equals != std::string_view::npos &&
                    lowercase(trim(field.substr(0, equals))) == "charset") {
                    auto charset = trim(field.substr(equals + 1U));
                    if (charset.size() >= 2U && charset.front() == '"' && charset.back() == '"') {
                        charset = charset.substr(1U, charset.size() - 2U);
                    }
                    const auto name = lowercase(charset);
                    if (name == "utf-8" || name == "utf8") return Encoding::utf8;
                    if (name == "us-ascii" || name == "ascii") return Encoding::ascii;
                    return Encoding::unsupported;
                }
                if (semicolon == std::string_view::npos) break;
                line.remove_prefix(semicolon + 1U);
            }
        }
        if (newline == std::string_view::npos) break;
        remaining.remove_prefix(newline + 1U);
    }
    return Encoding::utf8;
}

bool valid_label(const std::string_view label, const Encoding charset) {
    if (label.size() > UiTranslations::max_label_bytes) return false;
    std::size_t offset = 0;
    while (offset < label.size()) {
        const auto first = static_cast<unsigned char>(label[offset++]);
        std::uint32_t value = first;
        std::size_t following = 0;
        std::uint32_t minimum = 0;
        if (first >= 0x80U) {
            if (charset == Encoding::ascii) return false;
            if (first >= 0xc2U && first <= 0xdfU) {
                value = first & 0x1fU; following = 1U; minimum = 0x80U;
            } else if (first >= 0xe0U && first <= 0xefU) {
                value = first & 0x0fU; following = 2U; minimum = 0x800U;
            } else if (first >= 0xf0U && first <= 0xf4U) {
                value = first & 0x07U; following = 3U; minimum = 0x10000U;
            } else {
                return false;
            }
            if (following > label.size() - offset) return false;
            for (std::size_t index = 0; index < following; ++index) {
                const auto next = static_cast<unsigned char>(label[offset++]);
                if (next < 0x80U || next > 0xbfU) return false;
                value = (value << 6U) | (next & 0x3fU);
            }
            if (value < minimum || value > 0x10ffffU ||
                (value >= 0xd800U && value <= 0xdfffU)) return false;
        }
        if (value < 0x20U || (value >= 0x7fU && value <= 0x9fU)) return false;
    }
    return true;
}

using Labels = std::array<std::string, ui_label_count>;
using Captions = std::array<std::string, ui_caption_count>;

struct PluralForms {
    std::size_t count;
    detail::PluralExpression expression;
};

TranslationDiagnostic plural_diagnostic(
    const detail::PluralError error, const std::filesystem::path& path
) {
    using detail::PluralError;
    switch (error) {
        case PluralError::limit:
            return {TranslationIssue::limit_exceeded, path,
                    "Plural-Forms exceeds the supported byte, node or nesting limit."};
        case PluralError::overflow:
            return {TranslationIssue::plural_arithmetic_overflow, path,
                    "Plural-Forms arithmetic is outside the supported uint64_t range."};
        case PluralError::division_by_zero:
            return {TranslationIssue::plural_division_by_zero, path,
                    "Plural-Forms evaluated division or remainder by zero."};
        case PluralError::index:
            return {TranslationIssue::plural_index_out_of_range, path,
                    "Plural-Forms evaluated an index outside nplurals."};
        default:
            return {TranslationIssue::invalid_plural_forms, path,
                    "Invalid GNU Plural-Forms expression."};
    }
}

bool parse_plural_forms(
    const std::string_view header, const std::filesystem::path& path,
    Diagnostics& diagnostics, std::optional<PluralForms>& result
) {
    std::optional<std::string_view> declaration;
    auto remaining = header;
    while (!remaining.empty()) {
        const auto newline = remaining.find('\n');
        const auto line = remaining.substr(0, newline);
        const auto colon = line.find(':');
        if (colon != std::string_view::npos &&
            lowercase(trim(line.substr(0, colon))) == "plural-forms") {
            if (declaration) {
                diagnose(diagnostics, TranslationIssue::invalid_plural_forms, path,
                         "Duplicate Plural-Forms metadata.");
                return false;
            }
            declaration = trim(line.substr(colon + 1U));
        }
        if (newline == std::string_view::npos) break;
        remaining.remove_prefix(newline + 1U);
    }
    if (!declaration) return true;
    std::optional<std::string_view> form_count;
    std::optional<std::string_view> expression;
    remaining = *declaration;
    const auto invalid = [&diagnostics, &path]() {
        diagnose(diagnostics, TranslationIssue::invalid_plural_forms, path,
                 "Plural-Forms requires one nplurals and one plural expression.");
        return false;
    };
    while (!remaining.empty()) {
        const auto semicolon = remaining.find(';');
        const auto field = trim(remaining.substr(0, semicolon));
        if (!field.empty()) {
            const auto equals = field.find('=');
            if (equals == std::string_view::npos) return invalid();
            const auto name = trim(field.substr(0, equals));
            const auto value = trim(field.substr(equals + 1U));
            if (name == "nplurals" && !form_count) form_count = value;
            else if (name == "plural" && !expression) expression = value;
            else return invalid();
        }
        if (semicolon == std::string_view::npos) break;
        remaining.remove_prefix(semicolon + 1U);
    }
    if (!form_count || form_count->empty() || !expression || expression->empty()) return invalid();
    std::size_t count = 0U;
    for (const char character : *form_count) {
        if (character < '0' || character > '9') return invalid();
        const auto digit = static_cast<std::size_t>(character - '0');
        if (count > (UiTranslations::max_plural_forms - digit) / 10U ||
            digit > UiTranslations::max_plural_forms) {
            diagnose(diagnostics, TranslationIssue::limit_exceeded, path,
                     "nplurals exceeds the supported form-count limit.");
            return false;
        }
        count = count * 10U + digit;
    }
    if (count == 0U || count > UiTranslations::max_plural_forms) return invalid();
    detail::PluralError error{};
    auto parsed = detail::PluralExpression::parse(*expression, error);
    if (!parsed) {
        diagnostics.push_back(plural_diagnostic(error, path));
        return false;
    }
    if (!parsed->uses_count()) {
        const auto value = parsed->evaluate(0U, count);
        if (value.error != detail::PluralError::none) {
            diagnostics.push_back(plural_diagnostic(value.error, path));
            return false;
        }
    }
    result = PluralForms{count, std::move(*parsed)};
    return true;
}

bool valid_count_format(const std::string_view text) {
    constexpr std::string_view token = "{count}";
    const auto marker = text.find(token);
    return marker != std::string_view::npos &&
           text.substr(0U, marker).find_first_of("{}") == std::string_view::npos &&
           text.substr(marker + token.size()).find_first_of("{}") == std::string_view::npos;
}

std::string substitute_count(const std::string_view text, const std::uint64_t count) {
    constexpr std::string_view token = "{count}";
    const auto marker = text.find(token);
    std::string result(text.substr(0U, marker));
    result += std::to_string(count);
    result += text.substr(marker + token.size());
    return result;
}

struct DecodedCatalog {
    Labels labels;
    Captions captions;
    std::array<std::vector<std::string>, ui_count_message_count> counts;
    std::optional<PluralForms> plural;
};

std::optional<DecodedCatalog> decode_catalog(
    const std::filesystem::path& path, const std::vector<unsigned char>& data,
    Diagnostics& diagnostics
) {
    const auto invalid = [&diagnostics, &path](const std::string_view message) {
        diagnose(diagnostics, TranslationIssue::invalid_catalog, path, message);
        return std::nullopt;
    };
    if (data.size() < 28U) return invalid("Truncated GNU .mo header.");
    const auto magic = MoView(data, false).word(0);
    if (magic != 0x950412deU && magic != 0xde120495U) return invalid("Not a GNU .mo catalog.");
    const MoView view(data, magic == 0xde120495U);
    if (view.word(4U) != 0U) {
        diagnose(diagnostics, TranslationIssue::unsupported_revision, path,
                 "Only revision-0 GNU .mo catalogs are supported.");
        return std::nullopt;
    }
    const auto count = static_cast<std::size_t>(view.word(8U));
    if (count > UiTranslations::max_catalog_entries) {
        diagnose(diagnostics, TranslationIssue::limit_exceeded, path,
                 "Catalog exceeds the supported entry limit.");
        return std::nullopt;
    }
    const auto originals = static_cast<std::size_t>(view.word(12U));
    const auto translations = static_cast<std::size_t>(view.word(16U));
    if (!view.table(originals, count) || !view.table(translations, count)) {
        return invalid("GNU .mo string table is outside the catalog.");
    }
    std::optional<std::string_view> header;
    std::size_t referenced_bytes = 0;
    for (std::size_t index = 0; index < count; ++index) {
        const auto original = view.entry(originals + index * 8U);
        const auto translation = view.entry(translations + index * 8U);
        if (!original || !translation) return invalid("Invalid GNU .mo string extent or terminator.");
        const auto bytes = original->size() + translation->size();
        if (bytes > UiTranslations::max_referenced_string_bytes - referenced_bytes) {
            diagnose(diagnostics, TranslationIssue::limit_exceeded, path,
                     "Catalog string references exceed the supported byte budget.");
            return std::nullopt;
        }
        referenced_bytes += bytes;
        if (original->empty()) {
            if (header || translation->size() > UiTranslations::max_metadata_bytes ||
                translation->find('\0') != std::string_view::npos) {
                return invalid("Invalid or duplicate GNU .mo metadata.");
            }
            header = *translation;
        }
    }
    const auto charset = encoding(header.value_or(std::string_view{}));
    if (charset == Encoding::unsupported) {
        diagnose(diagnostics, TranslationIssue::unsupported_charset, path,
                 "Only UTF-8 and ASCII catalogs are supported; no charset conversion is performed.");
        return std::nullopt;
    }
    DecodedCatalog result;
    if (!parse_plural_forms(header.value_or(std::string_view{}), path, diagnostics, result.plural)) {
        return std::nullopt;
    }
    std::array<bool, ui_label_count> seen{};
    std::array<bool, ui_caption_count> seen_captions{};
    std::array<bool, ui_count_message_count> seen_counts{};
    std::size_t plurals = 0;
    std::size_t contexts = 0;
    std::size_t invalid_labels = 0;
    for (std::size_t index = 0; index < count; ++index) {
        const auto original = *view.entry(originals + index * 8U);
        const auto translation = *view.entry(translations + index * 8U);
        const auto separator = original.find('\0');
        if (separator != std::string_view::npos) {
            const auto key = original.substr(0U, separator);
            const auto found = std::find_if(count_messages.begin(), count_messages.end(),
                [key](const auto& message) { return message.key == key; });
            if (found == count_messages.end()) { ++plurals; continue; }
            const auto message = static_cast<std::size_t>(found - count_messages.begin());
            if (seen_counts[message]) return invalid("Duplicate built-in plural message in GNU .mo catalog.");
            seen_counts[message] = true;
            if (separator + 1U == original.size() ||
                original.find('\0', separator + 1U) != std::string_view::npos) {
                return invalid("Invalid GNU .mo plural original.");
            }
            if (!result.plural) {
                diagnose(diagnostics, TranslationIssue::invalid_plural_forms, path,
                         "A built-in plural translation requires explicit Plural-Forms metadata.");
                return std::nullopt;
            }
            std::vector<std::string> forms;
            auto remaining = translation;
            std::size_t invalid_forms = 0U;
            std::size_t invalid_formats = 0U;
            for (;;) {
                const auto next = remaining.find('\0');
                const auto form = remaining.substr(0U, next);
                if (!valid_label(form, charset)) ++invalid_forms;
                else if (!form.empty() && !valid_count_format(form)) ++invalid_formats;
                forms.emplace_back(form);
                if (next == std::string_view::npos || forms.size() > result.plural->count) break;
                remaining.remove_prefix(next + 1U);
            }
            if (forms.size() != result.plural->count ||
                (forms.size() == result.plural->count && remaining.find('\0') != std::string_view::npos)) {
                diagnose(diagnostics, TranslationIssue::invalid_plural_forms, path,
                         "The number of translated plural forms does not equal nplurals.");
                return std::nullopt;
            }
            if (invalid_formats != 0U) {
                diagnose(diagnostics, TranslationIssue::invalid_count_format, path,
                         "Each nonempty count form must contain exactly one {count} and no other braces.",
                         invalid_formats);
                return std::nullopt;
            }
            invalid_labels += invalid_forms;
            if (invalid_forms == 0U) result.counts[message] = std::move(forms);
            continue;
        }
        if (original.find('\004') != std::string_view::npos) {
            const auto found = std::find_if(
                detail::builtin_caption_messages.begin(), detail::builtin_caption_messages.end(),
                [original](const auto& message) { return message.key == original; });
            if (found == detail::builtin_caption_messages.end()) { ++contexts; continue; }
            const auto caption = static_cast<std::size_t>(found->id);
            if (seen_captions[caption]) return invalid("Duplicate built-in caption in GNU .mo catalog.");
            seen_captions[caption] = true;
            if (!valid_label(translation, charset)) { ++invalid_labels; continue; }
            result.captions[caption] = translation;
            continue;
        }
        const auto found = std::find(english_labels.begin(), english_labels.end(), original);
        if (found == english_labels.end()) continue;
        const auto label = static_cast<std::size_t>(found - english_labels.begin());
        if (seen[label]) return invalid("Duplicate built-in label in GNU .mo catalog.");
        seen[label] = true;
        if (!valid_label(translation, charset)) { ++invalid_labels; continue; }
        result.labels[label] = translation;
    }
    if (plurals != 0U) diagnose(diagnostics, TranslationIssue::skipped_plural_entries, path,
        "Plural entries outside the fixed built-in message set were skipped.", plurals);
    if (contexts != 0U) diagnose(diagnostics, TranslationIssue::skipped_context_entries, path,
        "Contextual entries outside the fixed built-in caption set were skipped.", contexts);
    if (invalid_labels != 0U) diagnose(diagnostics, TranslationIssue::invalid_label, path,
        "Labels with invalid encoding, control characters or excessive length were skipped.", invalid_labels);
    return result;
}

}  // namespace

struct UiTranslations::CountData {
    struct Catalog {
        std::filesystem::path path;
        detail::PluralExpression expression;
        std::vector<std::string> forms;
    };

    std::array<std::vector<Catalog>, ui_count_message_count> messages;
};

std::string_view english_ui_label(const UiLabel label) noexcept {
    const auto index = static_cast<std::size_t>(label);
    return index < english_labels.size() ? english_labels[index] : std::string_view{};
}

std::string_view english_ui_caption(const UiCaption caption) noexcept {
    return detail::builtin_english_caption(caption);
}

UiTranslations::UiTranslations() {
    std::copy(english_labels.begin(), english_labels.end(), labels_.begin());
    for (const auto& message : detail::builtin_caption_messages) {
        captions_[static_cast<std::size_t>(message.id)] = message.english();
    }
}

UiTranslations UiTranslations::load(
    const std::filesystem::path& root, const std::string_view domain
) {
    return load(root, {
        environment_value("LC_ALL"), environment_value("LC_MESSAGES"),
        environment_value("LANG"), environment_value("LANGUAGE"),
    }, domain);
}

UiTranslations UiTranslations::load(
    const std::filesystem::path& root, const TranslationEnvironment& environment,
    const std::string_view domain
) {
    UiTranslations result;
    if (domain.empty() || domain.size() > 64U || domain == "." || domain == ".." ||
        !std::all_of(domain.begin(), domain.end(), [](const char value) {
            return ascii_alnum(value) || value == '_' || value == '-' || value == '.';
        })) {
        diagnose(result.diagnostics_, TranslationIssue::invalid_domain, {},
                 "Catalog domain must be a single filename component of at most 64 bytes.");
        return result;
    }
    if (root.empty()) return result;
    if (root.native().find('\0') != std::string::npos) {
        diagnose(result.diagnostics_, TranslationIssue::unreadable_catalog, root,
                 "Catalog directory contains a NUL byte.");
        return result;
    }
    const auto candidates = locale_candidates(environment, result.diagnostics_);
    auto count_data = std::make_shared<CountData>();
    for (const auto& locale : candidates) {
        const auto path = root / locale / "LC_MESSAGES" / (std::string(domain) + ".mo");
        const auto bytes = read_catalog(path, result.diagnostics_);
        if (!bytes) continue;
        auto decoded = decode_catalog(path, *bytes, result.diagnostics_);
        if (!decoded) continue;
        result.catalogs_.push_back(path);
        for (std::size_t index = 0; index < ui_label_count; ++index) {
            if (result.translated_[index] || decoded->labels[index].empty()) continue;
            result.labels_[index] = std::move(decoded->labels[index]);
            result.translated_[index] = true;
        }
        for (std::size_t index = 0U; index < ui_caption_count; ++index) {
            if (result.captions_translated_[index] || decoded->captions[index].empty()) continue;
            result.captions_[index] = std::move(decoded->captions[index]);
            result.captions_translated_[index] = true;
        }
        for (std::size_t index = 0U; index < ui_count_message_count; ++index) {
            if (decoded->counts[index].empty()) continue;
            count_data->messages[index].push_back({
                path, decoded->plural->expression, std::move(decoded->counts[index]),
            });
        }
    }
    result.count_data_ = std::move(count_data);
    return result;
}

const std::string& UiTranslations::text(const UiLabel label) const noexcept {
    const auto index = static_cast<std::size_t>(label);
    static const std::string empty;
    return index < labels_.size() ? labels_[index] : empty;
}

bool UiTranslations::translated(const UiLabel label) const noexcept {
    const auto index = static_cast<std::size_t>(label);
    return index < translated_.size() && translated_[index];
}

std::size_t UiTranslations::translated_labels() const noexcept {
    return static_cast<std::size_t>(std::count(translated_.begin(), translated_.end(), true));
}

const std::string& UiTranslations::caption(const UiCaption caption) const noexcept {
    const auto index = static_cast<std::size_t>(caption);
    static const std::string empty;
    return index < captions_.size() ? captions_[index] : empty;
}

bool UiTranslations::translated(const UiCaption caption) const noexcept {
    const auto index = static_cast<std::size_t>(caption);
    return index < captions_translated_.size() && captions_translated_[index];
}

UiCountText UiTranslations::count_text(
    const UiCountMessage message, const std::uint64_t count
) const {
    const auto index = static_cast<std::size_t>(message);
    if (index >= count_messages.size()) {
        return {{}, false, TranslationDiagnostic{
            TranslationIssue::invalid_label, {}, "Invalid built-in count message.",
        }};
    }
    const auto& original = count_messages[index];
    UiCountText result{
        substitute_count(count == 1U ? original.singular : original.plural, count),
        false, std::nullopt,
    };
    if (!count_data_) return result;
    for (const auto& catalog : count_data_->messages[index]) {
        const auto value = catalog.expression.evaluate(count, catalog.forms.size());
        if (value.error != detail::PluralError::none) {
            result.diagnostic = plural_diagnostic(value.error, catalog.path);
            return result;
        }
        const auto& form = catalog.forms[static_cast<std::size_t>(value.value)];
        if (form.empty()) continue;
        result.text = substitute_count(form, count);
        result.translated = true;
        return result;
    }
    return result;
}

std::span<const std::filesystem::path> UiTranslations::loaded_catalogs() const noexcept {
    return catalogs_;
}

std::span<const TranslationDiagnostic> UiTranslations::diagnostics() const noexcept {
    return diagnostics_;
}

}  // namespace mcpp
