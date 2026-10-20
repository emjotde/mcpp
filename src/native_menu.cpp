#include "mcpp/native_menu.hpp"
#include "mcpp/native_regex.hpp"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <fnmatch.h>
#include <pwd.h>
#include <stdexcept>
#include <system_error>
#include <sys/stat.h>
#include <utility>

namespace mcpp {
namespace {

bool blank(const char character) {
    return character == ' ' || character == '\t' ||
           character == '\r' || character == '\n';
}

std::string_view trim(std::string_view value) {
    while (!value.empty() && blank(value.front())) value.remove_prefix(1U);
    while (!value.empty() && blank(value.back())) value.remove_suffix(1U);
    return value;
}

[[noreturn]] void menu_error(
    const std::string_view source,
    const std::size_t line,
    const std::string_view message
) {
    throw std::invalid_argument(
        (source.empty() ? std::string("user menu") : std::string(source)) +
        ":" + std::to_string(line) + ": " + std::string(message)
    );
}

class Pattern {
public:
    explicit Pattern(const std::string& value) : expression_(value) {}
    Pattern(const Pattern&) = delete;
    Pattern& operator=(const Pattern&) = delete;
    [[nodiscard]] bool matches(const std::string& value) const {
        return expression_.search(value);
    }
private:
    NativeRegex expression_;
};

std::string condition_operand(std::string_view value) {
    value = trim(value);
    if (value.size() >= 2U && (value.front() == '\'' || value.front() == '"') &&
        value.back() == value.front()) {
        value.remove_prefix(1U);
        value.remove_suffix(1U);
    }
    std::string result;
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] == '\\' && index + 1U < value.size() &&
            std::string_view(" \t'\"&|#\\").find(value[index + 1U]) != std::string_view::npos) {
            ++index;
        }
        result += value[index];
    }
    return result;
}

NativeMenuCondition parse_condition(
    std::string_view value,
    const std::string_view source,
    const std::size_t line,
    const bool shell_patterns
) {
    NativeMenuCondition result;
    result.shell_patterns = shell_patterns;
    result.line = line;
    result.addition = value.front() == '+';
    result.default_selection = value.front() == '=';
    const auto prefix = value.front();
    value.remove_prefix(1U);
    if (!value.empty() && (value.front() == '+' || value.front() == '=') && value.front() != prefix) {
        result.addition = true;
        result.default_selection = true;
        value.remove_prefix(1U);
    }
    if (!value.empty() && value.front() == '?') {
        result.debug = true;
        value.remove_prefix(1U);
    }
    char operation = '&';
    while (!(value = trim(value)).empty()) {
        NativeMenuTest test;
        test.operation = operation;
        while (!value.empty() && value.front() == '!') {
            test.negate = !test.negate;
            value.remove_prefix(1U);
            value = trim(value);
        }
        if (value.empty() || std::string_view("yfFdDtTx").find(value.front()) == std::string_view::npos) {
            menu_error(source, line, "expected a local menu condition (y, f, F, d, D, t, T, or x)");
        }
        test.predicate = value.front();
        value.remove_prefix(1U);
        value = trim(value);
        std::size_t end = 0;
        char quote = '\0';
        bool escaped = false;
        for (; end < value.size(); ++end) {
            const auto character = value[end];
            if (escaped) {
                escaped = false;
            } else if (character == '\\') {
                escaped = true;
            } else if ((character == '\'' || character == '"') &&
                       (quote == '\0' || quote == character)) {
                quote = quote == '\0' ? character : '\0';
            } else if (quote == '\0' && (character == '&' || character == '|' || character == '#')) {
                break;
            }
        }
        if (quote != '\0' || escaped) menu_error(source, line, "unfinished quoting in menu condition");
        test.operand = condition_operand(value.substr(0, end));
        if (test.operand.empty()) menu_error(source, line, "menu condition needs an operand");
        if (test.predicate == 't' || test.predicate == 'T') {
            for (const auto type : test.operand) {
                if (std::string_view("nrdlcbfsxt").find(type) == std::string_view::npos) {
                    menu_error(source, line, "unknown file type in menu condition");
                }
            }
        } else if (test.predicate != 'x' && !shell_patterns) {
            try {
                const Pattern validated(test.operand);
            } catch (const std::invalid_argument& error) {
                menu_error(source, line, error.what());
            }
        }
        result.tests.push_back(std::move(test));
        if (end == value.size() || value[end] == '#') break;
        operation = value[end];
        value.remove_prefix(end + 1U);
        if (trim(value).empty()) menu_error(source, line, "menu condition ends with an operator");
    }
    if (result.tests.empty()) menu_error(source, line, "empty menu condition");
    return result;
}

std::string dedent_command(const std::vector<std::string>& lines) {
    std::string prefix;
    bool first = true;
    for (const auto& line : lines) {
        if (trim(line).empty()) continue;
        const auto end = line.find_first_not_of(" \t");
        const auto indentation = line.substr(0, end);
        if (first) {
            prefix = indentation;
            first = false;
        } else {
            std::size_t common = 0;
            while (common < prefix.size() && common < indentation.size() &&
                   prefix[common] == indentation[common]) ++common;
            prefix.resize(common);
        }
    }
    std::string result;
    for (const auto& line : lines) {
        if (!trim(line).empty()) result += line.substr(prefix.size());
        result += '\n';
    }
    return result;
}

bool matches_pattern(
    const std::string& value,
    const std::string& pattern,
    const bool shell_patterns
) {
    return shell_patterns ? fnmatch(pattern.c_str(), value.c_str(), 0) == 0
                          : Pattern(pattern).matches(value);
}

bool matches_type(const NativeMenuPane& pane, const char type) {
    if (type == 't') return !pane.tagged.empty();
    if (!pane.file) return false;
    const auto& file = *pane.file;
    switch (type) {
        case 'n': return file.kind != EntryKind::directory;
        case 'r': return file.kind == EntryKind::file || S_ISREG(file.mode);
        case 'd': return file.kind == EntryKind::directory || S_ISDIR(file.mode);
        case 'l': return file.kind == EntryKind::symlink || S_ISLNK(file.mode);
        case 'c': return S_ISCHR(file.mode);
        case 'b': return S_ISBLK(file.mode);
        case 'f': return S_ISFIFO(file.mode);
        case 's': return S_ISSOCK(file.mode);
        case 'x':
            return (file.kind == EntryKind::file || S_ISREG(file.mode)) &&
                   ((file.mode & 0111U) != 0U ||
                    (file.permissions != std::filesystem::perms::unknown &&
                     (file.permissions & (std::filesystem::perms::owner_exec |
                                          std::filesystem::perms::group_exec |
                                          std::filesystem::perms::others_exec)) != std::filesystem::perms::none));
        default: return false;
    }
}

