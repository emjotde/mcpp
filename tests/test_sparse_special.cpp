#include "mcpp/archive_provider.hpp"
#include "mcpp/local_provider.hpp"
#include "mcpp/operations.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/xattr.h>
#include <fcntl.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace faults {

enum class Seek { normal, no_data, no_hole, dense, error };
enum class Io { normal, interrupted, short_write, zero, error };

struct State {
    Seek seek{Seek::normal};
    Io read{Io::normal};
    Io write{Io::normal};
    bool truncate{false};
    bool sync{false};
    unsigned int sync_after{0};
    std::size_t events{0};
} state;

struct Reset {
    ~Reset() { state = {}; }
};

}  // namespace faults

extern "C" off_t __real_lseek(int descriptor, off_t offset, int whence);
extern "C" ssize_t __real_pread(int descriptor, void* data, std::size_t count, off_t offset);
#if defined(__GLIBC__)
extern "C" ssize_t __real___pread_chk(int descriptor, void* data, std::size_t count, off_t offset, std::size_t capacity);
#endif
extern "C" ssize_t __real_pwrite(int descriptor, const void* data, std::size_t count, off_t offset);
extern "C" int __real_ftruncate(int descriptor, off_t length);
extern "C" int __real_fsync(int descriptor);

extern "C" off_t __wrap_lseek(const int descriptor, const off_t offset, const int whence) {
    if (whence == SEEK_DATA || whence == SEEK_HOLE) {
        const auto mode = faults::state.seek;
        if (mode != faults::Seek::normal) ++faults::state.events;
        if (mode == faults::Seek::error ||
            (mode == faults::Seek::no_data && whence == SEEK_DATA) ||
            (mode == faults::Seek::no_hole && whence == SEEK_HOLE)) {
            errno = mode == faults::Seek::error ? EIO : EINVAL;
            return -1;
        }
        if (mode == faults::Seek::dense) {
            struct stat status{};
            if (::fstat(descriptor, &status) != 0) return -1;
            if (offset >= status.st_size) {
                errno = ENXIO;
                return -1;
            }
            return whence == SEEK_DATA ? offset : status.st_size;
        }
    }
    return __real_lseek(descriptor, offset, whence);
}

extern "C" ssize_t __wrap_pread(
    const int descriptor, void* data, const std::size_t count, const off_t offset
) {
    const auto mode = std::exchange(faults::state.read, faults::Io::normal);
    if (mode != faults::Io::normal) {
        ++faults::state.events;
        errno = mode == faults::Io::interrupted ? EINTR : EIO;
        return -1;
    }
    return __real_pread(descriptor, data, count, offset);
}

#if defined(__GLIBC__)
extern "C" ssize_t __wrap___pread_chk(
    const int descriptor, void* data, const std::size_t count,
    const off_t offset, const std::size_t capacity
) {
    if (faults::state.read != faults::Io::normal) return __wrap_pread(descriptor, data, count, offset);
    return __real___pread_chk(descriptor, data, count, offset, capacity);
}
#endif

extern "C" ssize_t __wrap_pwrite(
    const int descriptor, const void* data, const std::size_t count, const off_t offset
) {
    const auto mode = std::exchange(faults::state.write, faults::Io::normal);
    if (mode != faults::Io::normal) {
        ++faults::state.events;
        if (mode == faults::Io::short_write) {
            return __real_pwrite(descriptor, data, std::min<std::size_t>(count, 7U), offset);
        }
        if (mode == faults::Io::zero) return 0;
        errno = mode == faults::Io::interrupted ? EINTR : EIO;
        return -1;
    }
    return __real_pwrite(descriptor, data, count, offset);
}

extern "C" int __wrap_ftruncate(const int descriptor, const off_t length) {
    if (std::exchange(faults::state.truncate, false)) {
        ++faults::state.events;
        errno = EIO;
        return -1;
    }
    return __real_ftruncate(descriptor, length);
}

extern "C" int __wrap_fsync(const int descriptor) {
    const bool delayed = faults::state.sync_after > 0U && --faults::state.sync_after == 0U;
    if (std::exchange(faults::state.sync, false) || delayed) {
        ++faults::state.events;
        errno = EIO;
        return -1;
    }
    return __real_fsync(descriptor);
}

namespace fs = std::filesystem;
using Time = std::chrono::system_clock::time_point;

