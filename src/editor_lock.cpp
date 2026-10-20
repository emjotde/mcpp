#include "mcpp/editor_lock.hpp"

#include <array>
#include <cerrno>
#include <charconv>
#include <csignal>
#include <fcntl.h>
#include <limits>
#include <mutex>
#include <pwd.h>
#include <string_view>
#include <sys/stat.h>
#include <system_error>
#include <unordered_map>
#include <unistd.h>
#include <utility>
#include <vector>

namespace mcpp {
namespace {

class Descriptor {
public:
    explicit Descriptor(const int value = -1) noexcept : value_(value) {}
    ~Descriptor() { if (value_ >= 0) static_cast<void>(::close(value_)); }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    [[nodiscard]] int get() const noexcept { return value_; }

private:
    int value_;
};

std::filesystem::path resolve_file(const std::filesystem::path& file) {
    if (file.empty() || file.native().find('\0') != std::string::npos) {
        throw std::invalid_argument("editor lock requires a file name");
    }
    const auto absolute = std::filesystem::absolute(file).lexically_normal();
    return std::filesystem::weakly_canonical(absolute.parent_path()) / absolute.filename();
}

std::string local_host() {
    std::array<char, 256> hostname{};
    if (::gethostname(hostname.data(), hostname.size() - 1U) != 0) {
        throw std::system_error(errno, std::generic_category(), "get editor lock hostname");
    }
    return hostname.data();
}

std::string local_owner() {
    struct passwd entry{};
    struct passwd* found = nullptr;
    std::vector<char> buffer(4096U);
    int result = 0;
    while ((result = ::getpwuid_r(::geteuid(), &entry, buffer.data(), buffer.size(), &found)) == ERANGE &&
           buffer.size() < 65536U) {
        buffer.resize(buffer.size() * 2U);
    }
    const auto user = result == 0 && found != nullptr
        ? std::string(found->pw_name) : std::to_string(::geteuid());
    return user + "@" + local_host() + "." + std::to_string(::getpid());
}

bool same_node(const struct stat& left, const struct stat& right) noexcept {
    return left.st_dev == right.st_dev && left.st_ino == right.st_ino &&
           left.st_mode == right.st_mode &&
           left.st_ctim.tv_sec == right.st_ctim.tv_sec &&
           left.st_ctim.tv_nsec == right.st_ctim.tv_nsec;
}

std::string link_contents(const int descriptor) {
    std::array<char, 4097> contents{};
    const auto count = ::readlinkat(descriptor, "", contents.data(), contents.size());
    if (count < 0) {
        throw std::system_error(errno, std::generic_category(), "read editor lock");
    }
    if (static_cast<std::size_t>(count) == contents.size()) {
        throw std::runtime_error("editor lock owner is too long");
    }
    return {contents.data(), static_cast<std::size_t>(count)};
}

}  // namespace

struct EditorLockState {
    std::filesystem::path file;
    std::string name;
    std::string target;
    std::string owner{"unknown"};
    std::string host;
    std::optional<std::int64_t> process;
    std::shared_ptr<Descriptor> directory;
    std::shared_ptr<Descriptor> node;
    struct stat identity{};
    bool owned{false};

    [[nodiscard]] bool current() const noexcept {
        struct stat present{};
        return directory && node &&
               ::fstatat(directory->get(), name.c_str(), &present, AT_SYMLINK_NOFOLLOW) == 0 &&
               same_node(identity, present);
    }

    [[nodiscard]] bool remove_if_current() const noexcept {
        return S_ISLNK(identity.st_mode) && current() &&
               ::unlinkat(directory->get(), name.c_str(), 0) == 0;
    }

