#include "mcpp/extension.hpp"

#include "mcpp/editor_search.hpp"
#include "mcpp/local_tools.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <fcntl.h>
#include <map>
#include <regex.h>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace mcpp {
namespace {

std::string_view trim(std::string_view value) {
    const auto begin = value.find_first_not_of(" \t\r");
    if (begin == std::string_view::npos) return {};
    return value.substr(begin, value.find_last_not_of(" \t\r") - begin + 1U);
}

[[noreturn]] void extension_error(
    const std::string_view source, const std::size_t line, const std::string& message
) {
    throw std::invalid_argument(
        message + " (" + (source.empty() ? std::string("extension file") : std::string(source)) +
        ":" + std::to_string(line) + ")"
    );
}

struct Value {
    std::string text;
    std::size_t line{0};
};

struct Section {
    std::string name;
    std::size_t line{0};
    std::map<std::string, Value, std::less<>> values;
    bool legacy{false};
};

std::string ini_value(
    const std::string_view input, const std::string_view source, const std::size_t line
) {
    std::string result;
    for (std::size_t index = 0; index < input.size(); ++index) {
        if (input[index] != '\\') {
            result += input[index];
            continue;
        }
        if (++index == input.size()) extension_error(source, line, "unfinished INI escape");
        switch (input[index]) {
            case '\\': result += '\\'; break;
            case 's': result += ' '; break;
            case 'n': result += '\n'; break;
            case 'r': result += '\r'; break;
            case 't': result += '\t'; break;
            default: extension_error(source, line, "invalid INI escape; double literal backslashes");
        }
    }
    return result;
}

class Expression {
public:
    Expression(const Value& value, const bool ignore_case, const std::string_view source) {
        if (value.text.size() > 4096U) extension_error(source, value.line, "expression exceeds 4096 bytes");
        for (std::size_t index = 0; index + 1U < value.text.size(); ++index) {
            if (value.text[index] != '\\') continue;
            if (std::string_view("dDpPKQERXxukgzZ").find(value.text[++index]) != std::string_view::npos) {
                extension_error(source, value.line, "unsupported expression escape; use POSIX character classes");
            }
        }
        auto expression = std::make_shared<Compiled>();
        const auto result = regcomp(&expression->regex, value.text.c_str(), REG_EXTENDED | REG_NOSUB |
                                    (ignore_case ? REG_ICASE : 0));
        if (result != 0) {
            std::array<char, 256> message{};
            regerror(result, &expression->regex, message.data(), message.size());
            extension_error(source, value.line, "invalid expression (POSIX extended): " + std::string(message.data()));
        }
        expression->ready = true;
        compiled_ = std::move(expression);
    }

