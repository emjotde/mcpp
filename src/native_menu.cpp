#include "mcpp/native_menu.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fnmatch.h>
#include <regex.h>
#include <stdexcept>
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
    explicit Pattern(const std::string& value) {
        const auto error = regcomp(&expression_, value.c_str(), REG_EXTENDED | REG_NOSUB);
        if (error != 0) {
            std::vector<char> description(regerror(error, &expression_, nullptr, 0));
            static_cast<void>(regerror(error, &expression_, description.data(), description.size()));
            throw std::invalid_argument("invalid menu regular expression: " + std::string(description.data()));
        }
    }
    ~Pattern() { regfree(&expression_); }
    Pattern(const Pattern&) = delete;
    Pattern& operator=(const Pattern&) = delete;
    [[nodiscard]] bool matches(const std::string& value) const {
        return regexec(&expression_, value.c_str(), 0, nullptr, 0) == 0;
    }
private:
    regex_t expression_{};
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
        if (std::string_view("fpxbdFDTtuUsSmkeciny").find(result.code) == std::string_view::npos) {
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

struct ShellNesting {
    char quote;
    std::size_t parentheses;
    bool arithmetic;
};

struct LocatedMacro {
    Macro macro;
    std::size_t begin;
    char quote;
};

std::vector<LocatedMacro> command_macros(const std::string_view command) {
    std::vector<LocatedMacro> result;
    std::vector<ShellNesting> nesting;
    char quote = '\0';
    bool escaped = false;
    bool ansi_quote = false;
    bool heredoc = false;
    for (std::size_t index = 0; index < command.size(); ++index) {
        const auto character = command[index];
        if (escaped && character != '%') {
            escaped = false;
            continue;
        }
        if (character == '\\' && quote != '\'') {
            escaped = true;
            continue;
        }
        if (character == '#' && quote == '\0' &&
            (index == 0U || blank(command[index - 1U]) ||
             std::string_view(";|&()").find(command[index - 1U]) != std::string_view::npos)) {
            const auto end = command.find('\n', index);
            if (end == std::string_view::npos) break;
            index = end;
            continue;
        }
        if (character == '`' && quote != '\'') {
            auto end = index + 1U;
            for (; end < command.size(); ++end) {
                if (command[end] == '\\' && end + 1U < command.size()) ++end;
                else if (command[end] == '%') {
                    throw std::invalid_argument("menu macros inside backticks are unavailable; use $(...) instead");
                } else if (command[end] == '`') break;
            }
            if (end == command.size()) throw std::invalid_argument("unfinished backticks in user-menu command");
            index = end;
            continue;
        }
        if (character == '$' && quote != '\'' && index + 1U < command.size() && command[index + 1U] == '(') {
            nesting.push_back({quote, 1U, index + 2U < command.size() && command[index + 2U] == '('});
            quote = '\0';
            ++index;
            continue;
        }
        if (character == '(' && quote == '\0' && index + 1U < command.size() && command[index + 1U] == '(') {
            nesting.push_back({quote, 2U, true});
            ++index;
            continue;
        }
        if (!nesting.empty() && quote == '\0') {
            if (command.substr(index).starts_with("case") &&
                (index == 0U || blank(command[index - 1U]) || command[index - 1U] == '(') &&
                index + 4U < command.size() && blank(command[index + 4U])) {
                throw std::invalid_argument("case inside a menu command substitution is unavailable; use a separate command block");
            }
            if (character == '(') ++nesting.back().parentheses;
            else if (character == ')' && --nesting.back().parentheses == 0U) {
                quote = nesting.back().quote;
                nesting.pop_back();
                continue;
            }
        }
        if ((character == '\'' || character == '"') && (quote == '\0' || quote == character)) {
            if (quote == '\0') {
                quote = character;
                ansi_quote = character == '\'' && index != 0U && command[index - 1U] == '$';
            } else {
                quote = '\0';
                ansi_quote = false;
            }
            continue;
        }
        if (character == '<' && quote == '\0' && index + 1U < command.size() && command[index + 1U] == '<') {
            heredoc = true;
        }
        if (character != '%') continue;
        const auto macro = read_macro(command, index);
        const bool arithmetic = std::ranges::any_of(nesting, [](const ShellNesting& level) { return level.arithmetic; });
        if (macro.kind != MacroKind::literal && (escaped || ansi_quote || heredoc || arithmetic)) {
            throw std::invalid_argument(
                "menu macros cannot follow a backslash or occur in ANSI quotes, arithmetic, or here-documents; "
                "use ordinary quoted arguments"
            );
        }
        result.push_back({macro, index, quote});
        index = macro.end - 1U;
        escaped = false;
    }
    if (quote != '\0' || escaped || !nesting.empty()) {
        throw std::invalid_argument("unfinished shell quoting in user-menu command");
    }
    return result;
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
    NativeMenuRequirements result;
    for (const auto& location : command_macros(command)) {
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
    std::size_t prompt = 0;
    std::size_t previous = 0;
    for (const auto& location : command_macros(command)) {
        result.command += command.substr(previous, location.begin - previous);
        previous = location.macro.end;
        const auto& macro = location.macro;
        const auto quote = location.quote;
        if (macro.kind == MacroKind::literal) {
            result.command += '%';
            continue;
        }
        if (macro.raw && quote == '\0') {
            throw std::invalid_argument("unquoted %0 menu macros are unsafe; use a quoted macro or the default quoting");
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
            const bool other = code == 'F' || code == 'D' || code == 'T' || code == 'U' || code == 'S';
            const auto& pane = other ? context.other : context.active;
            if (other && !pane.local && result.action != NativeMenuAction::change_directory) {
                throw std::invalid_argument("other-pane file macros are unavailable for archive or remote directories");
            }
            auto& tagged = other ? other_tagged : active_tagged;
            switch (code) {
                case 'f':
                case 'p':
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
                    if ((code == 's' || code == 'S') && values.empty() && pane.file) values.push_back(file_name(pane));
                    if (code == 'u' || code == 'U') {
                        auto& untag = other ? result.untag_other : result.untag_active;
                        for (const auto& file : tagged) untag.push_back(file.path);
                        tagged.clear();
                    }
                    break;
                default: throw std::invalid_argument("unsupported user-menu macro");
            }
        }
        if (list && quote != '\0') throw std::invalid_argument("tagged-file menu macros must be outside quotes");
        bool first = true;
        for (const auto& value : values) {
            if (!first) result.command += ' ';
            result.command += quoted_value(value, quote);
            first = false;
        }
    }
    result.command += command.substr(previous);
    if (prompt != answers.size()) throw std::invalid_argument("too many answers for user-menu prompts");
    if (result.action == NativeMenuAction::change_directory && trim(result.command).empty()) {
        throw std::invalid_argument("%cd needs a directory");
    }
    if (result.action == NativeMenuAction::shell && trim(result.command).empty()) {
        throw std::invalid_argument("user-menu command is empty");
    }
    return result;
}

}  // namespace mcpp
