#include "mcpp/completion.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <unistd.h>

extern char** environ;

namespace mcpp {
namespace {

using Quote = CompletionQuote;

constexpr std::array posix_commands{
    "!", ".", ":", "[", "alias", "bg", "break", "case", "cd", "command",
    "continue", "do", "done", "echo", "elif", "else", "esac", "eval", "exec",
    "exit", "export", "false", "fc", "fg", "fi", "for", "getopts", "hash",
    "if", "in", "jobs", "kill", "printf", "pwd", "read", "readonly", "return",
    "set", "shift", "test", "then", "times", "trap", "true", "type", "ulimit",
    "umask", "unalias", "unset", "until", "wait", "while", "{", "}"
};

constexpr std::array bash_commands{
    "[[", "]]", "bind", "builtin", "caller", "compgen", "complete", "compopt",
    "coproc", "declare", "dirs", "disown", "enable", "function", "help",
    "history", "let", "local", "logout", "mapfile", "popd", "pushd",
    "readarray", "select", "shopt", "source", "suspend", "time", "typeset"
};

bool name_start(const char byte) {
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
           byte == '_';
}

bool name_byte(const char byte) {
    return name_start(byte) || (byte >= '0' && byte <= '9');
}

bool identifier(const std::string_view name) {
    return !name.empty() && name_start(name.front()) &&
           std::all_of(name.begin(), name.end(), name_byte);
}

bool space(const char byte) {
    return byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r';
}

bool shell_operator(const char byte) {
    return byte == ';' || byte == '|' || byte == '&' || byte == '(' ||
           byte == ')' || byte == '<' || byte == '>';
}

std::size_t boundary(const std::string_view value, std::size_t position) {
    position = std::min(position, value.size());
    while (position > 0U && position < value.size() &&
           (static_cast<unsigned char>(value[position]) & 0xC0U) == 0x80U) {
        --position;
    }
    return position;
}

struct Character {
    char byte;
    std::size_t begin;
    std::size_t end;
    Quote quote;
    bool escaped;
};

struct Word {
    std::size_t begin{0};
    std::size_t end{0};
    std::vector<Character> characters;
    std::vector<Quote> quotes;
    bool command{false};
    bool quoted{false};
};

Word shell_word(const std::string_view value, const std::size_t begin) {
    Word word{.begin=begin, .end=begin, .characters={}, .quotes={Quote::unquoted}};
    auto quote = Quote::unquoted;
    auto position = begin;
    const auto advance = [&](const std::size_t count, const Quote state) {
        word.quotes.insert(word.quotes.end(), count, state);
    };
    while (position < value.size()) {
        const auto byte = value[position];
        if (quote == Quote::unquoted && (space(byte) || shell_operator(byte))) break;
        if ((byte == '\'' && quote != Quote::double_quote) ||
            (byte == '"' && quote != Quote::single)) {
            quote = quote == Quote::unquoted
                        ? (byte == '\'' ? Quote::single : Quote::double_quote)
                        : Quote::unquoted;
            word.quoted = true;
            advance(1U, quote);
            ++position;
            continue;
        }
        if (byte == '\\' && quote != Quote::single) {
            if (position + 1U == value.size()) {
                advance(1U, quote);
                ++position;
                continue;
            }
            const auto next = value[position + 1U];
            if (quote == Quote::unquoted || next == '$' || next == '`' ||
                next == '"' || next == '\\' || next == '\n') {
                if (next != '\n') {
                    word.characters.push_back({next, position, position + 2U, quote, true});
                }
                advance(2U, quote);
                position += 2U;
                continue;
            }
        }
        if (quote != Quote::single &&
            (byte == '`' || (byte == '$' && position + 1U < value.size() &&
                             value[position + 1U] == '('))) {
            throw std::runtime_error("Completion does not evaluate shell substitutions");
        }
        word.characters.push_back({byte, position, position + 1U, quote, false});
        advance(1U, quote);
        ++position;
    }
    word.end = position;
    return word;
}

std::string decoded(const Word& word, const std::size_t before) {
    std::string result;
    for (const auto& character : word.characters) {
        if (character.end <= before) result += character.byte;
    }
    return result;
}

bool command_prefix(const Word& word, const std::string_view token) {
    if (token == "command" || token == "builtin" || token == "exec") return true;
    if (word.quoted || std::any_of(word.characters.begin(), word.characters.end(),
        [](const Character& character) { return character.escaped; })) return false;
    constexpr std::array prefixes{
        "!", "{", "if", "then", "elif", "else", "while", "until", "do",
        "time", "coproc"
    };
    return std::find(prefixes.begin(), prefixes.end(), token) != prefixes.end();
}

Word current_word(const std::string_view value, const std::size_t cursor) {
    bool command = true;
    bool redirect = false;
    auto position = std::size_t{0};
    while (position < cursor) {
        if (space(value[position])) {
            if (value[position] == '\n') { command = true; redirect = false; }
            ++position;
            continue;
        }
        if (shell_operator(value[position])) {
            const auto byte = value[position++];
            if (byte == '<' || byte == '>' ||
                (byte == '&' && position < value.size() && value[position] == '>')) {
                while (position < cursor &&
                       (value[position] == '<' || value[position] == '>')) ++position;
                if (position < cursor && value[position] == '&') ++position;
                redirect = true;
            } else {
                command = true;
                redirect = false;
            }
            continue;
        }
        if (value[position] == '#') {
            throw std::runtime_error("Completion is not available in shell comments");
        }
        auto word = shell_word(value, position);
        if (word.end >= cursor) {
            word.command = command && !redirect;
            return word;
        }
        const auto token = decoded(word, word.end);
        const bool descriptor = word.end < value.size() &&
            (value[word.end] == '<' || value[word.end] == '>') &&
            !token.empty() && std::all_of(token.begin(), token.end(), [](const char byte) {
                return byte >= '0' && byte <= '9';
            });
        const auto equal = value.substr(word.begin, word.end - word.begin).find('=');
        const bool assignment = equal != std::string_view::npos &&
            identifier(value.substr(word.begin, equal));
        if (redirect) redirect = false;
        else if (!descriptor && !(command && (assignment || command_prefix(word, token)))) command = false;
        position = word.end;
    }
    if (cursor < value.size() && value[cursor] == '#') {
        throw std::runtime_error("Completion is not available in shell comments");
    }
    auto word = shell_word(value, cursor);
    word.command = command && !redirect;
    return word;
}

std::vector<std::string> local_lines(const std::filesystem::path& path) {
    errno = 0;
    std::ifstream input(path);
    if (!input) {
        throw std::system_error(errno == 0 ? EIO : errno, std::generic_category(),
                                "read completion source " + path.string());
    }
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(input, line)) lines.push_back(std::move(line));
    if (input.bad()) throw std::runtime_error("cannot read completion source " + path.string());
    return lines;
}

struct User {
    std::string name;
    std::string home;
};

std::vector<User> local_users() {
    std::vector<User> users;
    for (const auto& line : local_lines("/etc/passwd")) {
        if (line.empty() || line.starts_with('#')) continue;
        std::array<std::string, 7> fields;
        std::size_t begin = 0;
        for (std::size_t index = 0; index < fields.size() - 1U; ++index) {
            const auto end = line.find(':', begin);
            if (end == std::string::npos) {
                throw std::runtime_error("Malformed local passwd entry in completion source");
            }
            fields[index] = line.substr(begin, end - begin);
            begin = end + 1U;
        }
        if (fields[0].empty()) {
            throw std::runtime_error("Malformed local passwd entry in completion source");
        }
        users.push_back({std::move(fields[0]), std::move(fields[5])});
    }
    return users;
}

std::string home_directory(const std::string_view directory) {
    if (!directory.starts_with('~')) return std::string(directory);
    const auto slash = directory.find('/');
    if (slash == std::string_view::npos) return std::string(directory);
    std::string home;
    if (slash == 1U) {
        const auto* value = std::getenv("HOME");
        if (value == nullptr || *value == '\0') throw std::runtime_error("HOME is not set");
        home = value;
    } else {
        const auto users = local_users();
        const auto name = directory.substr(1U, slash - 1U);
        const auto found = std::find_if(users.begin(), users.end(), [&](const User& user) {
            return user.name == name;
        });
        if (found == users.end()) throw std::runtime_error("Unknown local user: " + std::string(name));
        home = found->home;
    }
    if (home.empty() || !std::filesystem::path(home).is_absolute()) {
        throw std::runtime_error("Completion home directory is not an absolute path");
    }
    return home + std::string(directory.substr(slash));
}

void warn(CompletionResult& result, const std::string& action, const std::error_code error) {
    if (result.warning.empty()) result.warning = action + ": " + error.message();
}

void commands(CompletionResult& result, const std::string_view prefix,
              const std::string_view directory) {
    const auto add = [&](const auto& names) {
        for (const auto* name : names) {
            if (std::string_view(name).starts_with(prefix)) result.matches.emplace_back(name);
        }
    };
    add(posix_commands);
    const auto* shell = std::getenv("SHELL");
    const auto shell_name = shell == nullptr ? std::string{} : std::filesystem::path(shell).filename().string();
    if (shell_name == "bash" || shell_name == "rbash") add(bash_commands);
    const auto* path = std::getenv("PATH");
    if (path == nullptr) {
        result.warning = "PATH is not set; only shell words are available";
        return;
    }
    const auto directories = std::string_view(path);
    std::size_t offset = 0;
    for (;;) {
        const auto end = directories.find(':', offset);
        auto source = std::filesystem::path(directories.substr(
            offset, end == std::string_view::npos ? end : end - offset));
        if (source.empty() || source.is_relative()) source = std::filesystem::path(directory) / source;
        std::error_code error;
        auto iterator = std::filesystem::directory_iterator(source, error);
        if (error) warn(result, "cannot read PATH directory " + source.string(), error);
        const auto finish = std::filesystem::directory_iterator{};
        while (!error && iterator != finish) {
            const auto name = iterator->path().filename().string();
            if (name.starts_with(prefix) && (!name.starts_with('.') || prefix.starts_with('.'))) {
                std::error_code status_error;
                const bool regular = iterator->is_regular_file(status_error);
                if (status_error) {
                    warn(result, "cannot inspect command " + iterator->path().string(), status_error);
                } else if (regular) {
                    if (faccessat(AT_FDCWD, iterator->path().c_str(), X_OK, AT_EACCESS) == 0) {
                        result.matches.push_back(name);
                    } else if (errno != EACCES && errno != ENOENT) {
                        warn(result, "cannot inspect executable " + iterator->path().string(),
                             std::error_code(errno, std::generic_category()));
                    }
                }
            }
            iterator.increment(error);
            if (error) warn(result, "cannot read PATH directory " + source.string(), error);
        }
        if (end == std::string_view::npos) break;
        offset = end + 1U;
    }
}

bool hostname_byte(const char byte) {
    return name_byte(byte) || byte == '-' || byte == '.';
}

void hosts(CompletionResult& result, const std::string_view prefix) {
    const auto add = [&](const std::string_view name) {
        if (!name.empty() && name.starts_with(prefix) &&
            std::all_of(name.begin(), name.end(), hostname_byte)) {
            result.matches.emplace_back(name);
        }
    };
    for (const auto& line : local_lines("/etc/hosts")) {
        std::istringstream record(line.substr(0, line.find('#')));
        std::string address;
        if (!(record >> address)) continue;
        std::string name;
        while (record >> name) add(name);
    }
    std::array<char, 256> hostname{};
    if (gethostname(hostname.data(), hostname.size()) != 0) {
        throw std::system_error(errno, std::generic_category(), "read local hostname for completion");
    }
    hostname.back() = '\0';
    add(hostname.data());
}

Quote quote_at(const Word& word, const std::size_t position) {
    return word.quotes.empty() ? Quote::literal : word.quotes.at(position - word.begin);
}

std::string escape(const std::string_view match, const Quote quote) {
    std::string result;
    for (const auto byte : match) {
        if (quote == Quote::single && byte == '\'') result += "'\\''";
        else if (quote == Quote::double_quote &&
                 (byte == '"' || byte == '\\' || byte == '$' || byte == '`')) {
            result += '\\';
            result += byte;
        } else if (quote == Quote::unquoted &&
                   static_cast<unsigned char>(byte) < 0x80U &&
                   !name_byte(byte) && std::string_view("/.:-@%+=,").find(byte) == std::string_view::npos) {
            if (byte == '\n') result += "'\n'";
            else { result += '\\'; result += byte; }
        } else result += byte;
    }
    return result;
}

std::string transition(const Quote from, const Quote to) {
    if (from == to || from == Quote::literal || to == Quote::literal) return {};
    std::string result;
    if (from == Quote::single) result += '\'';
    else if (from == Quote::double_quote) result += '"';
    if (to == Quote::single) result += '\'';
    else if (to == Quote::double_quote) result += '"';
    return result;
}

}  // namespace