    [[nodiscard]] bool matches(const std::string& value) const {
        const auto result = regexec(&compiled_->regex, value.c_str(), 0, nullptr, 0);
        if (result != 0 && result != REG_NOMATCH) throw std::runtime_error("extension matching failed");
        return result == 0;
    }

private:
    struct Compiled {
        regex_t regex{};
        bool ready{false};
        ~Compiled() { if (ready) regfree(&regex); }
    };
    std::shared_ptr<Compiled> compiled_;
};

struct Rule {
    std::string name;
    std::size_t line{0};
    std::optional<Expression> directory;
    std::optional<Expression> type;
    std::optional<Expression> mime;
    std::optional<Expression> regex;
    std::optional<std::string> shell;
    std::optional<std::string> pattern;
    bool shell_ignore_case{false};
    bool fallback{false};
    bool include_only{false};
    std::optional<std::string> include;
    std::array<std::optional<ExtensionCommand>, 3> actions;
};

constexpr std::array<std::string_view, 3> action_names{"Open", "View", "Edit"};

bool boolean_value(const Section& section, const std::string& key, const std::string_view source) {
    const auto found = section.values.find(key);
    if (found == section.values.end()) return false;
    if (found->second.text == "true") return true;
    if (found->second.text == "false") return false;
    extension_error(source, found->second.line, key + " needs true or false");
}

Rule compile_section(const Section& section, const std::string_view source) {
    Rule rule;
    rule.name = section.name;
    rule.line = section.line;
    rule.fallback = section.name == "Default";
    rule.include_only = section.name.starts_with("Include/");
    if (rule.include_only && section.name.size() == 8U) {
        extension_error(source, section.line, "Include section needs a name");
    }
    static constexpr std::array<std::string_view, 15> keys{
        "Directory", "Type", "TypeIgnoreCase", "Mime", "MimeIgnoreCase", "Regex", "RegexIgnoreCase",
        "Shell", "ShellIgnoreCase", "Include", "Open", "View", "Edit", "Pattern", "Version"
    };
    for (const auto& [key, value] : section.values) {
        if (std::ranges::find(keys, key) == keys.end() || key == "Version" ||
            (key == "Pattern" && !section.legacy)) {
            extension_error(source, value.line, "unsupported extension key: " + key);
        }
    }
    const auto expression = [&](const std::string& key, const bool ignore_case) -> std::optional<Expression> {
        const auto found = section.values.find(key);
        if (found == section.values.end()) return std::nullopt;
        return Expression(found->second, ignore_case, source);
    };
    rule.directory = expression("Directory", false);
    rule.type = expression("Type", boolean_value(section, "TypeIgnoreCase", source));
    rule.mime = expression("Mime", boolean_value(section, "MimeIgnoreCase", source));
    rule.regex = expression("Regex", boolean_value(section, "RegexIgnoreCase", source));
    rule.shell_ignore_case = boolean_value(section, "ShellIgnoreCase", source);
    for (const auto key : {"Shell", "Pattern", "Include"}) {
        const auto found = section.values.find(key);
        if (found == section.values.end()) continue;
        if (found->second.text.empty()) extension_error(source, found->second.line, std::string(key) + " is empty");
        if (std::string_view(key) == "Shell") rule.shell = found->second.text;
        else if (std::string_view(key) == "Pattern") {
            rule.pattern = found->second.text;
            static_cast<void>(EditorSearch::filename_matches("", *rule.pattern, true));
        } else rule.include = "Include/" + found->second.text;
    }
    if (!rule.directory && !rule.type && !rule.mime && !rule.regex && !rule.shell && !rule.pattern &&
        !rule.fallback && !rule.include_only) {
        extension_error(source, section.line, "section needs a filename, directory, Type, or Mime condition");
    }
    for (std::size_t index = 0; index < action_names.size(); ++index) {
        const auto found = section.values.find(action_names[index]);
        if (found != section.values.end()) {
            rule.actions[index] = ExtensionCommand{
                found->second.text, std::string(source), section.name, found->second.line, section.legacy
            };
        }
    }
    return rule;
}

bool shell_matches(std::string name, std::string shell, const bool ignore_case) {
    if (ignore_case) {
        const auto lower = [](const unsigned char character) { return static_cast<char>(std::tolower(character)); };
        std::ranges::transform(name, name.begin(), lower);
        std::ranges::transform(shell, shell.begin(), lower);
    }
    return shell.starts_with('.') ? name.ends_with(shell) : name == shell;
}

std::vector<Section> parse_sections(
    const std::string_view content, const std::string_view source, const bool ini
) {
    std::vector<Section> sections;
    std::istringstream input{std::string(content)};
    std::string line;
    std::size_t line_number = 0;
    std::optional<std::size_t> current;
    while (std::getline(input, line)) {
        ++line_number;
        const auto text = trim(line);
        if (text.empty() || text.starts_with('#')) continue;
        if (ini && text.starts_with('[')) {
            if (!text.ends_with(']') || text.size() < 3U) extension_error(source, line_number, "invalid section header");
            const auto name = std::string(trim(text.substr(1U, text.size() - 2U)));
            if (name.empty()) extension_error(source, line_number, "section name is empty");
            const auto found = std::ranges::find(sections, name, &Section::name);
            if (found == sections.end()) {
                sections.push_back({name, line_number, {}, false});
                current = sections.size() - 1U;
            } else current = static_cast<std::size_t>(found - sections.begin());
            continue;
        }
        if (!ini && line.front() != ' ' && line.front() != '\t') {
            static constexpr std::array<std::pair<std::string_view, std::string_view>, 7> headers{{
                {"shell/", "Shell"}, {"regex/", "Regex"}, {"type/", "Type"},
                {"directory/", "Directory"}, {"include/", "Include"}, {"default/", "Default"},
                {"mime/", "Mime"}
            }};
            bool header = false;
            for (const auto& [prefix, key] : headers) {
                if (!text.starts_with(prefix)) continue;
                auto operand = text.substr(prefix.size());
                bool ignore_case = false;
                if ((key == "Shell" || key == "Regex" || key == "Type" || key == "Mime") &&
                    operand.starts_with("i/")) {
                    ignore_case = true;
                    operand.remove_prefix(2U);
                }
                const auto name = key == "Include" ? "Include/" + std::string(operand)
                    : key == "Default" ? std::string("Default") : std::string(text);
                sections.push_back({name, line_number, {}, false});
                current = sections.size() - 1U;
                if (key != "Include" && key != "Default") {
                    sections.back().values[std::string(key)] = {std::string(operand), line_number};
                    if (ignore_case) sections.back().values[std::string(key) + "IgnoreCase"] = {"true", line_number};
                }
                header = true;
                break;
            }
            if (header) continue;
        }
        const auto equal = text.find('=');
        if (equal == std::string_view::npos) extension_error(source, line_number, "expected key=value or an extension rule");
        const auto key = std::string(trim(text.substr(0, equal)));
        const auto value = trim(text.substr(equal + 1U));
        if (key.empty()) extension_error(source, line_number, "empty extension key");
        if (!ini && (!current || (line.front() != ' ' && line.front() != '\t'))) {
            if (current && (std::ranges::find(action_names, key) != action_names.end() || key == "Include")) {
                extension_error(source, line_number, "legacy extension actions must be indented");
            }
            sections.push_back({"legacy " + key, line_number, {}, true});
            sections.back().values["Pattern"] = {key, line_number};
            sections.back().values["Open"] = {std::string(value), line_number};
            if (value.empty()) extension_error(source, line_number, "legacy extension command is empty");
            current.reset();
        } else {
            if (!current) extension_error(source, line_number, "key has no extension section");
            sections[*current].values[key] = {ini ? ini_value(value, source, line_number) : std::string(value), line_number};
        }
        if (sections.size() > 4096U) extension_error(source, line_number, "too many extension sections");
    }
    if (sections.size() > 4096U) extension_error(source, line_number, "too many extension sections");
    return sections;
}

}  // namespace

