#include "mcpp/archive_provider.hpp"
#include "mcpp/local_provider.hpp"
#include "mcpp/operations.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
using namespace std::string_literals;
using Time = std::chrono::system_clock::time_point;

namespace {

std::size_t checks = 0;
const std::string payload = "A\0B\nC\xff"s;
const std::string odd_name = "line\n tab\t '\"\\ --literal";
const Time precise_time{std::chrono::seconds(1700000000) + std::chrono::nanoseconds(123456789)};

void check(const bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
    ++checks;
}

template<class Function>
void rejects(Function&& function, const std::string& message) {
    bool rejected = false;
    try {
        function();
    } catch (const mcpp::OperationCancelled&) {
        throw;
    } catch (const std::exception&) {
        rejected = true;
    }
    check(rejected, message);
}

template<class Function>
void cancelled(Function&& function, const std::string& message) {
    bool rejected = false;
    try {
        function();
    } catch (const mcpp::OperationCancelled&) {
        rejected = true;
    }
    check(rejected, message);
}

void put(const fs::path& path, const std::string_view bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    check(static_cast<bool>(output), "write fixture: " + path.string());
}

std::string get(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("open fixture: " + path.string());
    std::string bytes{std::istreambuf_iterator<char>(input), {}};
    if (input.bad()) throw std::runtime_error("read fixture: " + path.string());
    return bytes;
}

std::string read(mcpp::Provider& provider, const std::string& path) {
    std::ostringstream output(std::ios::binary);
    provider.read(path, 0, std::numeric_limits<std::uint64_t>::max(), output, {});
    return std::move(output).str();
}

void write(mcpp::Provider& provider, const std::string& path, const std::string& bytes) {
    std::istringstream input(bytes, std::ios::binary);
    provider.write(path, input, bytes.size(), false, std::nullopt, {}, {});
}

mcpp::ArchiveOpenOptions options(const bool writable = false) {
    mcpp::ArchiveOpenOptions result;
    result.writable = writable;
    result.draft_directory = "drafts";
    return result;
}

void empty_archive(const fs::path& path, const mcpp::ArchiveFormat format) {
    if (format == mcpp::ArchiveFormat::tar) {
        put(path, std::string(1024, '\0'));
        return;
    }
    std::string bytes = format == mcpp::ArchiveFormat::cpio_crc ? "070702" : "070701";
    for (const auto value : std::array<unsigned int, 13>{0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 11, 0}) {
        std::ostringstream field;
        field << std::hex << value;
        bytes += std::string(8U - field.str().size(), '0') + field.str();
    }
    bytes += "TRAILER!!!"s + '\0';
    bytes.append((512U - bytes.size() % 512U) % 512U, '\0');
    put(path, bytes);
}

Time archive_time(const mcpp::ArchiveFormat format) {
    return format == mcpp::ArchiveFormat::tar ? precise_time :
        Time{std::chrono::floor<std::chrono::seconds>(precise_time.time_since_epoch())};
}

void archive_tree(const fs::path& path, const mcpp::ArchiveFormat format) {
    empty_archive(path, format);
    mcpp::ArchiveProvider provider(path, options(true));
    provider.create_directory("/tree");
    for (const auto* directory : {"d1", "d2", "empty"}) provider.create_directory("/tree/"s + directory);
    write(provider, "/tree/a-first", payload);
    for (const auto* name : {"/tree/d1/linked", "/tree/d2/linked", "/tree/z-last"}) {
        provider.create_hardlink("/tree/a-first", name);
    }
    write(provider, "/tree/middle", "independent");
    write(provider, "/tree/" + odd_name, payload);
    provider.create_symlink("a-first", "/tree/sym");
    const auto modified = archive_time(format);
    provider.set_permissions("/tree/a-first", fs::perms{0440});
    provider.set_modified("/tree/a-first", modified);
    provider.set_modified("/tree/sym", modified - std::chrono::seconds(1));
    for (const auto* directory : {"/tree/d1", "/tree/d2", "/tree/empty"}) {
        provider.set_modified(directory, modified - std::chrono::seconds(2));
    }
    provider.set_permissions("/tree", fs::perms{0750});
    provider.set_modified("/tree", modified - std::chrono::seconds(100));
}

void local_tree(mcpp::LocalProvider& provider, const fs::path& path) {
    fs::create_directories(path / "d1");
    fs::create_directory(path / "d2");
    fs::create_directory(path / "empty");
    put(path / "a-first", payload);
    for (const auto* name : {"d1/linked", "d2/linked", "z-last"}) {
        fs::create_hard_link(path / "a-first", path / name);
    }
    put(path / "middle", "independent");
    put(path / odd_name, payload);
    fs::create_symlink("a-first", path / "sym");
    fs::permissions(path / "a-first", fs::perms{0440});
    provider.set_modified((path / "a-first").string(), precise_time);
    provider.set_modified((path / "sym").string(), precise_time - std::chrono::seconds(1));
    for (const auto* directory : {"d1", "d2", "empty"}) {
        provider.set_modified((path / directory).string(), precise_time - std::chrono::seconds(2));
    }
    fs::permissions(path, fs::perms{0750});
    provider.set_modified(path.string(), precise_time - std::chrono::seconds(100));
}

void check_tree(
    mcpp::Provider& provider, const std::string& root,
    const Time modified, const bool preserve_permissions = true
) {
    const auto path = [&](const std::string_view name) { return provider.join(root, name); };
    const auto first = provider.stat(path("a-first"), {});
    for (const auto* name : {"a-first", "z-last", "d1/linked", "d2/linked"}) {
        const auto full = root + "/" + name;
        const auto member = provider.stat(full, {});
        check(read(provider, full) == payload, "copy retains exact hardlink bytes");
        check(member.inode == first.inode && member.links == 4U, "copy retains one four-name hardlink group");
        check(member.modified == modified, "hardlink modification time is preserved");
        check(member.permissions == (preserve_permissions ? fs::perms{0440} : fs::perms{0644}),
              "hardlink permissions follow copy options");
    }
    check(read(provider, path("middle")) == "independent" &&
          provider.stat(path("middle"), {}).inode != first.inode, "unrelated member is not linked");
    check(read(provider, path(odd_name)) == payload, "copy preserves literal Linux filenames");
    const auto link = provider.stat(path("sym"), {});
    check(link.kind == mcpp::EntryKind::symlink && link.target == "a-first" &&
          link.modified == modified - std::chrono::seconds(1), "symlink text and its own mtime are preserved");
    check(provider.stat(root, {}).modified == modified - std::chrono::seconds(100), "directory mtime is restored after children");
    check(provider.stat(root, {}).permissions == (preserve_permissions ? fs::perms{0750} : fs::perms{0755}),
          "directory permissions follow copy options");
    check(provider.stat(path("empty"), {}).kind == mcpp::EntryKind::directory &&
          provider.list(path("empty"), {}, {}).empty(), "empty directory is copied");
}

void copies(const fs::path& root, mcpp::LocalProvider& local) {
    mcpp::OperationEngine engine;
    local_tree(local, root / "local-tree");
    const std::array<mcpp::ArchiveFormat, 3> formats{
        mcpp::ArchiveFormat::tar, mcpp::ArchiveFormat::cpio_newc, mcpp::ArchiveFormat::cpio_crc
    };
    for (std::size_t index = 0; index < formats.size(); ++index) {
        const auto format = formats[index];
        const auto prefix = "format-" + std::to_string(index);
        const auto source_path = root / (prefix + "-source.archive");
        archive_tree(source_path, format);
        mcpp::ArchiveProvider source(source_path, options());
        const auto export_path = root / (prefix + "-export");
        engine.copy_exact(source, source.stat("/tree", {}), local, export_path.string(), {}, {});
        check_tree(local, export_path.string(), archive_time(format));
        check(source.stat("/tree/a-first", {}).links == 4U, "archive source links remain unchanged");
        const auto destination_path = root / (prefix + "-destination.archive");
        empty_archive(destination_path, format);
        mcpp::ArchiveProvider destination(destination_path, options(true));
        engine.copy_exact(local, local.stat((root / "local-tree").string(), {}), destination, "/import", {}, {});
        check_tree(destination, "/import", archive_time(format));
        mcpp::ArchiveProvider reopened(destination_path, options());
        check_tree(reopened, "/import", archive_time(format));
        const auto first_revision = destination.stat("/import/a-first", {}).revision;
        write(destination, "/unrelated", "not in the group");
        check(destination.stat("/import/a-first", {}).revision == first_revision,
              "unrelated archive publications do not invalidate a cached member revision");
        engine.copy_exact(destination, destination.stat("/import", {}), destination, "/copy", {}, {});
        check_tree(destination, "/copy", archive_time(format));
        check(destination.stat("/copy/a-first", {}).inode != destination.stat("/import/a-first", {}).inode,
              "same-archive copy does not link the destination back to the source");
        const auto cross_path = root / (prefix + "-cross.archive");
        empty_archive(cross_path, format);
        mcpp::ArchiveProvider cross(cross_path, options(true));
        engine.copy_exact(source, source.stat("/tree", {}), cross, "/cross", {}, {});
        check_tree(cross, "/cross", archive_time(format));
    }
    const auto no_permissions_path = root / "no-permissions.tar";
    empty_archive(no_permissions_path, mcpp::ArchiveFormat::tar);
    mcpp::ArchiveProvider no_permissions(no_permissions_path, options(true));
    mcpp::OperationOptions unpreserved;
    unpreserved.preserve_permissions = false;
    engine.copy_exact(local, local.stat((root / "local-tree").string(), {}), no_permissions, "/tree",
                      unpreserved, {});
    check_tree(no_permissions, "/tree", precise_time, false);
    engine.copy_exact(local, local.stat((root / "local-tree").string(), {}), local,
                      (root / "local-local").string(), {}, {});
    check_tree(local, (root / "local-local").string(), precise_time);
    check(!fs::equivalent(root / "local-local/a-first", root / "local-tree/a-first"),
          "ordinary local copy still creates an independent group");
}

void inode_collisions(const fs::path& root, mcpp::LocalProvider& local) {
    fs::create_directory(root / "device-keys");
    put(root / "device-keys/a1", "ONE");
    put(root / "device-keys/b1", "TWO");
    fs::create_hard_link(root / "device-keys/a1", root / "device-keys/a2");
    fs::create_hard_link(root / "device-keys/b1", root / "device-keys/b2");
    std::vector<mcpp::Entry> sources;
    for (const auto* name : {"a1", "b1", "a2", "b2"}) {
        auto entry = local.stat((root / "device-keys" / name).string(), {});
        entry.inode = 777;
        // Synthetic device:inode revisions exercise collisions without mounts.
        entry.revision = std::string(name[0] == 'a' ? "17:777:" : "23:777:") + "1700000000:0:3:1700000000:0";
        sources.push_back(std::move(entry));
    }
    empty_archive(root / "device-keys.tar", mcpp::ArchiveFormat::tar);
    mcpp::ArchiveProvider archive(root / "device-keys.tar", options(true));
    mcpp::OperationEngine engine;
    engine.copy(local, sources, archive, "/", {}, {});
    check(read(archive, "/a1") == "ONE" && read(archive, "/b1") == "TWO", "device collision does not mix bytes");
    check(archive.stat("/a1", {}).inode == archive.stat("/a2", {}).inode &&
          archive.stat("/b1", {}).inode == archive.stat("/b2", {}).inode &&
          archive.stat("/a1", {}).inode != archive.stat("/b1", {}).inode, "different device revisions form separate groups");
    engine.copy(local, sources, local, (root / "device-key-export").string(), {}, {});
    check(fs::equivalent(root / "device-key-export/a1", root / "device-key-export/a2") &&
          fs::equivalent(root / "device-key-export/b1", root / "device-key-export/b2") &&
          !fs::equivalent(root / "device-key-export/a1", root / "device-key-export/b1"),
          "existing local grouping also distinguishes device revisions");
}

void readonly_operations(const fs::path& root, mcpp::LocalProvider& local) {
    mcpp::OperationEngine engine;
    archive_tree(root / "readonly.tar", mcpp::ArchiveFormat::tar);
    mcpp::ArchiveProvider archive(root / "readonly.tar", options());
    const auto before = get(root / "readonly.tar");
    const auto source = local.stat((root / "local-tree").string(), {});
    rejects([&] { engine.copy_exact(local, source, archive, "/denied", {}, {}); }, "readonly archive destination is rejected");
    check(!archive.exists("/denied") && get(root / "readonly.tar") == before && !archive.last_draft(),
          "readonly copy creates no directory, file or draft");
    rejects([&] {
        engine.move(archive, {archive.stat("/tree", {})}, local, (root / "no-move-out").string(), {}, {});
    }, "move from readonly source fails before copying");
    check(!fs::exists(root / "no-move-out") && archive.exists("/tree"), "readonly move-out has no partial destination");
    rejects([&] { engine.move(local, {source}, archive, "/", {}, {}); }, "move into readonly destination is rejected");
    check(fs::is_directory(root / "local-tree") && get(root / "readonly.tar") == before,
          "readonly move-in does not remove its source");
    rejects([&] { engine.move_exact(archive, archive.stat("/tree", {}), local,
                                    (root / "no-move-exact").string(), {}, {}); }, "exact readonly move-out is rejected");
    check(!fs::exists(root / "no-move-exact"), "exact readonly move-out has no partial destination");
    rejects([&] { engine.remove(archive, {archive.stat("/tree", {})}, {}, {}); }, "readonly deletion is rejected");
    check(get(root / "readonly.tar") == before, "readonly delete leaves archive bytes unchanged");
    mcpp::OperationOptions skip;
    std::size_t copy_errors = 0;
    skip.error = [&](const mcpp::Entry&, const std::string&) {
        ++copy_errors;
        return mcpp::OperationErrorDecision::skip;
    };
    mcpp::OperationResult copy_result;
    engine.copy(local, {source}, archive, "/", skip, {}, {}, &copy_result);
    check(copy_errors == 1U && copy_result.completed_sources.empty() && copy_result.skipped.size() == 1U,
          "readonly copy errors retain the existing skip resolver behavior");
    std::size_t delete_errors = 0;
    mcpp::OperationResult remove_result;
    engine.remove(archive, {archive.stat("/tree", {})}, {}, {}, &remove_result,
                  [&](const mcpp::Entry&, const std::string&) {
                      ++delete_errors;
                      return mcpp::OperationErrorDecision::skip;
                  });
    check(delete_errors == 1U && remove_result.completed_sources.empty() && remove_result.skipped.size() == 1U,
          "readonly delete errors retain the existing skip resolver behavior");
    mcpp::Cancellation stop;
    stop.request();
    cancelled([&] { engine.move(archive, {archive.stat("/tree", {})}, local,
                                 (root / "no-cancelled-move").string(), {}, stop); },
              "cancellation takes precedence over readonly move validation");
}

void directory_cycles(const fs::path& root, mcpp::LocalProvider& local) {
    mcpp::OperationEngine engine;
    archive_tree(root / "cycles.tar", mcpp::ArchiveFormat::tar);
    mcpp::ArchiveProvider archive(root / "cycles.tar", options(true));
    archive.create_symlink("tree", "/alias");
    const auto unchanged = get(root / "cycles.tar");
    rejects([&] { engine.copy_exact(archive, archive.stat("/tree", {}), archive, "/tree/sub", {}, {}); },
            "lexical archive copy cycle is rejected");
    rejects([&] { engine.copy_exact(archive, archive.stat("/tree", {}), archive, "/alias/sub", {}, {}); },
            "archive destination alias cycle is rejected");
    rejects([&] { engine.move_exact(archive, archive.stat("/tree", {}), archive, "/alias/sub", {}, {}); },
            "archive move alias cycle is rejected");
    mcpp::OperationOptions dereference;
    dereference.preserve_symlinks = false;
    rejects([&] { engine.copy_exact(archive, archive.stat("/alias", {}), archive, "/tree/sub", dereference, {}); },
            "dereferenced archive source alias cycle is rejected");
    mcpp::ArchiveProvider other_view(root / "cycles.tar", options());
    rejects([&] { engine.copy_exact(other_view, other_view.stat("/tree", {}), archive, "/alias/sub", {}, {}); },
            "separate views of one archive still reject a directory cycle");
    check(get(root / "cycles.tar") == unchanged && !archive.exists("/tree/sub"),
          "archive cycle guards run before publishing any destination");
    archive.create_directory("/loop");
    archive.create_symlink(".", "/loop/back");
    rejects([&] { engine.copy_exact(archive, archive.stat("/loop", {}), local,
                                    (root / "loop-output").string(), dereference, {}); },
            "ancestor directory symlink is not recursively followed forever");
    check(!fs::exists(root / "loop-output/back"), "cyclic descendant is not created");
    fs::create_directory(root / "contains-destination");
    put(root / "contains-destination/data", "payload");
    empty_archive(root / "contains-destination/self.tar", mcpp::ArchiveFormat::tar);
    mcpp::ArchiveProvider inside(root / "contains-destination/self.tar", options(true));
    const auto inside_before = get(root / "contains-destination/self.tar");
    rejects([&] { engine.copy_exact(local, local.stat((root / "contains-destination").string(), {}),
                                    inside, "/recursive", {}, {}); },
            "local tree cannot be copied into an archive contained in that tree");
    check(get(root / "contains-destination/self.tar") == inside_before && !inside.exists("/recursive"),
          "mixed local/archive cycle is rejected before publication");
    fs::create_directory_symlink(root / "local-tree", root / "local-alias");
    rejects([&] { engine.copy_exact(local, local.stat((root / "local-tree").string(), {}), local,
                                    (root / "local-alias/recursive").string(), {}, {}); },
            "existing local directory alias guard still applies");
    check(!fs::exists(root / "local-tree/recursive"), "local cycle guard creates no descendant");
}

void stale_publications(const fs::path& root, mcpp::LocalProvider& local) {
    mcpp::OperationEngine engine;
    fs::create_directory(root / "stale-source");
    put(root / "stale-source/a", payload);
    fs::create_hard_link(root / "stale-source/a", root / "stale-source/b");
    empty_archive(root / "stale.tar", mcpp::ArchiveFormat::tar);
    mcpp::ArchiveProvider archive(root / "stale.tar", options(true));
    archive.create_directory("/out");
    write(archive, "/out/b", "KEEP");
    const auto original_b = archive.stat("/out/b", {});
    const auto first = local.stat((root / "stale-source/a").string(), {});
    const auto second = local.stat((root / "stale-source/b").string(), {});
    mcpp::OperationOptions overwrite;
    overwrite.overwrite = true;
    bool changed = false;
    std::string external;
    mcpp::OperationResult result;
    rejects([&] {
        engine.copy(local, {first, second}, archive, "/out", overwrite, {},
                    [&](const mcpp::TransferProgress& progress) {
                        if (!changed && progress.current_path == second.path) {
                            external = get(root / "stale.tar") + std::string(512, '\0');
                            put(root / "stale.tar", external);
                            changed = true;
                        }
                    }, &result);
    }, "stale hardlink overwrite publication is rejected");
    check(changed && get(root / "stale.tar") == external, "external writer is not overwritten");
    check(read(archive, "/out/a") == payload && read(archive, "/out/b") == "KEEP" &&
          archive.stat("/out/b", {}).inode == original_b.inode,
          "failed hardlink replacement never deletes or replaces its old member");
    check(result.completed_sources == std::vector<std::string>{first.path}, "earlier completed source remains reported");
    check(archive.last_draft() && fs::is_regular_file(*archive.last_draft()), "stale hardlink publication retains complete draft");
    mcpp::ArchiveProvider draft(*archive.last_draft(), options());
    check(read(draft, "/out/a") == payload && read(draft, "/out/b") == payload &&
          draft.stat("/out/a", {}).inode == draft.stat("/out/b", {}).inode &&
          draft.stat("/out/a", {}).links == 2U, "retained draft contains one correctly linked replacement group");
    check(get(root / "stale-source/a") == payload && get(root / "stale-source/b") == payload,
          "publication failure does not change local sources");
}

void cancellation_and_retry(const fs::path& root, mcpp::LocalProvider& local) {
    mcpp::OperationEngine engine;
    put(root / "large-source", std::string(131072, 'x'));
    empty_archive(root / "cancel.tar", mcpp::ArchiveFormat::tar);
    mcpp::ArchiveProvider archive(root / "cancel.tar", options(true));
    write(archive, "/target", "KEEP");
    const auto before = get(root / "cancel.tar");
    mcpp::OperationOptions overwrite;
    overwrite.overwrite = true;
    mcpp::Cancellation stop;
    mcpp::OperationResult cancelled_result;
    cancelled([&] {
        engine.copy_exact(local, local.stat((root / "large-source").string(), {}), archive, "/target",
                          overwrite, stop, [&](const mcpp::TransferProgress& progress) {
                              if (progress.bytes_done > 0U) stop.request();
                          }, &cancelled_result);
    }, "cancellation interrupts archive copy before publication");
    check(get(root / "cancel.tar") == before && read(archive, "/target") == "KEEP" &&
          cancelled_result.completed_sources.empty(), "cancelled archive copy preserves old destination");
    mcpp::OperationResult skipped;
    engine.copy_exact(local, local.stat((root / "large-source").string(), {}), archive, "/target",
                      overwrite, {}, [](const mcpp::TransferProgress& progress) {
                          if (progress.bytes_done > 0U) throw mcpp::SkipCurrent{};
                      }, &skipped);
    check(get(root / "cancel.tar") == before && skipped.completed_sources.empty() && skipped.skipped.size() == 1U,
          "SkipCurrent preserves old archive destination and reports skipped source");
    put(root / "retry-source", "retry data");
    fs::permissions(root / "retry-source", fs::perms::none);
    mcpp::OperationOptions retry;
    std::size_t failures = 0;
    retry.error = [&](const mcpp::Entry&, const std::string&) {
        ++failures;
        if (failures > 1U) throw std::runtime_error("unexpected extra retry");
        fs::permissions(root / "retry-source", fs::perms{0600});
        return mcpp::OperationErrorDecision::retry;
    };
    engine.copy_exact(local, local.stat((root / "retry-source").string(), {}), archive, "/retried", retry, {});
    check(failures == 1U && read(archive, "/retried") == "retry data" &&
          archive.stat("/retried", {}).permissions == fs::perms{0600}, "retry refreshes source metadata and succeeds");
}

void moves(const fs::path& root, mcpp::LocalProvider& local) {
    mcpp::OperationEngine engine;
    fs::create_directory(root / "move-source");
    put(root / "move-source/a", payload);
    fs::create_hard_link(root / "move-source/a", root / "move-source/b");
    empty_archive(root / "move.tar", mcpp::ArchiveFormat::tar);
    mcpp::ArchiveProvider archive(root / "move.tar", options(true));
    engine.move(local, {local.stat((root / "move-source/a").string(), {}),
                        local.stat((root / "move-source/b").string(), {})}, archive, "/", {}, {});
    check(!fs::exists(root / "move-source/a") && !fs::exists(root / "move-source/b") &&
          read(archive, "/a") == payload && archive.stat("/a", {}).inode == archive.stat("/b", {}).inode,
          "local-to-archive batch move preserves its copied hardlink group");
    engine.move(archive, {archive.stat("/a", {}), archive.stat("/b", {})}, local,
                (root / "moved-back").string(), {}, {});
    check(!archive.exists("/a") && !archive.exists("/b") &&
          fs::equivalent(root / "moved-back/a", root / "moved-back/b") &&
          get(root / "moved-back/a") == payload, "archive-to-local move preserves group and removes sources");
}

}  // namespace