namespace {

std::size_t checks = 0;
const Time precise_time{std::chrono::seconds(1700000000) + std::chrono::nanoseconds(123456789)};
constexpr off_t logical_size = 2 * 1024 * 1024 + 123;

void check(const bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
    ++checks;
}

template<class Function>
std::string rejects(Function&& function, const std::string& message) {
    std::string reason;
    try {
        function();
    } catch (const mcpp::OperationCancelled&) {
        throw;
    } catch (const std::exception& error) {
        reason = error.what();
    }
    check(!reason.empty(), message);
    return reason;
}

template<class Function>
void cancelled(Function&& function, const std::string& message) {
    bool stopped = false;
    try {
        function();
    } catch (const mcpp::OperationCancelled&) {
        stopped = true;
    }
    check(stopped, message);
}

class Descriptor final {
public:
    explicit Descriptor(const int value) : value_(value) {
        if (value < 0) throw std::runtime_error("open fixture: " + std::string(std::strerror(errno)));
    }
    ~Descriptor() { static_cast<void>(::close(value_)); }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    [[nodiscard]] int get() const { return value_; }

private:
    int value_;
};

struct stat native_stat(const fs::path& path) {
    struct stat result{};
    if (::lstat(path.c_str(), &result) != 0) throw std::runtime_error("lstat fixture: " + path.string());
    return result;
}

void put(const fs::path& path, const std::string_view bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    check(static_cast<bool>(output), "write fixture: " + path.string());
}

std::string get(const fs::path& path) {
    check(S_ISREG(native_stat(path).st_mode), "never open a special file as fixture data");
    std::ifstream input(path, std::ios::binary);
    check(static_cast<bool>(input), "open regular fixture: " + path.string());
    std::string result{std::istreambuf_iterator<char>(input), {}};
    check(!input.bad(), "read regular fixture: " + path.string());
    return result;
}

struct SparseFile {
    fs::path path;
    std::string bytes;
    std::vector<off_t> holes;
};

SparseFile sparse(
    mcpp::LocalProvider& local, const fs::path& path,
    const std::vector<std::pair<off_t, std::string>>& data,
    std::vector<off_t> holes
) {
    fs::create_directories(path.parent_path());
    SparseFile result{path, std::string(static_cast<std::size_t>(logical_size), '\0'), std::move(holes)};
    const Descriptor output(::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600));
    check(::ftruncate(output.get(), logical_size) == 0, "set sparse fixture size");
    for (const auto& [offset, bytes] : data) {
        check(offset >= 0 && offset + static_cast<off_t>(bytes.size()) <= logical_size, "bounded sparse fixture");
        check(::pwrite(output.get(), bytes.data(), bytes.size(), offset) == static_cast<ssize_t>(bytes.size()),
              "write sparse fixture extent");
        result.bytes.replace(static_cast<std::size_t>(offset), bytes.size(), bytes);
    }
    check(::fsync(output.get()) == 0, "sync sparse fixture");
    fs::permissions(path, fs::perms{0640});
    local.set_modified(path.string(), precise_time);
    return result;
}

bool hole_at(const fs::path& path, const off_t offset) {
    const Descriptor input(::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
    return ::lseek(input.get(), offset, SEEK_HOLE) == offset;
}

void exact(
    mcpp::LocalProvider& local, const SparseFile& source,
    const fs::path& destination, const bool compare_holes = true
) {
    const auto from = native_stat(source.path);
    const auto to = native_stat(destination);
    check(S_ISREG(to.st_mode) && to.st_size == logical_size, "copy retains regular type and exact logical size");
    check(get(destination) == source.bytes, "copy retains every logical byte, including hole boundaries");
    check(from.st_dev != to.st_dev || from.st_ino != to.st_ino, "ordinary copy does not link back to its source");
    check(local.stat(destination.string(), {}).modified == precise_time, "copy retains nanosecond modification time");
    if (from.st_blocks * 512 < logical_size / 2) {
        check(to.st_blocks * 512 < logical_size / 2, "copy retains sparse allocation where supported");
    }
    std::size_t verified_holes = 0;
    if (compare_holes) {
        for (const auto offset : source.holes) {
            if (hole_at(source.path, offset)) {
                check(hole_at(destination, offset), "copy retains a leading/interior/trailing hole");
                ++verified_holes;
            }
        }
    }
    std::cout << "sparse: " << destination.filename() << " size=" << to.st_size
              << " source_blocks=" << from.st_blocks << " destination_blocks=" << to.st_blocks
              << " checked_holes=" << verified_holes << '\n';
}

bool staging_present(const fs::path& directory, const std::string_view prefix) {
    if (!fs::is_directory(directory)) return false;
    return std::ranges::any_of(fs::directory_iterator(directory), [&](const fs::directory_entry& entry) {
        return entry.path().filename().string().starts_with(prefix);
    });
}

void clean_staging(const fs::path& root) {
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        const auto name = entry.path().filename().string();
        check(!name.starts_with(".mcpp-write-") && !name.starts_with(".mcpp-special-") &&
              !name.starts_with(".mcpp-hardlink-") && !name.starts_with(".mcpp-link-"),
              "publication leaves no staging entry: " + entry.path().string());
    }
}

void native_paths(const fs::path& root) {
    const auto confined_root = root / "native-root";
    fs::create_directory(confined_root);
    put(confined_root / "inside", "inside");
    put(root / "outside", "outside");
    fs::create_symlink("inside", confined_root / "safe-link");
    fs::create_symlink("../outside", confined_root / "escape");
    fs::create_directory_symlink("..", confined_root / "escape-parent");
    mcpp::LocalProvider confined(confined_root);
    check(confined.native_path("inside") == confined_root / "inside", "native path resolves a rooted relative filename");
    check(confined.native_path("safe-link") == confined_root / "inside", "native path follows a confined symbolic link");
    rejects([&] { static_cast<void>(confined.native_path("escape")); }, "native path rejects an escaping final symbolic link");
    rejects([&] { static_cast<void>(confined.native_path("escape-parent/outside")); },
            "native path rejects an escaping symbolic-link parent");
    rejects([&] { static_cast<void>(confined.native_path("../outside")); }, "native path rejects lexical root escape");
    check(get(root / "outside") == "outside", "root confinement does not alter outside data");
}

