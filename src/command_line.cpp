#include "mcpp/command_line.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <ostream>
#include <stdexcept>
#include <utility>

namespace mcpp {

std::size_t parse_line_number(const std::string_view value) {
    if (value.empty()) throw std::invalid_argument("line numbers must be nonnegative integers");
    std::size_t result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
        throw std::invalid_argument("line numbers must be nonnegative integers");
    }
    return std::max(std::size_t{1}, result);
}

EditorFile parse_editor_file(std::string path) {
    if (path.empty()) throw std::invalid_argument("file name is empty");
    const auto end = path.size() - (path.ends_with(':') ? 1U : 0U);
    const auto separator = path.rfind(':', end == 0U ? 0U : end - 1U);
    if (separator == std::string::npos || separator == 0U || separator + 1U >= end) {
        return {std::move(path), 0};
    }
    const auto number = std::string_view(path).substr(separator + 1U, end - separator - 1U);
    if (!std::ranges::all_of(number, [](const char value) { return value >= '0' && value <= '9'; }) ||
        std::filesystem::exists(std::filesystem::symlink_status(path))) {
        return {std::move(path), 0};
    }
    const auto line = parse_line_number(number);
    path.resize(separator);
    return {std::move(path), line};
}

namespace {

[[noreturn]] void unsupported_terminal_option(const std::string_view option) {
    std::string_view reason;
    if (option == "-x" || option == "--xterm") {
        reason = "FTXUI does not expose an xterm capability override";
    } else if (option == "-X" || option == "--no-x11") {
        reason = "mcpp has no X11 integration";
    } else if (option == "-g" || option == "--oldmouse") {
        reason = "legacy mouse tracking is not implemented";
    } else if (option == "-t" || option == "--termcap") {
        reason = "the terminal backend does not use termcap";
    } else if (option == "-s" || option == "--slow") {
        reason = "a slow-terminal rendering mode is not implemented";
    } else {
        reason = "HP soft-key programming is not implemented";
    }
    throw std::invalid_argument(std::string(option) + " is not supported: " + std::string(reason));
}

bool blocked_terminal_option(const std::string_view option) {
    static constexpr std::array names{
        "-x", "--xterm", "-X", "--no-x11", "-g", "--oldmouse",
        "-t", "--termcap", "-s", "--slow", "-k", "--resetsoft"
    };
    return std::ranges::find(names, option) != names.end();
}

std::filesystem::path user_directory(const char* variable, const std::string_view fallback) {
    if (const auto* value = std::getenv(variable); value && *value) return value;
    if (const auto* home = std::getenv("HOME"); home && *home) {
        return std::filesystem::path(home) / fallback;
    }
    return {};
}

void print_terminal_options(std::ostream& output) {
    output << "\nTerminal controls:\n"
           << "  -d, --nomouse         Leave mouse reporting disabled\n"
           << "  -a, --stickchars      Draw frames with ASCII characters\n"
           << "  -K, --keymap FILE     Read MC-format key bindings\n"
           << "      --nokeymap        Use only the built-in bindings\n"
           << "ASCII frames do not enable a non-UTF-8 or 7-bit terminal mode.\n"
           << "Unavailable backend controls (reported as errors):\n"
           << "  -x/--xterm, -X/--no-x11, -g/--oldmouse, -t/--termcap,\n"
           << "  -s/--slow, -k/--resetsoft\n";
}

void print_color_options(std::ostream& output) {
    output << "\nLaunch colors:\n"
           << "  -b, --nocolor         Use terminal defaults and text emphasis\n"
           << "  -c, --color           Enable at least the basic color palette\n"
           << "  -C, --colors SPEC     Override color roles for this run\n"
           << "  -S, --skin NAME       Use a built-in skin or a skin file\n"
           << "SPEC contains role=foreground,background,attributes entries joined by ':'.\n"
           << "An omitted field keeps its current value; join attributes with '+'.\n"
           << "Colors: black, red, green, brown, blue, magenta, cyan, lightgray,\n"
           << "gray, brightred, brightgreen, yellow, brightblue, brightmagenta,\n"
           << "brightcyan, white, default, color0..color255, rgb000..rgb555, gray0..gray23.\n"
           << "Attributes: bold, italic, underline, reverse, blink, none.\n";
    output << cli_color_roles();
}

}  // namespace

