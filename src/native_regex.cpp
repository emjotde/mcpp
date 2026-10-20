#include "mcpp/native_regex.hpp"

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

#include <array>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>

namespace mcpp {
namespace {

using Code = std::unique_ptr<pcre2_code, decltype(&pcre2_code_free)>;
using MatchData = std::unique_ptr<pcre2_match_data, decltype(&pcre2_match_data_free)>;
using MatchContext = std::unique_ptr<pcre2_match_context, decltype(&pcre2_match_context_free)>;

PCRE2_SPTR bytes(const std::string_view value) {
    return reinterpret_cast<PCRE2_SPTR>(value.empty() ? "" : value.data());
}

std::string error_message(const int error) {
    std::array<PCRE2_UCHAR, 256> message{};
    const auto length = pcre2_get_error_message(error, message.data(), message.size());
    if (length < 0) return "PCRE2 error " + std::to_string(error);
    return {reinterpret_cast<const char*>(message.data()), static_cast<std::size_t>(length)};
}

}  // namespace

struct NativeRegex::Compiled {
    explicit Compiled(Code expression) : code(std::move(expression)) {}
    Code code;
};

NativeRegex::NativeRegex(const std::string_view pattern, const bool ignore_case) {
    int error = 0;
    PCRE2_SIZE offset = 0;
    const auto flags = PCRE2_UTF | PCRE2_UCP | PCRE2_MATCH_INVALID_UTF |
                       (ignore_case ? PCRE2_CASELESS : 0U);
    Code code(pcre2_compile(bytes(pattern), pattern.size(), flags, &error, &offset, nullptr),
              &pcre2_code_free);
    if (!code) {
        throw std::invalid_argument("invalid regular expression at byte " +
                                    std::to_string(offset) + ": " + error_message(error));
    }
    compiled_ = std::make_shared<const Compiled>(std::move(code));
}

std::optional<NativeRegexMatch> NativeRegex::find(
    const std::string_view subject, const std::size_t start
) const {
    if (!compiled_) throw std::logic_error("using a moved-from regular expression");
    if (start > subject.size()) throw std::invalid_argument("regular-expression start exceeds subject");
    MatchData data(pcre2_match_data_create(1, nullptr), &pcre2_match_data_free);
    MatchContext context(pcre2_match_context_create(nullptr), &pcre2_match_context_free);
    if (!data || !context) throw std::bad_alloc();
    if (pcre2_set_match_limit(context.get(), 1'000'000U) != 0 ||
        pcre2_set_depth_limit(context.get(), 1'000U) != 0 ||
        pcre2_set_heap_limit(context.get(), 8'192U) != 0) {
        throw std::runtime_error("cannot configure regular-expression match limits");
    }
    const auto result = pcre2_match(compiled_->code.get(), bytes(subject), subject.size(),
                                    start, 0, data.get(), context.get());
    if (result == PCRE2_ERROR_NOMATCH) return std::nullopt;
    if (result < 0) throw std::runtime_error("regular-expression matching failed: " + error_message(result));
    const auto* offsets = pcre2_get_ovector_pointer(data.get());
    if (offsets[0] > offsets[1] || offsets[1] > subject.size()) {
        throw std::runtime_error("regular expression returned an invalid match range");
    }
    return NativeRegexMatch{offsets[0], offsets[1] - offsets[0]};
}

bool NativeRegex::search(const std::string_view subject) const {
    return find(subject).has_value();
}

}  // namespace mcpp
