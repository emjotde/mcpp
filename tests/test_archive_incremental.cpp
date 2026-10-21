#include "mcpp/archive_provider.hpp"
#include "mcpp/local_provider.hpp"
#include "mcpp/local_tools.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {
namespace fs = std::filesystem;
using namespace std::string_literals;

void check(const bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

template<class Function>
void rejects(Function&& function, const std::string_view reason) {
    try {
        function();
    } catch (const mcpp::OperationCancelled&) {
        throw;
    } catch (const std::exception& error) {
        check(std::string_view(error.what()).find(reason) != std::string_view::npos,
              "unexpected rejection: " + std::string(error.what()));
        return;
    }
    throw std::runtime_error("expected rejection: " + std::string(reason));
}

template<class Function>
void cancelled(Function&& function) {
    try {
        function();
    } catch (const mcpp::OperationCancelled&) {
        return;
    }
    throw std::runtime_error("expected cancellation");
}

void put(const fs::path& path, const std::string_view bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    check(static_cast<bool>(output), "write fixture: " + path.string());
}

std::string get(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    check(static_cast<bool>(input), "open fixture: " + path.string());
    std::string result{std::istreambuf_iterator<char>(input), {}};
    check(!input.bad(), "read fixture: " + path.string());
    return result;
}

mcpp::ArchiveOpenOptions options(const bool writable = false) {
    mcpp::ArchiveOpenOptions result;
    result.writable = writable;
    result.draft_directory = "drafts";
    return result;
}

std::string read(mcpp::Provider& provider, const std::string_view path,
                 const std::uint64_t offset = 0,
                 const std::uint64_t length = std::numeric_limits<std::uint64_t>::max(),
                 const mcpp::Cancellation& cancellation = {}) {
    std::ostringstream output;
    provider.read(path, offset, length, output, cancellation);
    return output.str();
}

void octal(std::string& header, const std::size_t offset, const std::size_t width,
           const std::uint64_t value) {
    std::ostringstream text;
    text << std::oct << value;
    const auto digits = text.str();
    check(digits.size() < width, "fixture number too large");
    header.replace(offset, width, std::string(width - digits.size() - 1U, '0') + digits + '\0');
}

void checksum(std::string& header) {
    header.replace(148, 8, 8, ' ');
    std::uint64_t sum = 0;
    for (const unsigned char byte : std::string_view(header).substr(0, 512)) sum += byte;
    octal(header, 148, 7, sum);
    header[155] = ' ';
}

std::string record(const std::string& name, const char type, const std::string& body,
                   const std::string& target = {}) {
    check(name.size() <= 100U && target.size() <= 100U, "fixture name too large");
    std::string header(512, '\0');
    header.replace(0, name.size(), name);
    octal(header, 100, 8, type == 'D' || type == '5' ? 0750 : 0640);
    octal(header, 108, 8, 1001);
    octal(header, 116, 8, 1001);
    octal(header, 124, 12, body.size());
    octal(header, 136, 12, 1700000000);
    header[156] = type;
    header.replace(157, target.size(), target);
    header.replace(257, 6, "ustar ", 6);
    header.replace(263, 2, " \0", 2);
    octal(header, 345, 12, 1700000001);
    octal(header, 357, 12, 1700000002);
    checksum(header);
    return header + body + std::string((512U - body.size() % 512U) % 512U, '\0');
}

std::string pax(const std::string& key, const std::string& value) {
    const auto body = " " + key + "=" + value + "\n";
    auto length = body.size() + 1U;
    while (length != body.size() + std::to_string(length).size()) {
        length = body.size() + std::to_string(length).size();
    }
    return std::to_string(length) + body;
}

const std::string payload = "GNU incremental payload\nsecond line\n";
const std::string binary = "zero\0one\xff\n"s;
const std::string terminator(1024, '\0');

void make_source() {
    fs::create_directories("source/tree/empty");
    fs::create_directory("source/tree/sub");
    put("source/tree/payload.txt", payload);
    put("source/tree/sub/binary", binary);
    put("source/tree/line\n=name", "literal name\n");
    fs::permissions("source/tree", fs::perms{0750});
    fs::permissions("source/tree/empty", fs::perms{0710});
    fs::permissions("source/tree/payload.txt", fs::perms{0640});
    fs::create_hard_link("source/tree/payload.txt", "source/tree/hard");
    fs::create_symlink("payload.txt", "source/tree/symbolic");
}

void make_incremental(const std::string& format, const std::string& archive, const std::string& snapshot) {
    const auto result = mcpp::capture_command(
        {"/usr/bin/tar", "--format=" + format, "--listed-incremental=" + snapshot,
         "-cf", archive, "-C", "source", "tree"}, fs::current_path());
    check(result.status == 0, "generate GNU incremental fixture: " + result.error);
    check(fs::file_size(archive) <= 20U * 1024U, "incremental fixture remains tiny");
}

void browse() {
    make_source();
    mcpp::LocalProvider local(fs::current_path());
    for (const auto* format : {"gnu", "pax"}) {
        const auto filename = std::string(format) + ".tar";
        make_incremental(format, filename, std::string(format) + ".snar");
        const auto original = get(filename);
        mcpp::ArchiveProvider archive(filename, options());
        const auto directory = archive.stat("/tree", {});
        check(directory.kind == mcpp::EntryKind::directory && S_ISDIR(directory.mode) &&
              directory.permissions == fs::perms{0750} && directory.size == 0U &&
              directory.uid == 1001U && directory.gid == 1001U, "incremental directory metadata");
        check(archive.stat("/tree/empty", {}).permissions == fs::perms{0710} &&
              archive.list("/tree/empty", {}, {}).empty(), "empty directory remains a directory");
        const auto entries = archive.list("/tree", {}, {});
        check(entries.size() == 6U &&
              std::any_of(entries.begin(), entries.end(), [](const auto& entry) {
                  return entry.name == "sub" && entry.path == "/tree/sub" &&
                         entry.kind == mcpp::EntryKind::directory;
              }), "ordinary members, not directory dump records, form the listing");
        check(read(archive, "/tree/payload.txt") == payload && read(archive, "/tree/hard") == payload &&
              read(archive, "/tree/symbolic") == payload && read(archive, "/tree/sub/binary") == binary &&
              read(archive, "/tree/line\n=name") == "literal name\n", "regular and linked bytes remain exact");
        check(read(archive, "/tree/sub/binary", 3, 5) == binary.substr(3, 5) &&
              read(archive, "/tree/sub/binary", std::numeric_limits<std::uint64_t>::max()).empty(),
              "range reads retain their bounds");
        check(archive.stat("/tree/hard", {}).inode == archive.stat("/tree/payload.txt", {}).inode &&
              archive.hardlink_count("/tree/hard") == 2U &&
              archive.stat("/tree/symbolic", {}).kind == mcpp::EntryKind::symlink &&
              archive.stat("/tree/symbolic", {}).target == "payload.txt", "link identity and target survive");
        check(archive.parent("/tree/sub") == "/tree" && archive.join("/tree", "empty") == "/tree/empty" &&
              archive.stat_follow("/tree/symbolic", {}).kind == mcpp::EntryKind::file, "virtual navigation");
        rejects([&] { static_cast<void>(read(archive, "/tree")); }, "archive member");
        check(!mcpp::has_capability(archive.capabilities(), mcpp::ProviderCapability::write), "read-only capability");
        rejects([&] { mcpp::ArchiveProvider denied(filename, options(true)); }, "incremental archives are read-only");
        check(get(filename) == original && (!fs::exists("drafts") || fs::is_empty("drafts")),
              "unsupported rewrite does not drop restore metadata");
        const auto destination = std::string(format) + "-export";
        check(archive.copy_to_local("/tree/hard", local, destination, false, true, {}, {},
                                   archive.stat("/tree/hard", {}).revision), "incremental regular-file export");
        check(get(destination) == payload && fs::status(destination).permissions() == fs::perms{0640},
              "export has member bytes and permissions, not dump metadata");
    }
    fs::rename("source/tree/sub", "source/tree/renamed");
    put("source/tree/changed.txt", "new delta bytes\n");
    make_incremental("gnu", "delta.tar", "gnu.snar");
    const auto delta_bytes = get("delta.tar");
    check(delta_bytes.find("Npayload.txt\0"s) != std::string::npos &&
          delta_bytes.find("Rtree/sub\0Ttree/renamed\0"s) != std::string::npos,
          "native GNU tar generated omitted names and rename instructions");
    mcpp::ArchiveProvider delta("delta.tar", options());
    check(read(delta, "/tree/changed.txt") == "new delta bytes\n" &&
          !delta.exists("/tree/payload.txt") && !delta.exists("/tree/sub") &&
          delta.stat("/tree/renamed", {}).kind == mcpp::EntryKind::directory,
          "restore instructions do not fabricate omitted members or replay renames");
    std::cout << "PASS incremental browse: GNU D/PAX dumpdir, paths, bytes, links, delta metadata, read-only export\n";
}

void guards() {
    const auto dump = "Nabsent\0Ypresent\0Dempty\0X/ignored\0R/ignored/from\0T\0R\0T/ignored/to\0\0"s;
    const auto tail = record("tree/present", '0', binary) + record("tree/empty/", 'D', "\0"s) +
                      record("tree/link", '2', {}, "present") + record("tree/hard", '1', {}, "tree/present") +
                      terminator;
    const auto original = record("tree/", 'D', dump) + tail;
    put("guard.tar", original);
    put("sentinel", "untouched\n");
    auto archive = mcpp::open_archive_provider("guard.tar", options());
    check(archive->stat("/tree", {}).accessed == std::chrono::system_clock::time_point{std::chrono::seconds{1700000001}} &&
          archive->stat("/tree", {}).changed == std::chrono::system_clock::time_point{std::chrono::seconds{1700000002}} &&
          !archive->exists("/tree/absent") && get("sentinel") == "untouched\n", "restore metadata is not executed");
    mcpp::Cancellation stop;
    stop.request();
    cancelled([&] { static_cast<void>(read(*archive, "/tree/present", 0, 1, stop)); });
    cancelled([&] { static_cast<void>(archive->list("/tree", {}, stop)); });
    cancelled([&] { mcpp::ArchiveProvider denied("guard.tar", options(), stop); });
    mcpp::LocalProvider local(fs::current_path());
    rejects([&] { archive->copy_to_local("/tree/present", local, "sentinel", true, true, {}, {}, "stale"); },
            "changed");
    check(get("sentinel") == "untouched\n", "stale source export leaves destination intact");
    auto limited = options();
    limited.limits.archive_bytes = original.size() - 1U;
    rejects([&] { mcpp::ArchiveProvider denied("guard.tar", limited); }, "physical byte limit");
    limited = options();
    limited.limits.expanded_bytes = original.size() - 1U;
    rejects([&] { mcpp::ArchiveProvider denied("guard.tar", limited); }, "expanded byte limit");
    limited = options();
    limited.limits.members = 1;
    rejects([&] { mcpp::ArchiveProvider denied("guard.tar", limited); }, "member limit");
    limited = options();
    limited.limits.path_bytes = 8;
    rejects([&] { mcpp::ArchiveProvider denied("guard.tar", limited); }, "directory dump name");
    put("bad.tar", record("tree/", 'D', "Y" + std::string(511, 'a') + "\0\0"s) + terminator);
    limited = options();
    limited.limits.metadata_bytes = 512;
    rejects([&] { mcpp::ArchiveProvider denied("bad.tar", limited); }, "metadata byte limit");
    put("bad.tar", record("tree/", 'D', "Ypresent\0"s) + tail);
    rejects([&] { mcpp::ArchiveProvider denied("bad.tar", options()); }, "dump terminator");
    put("bad.tar", record("tree/", 'D', "Rsource\0\0"s) + tail);
    rejects([&] { mcpp::ArchiveProvider denied("bad.tar", options()); }, "dump terminator");
    put("bad.tar", record("tree/", 'D', "Y../escape\0\0"s) + tail);
    rejects([&] { mcpp::ArchiveProvider denied("bad.tar", options()); }, "unsafe member path");
    put("bad.tar", record("metadata", 'x', pax("GNU.dumpdir", dump)) + record("plain", '0', "regular") + terminator);
    rejects([&] { mcpp::ArchiveProvider denied("bad.tar", options()); }, "misplaced PAX directory dump");
    put("bad.tar", record("metadata", 'g', pax("GNU.dumpdir", dump)) + tail);
    rejects([&] { mcpp::ArchiveProvider denied("bad.tar", options()); }, "global PAX directory dump");
    put("bad.tar", record("metadata", 'x', pax("path", "tree\0escape"s)) + tail);
    rejects([&] { mcpp::ArchiveProvider denied("bad.tar", options()); }, "NUL in PAX metadata");
    put("bad.tar", record("metadata", 'x', pax("GNU.volume.offset", "1")) + tail);
    rejects([&] { mcpp::ArchiveProvider denied("bad.tar", options()); }, "unsupported PAX metadata");
    auto multivolume = original;
    octal(multivolume, 369, 12, 1);
    checksum(multivolume);
    put("bad.tar", multivolume);
    rejects([&] { mcpp::ArchiveProvider denied("bad.tar", options()); }, "multivolume");
    put("outer.tar", record("inside.tar", '0', original) + terminator);
    auto outer = mcpp::open_archive_provider("outer.tar", options());
    auto nested = mcpp::open_archive_provider(outer, "/inside.tar", options());
    check(nested->archive_path().empty() && nested->source_provider() == outer &&
          nested->source_path() == "/inside.tar" && read(*nested, "/tree/hard") == binary,
          "incremental nested snapshot retains its actual provider and source path");
    const auto identity = archive->id();
    const auto revision = archive->stat("/tree/present", {}).revision;
    put("guard.tar", record("replacement", '0', "new archive") + terminator);
    mcpp::ArchiveProvider fresh("guard.tar", options());
    outer.reset();
    check(archive->id() == identity && archive->stat("/tree/present", {}).revision == revision &&
          read(*archive, "/tree/link") == binary && read(*nested, "/tree/present") == binary &&
          fresh.id() != identity && read(fresh, "/replacement") == "new archive",
          "old incremental snapshots and revisions survive source replacement");
    mcpp::ArchiveProvider writable("guard.tar", options(true));
    const std::string saved = "ordinary tar publication\n";
    std::istringstream input(saved);
    writable.write("/replacement", input, saved.size(), true, writable.stat("/replacement", {}).revision, {}, {});
    mcpp::ArchiveProvider published("guard.tar", options());
    check(read(published, "/replacement") == saved, "ordinary tar publication is still supported");
    const auto external = record("external", '0', "do not overwrite") + terminator;
    put("guard.tar", external);
    std::istringstream pending(saved);
    rejects([&] { writable.write("/new", pending, saved.size(), false, {}, {}, {}); }, "changed since opening");
    check(get("guard.tar") == external && read(writable, "/replacement") == saved && writable.last_draft().has_value(),
          "ordinary publication still guards its captured source and retains a private recovery draft");
    mcpp::ArchiveProvider recovery(*writable.last_draft(), options());
    check(read(recovery, "/new") == saved, "failed ordinary publication retains usable data");
    rejects([&] { static_cast<void>(archive->normalize("/tree/../present")); }, "unsafe member path");
    check(get("sentinel") == "untouched\n", "metadata handling never touches a host restore path");
    std::cout << "PASS incremental guards: framing, bounds, cancellation, unsupported metadata, revision and nested snapshots\n";
}

void volume_label() {
    fs::create_directory("source");
    put("source/payload", payload);
    const auto generated = mcpp::capture_command(
        {"/usr/bin/tar", "--format=gnu", "--label=mcpp L28 label", "-cf", "labeled.tar",
         "-C", "source", "payload"}, fs::current_path());
    check(generated.status == 0, "generate GNU volume-label fixture: " + generated.error);
    const auto original = get("labeled.tar");
    check(original.size() >= 1536U && original[156] == 'V', "GNU tar emitted a volume-label record");

    mcpp::ArchiveProvider archive("labeled.tar", options());
    const auto entries = archive.list("/", {}, {});
    check(entries.size() == 1U && entries.front().name == "payload" &&
          read(archive, "/payload") == payload, "volume label is metadata and member bytes remain exact");

    rejects([&] { mcpp::ArchiveProvider denied("labeled.tar", options(true)); },
            "volume-label archives are read-only");
    check(get("labeled.tar") == original && (!fs::exists("drafts") || fs::is_empty("drafts")),
          "unsupported rewrite leaves the labeled archive unchanged");

    const auto label = original.substr(0, 512);
    put("late-label.tar", record("payload", '0', payload) + label + terminator);
    rejects([&] { mcpp::ArchiveProvider denied("late-label.tar", options()); }, "volume label");

    auto data_label = label;
    octal(data_label, 124, 12, 1);
    checksum(data_label);
    put("data-label.tar", data_label + "x" + std::string(511, '\0') + terminator);
    rejects([&] { mcpp::ArchiveProvider denied("data-label.tar", options()); }, "volume label");
    std::cout << "PASS GNU volume label: native fixture, hidden metadata, read-only guard, placement and size checks\n";
}
}  // namespace

int main(const int argc, char** argv) {
    try {
        check(::geteuid() == 1001 && fs::is_regular_file("/review/.operation-regressions-disposable") && argc == 3,
              "use the owned UID1001 review container and one focused selector");
        const auto base = fs::absolute(argv[1]).lexically_normal();
        check(base.parent_path() == "/review/cases" &&
              (base.filename().string().starts_with("archive-incremental-") ||
               base.filename().string().starts_with("archive-l28-")) &&
              !fs::is_symlink(fs::symlink_status(base)), "unexpected incremental archive test workspace");
        static_cast<void>(::umask(0077));
        fs::create_directory(base);
        const auto root = base / ("run-" + std::to_string(::getpid()));
        check(fs::create_directory(root), "unique incremental test workspace");
        fs::current_path(root);
        if (std::string_view(argv[2]) == "browse") browse();
        else if (std::string_view(argv[2]) == "guards") guards();
        else if (std::string_view(argv[2]) == "volume-label") volume_label();
        else throw std::runtime_error("unknown incremental archive selector");
        fs::current_path(base);
        fs::remove_all(root);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL archive-incremental: " << error.what() << '\n';
        return 1;
    }
}
