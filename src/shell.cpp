#include "mcpp/shell.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <iostream>
#include <mutex>
#include <poll.h>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <termios.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace mcpp {
namespace {

struct Fd {
    int value{-1};
    ~Fd() { reset(); }
    Fd() = default;
    explicit Fd(const int fd) : value(fd) {}
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    void reset(const int fd = -1) {
        if (value >= 0) {
            close(value);
        }
        value = fd;
    }
};

[[noreturn]] void fail(const std::string& action) {
    throw std::system_error(errno, std::generic_category(), action);
}

void write_all(const int fd, std::string_view data, const bool no_signal = false) {
    while (!data.empty()) {
        const auto count = no_signal ? send(fd, data.data(), data.size(), MSG_NOSIGNAL)
                                     : write(fd, data.data(), data.size());
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            fail("write shell data");
        }
        if (count == 0) throw std::runtime_error("shell write returned zero bytes");
        data.remove_prefix(static_cast<std::size_t>(count));
    }
}

struct RawTerminal {
    termios saved{};
    RawTerminal() {
        if (tcgetattr(STDIN_FILENO, &saved) != 0) {
            fail("read terminal settings");
        }
        auto raw = saved;
        cfmakeraw(&raw);
        if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) {
            fail("set shell terminal settings");
        }
    }
    ~RawTerminal() {
        if (tcsetattr(STDIN_FILENO, TCSANOW, &saved) != 0) {
            std::cerr << "mcpp: cannot restore terminal settings after shell: "
                      << std::generic_category().message(errno) << '\n';
        }
    }
};

int parse_number(const std::string& value) {
    int result = 0;
    const auto [end, error] = std::from_chars(
        value.data(), value.data() + value.size(), result
    );
    if (error != std::errc{} || end != value.data() + value.size()) {
        throw std::runtime_error("invalid shell control response");
    }
    return result;
}

}  // namespace

struct ShellSession::Impl {
    Fd master;
    Fd requests;
    Fd notifications;
    pid_t pid{-1};
    std::jthread reader;
    std::atomic<bool> output_closed{false};
    std::mutex output_mutex;
    std::condition_variable output_condition;
    std::uint64_t output_sync{0};
    std::uint64_t hidden_count{0};
    std::string sync_marker;
    std::string output;
    std::string output_error;
    bool output_omitted{false};
    std::string control;
    std::filesystem::path temporary;
    std::string directory;
    bool hidden{false};
    bool draft{false};
    bool initialized{false};
    bool ready{false};
    bool backgrounded{false};
    bool auto_return{false};
    std::string hidden_directory;
    std::string pending_command;
    std::function<void()> notify;
    std::atomic_bool notification_pending{false};
    int status{0};

    ~Impl() { stop(); }

