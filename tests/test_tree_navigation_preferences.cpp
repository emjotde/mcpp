#include "mcpp/config.hpp"
#include "tree_navigation.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;
using mcpp::detail::tree_selection_changes_location;
using mcpp::detail::tree_selection_follows;

void check(const bool condition, const std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

void write(const fs::path& path, const std::string_view contents) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path);
    output << contents;
    check(static_cast<bool>(output), "write original tree preference fixture");
}

std::string read(const fs::path& path) {
    std::ifstream input(path);
    check(static_cast<bool>(input), "read original tree preference fixture");
    return {std::istreambuf_iterator<char>(input), {}};
}

void configuration(const fs::path& root) {
    const auto native = root / "native/ini";
    write(native, "# original tree setting\n[Midnight-Commander]\nxtree_mode=0\n");
    auto config = mcpp::Config::load(native);
    check(config.xtree_mode == false && config.diagnostics.empty(),
          "native false must be a supported tree preference");
    config.save();
    check(mcpp::Config::load(native).xtree_mode == false &&
              read(native).find("# original tree setting") != std::string::npos,
          "native save must retain explicit false and existing comments");
    config.xtree_mode = true;
    config.save();
    check(mcpp::Config::load(native).xtree_mode == true,
          "native save must retain explicit true");

    const auto flat = root / "flat/config";
    write(flat, "xtree_mode=true\n");
    config = mcpp::Config::load(flat);
    check(config.xtree_mode == true && config.diagnostics.empty(),
          "flat true must be a supported tree preference");
    config.xtree_mode = false;
    config.save();
    check(mcpp::Config::load(flat).xtree_mode == false,
          "flat save must retain explicit false");

    const auto absent = root / "absent/ini";
    write(absent, "[Midnight-Commander]\nconfirm_exit=false\n");
    config = mcpp::Config::load(absent);
    check(!config.xtree_mode, "old profiles must leave the tree preference absent");
    config.save();
    check(!mcpp::Config::load(absent).xtree_mode &&
              read(absent).find("xtree_mode=") == std::string::npos,
          "saving an old profile must not enable native tree following");

    for (const auto contents : {
             "[Midnight-Commander]\nxtree_mode=2\n", "xtree_mode=maybe\n"}) {
        const auto invalid = root / "invalid/config";
        write(invalid, contents);
        bool rejected = false;
        try {
            static_cast<void>(mcpp::Config::load(invalid));
        } catch (const std::exception& error) {
            rejected = std::string_view(error.what()).find("invalid boolean") !=
                       std::string_view::npos;
        }
        check(rejected && read(invalid) == contents,
              "invalid tree booleans must fail without changing the profile");
    }
}

void follow_decisions() {
    for (const bool dynamic : {false, true}) {
        check(tree_selection_follows(std::nullopt, dynamic) == dynamic,
              "absent setting must retain legacy dynamic following");
        check(!tree_selection_follows(false, dynamic),
              "explicit false must disable following in either F4 mode");
        check(tree_selection_follows(true, dynamic),
              "explicit true must enable following independently of F4");
    }
    check(!tree_selection_changes_location(true, "/kept", "/kept"),
          "same provider and path must not reset listing cursor or marks");
    check(tree_selection_changes_location(true, "/before", "/after"),
          "a new tree directory must be followed");
    check(tree_selection_changes_location(false, "/", "/"),
          "equal path strings must not conflate local and virtual providers");
}

}  // namespace

int main(const int argc, char** argv) {
    try {
        check(argc == 2 && ::geteuid() == 1001U && fs::exists("/.dockerenv") &&
                  fs::exists("/review/.operation-regressions-disposable"),
              "tree preference tests require UID1001 in the owned review container");
        const auto root = fs::path(argv[1]).lexically_normal();
        check(root.is_relative() &&
                  fs::absolute(root).lexically_normal().string().starts_with(
                      "/review/cases/xtree-fd3eff6e/model/"),
              "use an owned tree preference model directory");
        ::umask(0077);
        fs::create_directories(root);
        check(!fs::is_symlink(root), "model directory must not be a symlink");
        configuration(root);
        follow_decisions();
        std::cout << "tree native/flat/absent/invalid preferences and following decisions: passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
