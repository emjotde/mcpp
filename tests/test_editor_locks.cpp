#include "mcpp/editor_lock.hpp"

#include <array>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

namespace {

namespace fs = std::filesystem;

void check(const bool condition, const std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

std::string hostname() {
    std::array<char, 256> name{};
    check(::gethostname(name.data(), name.size() - 1U) == 0, "gethostname");
    return name.data();
}

std::optional<mcpp::EditorLockConflict> conflict(const fs::path& file) {
    try {
        auto unexpected = mcpp::EditorLock::acquire(file);
    } catch (const mcpp::EditorLockConflict& held) {
        return held;
    }
    throw std::runtime_error("expected a live editor lock");
}

void lifecycle(const fs::path& root) {
    const auto file = root / "shared.txt";
    const auto alias = root / "alias.txt";
    std::ofstream(file) << "original\n";
    fs::create_symlink(file.filename(), alias);
    const auto lock_path = mcpp::EditorLock::lock_path(file);
    std::array<int, 2> ready{}, release{};
    check(::pipe(ready.data()) == 0 && ::pipe(release.data()) == 0, "create child pipes");
    const auto child = ::fork();
    check(child >= 0, "fork editor session");
    if (child == 0) {
        static_cast<void>(::close(ready[0]));
        static_cast<void>(::close(release[1]));
        int result = 0;
        try {
            auto held = mcpp::EditorLock::acquire(file);
            const char signal = 'r';
            check(::write(ready[1], &signal, 1U) == 1, "announce child lock");
            char finish = 0;
            check(::read(release[0], &finish, 1U) == 1, "wait for parent takeover");
            held.reset();
            check(fs::is_symlink(lock_path), "old editor must not delete a replacement lock");
        } catch (...) {
            result = 1;
        }
        ::_exit(result);
    }
    static_cast<void>(::close(ready[1]));
    static_cast<void>(::close(release[0]));
    char signal = 0;
    check(::read(ready[0], &signal, 1U) == 1, "child owns lock");
    static_cast<void>(::close(ready[0]));
    const auto child_target = fs::read_symlink(lock_path).string();
    check(child_target.ends_with("@" + hostname() + "." + std::to_string(child)),
          "native owner metadata is user@host.pid");
    auto held = conflict(file);
    check(held->process_id() == child && held->owner().ends_with("@" + hostname()),
          "live local owner is parsed");
    auto ignored = mcpp::EditorLock::ignore(*held);
    check(ignored.current() && !ignored.owned(), "Ignore lock retains an unowned decision");
    ignored.reset();
    check(fs::read_symlink(lock_path) == child_target, "Ignore never removes the other editor's lock");
    auto grabbed = mcpp::EditorLock::grab(*held);
    check(grabbed.owned(), "Grab lock acquires ownership");
    const char finish = 'q';
    check(::write(release[1], &finish, 1U) == 1, "release old editor");
    static_cast<void>(::close(release[1]));
    int status = 0;
    check(::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "old session exits without deleting the taken-over lock");
    auto copy = grabbed;
    auto moved = std::move(grabbed);
    auto duplicate = mcpp::EditorLock::acquire(root / "." / "shared.txt");
    check(duplicate.matches_file(file) && duplicate.owned(), "same-session normalized paths share ownership");
    check(mcpp::EditorLock::lock_path(alias) == root / ".#alias.txt",
          "native lock naming retains the edited symlink's basename");
    copy.reset();
    moved.reset();
    check(duplicate.owned(), "buffer copies and moves retain the lock");
    duplicate.reset();
    check(!fs::is_symlink(lock_path), "last owner removes the lock");

    fs::create_symlink("test@" + hostname() + "." + std::to_string(child), lock_path);
    {
        auto stale = mcpp::EditorLock::acquire(file);
        check(stale.owned(), "dead local process lock is reclaimed");
    }
    check(!fs::is_symlink(lock_path), "reclaimed lock is cleaned up");

    fs::create_symlink("remote@another-host." + std::to_string(child), lock_path);
    held = conflict(file);
    check(held->owner() == "remote@another-host", "remote process is never tested as a local stale PID");
    fs::remove(lock_path);
    fs::create_symlink("new-owner@another-host.1", lock_path);
    bool changed = false;
    try { auto replacement = mcpp::EditorLock::grab(*held); }
    catch (const mcpp::EditorLockConflict& current) {
        changed = current.owner() == "new-owner@another-host";
    }
    check(changed && fs::read_symlink(lock_path) == "new-owner@another-host.1",
          "a stale Grab decision cannot remove a new owner's lock");
    fs::remove(lock_path);
    std::ofstream(lock_path) << "not an editor symlink\n";
    held = conflict(file);
    bool protected_file = false;
    try { auto replacement = mcpp::EditorLock::grab(*held); }
    catch (const std::runtime_error&) { protected_file = true; }
    check(protected_file && fs::is_regular_file(lock_path), "never delete a non-symlink lock artifact");
    fs::remove(lock_path);
    fs::remove(alias);
    fs::remove(file);
}

}  // namespace

int main(const int argc, const char* const* argv) {
    try {
        check(argc == 2, "usage: mcpp_editor_lock_tests CASE_DIRECTORY");
        check(::geteuid() == 1001 && fs::exists("/review/.operation-regressions-disposable"),
              "editor lock checks require the owned uid1001 disposable container");
        const auto root = fs::absolute(argv[1]).lexically_normal();
        check(root.string().starts_with("/review/cases/editor-locks/"),
              "use a dedicated /review/cases/editor-locks child directory");
        fs::create_directories(root);
        struct stat directory{};
        check(::lstat(root.c_str(), &directory) == 0 && S_ISDIR(directory.st_mode) &&
                  directory.st_uid == ::geteuid(),
              "case directory must be owned and must not be a symlink");
        lifecycle(root);
        std::cout << "PASS native lock metadata, live/stale/remote owners, Grab/Ignore and safe cleanup\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