CompletionResult complete_input(
    const std::string_view value, std::size_t cursor, const std::optional<std::size_t> anchor,
    const bool command_line, const CompletionPathSource& paths,
    const std::string_view command_directory
) {
    cursor = boundary(value, cursor);
    auto text = std::string(value);
    if (anchor) {
        const auto other = boundary(value, *anchor);
        if (other != cursor) {
            const auto begin = std::min(other, cursor);
            text.erase(begin, std::max(other, cursor) - begin);
            cursor = begin;
        }
    }
    Word word;
    if (command_line) word = current_word(text, cursor);
    else {
        word.end = text.size();
        for (std::size_t offset = 0; offset < text.size(); ++offset) {
            word.characters.push_back({text[offset], offset, offset + 1U, Quote::literal, false});
        }
    }
    const auto prefix = decoded(word, cursor);
    CompletionResult result;
    auto begin = word.begin;
    auto end = word.end;
    auto stem = prefix;
    std::string directory;
    std::optional<std::string> literal_home_prefix;
    bool home = false;
    const auto& characters = word.characters;
    const bool first_active = !characters.empty() && !characters.front().escaped;
    const bool variable = !prefix.empty() && prefix.front() == '$' && first_active &&
                          characters.front().quote != Quote::single;
    const bool username = !prefix.empty() && prefix.front() == '~' && first_active &&
                          (characters.front().quote == Quote::unquoted || !command_line);
    const bool hostname = !prefix.empty() && prefix.front() == '@' && first_active;
    if (variable) {
        result.kind = CompletionKind::variable;
        const bool braces = prefix.starts_with("${");
        const auto marker = braces ? 2U : 1U;
        stem = prefix.substr(marker);
        if (!std::all_of(stem.begin(), stem.end(), name_byte)) {
            throw std::runtime_error("Completion does not expand variables or parameter operators");
        }
        if (command_line) {
            for (std::size_t index = 1U; index < prefix.size(); ++index) {
                if (characters[index].escaped ||
                    characters[index].quote != characters.front().quote ||
                    characters[index].begin != characters[index - 1U].end) {
                    throw std::runtime_error("Completion does not evaluate quoted parameter fragments");
                }
            }
        }
        begin = characters.at(marker - 1U).end;
        std::size_t index = marker;
        while (index < characters.size() && name_byte(characters[index].byte)) ++index;
        end = index < characters.size() ? characters[index].begin : word.end;
        result.target.close_variable = braces &&
            (index == characters.size() || characters[index].byte != '}');
        for (auto** item = environ; item != nullptr && *item != nullptr; ++item) {
            const auto entry = std::string_view(*item);
            const auto equal = entry.find('=');
            const auto name = entry.substr(0, equal);
            if (equal != std::string_view::npos && identifier(name) && name.starts_with(stem)) {
                result.matches.emplace_back(name);
            }
        }
    } else if ((username || hostname) && prefix.find('/') == std::string::npos) {
        result.kind = username ? CompletionKind::username : CompletionKind::hostname;
        begin = characters.front().end;
        stem = prefix.substr(1U);
        for (const auto& character : characters) {
            if (character.begin >= cursor && character.byte == '/') {
                end = character.begin;
                result.target.suffix_slash_bytes = character.end - character.begin;
                break;
            }
        }
        if (username) {
            for (const auto& user : local_users()) {
                if (user.name.starts_with(stem)) result.matches.push_back(user.name + "/");
            }
        } else hosts(result, stem);
    } else {
        const auto slash = prefix.rfind('/');
        if (slash != std::string::npos) {
            directory = prefix.substr(0, slash + 1U);
            stem = prefix.substr(slash + 1U);
            begin = characters.at(slash).end;
        }
        for (const auto& character : characters) {
            if (character.begin >= cursor && character.byte == '/') {
                end = character.begin;
                result.target.suffix_slash_bytes = character.end - character.begin;
                break;
            }
            if (command_line && character.end <= cursor && !character.escaped &&
                character.quote != Quote::single && character.byte == '$') {
                throw std::runtime_error("Path completion does not evaluate shell expansions");
            }
        }
        home = username;
        if (word.command && slash == std::string::npos) {
            result.kind = CompletionKind::command;
            commands(result, stem, command_directory);
        }
        if (result.matches.empty()) {
            result.kind = CompletionKind::path;
            const auto search_directory = home ? home_directory(directory) : directory;
            if (home && !command_line) literal_home_prefix = search_directory;
            for (const auto& entry : paths(search_directory)) {
                const bool is_directory = entry.kind == EntryKind::directory || entry.target_is_directory;
                if (entry.name.starts_with(stem) &&
                    (!entry.name.starts_with('.') || stem.starts_with('.')) &&
                    (result.target.suffix_slash_bytes == 0U || is_directory)) {
                    result.matches.push_back(entry.name + (is_directory ? "/" : ""));
                }
            }
        }
    }
    if (command_line && begin == word.begin && begin < text.size() &&
        (text[begin] == '\'' || text[begin] == '"')) ++begin;
    result.target.prefix = literal_home_prefix ? *literal_home_prefix : text.substr(0, begin);
    result.target.suffix = text.substr(end);
    result.target.quote = quote_at(word, begin);
    result.target.suffix_quote = quote_at(word, end);
    result.target.quoted_word = result.kind == CompletionKind::command && word.quoted &&
        begin == word.begin && result.target.quote == Quote::unquoted &&
        result.target.suffix_quote == Quote::unquoted;
    result.target.command = result.kind == CompletionKind::command;
    std::sort(result.matches.begin(), result.matches.end());
    result.matches.erase(std::unique(result.matches.begin(), result.matches.end()), result.matches.end());
    return result;
}

