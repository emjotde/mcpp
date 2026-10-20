#include "mcpp/local_provider.hpp"
#include "mcpp/operations.hpp"

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

void put(const fs::path& path) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path);
    output << "keep\n";
    check(output.good(), "fixture write failed");
}

void individual(const fs::path& root) {
    put(root / "keep/unreadable/file");
    put(root / "remove/nested/file");
    put(root / "outside/file");
    fs::create_directory_symlink(root / "outside", root / "remove/link");
    fs::permissions(root / "keep/unreadable", fs::perms::none);
    mcpp::LocalProvider local(root);
    mcpp::OperationResult result;
    unsigned int prompts = 0;
    mcpp::OperationEngine{}.remove(
        local, {local.stat("keep", {}), local.stat("remove", {})}, {}, {}, &result,
        [](const auto&, const auto& message) -> mcpp::OperationErrorDecision {
            throw std::runtime_error("refused directory was traversed: " + message);
        }, true,
        [&](const mcpp::Entry& entry) {
            ++prompts;
            return entry.name == "keep" ? mcpp::RecursiveDeleteDecision::no
                                       : mcpp::RecursiveDeleteDecision::yes;
        }
    );
    fs::permissions(root / "keep/unreadable", fs::perms{0700});
    check(prompts == 2U && result.completed_sources.size() == 1U && result.skipped.size() == 1U,
          "individual decisions did not preserve partial results");
    check(fs::exists(root / "keep/unreadable/file") && !fs::exists(root / "remove") &&
          fs::exists(root / "outside/file"), "No descended or Yes followed a symlink");
}

void changed_then_cancelled(const fs::path& root) {
    put(root / "done");
    put(root / "directory/file");
    put(root / "outside/file");
    mcpp::LocalProvider local(root);
    mcpp::OperationResult result;
    unsigned int errors = 0;
    bool cancelled = false;
    try {
        mcpp::OperationEngine{}.remove(
            local, {local.stat("done", {}), local.stat("directory", {})},
            {}, {}, &result,
            [&](const auto&, const std::string& message) {
                check(message.find("changed before removal") != std::string::npos,
                      "missing changed-directory guard");
                ++errors;
                return mcpp::OperationErrorDecision::cancel;
            }, false,
            [&](const auto&) {
                fs::rename(root / "directory", root / "saved");
                fs::create_directory_symlink(root / "outside", root / "directory");
                return mcpp::RecursiveDeleteDecision::yes;
            }
        );
    } catch (const mcpp::OperationCancelled&) {
        cancelled = true;
    }
    check(cancelled && errors == 1U && result.completed_sources.size() == 1U &&
          result.completed_sources.front() == local.normalize("done"),
          "cancellation lost an already completed source");
    check(fs::is_symlink(root / "directory") && fs::exists(root / "saved/file") &&
          fs::exists(root / "outside/file"), "directory replacement was traversed or removed");
}

}  // namespace

int main(const int argc, const char* const* argv) {
    try {
        if (getuid() != 1001 || geteuid() != 1001 || !fs::exists("/.dockerenv") ||
            !fs::exists("/review/.operation-regressions-disposable") || argc != 2) {
            throw std::runtime_error("recursive-delete checks require UID1001 in the approved Docker container");
        }
        const auto root = fs::absolute(argv[1]).lexically_normal();
        check(root.string().starts_with("/review/cases/recursive-delete/"),
              "fixtures must stay in /review/cases/recursive-delete");
        check(!fs::exists(root), "choose a fresh recursive-delete fixture directory");
        individual(root / "individual");
        changed_then_cancelled(root / "cancelled");
        std::cout << "PASS recursive delete model: individual and changed/cancelled (2 tiny cases)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
