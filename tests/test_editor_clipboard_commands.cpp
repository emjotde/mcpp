#include "mcpp/config.hpp"
#include "mcpp/editor.hpp"
#include "mcpp/editor_clipboard.hpp"
#include "mcpp/local_provider.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace {

void check(const bool condition, const std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

void write_file(const std::filesystem::path& path, const std::string_view data) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(data.data(), static_cast<std::streamsize>(data.size()));
    check(static_cast<bool>(output), "write original clipboard fixture");
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    check(static_cast<bool>(input), "read clipboard fixture");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void script(const std::filesystem::path& path, const std::string_view body) {
    write_file(path, "#!/bin/sh\n" + std::string(body));
    check(chmod(path.c_str(), 0700) == 0, "protect original clipboard helper");
}

template <typename Action>
void fails(Action&& action, const std::string_view expected) {
    try {
        action();
    } catch (const std::exception& error) {
        check(std::string_view(error.what()).find(expected) != std::string_view::npos,
              "configured failure includes its actual cause");
        return;
    }
    throw std::runtime_error("configured clipboard failure was silently accepted");
}

void no_staged_files(const std::filesystem::path& directory) {
    for (const auto& item : std::filesystem::directory_iterator(directory)) {
        check(!item.path().filename().string().starts_with(".mcpp-clip-"),
              "private raw/metadata files are cleaned up");
    }
}

void config_bindings(const std::filesystem::path& root) {
    const auto native = root / "native" / "ini";
    const auto flat = root / "flat" / "config";
    const auto empty = root / "empty" / "ini";
    const auto absent = root / "absent" / "config";
    for (const auto& path : {native, flat, empty, absent}) {
        std::filesystem::create_directories(path.parent_path());
    }
    write_file(native, "# original clipboard fixture\n[Misc]\n"
               "clipboard_store=./collect\\s--label=ż\n"
               "clipboard_paste=./produce\\s--format='text\\\\bytes'\n");
    auto config = mcpp::Config::load(native);
    check(config.clipboard_store == "./collect --label=ż" &&
          config.clipboard_paste == "./produce --format='text\\bytes'" &&
          config.diagnostics.empty(), "native Misc command strings decode and are supported");
    config.save();
    const auto restored = mcpp::Config::load(native);
    check(restored.clipboard_store == config.clipboard_store &&
          restored.clipboard_paste == config.clipboard_paste &&
          read_file(native).find("# original clipboard fixture") != std::string::npos,
          "native command strings round-trip without losing retained comments");
    write_file(flat, "clipboard_store=./collect --path=C:\\\\clip\n"
               "clipboard_paste=./produce\\n#trustedline\n");
    config = mcpp::Config::load(flat);
    check(config.clipboard_store == "./collect --path=C:\\clip" &&
          config.clipboard_paste == "./produce\n#trustedline" && config.diagnostics.empty(),
          "flat command strings retain existing escape decoding");
    config.save();
    const auto flat_restored = mcpp::Config::load(flat);
    check(flat_restored.clipboard_store == config.clipboard_store &&
          flat_restored.clipboard_paste == config.clipboard_paste,
          "flat command strings round-trip");
    write_file(empty, "[Misc]\nclipboard_store=\nclipboard_paste=\n");
    config = mcpp::Config::load(empty);
    check(config.clipboard_store.has_value() && config.clipboard_store->empty() &&
          config.clipboard_paste.has_value() && config.clipboard_paste->empty(),
          "explicit empty native settings remain engaged optionals");
    config.save();
    const auto empty_restored = mcpp::Config::load(empty);
    check(empty_restored.clipboard_store == config.clipboard_store &&
          empty_restored.clipboard_paste == config.clipboard_paste,
          "explicit empty settings survive native serialization");
    write_file(absent, "theme=paper\n");
    config = mcpp::Config::load(absent);
    check(!config.clipboard_store && !config.clipboard_paste, "absent legacy settings stay absent");
    config.save();
    check(read_file(absent).find("clipboard_store") == std::string::npos &&
          read_file(absent).find("clipboard_paste") == std::string::npos,
          "saving legacy config does not invent clipboard commands");
    for (const bool native_format : {false, true}) {
        for (const std::string_view key : {"clipboard_store", "clipboard_paste"}) {
            const auto invalid = root / (native_format ? "invalid-native" : "invalid-flat") /
                                 std::string(key);
            std::filesystem::create_directories(invalid.parent_path());
            std::string content = native_format ? "[Misc]\n" : "";
            content += std::string(key) + "=./collect";
            content.push_back('\0');
            content += "ignored\n";
            write_file(invalid, content);
            fails([&] { config = mcpp::Config::load(invalid); }, "NUL");
            check(!config.clipboard_store && !config.clipboard_paste &&
                  read_file(invalid) == content,
                  "invalid NUL load changes neither retained Config nor source file");
        }
    }
}

void success(const std::filesystem::path& root) {
    config_bindings(root / "config-bindings");
    const auto tools = root / "tools";
    const auto cache = root / "cache ' $(touch filename-injected)" / "clipboard";
    std::filesystem::create_directories(tools);
    script(tools / "collect",
           "[ \"$#\" -eq 1 ] || exit 31\n"
           "[ ! -t 0 ] || exit 32\n"
           "[ \"$(wc -c)\" -eq 0 ] || exit 33\n"
           "[ \"$(stat -c %a -- \"$1\")\" = 600 ] || exit 34\n"
           "printf '%s' \"$1\" > raw-path\n"
           "pwd > cwd\n"
           "cat -- \"$1\" > received\n");
    script(tools / "produce",
           "[ \"$#\" -eq 0 ] || exit 35\n"
           "[ \"$(wc -c)\" -eq 0 ] || exit 36\n"
           "cat output\n");
    std::string payload{"mcpp-clipboard-v1\ncolumn\n"};
    payload += std::string("\0\xff\r\n", 4);
    payload += "UTF8-é-ż $(touch payload-injected) `touch backtick-injected`";
    mcpp::save_editor_clipboard(cache, payload, true, "./collect", tools);
    check(read_file(tools / "received") == payload, "native store receives exact raw bytes");
    check(read_file(tools / "cwd") == tools.string() + "\n", "captured editor directory is used");
    check(!std::filesystem::exists(read_file(tools / "raw-path")), "raw file removed after store");
    struct stat state {};
    check(stat(cache.c_str(), &state) == 0 && (state.st_mode & 0777) == 0600,
          "persistent clipboard remains private");
    bool column = false;
    check(mcpp::load_editor_clipboard(cache, &column) == payload && column,
          "store keeps column metadata out of the raw file");
    write_file(tools / "output", payload);
    check(mcpp::load_editor_clipboard(cache, &column, "./produce", tools) == payload && !column,
          "paste stdout is binary data, including header-like bytes");
    check(mcpp::load_editor_clipboard(cache, &column) == payload && !column,
          "external paste publishes linear metadata atomically");
    check(!std::filesystem::exists(tools / "payload-injected") &&
          !std::filesystem::exists(tools / "backtick-injected") &&
          !std::filesystem::exists(tools / "filename-injected"),
          "payload and raw filename never become shell source");

    const auto persisted = read_file(cache);
    write_file(tools / "output", "");
    check(mcpp::load_editor_clipboard(cache, &column, "./produce", tools).empty(),
          "empty configured stdout returns no data, not internal fallback");
    check(read_file(cache) == persisted, "empty stdout retains internal clipboard");
    for (const std::optional<std::string>& command :
         {std::optional<std::string>{}, std::optional<std::string>{""}}) {
        mcpp::save_editor_clipboard(cache, "left\nright", true, command, tools);
        check(mcpp::load_editor_clipboard(cache, &column, command, tools) == "left\nright" && column,
              "absent and explicit empty settings preserve legacy column clipboard");
    }

    mcpp::EditorModel editor;
    editor.create(std::make_shared<mcpp::LocalProvider>(root), "");
    editor.insert("draft-ż\n");
    editor.select_all();
    const auto original = editor.text();
    const auto selected = editor.selected_text();
    check(selected.has_value(), "draft selection exists");
    mcpp::save_editor_clipboard(cache, *selected, false, "./collect", tools);
    check(editor.delete_selection() && editor.text().empty(), "successful store permits cut");
    check(editor.undo() && editor.text() == original && editor.selected_text() == selected,
          "one undo restores the cut draft and selection");
    write_file(tools / "output", "paste-é $(touch paste-injected)");
    const auto pasted = mcpp::load_editor_clipboard(cache, &column, "./produce", tools);
    editor.clear_selection();
    editor.end();
    editor.paste(pasted);
    check(editor.text().find(pasted) != std::string::npos, "editor receives literal UTF8 paste");
    check(editor.undo() && editor.text() == original, "paste remains one undo step");
    check(!std::filesystem::exists(tools / "paste-injected"), "paste output is not executed");
    no_staged_files(cache.parent_path());
}

void failure(const std::filesystem::path& root) {
    const auto tools = root / "tools";
    const auto cache = root / "cache" / "clipboard";
    std::filesystem::create_directories(tools);
    script(tools / "fail-store", "printf 'collector denied' >&2\nexit 7\n");
    script(tools / "fail-paste", "printf 'not a successful paste'\nprintf 'producer denied' >&2\nexit 9\n");
    script(tools / "wait", "printf '%s' \"$$\" > wait-pid\nsleep 10\n");
    script(tools / "called", "printf called > called-marker\n");
    script(tools / "produce", "printf 'new bytes'\n");
    mcpp::save_editor_clipboard(cache, "previous clipboard", true);
    const auto persisted = read_file(cache);

    mcpp::EditorModel editor;
    editor.create(std::make_shared<mcpp::LocalProvider>(root), "");
    editor.insert("unsaved-ż\n");
    editor.select_all();
    const auto original = editor.text();
    const auto selection = editor.selected_text();
    const auto change = editor.change_id();
    const auto unchanged = [&] {
        check(editor.text() == original && editor.selected_text() == selection &&
              editor.change_id() == change && editor.dirty() && editor.path().empty(),
              "failed clipboard action preserves draft, selection and undo state");
        check(read_file(cache) == persisted, "failure never publishes a replacement clipboard");
        no_staged_files(cache.parent_path());
    };
    const auto cut = [&](const std::optional<std::string>& command,
                         const mcpp::Cancellation& cancellation = {}) {
        mcpp::save_editor_clipboard(cache, *selection, false, command, tools, cancellation);
        static_cast<void>(editor.delete_selection());
    };
    const auto paste = [&](const std::string& command,
                           const mcpp::Cancellation& cancellation = {}) {
        const auto value = mcpp::load_editor_clipboard(cache, nullptr, command, tools, cancellation);
        if (!value.empty()) editor.paste(value);
    };
    fails([&] { cut("./fail-store"); }, "status 7: collector denied");
    unchanged();
    fails([&] { cut("./missing-store"); }, "status 127");
    unchanged();
    fails([&] { paste("./fail-paste"); }, "status 9: producer denied");
    unchanged();
    fails([&] { paste("./missing-paste"); }, "status 127");
    unchanged();
    fails([&] { cut(std::string("./called\0ignored", 16)); }, "cannot contain NUL");
    unchanged();
    check(!std::filesystem::exists(tools / "called-marker"), "invalid command is rejected before execution");

    const auto blocked = root / "not-a-directory";
    write_file(blocked, "original obstacle");
    fails([&] {
        mcpp::save_editor_clipboard(blocked / "clipboard", *selection, false, "./called", tools);
        static_cast<void>(editor.delete_selection());
    }, "directory");
    check(!std::filesystem::exists(tools / "called-marker"), "store staging errors precede command execution");
    fails([&] {
        const auto value = mcpp::load_editor_clipboard(blocked / "clipboard", nullptr, "./produce", tools);
        editor.paste(value);
    }, "directory");
    unchanged();
    const auto destination_directory = root / "destination-directory";
    std::filesystem::create_directory(destination_directory);
    fails([&] {
        mcpp::save_editor_clipboard(destination_directory, *selection, false, "./called", tools);
        static_cast<void>(editor.delete_selection());
    }, "rename");
    unchanged();
    no_staged_files(root);

    mcpp::Cancellation cancelled;
    cancelled.request();
    fails([&] { cut("./called", cancelled); }, "operation cancelled");
    unchanged();
    const auto cancellation_start = std::chrono::steady_clock::now();
    mcpp::Cancellation running;
    std::jthread cancel([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        running.request();
    });
    fails([&] { paste("./wait", running); }, "operation cancelled");
    check(std::chrono::steady_clock::now() - cancellation_start < std::chrono::seconds(2),
          "running producer cancellation is bounded");
    unchanged();
    const auto timeout_start = std::chrono::steady_clock::now();
    fails([&] { cut("./wait"); }, "timed out after 2 seconds");
    check(std::chrono::steady_clock::now() - timeout_start < std::chrono::seconds(4),
          "UI cannot wait indefinitely for configured store");
    unchanged();

    const auto corrupt = root / "corrupt";
    write_file(corrupt, "mcpp-clipboard-v1\ninvalid\nbytes");
    fails([&] { static_cast<void>(mcpp::load_editor_clipboard(corrupt)); }, "invalid clipboard mode");
    check(editor.undo() && editor.text().empty(), "failures did not add or remove editor undo steps");
}

}  // namespace

int main(const int argc, char** const argv) {
    try {
        check(argc == 3, "usage: mcpp_editor_clipboard_commands ROOT success|failure");
        check(getuid() == 1001 && std::filesystem::is_regular_file("/review/.operation-regressions-disposable"),
              "clipboard commands run only in the UID1001 disposable container");
        const auto root = std::filesystem::absolute(argv[1]).lexically_normal();
        check(root.string().starts_with("/review/cases/native-clipboard-commands/"),
              "clipboard fixtures stay under the approved case root");
        std::filesystem::create_directories(root);
        const std::string_view mode(argv[2]);
        if (mode == "success") success(root);
        else if (mode == "failure") failure(root);
        else throw std::runtime_error("unknown focused clipboard selector");
        std::cout << "PASS clipboard " << mode << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
