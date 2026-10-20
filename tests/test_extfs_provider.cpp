#include "mcpp/extfs_provider.hpp"
#include "mcpp/local_provider.hpp"
#include "mcpp/operations.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {
namespace fs = std::filesystem;

void check(const bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename Function>
void rejects(Function function, const std::string_view expected) {
    try { function(); }
    catch (const std::exception& error) {
        if (std::string_view(error.what()).find(expected) == std::string_view::npos) {
            throw std::runtime_error(std::string("unexpected diagnostic: ") + error.what());
        }
        return;
    }
    throw std::runtime_error("expected rejection: " + std::string(expected));
}

void put(const fs::path& path, const std::string_view value) {
    std::ofstream output(path, std::ios::binary);
    output.exceptions(std::ios::badbit | std::ios::failbit);
    output.write(value.data(), static_cast<std::streamsize>(value.size()));
}

std::string get(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    check(input.good(), "open fixture");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::string read(const mcpp::ProviderPtr& provider, const std::string_view path) {
    std::ostringstream output;
    provider->read(path, 0, provider->stat_follow(path, {}).size, output, {});
    return output.str();
}

void save(const mcpp::ProviderPtr& provider, const std::string_view path, const std::string_view bytes,
          const std::optional<std::string> revision = {}) {
    std::istringstream input{std::string(bytes)};
    provider->write(path, input, bytes.size(), true, revision, {}, {});
}

constexpr std::string_view initial =
    R"({"nodes":{"folder":{"kind":"d"},"folder/file with space":{"kind":"f","data":"LOCAL-EXTFS-CONTENTS\n","date":"01-02-2024 03:04:05"},"alias":{"kind":"l","target":"/folder/file with space","date":"Jan 02 03:04"},".hidden":{"kind":"f","data":"hidden"}}})";

mcpp::ExtfsOpenOptions options(const fs::path& root, const fs::path& fixture) {
    const auto helpers = root / "data" / "mc" / "extfs.d";
    fs::create_directories(helpers);
    fs::copy_file(fixture, helpers / "probe");
    fs::permissions(helpers / "probe", fs::perms::owner_all);
    check(::setenv("XDG_DATA_HOME", (root / "data").c_str(), 1) == 0, "set fixture discovery");
    check(::setenv("XDG_CACHE_HOME", (root / "cache").c_str(), 1) == 0, "set private cache");
    return {.cache_directory=root / "private-cache", .helper_directories={helpers}};
}

void snapshot_case(const fs::path& root, const fs::path& fixture) {
    auto settings = options(root, fixture);
    const auto path = root / "sample.tiny";
    put(path, initial);
    auto local = std::make_shared<mcpp::LocalProvider>(root);
    mcpp::ProviderPtr held;
    {
        mcpp::ExtfsCache cache(std::chrono::seconds(0), settings);
        held = cache.open(local, path.string(), "logical-source", "probe");
        const auto* extfs = dynamic_cast<const mcpp::ExtfsProvider*>(held.get());
        check(extfs != nullptr && extfs->source().provider == local &&
              extfs->source().path == path && !extfs->source().revision.empty(),
              "source namespace and revision are retained");
        check(extfs->location("/folder/file with space") == "logical-source/probe://folder/file with space",
              "native URI preserves source identity");
        check(cache.open(local, path.string(), "logical-source", "probe") == held, "cache reuses same source revision");
        check(cache.open(local, path.string(), "another-alias", "probe") != held, "distinct source URIs are not conflated");
        check(held->list("/", {}, {}).size() == 2U && held->list("/", {.show_hidden=true}, {}).size() == 3U,
              "listing filters hidden members");
        check(held->stat("/alias", {}).kind == mcpp::EntryKind::symlink &&
              held->stat_follow("/alias", {}).kind == mcpp::EntryKind::file, "absolute symlinks stay virtual");
        check(read(held, "/alias") == "LOCAL-EXTFS-CONTENTS\n", "copyout resolves a virtual absolute symlink");
        const auto nested = root / "inner.tiny";
        put(nested, R"({"nodes":{"inside":{"kind":"f","data":"virtual namespace"}}})");
        const auto outer = root / "outer.tiny";
        std::string outer_bytes = R"({"nodes":{"file":{"kind":"f","data":")";
        for (const char byte : get(nested)) {
            if (byte == '"' || byte == '\\') outer_bytes += '\\';
            outer_bytes += byte;
        }
        outer_bytes += R"("}}})";
        put(outer, outer_bytes);
        const auto outer_provider = cache.open(local, outer.string(), outer.string(), "probe");
        const auto inner_provider = cache.open(outer_provider, "/file", outer.string() + "/probe://file", "probe");
        check(read(inner_provider, "/inside") == "virtual namespace", "virtual /file is never host /file");
        put(path, R"({"nodes":{"changed":{"kind":"f","data":"new source"}}})");
        check(cache.open(local, path.string(), "logical-source", "probe") != held, "changed source creates a new snapshot");
        cache.prune();
    }
    check(read(held, "/folder/file with space") == "LOCAL-EXTFS-CONTENTS\n", "held provider survives source and cache changes");
    held.reset();
    check(fs::is_empty(settings.cache_directory), "private snapshots are cleaned");
    check(mcpp::find_extfs_helper("probe")->executable == settings.helper_directories.front() / "probe",
          "native user helper discovery works");
    const auto system = root / "system-helpers";
    fs::create_directory(system);
    fs::copy_file(fixture, system / "probe");
    fs::copy_file(fixture, system / "secondary");
    fs::permissions(system / "probe", fs::perms::owner_all);
    fs::permissions(system / "secondary", fs::perms::owner_all);
    put(settings.helper_directories.front() / "secondary", "not an executable helper\n");
    settings.helper_directories.push_back(system);
    check(mcpp::find_extfs_helper("probe", settings)->executable == settings.helper_directories.front() / "probe" &&
          mcpp::find_extfs_helper("secondary", settings)->executable == system / "secondary" &&
          mcpp::discover_extfs_helpers(settings).size() == 2U, "user helpers override system fallback helpers");
}

