#pragma once

#include <atomic>
#include <memory>
#include <stdexcept>

namespace mcpp {

class OperationCancelled final : public std::runtime_error {
public:
    OperationCancelled()
        : std::runtime_error("operation cancelled") {}
};

class Cancellation {
public:
    Cancellation()
        : state_(std::make_shared<std::atomic_bool>(false)) {}

    void request() const noexcept {
        state_->store(true, std::memory_order_release);
    }

    [[nodiscard]] bool requested() const noexcept {
        return state_->load(std::memory_order_acquire);
    }

    void throw_if_requested() const {
        if (requested()) {
            throw OperationCancelled{};
        }
    }

private:
    std::shared_ptr<std::atomic_bool> state_;
};

}  // namespace mcpp