bool evaluate_condition(const NativeMenuCondition& condition, const NativeMenuContext& context) {
    bool result = false;
    bool first = true;
    for (const auto& test : condition.tests) {
        const auto& pane = test.predicate == 'F' || test.predicate == 'D' || test.predicate == 'T'
                               ? context.other : context.active;
        bool value = false;
        switch (test.predicate) {
            case 'f':
            case 'F':
                value = pane.file && matches_pattern(pane.file->name, test.operand, condition.shell_patterns);
                break;
            case 'd':
            case 'D':
                value = matches_pattern(pane.directory, test.operand, condition.shell_patterns);
                break;
            case 't':
            case 'T':
                value = std::ranges::any_of(test.operand, [&](const char type) { return matches_type(pane, type); });
                break;
            case 'y':
                value = context.editor && matches_pattern(context.syntax, test.operand, condition.shell_patterns);
                break;
            case 'x':
                if (!context.active.local || !context.executable) {
                    throw std::invalid_argument("executable menu conditions are unavailable for non-local directories");
                }
                value = context.executable(test.operand);
                break;
            default: break;
        }
        if (test.negate) value = !value;
        if (first) result = value;
        else result = test.operation == '|' ? result || value : result && value;
        first = false;
    }
    return result;
}

enum class MacroKind { scalar, prompt, environment, view, change_directory, literal };

struct Macro {
    MacroKind kind{MacroKind::scalar};
    char code{'\0'};
    std::string argument;
    std::size_t end{0};
    bool raw{false};
};

Macro read_macro(const std::string_view command, const std::size_t begin) {
    auto index = begin + 1U;
    if (index == command.size()) throw std::invalid_argument("unfinished % in user menu; use %% for a literal percent");
    Macro result;
    if (command[index] == '0' || command[index] == '1') {
        result.raw = command[index] == '0';
        if (++index == command.size()) throw std::invalid_argument("unfinished user-menu quoting prefix");
    }
    if (command[index] == '%') {
        result.kind = MacroKind::literal;
    } else if (command[index] == '{' || command.substr(index).starts_with("var{")) {
        const bool environment = command[index] != '{';
        if (environment) index += 3U;
        const auto end = command.find('}', index + 1U);
        if (end == std::string_view::npos) throw std::invalid_argument("unfinished braced user-menu macro");
        result.kind = environment ? MacroKind::environment : MacroKind::prompt;
        result.argument = command.substr(index + 1U, end - index - 1U);
        index = end;
    } else if (command.substr(index).starts_with("view") &&
               (index + 4U == command.size() || command[index + 4U] == '{' || blank(command[index + 4U]))) {
        result.kind = MacroKind::view;
        index += 3U;
        if (index + 1U < command.size() && command[index + 1U] == '{') {
            const auto end = command.find('}', index + 2U);
            if (end == std::string_view::npos) throw std::invalid_argument("unfinished %view arguments");
            result.argument = command.substr(index + 2U, end - index - 2U);
            index = end;
        }
    } else if (command.substr(index).starts_with("cd") &&
               (index + 2U == command.size() || blank(command[index + 2U]))) {
        result.kind = MacroKind::change_directory;
        ++index;
    } else {
        result.code = command[index];
        if (std::string_view("fpPxbdFDTtuUsSmkeciny").find(result.code) == std::string_view::npos) {
            throw std::invalid_argument("unsupported user-menu macro: %" + std::string(1, result.code));
        }
    }
    result.end = index + 1U;
    return result;
}

std::string quoted_value(const std::string_view value, const char quote) {
    if (value.find('\0') != std::string_view::npos) throw std::invalid_argument("NUL is not valid in a menu argument");
    std::string result;
    if (quote == '\0') result += '\'';
    for (const auto character : value) {
        if (quote == '"') {
            if (character == '"' || character == '\\' || character == '$' || character == '`') result += '\\';
            result += character;
        } else if (character == '\'') result += "'\\''";
        else result += character;
    }
    if (quote == '\0') result += '\'';
    return result;
}

std::string file_name(const NativeMenuPane& pane) {
    return pane.file ? pane.file->name : std::string{};
}

std::string without_extension(const std::string& name) {
    const auto slash = name.find_last_of('/');
    const auto dot = name.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash) ||
        dot == (slash == std::string::npos ? 0U : slash + 1U)) return name;
    return name.substr(0, dot);
}

std::string extension(const std::string& name) {
    const auto base = without_extension(name);
    return base.size() == name.size() ? std::string{} : name.substr(base.size() + 1U);
}

std::string environment_value(const std::string& argument) {
    const auto colon = argument.find(':');
    if (colon == std::string::npos) throw std::invalid_argument("%var needs ENV:default");
    const auto name = argument.substr(0, colon);
    if (name.empty() || !(std::isalpha(static_cast<unsigned char>(name.front())) != 0 || name.front() == '_') ||
        !std::ranges::all_of(name, [](const unsigned char character) {
            return std::isalnum(character) != 0 || character == '_';
        })) {
        throw std::invalid_argument("invalid environment variable name in user menu");
    }
    const auto* value = std::getenv(name.c_str());
    return value == nullptr ? argument.substr(colon + 1U) : std::string(value);
}

void viewer_options(const std::string& argument, NativeMenuExpansion& result) {
    auto options = std::string_view(argument);
    while (!(options = trim(options)).empty()) {
        const auto end = options.find_first_of(", \t");
        const auto option = options.substr(0, end);
        if (option == "ascii") result.view_hex = false;
        else if (option == "hex") result.view_hex = true;
        else if (option == "nroff") result.view_nroff = true;
        else if (option == "unform" || option == "unformatted") result.view_nroff = false;
        else throw std::invalid_argument("unsupported %view option: " + std::string(option));
        if (end == std::string_view::npos) break;
        options.remove_prefix(end + 1U);
    }
}