void mutation_case(const fs::path& root, const fs::path& fixture) {
    const auto settings = options(root, fixture);
    const auto path = root / "writable.tiny";
    put(path, initial);
    auto local = std::make_shared<mcpp::LocalProvider>(root);
    mcpp::ExtfsCache cache(std::chrono::seconds(60), settings);
    const auto provider = cache.open(local, path.string(), path.string(), "probe");
    check(mcpp::has_capability(provider->capabilities(), mcpp::ProviderCapability::write), "local source permits guarded writes");
    const auto revision = provider->stat("/folder/file with space", {}).revision;
    save(provider, "/folder/file with space", "saved\n", revision);
    check(read(provider, "/folder/file with space") == "saved\n", "copyin publishes verified data");
    rejects([&] { save(provider, "/folder/file with space", "stale\n", revision); }, "member changed");
    check(cache.open(local, path.string(), path.string(), "probe") == provider, "cache follows its own successful publication");
    provider->create_directory("/created");
    save(provider, "/created/new", "copied in\n");
    rejects([&] { provider->remove("/created", false, {}, {}); }, "Directory not empty");
    provider->remove("/created", true, {}, {});
    check(!provider->exists("/created"), "recursive rm/rmdir publishes once");
    const auto reopened = mcpp::ExtfsCache(std::chrono::seconds(0), settings).open(local, path.string(), path.string(), "probe");
    check(read(reopened, "/alias") == "saved\n", "published container survives reopen");
    const auto noop = root / "noop.tiny";
    put(noop, R"({"nodes":{"file":{"kind":"f","data":"unchanged"}},"noop":["copyin"]})");
    const auto noop_provider = cache.open(local, noop.string(), noop.string(), "probe");
    const auto before = get(noop);
    rejects([&] { save(noop_provider, "/file", "must not appear"); }, "did not produce");
    check(get(noop) == before, "a helper returning zero without changing data is not successful");
    const auto guarded = root / "guarded.tiny";
    put(guarded, R"({"nodes":{"file":{"kind":"f","data":"old"}},"during_copyin":")" + guarded.string() + R"("})");
    const auto guarded_provider = cache.open(local, guarded.string(), guarded.string(), "probe");
    rejects([&] { save(guarded_provider, "/file", "candidate"); }, "source container changed");
    check(get(guarded) == "external source change\n", "late source change is not overwritten");
    const auto readonly = root / "readonly.tiny";
    put(readonly, initial);
    fs::permissions(readonly, fs::perms::owner_read);
    const auto protected_provider = cache.open(local, readonly.string(), readonly.string(), "probe");
    check(!mcpp::has_capability(protected_provider->capabilities(), mcpp::ProviderCapability::write),
          "read-only local source does not advertise writes");
    rejects([&] { save(protected_provider, "/new", "no"); }, "not writable");
    fs::permissions(readonly, fs::perms::owner_read | fs::perms::owner_write);
    check(fs::is_empty(settings.cache_directory), "mutation workspaces are cleaned");
}

