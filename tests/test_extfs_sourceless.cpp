#include "mcpp/extfs_provider.hpp"
#include "mcpp/local_provider.hpp"

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

void put(const fs::path& path, const std::string_view bytes) {
    std::ofstream output(path, std::ios::binary);
    output.exceptions(std::ios::badbit | std::ios::failbit);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::string get(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    check(input.good(), "open original source-less fixture");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::string read(const mcpp::ProviderPtr& provider, const std::string_view path) {
    std::ostringstream output;
    provider->read(path, 0, provider->stat_follow(path, {}).size, output, {});
    return output.str();
}

struct Fixture {
    fs::path mount;
    fs::path working;
    fs::path log;
    fs::path configuration;
    mcpp::ProviderPtr local;
    mcpp::ExtfsOpenOptions options;

    [[nodiscard]] mcpp::ExtfsOrigin origin() const {
        return {{local, mount.string(), mount.string()}, {local, working.string(), working.string()}};
    }

    void configure(const std::string_view extra = {}) const {
        put(configuration, "{\"log\":\"" + log.string() + "\",\"nodes\":{"
            "\"folder\":{\"kind\":\"d\"},"
            "\"first\":{\"kind\":\"f\",\"from_working\":\"context.txt\"},"
            "\"folder/child\":{\"kind\":\"f\",\"data\":\"nested\\n\"},"
            "\"alias\":{\"kind\":\"l\",\"target\":\"/first\"}}" + std::string(extra) + "}");
    }
};

Fixture fixture(const fs::path& root, const fs::path& helper_fixture) {
    Fixture result;
    result.mount = root / "mount space";
    result.working = root / "working ' one";
    result.log = root / "helper-calls.jsonl";
    const auto helpers = root / "helpers";
    fs::create_directory(result.mount);
    fs::create_directory(result.working);
    fs::create_directory(helpers);
    put(result.mount / "context.txt", "WRONG-MOUNT\n");
    put(result.working / "context.txt", "CONTEXT-ONE\n");
    fs::copy_file(helper_fixture, helpers / "localprobe+");
    fs::permissions(helpers / "localprobe+", fs::perms::owner_all);
    result.configuration = helpers / "localprobe+.json";
    result.local = std::make_shared<mcpp::LocalProvider>(root);
    result.options = {.cache_directory=root / "private-cache", .helper_directories={helpers}};
    result.configure();
    return result;
}

void origin_case(const fs::path& root, const fs::path& helper, const fs::path& container_helper) {
    auto test = fixture(root, helper);
    mcpp::ProviderPtr held;
    const std::weak_ptr<mcpp::Provider> namespace_lifetime = test.local;
    {
        mcpp::ExtfsCache cache(std::chrono::seconds(0), test.options);
        held = cache.open_source_less(test.origin(), "localprobe");
        const auto* external = dynamic_cast<const mcpp::ExtfsProvider*>(held.get());
        check(external != nullptr && external->is_source_less(), "source-less helper has an explicit origin type");
        const auto origin = external->origin();
        check(origin && origin->mount.provider == test.local && origin->mount.path == test.mount &&
              origin->working.provider == test.local && origin->working.path == test.working,
              "mount and execution directories retain their real provider/path pairs");
        rejects([&] { static_cast<void>(external->source()); }, "not a container source");
        check(external->location("/folder/child") == test.mount.string() + "/localprobe://folder/child",
              "native URI uses the actual mount directory, not a fake source file");
        check(held->capabilities() == mcpp::ProviderCapability::read, "source-less helper is always read-only");
        check(read(held, "/alias") == "CONTEXT-ONE\n", "virtual absolute link resolves inside the helper namespace");
        const auto calls = get(test.log);
        check(calls.find("\"argv\": [\"list\"]") != std::string::npos &&
              calls.find("\"copyout\", \"" + test.mount.string() + "\", \"first\"") != std::string::npos &&
              calls.find("\"cwd\": \"" + test.working.string() + "\"") != std::string::npos,
              "list has no source argument; copyout gets the real directory; cwd is independently captured");
        put(test.working / "context.txt", "CHANGED-ONE\n");
        put(test.working / "unrelated", "directory mtime is not a container revision\n");
        check(cache.open_source_less(test.origin(), "localprobe") == held,
              "directory contents do not invalidate a retained source-less namespace");
        check(read(held, "/first") == "CONTEXT-ONE\n", "copied-out bytes are retained, not silently refreshed");
        check(read(held, "/folder/child") == "nested\n", "full nested member paths reach the helper");
        auto other = test.origin();
        const auto second = root / "working two";
        fs::create_directory(second);
        put(second / "context.txt", "CONTEXT-TWO\n");
        other.working = {test.local, second.string(), second.string()};
        const auto independent = cache.open_source_less(other, "localprobe");
        check(independent != held && independent->id() != held->id() &&
              read(independent, "/first") == "CONTEXT-TWO\n", "same URI with a different cwd has an independent namespace");
        auto another_namespace = test.origin();
        another_namespace.mount.provider = std::make_shared<mcpp::LocalProvider>(root);
        check(cache.open_source_less(another_namespace, "localprobe") != held,
              "equal provider IDs do not conflate distinct provider instances");

        const auto source = root / "stable-container";
        const auto helpers = test.options.helper_directories.front();
        fs::copy_file(container_helper, helpers / "containerprobe");
        fs::permissions(helpers / "containerprobe", fs::perms::owner_all);
        put(source, R"({"nodes":{"file":{"kind":"f","data":"old"}}})");
        const auto container = cache.open(test.local, source.string(), source.string(), "containerprobe");
        const auto revision = container->stat("/file", {}).revision;
        std::istringstream input("published");
        container->write("/file", input, 9U, true, revision, {}, {});
        check(read(container, "/file") == "published", "source-based guarded publication still works in a mixed cache");
        std::istringstream stale("stale");
        rejects([&] { container->write("/file", stale, 5U, true, revision, {}, {}); }, "member changed");
        check(read(held, "/first") == "CONTEXT-ONE\n", "container writes do not change a retained source-less view");
        cache.prune();
    }
    test.local.reset();
    check(!namespace_lifetime.expired() && read(held, "/first") == "CONTEXT-ONE\n",
          "origin provider and cached bytes outlive the opening cache");
    held.reset();
    check(namespace_lifetime.expired() && fs::is_empty(test.options.cache_directory),
          "final release cleans contexts without leaving private files");
}

void errors_case(const fs::path& root, const fs::path& helper) {
    auto test = fixture(root, helper);
    const auto open = [&] {
        mcpp::ExtfsCache cache(std::chrono::seconds(0), test.options);
        return cache.open_source_less(test.origin(), "localprobe");
    };
    const auto held = open();
    const auto before = get(test.log);
    std::istringstream input("must not be consumed");
    rejects([&] { held->write("/new", input, {}, true, {}, {}, {}); }, "source-less helpers are read-only");
    rejects([&] { held->create_directory("/new"); }, "source-less helpers are read-only");
    rejects([&] { held->remove("/first", false, {}, {}); }, "source-less helpers are read-only");
    check(input.tellg() == 0 && get(test.log) == before, "unsupported mutations neither consume input nor run a helper");
    auto invalid = test.origin();
    invalid.mount.path = (test.mount / "context.txt").string();
    mcpp::ExtfsCache cache(std::chrono::seconds(0), test.options);
    rejects([&] { static_cast<void>(cache.open_source_less(invalid, "localprobe")); }, "not a directory");
    invalid = test.origin();
    invalid.working = {held, "/folder", "virtual/folder"};
    rejects([&] { static_cast<void>(cache.open_source_less(invalid, "localprobe")); }, "must be local directories");
    rejects([&] { static_cast<void>(cache.open_source_less(test.origin(), "localprobe+")); }, "trailing '+'");
    test.configure(",\"listing\":\"-r--r--r-- 1 1001 1001 1 Jan 02 2024 ../escape\\n\"");
    rejects([&] { static_cast<void>(open()); }, "listing line");
    test.configure(",\"unsupported\":[\"copyout\"]");
    const auto failed = open();
    rejects([&] { static_cast<void>(read(failed, "/first")); }, "unsupported (7)");
    const auto marker = root / "host-marker";
    put(marker, "HOST CONTENT MUST STAY PRIVATE");
    test.configure(",\"unsafe_copyout\":\"" + marker.string() + "\"");
    const auto unsafe = open();
    rejects([&] { static_cast<void>(read(unsafe, "/first")); }, "Too many levels");
    check(get(marker) == "HOST CONTENT MUST STAY PRIVATE", "unsafe output cannot expose or change its host target");
    test.configure();
    test.options.limits.cached_member_bytes = 12U;
    const auto bounded = open();
    check(read(bounded, "/first") == "CONTEXT-ONE\n", "first member fits the bounded cache");
    rejects([&] { static_cast<void>(read(bounded, "/folder/child")); }, "member cache limit");
    check(read(bounded, "/first") == "CONTEXT-ONE\n", "cache pressure cannot replace retained member bytes");
    test.options.limits.cached_member_bytes = 128U * 1024U * 1024U;
    const auto replaced = open();
    check(read(replaced, "/first") == "CONTEXT-ONE\n", "capture one member before directory replacement");
    fs::rename(test.working, root / "old working");
    fs::create_directory(test.working);
    put(test.working / "context.txt", "NEW-ORIGIN\n");
    check(read(replaced, "/first") == "CONTEXT-ONE\n", "cached data outlives origin replacement");
    rejects([&] { static_cast<void>(read(replaced, "/folder/child")); }, "directory origin changed");
    check(read(open(), "/first") == "NEW-ORIGIN\n", "reopening captures the new directory rather than reusing the old context");
    mcpp::Cancellation cancelled;
    cancelled.request();
    rejects([&] { static_cast<void>(cache.open_source_less(test.origin(), "localprobe", cancelled)); }, "cancelled");
    check(fs::is_empty(test.options.cache_directory), "error paths clean private copyout files");
}

}  // namespace

int main(const int argc, char** argv) {
    try {
        if (::geteuid() != 1001 || !fs::is_regular_file("/.dockerenv") ||
            !fs::is_regular_file("/review/.operation-regressions-disposable")) {
            throw std::runtime_error("source-less extfs tests require the owned non-root disposable container");
        }
        if (argc != 5) throw std::runtime_error("expected origin|errors, unique case base, source-less and container fixtures");
        const std::string mode(argv[1]);
        if (mode != "origin" && mode != "errors") throw std::runtime_error("unknown source-less extfs case");
        const auto base = fs::absolute(argv[2]).lexically_normal();
        if (base != fs::path("/review/cases") / ("extfs-local-sourceless-" + mode)) {
            throw std::runtime_error("invalid source-less fixture base");
        }
        fs::create_directories(base);
        struct stat state{};
        if (::lstat(base.c_str(), &state) != 0 || !S_ISDIR(state.st_mode) || state.st_uid != ::geteuid()) {
            throw std::runtime_error("source-less fixture base must be owned and not a symlink");
        }
        const auto root = base / ("run-" + std::to_string(::getpid()));
        if (!fs::create_directory(root)) throw std::runtime_error("fixture run already exists");
        if (mode == "origin") origin_case(root, argv[3], argv[4]);
        else errors_case(root, argv[3]);
        fs::remove_all(root);
        std::cout << "source-less extfs " << mode << " passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