std::string_view cli_program_name(const CliFrontend frontend) {
    switch (frontend) {
        case CliFrontend::manager: return "mcpp";
        case CliFrontend::editor: return "mcppedit";
        case CliFrontend::viewer: return "mcppview";
        case CliFrontend::diff: return "mcppdiff";
    }
    throw std::invalid_argument("invalid command-line frontend");
}

std::string_view cli_color_roles() {
    return "Color pairs: normal, selected, markselect, reverse, gauge, errors,\n"
           "dnormal, dfocus, errdhotfocus, menunormal, menusel, pmenusel,\n"
           "bbarhotkey, bbarbutton, editnormal, editbold, editmarked, viewnormal,\n"
           "viewselected, helpnormal, helplink, helpslink.\n"
           "Foreground-only roles: marked, disabled, header, menuhot, menuinactive, editwhitespace.\n"
           "Other MC roles are not rendered independently and are rejected.\n";
}

CliOptions parse_cli(
    const std::span<const std::string_view> arguments, const CliFrontend frontend
) {
    CliOptions result;
    result.frontend = frontend;
    result.edit = frontend == CliFrontend::editor;
    bool options = true;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const auto argument = arguments[index];
        const auto nonempty = [](const std::string_view value, const std::string_view name) {
            if (value.empty()) throw std::invalid_argument(std::string(name) + " is empty");
            return std::string(value);
        };
        const auto value = [&](const std::string_view option, const std::optional<std::string_view> attached) {
            if (attached) return *attached;
            if (++index >= arguments.size()) {
                throw std::invalid_argument(std::string(option) + " requires a value");
            }
            return arguments[index];
        };
        const auto edit = [&](const std::optional<std::string_view> attached) {
            if (frontend != CliFrontend::manager && frontend != CliFrontend::editor) {
                throw std::invalid_argument("--edit is available in mcpp and mcppedit");
            }
            result.edit = true;
            if (attached) result.paths.push_back(nonempty(*attached, "file name"));
        };
        const auto view = [&](const std::string_view path) {
            if (frontend != CliFrontend::manager) {
                throw std::invalid_argument("--view is available in mcpp; pass a file directly to mcppview");
            }
            result.view_file = nonempty(path, "file name");
        };
        if (!options) {
            result.paths.push_back(nonempty(argument, "file name"));
            continue;
        }
        if (argument == "--") {
            options = false;
            continue;
        }
        if (argument.starts_with("--")) {
            const auto equals = argument.find('=');
            const auto name = argument.substr(0, equals);
            const auto attached = equals == std::string_view::npos
                ? std::optional<std::string_view>{} : argument.substr(equals + 1U);
            const auto no_value = [&] {
                if (attached) throw std::invalid_argument(std::string(name) + " does not take a value");
            };
            if (name == "--help" || name == "--help-all" ||
                name == "--help-terminal" || name == "--help-color" ||
                name == "--version" || name == "--datadir" ||
                name == "--datadir-info" || name == "--configure-options") {
                no_value();
                if (name == "--help") result.action = CliAction::help;
                else if (name == "--help-all") result.action = CliAction::help_all;
                else if (name == "--help-terminal") result.action = CliAction::help_terminal;
                else if (name == "--help-color") result.action = CliAction::help_color;
                else if (name == "--version") result.action = CliAction::version;
                else if (name == "--datadir") result.action = CliAction::data_directory;
                else if (name == "--datadir-info") result.action = CliAction::data_directory_info;
                else result.action = CliAction::configure_options;
                return result;
            }
            if (name == "--skin") result.skin = nonempty(value(name, attached), "skin name");
            else if (name == "--keymap") {
                result.keymap = nonempty(value(name, attached), "keymap file name");
                result.keymap_disabled = false;
            } else if (name == "--colors") {
                result.colors.push_back(nonempty(value(name, attached), "color specification"));
            } else if (name == "--printwd") {
                result.print_directory = nonempty(value(name, attached), "printwd file name");
            } else if (name == "--view") view(value(name, attached));
            else if (name == "--edit") edit(attached);
            else if (blocked_terminal_option(name)) unsupported_terminal_option(name);
            else if (name == "--ftplog") {
                throw std::invalid_argument("--ftplog is not supported: remote providers are not enabled");
            } else {
                no_value();
                if (name == "--nokeymap") result.keymap_disabled = true;
                else if (name == "--subshell") result.subshell = true;
                else if (name == "--nosubshell") result.subshell = false;
                else if (name == "--nomouse") result.mouse = false;
                else if (name == "--stickchars") result.stickchars = true;
                else if (name == "--nocolor") result.color = false;
                else if (name == "--color") result.color = true;
                else throw std::invalid_argument("unknown option: " + std::string(argument));
            }
        } else if (argument.size() > 1U && argument.front() == '-') {
            for (std::size_t offset = 1; offset < argument.size(); ++offset) {
                const auto option = argument.substr(offset, 1U);
                const auto short_name = "-" + std::string(option);
                const auto short_value = [&] {
                    const auto attached = offset + 1U < argument.size()
                        ? std::optional(argument.substr(offset + 1U)) : std::nullopt;
                    offset = argument.size() - 1U;
                    return value(short_name, attached);
                };
                switch (argument[offset]) {
                    case 'h': result.action = CliAction::help; return result;
                    case 'V': result.action = CliAction::version; return result;
                    case 'f': result.action = CliAction::data_directory; return result;
                    case 'F': result.action = CliAction::data_directory_info; return result;
                    case 'S': result.skin = nonempty(short_value(), "skin name"); break;
                    case 'K':
                        result.keymap = nonempty(short_value(), "keymap file name");
                        result.keymap_disabled = false;
                        break;
                    case 'C': result.colors.push_back(nonempty(short_value(), "color specification")); break;
                    case 'P': result.print_directory = nonempty(short_value(), "printwd file name"); break;
                    case 'v': view(short_value()); break;
                    case 'e': edit({}); break;
                    case 'U': result.subshell = true; break;
                    case 'u': result.subshell = false; break;
                    case 'd': result.mouse = false; break;
                    case 'a': result.stickchars = true; break;
                    case 'b': result.color = false; break;
                    case 'c': result.color = true; break;
                    case 'x': case 'X': case 'g': case 't': case 's': case 'k':
                        unsupported_terminal_option(short_name);
                    case 'l':
                        throw std::invalid_argument("-l is not supported: remote providers are not enabled");
                    default: throw std::invalid_argument("unknown option: " + short_name);
                }
            }
        } else if (result.edit && argument.starts_with('+')) {
            result.first_line = parse_line_number(argument.substr(1));
        } else {
            result.paths.push_back(nonempty(argument, "file name"));
        }
    }
    if (result.view_file && result.edit) {
        throw std::invalid_argument("--view and --edit cannot be combined");
    }
    if (frontend == CliFrontend::manager && !result.edit && result.paths.size() > 2U) {
        throw std::invalid_argument("expected at most two pane paths");
    }
    if (frontend == CliFrontend::viewer && result.paths.size() != 1U) {
        throw std::invalid_argument("expected one file; use --help for usage");
    }
    if (frontend == CliFrontend::diff && result.paths.size() != 2U) {
        throw std::invalid_argument("expected two files; use --help for usage");
    }
    return result;
}

