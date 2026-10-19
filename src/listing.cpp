#include "mcpp/listing.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace mcpp {
namespace {

struct Definition {
    std::string_view name;
    std::string_view title;
    std::size_t width;
    bool expand;
    ListingAlignment alignment;
};

constexpr auto left = ListingAlignment::left;
constexpr auto right = ListingAlignment::right;
constexpr std::array<Definition, 22> definitions{{
    {"unsorted", "Name", 12, true, left}, {"version", "Name", 12, true, left},
    {"extension", "Name", 12, true, left}, {"name", "Name", 12, true, left},
    {"bsize", "Block Size", 7, false, right}, {"size", "Size", 7, false, right},
    {"type", "", 1, false, left}, {"mtime", "Modify time", 12, false, right},
    {"atime", "Access time", 12, false, right}, {"ctime", "Change time", 12, false, right},
    {"perm", "Permission", 10, false, left}, {"mode", "Perm", 6, false, right},
    {"nlink", "Nl", 2, false, right}, {"inode", "Inode", 5, false, right},
    {"nuid", "UID", 5, false, right}, {"ngid", "GID", 5, false, right},
    {"owner", "Owner", 8, false, left}, {"group", "Group", 8, false, left},
    {"mark", "", 1, false, right}, {"space", "", 1, false, left},
    {"dot", "", 1, false, left}, {"|", "", 1, false, left}
}};

}  // namespace

ListingSpec parse_listing_format(std::string_view format) {
    ListingSpec result;
    const auto skip = [&] {
        while (!format.empty() && (format.front() == ',' ||
               std::isspace(static_cast<unsigned char>(format.front())) != 0)) format.remove_prefix(1);
    };
    skip();
    if (format.starts_with("full") || format.starts_with("half")) {
        result.full_width = format.starts_with("full");
        format.remove_prefix(4);
        skip();
    }
    if (!format.empty() && format.front() >= '0' && format.front() <= '9') {
        result.columns = static_cast<std::size_t>(format.front() - '0');
        if (result.columns == 0U) throw std::runtime_error("listing columns must be 1 through 9");
        format.remove_prefix(1);
    }
    for (;;) {
        skip();
        if (format.empty()) break;
        auto alignment = ListingAlignment::left;
        bool aligned = false;
        if (format.front() == '<' || format.front() == '=' || format.front() == '>') {
            alignment = format.front() == '=' ? ListingAlignment::center
                        : format.front() == '>' ? ListingAlignment::right : ListingAlignment::left;
            aligned = true;
            format.remove_prefix(1);
            skip();
        }
        const auto definition = std::ranges::find_if(definitions, [&](const Definition& field) {
            return format.starts_with(field.name);
        });
        if (definition == definitions.end()) {
            throw std::runtime_error("Unknown tag on display format: " + std::string(format.substr(0, 8)));
        }
        ListingField field{std::string(definition->name), std::string(definition->title),
                           definition->width, definition->expand,
                           aligned ? alignment : definition->alignment};
        format.remove_prefix(definition->name.size());
        skip();
        if (!format.empty() && format.front() == ':') {
            format.remove_prefix(1);
            const auto end = format.find_first_not_of("0123456789");
            const auto digits = format.substr(0, end);
            const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), field.width);
            if (digits.empty() || parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size() ||
                field.width > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
                throw std::runtime_error("Invalid display field width");
            }
            field.expand = false;
            format.remove_prefix(digits.size());
            if (!format.empty() && format.front() == '+') {
                field.expand = true;
                format.remove_prefix(1);
            }
        }
        result.fields.push_back(std::move(field));
    }
    if (result.fields.empty()) throw std::runtime_error("Display format has no fields");
    return result;
}

std::vector<int> listing_field_widths(const ListingSpec& spec, const int width) {
    std::vector<int> result;
    const auto available = std::max(0, width);
    for (const auto& field : spec.fields) {
        result.push_back(static_cast<int>(std::min<std::size_t>(field.width,
                                                static_cast<std::size_t>(available))));
    }
    auto used = std::accumulate(result.begin(), result.end(), std::int64_t{0});
    while (used > available) {
        bool changed = false;
        for (auto& field : result) {
            if (used <= available) break;
            if (field > 1) {
                --field;
                --used;
                changed = true;
            }
        }
        if (!changed) break;
    }
    std::vector<std::size_t> expanding;
    for (std::size_t index = 0; index < spec.fields.size() && expanding.size() < 4U; ++index) {
        if (spec.fields[index].expand) expanding.push_back(index);
    }
    for (std::size_t index = 0; used < available && !expanding.empty(); ++index, ++used) {
        ++result[expanding[index % expanding.size()]];
    }
    return result;
}

std::string listing_format(const ListingFormat format, const std::size_t brief_columns,
                           const std::string& user_format) {
    switch (format) {
        case ListingFormat::full: return "half type name | size | mtime";
        case ListingFormat::brief: return "half " + std::to_string(brief_columns) + " type name";
        case ListingFormat::long_list: return "full perm space nlink space owner space group space size space mtime space name";
        case ListingFormat::custom: return user_format;
    }
    throw std::runtime_error("Invalid listing format");
}

std::string mini_status_format(const ListingFormat format, const std::string& user_format) {
    if (format == ListingFormat::full) return "half type name";
    if (format == ListingFormat::brief) return "half type name space bsize space perm space";
    return listing_format(format, 1, user_format);
}

}  // namespace mcpp