std::vector<SparseFile> sparse_copies(
    const fs::path& root, mcpp::LocalProvider& local, const mcpp::OperationEngine& engine
) {
    std::string binary(8193, '\0');
    for (std::size_t index = 0; index < binary.size(); ++index) binary[index] = static_cast<char>(index % 251U);
    std::vector<SparseFile> files;
    files.push_back(sparse(local, root / "leading", {{524305, binary}, {logical_size - 4, "tail"}}, {65536, 1048576}));
    files.push_back(sparse(local, root / "interior", {{0, binary}, {logical_size - 8193, binary}}, {1048576}));
    files.push_back(sparse(local, root / "trailing", {{0, binary}}, {logical_size - 65536}));
    files.push_back(sparse(local, root / "all-hole", {}, {65536, 1048576, logical_size - 65536}));
    for (const auto& file : files) {
        const auto destination = root / (file.path.filename().string() + "-copy");
        mcpp::OperationResult result;
        mcpp::TransferProgress last;
        engine.copy_exact(local, local.stat(file.path.string(), {}), local, destination.string(), {}, {},
                          [&](const auto& value) { last = value; }, &result);
        exact(local, file, destination);
        check((native_stat(destination).st_mode & 07777U) == 0640U, "sparse copy preserves mode");
        check(last.bytes_done == static_cast<std::uint64_t>(logical_size) && last.files_done == 1U &&
              result.completed_sources == std::vector<std::string>{file.path.string()}, "sparse progress counts logical bytes");
    }
    for (const auto mode : {faults::Seek::no_data, faults::Seek::no_hole, faults::Seek::dense}) {
        const auto destination = root / ("fallback-" + std::to_string(static_cast<int>(mode)));
        {
            const faults::Reset reset;
            faults::state.seek = mode;
            engine.copy_exact(local, local.stat(files[0].path.string(), {}), local, destination.string(), {}, {});
            check(faults::state.events > 0U, "exercise unsupported/conservative extent seek");
        }
        exact(local, files[0], destination);
    }
    const auto small = sparse(local, root / "small-holes", {{4096, "one"}, {12288, "two"}}, {0, 8192, 65536});
    {
        const faults::Reset reset;
        faults::state.seek = faults::Seek::no_data;
        engine.copy_exact(local, local.stat(small.path.string(), {}), local, (root / "small-hole-copy").string(), {}, {});
    }
    exact(local, small, root / "small-hole-copy");
    local.set_preallocate(true);
    engine.copy_exact(local, local.stat(files[2].path.string(), {}), local, (root / "preallocated-sparse").string(), {}, {});
    local.set_preallocate(false);
    exact(local, files[2], root / "preallocated-sparse");
    check(local.copy_direct(files[1].path.string(), (root / "direct-sparse").string(), false, {}, {}),
          "direct local copying uses sparse publication");
    exact(local, files[1], root / "direct-sparse");
    put(root / "empty", "");
    engine.copy_exact(local, local.stat((root / "empty").string(), {}), local, (root / "empty-copy").string(), {}, {});
    check(get(root / "empty-copy").empty(), "empty regular files remain regular and empty");
    put(root / "dense", std::string(262144, 'd'));
    engine.copy_exact(local, local.stat((root / "dense").string(), {}), local, (root / "dense-copy").string(), {}, {});
    check(get(root / "dense-copy") == get(root / "dense"), "dense regular copying is unchanged");
    put(root / "dense-zero", std::string(262144, '\0'));
    engine.copy_exact(local, local.stat((root / "dense-zero").string(), {}), local, (root / "dense-zero-copy").string(), {}, {});
    check(get(root / "dense-zero-copy") == get(root / "dense-zero"), "zero data is preserved when converted to holes");
    clean_staging(root);
    return files;
}

