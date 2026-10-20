#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

namespace mcpp {

struct EditorLockState;

class EditorLockConflict final : public std::runtime_error {
public:
    [[nodiscard]] const std::filesystem::path& file() const noexcept;
    [[nodiscard]] const std::string& owner() const noexcept;
    [[nodiscard]] std::optional<std::int64_t> process_id() const noexcept;

private:
    explicit EditorLockConflict(std::shared_ptr<EditorLockState> state);
    std::shared_ptr<EditorLockState> state_;
    friend class EditorLock;
};

// Copies share ownership; moving or switching editor buffers does not release a lock.
class EditorLock {
public:
    EditorLock() = default;

    [[nodiscard]] static EditorLock acquire(const std::filesystem::path& file);
    [[nodiscard]] static EditorLock grab(const EditorLockConflict& conflict);
    [[nodiscard]] static EditorLock ignore(const EditorLockConflict& conflict);
    [[nodiscard]] static std::filesystem::path lock_path(const std::filesystem::path& file);

    [[nodiscard]] bool matches_file(const std::filesystem::path& file) const;
    [[nodiscard]] bool current() const noexcept;
    [[nodiscard]] bool owned() const noexcept;
    void reset() noexcept;

private:
    explicit EditorLock(std::shared_ptr<EditorLockState> state);
    std::shared_ptr<EditorLockState> state_;
};

}  // namespace mcpp
