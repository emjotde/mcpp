#include "mcpp/archive_provider.hpp"
#include "mcpp/editor.hpp"
#include "mcpp/local_provider.hpp"
#include "mcpp/local_tools.hpp"
#include "mcpp/location.hpp"
#include "mcpp/operations.hpp"
#include "mcpp/transform_provider.hpp"
#include "mcpp/viewer.hpp"

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
void rejects(Function function, const std::string& message, const std::string_view expected = {}) {
    try { function(); }
    catch (const std::exception& error) {
        check(expected.empty() || std::string_view(error.what()).find(expected) != std::string_view::npos,
              message + ": unexpected error: " + error.what());
        return;
    }
    throw std::runtime_error(message);
}

void put(const fs::path& path, const std::string_view bytes) {
    std::ofstream output(path, std::ios::binary);
    output.exceptions(std::ios::badbit | std::ios::failbit);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::string get(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    check(input.good(), "open test file");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::string read(const mcpp::ProviderPtr& provider, const std::string_view path = "/") {
    const auto entry = provider->stat_follow(path, {});
    std::ostringstream output;
    provider->read(path, 0, entry.size, output, {});
    return output.str();
}

mcpp::TransformOpenOptions options(const fs::path& root) {
    const auto registry = root / "sfs.ini";
    put(registry, "# Original local test converters; no MC rules are bundled.\n");
    return {.cache_directory=root / "private-cache", .registry_file=registry};
}

void local_case(const fs::path& root) {
    auto settings = options(root);
    put(*settings.registry_file,
        "upper/1\t/bin/echo called >> '" + (root / "calls").string() +
        "'; /usr/bin/tr '[:lower:]' '[:upper:]' < %1 > %3\n"
        "literal/1\t/bin/printf '%%s\\n' \"%2\" > %3\n"
        "quoted/1\t/bin/cat '%1' > '%3'\n"
        "name/1\t/usr/bin/basename %1 > %3\n"
        "upper/1\t/bin/echo wrong-duplicate > %3\n");
    const auto source = root / "source text'.txt";
    const std::string original = "small native transformation\n";
    put(source, original);
    auto local = std::make_shared<mcpp::LocalProvider>();
    mcpp::ArchiveCache cache(local, std::chrono::seconds(60), settings);
    const auto uri = source.string() + "/upper://";
    const auto transformed = mcpp::open_location(uri, local, &cache);
    check(read(transformed.provider) == "SMALL NATIVE TRANSFORMATION\n",
          "registered /1 helper reads the correct private local copy");
    const auto* sfs = dynamic_cast<const mcpp::TransformProvider*>(transformed.provider.get());
    check(sfs != nullptr && sfs->source().provider == local && sfs->source().path == source &&
          sfs->source().revision == local->stat_follow(source.string(), {}).revision &&
          sfs->source().location == source, "provider captures logical source, namespace and revision");
    check(cache.format(transformed.provider, transformed.path) == uri,
          "formatting uses native SFS paths, not cache filenames");
    check(transformed.path == "/" && transformed.provider->stat("/", {}).kind == mcpp::EntryKind::file,
          "an SFS result is a single file, not an invented directory");
    check(!mcpp::has_capability(transformed.provider->capabilities(), mcpp::ProviderCapability::write),
          "transformed provider has no write capability");
    const auto quoted = mcpp::open_location(source.string() + "/quoted://", local, &cache);
    check(read(quoted.provider) == original, "quoted native placeholders remain literal arguments");
    fs::create_symlink(source.filename(), root / "alias.txt");
    const auto alias = mcpp::open_location((root / "alias.txt").string() + "/name://", local, &cache);
    check(read(alias.provider) == "alias.txt\n", "physical snapshots preserve a logical source alias basename");
    auto independent = std::make_shared<mcpp::LocalProvider>();
    const auto same_id = cache.open_transform({independent, source.string()}, "quoted");
    check(same_id != quoted.provider &&
          static_cast<const mcpp::TransformProvider&>(*same_id).source().provider == independent,
          "equal provider ID strings cannot merge independently captured providers");
    const std::string parameter = "data ' \" $(touch " + (root / "must-not-exist").string() + ")";
    const auto literal = mcpp::open_location(source.string() + "/literal://" + parameter, local, &cache);
    check(read(literal.provider) == parameter + "\n" && !fs::exists(root / "must-not-exist"),
          "suffix parameters cannot inject shell commands, including inside quotes");
    mcpp::ViewerModel viewer;
    viewer.open(transformed.provider, "/");
    check(viewer.lines(80, 1).front() == "SMALL NATIVE TRANSFORMATION", "viewer reads transformed bytes");
    rejects([&] { viewer.toggle_hex_editing(); }, "transformed bytes must not become hex-editable");
    mcpp::EditorModel editor;
    editor.open(transformed.provider, "/");
    editor.insert("X");
    rejects([&] { editor.save(false); }, "Save must not execute a reverse conversion or overwrite its source");
    check(get(root / "calls") == "called\n" && get(source) == original,
          "failed saves never rerun the converter or alter the source");
    editor.save_as(local, (root / "saved.txt").string(), false, mcpp::EditorLineEnding::unchanged);
    check(get(root / "saved.txt") == "XSMALL NATIVE TRANSFORMATION\n",
          "explicit Save As publishes ordinary local bytes");
    editor.insert("Y");
    editor.save(false);
    check(get(root / "saved.txt") == "XYSMALL NATIVE TRANSFORMATION\n",
          "a saved local copy is writable without enabling conversion on the source");
    mcpp::OperationEngine{}.copy_exact(
        *transformed.provider, transformed.provider->stat("/", {}), *local,
        (root / "copied.txt").string(), {}, {});
    check(get(root / "copied.txt") == read(transformed.provider),
          "copy uses provider data instead of the host root");
    for (const auto* name : {"gz", "bz2", "xz"}) {
        const auto encoded = mcpp::open_location(source.string() + "/" + name + "://", local, &cache);
        const auto stored = root / (std::string("encoded.") + name);
        put(stored, read(encoded.provider));
        const auto decoded = mcpp::open_location(stored.string() + "/u" + name + "://", local, &cache);
        check(read(decoded.provider) == original, std::string(name) + " standard native transform roundtrip");
        mcpp::ViewerModel transformed_viewer;
        transformed_viewer.open(encoded.provider, "/");
        const auto transformed_rows = transformed_viewer.lines(80, 1);
        check(!transformed_rows.empty() && transformed_rows.front() == "small native transformation",
              std::string(name) + " compressed virtual output is parsed through its captured provider");
        mcpp::ViewerModel local_viewer;
        local_viewer.open(local, stored.string());
        const auto local_rows = local_viewer.lines(80, 1);
        check(!local_rows.empty() && local_rows.front() == "small native transformation",
              std::string(name) + " local parsed viewing retains its executable mapping");
    }
    check(read(mcpp::open_location(source.string() + "/ugz://", local, &cache).provider) == original,
          "native ugz keeps gzip's force-to-stdout behavior for uncompressed input");
    auto native_settings = settings;
    native_settings.registry_file.reset();
    mcpp::TransformCache native(std::chrono::seconds(0), native_settings);
    const auto compressed = root / "encoded.gz";
    check(read(native.open(local, compressed.string(), compressed.string(), "ugz")) == original,
          "the installed native sfs.ini local helper interface is usable without bundling its rules");
    put(*settings.registry_file, "upper/1\t/bin/cat %1 > %3\n");
    const auto changed_rule = mcpp::open_location(uri, local, &cache);
    check(changed_rule.provider != transformed.provider && read(changed_rule.provider) == original &&
          read(transformed.provider) == "SMALL NATIVE TRANSFORMATION\n",
          "changed converter definitions get new snapshots without altering existing ones");
    check(fs::is_empty(settings.cache_directory), "private helper workspaces are released after snapshot creation");
    check(mcpp::is_virtual_location(uri) && mcpp::is_transform_location(uri) &&
          !mcpp::is_archive_location(uri) && mcpp::is_virtual_location("sftp://paused/file"),
          "shared URI recognition preserves native and unsupported provider paths");
}

void namespace_case(const fs::path& root) {
    const auto settings = options(root);
    auto local = std::make_shared<mcpp::LocalProvider>(root);
    const auto source = root / "document";
    put(source, "archive snapshot one\n");
    mcpp::ArchiveCache cache(local, std::chrono::seconds(60), settings);
    const auto gzip = mcpp::open_location(source.string() + "/gz://", local, &cache);
    fs::create_directories(root / "members/directory");
    put(root / "members/directory/document.gz", read(gzip.provider));
    const auto packed = root / "sample.tar.gz";
    const auto made = mcpp::capture_command(
        {"tar", "--format=ustar", "-czf", packed.string(), "-C", (root / "members").string(), "directory"},
        root);
    check(made.status == 0, "create tiny compressed tar: " + made.error);
    const auto native_archive = packed.string() + "/utar://directory";
    const auto archive = mcpp::open_location(native_archive, local, &cache);
    check(dynamic_cast<const mcpp::ArchiveProvider*>(archive.provider.get()) != nullptr &&
          mcpp::is_archive_filename(packed.filename().string()),
          "compressed-tar archive precedence and native dispatch are retained");
    const auto relative = mcpp::open_location("document.gz/ugz://", archive, &cache);
    const auto absolute = mcpp::open_location("/directory/document.gz/ugz://", archive, &cache);
    const auto dotted = mcpp::open_location("../directory/./document.gz/ugz://", archive, &cache);
    check(relative.provider == absolute.provider && relative.provider == dotted.provider &&
          read(relative.provider) == "archive snapshot one\n",
          "relative and rooted member transforms stay in their captured virtual namespace");
    check(cache.format(relative.provider, "/") == native_archive + "/document.gz/ugz://",
          "a transformed member retains the full native source chain");
    put(root / "host-only.gz", read(gzip.provider));
    rejects([&] { static_cast<void>(mcpp::open_location(
        (root / "host-only.gz").string() + "/ugz://", archive, &cache)); },
        "an absolute archive member must never fall back to an existing host path");
    rejects([&] { static_cast<void>(relative.provider->stat("/document", {})); },
            "unknown transformed member paths never access host files");
    const auto before = mcpp::open_location(source.string() + "/gz://", local, &cache);
    put(source, "archive snapshot two\n");
    const auto after = mcpp::open_location(source.string() + "/gz://", local, &cache);
    check(before.provider != after.provider &&
          mcpp::decompress_bytes(read(before.provider), "ugz", {}, settings) == "archive snapshot one\n" &&
          mcpp::decompress_bytes(read(after.provider), "ugz", {}, settings) == "archive snapshot two\n",
          "source revision changes create new snapshots without invalidating existing viewers");
    mcpp::TransformCache lifetime(std::chrono::seconds(0), settings);
    auto held = lifetime.open(local, source.string(), source.string(), "gz");
    std::weak_ptr<mcpp::Provider> weak = held;
    mcpp::ViewerModel first;
    mcpp::ViewerModel second;
    // Plain registered output avoids relying on parsed-view settings here.
    put(*settings.registry_file, "plain/1\t/bin/cat %1 > %3\n");
    {
        mcpp::TransformCache guarded(std::chrono::seconds(0), settings);
        rejects([&] { static_cast<void>(guarded.open(
            local, source.string(), source.string(), "plain", "..")); },
            "unsafe output basenames must fail before conversion", "unsafe output basename");
    }
    held = lifetime.open(local, source.string(), source.string(), "plain");
    lifetime.prune();
    check(weak.expired(), "unused previous snapshots are released at zero timeout");
    weak = held;
    first.open(held, "/");
    second.open(held, "/");
    held.reset();
    lifetime.prune();
    check(!weak.expired() && !lifetime.locations().empty(), "independent viewers keep the snapshot alive");
    first.close();
    lifetime.prune();
    check(!weak.expired(), "closing one viewer cannot expire another viewer's snapshot");
    second.close();
    lifetime.prune();
    check(weak.expired() && lifetime.locations().empty(), "last viewer close permits cache expiration");
    {
        mcpp::TransformCache short_lived(std::chrono::seconds(0), settings);
        held = short_lived.open(local, source.string(), source.string(), "plain");
    }
    first.open(held, "/");
    held.reset();
    check(first.lines(80, 1).front() == "archive snapshot two",
          "a viewer snapshot outlives the cache that opened it");
    first.close();
    rejects([&] { lifetime.set_timeout(std::chrono::seconds(-1)); }, "negative cache timeout must fail");
    check(fs::is_empty(settings.cache_directory), "virtual-source helper workspaces are cleaned");
}

void errors_case(const fs::path& root) {
    auto settings = options(root);
    auto local = std::make_shared<mcpp::LocalProvider>(root);
    const auto source = root / "source";
    put(source, "tiny\n");
    const auto open = [&](const std::string_view name) {
        mcpp::TransformCache cache(std::chrono::seconds(0), settings);
        return cache.open(local, source.string(), source.string(), name);
    };
    put(*settings.registry_file,
        "missing/1\tmcpp-no-such-single-transform-helper < %1 > %3\n"
        "failed/1\texit 7\n"
        "unsafe/1\t/bin/ln -sf %1 %3\n"
        "large/1\t/bin/echo 1234567890 > %3\n"
        "wait/1\t/bin/sleep 2; /bin/cat %1 > %3\n"
        "remote:2\t/bin/echo %2 > %3\n");
    rejects([&] { static_cast<void>(open("missing")); }, "missing helpers must fail explicitly", "(127)");
    rejects([&] { static_cast<void>(open("failed")); }, "nonzero helpers must fail explicitly", "(7)");
    rejects([&] { static_cast<void>(open("unsafe")); }, "symlink helper output must fail without reading its target");
    rejects([&] { static_cast<void>(open("remote")); }, "non-local helpers must remain explicitly unsupported");
    rejects([&] { static_cast<void>(open("unregistered")); }, "unknown native SFS names cannot fall back locally");
    settings.limits.output_bytes = 4;
    rejects([&] { static_cast<void>(open("large")); }, "redirected helper output is resource-limited");
    settings.limits.output_bytes = 1024;
    settings.limits.elapsed = std::chrono::milliseconds(50);
    rejects([&] { static_cast<void>(open("wait")); }, "helpers have a bounded elapsed time", "elapsed-time limit");
    settings.limits.elapsed = std::chrono::milliseconds(30000);
    settings.limits.input_bytes = 2;
    rejects([&] { static_cast<void>(open("large")); }, "input bytes are bounded before invoking helpers");
    settings.limits.input_bytes = 1024;
    put(*settings.registry_file, "broken/one\t/bin/cat %1 > %3\n");
    rejects([&] { static_cast<void>(open("broken")); }, "malformed flags must fail", "invalid helper flags");
    put(*settings.registry_file, "broken/1\t/bin/echo %9 > %3\n");
    rejects([&] { static_cast<void>(open("broken")); }, "unknown helper placeholders must fail");
    put(*settings.registry_file, std::string("broken/1\t/bin/cat %1 > %3\0\n", 27));
    rejects([&] { static_cast<void>(open("broken")); }, "NUL-containing registries must fail");
    put(*settings.registry_file, "plain/1\t/bin/cat %1 > %3\n");
    mcpp::Cancellation cancelled;
    cancelled.request();
    mcpp::TransformCache cache(std::chrono::seconds(0), settings);
    rejects([&] { static_cast<void>(cache.open(local, source.string(), source.string(), "plain", {}, cancelled)); },
            "cancelled conversions do not invoke a helper");
    fs::permissions(settings.cache_directory, fs::perms::owner_all | fs::perms::others_all);
    rejects([&] { static_cast<void>(open("plain")); }, "insecure private storage must fail, not use /tmp");
    fs::permissions(settings.cache_directory, fs::perms::owner_all);
    settings.registry_file = root / "absent.ini";
    rejects([&] { static_cast<void>(open("gz")); }, "an explicit missing registry cannot silently select defaults");
    check(get(source) == "tiny\n" && fs::is_empty(settings.cache_directory),
          "error paths retain source bytes and remove private helper files");
}
}  // namespace

int main(const int argc, char** argv) {
    try {
        if (::geteuid() != 1001 || !fs::is_regular_file("/.dockerenv") ||
            !fs::is_regular_file("/review/.operation-regressions-disposable")) {
            throw std::runtime_error("SFS tests require the non-root owned disposable container");
        }
        if (argc != 3) throw std::runtime_error("expected local, namespace or errors and its fixture directory");
        const std::string mode(argv[1]);
        if (mode != "local" && mode != "namespace" && mode != "errors") throw std::runtime_error("invalid SFS case");
        const auto base = fs::absolute(argv[2]).lexically_normal();
        if (base != fs::path("/review/cases") / ("single-transform-" + mode)) {
            throw std::runtime_error("invalid SFS fixture directory");
        }
        fs::create_directories(base);
        if (fs::is_symlink(fs::symlink_status(base))) throw std::runtime_error("fixture directory is a symlink");
        const auto root = base / ("run-" + std::to_string(::getpid()));
        if (!fs::create_directory(root)) throw std::runtime_error("fixture directory already exists");
        check(::setenv("XDG_CACHE_HOME", (root / "xdg-cache").c_str(), 1) == 0, "set private test cache");
        if (mode == "local") local_case(root);
        else if (mode == "namespace") namespace_case(root);
        else errors_case(root);
        fs::remove_all(root);
        std::cout << "SFS " << mode << " case passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