void rooted_copies(const fs::path& root, const mcpp::OperationEngine& engine) {
    fs::create_directory(root / "from-root");
    fs::create_directory(root / "to-root");
    mcpp::LocalProvider from(root / "from-root");
    mcpp::LocalProvider to(root / "to-root");
    const auto source = sparse(from, root / "from-root/data", {{524305, "rooted bytes"}}, {65536, 1048576});
    fs::permissions(source.path, fs::perms{0600});
    engine.copy_exact(from, from.stat("data", {}), to, "copy", {}, {});
    exact(to, source, root / "to-root/copy");
    check((native_stat(root / "to-root/copy").st_mode & 07777U) == 0600U, "different rooted providers preserve mode");
    mcpp::OperationOptions plain;
    plain.preserve_permissions = false;
    engine.copy_exact(from, from.stat("data", {}), to, "plain", plain, {});
    exact(to, source, root / "to-root/plain");
    check((native_stat(root / "to-root/plain").st_mode & 07777U) == 0640U, "unpreserved mode follows process umask");
    put(root / "to-root/existing", "KEEP");
    fs::permissions(root / "to-root/existing", fs::perms{0620});
    const auto attribute = std::string("existing metadata");
    const bool attributes = ::setxattr((root / "to-root/existing").c_str(), "user.l09", attribute.data(), attribute.size(), 0) == 0;
    plain.overwrite = true;
    engine.copy_exact(from, from.stat("data", {}), to, "existing", plain, {});
    exact(to, source, root / "to-root/existing");
    check((native_stat(root / "to-root/existing").st_mode & 07777U) == 0620U, "unpreserved overwrite retains destination mode");
    if (attributes) {
        std::array<char, 64> value{};
        const auto count = ::getxattr((root / "to-root/existing").c_str(), "user.l09", value.data(), value.size());
        check(count == static_cast<ssize_t>(attribute.size()) &&
              std::string_view(value.data(), attribute.size()) == attribute, "sparse overwrite retains output attributes");
    }
    fs::create_symlink("data", root / "from-root/link");
    plain.preserve_symlinks = false;
    engine.copy_exact(from, from.stat("link", {}), to, "dereferenced", plain, {});
    exact(to, source, root / "to-root/dereferenced");
    fs::create_directory_symlink("..", root / "to-root/escape");
    rejects([&] {
        engine.copy_exact(from, from.stat("data", {}), to, "escape/outside", {}, {});
    }, "copy destination retains rooted-provider confinement");
}

void hardlinks(const fs::path& root, mcpp::LocalProvider& local, const mcpp::OperationEngine& engine) {
    fs::create_directories(root / "group/left");
    fs::create_directory(root / "group/right");
    const auto first = sparse(local, root / "group/a", {{8193, "linked sparse bytes"}}, {1048576});
    for (const auto* name : {"left/b", "right/c", "z"}) fs::create_hard_link(first.path, root / "group" / name);
    engine.copy_exact(local, local.stat((root / "group").string(), {}), local, (root / "group-copy").string(), {}, {});
    const auto copied = native_stat(root / "group-copy/a");
    for (const auto* name : {"a", "left/b", "right/c", "z"}) {
        const auto destination = root / "group-copy" / name;
        const auto member = native_stat(destination);
        check(member.st_ino == copied.st_ino && member.st_dev == copied.st_dev && member.st_nlink == 4U,
              "sparse copies retain a four-name hardlink group");
        exact(local, first, destination);
    }
}

