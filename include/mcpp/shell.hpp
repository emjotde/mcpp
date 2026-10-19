#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

namespace mcpp {

struct ShellResult {
    std::string directory;
    int status{0};
    bool exited{false};
    bool busy{false};
};

class ShellBusy final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class ShellSession {
public:
    ShellSession();
    ~ShellSession();
    ShellSession(const ShellSession&) = delete;
    ShellSession& operator=(const ShellSession&) = delete;

    [[nodiscard]] static bool supported();
    [[nodiscard]] bool running() const;
    [[nodiscard]] bool busy() const;
    void set_notify(std::function<void()> notify);
    [[nodiscard]] std::optional<ShellResult> poll();
    void close();
    ShellResult show(const std::string& directory, const std::string& command = {});

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mcpp
