#include "../src/shell_display.hpp"

#include <array>
#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

bool same_settings(const termios& first, const termios& second) {
    return first.c_iflag == second.c_iflag && first.c_oflag == second.c_oflag &&
        first.c_cflag == second.c_cflag && first.c_lflag == second.c_lflag &&
        cfgetispeed(&first) == cfgetispeed(&second) && cfgetospeed(&first) == cfgetospeed(&second) &&
        std::equal(std::begin(first.c_cc), std::end(first.c_cc), std::begin(second.c_cc));
}

struct Descriptor {
    int value{-1};
    ~Descriptor() { if (value >= 0) close(value); }
};

void model() {
    const std::array<std::optional<bool>, 3> values{std::nullopt, false, true};
    std::size_t plans = 0;
    for (const auto clear : values) {
        for (const auto starts : values) {
            for (const auto persistent : {false, true}) {
                for (const auto running : {false, true}) {
                    for (const auto interactive : {false, true}) {
                        const auto plan = mcpp::detail::shell_display_plan(
                            clear, starts, persistent, running, interactive
                        );
                        require(plan.clear_panels == clear.value_or(false), "panel clear policy");
                        require(plan.clear_output ==
                            (!clear.has_value() && (persistent ? !running : interactive)), "legacy output clear policy");
                        require(plan.output_only ==
                            (interactive && !persistent && !starts.value_or(true)), "output-only policy");
                        ++plans;
                    }
                }
            }
        }
    }
    Descriptor master{posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC)};
    require(master.value >= 0 && grantpt(master.value) == 0 && unlockpt(master.value) == 0, "create test PTY");
    std::array<char, 256> name{};
    require(ptsname_r(master.value, name.data(), name.size()) == 0, "name test PTY");
    Descriptor slave{open(name.data(), O_RDWR | O_NOCTTY | O_CLOEXEC)};
    require(slave.value >= 0, "open test PTY");
    termios original{};
    require(tcgetattr(slave.value, &original) == 0, "read original test settings");
    try {
        mcpp::detail::OutputInputMode raw(slave.value);
        termios current{};
        require(tcgetattr(slave.value, &current) == 0 &&
            (current.c_lflag & (ICANON | ECHO | ISIG)) == 0, "output input is raw");
        throw std::runtime_error("injected read failure");
    } catch (const std::runtime_error& error) {
        require(std::string_view(error.what()) == "injected read failure", "unexpected raw-mode failure");
    }
    termios restored{};
    require(tcgetattr(slave.value, &restored) == 0 && same_settings(original, restored),
        "restore all terminal settings during unwinding");
    bool rejected = false;
    try {
        mcpp::detail::wait_for_output_key(-1, std::chrono::microseconds{10000});
    } catch (const std::system_error&) {
        rejected = true;
    }
    require(rejected, "reject an invalid terminal descriptor");
    std::cout << "shell display: " << plans << " plans; terminal error restoration passed\n";
}

void key(const std::string_view expected_tail) {
    termios original{};
    require(tcgetattr(STDIN_FILENO, &original) == 0, "read input test settings");
    std::cout << "KEY_READY\n" << std::flush;
    mcpp::detail::wait_for_output_key(STDIN_FILENO, std::chrono::microseconds{20000});
    termios restored{};
    require(tcgetattr(STDIN_FILENO, &restored) == 0 && same_settings(original, restored),
        "restore all settings after a logical key");
    {
        mcpp::detail::OutputInputMode raw(STDIN_FILENO);
        std::string tail;
        while (tail.size() < expected_tail.size()) {
            pollfd input{STDIN_FILENO, POLLIN, 0};
            require(poll(&input, 1, 500) > 0, "queued input was consumed");
            char character = 0;
            require(read(STDIN_FILENO, &character, 1) == 1, "read queued input");
            tail += character;
        }
        require(tail == expected_tail, "queued input changed");
        raw.restore();
    }
    require(tcgetattr(STDIN_FILENO, &restored) == 0 && same_settings(original, restored),
        "restore settings after inspecting queued input");
    std::cout << "KEY_OK\n" << std::flush;
}

}  // namespace

int main(const int argc, const char* const argv[]) {
    try {
        if (argc == 1 || (argc == 2 && std::string_view(argv[1]) == "model")) model();
        else if (argc == 3 && std::string_view(argv[1]) == "key") key(argv[2]);
        else return 2;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "shell display test: " << error.what() << '\n';
        return 1;
    }
}
