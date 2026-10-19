#include "mcpp/spelling.hpp"
#include "mcpp/local_tools.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace mcpp {

void validate_spelling_language(const std::string_view language) {
    if (!std::ranges::all_of(language, [](const unsigned char value) {
            return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
                   (value >= '0' && value <= '9') || value == '_' || value == '-' ||
                   value == '.' || value == '@';
        })) {
        throw std::invalid_argument("invalid spelling language");
    }
}

namespace {

std::string run_aspell(
    std::vector<std::string> arguments, const std::filesystem::path& directory,
    const Cancellation& cancellation, const std::optional<std::filesystem::path>& input = std::nullopt
) {
    const auto result = capture_command(arguments, directory, cancellation, 64U * 1024U * 1024U, input);
    if (result.status != 0 || !result.error.empty()) {
        throw std::runtime_error("aspell exited " + std::to_string(result.status) +
            (result.error.empty() ? "" : ": " + result.error));
    }
    return result.output;
}

std::vector<std::string> pipe_arguments(const std::string& language) {
    validate_spelling_language(language);
    std::vector<std::string> arguments{
        "aspell", "--encoding=utf-8", "--byte-offsets", "--dont-normalize", "--mode=none", "-a"
    };
    if (!language.empty()) arguments.push_back("--lang=" + language);
    return arguments;
}

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r");
    return value.substr(first, last - first + 1U);
}

}  // namespace

std::vector<std::string> spelling_languages(
    const std::filesystem::path& directory, const Cancellation& cancellation
) {
    const auto output = run_aspell({"aspell", "dump", "dicts"}, directory, cancellation);
    std::istringstream input(output);
    std::vector<std::string> languages;
    for (std::string line; std::getline(input, line);) {
        line = trim(std::move(line));
        if (line.empty()) continue;
        validate_spelling_language(line);
        languages.push_back(std::move(line));
    }
    std::ranges::sort(languages);
    languages.erase(std::unique(languages.begin(), languages.end()), languages.end());
    if (languages.empty()) throw std::runtime_error("no Aspell dictionaries are installed");
    return languages;
}

std::vector<SpellingIssue> check_spelling(
    const std::string_view text, const std::string& language,
    const std::filesystem::path& directory, const Cancellation& cancellation
) {
    static_cast<void>(convert_encoding(text, "UTF-8", "UTF-8"));
    std::string protocol;
    std::vector<std::pair<std::size_t, std::size_t>> lines;
    for (std::size_t begin = 0;;) {
        cancellation.throw_if_requested();
        const auto newline = text.find('\n', begin);
        const auto end = newline == std::string_view::npos ? text.size() : newline;
        lines.emplace_back(begin, end - begin);
        protocol += '^';
        for (auto offset = begin; offset < end; ++offset) {
            const auto value = static_cast<unsigned char>(text[offset]);
            protocol += value < 32U && value != '\t' ? ' ' : text[offset];
        }
        protocol += '\n';
        if (newline == std::string_view::npos) break;
        begin = newline + 1U;
    }
    const TemporaryFile source(protocol);
    const auto output = run_aspell(pipe_arguments(language), directory, cancellation, source.path());
    std::istringstream input(output);
    std::string line;
    if (!std::getline(input, line) || !line.starts_with("@(#)")) {
        throw std::runtime_error("invalid Aspell response header");
    }
    std::size_t row = 0;
    std::vector<SpellingIssue> issues;
    while (std::getline(input, line)) {
        cancellation.throw_if_requested();
        if (line.empty()) {
            ++row;
            continue;
        }
        if (row >= lines.size()) throw std::runtime_error("Aspell returned too many lines");
        if (line.front() == '*' || line.front() == '+' || line.front() == '-') continue;
        std::istringstream fields(line);
        char kind = '\0';
        std::size_t count = 0, offset = 0;
        SpellingIssue issue;
        if (!(fields >> kind >> issue.word) || (kind != '&' && kind != '?' && kind != '#') ||
            (kind != '#' && !(fields >> count)) || !(fields >> offset) || offset == 0U) {
            throw std::runtime_error("invalid Aspell word response");
        }
        const auto [start, length] = lines[row];
        --offset;
        if (offset > length || issue.word.size() > length - offset ||
            text.substr(start + offset, issue.word.size()) != issue.word) {
            throw std::runtime_error("Aspell word offsets do not match the editor text");
        }
        issue.offset = start + offset;
        if (!issues.empty() && issue.offset < issues.back().offset + issues.back().word.size()) {
            throw std::runtime_error("Aspell returned overlapping or unordered words");
        }
        if (kind != '#') {
            char colon = '\0';
            if (!(fields >> colon) || colon != ':') throw std::runtime_error("invalid Aspell suggestion response");
            for (std::string suggestion; std::getline(fields, suggestion, ',');) {
                suggestion = trim(std::move(suggestion));
                if (!suggestion.empty()) issue.suggestions.push_back(std::move(suggestion));
            }
            if (issue.suggestions.size() != count) throw std::runtime_error("invalid Aspell suggestion count");
        }
        issues.push_back(std::move(issue));
    }
    if (row != lines.size()) throw std::runtime_error("Aspell did not finish checking the text");
    return issues;
}

void add_spelling_word(
    const std::string_view word, const std::string& language,
    const std::filesystem::path& directory, const Cancellation& cancellation
) {
    if (word.empty() || std::ranges::any_of(word, [](const unsigned char value) {
            return value <= 32U || value == 127U;
        })) throw std::invalid_argument("invalid personal dictionary word");
    const TemporaryFile input("*" + std::string(word) + "\n#\n");
    const auto output = run_aspell(pipe_arguments(language), directory, cancellation, input.path());
    if (!output.starts_with("@(#)")) throw std::runtime_error("invalid Aspell dictionary response");
    if (!check_spelling(word, language, directory, cancellation).empty()) {
        throw std::runtime_error("Aspell did not retain the personal dictionary word");
    }
}

}  // namespace mcpp