enum class ShellQuote { none, single, double_quote, ansi, heredoc_literal, heredoc_expanding };

struct LocatedMacro {
    Macro macro;
    std::size_t begin;
    ShellQuote quote;
    bool arithmetic{false};
    std::size_t backticks{0};
};

struct HereDocument {
    std::string delimiter;
    bool literal{false};
    bool strip_tabs{false};
    std::size_t begin{0};
    std::size_t end{0};
};

struct CommandMacros {
    std::vector<LocatedMacro> macros;
    std::vector<HereDocument> heredocs;
};

class SubstitutionCases {
public:
    explicit SubstitutionCases(const bool enabled) : enabled_(enabled) {}

    [[nodiscard]] bool enabled() const { return enabled_; }
    [[nodiscard]] bool word_started() const { return word_started_; }

    [[nodiscard]] bool extended_pattern(const std::string_view remaining) const {
        return enabled_ && !cases_.empty() && cases_.back().phase == Phase::pattern &&
            remaining.size() >= 2U && remaining[1] == '(' &&
            std::string_view("?*+@!").find(remaining.front()) != std::string_view::npos;
    }

    void literal(const char character) {
        if (!enabled_) return;
        word_started_ = true;
        word_ += character;
    }

    void quoted() {
        if (!enabled_) return;
        word_started_ = true;
        literal_word_ = false;
    }

    void finish_word(const bool redirection = false) {
        if (!enabled_ || !word_started_) return;
        const auto word = std::move(word_);
        word_.clear();
        const bool literal_word = literal_word_;
        word_started_ = false;
        literal_word_ = true;
        if (redirection_word_) {
            redirection_word_ = false;
            return;
        }
        if (conditional_) {
            if (literal_word && word == "]]") conditional_ = false;
            command_start_ = false;
            return;
        }
        if (!cases_.empty()) {
            auto& current = cases_.back();
            if (current.phase == Phase::subject) {
                current.phase = Phase::in;
                return;
            }
            if (current.phase == Phase::in) {
                if (!literal_word || word != "in") {
                    throw std::invalid_argument("a menu substitution case needs a literal in keyword");
                }
                current.phase = Phase::pattern;
                return;
            }
            if (current.phase == Phase::pattern) {
                if (literal_word && word == "esac" && !current.pattern &&
                    !current.opening && !current.alternative) {
                    cases_.pop_back();
                    command_start_ = true;
                } else {
                    if (current.pattern) {
                        throw std::invalid_argument("a menu substitution case pattern needs | or )");
                    }
                    current.pattern = true;
                }
                return;
            }
        }
        if (redirection && literal_word && std::ranges::all_of(word, [](const unsigned char character) {
                return character >= '0' && character <= '9';
            })) return;
        if (function_name_) {
            function_name_ = false;
            command_start_ = true;
            return;
        }
        if (time_option_) {
            time_option_ = false;
            if (literal_word && word == "-p") {
                command_start_ = true;
                return;
            }
        }
        if (coproc_name_) {
            coproc_name_ = false;
            if (!literal_word || (word != "case" && word != "if" && word != "while" &&
                                 word != "until" && word != "for" && word != "select" &&
                                 word != "function" && word != "coproc" && word != "time" &&
                                 word != "!" && word != "{" && word != "[[")) {
                command_start_ = true;
                return;
            }
        }
        if (!command_start_) return;
        if (literal_word && word == "case") {
            cases_.push_back({Phase::subject, groups_});
            command_start_ = false;
        } else if (literal_word && word == "esac" && !cases_.empty() &&
                   cases_.back().groups == groups_) {
            cases_.pop_back();
            command_start_ = true;
        } else {
            function_name_ = literal_word && word == "function";
            coproc_name_ = literal_word && word == "coproc";
            time_option_ = literal_word && word == "time";
            conditional_ = literal_word && word == "[[";
            command_start_ = literal_word &&
                (word == "if" || word == "then" || word == "elif" || word == "else" ||
                 word == "while" || word == "until" || word == "do" ||
                 word == "!" || word == "time" || word == "{" ||
                 word == "}" || word == "fi" || word == "done" ||
                 word == "function" || word == "coproc");
        }
    }

    std::size_t operation(const std::string_view remaining) {
        if (!enabled_) return 0U;
        const auto character = remaining.front();
        if (character == '\n') {
            if (!conditional_ && (cases_.empty() || cases_.back().phase == Phase::body)) {
                command_start_ = true;
            }
            return 0U;
        }
        if (std::string_view(";&|()<>").find(character) == std::string_view::npos) return 0U;
        function_name_ = false;
        coproc_name_ = false;
        time_option_ = false;
        if (conditional_ && character != '(' && character != ')') return 1U;
        if (!cases_.empty()) {
            auto& current = cases_.back();
            if (current.phase == Phase::subject || current.phase == Phase::in) unfinished();
            if (current.phase == Phase::pattern) {
                if (character == '(' && !current.pattern && !current.opening && !current.alternative) {
                    current.opening = true;
                } else if (character == '|' && current.pattern) {
                    current.pattern = false;
                    current.alternative = true;
                } else if (character == ')' && current.pattern) {
                    current.phase = Phase::body;
                    command_start_ = true;
                } else {
                    throw std::invalid_argument("invalid or unsupported menu substitution case pattern");
                }
                return 1U;
            }
            if (character == ';' && (remaining.starts_with(";;") || remaining.starts_with(";&"))) {
                if (current.groups != groups_) unfinished();
                current.phase = Phase::pattern;
                current.pattern = false;
                current.opening = false;
                current.alternative = false;
                return remaining.starts_with(";;&") ? 3U : 2U;
            }
        }
        if (character == '(') {
            command_start_ = !remaining.starts_with("((");
            redirection_word_ = false;
            if (command_start_) ++groups_;
        } else if (character == ')') {
            if (!cases_.empty() && cases_.back().groups == groups_) unfinished();
            if (groups_ != 0U) --groups_;
            command_start_ = true;
            redirection_word_ = false;
        } else if (character == '<' || character == '>') {
            redirection_word_ = !remaining.starts_with("<<") || remaining.starts_with("<<<");
            if (remaining.starts_with("<<")) return 0U;
            return remaining.starts_with(">>") || remaining.starts_with("<&") ||
                remaining.starts_with(">&") || remaining.starts_with("<>") ||
                remaining.starts_with(">|") ? 2U : 1U;
        } else {
            command_start_ = true;
            redirection_word_ = false;
            return 1U;
        }
        return 0U;
    }

