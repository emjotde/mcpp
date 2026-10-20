#include "mcpp/archive_provider.hpp"
#include "mcpp/local_provider.hpp"
#include "mcpp/local_tools.hpp"

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
using Format = mcpp::ArchiveFormat;

void check(const bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

template<class Function>
void rejects(Function&& function, const std::string& message) {
    try { function(); }
    catch (const std::exception&) { return; }
    throw std::runtime_error(message);
}

void put(const fs::path& path, const std::string_view bytes) {
    std::ofstream output(path, std::ios::binary);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    check(static_cast<bool>(output), "write fixture");
}

std::string read(mcpp::Provider& provider, const std::string_view path,
                 const std::uint64_t offset = 0, const std::uint64_t length = 64) {
    std::ostringstream output;
    provider.read(path, offset, length, output, {});
    return output.str();
}

void write(mcpp::ArchiveProvider& provider, const std::string_view path, const std::string& bytes,
           const bool overwrite = false) {
    std::istringstream input(bytes);
    provider.write(path, input, bytes.size(), overwrite, {}, {}, {});
}

mcpp::ArchiveOpenOptions options(const bool writable = false) {
    mcpp::ArchiveOpenOptions result;
    result.writable = writable;
    result.draft_directory = "drafts";
    return result;
}

std::string number(const std::uint64_t value, const int base, const std::size_t width) {
    std::ostringstream stream;
    if (base == 8) stream << std::oct;
    else stream << std::hex;
    stream << value;
    const auto text = stream.str();
    check(text.size() <= width, "fixture field width");
    return std::string(width - text.size(), '0') + text;
}

std::string tar_header(const std::string& name, const char type, const std::uint64_t size,
                       const std::string& target = {}) {
    std::string result(512, '\0');
    result.replace(0, name.size(), name);
    result.replace(100, 7, number(type == '5' ? 0750 : 0640, 8, 7));
    result.replace(108, 7, number(1001, 8, 7));
    result.replace(116, 7, number(1001, 8, 7));
    result.replace(124, 11, number(size, 8, 11));
    result.replace(136, 11, number(1700000000, 8, 11));
    result.replace(148, 8, 8, ' ');
    result[156] = type;
    result.replace(157, target.size(), target);
    result.replace(257, 6, "ustar\0", 6);
    result.replace(263, 2, "00");
    result.replace(329, 7, number(type == '3' ? 1U : type == '4' ? 8U : 0U, 8, 7));
    result.replace(337, 7, number(type == '3' ? 3U : 0U, 8, 7));
    std::uint64_t sum = 0;
    for (const unsigned char byte : result) sum += byte;
    result.replace(148, 6, number(sum, 8, 6));
    result[154] = '\0';
    result[155] = ' ';
    return result;
}

std::string tar_record(const std::string& name, const std::string& body, const char type = '0',
                       const std::string& target = {}) {
    return tar_header(name, type, body.size(), target) + body + std::string((512U - body.size() % 512U) % 512U, '\0');
}

std::string tiny_tar(const std::string& body) {
    return tar_record("payload.txt", body) + tar_record("linked.txt", {}, '1', "payload.txt") +
           tar_record("fifo", {}, '6') + tar_record("character", {}, '3') +
           tar_record("block", {}, '4') + std::string(1024, '\0');
}

std::string cpio_fixture(const Format format) {
    std::string result;
    const bool modern = format == Format::cpio_newc || format == Format::cpio_crc;
    const bool odc = format == Format::cpio_odc;
    const auto member = [&](const std::string& name, const std::string& body, const std::uint32_t mode,
                            const std::uint32_t inode, const std::uint32_t links) {
        const std::uint32_t time = 1700000000;
        const auto checksum = [&] {
            std::uint32_t sum = 0;
            if (format == Format::cpio_crc) for (const unsigned char byte : body) sum += byte;
            return sum;
        }();
        if (modern) {
            result += format == Format::cpio_crc ? "070702" : "070701";
            for (const auto value : std::array<std::uint64_t, 13>{
                inode, mode, 1001, 1001, links, time, body.size(), 0, 0, 0, 0, name.size() + 1U, checksum
            }) result += number(value, 16, 8);
        } else if (odc) {
            result += "070707";
            const std::array<std::uint64_t, 10> fields{
                0, inode, mode, 1001, 1001, links, 0, time, name.size() + 1U, body.size()
            };
            for (std::size_t index = 0; index < fields.size(); ++index) {
                result += number(fields[index], 8, index == 7U || index == 9U ? 11U : 6U);
            }
        } else {
            const std::array<std::uint64_t, 13> fields{
                070707, 0, inode, mode, 1001, 1001, links, 0, time >> 16U, time & 65535U,
                name.size() + 1U, body.size() >> 16U, body.size() & 65535U
            };
            for (const auto value : fields) {
                const auto low = static_cast<char>(value & 255U);
                const auto high = static_cast<char>(value >> 8U);
                result += format == Format::cpio_binary_le ? std::string{low, high} : std::string{high, low};
            }
        }
        result += name;
        result += '\0';
        const auto alignment = modern ? 4U : odc ? 1U : 2U;
        result.append((alignment - result.size() % alignment) % alignment, '\0');
        result += body;
        result.append((alignment - result.size() % alignment) % alignment, '\0');
    };
    member("first", "legacy payload\n", S_IFREG | 0640U, 7, 2);
    member("second", modern ? "" : "legacy payload\n", S_IFREG | 0640U, 7, 2);
    member("fifo", {}, S_IFIFO | 0640U, 8, 1);
    member("socket", {}, S_IFSOCK | 0600U, 9, 1);
    member("TRAILER!!!", {}, 0, 0, 1);
    result.append((512U - result.size() % 512U) % 512U, '\0');
    return result;
}

void formats() {
    fs::create_directory("source");
    constexpr std::uint64_t logical = (1ULL << 32U) + 17U;
    {
        std::ofstream sparse("source/sparse", std::ios::binary);
        sparse.write("HEAD", 4);
        sparse.seekp(static_cast<std::streamoff>(logical - 4U));
        sparse.write("TAIL", 4);
        sparse.close();
        check(static_cast<bool>(sparse), "create sparse logical boundary fixture");
    }
    fs::permissions("source/sparse", fs::perms{0640});
    fs::create_hard_link("source/sparse", "source/linked");
    check(::mkfifo("source/fifo", 0640) == 0, "create private FIFO fixture");
    mcpp::LocalProvider local(fs::current_path());
    for (const auto* format : {"gnu", "0.0", "0.1", "1.0"}) {
        const auto filename = "sparse-" + std::string(format) + ".tar";
        std::vector<std::string> arguments{"/usr/bin/tar", "--sparse", "--hole-detection=seek"};
        if (std::string_view(format) == "gnu") arguments.push_back("--format=gnu");
        else {
            arguments.push_back("--format=pax");
            arguments.push_back("--sparse-version=" + std::string(format));
        }
        arguments.insert(arguments.end(), {"-cf", filename, "-C", "source", "sparse", "linked", "fifo"});
        const auto command = mcpp::capture_command(arguments, fs::current_path());
        check(command.status == 0, "create native GNU sparse fixture: " + command.error);
        check(fs::file_size(filename) < 64U * 1024U, "fixture contains only a few stored blocks");
        mcpp::ArchiveProvider archive(filename, options(true));
        check(archive.stat("/sparse", {}).size == logical && read(archive, "/sparse", logical - 4U, 4) == "TAIL" &&
              read(archive, "/sparse", 0, 4) == "HEAD" && read(archive, "/sparse", 1024, 8) == std::string(8, '\0'),
              "GNU sparse logical size, data and hole semantics");
        check(archive.stat("/sparse", {}).inode == archive.stat("/linked", {}).inode &&
              archive.hardlink_count("/sparse") == 2U, "sparse hardlink identity");
        check(archive.stat("/fifo", {}).kind == mcpp::EntryKind::other &&
              S_ISFIFO(archive.stat("/fifo", {}).mode), "FIFO remains a FIFO, not a regular file");
        rejects([&] { static_cast<void>(read(archive, "/fifo")); }, "FIFO read must fail without blocking");
        archive.set_permissions("/sparse", fs::perms{0640});
        mcpp::ArchiveProvider reopened(filename, options());
        check(read(reopened, "/linked", logical - 4U, 4) == "TAIL" &&
              reopened.hardlink_count("/sparse") == 2U, "sparse publication keeps content and hardlinks");
        if (std::string_view(format) == "gnu") {
            const auto listing = mcpp::capture_command({"/usr/bin/tar", "-tvf", filename}, fs::current_path());
            check(listing.status == 0 && listing.output.find(std::to_string(logical)) != std::string::npos,
                  "GNU tar independently reads the published sparse format: " + listing.error);
            check(reopened.copy_to_local("/sparse", local, "export-sparse", false, true, {}, {}), "sparse local export");
            struct stat state{};
            check(::stat("export-sparse", &state) == 0 && static_cast<std::uint64_t>(state.st_size) == logical &&
                  state.st_blocks < 256, "sparse export does not allocate or scan its holes");
            check(reopened.copy_to_local("/fifo", local, "export-fifo", false, true, {}, {}), "FIFO local export");
            check(fs::is_fifo(fs::symlink_status("export-fifo")), "FIFO export recreates the type without reading");
        }
    }
    for (const auto format : {Format::cpio_odc, Format::cpio_binary_le, Format::cpio_binary_be,
                              Format::cpio_newc, Format::cpio_crc}) {
        const auto filename = "legacy-" + std::to_string(static_cast<int>(format)) + ".cpio";
        put(filename, cpio_fixture(format));
        mcpp::ArchiveProvider archive(filename, options(true));
        check(archive.format() == format && read(archive, "/second") == "legacy payload\n" &&
              archive.hardlink_count("/first") == 2U, "cpio framing and hardlink content");
        check(S_ISFIFO(archive.stat("/fifo", {}).mode) && S_ISSOCK(archive.stat("/socket", {}).mode) &&
              archive.stat("/socket", {}).kind == mcpp::EntryKind::other, "cpio special kinds");
        rejects([&] { static_cast<void>(read(archive, "/socket")); }, "socket read must be explicit");
        put("socket-destination", "keep");
        rejects([&] { archive.copy_to_local("/socket", local, "socket-destination", true, true, {}, {}); },
                "socket copy must be explicit");
        check(fs::file_size("socket-destination") == 4U, "socket failure leaves destination intact");
        archive.set_permissions("/first", fs::perms{0600});
        mcpp::ArchiveProvider reopened(filename, options());
        check(reopened.format() == format && reopened.stat("/second", {}).permissions == fs::perms{0600} &&
              reopened.hardlink_count("/first") == 2U && read(reopened, "/first") == "legacy payload\n" &&
              S_ISSOCK(reopened.stat("/socket", {}).mode), "old cpio writable roundtrip preserves format and kinds");
    }
    put("devices.tar", tiny_tar("small\n"));
    mcpp::ArchiveProvider specials("devices.tar", options());
    check(specials.stat("/character", {}).kind == mcpp::EntryKind::other &&
          S_ISCHR(specials.stat("/character", {}).mode) && S_ISBLK(specials.stat("/block", {}).mode),
          "device headers retain their real types without creating host devices");
    std::cout << "PASS formats: GNU sparse, V7/odc/newc/CRC cpio, safe special entries\n";
}

void snapshots() {
    const auto inner = tiny_tar("old immutable payload\n");
    put("outer.tar", tar_record("inner.tar", inner) + std::string(1024, '\0'));
    auto outer = mcpp::open_archive_provider("outer.tar", options(true));
    auto nested = mcpp::open_archive_provider(outer, "/inner.tar", options());
    const auto identity = nested->id();
    check(nested->archive_path().empty() && nested->source_provider() == outer &&
          nested->source_path() == "/inner.tar" && read(*nested, "/payload.txt") == "old immutable payload\n",
          "nested archive captures its actual provider and path");
    rejects([&] { static_cast<void>(mcpp::open_archive_provider(outer, "/inner.tar", options(true))); },
            "nested archives must be read-only");
    write(*outer, "/inner.tar", tiny_tar("new immutable payload\n"), true);
    auto fresh = mcpp::open_archive_provider(outer, "/inner.tar", options());
    check(fresh->id() != identity && read(*fresh, "/linked.txt") == "new immutable payload\n" &&
          read(*nested, "/linked.txt") == "old immutable payload\n", "new containers open fresh snapshots, old views stay unchanged");
    outer.reset();
    fresh.reset();
    check(read(*nested, "/payload.txt") == "old immutable payload\n", "nested snapshot retains its lifetime");
    constexpr std::uint64_t size = 64U * 1024U * 1024U + 17U;
    {
        std::ofstream raw("large.tar", std::ios::binary);
        const auto header = tar_header("large", '0', size);
        raw.write(header.data(), static_cast<std::streamsize>(header.size()));
        raw.write("HEAD", 4);
        raw.seekp(static_cast<std::streamoff>(512U + size - 4U));
        raw.write("TAIL", 4);
        const std::string end(static_cast<std::size_t>((512U - size % 512U) % 512U) + 1024U, '\0');
        raw.write(end.data(), static_cast<std::streamsize>(end.size()));
    }
    mcpp::ArchiveProvider large("large.tar", options());
    check(large.stat("/large", {}).size == size && read(large, "/large", size - 4U, 4) == "TAIL" &&
          read(large, "/large", 0, 4) == "HEAD", "encoded archives beyond the former RAM cap remain range readable");
    auto limited = options();
    limited.limits.archive_bytes = 1024;
    rejects([&] { mcpp::ArchiveProvider denied("large.tar", limited); }, "explicit physical byte limits remain effective");
    put("compressed.tar", tiny_tar("compressed payload\n"));
    const auto gzip = mcpp::capture_command({"/usr/bin/gzip", "-nc", "compressed.tar"}, fs::current_path());
    check(gzip.status == 0, "create tiny compressed fixture");
    put("compressed.tar.gz", gzip.output);
    mcpp::ArchiveProvider compressed("compressed.tar.gz", options(true));
    check(read(compressed, "/linked.txt") == "compressed payload\n", "streamed decompression preserves content");
    write(compressed, "/new", "streamed publication");
    mcpp::ArchiveProvider compressed_fresh("compressed.tar.gz", options());
    check(read(compressed_fresh, "/new") == "streamed publication" &&
          compressed_fresh.compression() == mcpp::ArchiveCompression::gzip, "streamed compression publication roundtrip");
    limited = options();
    limited.limits.expanded_bytes = 1024;
    rejects([&] { mcpp::ArchiveProvider denied("compressed.tar.gz", limited); }, "streamed decoder enforces explicit expansion limits");
    mcpp::Cancellation stop;
    stop.request();
    rejects([&] { static_cast<void>(read(*nested, "/missing")); }, "missing members remain confined");
    bool cancelled = false;
    try { std::ostringstream output; nested->read("/payload.txt", 0, 4, output, stop); }
    catch (const mcpp::OperationCancelled&) { cancelled = true; }
    check(cancelled, "range reads remain cancellable");
    put("conflict.tar", tiny_tar("original\n"));
    mcpp::ArchiveProvider conflict("conflict.tar", options(true));
    put("conflict.tar", tiny_tar("external\n"));
    rejects([&] { write(conflict, "/new", "private recovery"); }, "changed source must reject publication");
    check(conflict.last_draft().has_value() && read(conflict, "/payload.txt") == "original\n",
          "failed publication keeps its original immutable view and private draft");
    const auto state = fs::status(conflict.last_draft()->parent_path());
    check((state.permissions() & fs::perms{0777}) == fs::perms{0700}, "recovery draft directory remains private");
    mcpp::ArchiveProvider recovery(*conflict.last_draft(), options());
    check(read(recovery, "/new") == "private recovery", "retained streamed draft is a usable archive");
    std::cout << "PASS snapshots: nested identity/lifetime, large sparse source, bounded/cancelled/conflicting publication\n";
}
}  // namespace

int main(const int argc, char** argv) {
    try {
        if (::geteuid() != 1001 || !fs::is_regular_file("/review/.operation-regressions-disposable") ||
            argc < 3) throw std::runtime_error("use the owned UID1001 review container and focused selectors");
        const auto base = fs::absolute(argv[1]).lexically_normal();
        if (base.parent_path() != "/review/cases" || !base.filename().string().starts_with("archive-complete-") ||
            fs::is_symlink(fs::symlink_status(base))) throw std::runtime_error("unexpected archive test workspace");
        fs::create_directory(base);
        const auto root = base / ("run-" + std::to_string(::getpid()));
        check(fs::create_directory(root), "unique archive test workspace");
        fs::current_path(root);
        for (int index = 2; index < argc; ++index) {
            if (std::string_view(argv[index]) == "formats") formats();
            else if (std::string_view(argv[index]) == "snapshots") snapshots();
            else throw std::runtime_error("unknown focused archive selector");
        }
        fs::current_path(base);
        fs::remove_all(root);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL archive-complete: " << error.what() << '\n';
        return 1;
    }
}