    ~EditorLockState() {
        const auto saved_errno = errno;
        if (owned && process == ::getpid()) static_cast<void>(remove_if_current());
        errno = saved_errno;
    }
};

namespace {

std::mutex locks_mutex;
std::unordered_map<std::string, std::weak_ptr<EditorLockState>> owned_locks;

std::shared_ptr<EditorLockState> observe(
    const std::filesystem::path& file, const std::shared_ptr<Descriptor>& directory,
    const std::string& name
) {
    const auto descriptor = ::openat(directory->get(), name.c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC);
    if (descriptor < 0) {
        if (errno == ENOENT) return {};
        throw std::system_error(errno, std::generic_category(), "inspect editor lock");
    }
    auto state = std::make_shared<EditorLockState>();
    state->node = std::make_shared<Descriptor>(descriptor);
    state->directory = directory;
    state->file = file;
    state->name = name;
    if (::fstat(descriptor, &state->identity) != 0) {
        throw std::system_error(errno, std::generic_category(), "stat editor lock");
    }
    if (S_ISLNK(state->identity.st_mode)) {
        state->target = link_contents(descriptor);
        const auto dot = state->target.find_last_of('.');
        const auto at = state->target.find_last_of('@', dot);
        if (dot != std::string::npos && at != std::string::npos && at != 0U && at + 1U < dot) {
            const auto pid = std::string_view(state->target).substr(dot + 1U);
            std::int64_t process = 0;
            const auto parsed = std::from_chars(pid.data(), pid.data() + pid.size(), process);
            if (parsed.ec == std::errc{} && parsed.ptr == pid.data() + pid.size() &&
                process > 0 && process <= std::numeric_limits<pid_t>::max()) {
                state->owner = state->target.substr(0, dot);
                state->host = state->target.substr(at + 1U, dot - at - 1U);
                state->process = process;
            }
        }
        if (!state->process) state->owner = state->target.empty() ? "unknown" : state->target;
    }
    return state;
}

bool stale(const EditorLockState& state) {
    if (!state.process || state.host != local_host()) return false;
    return ::kill(static_cast<pid_t>(*state.process), 0) != 0 && errno == ESRCH;
}

}  // namespace

EditorLockConflict::EditorLockConflict(std::shared_ptr<EditorLockState> state)
    : std::runtime_error("file is already being edited"), state_(std::move(state)) {}

const std::filesystem::path& EditorLockConflict::file() const noexcept {
    return state_->file;
}

const std::string& EditorLockConflict::owner() const noexcept {
    return state_->owner;
}

std::optional<std::int64_t> EditorLockConflict::process_id() const noexcept {
    return state_->process;
}

EditorLock::EditorLock(std::shared_ptr<EditorLockState> state) : state_(std::move(state)) {}

std::filesystem::path EditorLock::lock_path(const std::filesystem::path& file) {
    const auto resolved = resolve_file(file);
    return resolved.parent_path() / (".#" + resolved.filename().string());
}

EditorLock EditorLock::acquire(const std::filesystem::path& file) {
    const auto resolved = resolve_file(file);
    const auto key = resolved.string();
    const std::lock_guard guard(locks_mutex);
    if (const auto found = owned_locks.find(key); found != owned_locks.end()) {
        if (auto existing = found->second.lock();
            existing && existing->process == ::getpid() && existing->current()) {
            return EditorLock(std::move(existing));
        }
    }
    const auto parent = resolved.parent_path();
    const auto descriptor = ::open(parent.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (descriptor < 0) {
        throw std::system_error(errno, std::generic_category(), "open editor lock directory");
    }
    const auto directory = std::make_shared<Descriptor>(descriptor);
    const auto name = ".#" + resolved.filename().string();
    const auto target = local_owner();
    for (unsigned attempt = 0; attempt < 8U; ++attempt) {
        if (::symlinkat(target.c_str(), descriptor, name.c_str()) == 0) {
            auto created = observe(resolved, directory, name);
            if (!created || created->target != target || !created->current()) continue;
            created->owned = true;
            owned_locks[key] = created;
            return EditorLock(std::move(created));
        }
        if (errno != EEXIST) {
            throw std::system_error(errno, std::generic_category(), "create editor lock");
        }
        auto present = observe(resolved, directory, name);
        if (!present || !present->current()) continue;
        if (stale(*present) && present->remove_if_current()) continue;
        throw EditorLockConflict(std::move(present));
    }
    throw std::runtime_error("editor lock changed repeatedly; try opening the file again");
}

EditorLock EditorLock::grab(const EditorLockConflict& conflict) {
    if (!S_ISLNK(conflict.state_->identity.st_mode)) {
        throw std::runtime_error("refusing to remove a non-symlink editor lock");
    }
    {
        const std::lock_guard guard(locks_mutex);
        if (conflict.state_->current() && !conflict.state_->remove_if_current()) {
            throw std::system_error(errno, std::generic_category(), "grab editor lock");
        }
    }
    return acquire(conflict.file());
}

EditorLock EditorLock::ignore(const EditorLockConflict& conflict) {
    return EditorLock(conflict.state_);
}

bool EditorLock::matches_file(const std::filesystem::path& file) const {
    return state_ && state_->file == resolve_file(file);
}

bool EditorLock::current() const noexcept {
    return state_ && state_->current();
}

bool EditorLock::owned() const noexcept {
    return state_ && state_->owned && state_->process == ::getpid() && state_->current();
}

void EditorLock::reset() noexcept {
    state_.reset();
}

}  // namespace mcpp
