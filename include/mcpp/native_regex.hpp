#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string_view>

namespace mcpp {

struct NativeRegexMatch {
    std::size_t offset;
    std::size_t length;
};

class NativeRegex {
public:
    explicit NativeRegex(std::string_view pattern, bool ignore_case = false);

    [[nodiscard]] std::optional<NativeRegexMatch> find(
        std::string_view subject, std::size_t start = 0
    ) const;
    [[nodiscard]] bool search(std::string_view subject) const;

private:
    struct Compiled;
    std::shared_ptr<const Compiled> compiled_;
};

}  // namespace mcpp
