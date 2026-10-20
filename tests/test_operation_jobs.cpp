#include "mcpp/operation_job.hpp"

#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {

using namespace mcpp;
using namespace std::chrono_literals;

void check(const bool condition, const std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

struct CancelJobs {
    OperationJobControl& first;
    OperationJobControl& second;
    ~CancelJobs() {
        first.cancel();
        second.cancel();
    }
};

template<class T>
T completed(std::future<T>& result) {
    check(result.wait_for(2s) == std::future_status::ready, "job did not wake");
    return result.get();
}

void pause_and_skip() {
    OperationJobControl first, second;
    std::future<bool> paused;
    const CancelJobs cleanup{first, second};
    check(first.toggle_pause(), "first job did not pause");
    paused = std::async(std::launch::async, [&] {
        return first.checkpoint({1, 2, 0, 1, "first"}, true);
    });
    check(second.checkpoint({3, 4, 0, 1, "second"}, true), "second progress was blocked");
    second.skip();
    bool skipped = false;
    try { static_cast<void>(second.checkpoint({3, 4, 0, 1, "second"}, true)); }
    catch (const SkipCurrent&) { skipped = true; }
    check(skipped, "second job did not skip");
    second.cancel();
    check(!first.cancellation().requested() && first.snapshot().paused,
          "second job controls changed first job");
    check(!first.toggle_pause(), "first job did not resume");
    check(completed(paused), "first progress was lost");
    check(first.consume_progress().current_path == "first", "progress crossed jobs");
    check(second.consume_progress().current_path == "second", "second progress was lost");
}

void question_answers() {
    OperationJobControl first, second;
    std::future<OperationChoice> first_answer, second_answer;
    const CancelJobs cleanup{first, second};
    const auto first_sequence = first.begin_question({});
    const auto second_sequence = second.begin_question({});
    first_answer = std::async(std::launch::async, [&] { return first.wait_for_answer(first_sequence); });
    second_answer = std::async(std::launch::async, [&] { return second.wait_for_answer(second_sequence); });
    check(!first.answer(first_sequence + 1U, OperationChoice::cancel), "accepted another question's answer");
    check(first.answer(first_sequence, OperationChoice::append), "first answer rejected");
    check(second.question().has_value(), "first answer consumed second question");
    check(second.answer(second_sequence, OperationChoice::reget), "second answer rejected");
    check(completed(first_answer) == OperationChoice::append, "first answer crossed jobs");
    check(completed(second_answer) == OperationChoice::reget, "second answer crossed jobs");
    const auto next = first.begin_question({});
    check(!first.answer(first_sequence, OperationChoice::overwrite_all), "accepted a stale answer");
    check(first.answer(next, OperationChoice::skip_all), "next answer rejected");
    check(first.wait_for_answer(next) == OperationChoice::skip_all, "lost an answer delivered before waiting");
}

void cancel_waiters() {
    OperationJobControl first, second;
    std::future<bool> paused, question;
    const CancelJobs cleanup{first, second};
    static_cast<void>(first.toggle_pause());
    const auto sequence = second.begin_question({});
    paused = std::async(std::launch::async, [&] {
        try { first.wait_until_resumed(); }
        catch (const OperationCancelled&) { return true; }
        return false;
    });
    question = std::async(std::launch::async, [&] {
        try { static_cast<void>(second.wait_for_answer(sequence)); }
        catch (const OperationCancelled&) { return true; }
        return false;
    });
    first.cancel();
    second.cancel();
    check(completed(paused), "cancel did not wake the paused job");
    check(completed(question), "cancel did not wake the question");
    check(!second.answer(sequence, OperationChoice::overwrite), "cancelled job accepted an answer");
    first.finish();
    second.finish();
    check(!first.snapshot().running && !second.snapshot().running, "finished jobs remained active");
}

}  // namespace

int main() {
    try {
        pause_and_skip();
        question_answers();
        cancel_waiters();
        std::cout << "PASS operation jobs: pause/skip, answers, shutdown\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