    void finish() {
        finish_word();
        if (!cases_.empty()) unfinished();
    }

private:
    enum class Phase { subject, in, pattern, body };
    struct Case {
        Phase phase;
        std::size_t groups;
        bool pattern{false};
        bool opening{false};
        bool alternative{false};
    };

    bool enabled_;
    bool command_start_{true};
    bool redirection_word_{false};
    bool function_name_{false};
    bool coproc_name_{false};
    bool time_option_{false};
    bool conditional_{false};
    bool word_started_{false};
    bool literal_word_{true};
    std::size_t groups_{0};
    std::string word_;
    std::vector<Case> cases_;

    [[noreturn]] static void unfinished() {
        throw std::invalid_argument("unfinished case statement in menu command substitution");
    }
};

class MacroScanner {
public:
    explicit MacroScanner(const std::string_view command) : command_(command) {}

    CommandMacros scan() {
        shell('\0', false, 0U);
        return std::move(result_);
    }

private:
    std::string_view command_;
    std::size_t index_{0};
    CommandMacros result_;

    void macro(const ShellQuote quote, const bool arithmetic, const std::size_t backticks,
               const bool escaped = false) {
        const auto value = read_macro(command_, index_);
        if (escaped && value.kind != MacroKind::literal && !value.raw) {
            throw std::invalid_argument("a quoted menu macro cannot follow a backslash; remove the backslash or use %0");
        }
        result_.macros.push_back({value, index_, quote, arithmetic, backticks});
        index_ = value.end;
    }

    HereDocument delimiter() {
        HereDocument result;
        index_ += 2U;
        if (index_ < command_.size() && command_[index_] == '-') {
            result.strip_tabs = true;
            ++index_;
        }
        while (index_ < command_.size() && (command_[index_] == ' ' || command_[index_] == '\t')) ++index_;
        char quote = '\0';
        bool started = false;
        while (index_ < command_.size()) {
            const auto character = command_[index_];
            if (quote == '\0' && (blank(character) || std::string_view(";&|<>()").find(character) != std::string_view::npos)) break;
            started = true;
            if (character == '%') {
                throw std::invalid_argument("menu macros in here-document delimiters are unavailable; use a literal delimiter");
            }
            if (character == '$' && quote == '\0' && index_ + 1U < command_.size() &&
                (command_[index_ + 1U] == '\'' || command_[index_ + 1U] == '"')) {
                throw std::invalid_argument("ANSI or locale quoted here-document delimiters are unavailable; use ordinary quotes");
            }
            if ((character == '\'' || character == '"') && (quote == '\0' || quote == character)) {
                quote = quote == '\0' ? character : '\0';
                result.literal = true;
                ++index_;
            } else if (character == '\\' && quote != '\'') {
                result.literal = true;
                if (++index_ == command_.size()) throw std::invalid_argument("unfinished here-document delimiter");
                if (quote == '"' && std::string_view("$`\"\\\n").find(command_[index_]) == std::string_view::npos) {
                    result.delimiter += '\\';
                }
                if (command_[index_] != '\n') result.delimiter += command_[index_];
                ++index_;
            } else {
                result.delimiter += character;
                ++index_;
            }
        }
        if (!started || quote != '\0') throw std::invalid_argument("unfinished here-document delimiter");
        return result;
    }

    void body(HereDocument document, const std::size_t backticks) {
        document.begin = index_;
        while (index_ < command_.size()) {
            const auto line_begin = index_;
            auto line_end = index_;
            std::string logical_line;
            bool continued;
            do {
                const auto newline = command_.find('\n', line_end);
                const auto end = newline == std::string_view::npos ? command_.size() : newline;
                auto line = command_.substr(line_end, end - line_end);
                if (document.strip_tabs) {
                    while (!line.empty() && line.front() == '\t') line.remove_prefix(1U);
                }
                std::size_t backslashes = 0;
                while (backslashes < line.size() && line[line.size() - backslashes - 1U] == '\\') ++backslashes;
                continued = !document.literal && newline != std::string_view::npos && backslashes % 2U != 0U;
                if (continued) line.remove_suffix(1U);
                logical_line += line;
                line_end = newline == std::string_view::npos ? command_.size() : newline + 1U;
            } while (continued && line_end < command_.size());
            if (logical_line == document.delimiter) {
                document.end = line_begin;
                result_.heredocs.push_back(std::move(document));
                index_ = line_end;
                return;
            }
            const auto quote = document.literal ? ShellQuote::heredoc_literal : ShellQuote::heredoc_expanding;
            bool escaped = false;
            while (index_ < line_end) {
                if (command_[index_] == '%') {
                    macro(quote, false, backticks, escaped && !document.literal);
                    if (index_ > line_end) throw std::invalid_argument("a here-document menu macro must end on the same logical line");
                    escaped = false;
                } else {
                    escaped = !escaped && command_[index_] == '\\';
                    ++index_;
                }
            }
        }
        throw std::invalid_argument("unfinished here-document in user-menu command");
    }

