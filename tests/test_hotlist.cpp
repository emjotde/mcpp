#include "mcpp/hotlist.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace {
namespace fs = std::filesystem;

void check(const bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

template <typename Function>
void rejects(Function function, const std::string& message) {
    try {
        function();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error(message);
}

std::string read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot read hotlist fixture");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void put(const fs::path& path, const std::string& content) {
    std::ofstream output(path, std::ios::binary);
    output.exceptions(std::ios::badbit | std::ios::failbit);
    output << content;
}

void persistence_and_groups(const fs::path& root) {
    const auto path = root / "hotlist";
    const auto uri = (root / "bundle.tar").string() + "/utar://documents";
    put(path, "# user comment\nGROUP \"Work \\\"quoted\\\"\"\n"
              "  ENTRY \"Archive \\\\ files\" URL \"" + uri + "\"\nENDGROUP\n");
    auto hotlist = mcpp::DirectoryHotlist::load(path, {"/legacy", uri});
    check(hotlist.group({}).size() == 3U && hotlist.group({1}).size() == 1U,
          "native groups and comments load; legacy bookmarks merge without duplicates");
    check(hotlist.group({1})[0].directory == uri, "archive URI is kept as a complete location");
    const auto group = hotlist.insert({}, {mcpp::HotlistKind::group, "Second", {}, {}}, 1);
    check(group == 1U, "Insert is before the selected item");
    const auto moved = hotlist.move({2, 0}, {1});
    check(moved == mcpp::HotlistPath({1, 0}) && hotlist.group({2}).empty(), "entries move across groups");
    const auto before_cycle = hotlist.serialize();
    rejects([&] { hotlist.move({1}, {1}); }, "moving a group into itself must fail");
    check(hotlist.serialize() == before_cycle, "invalid moves retain the complete tree");
    hotlist.erase({3});
    hotlist.save(path);
    auto reloaded = mcpp::DirectoryHotlist::load(path, {"/legacy", uri});
    check(reloaded.serialize() == hotlist.serialize() && reloaded.bookmarks() == std::vector<std::string>{uri},
          "native-format persistence retains names and deleted legacy entries do not reappear");
    check(read(path).find("# user comment") != std::string::npos, "public-format comments survive saving");
}

void failed_persistence_retains_data(const fs::path& root) {
    const auto path = root / "protected/hotlist";
    fs::create_directory(path.parent_path());
    put(path, "ENTRY \"Keep\" URL \"/keep\"\n");
    auto hotlist = mcpp::DirectoryHotlist::load(path);
    hotlist.insert({}, {mcpp::HotlistKind::entry, "Pending", "/pending", {}});
    const auto draft = hotlist.serialize();
    const auto original = read(path);
    fs::permissions(path.parent_path(), fs::perms::owner_read | fs::perms::owner_exec);
    rejects([&] { hotlist.save(path); }, "an unwritable hotlist directory must fail");
    fs::permissions(path.parent_path(), fs::perms::owner_all);
    check(hotlist.serialize() == draft && read(path) == original, "failed saves retain the draft and original file");
    hotlist.save(path);
    check(read(path) == draft, "a retained draft can be saved after the error is corrected");
    put(path, "ENTRY \"External change\" URL \"/external\"\n");
    rejects([&] { hotlist.save(path); }, "external changes cannot be silently overwritten");
    check(read(path).find("External change") != std::string::npos && hotlist.serialize() == draft,
          "external edits and unsaved in-memory data both survive a conflict");
    put(root / "malformed", "GROUP \"Unclosed\"\nENTRY \"Keep\" URL \"/keep\"\n");
    const auto malformed = read(root / "malformed");
    rejects([&] { static_cast<void>(mcpp::DirectoryHotlist::load(root / "malformed")); },
            "malformed native data must not be accepted for destructive rewriting");
    check(read(root / "malformed") == malformed, "malformed source data is not modified by loading");
}
}  // namespace

int main(const int argc, char** argv) {
    try {
        if (geteuid() != 1001 || !fs::is_regular_file("/.dockerenv") ||
            !fs::is_regular_file("/review/.operation-regressions-disposable")) {
            throw std::runtime_error("hotlist tests require the owned uid1001 disposable container");
        }
        if (argc != 2 || fs::absolute(argv[1]).lexically_normal() != "/review/cases/hierarchical-hotlists") {
            throw std::runtime_error("expected /review/cases/hierarchical-hotlists");
        }
        const auto base = fs::absolute(argv[1]).lexically_normal();
        fs::create_directories(base);
        if (fs::is_symlink(fs::symlink_status(base))) throw std::runtime_error("hotlist fixture is a symlink");
        const auto root = base / ("model-" + std::to_string(getpid()));
        check(fs::create_directory(root), "hotlist model fixture already exists");
        persistence_and_groups(root);
        failed_persistence_retains_data(root);
        fs::remove_all(root);
        std::cout << "Hotlist persistence/navigation and failure-retention cases passed (2 cases)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