struct ExtensionRules::Data {
    std::string source;
    std::vector<Rule> rules;
};

ExtensionRules ExtensionRules::parse(const std::string_view content, const std::string_view source) {
    if (content.size() > 1024U * 1024U) extension_error(source, 1U, "extension file exceeds 1 MiB");
    if (content.find('\0') != std::string_view::npos) extension_error(source, 1U, "NUL in extension file");
    auto first = content;
    while (!first.empty()) {
        const auto end = first.find('\n');
        const auto line = trim(first.substr(0, end));
        if (!line.empty() && !line.starts_with('#')) break;
        if (end == std::string_view::npos) { first = {}; break; }
        first.remove_prefix(end + 1U);
    }
    const bool ini = trim(first).starts_with('[') || source.ends_with(".ini");
    const auto sections = parse_sections(content, source, ini);
    if (ini) {
        const auto metadata = std::ranges::find(sections, "mc.ext.ini", &Section::name);
        if (metadata == sections.end()) extension_error(source, 1U, "missing [mc.ext.ini] metadata section");
        const auto version = metadata->values.find("Version");
        if (version == metadata->values.end() || version->second.text != "4.0") {
            extension_error(source, metadata->line, "[mc.ext.ini] needs Version=4.0");
        }
        if (metadata->values.size() != 1U) extension_error(source, metadata->line, "unsupported metadata key");
    }
    auto data = std::make_shared<Data>();
    data->source = source;
    for (const auto& section : sections) {
        if (ini && section.name == "mc.ext.ini") continue;
        data->rules.push_back(compile_section(section, source));
    }
    const auto resolve = [&](const auto& self, const Rule& rule, std::vector<std::string> chain) -> void {
        if (!rule.include) return;
        if (chain.size() >= 32U || std::ranges::find(chain, *rule.include) != chain.end()) {
            extension_error(source, rule.line, "cyclic or excessively deep extension Include");
        }
        const auto target = std::ranges::find(data->rules, *rule.include, &Rule::name);
        if (target == data->rules.end() || !target->include_only) {
            extension_error(source, rule.line, "missing [" + *rule.include + "] section");
        }
        chain.push_back(*rule.include);
        self(self, *target, std::move(chain));
    };
    for (const auto& rule : data->rules) resolve(resolve, rule, {rule.name});
    ExtensionRules result;
    result.data_ = std::move(data);
    return result;
}