    void stop() {
        requests.reset();
        if (pid > 0) {
            if (kill(pid, SIGHUP) != 0 && errno != ESRCH) {
                std::cerr << "mcpp: cannot stop persistent shell: "
                          << std::generic_category().message(errno) << '\n';
            }
            for (int attempt = 0; attempt < 20; ++attempt) {
                const auto waited = waitpid(pid, nullptr, WNOHANG);
                if (waited == pid || (waited < 0 && errno == ECHILD)) {
                    pid = -1;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (pid > 0) {
                if (kill(pid, SIGKILL) == 0) {
                    while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {}
                } else if (errno != ESRCH) {
                    std::cerr << "mcpp: cannot terminate persistent shell: "
                              << std::generic_category().message(errno) << '\n';
                }
                pid = -1;
            }
        }
        reader.request_stop();
        if (reader.joinable()) {
            reader.join();
        }
        master.reset();
        notifications.reset();
        if (!temporary.empty()) {
            if (unlink((temporary / "bashrc").c_str()) != 0 && errno != ENOENT) {
                std::cerr << "mcpp: cannot remove shell startup file: "
                          << std::generic_category().message(errno) << '\n';
            }
            if (rmdir(temporary.c_str()) != 0 && errno != ENOENT) {
                std::cerr << "mcpp: cannot remove shell startup directory: "
                          << std::generic_category().message(errno) << '\n';
            }
            temporary.clear();
        }
        hidden = false;
        draft = false;
        initialized = false;
        ready = backgrounded = auto_return = false;
        hidden_directory.clear();
        pending_command.clear();
        notification_pending.store(false);
        control.clear();
    }

    struct Message {
        char kind;
        std::string directory;
        int value;
    };

    void read_notifications() {
        if (notifications.value < 0) return;
        std::array<char, 8192> data{};
        for (;;) {
            const auto count = read(notifications.value, data.data(), data.size());
            if (count > 0) {
                control.append(data.data(), static_cast<std::size_t>(count));
                if (control.size() > 65536U) throw std::runtime_error("shell control response is too large");
            } else if (count == 0 || (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))) {
                break;
            } else if (errno != EINTR) {
                fail("read shell response");
            }
        }
        notification_pending.store(false);
    }

    std::optional<Message> next_message() {
        std::array<std::string, 3> fields;
        std::size_t offset = 0;
        for (auto& field : fields) {
            const auto end = control.find('\0', offset);
            if (end == std::string::npos) return std::nullopt;
            field = control.substr(offset, end - offset);
            offset = end + 1U;
        }
        if (fields[0] != "P" && fields[0] != "H" && fields[0] != "R") {
            throw std::runtime_error("unknown shell control response");
        }
        control.erase(0, offset);
        const bool prompt = fields[0] == "P";
        Message message{fields[0][0], fields[prompt ? 2U : 1U], parse_number(fields[prompt ? 1U : 2U])};
        if (message.directory.empty() || !std::filesystem::path(message.directory).is_absolute() ||
            message.value < 0 || message.value > 255 || (message.kind == 'H' && message.value > 1)) {
            throw std::runtime_error("invalid shell control response");
        }
        return message;
    }

    void observe_prompt(const Message& message) {
        status = message.value;
        directory = message.directory;
        ready = initialized = true;
    }

    bool submit_pending_command() {
        if (pending_command.empty()) return false;
        write_all(master.value, "\033[200~" + pending_command + "\033[201~\r");
        pending_command.clear();
        ready = false;
        auto_return = true;
        return true;
    }

    std::optional<ShellResult> poll() {
        if (pid <= 0) return std::nullopt;
        {
            const std::lock_guard lock(output_mutex);
            if (!output_error.empty()) throw std::runtime_error(output_error);
        }
        read_notifications();
        std::optional<ShellResult> result;
        while (const auto message = next_message()) {
            if (message->kind != 'P') throw std::runtime_error("unexpected shell response while panels are visible");
            observe_prompt(*message);
            if (submit_pending_command()) continue;
            auto_return = false;
            result = ShellResult{directory, status, false, false};
        }
        int child_status = 0;
        const auto waited = waitpid(pid, &child_status, WNOHANG);
        if (waited < 0 && errno != EINTR) fail("wait for persistent shell");
        if (waited == pid) {
            pid = -1;
            status = WIFEXITED(child_status) ? WEXITSTATUS(child_status) : 128 + WTERMSIG(child_status);
            const auto final_directory = directory;
            const bool started = initialized;
            stop();
            if (!started) throw std::runtime_error("Bash exited before providing a prompt (exit " +
                                                   std::to_string(status) + ")");
            return ShellResult{final_directory, status, true, false};
        }
        if (output_closed.load()) throw std::runtime_error("Bash closed its terminal unexpectedly");
        return result;
    }

    void start(const std::string& cwd) {
        stop();
        output_closed.store(false);
        {
            std::lock_guard lock(output_mutex);
            output.clear();
            output_sync = hidden_count = 0;
            output_error.clear();
            output_omitted = false;
        }
        std::array<int, 2> incoming{};
        if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, incoming.data()) != 0) {
            fail("create shell request channel");
        }
        Fd child_requests(incoming[0]);
        requests.reset(incoming[1]);
        std::array<int, 2> outgoing{};
        if (pipe2(outgoing.data(), O_CLOEXEC) != 0) {
            fail("create shell response pipe");
        }
        notifications.reset(outgoing[0]);
        const auto notification_flags = fcntl(notifications.value, F_GETFL);
        if (notification_flags < 0 ||
            fcntl(notifications.value, F_SETFL, notification_flags | O_NONBLOCK) < 0) {
            fail("set shell response channel mode");
        }
        Fd child_notifications(outgoing[1]);
        master.reset(posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC));
        if (master.value < 0 || grantpt(master.value) != 0 ||
            unlockpt(master.value) != 0) {
            fail("create shell pseudo-terminal");
        }
        std::array<char, 256> slave_name{};
        const auto name_error = ptsname_r(
            master.value, slave_name.data(), slave_name.size()
        );
        if (name_error != 0) {
            throw std::system_error(
                name_error, std::generic_category(), "name shell pseudo-terminal"
            );
        }
        Fd slave(open(slave_name.data(), O_RDWR | O_NOCTTY | O_CLOEXEC));
        if (slave.value < 0) {
            fail("open shell pseudo-terminal");
        }
        termios settings{};
        if (tcgetattr(STDIN_FILENO, &settings) != 0 ||
            tcsetattr(slave.value, TCSANOW, &settings) != 0) {
            fail("initialize shell terminal");
        }
        winsize size{};
        if (ioctl(STDIN_FILENO, TIOCGWINSZ, &size) == 0) {
            ioctl(slave.value, TIOCSWINSZ, &size);
        }
        const auto prefix = (std::filesystem::temp_directory_path() / "mcpp-shell-XXXXXX").string();
        std::vector<char> pattern(prefix.begin(), prefix.end());
        pattern.push_back('\0');
        const auto* created = mkdtemp(pattern.data());
        if (created == nullptr) {
            fail("create private shell startup directory");
        }
        temporary = created;
        const auto sync_token = temporary.filename().string();
        sync_marker = "\033]777;mcpp-sync:" + sync_token + "\007";
        const auto startup = temporary / "bashrc";
        Fd rc(open(startup.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600));
        if (rc.value < 0) {
            fail("create shell startup file");
        }
        auto script = std::string(R"(__mcpp_data="${XDG_DATA_HOME:-$HOME/.local/share}/mcpp"
if [[ -r "$__mcpp_data/bashrc" ]]; then builtin source "$__mcpp_data/bashrc"
elif [[ -r "$HOME/.bashrc" ]]; then builtin source "$HOME/.bashrc"; fi
if [[ -r "$__mcpp_data/inputrc" ]]; then builtin bind -f "$__mcpp_data/inputrc"; fi
unset __mcpp_data
if ! IFS= builtin read -r -d '' __mcpp_initial_directory <&@IN@; then builtin exit 125; fi
builtin cd -- "$__mcpp_initial_directory" || builtin exit 126
unset __mcpp_initial_directory
__mcpp_before_prompt() {
    __mcpp_last_status=$?
    return "$__mcpp_last_status"
}
__mcpp_prompt() {
    builtin printf 'P\0%d\0%s\0' "$__mcpp_last_status" "$PWD" >&@OUT@
    return "$__mcpp_last_status"
}
__mcpp_toggle() {
    local __mcpp_path __mcpp_changed=0 __mcpp_draft=0
    [[ -n ${READLINE_LINE-} ]] && __mcpp_draft=1
    if ! builtin printf '\033]777;mcpp-sync:@SYNC@\007' > /dev/tty; then builtin exit 125; fi
    builtin printf 'H\0%s\0%d\0' "$PWD" "$__mcpp_draft" >&@OUT@
    if ! IFS= builtin read -r -d '' __mcpp_path <&@IN@; then builtin exit; fi
    if [[ "$PWD" != "$__mcpp_path" ]]; then
        builtin cd -- "$__mcpp_path" || __mcpp_changed=$?
    fi
    builtin printf 'R\0%s\0%d\0' "$PWD" "$__mcpp_changed" >&@OUT@
}
builtin bind -x '"\C-o":__mcpp_toggle'
builtin bind 'set enable-bracketed-paste on'
PROMPT_COMMAND=(__mcpp_before_prompt "${PROMPT_COMMAND[@]}" __mcpp_prompt)
)");
        for (const auto& [token, value] : {
                 std::pair{std::string("@IN@"), child_requests.value},
                 std::pair{std::string("@OUT@"), child_notifications.value},
             }) {
            std::size_t offset = 0;
            while ((offset = script.find(token, offset)) != std::string::npos) {
                const auto replacement = std::to_string(value);
                script.replace(offset, token.size(), replacement);
                offset += replacement.size();
            }
        }
        if (const auto offset = script.find("@SYNC@"); offset != std::string::npos) {
            script.replace(offset, 6U, sync_token);
        }
        write_all(rc.value, script);
        rc.reset();
        const auto* executable = std::getenv("SHELL");
        if (executable == nullptr || *executable == '\0') {
            executable = "/bin/bash";
        }
        pid = fork();
        if (pid < 0) {
            fail("fork persistent shell");
        }
        if (pid == 0) {
            if (setsid() < 0 || ioctl(slave.value, TIOCSCTTY, 0) < 0 ||
                dup2(slave.value, STDIN_FILENO) < 0 ||
                dup2(slave.value, STDOUT_FILENO) < 0 ||
                dup2(slave.value, STDERR_FILENO) < 0 ||
                fcntl(child_requests.value, F_SETFD, 0) < 0 ||
                fcntl(child_notifications.value, F_SETFD, 0) < 0 ||
                chdir(cwd.c_str()) != 0) {
                _exit(126);
            }
            execlp(
                executable, "bash", "--rcfile", startup.c_str(), "-i",
                static_cast<char*>(nullptr)
            );
            _exit(127);
        }
        directory = cwd;
        status = 0;
        write_all(requests.value, cwd + '\0', true);
        const auto child_pid = pid;
        reader = std::jthread([this, child_pid](const std::stop_token token) {
            try {
                std::array<char, 16384> data{};
                std::string pending;
                while (!token.stop_requested()) {
                    siginfo_t child{};
                    if (waitid(P_PID, static_cast<id_t>(child_pid), &child, WEXITED | WNOHANG | WNOWAIT) != 0) {
                        if (errno == ECHILD) break;
                        if (errno == EINTR) continue;
                        fail("observe persistent shell");
                    }
                    if (child.si_pid == child_pid) break;
                    std::array<pollfd, 2> descriptors{{
                        {master.value, POLLIN, 0},
                        {notification_pending.load() ? -1 : notifications.value, POLLIN, 0},
                    }};
                    const auto polled = ::poll(descriptors.data(), descriptors.size(), 100);
                    if (polled < 0 && errno == EINTR) continue;
                    if (polled == 0) continue;
                    if (polled > 0 && descriptors[1].revents != 0 &&
                        !notification_pending.exchange(true) && notify) notify();
                    if (polled > 0 && descriptors[0].revents == 0) continue;
                    const std::lock_guard lock(output_mutex);
                    const auto count = polled < 0 ? -1 : read(master.value, data.data(), data.size());
                    if (count <= 0) {
                        if (count < 0 && errno == EINTR) continue;
                        if (count < 0 && errno != EIO) {
                            output_error = std::system_error(
                                errno, std::generic_category(), "read shell output"
                            ).what();
                        }
                        break;
                    }
                    pending.append(data.data(), static_cast<std::size_t>(count));
                    consume_output_locked(pending);
                    output_condition.notify_all();
                }
            } catch (const std::exception& error) {
                const std::lock_guard lock(output_mutex);
                output_error = error.what();
            }
            output_closed.store(true);
            output_condition.notify_all();
            if (!token.stop_requested() && !notification_pending.exchange(true) && notify) {
                try {
                    notify();
                } catch (const std::exception& error) {
                    std::cerr << "mcpp: cannot report shell completion: " << error.what() << '\n';
                }
            }
        });
    }

    void append_output_locked(const std::string_view value) {
        if (output.size() + value.size() > 1024U * 1024U) {
            output.clear();
            output_omitted = true;
        }
        output.append(value);
    }

    void consume_output_locked(std::string& pending) {
        for (;;) {
            const auto marker = pending.find(sync_marker);
            if (marker == std::string::npos) break;
            append_output_locked(std::string_view(pending).substr(0, marker));
            pending.erase(0, marker + sync_marker.size());
            ++output_sync;
        }
        auto retained = std::min(pending.size(), sync_marker.size() - 1U);
        while (retained > 0U && !std::string_view(sync_marker).starts_with(
                   std::string_view(pending).substr(pending.size() - retained))) --retained;
        append_output_locked(std::string_view(pending).substr(0, pending.size() - retained));
        pending.erase(0, pending.size() - retained);
    }

    void flush_output(const bool drain = false) {
        std::string pending;
        bool omitted = false;
        {
            std::unique_lock lock(output_mutex);
            if (drain) {
                // Control-pipe messages can arrive before the preceding PTY output.
                output_condition.wait(lock, [&] { return output_sync >= hidden_count || output_closed.load(); });
                if (output_sync < hidden_count) throw std::runtime_error("shell output closed before the prompt");
            }
            if (!output_error.empty()) {
                throw std::runtime_error(output_error);
            }
            pending.swap(output);
            omitted = std::exchange(output_omitted, false);
        }
        if (omitted) {
            write_all(STDOUT_FILENO, "\033[0m\r\n[mcpp: older background shell output omitted]\r\n");
        }
        write_all(STDOUT_FILENO, pending);
    }

    ShellResult show(const std::string& cwd, const std::string& command) {
        if (hidden && draft && !command.empty()) {
            throw ShellBusy(
                "shell has unfinished input; use Ctrl-O to finish or clear it"
            );
        }
        if (pid > 0) {
            read_notifications();
            while (const auto message = next_message()) {
                if (message->kind != 'P') throw std::runtime_error("unexpected pending shell response");
                observe_prompt(*message);
                static_cast<void>(submit_pending_command());
            }
        }
        if (backgrounded && !command.empty() && (!ready || tcgetpgrp(master.value) != pid)) {
            throw ShellBusy("shell is already running a command; use Ctrl-O to resume it");
        }
        if (pid <= 0) start(cwd);
        if (!command.empty()) pending_command = command;
        auto requested_directory = cwd;
        if (backgrounded && ready && cwd == hidden_directory) requested_directory = directory;
        RawTerminal raw;
        bool submitted = false;
        bool returning = false;
        bool directory_failed = false;
        bool synchronizing = backgrounded && ready;
        if (synchronizing) {
            auto_return = false;
            write_all(master.value, "\017");
        }
        if (backgrounded) {
            const auto foreground = tcgetpgrp(master.value);
            if (foreground > 0 && kill(-foreground, SIGWINCH) != 0 && errno != ESRCH) {
                std::cerr << "mcpp: cannot request shell redraw: "
                          << std::generic_category().message(errno) << '\n';
            }
            backgrounded = false;
        }
        if (hidden) {
            write_all(requests.value, requested_directory + '\0', true);
            hidden = false;
        }
        const auto submit = [&] {
            submitted = submit_pending_command() || submitted;
        };
        const auto requested_start = std::chrono::steady_clock::now();
        while (true) {
            flush_output();
            int child_status = 0;
            if (waitpid(pid, &child_status, WNOHANG) == pid) {
                pid = -1;
                status = WIFEXITED(child_status)
                             ? WEXITSTATUS(child_status)
                             : 128 + WTERMSIG(child_status);
                const bool started = initialized;
                reader.request_stop();
                if (reader.joinable()) {
                    reader.join();
                }
                stop();
                flush_output();
                if (!started) throw std::runtime_error("Bash exited before providing a prompt (exit " +
                                                       std::to_string(status) + ")");
                return {directory, status, true};
            }
            winsize size{};
            if (ioctl(STDIN_FILENO, TIOCGWINSZ, &size) == 0) {
                ioctl(master.value, TIOCSWINSZ, &size);
            }
            std::array<pollfd, 2> events{{
                {STDIN_FILENO, POLLIN, 0},
                {notifications.value, POLLIN, 0},
            }};
            const auto polled = ::poll(events.data(), events.size(), 30);
            if (polled < 0) {
                if (errno == EINTR) {
                    continue;
                }
                fail("wait for shell");
            }
            std::array<char, 8192> data{};
            if ((events[1].revents & (POLLIN | POLLHUP)) != 0) {
                read_notifications();
                while (const auto message = next_message()) {
                    if (message->kind == 'H') {
                        ++hidden_count;
                        directory = message->directory;
                        draft = message->value != 0;
                        hidden = true;
                        if (synchronizing) {
                            write_all(requests.value, requested_directory + '\0', true);
                            hidden = false;
                            synchronizing = false;
                            continue;
                        }
                        auto_return = false;
                        flush_output(true);
                        if (directory_failed) {
                            throw std::runtime_error("shell cannot change directory to " + requested_directory);
                        }
                        return {directory, status, false, false};
                    }
                    if (message->kind == 'R') {
                        directory = message->directory;
                        if (message->value != 0) {
                            directory_failed = true;
                            write_all(master.value, "\017");
                        } else if (!pending_command.empty()) {
                            submit();
                        }
                    } else {
                        observe_prompt(*message);
                        if (synchronizing) continue;
                        if (!pending_command.empty()) {
                            submit();
                        } else if (auto_return && !returning) {
                            write_all(master.value, "\017");
                            returning = true;
                        }
                    }
                }
            }
            if ((events[0].revents & POLLIN) != 0) {
                // Leave bytes following Ctrl-O available to the manager.
                const auto count = read(STDIN_FILENO, data.data(), 1);
                if (count <= 0) {
                    throw std::runtime_error("shell terminal input closed");
                }
                for (std::ptrdiff_t index = 0; index < count; ++index) {
                    if (data[static_cast<std::size_t>(index)] == '\017' &&
                        (!ready || tcgetpgrp(master.value) != pid)) {
                        backgrounded = true;
                        hidden_directory = requested_directory;
                        flush_output();
                        return {directory, status, false, true};
                    } else {
                        if (data[static_cast<std::size_t>(index)] == '\r' ||
                            data[static_cast<std::size_t>(index)] == '\n') ready = false;
                        write_all(
                            master.value,
                            std::string_view(&data[static_cast<std::size_t>(index)], 1)
                        );
                    }
                }
            }
            if (!command.empty() && !submitted &&
                std::chrono::steady_clock::now() - requested_start >
                    std::chrono::seconds(10)) {
                throw std::runtime_error("Bash did not provide a command prompt");
            }
        }
    }
};