CompletionEdit completion_edit(const CompletionTarget& target, const std::string_view match) {
    auto inserted = escape(match, target.quote);
    if (target.quoted_word) inserted = "'" + escape(match, Quote::single) + "'";
    else if (target.command && target.quote == Quote::unquoted &&
             (match == "!" || match == "{" || match == "}" || match == "[[" || match == "]]")) {
        inserted = match;
    }
    const auto suffix = match.ends_with('/') && target.suffix_slash_bytes != 0U
        ? std::string_view(target.suffix).substr(target.suffix_slash_bytes)
        : std::string_view(target.suffix);
    return {
        target.prefix + inserted + (target.close_variable ? "}" : "") +
            transition(target.quote, target.suffix_quote) + std::string(suffix),
        target.prefix.size() + inserted.size()
    };
}

std::string completion_common_prefix(const std::vector<std::string>& matches) {
    if (matches.empty()) return {};
    auto common = matches.front();
    for (const auto& match : matches) {
        std::size_t count = 0;
        while (count < common.size() && count < match.size() && common[count] == match[count]) ++count;
        common.resize(boundary(common, count));
    }
    return common;
}

std::string_view completion_kind_name(const CompletionKind kind) {
    switch (kind) {
        case CompletionKind::path: return "paths";
        case CompletionKind::command: return "commands";
        case CompletionKind::variable: return "variables";
        case CompletionKind::username: return "users";
        case CompletionKind::hostname: return "hosts";
    }
    return "matches";
}

}  // namespace mcpp
