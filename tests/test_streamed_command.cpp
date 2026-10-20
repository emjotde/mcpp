#include "mcpp/local_tools.hpp"
#include "../src/local_tools_stream.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <unistd.h>

namespace {

void check(const bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

class RejectingBuffer final : public std::streambuf {
    std::streamsize xsputn(const char*, std::streamsize) override { return 0; }
    int_type overflow(int_type) override { return traits_type::eof(); }
};

class SignallingBuffer final : public std::streambuf {
public:
    std::future<void> started() { return written_.get_future(); }
private:
    std::streamsize xsputn(const char*, const std::streamsize length) override {
        if (!signalled_) {
            signalled_ = true;
            written_.set_value();
        }
        return length;
    }
    std::promise<void> written_;
    bool signalled_{false};
};

void run(const std::filesystem::path& root) {
    using namespace std::chrono_literals;
    const auto temporary_directory = root / "temporary";
    std::filesystem::create_directories(temporary_directory);
    std::filesystem::permissions(temporary_directory, std::filesystem::perms::owner_all);
    check(::setenv("TMPDIR", temporary_directory.c_str(), 1) == 0, "set private temporary directory");
    std::filesystem::path name;
    {
        mcpp::TemporaryFile temporary("");
        name = temporary.path();
        std::ofstream output;
        output.exceptions(std::ios::badbit | std::ios::failbit);
        output.open(name, std::ios::binary | std::ios::trunc);
        const auto result = mcpp::capture_shell_to(output,
            "printf 'BEGIN\\000END'; printf warn >&2", root, {}, 4);
        output.close();
        check(result.status == 0 && result.output.empty() && result.error == "warn",
              "streamed stdout is independent of the bounded diagnostic buffer");
        std::ifstream input(name, std::ios::binary);
        const std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        check(bytes == std::string("BEGIN\0END", 9), "binary stdout is stored exactly");
    }
    check(!std::filesystem::exists(name), "completed output file releases its owned temporary");

    std::ostringstream partial;
    const auto failed = mcpp::capture_shell_to(partial,
        "printf PARTIAL; printf diagnostic >&2; exit 7", root);
    check(failed.status == 7 && failed.error == "diagnostic" && partial.str() == "PARTIAL",
          "nonzero command status and diagnostics remain explicit");

    RejectingBuffer rejected;
    std::ostream failing(&rejected);
    failing.exceptions(std::ios::badbit | std::ios::failbit);
    const auto before_failure = std::chrono::steady_clock::now();
    bool write_failed = false;
    try {
        static_cast<void>(mcpp::capture_shell_to(failing, "yes x", root));
    } catch (const std::ios_base::failure&) {
        write_failed = true;
    }
    check(write_failed && std::chrono::steady_clock::now() - before_failure < 3s,
          "output failure stops and reaps the running producer");

    mcpp::Cancellation cancellation;
    SignallingBuffer discarded;
    auto started = discarded.started();
    std::ostream output(&discarded);
    auto command = std::async(std::launch::async, [&] {
        try {
            static_cast<void>(mcpp::capture_shell_to(output, "yes x", root, cancellation));
        } catch (const mcpp::OperationCancelled&) {
            return true;
        }
        return false;
    });
    const bool wrote = started.wait_for(3s) == std::future_status::ready;
    cancellation.request();
    check(wrote, "producer reached its streaming callback");
    check(command.wait_for(3s) == std::future_status::ready && command.get(),
          "cancellation stops and reaps an active streamed command");
}

}  // namespace

int main(const int argc, char** argv) {
    try {
        if (argc != 2 || ::geteuid() != 1001U || !std::filesystem::is_regular_file("/.dockerenv") ||
            !std::filesystem::is_regular_file("/review/.operation-regressions-disposable")) {
            throw std::runtime_error("streamed command checks require the owned UID1001 container");
        }
        const auto root = std::filesystem::absolute(argv[1]).lexically_normal();
        check(root.string().starts_with("/review/cases/streamed-view-filters"), "invalid owned case path");
        std::filesystem::create_directories(root);
        run(root);
        std::cout << "streamed command bytes, failures, lifetime and cancellation: passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
