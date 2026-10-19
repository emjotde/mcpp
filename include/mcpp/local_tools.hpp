#pragma once

#include "mcpp/cancellation.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
#include <utility>

namespace mcpp {

struct CommandOutput {
    int status{0};
    std::string output;
    std::string error;
};

struct Codepage {
    std::string id;
    std::string name;
};

[[nodiscard]] const std::vector<Codepage>& supported_codepages();

struct Declaration {
    std::string path;
    std::size_t line{1};
};

[[nodiscard]] std::vector<Declaration> find_declarations(
    const std::filesystem::path& source, std::string_view word,
    const Cancellation& cancellation = {}
);
[[nodiscard]] std::vector<std::pair<std::string, std::string>> read_local_rules(
    const std::filesystem::path& path
);

class TemporaryFile {
public:
    explicit TemporaryFile(std::string_view value);
    ~TemporaryFile();
    TemporaryFile(const TemporaryFile&) = delete;
    TemporaryFile& operator=(const TemporaryFile&) = delete;
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
private:
    std::filesystem::path path_;
};

[[nodiscard]] CommandOutput capture_command(
    const std::vector<std::string>& arguments,
    const std::filesystem::path& directory,
    const Cancellation& cancellation = {},
    std::size_t limit = 64U * 1024U * 1024U
);
[[nodiscard]] CommandOutput capture_shell(
    std::string_view command,
    const std::filesystem::path& directory,
    const Cancellation& cancellation = {},
    std::size_t limit = 64U * 1024U * 1024U
);
[[nodiscard]] std::string convert_encoding(
    std::string_view value, const std::string& source, const std::string& destination
);
[[nodiscard]] unsigned int local_file_flags(const std::filesystem::path& path);
void set_local_file_flags(const std::filesystem::path& path, unsigned int flags);

}  // namespace mcpp