void regular_guards(
    const fs::path& root, mcpp::LocalProvider& local,
    const mcpp::OperationEngine& engine, const SparseFile& source
) {
    const auto target = root / "guarded";
    mcpp::OperationOptions overwrite;
    overwrite.overwrite = true;
    const auto copy = [&](const mcpp::Cancellation& stop, const mcpp::ProgressCallback& progress) {
        engine.copy_exact(local, local.stat(source.path.string(), {}), local, target.string(), overwrite, stop, progress);
    };
    put(target, "KEEP");
    const auto original = native_stat(target);
    mcpp::Cancellation stop;
    cancelled([&] {
        copy(stop, [&](const auto& progress) {
            if (progress.bytes_done > 0U) {
                check(get(target) == "KEEP" && native_stat(target).st_ino == original.st_ino, "old output remains visible while staging");
                stop.request();
            }
        });
    }, "cancellation interrupts sparse copying before publication");
    check(get(target) == "KEEP" && native_stat(target).st_ino == original.st_ino, "cancelled sparse overwrite preserves old inode");
    mcpp::Cancellation final_stop;
    std::size_t terminal = 0;
    cancelled([&] {
        copy(final_stop, [&](const auto& progress) {
            if (progress.files_done == 0U && progress.bytes_done == static_cast<std::uint64_t>(logical_size) &&
                ++terminal == 2U) final_stop.request();
        });
    }, "cancellation at the closed-file publication checkpoint is honored");
    check(terminal == 2U && get(target) == "KEEP", "final checkpoint cancellation preserves output");
    mcpp::Cancellation hole_stop;
    cancelled([&] {
        engine.copy_exact(local, local.stat((root / "all-hole").string(), {}), local, target.string(), overwrite, hole_stop,
                          [&](const auto& progress) { if (progress.bytes_done > 0U) hole_stop.request(); });
    }, "all-hole copying remains cancellable without reading source data");
    check(get(target) == "KEEP", "cancelled all-hole copy preserves output");
    mcpp::OperationResult skipped;
    engine.copy_exact(local, local.stat(source.path.string(), {}), local, target.string(), overwrite, {},
                      [](const auto& progress) { if (progress.bytes_done > 0U) throw mcpp::SkipCurrent{}; }, &skipped);
    check(skipped.completed_sources.empty() && skipped.skipped.size() == 1U && get(target) == "KEEP",
          "SkipCurrent preserves output and records a skipped sparse source");
    const auto race = root / "late-no-clobber";
    bool raced = false;
    rejects([&] {
        engine.copy_exact(local, local.stat(source.path.string(), {}), local, race.string(), {}, {},
                          [&](const auto& progress) {
                              if (!raced && progress.bytes_done > 0U) {
                                  put(race, "LATE");
                                  raced = true;
                              }
                          });
    }, "sparse publication refuses a late no-overwrite collision");
    check(raced && get(race) == "LATE", "late collision does not clobber a new output");
    bool revised = false;
    const auto reason = rejects([&] {
        copy({}, [&](const auto& progress) {
            if (!revised && progress.bytes_done > 0U) {
                put(target, "EXTERNAL");
                revised = true;
            }
        });
    }, "sparse overwrite rejects destination revision changes");
    check(reason.find("destination changed") != std::string::npos && get(target) == "EXTERNAL",
          "revision failure identifies and preserves the external output");
    put(target, "KEEP");
    put(root / "changing-source", std::string(262144, 'c'));
    bool replaced = false;
    rejects([&] {
        engine.copy_exact(local, local.stat((root / "changing-source").string(), {}), local, target.string(), overwrite, {},
                          [&](const auto& progress) {
                              if (!replaced && progress.bytes_done > 0U) {
                                  fs::rename(root / "changing-source", root / "original-source");
                                  check(::mkfifo((root / "changing-source").c_str(), 0600) == 0, "replace source with FIFO fixture");
                                  replaced = true;
                              }
                          });
    }, "sparse source identity is checked again before publication");
    check(replaced && get(target) == "KEEP" && S_ISFIFO(native_stat(root / "changing-source").st_mode),
          "source type replacement neither blocks nor publishes data from the wrong object");
    put(root / "retry-source", "retry bytes");
    fs::permissions(root / "retry-source", fs::perms::none);
    mcpp::OperationOptions retry;
    std::size_t failures = 0;
    retry.error = [&](const auto&, const std::string&) {
        check(++failures == 1U, "bounded source permission retry");
        fs::permissions(root / "retry-source", fs::perms{0600});
        return mcpp::OperationErrorDecision::retry;
    };
    engine.copy_exact(local, local.stat((root / "retry-source").string(), {}), local, (root / "retry-copy").string(), retry, {});
    check(failures == 1U && get(root / "retry-copy") == "retry bytes" &&
          (native_stat(root / "retry-copy").st_mode & 07777U) == 0600U, "retry refreshes source metadata and succeeds");
    clean_staging(root);
}

void io_failures(
    const fs::path& root, mcpp::LocalProvider& local,
    const mcpp::OperationEngine& engine, const SparseFile& source
) {
    const auto target = root / "io-target";
    mcpp::OperationOptions overwrite;
    overwrite.overwrite = true;
    for (int kind = 0; kind < 6; ++kind) {
        put(target, "KEEP");
        const auto before = native_stat(target);
        {
            const faults::Reset reset;
            switch (kind) {
                case 0: faults::state.write = faults::Io::error; break;
                case 1: faults::state.read = faults::Io::error; break;
                case 2: faults::state.truncate = true; break;
                case 3: faults::state.sync = true; break;
                case 4: faults::state.seek = faults::Seek::error; break;
                case 5: faults::state.write = faults::Io::zero; break;
                default: throw std::runtime_error("unexpected injection");
            }
            rejects([&] {
                engine.copy_exact(local, local.stat(source.path.string(), {}), local, target.string(), overwrite, {});
            }, "sparse syscall failure is reported rather than silently publishing: kind=" + std::to_string(kind));
            check(faults::state.events > 0U, "the sparse publication syscall was actually intercepted");
        }
        check(get(target) == "KEEP" && native_stat(target).st_ino == before.st_ino, "I/O failure preserves previous output inode and bytes");
        clean_staging(root);
    }
    for (int kind = 0; kind < 3; ++kind) {
        const auto destination = root / ("interrupted-" + std::to_string(kind));
        {
            const faults::Reset reset;
            if (kind == 0) faults::state.read = faults::Io::interrupted;
            else faults::state.write = kind == 1 ? faults::Io::interrupted : faults::Io::short_write;
            engine.copy_exact(local, local.stat(source.path.string(), {}), local, destination.string(), {}, {});
            check(faults::state.events == 1U, "exercise interrupted/short sparse I/O once");
        }
        exact(local, source, destination);
    }
    put(target, "KEEP");
    const auto revision = local.stat(target.string(), {}).revision;
    std::istringstream input("replacement");
    bool changed = false;
    rejects([&] {
        local.write(target.string(), input, 11U, true, revision, {}, [&](const auto&) {
            if (!changed) {
                put(target, "EXTERNAL");
                changed = true;
            }
        });
    }, "ordinary writes retain expected-revision checks through the shared staging helper");
    check(get(target) == "EXTERNAL", "ordinary write revision failure preserves the other writer");
    put(target, "KEEP");
    {
        const faults::Reset reset;
        faults::state.truncate = true;
        rejects([&] {
            engine.copy_exact(local, local.stat((root / "all-hole").string(), {}), local, target.string(), overwrite, {});
        }, "all-hole output reports ftruncate failure without publication");
        check(faults::state.events == 1U, "all-hole publication exercises ftruncate rather than data writes");
    }
    check(get(target) == "KEEP", "failed all-hole truncation preserves previous bytes");
    {
        const faults::Reset reset;
        faults::state.sync_after = 2;
        const auto message = rejects([&] {
            engine.copy_exact(local, local.stat(source.path.string(), {}), local, target.string(), overwrite, {});
        }, "post-rename durability failure is reported");
        check(faults::state.events == 1U && message.find("was published") != std::string::npos,
              "post-rename sync error states that publication already happened");
    }
    exact(local, source, target);
    clean_staging(root);
}

