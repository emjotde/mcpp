#pragma once

#include <cstddef>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace mcpp {

class InputHistories {
public:
    static constexpr std::size_t default_limit = 100U;
    explicit InputHistories(std::filesystem::path path, std::size_t limit = default_limit);

    void load();
    [[nodiscard]] const std::vector<std::string>& history(std::string_view id) const;
    void remember(std::string_view id, std::string_view value);
    void remove(std::string_view id, std::string_view value);
    void clear(std::string_view id);

private:
    enum class Change { remember, remove, clear };
    using Histories = std::map<std::string, std::vector<std::string>, std::less<>>;

    void update(std::string_view id, std::string_view value, Change change);

    std::filesystem::path path_;
    std::size_t limit_;
    Histories histories_;
};

}  // namespace mcpp
