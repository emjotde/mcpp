#include "mcpp/editor.hpp"
#include "mcpp/local_provider.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;

void check(const bool condition, const std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

class ReadReached final : public std::runtime_error {
public:
    ReadReached() : std::runtime_error("read reached") {}
};

class ProbeProvider final : public mcpp::Provider {
public:
    ProbeProvider() {
        file.name = "fixture";
        file.path = "/fixture";
        file.kind = mcpp::EntryKind::file;
        file.size = bytes.size();
        file.revision = "original";
    }

    std::string bytes{"caf\xe9\nsecond\n"};
    mutable mcpp::Entry file;
    mutable std::size_t reads{0};
    bool trap_read{false};
    bool cancel_during_read{false};
    bool change_during_read{false};

    [[nodiscard]] std::string id() const override { return "large-file-probe"; }
    [[nodiscard]] std::string display_name() const override { return id(); }
    [[nodiscard]] mcpp::ProviderCapability capabilities() const override {
        return mcpp::ProviderCapability::read;
    }
    [[nodiscard]] std::string normalize(const std::string_view path) const override {
        return std::string(path);
    }
    [[nodiscard]] std::string join(const std::string_view directory, const std::string_view name) const override {
        return std::string(directory) + "/" + std::string(name);
    }
    [[nodiscard]] std::string parent(std::string_view) const override { return "/"; }
    [[nodiscard]] std::string basename(std::string_view) const override { return "fixture"; }
    [[nodiscard]] std::vector<mcpp::Entry> list(
        std::string_view, const mcpp::ListRequest&, const mcpp::Cancellation&
    ) const override { return {file}; }
    [[nodiscard]] mcpp::Entry stat(
        std::string_view, const mcpp::Cancellation& cancellation
    ) const override {
        cancellation.throw_if_requested();
        return file;
    }
    [[nodiscard]] mcpp::Entry stat_follow(
        const std::string_view path, const mcpp::Cancellation& cancellation
    ) const override { return stat(path, cancellation); }
    [[nodiscard]] bool exists(std::string_view) const override { return true; }
    [[nodiscard]] mcpp::SpaceInfo space(std::string_view) const override { return {}; }
    void read(
        std::string_view, std::uint64_t, std::uint64_t, std::ostream& output,
        const mcpp::Cancellation& cancellation
    ) const override {
        ++reads;
        if (cancel_during_read) cancellation.request();
        cancellation.throw_if_requested();
        if (trap_read) throw ReadReached{};
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (change_during_read) file.revision = "changed during read";
    }
    void write(
        std::string_view, std::istream&, std::optional<std::uint64_t>, bool,
        std::optional<std::string>, const mcpp::Cancellation&, const mcpp::ProgressCallback&
    ) override { throw std::runtime_error("probe is read-only"); }
    void create_directory(std::string_view) override { throw std::runtime_error("probe is read-only"); }
    void remove(
        std::string_view, bool, const mcpp::Cancellation&, const mcpp::ProgressCallback&
    ) override { throw std::runtime_error("probe is read-only"); }
    void rename(std::string_view, std::string_view, bool) override { throw std::runtime_error("probe is read-only"); }
    bool copy_direct(
        std::string_view, std::string_view, bool, const mcpp::Cancellation&, const mcpp::ProgressCallback&
    ) override { return false; }
    void set_permissions(std::string_view, fs::perms) override { throw std::runtime_error("probe is read-only"); }
    void set_owner(
        std::string_view, std::optional<std::string>, std::optional<std::string>
    ) override { throw std::runtime_error("probe is read-only"); }
    void create_symlink(
        std::string_view, std::string_view, bool, const mcpp::Cancellation&
    ) override { throw std::runtime_error("probe is read-only"); }
    void create_hardlink(std::string_view, std::string_view) override { throw std::runtime_error("probe is read-only"); }
};

void thresholds_and_retention() {
    const auto provider = std::make_shared<ProbeProvider>();
    mcpp::EditorModel editor;
    editor.open(provider, "/fixture");
    editor.set_encoding("ISO-8859-1");
    editor.set_position({1, 2});
    editor.toggle_bookmark();
    editor.insert("draft");
    editor.toggle_mark();
    editor.move(0, 2);
    editor.toggle_mark();
    const auto original = editor.text();
    const auto position = mcpp::EditorPosition{editor.row(), editor.column()};
    const auto selection = editor.selection();
    const auto change = editor.change_id();
    const auto retained = [&] {
        check(editor.provider() == provider && editor.path() == "/fixture" &&
                  editor.text() == original && editor.encoding() == "ISO-8859-1" &&
                  editor.dirty() && editor.change_id() == change &&
                  mcpp::EditorPosition{editor.row(), editor.column()} == position &&
                  editor.selection() == selection && editor.bookmarks().contains(1),
              "unsuccessful opens retain provider, encoding, edits, cursor, selection, bookmarks and undo state");
    };
    provider->trap_read = true;
    for (const auto size : {16ULL * 1024ULL * 1024ULL + 1ULL,
                            static_cast<unsigned long long>(mcpp::EditorModel::large_file_threshold)}) {
        provider->file.size = size;
        bool reached = false;
        try { editor.open(provider, "/fixture"); }
        catch (const ReadReached&) { reached = true; }
        check(reached, "files above the old cap and at exactly 64 MiB do not require confirmation");
        retained();
    }
    std::optional<mcpp::Entry> approval;
    for (const auto size : {static_cast<unsigned long long>(mcpp::EditorModel::large_file_threshold) + 1ULL,
                            1024ULL * 1024ULL * 1024ULL * 1024ULL}) {
        provider->file.size = size;
        provider->reads = 0;
        try { editor.open(provider, "/fixture"); }
        catch (const mcpp::EditorLargeFileConfirmationRequired& required) { approval = required.file(); }
        check(approval && approval->size == size && provider->reads == 0U,
              "large-file confirmation precedes every provider read, without another arbitrary cap");
        retained();
    }
    provider->reads = 0;
    bool reached = false;
    try { editor.open(provider, "/fixture", {}, approval); }
    catch (const ReadReached&) { reached = true; }
    check(reached && provider->reads == 1U, "explicit approval permits the read");
    retained();

    provider->file.revision = "changed while awaiting confirmation";
    provider->reads = 0;
    bool reconfirmed = false;
    try { editor.open(provider, "/fixture", {}, approval); }
    catch (const mcpp::EditorLargeFileConfirmationRequired&) { reconfirmed = true; }
    check(reconfirmed && provider->reads == 0U, "approval cannot silently authorize a different revision");
    retained();

    mcpp::Cancellation cancelled;
    cancelled.request();
    bool stopped = false;
    try { editor.open(provider, "/fixture", cancelled, provider->file); }
    catch (const mcpp::OperationCancelled&) { stopped = true; }
    check(stopped && provider->reads == 0U, "pre-cancelled open does not read");
    retained();
    provider->cancel_during_read = true;
    stopped = false;
    try { editor.open(provider, "/fixture", {}, provider->file); }
    catch (const mcpp::OperationCancelled&) { stopped = true; }
    check(stopped, "approved large-file read remains cancellable");
    retained();

    provider->cancel_during_read = false;
    provider->trap_read = false;
    provider->change_during_read = true;
    provider->file.size = provider->bytes.size();
    stopped = false;
    try { editor.open(provider, "/fixture"); }
    catch (const std::runtime_error& error) {
        stopped = std::string_view(error.what()).find("changed") != std::string_view::npos;
    }
    check(stopped, "same-sized revision changes during reads are rejected");
    retained();
    check(editor.undo(), "a declined or failed open retains the old undo history");
    std::cout << "PASS thresholds, pre-read confirmation, cancellation and buffer retention\n";
}

void write_large(const fs::path& path) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    const auto line = std::string("caf\xe9 ") + std::string(1017, 'x') + "\r\n";
    check(line.size() == 1024U, "large fixture line size");
    std::string block;
    for (std::size_t count = 0; count < 1024U; ++count) block += line;
    for (std::size_t count = 0; count < 64U; ++count) {
        output.write(block.data(), static_cast<std::streamsize>(block.size()));
    }
    output << line;
    check(static_cast<bool>(output), "write large fixture");
}