void fifos(const fs::path& root, mcpp::LocalProvider& local, const mcpp::OperationEngine& engine) {
    const auto source = root / "fifo";
    check(::mkfifo(source.c_str(), 0600) == 0, "create unread FIFO fixture");
    fs::permissions(source, fs::perms{0620});
    const Descriptor pipe(::open(source.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC));
    const std::string queued = "unread FIFO bytes";
    check(::write(pipe.get(), queued.data(), queued.size()) == static_cast<ssize_t>(queued.size()), "queue FIFO fixture data without blocking");
    local.set_modified(source.string(), precise_time);
    const auto destination = root / "fifo-copy";
    engine.copy_exact(local, local.stat(source.string(), {}), local, destination.string(), {}, {});
    check(S_ISFIFO(native_stat(destination).st_mode) && native_stat(destination).st_ino != native_stat(source).st_ino,
          "FIFO copy recreates an independent FIFO, not a regular file");
    check((native_stat(destination).st_mode & 07777U) == 0620U && local.stat(destination.string(), {}).modified == precise_time,
          "FIFO copy preserves mode and modification time");
    std::array<char, 64> remaining{};
    const auto count = ::read(pipe.get(), remaining.data(), remaining.size());
    check(count == static_cast<ssize_t>(queued.size()) && std::string_view(remaining.data(), queued.size()) == queued,
          "FIFO copy never consumes the source stream");
    fs::create_directory(root / "fifo-destination-root");
    mcpp::LocalProvider other(root / "fifo-destination-root");
    mcpp::OperationOptions plain;
    plain.preserve_permissions = false;
    engine.copy_exact(local, local.stat(source.string(), {}), other, "plain", plain, {});
    check(S_ISFIFO(native_stat(root / "fifo-destination-root/plain").st_mode) &&
          (native_stat(root / "fifo-destination-root/plain").st_mode & 07777U) == 0640U,
          "FIFO copying works across rooted providers with permissions disabled");
    fs::create_symlink("fifo", root / "fifo-link");
    plain.preserve_symlinks = false;
    engine.copy_exact(local, local.stat((root / "fifo-link").string(), {}), local, (root / "fifo-dereferenced").string(), plain, {});
    check(S_ISFIFO(native_stat(root / "fifo-dereferenced").st_mode), "dereferenced FIFO links are recreated without reading");
    check(local.copy_direct(source.string(), (root / "fifo-direct").string(), false, {}, {}), "direct FIFO copy succeeds without a peer");
    fs::create_directory(root / "fifo-tree");
    check(::mkfifo((root / "fifo-tree/pipe").c_str(), 0000) == 0, "create unreadable FIFO fixture");
    fs::create_hard_link(root / "fifo-tree/pipe", root / "fifo-tree/alias");
    put(root / "fifo-tree/plain", "tree bytes");
    engine.copy_exact(local, local.stat((root / "fifo-tree").string(), {}), local, (root / "fifo-tree-copy").string(), {}, {});
    check(S_ISFIFO(native_stat(root / "fifo-tree-copy/pipe").st_mode) &&
          (native_stat(root / "fifo-tree-copy/pipe").st_mode & 07777U) == 0000U &&
          get(root / "fifo-tree-copy/plain") == "tree bytes", "recursive copy recreates FIFOs even when source stream access is denied");
    const auto copied_pipe = native_stat(root / "fifo-tree-copy/pipe");
    const auto alias = native_stat(root / "fifo-tree-copy/alias");
    const auto source_pipe = native_stat(root / "fifo-tree/pipe");
    check(copied_pipe.st_dev == alias.st_dev && copied_pipe.st_ino == alias.st_ino &&
          copied_pipe.st_nlink == 2U &&
          (copied_pipe.st_dev != source_pipe.st_dev || copied_pipe.st_ino != source_pipe.st_ino),
          "copied FIFO hardlink groups share a new FIFO inode");
    mcpp::OperationOptions overwrite;
    overwrite.overwrite = true;
    const auto guarded = root / "fifo-guarded";
    put(guarded, "KEEP");
    const auto original = native_stat(guarded);
    mcpp::Cancellation stop;
    bool staged = false;
    cancelled([&] {
        engine.copy_exact(local, local.stat(source.string(), {}), local, guarded.string(), overwrite, stop,
                          [&](const auto&) {
                              if (staging_present(root, ".mcpp-special-")) {
                                  check(get(guarded) == "KEEP", "FIFO staging keeps old output visible");
                                  staged = true;
                                  stop.request();
                              }
                          });
    }, "FIFO cancellation is checked after staging and before rename");
    check(staged && get(guarded) == "KEEP" && native_stat(guarded).st_ino == original.st_ino, "cancelled FIFO copy preserves prior output");
    mcpp::OperationResult skipped;
    engine.copy_exact(local, local.stat(source.string(), {}), local, guarded.string(), overwrite, {},
                      [&](const auto&) { if (staging_present(root, ".mcpp-special-")) throw mcpp::SkipCurrent{}; }, &skipped);
    check(skipped.skipped.size() == 1U && skipped.completed_sources.empty() && get(guarded) == "KEEP",
          "FIFO SkipCurrent cleans staging and reports skip");
    {
        const faults::Reset reset;
        faults::state.sync = true;
        rejects([&] {
            engine.copy_exact(local, local.stat(source.string(), {}), local, guarded.string(), overwrite, {});
        }, "special inode creation is synced before publication");
        check(faults::state.events == 1U, "special-file directory sync was intercepted");
    }
    check(get(guarded) == "KEEP", "failed special-file sync preserves old destination");
    bool changed_output = false;
    const auto revision_error = rejects([&] {
        engine.copy_exact(local, local.stat(source.string(), {}), local, guarded.string(), overwrite, {},
                          [&](const auto&) {
                              if (!changed_output && staging_present(root, ".mcpp-special-")) {
                                  put(guarded, "EXTERNAL");
                                  changed_output = true;
                              }
                          });
    }, "FIFO publication checks an overwritten output revision");
    check(changed_output && revision_error.find("destination changed") != std::string::npos && get(guarded) == "EXTERNAL",
          "FIFO revision failure preserves the other writer");
    put(guarded, "KEEP");
    bool changed_source = false;
    rejects([&] {
        engine.copy_exact(local, local.stat(source.string(), {}), local, guarded.string(), overwrite, {},
                          [&](const auto&) {
                              if (!changed_source && staging_present(root, ".mcpp-special-")) {
                                  fs::rename(source, root / "fifo-original");
                                  check(::mkfifo(source.c_str(), 0600) == 0, "replace FIFO source fixture");
                                  changed_source = true;
                              }
                          });
    }, "FIFO publication checks source identity");
    check(changed_source && get(guarded) == "KEEP", "changed FIFO source does not publish a stale node");
    fs::remove(source);
    fs::rename(root / "fifo-original", source);
    bool raced = false;
    const auto late = root / "fifo-late";
    rejects([&] {
        engine.copy_exact(local, local.stat(source.string(), {}), local, late.string(), {}, {}, [&](const auto&) {
            if (!raced && staging_present(root, ".mcpp-special-")) {
                put(late, "LATE");
                raced = true;
            }
        });
    }, "FIFO publication refuses a late no-overwrite collision");
    check(raced && get(late) == "LATE", "late FIFO collision keeps the new regular output intact");
    engine.copy_exact(local, local.stat(source.string(), {}), local, guarded.string(), overwrite, {});
    check(S_ISFIFO(native_stat(guarded).st_mode), "successful overwrite publishes a FIFO over an old regular file");
    clean_staging(root);
    std::cout << "FIFO: recreated, no source bytes consumed, cancellation/skip/sync failure guarded\n";
}