void rejection_case(const fs::path& root, const fs::path& fixture) {
    auto settings = options(root, fixture);
    auto local = std::make_shared<mcpp::LocalProvider>(root);
    mcpp::ExtfsCache cache(std::chrono::seconds(0), settings);
    const auto path = root / "unsafe.tiny";
    const auto open = [&] { return cache.open(local, path.string(), path.string(), "probe"); };
    for (const auto* listing : {
        R"({"nodes":{},"listing":"-rw-r--r-- 1 1001 1001 1 Jan 02 2024 ../escape\n"})",
        R"({"nodes":{},"listing":"lrwxrwxrwx 1 1001 1001 1 Jan 02 2024 link -> ../../escape\n"})",
        R"({"nodes":{},"listing":"-rw-r--r-- 1 1001 1001 1 Jan 02 2024 same\n-rw-r--r-- 1 1001 1001 1 Jan 02 2024 same\n"})",
        R"({"nodes":{},"listing":"-rw-r--r-- 1 1001 1001 1 Jan 02 2024 ambiguous -> name\n"})"}) {
        put(path, listing);
        rejects([&] { static_cast<void>(open()); }, "extfs:");
    }
    const auto marker = root / "host-marker";
    put(marker, "HOST-MUST-NOT-BE-READ");
    put(path, R"({"nodes":{"file":{"kind":"f","data":"x"}},"unsafe_copyout":")" + marker.string() + R"("})");
    const auto unsafe = open();
    rejects([&] { static_cast<void>(read(unsafe, "/file")); }, "Too many levels");
    check(get(marker) == "HOST-MUST-NOT-BE-READ", "unsafe helper output does not alter host target");
    put(path, R"({"nodes":{"file":{"kind":"f","data":"x"}},"unsupported":["copyin"]})");
    const auto incomplete = open();
    const auto original = get(path);
    rejects([&] { save(incomplete, "/file", "not published"); }, "unsupported (7)");
    check(get(path) == original, "unsupported helper operation cannot publish");
    rejects([&] { static_cast<void>(cache.open(local, path.string(), path.string(), "missing")); }, "not installed");
    fs::copy_file(fixture, settings.helper_directories.front() / "global+");
    fs::permissions(settings.helper_directories.front() / "global+", fs::perms::owner_all);
    rejects([&] { static_cast<void>(cache.open(local, path.string(), path.string(), "global")); }, "source-less");
    mcpp::Cancellation cancelled;
    cancelled.request();
    rejects([&] { static_cast<void>(cache.open(local, path.string(), path.string(), "probe", cancelled)); }, "cancelled");

    const auto transfer = root / "transfer";
    fs::create_directory(transfer);
    check(::setenv("TMPDIR", transfer.c_str(), 1) == 0, "keep operation staging inside the fixture");
    const auto guarded_path = root / "operation-guards.tiny";
    put(guarded_path,
        R"({"nodes":{"folder":{"kind":"d"},"folder/data":{"kind":"f","data":"COMPLETE-VIRTUAL-TARGET-CONTENTS\n"},"alias":{"kind":"l","target":"/folder/data"},"directory-alias":{"kind":"l","target":"/folder"}}})");
    const auto guarded = cache.open(local, guarded_path.string(), guarded_path.string(), "probe");
    const auto output = root / "copy-target";
    fs::create_directory(output);
    mcpp::OperationEngine engine;
    mcpp::OperationOptions preserve;
    preserve.compute_totals = false;
    const auto before_links = get(guarded_path);
    rejects([&] {
        engine.copy_exact(*guarded, guarded->stat("/alias", {}), *local,
            (output / "absolute-link").string(), preserve, {});
    }, "container root; choose follow links");
    check(!local->exists((output / "absolute-link").string()), "virtual absolute link is not exported as a host link");
    const auto local_data = root / "local-data";
    const std::string local_payload = "COMPLETE-LOCAL-TARGET-CONTENTS\n";
    put(local_data, local_payload);
    const auto local_link = root / "local-link";
    fs::create_symlink("local-data", local_link);
    rejects([&] {
        engine.copy_exact(*local, local->stat(local_link.string(), {}), *guarded,
            "/from-local-link", preserve, {});
    }, "cannot create symlinks; choose follow links");
    check(!guarded->exists("/from-local-link") && get(guarded_path) == before_links,
          "unsupported link preservation cannot create a truncated regular member");
    auto follow = preserve;
    follow.preserve_symlinks = false;
    engine.copy_exact(*guarded, guarded->stat("/alias", {}), *local,
        (output / "followed").string(), follow, {});
    check(get(output / "followed") == "COMPLETE-VIRTUAL-TARGET-CONTENTS\n",
          "following an absolute virtual link copies its full virtual target");
    engine.copy_exact(*local, local->stat(local_link.string(), {}), *guarded,
        "/from-local-link", follow, {});
    check(read(guarded, "/from-local-link") == local_payload,
          "following a local link into extfs uses the target length, not the link length");
    const auto before_directories = get(guarded_path);
    rejects([&] {
        engine.copy_exact(*guarded, guarded->stat("/folder", {}), *guarded,
            "/directory-alias/child", preserve, {});
    }, "directory into itself");
    const auto alias_provider = cache.open(local, guarded_path.string(), "distinct-source-uri", "probe");
    check(alias_provider->id() != guarded->id(), "directory guard fixture uses distinct provider identities");
    rejects([&] {
        engine.copy_exact(*guarded, guarded->stat("/folder", {}), *alias_provider,
            "/directory-alias/child", preserve, {});
    }, "directory into itself");
    check(!guarded->exists("/folder/child") && !alias_provider->exists("/folder/child") &&
          get(guarded_path) == before_directories, "directory aliases cannot bypass the descendant guard");
    save(guarded, "/inner", R"({"nodes":{}})");
    const auto inner = cache.open(guarded, "/inner", guarded_path.string() + "/probe://inner", "probe");
    const auto before_backing = get(guarded_path);
    rejects([&] {
        engine.copy_exact(*local, local->stat(root.string(), {}), *inner,
            "/containing-directory", preserve, {});
    }, "directory into itself");
    check(!inner->exists("/containing-directory") && get(guarded_path) == before_backing,
          "recursive source-provider pairs identify the real local backing container");
    auto overwrite = preserve;
    overwrite.overwrite = true;
    bool newer = false;
    rejects([&] {
        engine.copy_exact(*local, local->stat(local_data.string(), {}), *guarded, "/folder/data",
            overwrite, {}, [&](const mcpp::TransferProgress&) {
                if (!newer) {
                    newer = true;
                    save(guarded, "/folder/data", "newer during staged copy\n");
                }
            });
    }, "member changed since it was opened");
    check(newer && read(guarded, "/folder/data") == "newer during staged copy\n",
          "generic extfs copy honors the captured destination revision");
    const auto moved_copy = output / "moved-copy";
    const auto moved_payload = read(guarded, "/folder/data");
    bool source_newer = false;
    rejects([&] {
        engine.move_exact(*guarded, guarded->stat("/folder/data", {}), *local, moved_copy.string(),
            preserve, {}, [&](const mcpp::TransferProgress& value) {
                if (!source_newer && value.files_done == 1U) {
                    check(get(moved_copy) == moved_payload, "move destination is published before source mutation");
                    source_newer = true;
                    save(guarded, "/folder/data", "newer source after copying\n");
                }
            });
    }, "source changed after copying");
    check(source_newer && read(guarded, "/folder/data") == "newer source after copying\n" &&
          get(moved_copy) == moved_payload, "move preserves the newer source and valid copied destination");
    check(fs::is_empty(transfer), "operation staging files are cleaned");
    check(fs::is_empty(settings.cache_directory), "failed helpers leave no private copies");
}

}  // namespace

