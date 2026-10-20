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

void write_file(const fs::path& path, const std::string_view bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    check(output.good(), "fixture write failed");
}

std::string read_file(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    check(input.good(), "fixture read failed");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void exact_bytes() {
    const fs::path root("model-bytes");
    fs::create_directories(root);
    mcpp::LocalProvider local(fs::absolute(root));
    mcpp::OperationEngine engine;
    const std::string source("A\0Btail\n", 8);
    const std::string prefix("old\0", 4);
    write_file(root / "source", source);
    write_file(root / "append", prefix);
    mcpp::OperationOptions options;
    options.collision = [](const auto&, const auto&) {
        return mcpp::CollisionDecision::append;
    };
    mcpp::TransferProgress last;
    const auto progress = [&](const mcpp::TransferProgress& value) {
        check(value.bytes_done >= last.bytes_done, "logical progress went backwards");
        check(value.bytes_done <= value.bytes_total, "retained prefix counted as transferred bytes");
        last = value;
    };
    mcpp::OperationResult result;
    engine.move_exact(
        local, local.stat("source", {}), local, local.normalize("append"),
        options, {}, progress, &result
    );
    check(read_file(root / "append") == prefix + source, "append did not preserve exact binary bytes");
    check(!fs::exists(root / "source"), "successful append move retained its source");
    check(result.completed_sources.size() == 1U && result.skipped.empty(), "append move result is incomplete");
    check(last.bytes_done == source.size() && last.bytes_total == source.size() &&
          last.files_done == 1U, "append logical totals are wrong");

    write_file(root / "source", source);
    write_file(root / "resume", source.substr(0, 3));
    options.collision = [](const auto&, const auto&) {
        return mcpp::CollisionDecision::reget;
    };
    last = {};
    result = {};
    engine.copy_exact(
        local, local.stat("source", {}), local, local.normalize("resume"),
        options, {}, progress, &result
    );
    check(read_file(root / "resume") == source, "reget duplicated or lost binary bytes");
    check(read_file(root / "source") == source, "reget altered its source");
    check(result.completed_sources.size() == 1U && result.skipped.empty(), "reget result is incomplete");
    check(last.bytes_done == source.size() && last.bytes_total == source.size() &&
          last.files_done == 1U, "reget logical totals are wrong");
}

void safety() {
    const fs::path root("model-safety");
    fs::create_directories(root);
    mcpp::LocalProvider local(fs::absolute(root));
    mcpp::OperationEngine engine;
    const std::string source("ABCDE\n");
    write_file(root / "source", source);
    write_file(root / "target", "WRONG");
    mcpp::OperationOptions options;
    options.collision = [](const auto&, const auto&) {
        return mcpp::CollisionDecision::reget;
    };
    unsigned int errors = 0;
    options.error = [&](const auto&, const std::string& message) {
        check(message.find("not a prefix") != std::string::npos, "missing mismatched-prefix error");
        ++errors;
        return mcpp::OperationErrorDecision::skip;
    };
    mcpp::OperationResult result;
    engine.copy_exact(
        local, local.stat("source", {}), local, local.normalize("target"),
        options, {}, {}, &result
    );
    check(errors == 1U && result.completed_sources.empty() && result.skipped.size() == 1U,
          "mismatched prefix was not reported as skipped");
    check(read_file(root / "target") == "WRONG", "mismatched reget altered the destination");

    options.collision = [&](const auto&, const auto&) {
        write_file(root / "target", "concurrent");
        return mcpp::CollisionDecision::append;
    };
    options.error = [&](const auto&, const std::string& message) {
        check(message.find("destination changed") != std::string::npos, "missing destination-conflict error");
        return mcpp::OperationErrorDecision::skip;
    };
    result = {};
    engine.copy_exact(
        local, local.stat("source", {}), local, local.normalize("target"),
        options, {}, {}, &result
    );
    check(read_file(root / "target") == "concurrent", "append overwrote a changed destination");
    check(result.completed_sources.empty() && result.skipped.size() == 1U, "conflict was reported as success");

    write_file(root / "target", "prefix");
    options.collision = [](const auto&, const auto&) {
        return mcpp::CollisionDecision::append;
    };
    options.error = {};
    mcpp::Cancellation cancellation;
    result = {};
    bool cancelled = false;
    try {
        engine.copy_exact(
            local, local.stat("source", {}), local, local.normalize("target"),
            options, cancellation,
            [&](const mcpp::TransferProgress& value) {
                if (value.bytes_done != 0U) cancellation.request();
            }, &result
        );
    } catch (const mcpp::OperationCancelled&) {
        cancelled = true;
    }
    check(cancelled && result.completed_sources.empty(), "cancelled append was reported as complete");
    check(read_file(root / "target") == "prefix", "cancelled append published partial data");
    check(read_file(root / "source") == source, "failed transfers changed their source");
    for (const auto& entry : fs::directory_iterator(root)) {
        check(!entry.path().filename().string().starts_with(".mcpp-"), "failed transfer left staged output");
    }
}

}  // namespace

int main(const int argc, char** argv) {
    try {
        check(geteuid() == 1001 && fs::exists("/.dockerenv") &&
              fs::exists("/review/.operation-regressions-disposable") &&
              fs::current_path() == "/review/cases/append-resume",
              "run only as UID1001 in the existing append-resume Docker fixture directory");
        check(argc == 2, "expected one case: bytes or safety");
        const std::string_view selected(argv[1]);
        if (selected == "bytes") exact_bytes();
        else if (selected == "safety") safety();
        else throw std::runtime_error("unknown append/reget case");
        std::cout << "PASS append-reget " << selected << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
