#include "mcpp/local_provider.hpp"
#include "mcpp/viewer.hpp"

#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;

void check(const bool condition, const std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

template<class Action>
void rejected(Action action, const std::string_view message) {
    bool failed = false;
    try { action(); }
    catch (const std::exception&) { failed = true; }
    check(failed, message);
}

template<class Predicate>
void wait(mcpp::ViewerModel& viewer, Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    do {
        static_cast<void>(viewer.refresh());
        if (predicate()) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);
    throw std::runtime_error("timed out waiting for viewer input");
}

std::string read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}

void growing(const fs::path& root) {
    auto local = std::make_shared<mcpp::LocalProvider>();
    const auto file = root / "growing.txt";
    const std::string original = "head\nneedle one\nlast\n";
    std::ofstream(file) << original;
    mcpp::ViewerModel viewer;
    viewer.open(local, file.string());
    viewer.goto_offset(5);
    viewer.select_bookmark(3);
    viewer.set_bookmark();
    check(viewer.find("needle", true) == 5U, "initial search origin");
    std::ofstream(file, std::ios::app) << "needle two\nTAIL\n";
    check(viewer.refresh() && viewer.data_size() > original.size(), "observe append");
    check(viewer.find("needle", true) == original.size(), "repeat search retains its origin after append");
    viewer.goto_bookmark();
    check(viewer.offset() == 5U, "append preserves bookmarks");
    viewer.go_to_end(40, 2);
    const auto tail = viewer.lines(40, 2);
    check(!tail.empty() && tail.back() == "TAIL" && viewer.position_percent() == 100U, "End sees appended rows");

    viewer.toggle_hex();
    viewer.set_hex_cursor(viewer.size() - 1U, true);
    std::ofstream(file) << "short\n";
    static_cast<void>(viewer.refresh());
    check(viewer.size() == 6U && viewer.hex_cursor() == 5U &&
          viewer.offset() == 0U && viewer.hex_ascii(), "truncate clamps the mirrored hex/ASCII cursor");
    viewer.goto_offset(0);
    viewer.toggle_hex_editing();
    viewer.edit_ascii_byte('S');
    std::ofstream(file, std::ios::app) << "new\n";
    static_cast<void>(viewer.refresh());
    check(viewer.dirty() && viewer.data_size() == 6U && !viewer.source_error().empty(), "pending edits freeze the old revision");
    rejected([&] { viewer.save_changes(); }, "append must reject a stale hex save");
    check(read(file) == "short\nnew\n", "stale save preserves appended bytes");
    viewer.discard_changes();
    check(!viewer.dirty() && viewer.data_size() == 10U, "discard reloads the new revision");

    viewer.goto_offset(0);
    viewer.edit_ascii_byte('S');
    std::ofstream(file) << "x\n";
    static_cast<void>(viewer.refresh());
    rejected([&] { viewer.save_changes(); }, "truncate must reject a stale hex save");
    check(read(file) == "x\n", "stale save preserves truncated bytes");
    viewer.discard_changes();
    viewer.goto_offset(0);
    viewer.edit_ascii_byte('X');
    const auto replacement = root / "replacement.txt";
    std::ofstream(replacement) << "replacement\n";
    fs::rename(replacement, file);
    static_cast<void>(viewer.refresh());
    rejected([&] { viewer.save_changes(); }, "replacement must reject a stale hex save");
    check(read(file) == "replacement\n", "stale save preserves replacement bytes");
    viewer.discard_changes();

    const auto linked = root / "linked.txt";
    fs::create_hard_link(file, linked);
    static_cast<void>(viewer.refresh());
    viewer.goto_offset(0);
    viewer.edit_ascii_byte('R');
    rejected([&] { viewer.save_changes(); }, "hardlinked hex saves still require a choice");
    viewer.save_changes({}, mcpp::ViewerLinkedSave::preserve_links);
    check(!viewer.dirty() && read(file) == "Replacement\n" && read(linked) == read(file),
          "explicit preserve-links save remains usable");
}

struct Pipe {
    std::array<int, 2> descriptors{-1, -1};
    Pipe() { check(::pipe(descriptors.data()) == 0, "create viewer input pipe"); }
    ~Pipe() { for (const auto descriptor : descriptors) if (descriptor >= 0) ::close(descriptor); }
    void finish() { ::close(descriptors[1]); descriptors[1] = -1; }
    void put(const std::string_view value) {
        check(::write(descriptors[1], value.data(), value.size()) == static_cast<ssize_t>(value.size()),
              "write small viewer input");
    }
};

