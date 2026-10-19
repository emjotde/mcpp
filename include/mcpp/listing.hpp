#pragma once

#include "mcpp/entry.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace mcpp {

enum class ListingAlignment { left, center, right };

struct ListingField {
    std::string name;
    std::string title;
    std::size_t width{0};
    bool expand{false};
    ListingAlignment alignment{ListingAlignment::left};
};

struct ListingSpec {
    bool full_width{false};
    std::size_t columns{1};
    std::vector<ListingField> fields;
};

[[nodiscard]] ListingSpec parse_listing_format(std::string_view format);
[[nodiscard]] std::vector<int> listing_field_widths(const ListingSpec& spec, int width);
[[nodiscard]] std::string listing_format(ListingFormat format, std::size_t brief_columns,
                                         const std::string& user_format);
[[nodiscard]] std::string mini_status_format(ListingFormat format, const std::string& user_format);

}  // namespace mcpp
