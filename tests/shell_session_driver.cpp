#include "mcpp/shell.hpp"

#include <array>
#include <cerrno>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace {

std::string field(const int fd) {
    std::string value;
    char character = 0;
    for (;;) {
        const auto count = read(fd, &character, 1);
        if (count < 0 && errno == EINTR) continue;
        if (count != 1) throw std::runtime_error("driver input closed");
        if (character == '\0') return value;
        value += character;
    }
}

void event(const int fd, const std::string& kind, const mcpp::ShellResult& result = {},
           const std::string& error = {}) {
    std::string message;
    for (const auto& value : std::array<std::string, 6>{
             kind, std::to_string(result.status), result.exited ? "1" : "0",
             result.busy ? "1" : "0", result.directory, error}) {
        message += value;
        message += '\0';
    }
    if (write(fd, message.data(), message.size()) != static_cast<ssize_t>(message.size())) {
        throw std::runtime_error("driver output failed");
    }
}

}  // namespace

int main(const int argc, const char* const argv[]) {
    if (argc != 3) return 2;
    const auto input = std::atoi(argv[1]);
    const auto output = std::atoi(argv[2]);
    mcpp::ShellSession shell;
    shell.set_notify([output] { event(output, "N"); });
    for (;;) {
        try {
            const auto action = field(input);
            if (action == "Q") return 0;
            if (action == "C") {
                shell.close();
                event(output, "R");
            } else if (action == "P") {
                const auto result = shell.poll();
                if (result) event(output, "R", *result);
                else event(output, "I");
            } else if (action == "S") {
                const auto directory = field(input);
                const auto command = field(input);
                event(output, "R", shell.show(directory, command));
            } else if (action == "A") {
                event(output, mcpp::ShellSession::supported() ? "Y" : "U");
            } else {
                return 2;
            }
        } catch (const mcpp::ShellBusy& error) {
            event(output, "B", {}, error.what());
        } catch (const std::exception& error) {
            event(output, "E", {}, error.what());
        }
    }
}