void stream(const fs::path& root) {
    check(::setenv("XDG_CACHE_HOME", (root / "cache").c_str(), 1) == 0, "set private viewer cache");
    auto local = std::make_shared<mcpp::LocalProvider>();
    Pipe pipe;
    mcpp::ViewerModel viewer;
    viewer.open_stream(local, pipe.descriptors[0], "input");
    check(viewer.streamed() && viewer.reading() && viewer.position_percent() == 0U, "empty live input is not a completed empty file");
    pipe.put("head\nneedle\n");
    wait(viewer, [&] { return viewer.size() == 12U; });
    check(viewer.lines(40, 2).front() == "head", "render data before EOF");
    viewer.goto_offset(5);
    viewer.set_bookmark();
    check(viewer.find("needle", true) == 5U, "search received data");
    pipe.put("second\nTAIL\n");
    wait(viewer, [&] { return viewer.size() == 24U; });
    viewer.goto_bookmark();
    check(viewer.offset() == 5U, "stream growth preserves bookmarks");
    viewer.go_to_end(40, 1);
    check(viewer.lines(40, 1).front() == "TAIL", "End seeks the available stream tail without waiting for EOF");
    viewer.go_to_start();
    viewer.toggle_hex();
    viewer.set_hex_cursor(1, true);
    check(viewer.lines(80, 1).front().find("68 65 61 64") != std::string::npos && viewer.hex_ascii(),
          "seek backward in spooled data and retain the ASCII cursor");
    rejected([&] { viewer.toggle_hex_editing(); }, "stream storage is read-only");
    pipe.finish();
    wait(viewer, [&] { return !viewer.reading(); });
    check(viewer.source_error().empty() && viewer.data_size() == 24U, "EOF retains all received bytes");
    viewer.close();

    Pipe idle;
    mcpp::Cancellation cancellation;
    viewer.open_stream(local, idle.descriptors[0], "waiting", cancellation);
    cancellation.request();
    wait(viewer, [&] { return !viewer.reading(); });
    check(viewer.source_error().find("cancelled") != std::string::npos, "input cancellation is explicit");
    viewer.close();
    viewer.open_stream(local, idle.descriptors[0], "waiting");
    const auto start = std::chrono::steady_clock::now();
    viewer.close();
    check(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(500), "close cancels a source that has no EOF");

    viewer.open(local, "/proc/version");
    wait(viewer, [&] { return !viewer.reading(); });
    check(viewer.streamed() && viewer.size() > 0U && !viewer.lines(80, 1).empty(), "size-zero proc file is read rather than treated as empty");
    viewer.open(local, "/proc/self/mem");
    wait(viewer, [&] { return !viewer.reading(); });
    check(!viewer.source_error().empty(), "a failing size-zero proc source reports its read error");
    viewer.close();
    for (const auto& entry : fs::directory_iterator(root / "cache" / "mcpp")) {
        check(!entry.path().filename().string().starts_with(".viewer-"), "private spool names do not remain on disk");
    }
}

}  // namespace

int main(const int argc, const char* const* argv) {
    try {
        check(argc == 3, "usage: mcpp_viewer_source_tests growing|stream CASE_DIRECTORY");
        check(::geteuid() == 1001 && fs::exists("/.dockerenv") &&
              fs::exists("/review/.operation-regressions-disposable"), "viewer source checks require the owned uid1001 disposable container");
        const auto root = fs::absolute(argv[2]).lexically_normal();
        check(root.string().starts_with("/review/cases/streamed-viewer/") &&
              fs::weakly_canonical(root) == root, "use a dedicated streamed-viewer child directory without symlinks");
        fs::create_directories(root);
        struct stat directory{};
        check(::lstat(root.c_str(), &directory) == 0 && S_ISDIR(directory.st_mode) &&
              directory.st_uid == ::geteuid(), "case directory must be owned by the test user");
        const std::string_view selected(argv[1]);
        if (selected == "growing") growing(root);
        else if (selected == "stream") stream(root);
        else throw std::invalid_argument("unknown viewer source case");
        std::cout << "PASS viewer source " << selected << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "viewer sources: " << error.what() << '\n';
        return 1;
    }
}