struct Fingerprint {
    std::uint64_t size{0};
    std::uint64_t hash{14695981039346656037ULL};
    auto operator<=>(const Fingerprint&) const = default;
};

Fingerprint fingerprint(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    check(static_cast<bool>(input), "open fingerprint fixture");
    std::array<char, 65536> bytes{};
    Fingerprint result;
    while (input.read(bytes.data(), static_cast<std::streamsize>(bytes.size())) || input.gcount() != 0) {
        const auto count = static_cast<std::size_t>(input.gcount());
        result.size += count;
        for (std::size_t index = 0; index < count; ++index) {
            result.hash ^= static_cast<unsigned char>(bytes[index]);
            result.hash *= 1099511628211ULL;
        }
    }
    check(input.eof(), "fingerprint read completed");
    return result;
}

void check_prefixed_copy(const fs::path& path, const std::string_view prefix, const fs::path& original) {
    std::ifstream actual(path, std::ios::binary), expected(original, std::ios::binary);
    std::string head(prefix.size(), '\0');
    actual.read(head.data(), static_cast<std::streamsize>(head.size()));
    check(head == prefix, "large save inserts the expected prefix");
    std::array<char, 65536> left{}, right{};
    do {
        actual.read(left.data(), static_cast<std::streamsize>(left.size()));
        expected.read(right.data(), static_cast<std::streamsize>(right.size()));
        check(actual.gcount() == expected.gcount() &&
                  std::equal(left.begin(), left.begin() + actual.gcount(), right.begin()),
              "large encoding-aware save preserves every remaining byte");
    } while (actual.gcount() != 0);
    check(actual.eof() && expected.eof(), "large saved bytes were fully compared");
}

