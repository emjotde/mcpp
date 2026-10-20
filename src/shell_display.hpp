#pragma once

#include "../third_party/ftxui/src/ftxui/component/terminal_input_parser.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <iostream>
#include <limits>
#include <optional>
#include <poll.h>
#include <stdexcept>
#include <system_error>
#include <termios.h>
#include <unistd.h>

#include <ftxui/component/event.hpp>

namespace mcpp::detail {

struct ShellDisplayPlan {
    bool clear_panels;
    bool clear_output;
    bool output_only;
};

inline ShellDisplayPlan shell_display_plan(
    const std::optional<bool> clear_before_exec,
    const std::optional<bool> show_output_starts_shell,
    const bool persistent,
    const bool running,
    const bool interactive
) {
    return {
        clear_before_exec.value_or(false),
        !clear_before_exec.has_value() && (persistent ? !running : interactive),
        interactive && !persistent && !show_output_starts_shell.value_or(true)
    };
}

class OutputInputMode {
public:
    explicit OutputInputMode(const int descriptor) : descriptor_(descriptor) {
        if (tcgetattr(descriptor_, &original_) != 0) {
            throw std::system_error(errno, std::generic_category(), "read output terminal settings");
        }
        auto raw = original_;
        cfmakeraw(&raw);
        if (tcsetattr(descriptor_, TCSANOW, &raw) != 0) {
            const auto error = errno;
            static_cast<void>(tcsetattr(descriptor_, TCSANOW, &original_));
            throw std::system_error(error, std::generic_category(), "set output terminal settings");
        }
    }

    OutputInputMode(const OutputInputMode&) = delete;
    OutputInputMode& operator=(const OutputInputMode&) = delete;

    ~OutputInputMode() {
        if (active_ && tcsetattr(descriptor_, TCSANOW, &original_) != 0) {
            std::cerr << "mcpp: cannot restore terminal settings after showing output\n";
        }
    }

    void restore() {
        if (tcsetattr(descriptor_, TCSANOW, &original_) != 0) {
            throw std::system_error(errno, std::generic_category(), "restore output terminal settings");
        }
        active_ = false;
    }

private:
    int descriptor_;
    termios original_{};
    bool active_{true};
};

inline void wait_for_output_key(const int descriptor, const std::chrono::microseconds timeout) {
    OutputInputMode input_mode(descriptor);
    bool complete = false;
    unsigned continuations = 0;
    ftxui::TerminalInputParser parser([&](const ftxui::Event& event) {
        if (event.is_cursor_position() || event.is_cursor_shape()) return;
        const auto& key = event.input();
        if (key.size() == 2U && key.front() == '\033') {
            // FTXUI reports Alt plus a UTF-8 leading byte before its continuation bytes.
            const auto character = static_cast<unsigned char>(key.back());
            continuations = character >= 0xc2U && character <= 0xdfU ? 1U
                : character >= 0xe0U && character <= 0xefU ? 2U
                : character >= 0xf0U && character <= 0xf4U ? 3U : 0U;
        }
        complete = true;
    });
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(timeout).count();
    const auto delay = static_cast<int>(std::clamp(
        milliseconds, std::chrono::milliseconds::rep{1},
        static_cast<std::chrono::milliseconds::rep>(std::numeric_limits<int>::max())
    ));
    bool started = false;
    while (!complete || continuations != 0U) {
        pollfd input{descriptor, POLLIN, 0};
        const auto ready = poll(&input, 1, started ? delay : -1);
        if (ready < 0) {
            if (errno == EINTR) continue;
            throw std::system_error(errno, std::generic_category(), "wait for output key");
        }
        if (ready == 0) {
            continuations = 0;
            parser.Timeout(std::numeric_limits<int>::max());
            continue;
        }
        char character = 0;
        const auto count = read(descriptor, &character, 1);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) {
            throw std::system_error(errno, std::generic_category(), "read output key");
        }
        if (count == 0) throw std::runtime_error("output terminal input closed");
        // The existing parser consumes one logical key without reading queued keys after it.
        if (continuations != 0U) --continuations;
        parser.Add(character);
        started = true;
    }
    input_mode.restore();
}

}  // namespace mcpp::detail