    void shell(const char closing, const bool arithmetic, const std::size_t backticks,
               const std::size_t depth = 0U, const bool pattern = false) {
        if (depth > 128U) throw std::invalid_argument("user-menu shell substitutions are nested too deeply");
        ShellQuote quote = ShellQuote::none;
        std::size_t parentheses = closing == ')' ? (arithmetic ? 2U : 1U) : closing == ']' ? 1U : 0U;
        std::vector<HereDocument> pending;
        SubstitutionCases cases(closing == ')' && !arithmetic && !pattern);
        while (index_ < command_.size()) {
            const auto character = command_[index_];
            if (character == '\\' && quote != ShellQuote::single) {
                ++index_;
                if (index_ == command_.size()) throw std::invalid_argument("unfinished shell escape in user-menu command");
                if (command_[index_] != '\n') cases.quoted();
                if (backticks != 0U && command_[index_] == '`') {
                    throw std::invalid_argument("nested escaped menu backticks are unavailable; use $(...) instead");
                }
                if (command_[index_] == '%') macro(quote, arithmetic, backticks, true);
                else ++index_;
                continue;
            }
            if (character == '`' && (backticks != 0U ||
                                    (quote != ShellQuote::single && quote != ShellQuote::ansi))) {
                cases.quoted();
                ++index_;
                if (closing == '`') {
                    if (quote != ShellQuote::none || !pending.empty()) {
                        throw std::invalid_argument("unfinished quoting or here-document inside menu backticks");
                    }
                    return;
                }
                if (backticks != 0U) throw std::invalid_argument("nested menu backticks are unavailable; use $(...) instead");
                shell('`', false, backticks + 1U, depth + 1U);
                continue;
            }
            if (character == '$' && quote != ShellQuote::single && quote != ShellQuote::ansi &&
                index_ + 1U < command_.size()) {
                if (command_[index_ + 1U] == '(') {
                    cases.quoted();
                    const bool nested_arithmetic = index_ + 2U < command_.size() && command_[index_ + 2U] == '(';
                    index_ += nested_arithmetic ? 3U : 2U;
                    shell(')', nested_arithmetic, backticks, depth + 1U);
                    continue;
                }
                if (command_[index_ + 1U] == '[') {
                    cases.quoted();
                    index_ += 2U;
                    shell(']', true, backticks, depth + 1U);
                    continue;
                }
                if (command_[index_ + 1U] == '\'' && quote == ShellQuote::none) {
                    cases.quoted();
                    quote = ShellQuote::ansi;
                    index_ += 2U;
                    continue;
                }
            }
            if ((character == '\'' && (quote == ShellQuote::none || quote == ShellQuote::single || quote == ShellQuote::ansi)) ||
                (character == '"' && (quote == ShellQuote::none || quote == ShellQuote::double_quote))) {
                cases.quoted();
                quote = quote == ShellQuote::none
                    ? (character == '\'' ? ShellQuote::single : ShellQuote::double_quote) : ShellQuote::none;
                ++index_;
                continue;
            }
            if (quote == ShellQuote::none) {
                if (cases.extended_pattern(command_.substr(index_))) {
                    cases.quoted();
                    index_ += 2U;
                    // Bash balances the pattern word separately from the case-arm close.
                    shell(')', false, backticks, depth + 1U, true);
                    continue;
                }
                if (blank(character) || std::string_view(";&|()<>").find(character) != std::string_view::npos) {
                    cases.finish_word(character == '<' || character == '>');
                }
                if (character == '#' && !arithmetic && !pattern &&
                    (cases.enabled() ? !cases.word_started() :
                     (index_ == 0U || blank(command_[index_ - 1U]) ||
                      std::string_view(";|&()").find(command_[index_ - 1U]) != std::string_view::npos))) {
                    const auto newline = command_.find('\n', index_);
                    index_ = newline == std::string_view::npos ? command_.size() : newline;
                    continue;
                }
                if (const auto length = cases.operation(command_.substr(index_)); length != 0U) {
                    index_ += length;
                    continue;
                }
                if (character == '\n' && !pending.empty()) {
                    ++index_;
                    for (auto& document : pending) body(std::move(document), backticks);
                    pending.clear();
                    continue;
                }
                if (!arithmetic && !pattern && character == '<' && command_.substr(index_).starts_with("<<<")) {
                    index_ += 3U;
                    continue;
                }
                if (!arithmetic && !pattern && character == '<' && command_.substr(index_).starts_with("<<")) {
                    pending.push_back(delimiter());
                    continue;
                }
                if (!arithmetic && !pattern && command_.substr(index_).starts_with("((")) {
                    index_ += 2U;
                    shell(')', true, backticks, depth + 1U);
                    continue;
                }
                if (closing == ')') {
                    if (character == '(') ++parentheses;
                    else if (character == ')' && --parentheses == 0U) {
                        if (!pending.empty()) throw std::invalid_argument("unfinished here-document in menu substitution");
                        ++index_;
                        return;
                    }
                } else if (closing == ']') {
                    if (character == '[') ++parentheses;
                    else if (character == ']' && --parentheses == 0U) {
                        ++index_;
                        return;
                    }
                }
            }
            if (character == '%') {
                cases.quoted();
                macro(quote, arithmetic, backticks);
            } else {
                if (quote == ShellQuote::none && !blank(character) &&
                    std::string_view(";&|()<>").find(character) == std::string_view::npos) {
                    cases.literal(character);
                }
                ++index_;
            }
        }
        cases.finish();
        if (closing != '\0' || quote != ShellQuote::none || !pending.empty()) {
            throw std::invalid_argument("unfinished shell quoting or here-document in user-menu command");
        }
    }
};

CommandMacros command_macros(const std::string_view command) {
    return MacroScanner(command).scan();
}

std::string arithmetic_value(const std::string_view value) {
    auto number = value;
    if (!number.empty() && (number.front() == '-' || number.front() == '+')) number.remove_prefix(1U);
    const bool hexadecimal = number.starts_with("0x") || number.starts_with("0X");
    if (hexadecimal) number.remove_prefix(2U);
    if (number.empty() || !std::ranges::all_of(number, [hexadecimal](const unsigned char character) {
            return (character >= '0' && character <= '9') ||
                (hexadecimal && ((character >= 'a' && character <= 'f') ||
                                 (character >= 'A' && character <= 'F')));
        })) {
        throw std::invalid_argument("a menu macro in arithmetic must be a decimal or hexadecimal integer; use %0 for a trusted expression");
    }
    const bool negative = value.front() == '-';
    if (!hexadecimal) {
        const auto first = number.find_first_not_of('0');
        number.remove_prefix(first == std::string_view::npos ? number.size() - 1U : first);
    }
    return "(" + std::string(negative ? "-" : "") +
        (hexadecimal ? "0x" : "") + std::string(number) + ")";
}

