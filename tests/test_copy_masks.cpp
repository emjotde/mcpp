#include "mcpp/local_provider.hpp"
#include "mcpp/operations.hpp"

#include <clocale>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

void check(const bool condition, const std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

void write_file(const fs::path& path, const std::string_view bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << bytes;
    check(output.good(), "fixture write failed");
}

std::string read_file(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    check(input.good(), "fixture read failed");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void masks(const fs::path& root) {
    check(mcpp::CopyMask("*.*").transform("Report.TXT", R"(\L\u\2.\1)") == "Txt.report",
          "case conversion or numbered shell groups are wrong");
    check(mcpp::CopyMask("file-?.txt").transform("file-A.txt", "*.bak") == "A.bak",
          "question-mark group was not substituted");
    check(mcpp::CopyMask("[[:alpha:]]-?.txt").transform("x-A.txt", "*.bak") == "A.bak",
          "source character class changed wildcard grouping");
    check(mcpp::CopyMask(R"(^([^.]+)\.(.*)$)", false).transform("report.TXT", R"(\2.\1)") == "TXT.report",
          "regular-expression groups were not substituted");
    check(mcpp::CopyMask("*").transform("one", R"(escaped\ name\*)") == "escaped name*" &&
          mcpp::CopyMask("*").transform("one", "\"quote name\"") == "\"quote name\"",
          "literal destination quoting changed");
    bool rejected = false;
    try {
        static_cast<void>(mcpp::CopyMask("*").transform("one", R"(\9)"));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected, "missing source group was accepted");

    fs::create_directories(root / "source");
    fs::create_directories(root / "copy");
    fs::create_directories(root / "moved");
    write_file(root / "source/alpha.txt", "MASK\n");
    fs::create_hard_link(root / "source/alpha.txt", root / "source/beta.txt");
    write_file(root / "source/keep.bin", "KEEP\n");
    mcpp::LocalProvider local(root);
    mcpp::OperationEngine engine;
    mcpp::OperationOptions options;
    options.source_mask = "*.txt";
    options.destination_mask = "*-copy.dat";
    mcpp::OperationResult result;
    engine.copy(local, local.list("source", {}, {}), local, local.normalize("copy"), options, {}, {}, &result);
    check(read_file(root / "copy/alpha-copy.dat") == "MASK\n" &&
          read_file(root / "copy/beta-copy.dat") == "MASK\n" &&
          !fs::exists(root / "copy/keep.bin"), "copy mask did not transform only matching sources");
    check(local.same_file("copy/alpha-copy.dat", local, "copy/beta-copy.dat"),
          "transformation broke a hardlink group");
    check(result.completed_sources.size() == 2U && result.skipped.empty(), "filtered copy result is wrong");
    options.shell_patterns = false;
    options.source_mask = R"(^(.*)-copy[.]dat$)";
    options.destination_mask = R"(\1.done)";
    result = {};
    engine.move(local, local.list("copy", {}, {}), local, local.normalize("moved"), options, {}, {}, &result);
    check(read_file(root / "moved/alpha.done") == "MASK\n" &&
          !fs::exists(root / "copy/alpha-copy.dat") &&
          local.same_file("moved/alpha.done", local, "moved/beta.done"),
          "rename mask lost data or hardlinks");
    check(result.completed_sources.size() == 2U && result.skipped.empty(), "masked rename result is wrong");
}

void placement(const fs::path& root) {
    fs::create_directories(root / "source/tree");
    write_file(root / "source/tree/leaf", "leaf");
    for (const auto* name : {"merge", "dive", "moved"}) {
        fs::create_directories(root / name / "tree");
        write_file(root / name / "tree/old", "old");
    }
    mcpp::LocalProvider local(root);
    mcpp::OperationEngine engine;
    mcpp::OperationOptions options;
    options.overwrite = true;
    const auto source = local.stat("source/tree", {});
    engine.copy(local, {source}, local, local.normalize("merge"), options, {});
    check(read_file(root / "merge/tree/leaf") == "leaf" &&
          !fs::exists(root / "merge/tree/tree"), "default directory placement did not merge");
    options.dive_into_subdirectories = true;
    engine.copy(local, {source}, local, local.normalize("dive"), options, {});
    check(read_file(root / "dive/tree/tree/leaf") == "leaf" &&
          !fs::exists(root / "dive/tree/leaf"), "dive did not add the native extra source directory");
    engine.move(local, {source}, local, local.normalize("moved"), options, {});
    check(read_file(root / "moved/tree/tree/leaf") == "leaf" &&
          !fs::exists(root / "source/tree"), "move did not honor directory placement");
}

void links(const fs::path& root) {
    fs::create_directories(root / "source");
    fs::create_directories(root / "destination/deep");
    write_file(root / "data", "target");
    fs::create_symlink("../data", root / "source/link");
    fs::create_symlink(root / "data", root / "source/absolute");
    fs::create_symlink("branch/../missing", root / "source/dangling");
    mcpp::LocalProvider source(root / "source");
    mcpp::LocalProvider destination(root / "destination");
    mcpp::OperationEngine engine;
    mcpp::OperationOptions options;
    options.stable_symlinks = true;
    engine.copy(source, source.list(".", {}, {}), destination, destination.normalize("deep"), options, {});
    check(fs::read_symlink(root / "destination/deep/link") == root / "source/../data" &&
          read_file(root / "destination/deep/link") == "target",
          "stable link did not keep its original referent across provider roots");
    check(fs::read_symlink(root / "destination/deep/absolute") == root / "data" &&
          fs::read_symlink(root / "destination/deep/dangling") == root / "source/branch/../missing",
          "stable copying followed or normalized a link target");
    engine.move_exact(source, source.stat("link", {}), destination, destination.normalize("moved"), options, {});
    check(!source.exists("link") && read_file(root / "destination/moved") == "target",
          "stable link move used an unchanged relative target");

    const auto stale = source.stat("dangling", {});
    fs::remove(root / "source/dangling");
    fs::create_symlink("changed-target", root / "source/dangling");
    options.error = [](const auto&, const auto&) { return mcpp::OperationErrorDecision::skip; };
    mcpp::OperationResult result;
    engine.copy_exact(source, stale, destination, destination.normalize("stale"), options, {}, {}, &result);
    check(!destination.exists("stale") && result.skipped.size() == 1U && result.completed_sources.empty(),
          "changed symlink source was published");
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        check(!entry.path().filename().string().starts_with(".mcpp-"), "symlink copy left staged output");
    }
}

}  // namespace

int main(const int argc, char** argv) {
    try {
        check(geteuid() == 1001 && fs::exists("/.dockerenv") &&
              fs::exists("/review/.operation-regressions-disposable") &&
              fs::current_path() == "/review/cases/copy-masks",
              "run only as UID1001 in the existing copy-masks Docker fixture directory");
        check(argc == 2, "expected one case: masks, placement or links");
        const std::string selected(argv[1]);
        check(selected == "masks" || selected == "placement" || selected == "links", "unknown copy-mask case");
        static_cast<void>(std::setlocale(LC_ALL, ""));
        const auto root = fs::current_path() / ("model-" + selected);
        fs::remove_all(root);
        fs::create_directory(root);
        if (selected == "masks") masks(root);
        else if (selected == "placement") placement(root);
        else links(root);
        std::cout << "PASS copy-masks " << selected << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