void print_cli_action(const CliOptions& options, std::ostream& output) {
    if (options.action == CliAction::run) return;
    const auto program = cli_program_name(options.frontend);
    if (options.action == CliAction::version) {
        output << program << ' ' << MCPP_VERSION << '\n';
        return;
    }
    if (options.action == CliAction::data_directory) {
        output << "/usr/share/mcpp\n";
        return;
    }
    if (options.action == CliAction::data_directory_info) {
        const auto configuration = user_directory("XDG_CONFIG_HOME", ".config");
        const auto data = user_directory("XDG_DATA_HOME", ".local/share");
        const auto cache = user_directory("XDG_CACHE_HOME", ".cache");
        if (configuration.empty()) output << "Configuration file: .mcpp-config\n";
        else output << "Configuration directory: " << (configuration / "mcpp").string() << '\n'
                    << "Configuration files: ini, panels.ini; legacy fallback: config\n";
        output << "User data directory: " << (data.empty() ? "not set" : (data / "mcpp").string()) << '\n'
               << "Cache directory: " << (cache.empty() ? "configuration directory fallback" : (cache / "mcpp").string()) << '\n'
               << "Built-in help, skins and syntax rules are compiled into the executable.\n"
               << "System resource search: /etc/mcpp, /usr/share/mcpp, /etc/mc, /usr/share/mc\n";
        return;
    }
    if (options.action == CliAction::configure_options) {
#ifdef MCPP_BUILD_OPTIONS
        output << MCPP_BUILD_OPTIONS << '\n';
#else
        output << "C++20; terminal=FTXUI; threads=enabled; remote-providers=disabled\n";
#endif
        return;
    }
    output << program << ' ' << MCPP_VERSION << '\n';
    if (options.action == CliAction::help_terminal) {
        print_terminal_options(output);
        return;
    }
    if (options.action == CliAction::help_color) {
        print_color_options(output);
        return;
    }
    output << "Usage: " << program << " [options]";
    switch (options.frontend) {
        case CliFrontend::manager: output << " [left-directory] [right-directory]\n"; break;
        case CliFrontend::editor: output << " [+line] [file[:line[:]] ...]\n"; break;
        case CliFrontend::viewer: output << " file\n"; break;
        case CliFrontend::diff: output << " first-file second-file\n"; break;
    }
    output << "  -h, --help            Show command-line usage\n"
           << "      --help-all        Include terminal and color controls\n"
           << "      --help-terminal   Show terminal controls and limitations\n"
           << "      --help-color      Show launch color controls\n"
           << "  -V, --version         Print the version without opening the UI\n"
           << "  -f, --datadir         Print the primary system data search directory\n"
           << "  -F, --datadir-info    Describe configuration and resource directories\n"
           << "      --configure-options  Print build features\n"
           << "  -P, --printwd FILE    Write the final active directory to FILE\n"
           << "  -U, --subshell        Use persistent Bash during this run\n"
           << "  -u, --nosubshell      Do not use persistent Bash during this run\n";
    if (options.frontend == CliFrontend::manager) {
        output << "  -v, --view FILE       Open a file in the viewer\n"
               << "  -e, --edit[=FILE]     Edit remaining files, or an unnamed buffer\n";
    }
    if (options.frontend == CliFrontend::manager || options.frontend == CliFrontend::editor) {
        output << "Editor files accept file:line and file:line:; an existing literal name wins.\n"
               << "+line selects the first editor file's starting line.\n";
    }
    if (options.frontend == CliFrontend::viewer) {
        output << "Use /dev/stdin to view redirected input.\n";
    }
    output << "Use -- before names beginning with '-' or '+' to stop option parsing.\n"
           << "Launch overrides do not change saved preferences.\n";
    if (options.action == CliAction::help_all) {
        print_terminal_options(output);
        print_color_options(output);
    } else {
        output << "Use --help-all for skin, keymap, mouse, frame and color options.\n";
    }
}

}  // namespace mcpp