std::string macro_value(const std::string_view value, const LocatedMacro& location) {
    if (value.find('\0') != std::string_view::npos) throw std::invalid_argument("NUL is not valid in a menu argument");
    if (location.macro.raw) return std::string(value);
    std::string result;
    if (location.arithmetic) {
        if (location.quote != ShellQuote::none) {
            throw std::invalid_argument("quoted menu macros in arithmetic are unavailable; use an unquoted integer macro");
        }
        result = arithmetic_value(value);
    } else if (location.quote == ShellQuote::none || location.quote == ShellQuote::single ||
               location.quote == ShellQuote::double_quote) {
        result = quoted_value(value, location.quote == ShellQuote::none ? '\0'
            : location.quote == ShellQuote::single ? '\'' : '"');
    } else {
        for (const auto character : value) {
            if ((location.quote == ShellQuote::ansi && (character == '\\' || character == '\'')) ||
                (location.quote == ShellQuote::heredoc_expanding &&
                 (character == '\\' || character == '$' || character == '`'))) result += '\\';
            result += character;
        }
    }
    for (std::size_t level = 0; level < location.backticks; ++level) {
        std::string escaped;
        for (const auto character : result) {
            if (character == '\\' || character == '$' || character == '`') escaped += '\\';
            escaped += character;
        }
        result = std::move(escaped);
    }
    return result;
}

struct MacroReplacement {
    std::size_t begin;
    std::size_t end;
    std::string value;
    bool raw;
};

void validate_heredocs(
    const std::string_view command,
    const std::vector<HereDocument>& documents,
    const std::vector<MacroReplacement>& replacements
) {
    for (const auto& document : documents) {
        std::string body;
        auto previous = document.begin;
        bool raw = false;
        for (const auto& replacement : replacements) {
            if (replacement.begin < document.begin || replacement.begin >= document.end) continue;
            body += command.substr(previous, replacement.begin - previous);
            if (document.strip_tabs && !replacement.raw && !replacement.value.empty()) {
                const auto newline = body.find_last_of('\n');
                const auto prefix = std::string_view(body).substr(newline == std::string::npos ? 0U : newline + 1U);
                if ((replacement.value.front() == '\t' && prefix.find_first_not_of('\t') == std::string_view::npos) ||
                    replacement.value.find("\n\t") != std::string::npos) {
                    throw std::invalid_argument("a menu macro would lose leading tabs in <<-; use << instead");
                }
            }
            body += replacement.value;
            previous = replacement.end;
            raw = raw || replacement.raw;
        }
        if (raw) continue;
        body += command.substr(previous, document.end - previous);
        if (document.strip_tabs) {
            std::string stripped;
            bool start = true;
            for (const auto character : body) {
                if (start && character == '\t') continue;
                stripped += character;
                start = character == '\n';
            }
            body = std::move(stripped);
        }
        if (!document.literal) {
            std::string joined;
            for (std::size_t index = 0; index < body.size(); ++index) {
                if (body[index] == '\\' && index + 1U < body.size()) {
                    if (body[index + 1U] != '\n') {
                        joined += body[index];
                        joined += body[index + 1U];
                    }
                    ++index;
                } else joined += body[index];
            }
            body = std::move(joined);
        }
        for (std::size_t offset = 0; offset < body.size();) {
            const auto end = body.find('\n', offset);
            auto line = std::string_view(body).substr(offset, end == std::string::npos ? end : end - offset);
            if (document.strip_tabs) {
                while (!line.empty() && line.front() == '\t') line.remove_prefix(1U);
            }
            if (line == document.delimiter) {
                throw std::invalid_argument("menu macro would terminate a here-document; use a different literal delimiter");
            }
            offset = end == std::string::npos ? body.size() : end + 1U;
        }
    }
}

struct DirectoryWord {
    std::string value;
    bool unquoted_start{false};
};

DirectoryWord directory_word(const std::string_view command) {
    DirectoryWord result;
    char quote = '\0';
    bool started = false;
    bool finished = false;
    for (std::size_t index = 0; index < command.size(); ++index) {
        const auto character = command[index];
        if (quote == '\0' && blank(character)) {
            if (started) finished = true;
            continue;
        }
        if (finished) throw std::invalid_argument("%cd needs one literal directory, not shell commands");
        if ((character == '\'' || character == '"') && (quote == '\0' || quote == character)) {
            quote = quote == '\0' ? character : '\0';
            started = true;
            continue;
        }
        if (!started) result.unquoted_start = character != '\\';
        started = true;
        if (character == '\\' && quote != '\'') {
            if (++index == command.size()) throw std::invalid_argument("unfinished escape in %cd directory");
            const auto next = command[index];
            if (quote == '"' && std::string_view("$`\"\\\n").find(next) == std::string_view::npos) result.value += '\\';
            if (next != '\n') result.value += next;
        } else {
            if ((quote != '\'' && (character == '$' || character == '`')) ||
                (quote == '\0' && std::string_view(";&|<>()*?[]{}").find(character) != std::string_view::npos)) {
                throw std::invalid_argument("%cd accepts literal paths and menu macros, not shell expansion or commands");
            }
            result.value += character;
        }
    }
    if (quote != '\0') throw std::invalid_argument("unfinished quoting in %cd directory");
    if (result.value.empty()) throw std::invalid_argument("%cd directory is empty");
    return result;
}

std::string home_directory(const std::string_view value) {
    const auto slash = value.find('/');
    const auto user = std::string(value.substr(1U, slash == std::string_view::npos ? slash : slash - 1U));
    std::string home;
    if (user.empty()) {
        const auto* environment = std::getenv("HOME");
        if (environment == nullptr || *environment == '\0') throw std::invalid_argument("%cd: HOME is not set");
        home = environment;
    } else {
        std::vector<char> buffer(1024U);
        struct passwd information {};
        struct passwd* found = nullptr;
        int error;
        while ((error = getpwnam_r(user.c_str(), &information, buffer.data(), buffer.size(), &found)) == ERANGE &&
               buffer.size() < 1024U * 1024U) buffer.resize(buffer.size() * 2U);
        if (error != 0) throw std::system_error(error, std::generic_category(), "%cd: look up home directory");
        if (found == nullptr || information.pw_dir == nullptr || *information.pw_dir == '\0') {
            throw std::invalid_argument("%cd: unknown home directory: " + user);
        }
        home = information.pw_dir;
    }
    if (!std::filesystem::path(home).is_absolute()) throw std::invalid_argument("%cd: home directory is not absolute");
    return home + (slash == std::string_view::npos ? std::string{} : std::string(value.substr(slash)));
}

bool directory_exists(const std::filesystem::path& path, const bool allow_regular_file = false) {
    struct stat information {};
    if (::stat(path.c_str(), &information) == 0) {
        return S_ISDIR(information.st_mode) || (allow_regular_file && S_ISREG(information.st_mode));
    }
    if (errno == ENOENT || errno == ENOTDIR) return false;
    throw std::system_error(errno, std::generic_category(), "%cd: inspect " + path.string());
}

}  // namespace