void large_saves_and_backups(const fs::path& root) {
    const auto path = root / "large-latin1-crlf.txt";
    const auto backup = root / "large-latin1-crlf.txt~";
    write_large(path);
    const auto original = fingerprint(path);
    check(original.size > mcpp::EditorModel::large_file_threshold, "real fixture exceeds the confirmation threshold");
    const auto provider = std::make_shared<mcpp::LocalProvider>(root);
    mcpp::EditorModel editor;
    std::optional<mcpp::Entry> approval;
    try { editor.open(provider, path.string()); }
    catch (const mcpp::EditorLargeFileConfirmationRequired& required) { approval = required.file(); }
    check(approval.has_value() && !editor.open(), "a new large editor stays unopened until approved");
    editor.open(provider, path.string(), {}, approval);
    editor.set_encoding("ISO-8859-1");
    check(editor.lines().front().starts_with("café ") && editor.storage_byte_size() == original.size,
          "large-file approval preserves codepage conversion and storage byte counts");
    editor.set_save_mode(mcpp::EditorSaveMode::backup);
    editor.insert("edited ");
    editor.save(false);
    check(!editor.dirty() && editor.encoding() == "ISO-8859-1", "large backup save completes cleanly");
    check(fingerprint(backup) == original, "streaming backup preserves all original bytes above 64 MiB");
    check_prefixed_copy(path, "edited ", backup);

    editor.insert("more ");
    {
        std::fstream external(path, std::ios::in | std::ios::out | std::ios::binary);
        external.put('!');
        check(static_cast<bool>(external), "write external revision change");
    }
    const auto changed = fingerprint(path);
    bool rejected = false;
    try { editor.save(false); }
    catch (const std::runtime_error&) { rejected = true; }
    check(rejected && editor.dirty() && fingerprint(path) == changed && fingerprint(backup) == original,
          "large backup saves retain revision guards without overwriting the file or prior backup");
    editor.close();

    editor.open(provider, path.string(), {}, provider->stat_follow(path.string(), {}));
    editor.insert("cancelled ");
    editor.set_save_mode(mcpp::EditorSaveMode::backup);
    mcpp::Cancellation cancellation;
    cancellation.request();
    rejected = false;
    try { editor.save(false, cancellation); }
    catch (const mcpp::OperationCancelled&) { rejected = true; }
    check(rejected && editor.dirty() && fingerprint(path) == changed && fingerprint(backup) == original,
          "cancelled large backup saves retain draft, original and previous backup");
    editor.close();

    const auto alias = root / "large-latin1-crlf.txt.alias";
    if (fs::exists(alias)) fs::remove(alias);
    fs::create_hard_link(path, alias);
    editor.open(provider, path.string(), {}, provider->stat_follow(path.string(), {}));
    editor.set_save_mode(mcpp::EditorSaveMode::backup);
    editor.set_backup_extension(".alias");
    editor.insert("must not save ");
    rejected = false;
    try { editor.save(false); }
    catch (const std::runtime_error&) { rejected = true; }
    check(rejected && editor.dirty() && fingerprint(path) == changed && fingerprint(alias) == changed,
          "large backup targets that alias the source remain rejected");
    editor.close();
    fs::remove(alias);

    editor.create(provider, "");
    editor.insert("replacement\n");
    editor.set_save_mode(mcpp::EditorSaveMode::backup);
    editor.set_backup_extension("~");
    editor.save_as(path.string(), true);
    check(!editor.dirty() && fingerprint(backup) == changed && fs::file_size(path) == 12U,
          "Save As streams a backup of a large destination even from a small unnamed buffer");
    std::ifstream replacement(path, std::ios::binary);
    check(std::string(std::istreambuf_iterator<char>(replacement), {}) == "replacement\n",
          "Save As publishes the exact replacement");
    std::cout << "PASS real large open, encoding, atomic saves, streaming backups, revision and alias guards\n";
}

}  // namespace

int main(const int argc, char** argv) {
    try {
        check(argc == 2, "usage: mcpp_editor_large_files /review/cases/editor-large-files/model");
        check(geteuid() == 1001 && fs::exists("/.dockerenv") &&
                  fs::exists("/review/.operation-regressions-disposable"),
              "large-file regressions require uid 1001 in the owned disposable container");
        const auto requested = fs::absolute(argv[1]).lexically_normal();
        check(requested.string().starts_with("/review/cases/editor-large-files/"),
              "large-file fixtures must stay in the owned case subtree");
        fs::create_directories(requested);
        const auto root = fs::canonical(requested);
        check(root.string().starts_with("/review/cases/editor-large-files/"), "fixture root cannot escape through a symlink");
        thresholds_and_retention();
        large_saves_and_backups(root);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "editor large-file regression failed: " << error.what() << '\n';
        return 1;
    }
}
