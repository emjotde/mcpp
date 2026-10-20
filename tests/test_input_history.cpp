#include "mcpp/input_history.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {
namespace fs = std::filesystem;

void check(const bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

template <typename Function>
void rejects(Function function) {
    try {
        function();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error("unsafe or unwritable input history was accepted");
}

std::string read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    input.exceptions(std::ios::badbit);
    check(static_cast<bool>(input), "cannot read input history fixture");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void put(const fs::path& path, const std::string& content) {
    std::ofstream output(path, std::ios::binary);
    output.exceptions(std::ios::badbit | std::ios::failbit);
    output << content;
}

void persistence(const fs::path& root) {
    const auto path = root / "roundtrip/input-history";
    mcpp::InputHistories first(path), second(path);
    first.load();
    check(first.history("directory.change").empty() && !fs::exists(path), "missing history stays absent");
    const std::string quoted = "dir with \\ and \" and \n line Å";
    first.remember("directory.change", quoted);
    second.load();
    second.remember("search.editor", "needle");
    first.remember("directory.change", "recent");
    first.remember("directory.change", quoted);
    first.remember("directory.change", "");
    second.load();
    check(second.history("directory.change") == std::vector<std::string>({"recent", quoted}),
          "values round-trip in unique oldest-to-newest order");
    check(second.history("search.editor") == std::vector<std::string>{"needle"},
          "another instance's different history is preserved");
    struct stat before{};
    check(::stat(path.c_str(), &before) == 0 && (before.st_mode & 0777) == 0600,
          "input history is owner-private");
    first.remove("directory.change", "recent");
    struct stat after{};
    check(::stat(path.c_str(), &after) == 0 && before.st_ino != after.st_ino,
          "updates atomically replace the history file");
    second.clear("search.editor");
    first.load();
    check(first.history("search.editor").empty() &&
          first.history("directory.change") == std::vector<std::string>{quoted},
          "delete and cleanup persist only the selected purpose");
}

void failure_retention(const fs::path& root) {
    const auto path = root / "errors/input-history";
    mcpp::InputHistories history(path);
    history.remember("filename.edit", "keep");
    const auto original = read(path);
    fs::permissions(path.parent_path(), fs::perms::owner_read | fs::perms::owner_exec);
    rejects([&] { history.remember("filename.edit", "draft"); });
    fs::permissions(path.parent_path(), fs::perms::owner_all);
    check(read(path) == original && history.history("filename.edit") == std::vector<std::string>{"keep"},
          "failed saves leave disk and loaded entries unchanged");
    history.remember("filename.edit", "draft");
    const auto saved = read(path);
    put(path, "mcpp-input-history-v1\nfilename.edit\tzz\n");
    const auto malformed = read(path);
    rejects([&] { history.load(); });
    rejects([&] { history.clear("filename.edit"); });
    check(read(path) == malformed && history.history("filename.edit").back() == "draft",
          "malformed files are reported without erasing loaded history");
    put(path, saved);
    fs::permissions(path, fs::perms::owner_read);
    rejects([&] { history.remember("filename.edit", "pending"); });
    check(read(path) == saved, "read-only history is not silently replaced");
    fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read);
    rejects([&] { history.load(); });
    fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write);
    fs::rename(path, path.parent_path() / "victim");
    fs::create_symlink("victim", path);
    rejects([&] { history.remember("filename.edit", "pending"); });
    check(fs::is_symlink(path) && read(path.parent_path() / "victim") == saved,
          "symlink history never changes its target");
}

}  // namespace

int main(const int argc, char** argv) {
    try {
        if (::geteuid() != 1001 || !fs::is_regular_file("/.dockerenv") ||
            !fs::is_regular_file("/review/.operation-regressions-disposable") ||
            argc != 2 || fs::absolute(argv[1]).lexically_normal() != "/review/cases/input-histories") {
            throw std::runtime_error("input history tests require uid1001 and /review/cases/input-histories in the disposable container");
        }
        const auto base = fs::canonical(argv[1]);
        check(base == "/review/cases/input-histories", "input history fixture path is unsafe");
        const auto root = base / ("model-" + std::to_string(::getpid()));
        check(fs::create_directory(root), "input history fixture already exists");
        persistence(root);
        failure_retention(root);
        fs::remove_all(root);
        std::cout << "Input history persistence and failure-retention passed (2 cases)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