ExtensionRules ExtensionRules::load(const std::filesystem::path& path) {
    int descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (descriptor < 0) {
        if (errno == ENOENT) return {};
        throw std::system_error(errno, std::generic_category(), "open extension file: " + path.string());
    }
    std::string content;
    try {
        struct stat metadata {};
        if (fstat(descriptor, &metadata) != 0) {
            throw std::system_error(errno, std::generic_category(), "inspect extension file: " + path.string());
        }
        if (!S_ISREG(metadata.st_mode) || (metadata.st_uid != geteuid() && metadata.st_uid != 0U) ||
            (metadata.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
            throw std::runtime_error("refusing insecure extension file: " + path.string());
        }
        std::array<char, 8192> buffer{};
        for (;;) {
            const auto size = read(descriptor, buffer.data(), buffer.size());
            if (size < 0 && errno == EINTR) continue;
            if (size < 0) throw std::system_error(errno, std::generic_category(), "read extension file: " + path.string());
            if (size == 0) break;
            if (content.size() + static_cast<std::size_t>(size) > 1024U * 1024U) {
                throw std::runtime_error("extension file exceeds 1 MiB: " + path.string());
            }
            content.append(buffer.data(), static_cast<std::size_t>(size));
        }
        const auto result = close(descriptor);
        descriptor = -1;
        if (result != 0) throw std::system_error(errno, std::generic_category(), "close extension file");
    } catch (...) {
        if (descriptor >= 0) close(descriptor);
        throw;
    }
    return parse(content, path.string());
}

std::optional<ExtensionCommand> ExtensionRules::match(
    const Entry& entry, const ExtensionAction action, const bool local,
    const std::function<std::string(ExtensionType)>& probe
) const {
    if (!data_) return std::nullopt;
    std::array<std::optional<std::string>, 2> types;
    const auto basename = std::filesystem::path(entry.name).filename().string();
    const bool directory = entry.kind == EntryKind::directory ||
                           (entry.kind == EntryKind::symlink && entry.target_is_directory);
    const auto type_matches = [&](const Expression& expression, const ExtensionType kind) {
        if (!local) throw std::runtime_error("copy archive members to a local directory before extension Type/Mime matching");
        if (!probe) throw std::runtime_error("extension Type/Mime matching requires the optional 'file' tool");
        const auto index = static_cast<std::size_t>(kind);
        if (!types[index]) types[index] = probe(kind);
        return expression.matches(*types[index]);
    };
    const auto matches = [&](const Rule& rule) {
        if (rule.directory) return directory && rule.directory->matches(basename);
        if (directory) return false;
        if (rule.pattern && !EditorSearch::filename_matches(entry.name, *rule.pattern, true)) return false;
        if (rule.regex) {
            if (!rule.regex->matches(basename)) return false;
        } else if (rule.shell && !shell_matches(basename, *rule.shell, rule.shell_ignore_case)) return false;
        if (rule.type && !type_matches(*rule.type, ExtensionType::description)) return false;
        return !rule.mime || type_matches(*rule.mime, ExtensionType::mime);
    };
    const auto command = [&](const Rule& first) {
        const Rule* rule = &first;
        while (rule->include) rule = &*std::ranges::find(data_->rules, *rule->include, &Rule::name);
        return rule->actions[static_cast<std::size_t>(action)];
    };
    for (const bool fallback : {false, true}) {
        for (const auto& rule : data_->rules) {
            if (rule.include_only || rule.fallback != fallback) continue;
            const auto selected = command(rule);
            if (selected && matches(rule)) return selected;
        }
    }
    return std::nullopt;
}

std::filesystem::path find_extension_file(const std::filesystem::path& config_directory) {
    for (const auto name : {"mc.ext.ini", "mc.ext", "extensions"}) {
        const auto path = config_directory / name;
        struct stat metadata {};
        if (lstat(path.c_str(), &metadata) == 0) return path;
        if (errno != ENOENT) {
            throw std::system_error(errno, std::generic_category(), "inspect extension resource: " + path.string());
        }
    }
    return config_directory / "mc.ext.ini";
}

std::string probe_extension_type(
    const std::filesystem::path& path, const ExtensionType type, const Cancellation& cancellation
) {
    auto arguments = std::vector<std::string>{"file", "--brief", "--dereference", "--error"};
    if (type == ExtensionType::mime) arguments.emplace_back("--mime-type");
    arguments.emplace_back("--");
    arguments.push_back(path.string());
    CommandOutput result;
    try {
        result = capture_command(arguments, path.parent_path(), cancellation, 64U * 1024U);
    } catch (const std::system_error& error) {
        if (error.code().value() == ENOENT) {
            throw std::runtime_error("extension Type/Mime matching requires the optional 'file' tool");
        }
        throw;
    }
    if (result.status != 0) {
        throw std::runtime_error("extension 'file' probe exited " + std::to_string(result.status) +
                                 (result.error.empty() ? "" : ": " + result.error));
    }
    while (!result.output.empty() && (result.output.back() == '\n' || result.output.back() == '\r')) {
        result.output.pop_back();
    }
    if (result.output.empty()) throw std::runtime_error("extension 'file' probe produced no type");
    return result.output;
}

}  // namespace mcpp
