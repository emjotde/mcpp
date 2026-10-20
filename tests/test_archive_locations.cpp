#include "mcpp/editor.hpp"
#include "mcpp/local_provider.hpp"
#include "mcpp/local_tools.hpp"
#include "mcpp/location.hpp"
#include "mcpp/pane.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace {
namespace fs = std::filesystem;

void check(const bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename Function>
void rejects(Function function, const std::string& message) {
    try {
        function();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error(message);
}

void put(const fs::path& path, const std::string& bytes) {
    std::ofstream output(path, std::ios::binary);
    output.exceptions(std::ios::badbit | std::ios::failbit);
    output << bytes;
}

std::string get(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    input.exceptions(std::ios::badbit);
    check(input.good(), "open test file");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::string read(const mcpp::ProviderPtr& provider, const std::string& path) {
    const auto entry = provider->stat_follow(path, {});
    std::ostringstream output;
    provider->read(path, 0, entry.size, output, {});
    return output.str();
}

void make_archive(const fs::path& root) {
    const auto result = mcpp::capture_command(
        {"tar", "--format=ustar", "-cf", (root / "sample.tar").string(),
         "-C", (root / "source").string(), "document"}, root
    );
    check(result.status == 0, "create standard tar fixture: " + result.error);
}

void run(const fs::path& root) {
    fs::create_directories(root / "source");
    const std::string original = "archive document\n";
    put(root / "source/document", original);
    make_archive(root);
    const auto archived_bytes = get(root / "sample.tar");
    const auto cache = root / "fresh-cache";
    check(::setenv("XDG_CACHE_HOME", cache.c_str(), 1) == 0, "set private test cache");
    auto local = std::make_shared<mcpp::LocalProvider>();
    mcpp::ArchiveCache archives(local);
    const auto uri = (root / "sample.tar").string() + "/utar://";
    auto location = mcpp::open_location(uri, local, &archives);
    auto again = mcpp::open_location(uri + "./document", local, &archives);
    check(location.provider == again.provider, "same archive revision shares its provider");
    check(again.path == "/document" && read(again.provider, again.path) == original,
          "native URI resolves normalized member paths");
    check(archives.format(again.provider, again.path) == uri + "document", "URI retains the archive namespace");
    fs::create_symlink(root / "sample.tar", root / "alias.tar");
    auto alias_location = mcpp::open_location((root / "alias.tar").string() + "/utar://", local, &archives);
    check(archives.format(alias_location.provider, "/") == (root / "alias.tar").string() + "/utar://",
          "opening an archive alias retains its own containing directory and name");
    check(fs::is_directory(cache / "mcpp"), "a fresh cache parent is created");
    check((fs::status(cache / "mcpp").permissions() & fs::perms::all) == fs::perms::owner_all,
          "a new cache parent is private");
    rejects([&] { static_cast<void>(mcpp::open_location(
        (root / "sample.tar").string() + "/ucpio://", local, &archives)); },
        "a mismatched native VFS format is rejected");
    rejects([&] { static_cast<void>(mcpp::open_location("sftp://host/path", local, &archives)); },
        "remote filesystem pause is retained");

    mcpp::Pane host(local, root.string());
    host.set_show_hidden(true);
    host.set_sort(mcpp::SortField::size, true);
    mcpp::Pane virtual_pane(location.provider, "/", &host, true);
    check(virtual_pane.show_hidden() && virtual_pane.sort_field() == mcpp::SortField::size &&
          virtual_pane.descending(), "archive panes inherit listing preferences");
    check(virtual_pane.entries().front().revision == "mcpp:parent", "archive root exposes its outer parent");
    host.swap_contents(virtual_pane);
    check(host.provider() == location.provider && virtual_pane.provider() == local,
          "pane-content swapping keeps paths and providers together");
    host.swap_contents(virtual_pane);

    mcpp::EditorModel editor;
    editor.open(location.provider, "/document");
    editor.insert("X");
    rejects([&] { editor.save(false); }, "normal save cannot change a readonly archive");
    rejects([&] { editor.save_as(local, (root / "source").string(), false,
                                 mcpp::EditorLineEnding::unchanged); },
            "failed cross-provider Save As retains its source");
    check(editor.provider() == location.provider && editor.path() == "/document" && editor.dirty(),
          "failed cross-provider saves retain identity and draft");
    const auto saved = root / "saved";
    editor.save_as(local, saved.string(), false, mcpp::EditorLineEnding::unchanged);
    check(editor.provider() == local && editor.path() == saved.string() && !editor.dirty() &&
          get(saved) == "X" + original, "Save As switches providers only after publishing exact bytes");
    check(editor.undo() && editor.text() == original, "cross-provider Save As retains undo history");

    const auto linked = root / "linked";
    put(linked, "old bytes\n");
    fs::create_hard_link(linked, root / "alias");
    mcpp::EditorModel preserve;
    preserve.open(location.provider, "/document");
    preserve.insert("Y");
    preserve.save_as_preserving_links(local, linked.string(), true, mcpp::EditorLineEnding::unchanged);
    check(get(linked) == "Y" + original && get(root / "alias") == "Y" + original &&
          fs::hard_link_count(linked) == 2, "cross-provider Save As preserves links only when explicitly requested");
    check(get(root / "sample.tar") == archived_bytes, "editing and Save As never rewrite the readonly source");

    put(root / "source/document", "new archive revision\n");
    make_archive(root);
    auto changed = mcpp::open_location(uri, local, &archives);
    check(changed.provider != location.provider && read(changed.provider, "/document") == "new archive revision\n",
          "external archive changes open a new cache revision");
    check(read(location.provider, "/document") == original, "in-use old snapshots remain readable");
    mcpp::ArchiveCache lifecycle(local, std::chrono::seconds(0));
    auto held = lifecycle.open((root / "sample.tar").string());
    lifecycle.prune();
    check(!lifecycle.locations().empty(), "active providers survive zero-timeout pruning");
    held.reset();
    lifecycle.prune();
    check(lifecycle.locations().empty(), "unused providers expire at the configured timeout");
    rejects([&] { lifecycle.set_timeout(std::chrono::seconds(-1)); }, "negative cache timeouts fail explicitly");
    put(root / "blocked-cache", "not a directory");
    check(::setenv("XDG_CACHE_HOME", (root / "blocked-cache").c_str(), 1) == 0, "set blocked cache fixture");
    mcpp::ArchiveCache blocked(local);
    rejects([&] { static_cast<void>(blocked.open((root / "sample.tar").string())); },
            "unusable cache paths fail explicitly");
}
}  // namespace

int main(const int argc, char** argv) {
    try {
        if (::geteuid() != 1001 || !fs::is_regular_file("/.dockerenv") ||
            !fs::is_regular_file("/review/.operation-regressions-disposable")) {
            throw std::runtime_error("archive location tests require the owned non-root disposable container");
        }
        if (argc != 2) throw std::runtime_error("expected archive location test directory");
        const auto base = fs::absolute(argv[1]).lexically_normal();
        if (base != "/review/cases/archive-locations") throw std::runtime_error("invalid fixture directory");
        fs::create_directories(base);
        if (fs::is_symlink(fs::symlink_status(base))) throw std::runtime_error("fixture directory is a symlink");
        const auto root = base / ("run-" + std::to_string(::getpid()));
        if (!fs::create_directory(root)) throw std::runtime_error("fixture directory already exists");
        run(root);
        fs::remove_all(root);
        std::cout << "Archive location, cache, pane and cross-provider Save As checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