int main(const int argc, char** argv) {
    try {
        if (::geteuid() != 1001 || !fs::is_regular_file("/.dockerenv") ||
            !fs::is_regular_file("/review/.operation-regressions-disposable")) {
            throw std::runtime_error("extfs tests require the owned non-root disposable container");
        }
        if (argc != 4) throw std::runtime_error("expected snapshot|mutation|rejection, unique case base, helper fixture");
        const std::string mode(argv[1]);
        if (mode != "snapshot" && mode != "mutation" && mode != "rejection") throw std::runtime_error("unknown extfs case");
        const auto base = fs::absolute(argv[2]).lexically_normal();
        if (base != fs::path("/review/cases") / ("extfs-local-" + mode)) throw std::runtime_error("invalid fixture base");
        fs::create_directories(base);
        struct stat state{};
        if (::lstat(base.c_str(), &state) != 0 || !S_ISDIR(state.st_mode) || state.st_uid != ::geteuid()) {
            throw std::runtime_error("extfs fixture base must be owned and not a symlink");
        }
        const auto root = base / ("run-" + std::to_string(::getpid()));
        if (!fs::create_directory(root)) throw std::runtime_error("fixture run already exists");
        if (mode == "snapshot") snapshot_case(root, argv[3]);
        else if (mode == "mutation") mutation_case(root, argv[3]);
        else rejection_case(root, argv[3]);
        fs::remove_all(root);
        std::cout << "extfs " << mode << " passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