std::vector<NativeMenuEntry> parse_native_menu(
    const std::string_view content,
    const std::string_view source,
    bool shell_patterns
) {
    if (content.find('\0') != std::string_view::npos) menu_error(source, 1, "NUL in menu file");
    std::vector<NativeMenuEntry> entries;
    std::vector<NativeMenuCondition> pending;
    std::optional<NativeMenuEntry> current;
    std::vector<std::string> commands;
    bool comment_block = false;
    bool first_content = true;
    const auto finish = [&] {
        if (current && !commands.empty()) {
            current->command = dedent_command(commands);
            if (!trim(current->command).empty()) entries.push_back(std::move(*current));
        }
        current.reset();
        commands.clear();
    };
    std::size_t line_number = 0;
    for (std::size_t offset = 0; offset < content.size();) {
        ++line_number;
        const auto end = content.find('\n', offset);
        auto line = content.substr(offset, end == std::string_view::npos ? end : end - offset);
        offset = end == std::string_view::npos ? content.size() : end + 1U;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1U);
        if (line.empty()) {
            if (current) commands.emplace_back();
            continue;
        }
        if (line.front() == ' ' || line.front() == '\t') {
            if (comment_block || trim(line).empty()) continue;
            if (!current) menu_error(source, line_number, "indented command has no menu entry");
            commands.emplace_back(line);
            continue;
        }
        finish();
        if (line.front() == '#') {
            comment_block = true;
            continue;
        }
        comment_block = false;
        if (line.starts_with("shell_patterns=")) {
            if (!first_content || (line != "shell_patterns=0" && line != "shell_patterns=1")) {
                menu_error(source, line_number, "shell_patterns=0 or shell_patterns=1 must precede all entries");
            }
            shell_patterns = line.back() == '1';
            first_content = false;
            continue;
        }
        first_content = false;
        if (line.front() == '+' || line.front() == '=') {
            pending.push_back(parse_condition(line, source, line_number, shell_patterns));
            continue;
        }
        const auto first = line.find('|');
        const auto second = first == 1U ? line.find('|', first + 1U) : std::string_view::npos;
        NativeMenuEntry entry;
        entry.key = line.front();
        entry.source = source;
        entry.line = line_number;
        entry.conditions = std::move(pending);
        pending.clear();
        if (first == 1U && second != std::string_view::npos) {
            if (second + 1U < line.size()) {
                entry.label = line.substr(first + 1U, second - first - 1U);
                entry.command = line.substr(second + 1U);
                entries.push_back(std::move(entry));
            }
        } else {
            entry.native = true;
            entry.label = trim(line.substr(1U));
            if (entry.label.empty()) menu_error(source, line_number, "menu entry needs a title");
            current = std::move(entry);
        }
    }
    finish();
    if (!pending.empty()) menu_error(source, pending.front().line, "condition has no following menu entry");
    return entries;
}

NativeMenuSelection select_native_menu(
    const std::vector<NativeMenuEntry>& entries,
    const NativeMenuContext& context
) {
    NativeMenuSelection result;
    bool default_found = false;
    for (const auto& entry : entries) {
        bool visible = true;
        bool preferred = false;
        for (const auto& condition : entry.conditions) {
            bool matches = false;
            try {
                matches = evaluate_condition(condition, context);
            } catch (const std::exception& error) {
                menu_error(entry.source, condition.line, error.what());
            }
            if (condition.addition) visible = visible && matches;
            if (condition.default_selection) preferred = preferred || matches;
            if (condition.debug) {
                result.trace.push_back(
                    std::to_string(condition.line) + ": " + entry.label + ": " +
                    (matches ? "true" : "false")
                );
            }
        }
        if (!visible) continue;
        if (preferred && !default_found) {
            result.selected = result.entries.size();
            default_found = true;
        }
        result.entries.push_back(entry);
    }
    return result;
}

NativeMenuRequirements native_menu_requirements(const std::string_view command, const bool editor) {
    if (command.find('\0') != std::string_view::npos) throw std::invalid_argument("NUL in user-menu command");
    NativeMenuRequirements result;
    for (const auto& location : command_macros(command).macros) {
        const auto& macro = location.macro;
        if (macro.kind == MacroKind::prompt) result.prompts.push_back(macro.argument);
        if (editor && macro.kind == MacroKind::scalar) {
            if (macro.code == 'b' || macro.code == 'k') result.block_file = true;
            if (macro.code == 'e') result.error_file = true;
        }
    }
    return result;
}

