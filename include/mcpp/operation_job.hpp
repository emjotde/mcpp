#pragma once

#include "mcpp/operations.hpp"

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <utility>

namespace mcpp {

enum class OperationChoice {
    overwrite,
    append,
    reget,
    skip,
    overwrite_all,
    skip_all,
    older,
    retry,
    ignore_all,
    cancel,
};

struct OperationQuestion {
    std::uint64_t sequence{0};
    Entry source;
    std::optional<Entry> destination;
    std::string error;
    CollisionOptions collision_options;
    bool recursive_delete{false};
};

class OperationJobControl {
public:
    struct Snapshot {
        TransferProgress progress;
        bool running{true};
        bool paused{false};
        bool waiting{false};
    };

    [[nodiscard]] const Cancellation& cancellation() const noexcept {
        return cancellation_;
    }

    void cancel() {
        {
            const std::lock_guard lock(mutex_);
            cancellation_.request();
            paused_ = false;
        }
        changed_.notify_all();
    }

    bool toggle_pause() {
        bool paused;
        {
            const std::lock_guard lock(mutex_);
            if (!running_ || cancellation_.requested()) return false;
            paused = paused_ = !paused_;
        }
        changed_.notify_all();
        return paused;
    }

    void skip() {
        {
            const std::lock_guard lock(mutex_);
            skip_ = true;
            paused_ = false;
        }
        changed_.notify_all();
    }

    void wait_until_resumed() {
        std::unique_lock lock(mutex_);
        wait_until_resumed(lock);
    }

    // Only one UI update may be queued, even if a provider reports every block.
    bool checkpoint(const TransferProgress& progress, const bool allow_skip) {
        std::unique_lock lock(mutex_);
        progress_ = progress;
        wait_until_resumed(lock);
        if (std::exchange(skip_, false) && allow_skip) throw SkipCurrent{};
        return !std::exchange(progress_queued_, true);
    }

    TransferProgress consume_progress() {
        const std::lock_guard lock(mutex_);
        progress_queued_ = false;
        return progress_;
    }

    std::uint64_t begin_question(OperationQuestion question) {
        std::unique_lock lock(mutex_);
        wait_until_resumed(lock);
        if (question_) throw std::logic_error("a job already has a pending question");
        question.sequence = ++question_sequence_;
        answer_.reset();
        question_ = std::move(question);
        return question_sequence_;
    }

    [[nodiscard]] std::optional<OperationQuestion> question() const {
        const std::lock_guard lock(mutex_);
        if (answer_ || cancellation_.requested() || !running_) return {};
        return question_;
    }

    bool answer(const std::uint64_t sequence, const OperationChoice choice) {
        {
            const std::lock_guard lock(mutex_);
            if (!question_ || question_->sequence != sequence || answer_ ||
                cancellation_.requested() || !running_) return false;
            answer_ = choice;
        }
        changed_.notify_all();
        return true;
    }

    OperationChoice wait_for_answer(const std::uint64_t sequence) {
        std::unique_lock lock(mutex_);
        changed_.wait(lock, [&] {
            return !running_ || cancellation_.requested() ||
                   (question_ && question_->sequence == sequence && answer_);
        });
        if (!running_ || cancellation_.requested()) {
            question_.reset();
            answer_.reset();
            throw OperationCancelled{};
        }
        const auto answer = *answer_;
        question_.reset();
        answer_.reset();
        return answer;
    }

    void finish() {
        {
            const std::lock_guard lock(mutex_);
            running_ = false;
            paused_ = false;
            question_.reset();
            answer_.reset();
            progress_queued_ = false;
        }
        changed_.notify_all();
    }

    [[nodiscard]] Snapshot snapshot() const {
        const std::lock_guard lock(mutex_);
        return {progress_, running_, paused_,
                question_.has_value() && !answer_ && !cancellation_.requested()};
    }

private:
    void wait_until_resumed(std::unique_lock<std::mutex>& lock) {
        changed_.wait(lock, [&] { return !running_ || !paused_ || cancellation_.requested(); });
        if (!running_) throw OperationCancelled{};
        cancellation_.throw_if_requested();
    }

    Cancellation cancellation_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    TransferProgress progress_;
    bool running_{true};
    bool paused_{false};
    bool skip_{false};
    bool progress_queued_{false};
    std::uint64_t question_sequence_{0};
    std::optional<OperationQuestion> question_;
    std::optional<OperationChoice> answer_;
};

}  // namespace mcpp