ShellSession::ShellSession() : impl_(std::make_unique<Impl>()) {}
ShellSession::~ShellSession() = default;

bool ShellSession::supported() {
    const auto* shell = std::getenv("SHELL");
    return shell == nullptr || *shell == '\0' ||
           std::filesystem::path(shell).filename() == "bash";
}

bool ShellSession::running() const {
    return impl_->pid > 0 && !impl_->output_closed.load();
}

bool ShellSession::busy() const {
    return impl_->pid > 0 && (!impl_->ready || tcgetpgrp(impl_->master.value) != impl_->pid);
}

void ShellSession::set_notify(std::function<void()> notify) {
    if (impl_->pid > 0) throw std::logic_error("cannot replace a running shell notification handler");
    impl_->notify = std::move(notify);
}

std::optional<ShellResult> ShellSession::poll() {
    try {
        return impl_->poll();
    } catch (...) {
        impl_->stop();
        throw;
    }
}

void ShellSession::close() {
    impl_->stop();
}

ShellResult ShellSession::show(const std::string& directory, const std::string& command) {
    try {
        return impl_->show(directory, command);
    } catch (const ShellBusy&) {
        throw;
    } catch (...) {
        if (!impl_->hidden) {
            impl_->stop();
        }
        throw;
    }
}

}  // namespace mcpp