int main(const int argc, char** argv) {
    try {
        if (::geteuid() == 0 || !fs::is_regular_file("/review/.operation-regressions-disposable")) {
            throw std::runtime_error("archive operation regressions require a non-root disposable review container");
        }
        if (argc != 2) throw std::runtime_error("usage: test_archive_operations /review/cases/agent-archive-operations");
        const auto base = fs::absolute(argv[1]).lexically_normal();
        if (base != "/review/cases/agent-archive-operations" || !fs::is_directory(base) ||
            fs::is_symlink(fs::symlink_status(base))) {
            throw std::runtime_error("unexpected archive operation regression workspace");
        }
        const auto root = base / ("run-" + std::to_string(::getpid()));
        if (!fs::create_directory(root)) throw std::runtime_error("archive operation workspace already exists");
        fs::permissions(root, fs::perms{0700});
        fs::current_path(root);
        fs::create_directory("transfers");
        check(::setenv("TMPDIR", (root / "transfers").c_str(), 1) == 0, "keep engine staging within disposable workspace");
        const auto old_mask = ::umask(0022);
        mcpp::LocalProvider local(root);
        copies(root, local);
        inode_collisions(root, local);
        readonly_operations(root, local);
        directory_cycles(root, local);
        stale_publications(root, local);
        cancellation_and_retry(root, local);
        moves(root, local);
        ::umask(old_mask);
        std::cout << "PASS: " << checks << " archive operation checks; uid=" << ::geteuid()
                  << "; workspace=" << root << '\n';
        fs::current_path(base);
        fs::remove_all(root);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " archive operation checks: " << error.what() << '\n';
        return 1;
    }
}