void unsupported(const fs::path& root, mcpp::LocalProvider& local, const mcpp::OperationEngine& engine) {
    const auto socket_path = root / "unix-socket";
    const Descriptor socket(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const auto name = socket_path.string();
    check(name.size() < sizeof(address.sun_path), "bounded Unix socket pathname");
    std::memcpy(address.sun_path, name.c_str(), name.size() + 1U);
    check(::bind(socket.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0, "create Unix socket fixture");
    const auto target = root / "socket-target";
    put(target, "KEEP");
    mcpp::OperationOptions overwrite;
    overwrite.overwrite = true;
    overwrite.preserve_permissions = false;
    const auto reason = rejects([&] {
        engine.copy_exact(local, local.stat(socket_path.string(), {}), local, target.string(), overwrite, {});
    }, "Unix socket copy is explicitly rejected");
    check(reason.find("Unix socket") != std::string::npos && get(target) == "KEEP", "socket rejection is clear and leaves old data intact");
    fs::create_symlink("unix-socket", root / "socket-link");
    auto dereference = overwrite;
    dereference.preserve_symlinks = false;
    rejects([&] {
        engine.copy_exact(local, local.stat((root / "socket-link").string(), {}), local, target.string(), dereference, {});
    }, "dereferencing a socket link does not turn it into an empty regular file");
    check(get(target) == "KEEP", "socket-link rejection preserves old bytes");
    rejects([&] { local.copy_direct(socket_path.string(), (root / "socket-direct").string(), false, {}, {}); },
            "direct copying does not read Unix sockets");
    check(!local.exists((root / "socket-direct").string()), "unsupported socket never becomes an empty regular file");
    mcpp::OperationOptions skip = overwrite;
    std::size_t failures = 0;
    skip.error = [&](const auto&, const std::string&) {
        ++failures;
        return mcpp::OperationErrorDecision::skip;
    };
    mcpp::OperationResult result;
    engine.copy(local, {local.stat(socket_path.string(), {}), local.stat((root / "dense").string(), {})},
                local, (root / "skip-errors").string(), skip, {}, {}, &result);
    check(failures == 1U && result.skipped.size() == 1U && result.completed_sources.size() == 1U &&
          get(root / "skip-errors/dense") == get(root / "dense") && !local.exists((root / "skip-errors/unix-socket").string()),
          "error skip continues later sources without publishing unsupported types");
    mcpp::LocalProvider filesystem;
    check(S_ISCHR(native_stat("/dev/null").st_mode), "device permission fixture is the container's character device");
    put(root / "device-target", "KEEP DEVICE DESTINATION");
    const auto old = native_stat(root / "device-target");
    const auto permission = rejects([&] {
        engine.copy_exact(filesystem, filesystem.stat("/dev/null", {}), local, (root / "device-target").string(), overwrite, {});
    }, "device creation obeys ordinary mknod permissions");
    check(permission.find("mknod") != std::string::npos && permission.find("permission") != std::string::npos &&
          (permission.find("Operation not permitted") != std::string::npos || permission.find("Permission denied") != std::string::npos),
          "device failure clearly reports the missing Linux permission");
    check(get(root / "device-target") == "KEEP DEVICE DESTINATION" &&
          native_stat(root / "device-target").st_ino == old.st_ino, "mknod failure preserves the old destination");
    mcpp::OperationResult device_skipped;
    engine.copy_exact(filesystem, filesystem.stat("/dev/null", {}), local, (root / "device-skipped").string(), skip, {}, {}, &device_skipped);
    check(device_skipped.skipped.size() == 1U && device_skipped.completed_sources.empty() &&
          !local.exists((root / "device-skipped").string()), "device permission errors use normal retry/skip handling");
    const auto archive_path = root / "special-rejection.tar";
    put(archive_path, std::string(1024, '\0'));
    mcpp::ArchiveOpenOptions archive_options;
    archive_options.writable = true;
    archive_options.draft_directory = root / "special-rejection-drafts";
    mcpp::ArchiveProvider archive(archive_path, archive_options);
    const auto archive_before = get(archive_path);
    for (const auto* name_to_copy : {"fifo", "unix-socket"}) {
        const auto message = rejects([&] {
            engine.copy_exact(local, local.stat((root / name_to_copy).string(), {}), archive, "/unsupported", {}, {});
        }, "generic cross-provider transfers reject special types before reading streams");
        check(message.find("unsupported special file copy between providers") != std::string::npos &&
              !archive.exists("/unsupported") && get(archive_path) == archive_before,
              "unsupported cross-provider copies never publish an empty archive member");
    }
    clean_staging(root);
    std::cout << "device: " << permission << "\nsocket: " << reason << '\n';
}

}  // namespace

int main(const int argc, char** argv) {
    try {
        if (::geteuid() != 1001 || !fs::is_regular_file("/review/.operation-regressions-disposable")) {
            throw std::runtime_error("sparse/special regressions require uid 1001 in the disposable review container");
        }
        if (argc != 2) throw std::runtime_error("usage: test_sparse_special /review/cases/sparse-special-files");
        const auto base = fs::absolute(argv[1]).lexically_normal();
        if (base != "/review/cases/sparse-special-files" || !fs::is_directory(base) ||
            fs::is_symlink(fs::symlink_status(base)) || fs::canonical(base) != base) {
            throw std::runtime_error("unexpected sparse/special regression workspace");
        }
        const auto root = base / ("run-" + std::to_string(::getpid()));
        if (!fs::create_directory(root)) throw std::runtime_error("sparse/special workspace already exists");
        fs::permissions(root, fs::perms{0700});
        fs::current_path(root);
        fs::create_directory("transfers");
        check(::setenv("TMPDIR", (root / "transfers").c_str(), 1) == 0, "keep any engine transfer staging inside the owned fixture");
        const auto previous_mask = ::umask(0027);
        mcpp::LocalProvider local(root);
        const mcpp::OperationEngine engine;
        native_paths(root);
        const auto files = sparse_copies(root, local, engine);
        rooted_copies(root, engine);
        hardlinks(root, local, engine);
        regular_guards(root, local, engine, files[0]);
        io_failures(root, local, engine, files[0]);
        fifos(root, local, engine);
        unsupported(root, local, engine);
        ::umask(previous_mask);
        clean_staging(root);
        fs::current_path(base);
        fs::remove_all(root);
        std::cout << "PASS: " << checks << " sparse/special checks; uid=" << ::geteuid() << "; workspace=" << root << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " sparse/special checks: " << error.what() << '\n';
        return 1;
    }
}