NativeMenuExpansion expand_native_menu(
    std::string_view command,
    const NativeMenuContext& context,
    const std::vector<std::string>& answers
) {
    if (command.find('\0') != std::string_view::npos) throw std::invalid_argument("NUL in user-menu command");
    NativeMenuExpansion result;
    command = trim(command);
    if (command.starts_with('%')) {
        const auto directive = read_macro(command, 0);
        if (directive.kind == MacroKind::view || directive.kind == MacroKind::change_directory) {
            result.action = directive.kind == MacroKind::view ? NativeMenuAction::view
                                                            : NativeMenuAction::change_directory;
            if (directive.raw) throw std::invalid_argument("quoting prefixes do not apply to menu directives");
            if (directive.kind == MacroKind::view) viewer_options(directive.argument, result);
            command.remove_prefix(directive.end);
            command = trim(command);
        }
    }
    const bool shell_command = result.action != NativeMenuAction::change_directory && !command.empty();
    if (shell_command && !context.active.local) {
        throw std::invalid_argument("copy archive members to a local directory before running file commands");
    }
    auto active_tagged = context.active.tagged;
    auto other_tagged = context.other.tagged;
    const auto scanned = command_macros(command);
    std::vector<MacroReplacement> replacements;
    std::size_t prompt = 0;
    std::size_t previous = 0;
    for (const auto& location : scanned.macros) {
        result.command += command.substr(previous, location.begin - previous);
        previous = location.macro.end;
        const auto& macro = location.macro;
        const auto quote = location.quote;
        if (macro.kind == MacroKind::literal) {
            result.command += '%';
            replacements.push_back({location.begin, macro.end, "%", false});
            continue;
        }
        if (macro.kind == MacroKind::view || macro.kind == MacroKind::change_directory) {
            throw std::invalid_argument("%view and %cd must be the first command in a menu entry");
        }
        std::vector<std::string> values;
        bool list = false;
        if (macro.kind == MacroKind::prompt) {
            if (prompt >= answers.size()) throw std::invalid_argument("user-menu prompt has not been answered");
            values.push_back(answers[prompt++]);
        } else if (macro.kind == MacroKind::environment) {
            values.push_back(environment_value(macro.argument));
        } else {
            const auto code = macro.code;
            const bool other = code == 'F' || code == 'P' || code == 'D' || code == 'T' || code == 'U' || code == 'S';
            const auto& pane = other ? context.other : context.active;
            if (other && !pane.local && result.action != NativeMenuAction::change_directory) {
                throw std::invalid_argument("other-pane file macros are unavailable for archive or remote directories");
            }
            auto& tagged = other ? other_tagged : active_tagged;
            switch (code) {
                case 'f':
                case 'p':
                case 'P':
                case 'F': values.push_back(file_name(pane)); break;
                case 'd':
                case 'D': values.push_back(pane.directory); break;
                case 'x': values.push_back(extension(file_name(pane))); break;
                case 'b':
                    if (context.editor) {
                        if (context.block_file.empty()) throw std::invalid_argument("editor block file is unavailable");
                        values.push_back(context.block_file);
                    } else values.push_back(without_extension(file_name(pane)));
                    break;
                case 'n':
                    if (!context.editor) throw std::invalid_argument("%n is only available in editor menus");
                    values.push_back(without_extension(file_name(pane)));
                    break;
                case 'k':
                    if (!context.editor || context.block_file.empty()) throw std::invalid_argument("editor block file is unavailable");
                    values.push_back(context.block_file);
                    break;
                case 'e':
                    if (!context.editor || context.error_file.empty()) throw std::invalid_argument("editor error file is unavailable");
                    values.push_back(context.error_file);
                    break;
                case 'm': values.push_back(context.menu_file); break;
                case 'c':
                    if (!context.editor) throw std::invalid_argument("%c is only available in editor menus");
                    values.push_back(std::to_string(context.column));
                    break;
                case 'i':
                    if (!context.editor) throw std::invalid_argument("%i is only available in editor menus");
                    values.emplace_back(context.column, ' ');
                    break;
                case 'y':
                    if (!context.editor) throw std::invalid_argument("%y is only available in editor menus");
                    values.push_back(context.syntax);
                    break;
                case 't':
                case 'T':
                case 'u':
                case 'U':
                case 's':
                case 'S':
                    list = true;
                    for (const auto& file : tagged) values.push_back(file.name);
                    if ((code == 's' || code == 'S') && values.empty() && pane.file) {
                        values.push_back(file_name(pane));
                        list = false;
                    }
                    if (code == 'u' || code == 'U') {
                        auto& untag = other ? result.untag_other : result.untag_active;
                        for (const auto& file : tagged) untag.push_back(file.path);
                        tagged.clear();
                    }
                    break;
                default: throw std::invalid_argument("unsupported user-menu macro");
            }
        }
        std::string replacement;
        if (list && location.arithmetic && !macro.raw && values.size() != 1U) {
            throw std::invalid_argument("a tagged-file macro in arithmetic needs exactly one integer literal");
        }
        if (list && !location.arithmetic && (quote != ShellQuote::none || macro.raw)) {
            std::string joined;
            for (const auto& value : values) {
                joined += value;
                joined += ' ';
            }
            replacement = macro_value(joined, location);
        } else {
            bool first = true;
            for (const auto& value : values) {
                if (!first) replacement += ' ';
                replacement += macro_value(value, location);
                first = false;
            }
        }
        result.command += replacement;
        replacements.push_back({location.begin, macro.end, std::move(replacement), macro.raw});
    }
    result.command += command.substr(previous);
    validate_heredocs(command, scanned.heredocs, replacements);
    if (prompt != answers.size()) throw std::invalid_argument("too many answers for user-menu prompts");
    if (result.action == NativeMenuAction::change_directory && trim(result.command).empty()) {
        throw std::invalid_argument("%cd needs a directory");
    }
    if (result.action == NativeMenuAction::shell && trim(result.command).empty()) {
        throw std::invalid_argument("user-menu command is empty");
    }
    return result;
}

std::string native_menu_directory(
    const std::string_view expanded_command,
    const NativeMenuContext& context,
    const std::string_view previous_directory
) {
    if (expanded_command.find('\0') != std::string_view::npos) throw std::invalid_argument("NUL in %cd directory");
    auto word = directory_word(expanded_command);
    if (word.unquoted_start && word.value == "-") {
        if (previous_directory.empty()) throw std::invalid_argument("%cd: no previous directory");
        return std::string(previous_directory);
    }
    if (word.unquoted_start && word.value.starts_with('~')) return home_directory(word.value);
    auto path = std::filesystem::path(word.value);
    if (path.is_absolute() || word.value.find("://") != std::string::npos || !context.active.local) return word.value;
    const auto base = std::filesystem::path(context.active.directory);
    if (!base.is_absolute()) throw std::invalid_argument("%cd: captured local directory is not absolute");
    const auto current = base / path;
    // The caller still needs to decide whether a regular file is an enterable archive.
    if (directory_exists(current, true)) return current.lexically_normal().string();
    if (const auto* environment = std::getenv("CDPATH"); environment != nullptr) {
        const auto search = std::string_view(environment);
        for (std::size_t offset = 0; offset <= search.size();) {
            const auto end = search.find(':', offset);
            auto directory = search.substr(offset, end == std::string_view::npos ? end : end - offset);
            if (!directory.empty()) {
                auto prefix = std::filesystem::path(directory.starts_with('~') ? home_directory(directory) : std::string(directory));
                if (prefix.is_relative()) prefix = base / prefix;
                const auto candidate = prefix / path;
                if (directory_exists(candidate)) return candidate.lexically_normal().string();
            }
            if (end == std::string_view::npos) break;
            offset = end + 1U;
        }
    }
    throw std::invalid_argument("%cd: directory not found in captured directory or CDPATH: " + word.value);
}

}  // namespace mcpp
