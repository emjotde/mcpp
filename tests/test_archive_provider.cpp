#include "mcpp/archive_provider.hpp"
#include "mcpp/local_provider.hpp"
#include "mcpp/local_tools.hpp"
#include "mcpp/operations.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <tuple>
#include <unistd.h>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using namespace std::string_literals;

namespace {

std::size_t checks = 0;

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
    std::string result{std::istreambuf_iterator<char>(input), {}};
    if (input.bad()) throw std::runtime_error("read fixture: " + path.string());
    return result;
}

void octal(std::string& bytes, const std::size_t offset, const std::size_t width, const std::uint64_t number) {
    std::ostringstream text;
    text << std::oct << number;
    const auto value = text.str();
    if (value.size() >= width) throw std::runtime_error("fixture octal overflow");
    bytes.replace(offset, width, std::string(width - value.size() - 1U, '0') + value + '\0');
}

struct TarMember {
    std::string name;
    char type{'0'};
    std::string data;
    std::string target;
};

std::string tar_fixture(const std::vector<TarMember>& members) {
    std::string result;
    for (const auto& member : members) {
        if (member.name.size() > 100U || member.target.size() > 100U) {
            throw std::runtime_error("fixture name too long");
        }
        std::string block(512, '\0');
        block.replace(0, member.name.size(), member.name);
        octal(block, 100, 8, member.type == '5' ? 0755 : member.type == '2' ? 0777 : 0640);
        octal(block, 108, 8, 1001);
        octal(block, 116, 8, 1001);
        octal(block, 124, 12, member.data.size());
        octal(block, 136, 12, 1700000000);
        block.replace(148, 8, 8, ' ');
        block[156] = member.type;
        block.replace(157, member.target.size(), member.target);
        block.replace(257, 6, "ustar\0", 6);
        block.replace(263, 2, "00");
        octal(block, 329, 8, 0);
        octal(block, 337, 8, 0);
        std::uint64_t sum = 0;
        for (const unsigned char byte : block) sum += byte;
        octal(block, 148, 7, sum);
        block[155] = ' ';
        result += block;
        result += member.data;
        result.append((512U - member.data.size() % 512U) % 512U, '\0');
    }
    result.append(1024, '\0');
    return result;
}

std::string pax(const std::string& key, const std::string& value) {
    auto body = " " + key + "=" + value + "\n";
    std::size_t size = body.size();
    while (std::to_string(size).size() + body.size() != size) size = std::to_string(size).size() + body.size();
    return std::to_string(size) + body;
}

struct CpioMember {
    std::string name;
    std::uint32_t mode{S_IFREG | 0640U};
    std::string data;
    std::uint32_t inode{1};
    std::uint32_t links{1};
};

std::string cpio_fixture(const std::vector<CpioMember>& members, const bool crc) {
    std::string result;
    auto record = [&](const CpioMember& member) {
        std::uint32_t sum = 0;
        if (crc) for (const unsigned char byte : member.data) sum += byte;
        const std::array<std::uint64_t, 13> fields{
            member.inode, member.mode, 1001, 1001, member.links, 1700000000,
            member.data.size(), 8, 1, 0, 0, member.name.size() + 1U, sum
        };
        result += crc ? "070702" : "070701";
        for (const auto value : fields) {
            std::ostringstream text;
            text << std::hex << value;
            result += std::string(8U - text.str().size(), '0') + text.str();
        }
        result += member.name;
        result.push_back('\0');
        result.append((4U - result.size() % 4U) % 4U, '\0');
        result += member.data;
        result.append((4U - result.size() % 4U) % 4U, '\0');
    };
    for (const auto& member : members) record(member);
    record({"TRAILER!!!", 0, {}, 0, 1});
    result.append((512U - result.size() % 512U) % 512U, '\0');
    return result;
}

mcpp::ArchiveOpenOptions options(const bool writable = false) {
    mcpp::ArchiveOpenOptions value;
    value.writable = writable;
    value.draft_directory = "drafts";
    return value;
}

std::string read(mcpp::Provider& provider, const std::string& path, const std::uint64_t offset = 0,
                 const std::uint64_t length = std::numeric_limits<std::uint64_t>::max()) {
    std::ostringstream output(std::ios::binary);
    provider.read(path, offset, length, output, {});
    return std::move(output).str();
}

void write(mcpp::Provider& provider, const std::string& path, const std::string& bytes,
           const bool overwrite = false, const std::optional<std::string>& revision = std::nullopt) {
    std::istringstream input(bytes, std::ios::binary);
    provider.write(path, input, bytes.size(), overwrite, revision, {}, {});
}

const std::string binary = "zero\0one\nlast\xff"s;
const std::string odd = "line\n tab\t quote'\" back\\ $; --literal";

std::vector<TarMember> tar_members() {
    return {
        {"folder/", '5', {}, {}},
        {"folder/" + odd, '0', binary, {}},
        {"plain", '0', "original", {}},
        {"folder/hard", '1', {}, "folder/" + odd},
        {"folder/up", '2', {}, "../plain"},
        {"folder-link", '2', {}, "folder"},
        {"dangling", '2', {}, "missing"},
        {".hidden", '0', "hidden", {}}
    };
}

std::vector<CpioMember> cpio_members() {
    return {
        {"plain", S_IFREG | 0640U, {}, 77, 2},
        {"hard", S_IFREG | 0640U, "original", 77, 2},
        {"folder", S_IFDIR | 0755U, {}, 78, 2},
        {"folder/" + odd, S_IFREG | 0600U, binary, 79, 1},
        {"folder/up", S_IFLNK | 0777U, "../plain", 80, 1},
        {"dangling", S_IFLNK | 0777U, "missing", 81, 1}
    };
}

void browse() {
    put("browse.tar", tar_fixture(tar_members()));
    mcpp::ArchiveProvider provider("browse.tar", options());
    check(provider.format() == mcpp::ArchiveFormat::tar, "detect tar");
    check(provider.compression() == mcpp::ArchiveCompression::none, "detect uncompressed");
    check(!mcpp::has_capability(provider.capabilities(), mcpp::ProviderCapability::write), "read-only capabilities");
    check(provider.normalize("") == "/" && provider.normalize("folder") == "/folder", "virtual root normalization");
    check(provider.parent("/") == "/" && provider.parent("/folder/file") == "/folder", "virtual parent");
    check(provider.basename("/folder/" + odd) == odd && provider.join("/folder", odd) == "/folder/" + odd,
          "literal Linux member names");
    check(read(provider, "/folder/" + odd) == binary, "binary read");
    check(read(provider, "/folder/" + odd, 3, 6) == binary.substr(3, 6), "range read");
    check(read(provider, "/plain", std::numeric_limits<std::uint64_t>::max()).empty(), "range beyond EOF");
    check(read(provider, "/plain", 0, 0).empty(), "empty range");
    check(read(provider, "/folder/hard") == binary, "tar hardlink data");
    check(provider.stat("/folder/hard", {}).inode == provider.stat("/folder/" + odd, {}).inode &&
          provider.stat("/folder/hard", {}).links == 2U, "tar hardlink identity");
    check(provider.stat("/folder/up", {}).kind == mcpp::EntryKind::symlink &&
          provider.stat("/folder/up", {}).target == "../plain", "literal symlink target");
    check(read(provider, "/folder/up") == "original", "relative symlink read");
    check(read(provider, "/folder-link/" + odd) == binary, "symlink directory traversal");
    check(provider.stat("/folder-link", {}).target_is_directory, "symlink directory stat");
    check(provider.stat_follow("/folder/up", {}).path == "/plain", "following stat returns resolved path");
    check(provider.exists("/dangling") && !provider.exists("/missing"), "dangling lstat and missing entry");
    rejects([&] { static_cast<void>(provider.stat_follow("/dangling", {})); }, "dangling follow fails");
    check(provider.stat("/folder/" + odd, {}).uid == 1001U &&
          provider.stat("/folder/" + odd, {}).permissions == fs::perms{0640}, "tar stat metadata");
    const auto visible = provider.list("/", {}, {});
    const auto all = provider.list("/", {.show_hidden = true}, {});
    check(all.size() == visible.size() + 1U, "hidden member filter");
    check(provider.list("/folder-link", {}, {}).size() == 3U, "list symbolic directory");
    const auto page = provider.list("/", {.show_hidden = true, .offset = 1, .limit = 2}, {});
    check(page.size() == 2U && page[0].name == all[1].name && page[1].name == all[2].name, "sorted pagination");
    check(provider.list("/", {.offset = std::numeric_limits<std::size_t>::max(),
                              .limit = std::numeric_limits<std::size_t>::max()}, {}).empty(), "pagination overflow safety");
    check(provider.space("/").capacity > 0U, "archive backing filesystem space");
    for (const auto& path : {"../plain"s, "/folder/../plain"s, "/./plain"s, "//plain"s, "/folder//up"s, "x\0y"s}) {
        rejects([&] { static_cast<void>(provider.normalize(path)); }, "reject path alias: " + path);
    }
    rejects([&] { static_cast<void>(provider.join("/", "one/two")); }, "join refuses path injection");
    rejects([&] { write(provider, "/denied", "data"); }, "read-only mutation fails");
    std::ostringstream failed;
    failed.setstate(std::ios::badbit);
    rejects([&] { provider.read("/plain", 0, 2, failed, {}); }, "failed output stream is not success");
    fs::create_symlink("browse.tar", "archive-link.tar");
    rejects([&] { mcpp::ArchiveProvider link("archive-link.tar", options()); }, "host archive symlink rejected");
    fs::create_hard_link("browse.tar", "archive-hardlink.tar");
    rejects([&] { mcpp::ArchiveProvider link("archive-hardlink.tar", options(true)); }, "writable host archive alias rejected");
}

void mutations(const fs::path& archive, const mcpp::ArchiveFormat format) {
    if (format == mcpp::ArchiveFormat::tar) put(archive, tar_fixture(tar_members()));
    else put(archive, cpio_fixture(cpio_members(), format == mcpp::ArchiveFormat::cpio_crc));
    fs::permissions(archive, fs::perms{0640});
    const std::string attribute = "archive metadata";
    check(::setxattr(archive.c_str(), "user.mcpp-archive", attribute.data(), attribute.size(), 0) == 0,
          "set archive metadata fixture");
    mcpp::ArchiveProvider provider(archive, options(true));
    check(provider.format() == format, "archive format detected");
    provider.create_directory("/new");
    write(provider, "/new/first", binary);
    auto reopened = mcpp::open_archive_provider(archive, options());
    check(read(*reopened, "/new/first") == binary, "write persisted before returning");
    const auto stale = provider.stat("/new/first", {}).revision;
    provider.set_permissions("/new/first", fs::perms{0600});
    rejects([&] { write(provider, "/new/first", "not saved", true, stale); }, "stale member revision rejected");
    check(read(provider, "/new/first") == binary, "failed member write leaves original");
    const auto fresh = provider.stat("/new/first", {}).revision;
    write(provider, "/new/first", "updated", true, fresh);
    provider.create_symlink("first", "/new/link");
    rejects([&] { provider.create_hardlink("/new/link", "/new/hard-symlink"); }, "hardlinked symlinks explicitly unsupported");
    provider.create_hardlink("/new/first", "/new/hard");
    check(provider.stat("/new/hard", {}).links == 2U &&
          provider.stat("/new/hard", {}).inode == provider.stat("/new/first", {}).inode, "new hardlink identity");
    provider.set_permissions("/new/hard", fs::perms{0601});
    check(provider.stat("/new/first", {}).permissions == fs::perms{0601}, "hardlink chmod changes shared inode");
    provider.set_owner("/new/hard", "4294967295", "12345");
    check(provider.stat("/new/first", {}).uid == std::numeric_limits<std::uint32_t>::max() &&
          provider.stat("/new/first", {}).gid == 12345U, "hardlink ownership and full-width IDs");
    auto modified = std::chrono::system_clock::time_point{std::chrono::seconds(1700000123)};
    if (format == mcpp::ArchiveFormat::tar) modified += std::chrono::nanoseconds(123456789);
    provider.set_modified("/new/first", modified);
    check(provider.stat("/new/hard", {}).modified == modified, "archive modification time");
    check(provider.copy_direct("/new/first", "/new/copy", false, {}, {}), "direct regular copy");
    check(read(provider, "/new/copy") == "updated" &&
          provider.stat("/new/copy", {}).inode != provider.stat("/new/first", {}).inode, "copy is not hardlinked");
    provider.rename("/new/first", "/new/hard", true);
    check(provider.exists("/new/first") && provider.exists("/new/hard"), "rename between same hardlinks is no-op");
    provider.rename("/new", "/moved", false);
    check(!provider.exists("/new") && read(provider, "/moved/link") == "updated", "directory rename preserves relative link");
    reopened = mcpp::open_archive_provider(archive, options());
    check(reopened->stat("/moved/hard", {}).modified == modified &&
          reopened->stat("/moved/hard", {}).uid == std::numeric_limits<std::uint32_t>::max(),
          "metadata roundtrip");
    check(reopened->stat("/moved/hard", {}).links == 2U &&
          read(*reopened, "/moved/hard") == "updated", "hardlink roundtrip");
    rejects([&] { provider.remove("/moved", false, {}, {}); }, "nonrecursive directory removal rejected");
    rejects([&] { provider.rename("/moved", "/moved/child", false); }, "self-descendant rename rejected");
    rejects([&] { provider.validate_directory_target("/moved", provider, "/moved/child"); }, "recursive engine self-copy guard");
    rejects([&] { static_cast<void>(provider.copy_direct("/moved", "/copy", false, {}, {})); }, "direct directory copy explicit error");
    rejects([&] { write(provider, "/moved/first", "wrong"); }, "overwrite false enforced");
    rejects([&] { provider.create_symlink("../escape", "/bad"); }, "new escaping symlink rejected");
    rejects([&] { provider.set_permissions("/moved/link", fs::perms{0600}); }, "symlink chmod explicit error");
    provider.create_directory("/occupied");
    provider.create_directory("/occupied-dash");
    write(provider, "/occupied/child", "do not merge");
    rejects([&] { provider.rename("/moved", "/occupied", true); }, "rename cannot merge nonempty directory");
    provider.create_symlink("moved", "/alias");
    rejects([&] { provider.validate_directory_target("/moved", provider, "/alias/child"); }, "engine self-copy guard resolves aliases");
    write(provider, "/alias/via", "through parent link");
    check(read(provider, "/moved/via") == "through parent link", "write through symbolic parent");
    provider.remove("/moved/copy", false, {}, {});
    provider.remove("/moved/hard", false, {}, {});
    check(provider.stat("/moved/first", {}).links == 1U, "hardlink count after remove");
    provider.remove("/moved", true, {}, {});
    check(!provider.exists("/moved") && provider.exists("/alias"), "recursive remove leaves dangling link");
    rejects([&] { provider.remove("/", true, {}, {}); }, "root removal rejected");
    reopened = mcpp::open_archive_provider(archive, options());
    check(!reopened->exists("/moved") && read(*reopened, "/plain") == "original", "remove persisted and original intact");
    check((fs::status(archive).permissions() & fs::perms{07777}) == fs::perms{0640}, "host archive permissions preserved");
    std::array<char, 64> saved_attribute{};
    const auto attribute_size = ::getxattr(archive.c_str(), "user.mcpp-archive", saved_attribute.data(), saved_attribute.size());
    check(attribute_size == static_cast<ssize_t>(attribute.size()) &&
          std::string_view(saved_attribute.data(), static_cast<std::size_t>(attribute_size)) == attribute,
          "host archive user attributes preserved");
    check(!provider.last_draft(), "successful operation leaves no recovery draft");
}

void link_preserving_saves(const fs::path& archive, const mcpp::ArchiveFormat format) {
    if (format == mcpp::ArchiveFormat::tar) {
        put(archive, tar_fixture({
            {"first", '0', "linked", {}}, {"second", '1', {}, "first"}, {"third", '1', {}, "second"},
            {"standalone", '0', "linked", {}}, {"symbolic", '2', {}, "second"},
            {"folder/", '5', {}, {}}, {"directory-alias", '2', {}, "folder"}
        }));
    } else {
        put(archive, cpio_fixture({
            {"first", S_IFREG | 0640U, {}, 77, 3}, {"second", S_IFREG | 0640U, "linked", 77, 3},
            {"third", S_IFREG | 0640U, {}, 77, 3}, {"standalone", S_IFREG | 0640U, "linked", 78, 1},
            {"symbolic", S_IFLNK | 0777U, "second", 79, 1}, {"folder", S_IFDIR | 0755U, {}, 80, 2},
            {"directory-alias", S_IFLNK | 0777U, "folder", 81, 1}
        }, format == mcpp::ArchiveFormat::cpio_crc));
    }
    mcpp::ArchiveProvider provider(archive, options(true));
    const auto original = provider.stat("/first", {});
    check(original.inode != 0U && original.links == 3U, "archive group has nonzero inode and three names");
    for (const auto* name : {"/first", "/second", "/third"}) {
        const auto member = provider.stat(name, {});
        check(member.inode == original.inode && member.links == 3U &&
              member.revision == original.revision && provider.hardlink_count(name) == 3U,
              "Entry and hardlink_count agree on imported group");
    }
    check(provider.stat("/standalone", {}).inode != original.inode &&
          provider.hardlink_count("/standalone") == 1U, "equal data does not make a hardlink group");
    check(provider.stat("/symbolic", {}).links == 1U &&
          provider.stat("/symbolic", {}).inode != original.inode &&
          provider.hardlink_count("/symbolic") == 3U, "hardlink inspection follows symlink, lstat does not");
    rejects([&] { static_cast<void>(provider.hardlink_count("/missing")); }, "missing hardlink inspection fails");
    rejects([&] { static_cast<void>(provider.hardlink_count("/folder")); }, "directory hardlink inspection is unsupported");
    provider.ensure_writable("/symbolic");
    std::istringstream first_input(binary);
    provider.write_preserving_links("/first", first_input, binary.size(), original.revision, {});
    for (const auto* name : {"/first", "/second", "/third"}) {
        const auto member = provider.stat(name, {});
        check(read(provider, name) == binary && member.inode == original.inode && member.links == 3U &&
              member.revision != original.revision && member.permissions == original.permissions &&
              member.uid == original.uid && member.gid == original.gid,
              "preserving save updates every alias without changing identity or ownership/mode");
    }
    check(read(provider, "/standalone") == "linked", "preserving save does not change an unrelated equal-data file");
    auto reopened = mcpp::open_archive_provider(archive, options());
    check(read(*reopened, "/third") == binary && reopened->hardlink_count("/first") == 3U &&
          reopened->stat("/first", {}).inode == reopened->stat("/third", {}).inode,
          "preserving save persists the hardlink group");
    rejects([&] { reopened->ensure_writable("/first"); }, "read-only provider preflight fails");
    std::istringstream read_only_input("not saved");
    rejects([&] {
        reopened->write_preserving_links("/first", read_only_input, std::nullopt, std::nullopt, {});
    }, "preserving save rejects a read-only provider");
    const auto before_stale = get(archive);
    std::istringstream stale_input("not saved");
    rejects([&] {
        provider.write_preserving_links("/second", stale_input, std::nullopt, original.revision, {});
    }, "preserving save rejects stale group revision");
    check(stale_input.tellg() == 0 && get(archive) == before_stale, "stale save does not consume input or publish");
    std::istringstream symlink_revision_input("not saved");
    rejects([&] {
        provider.write_preserving_links("/symbolic", symlink_revision_input, std::nullopt,
                                        provider.stat("/symbolic", {}).revision, {});
    }, "preserving save expects target revision, not symlink inode revision");
    std::istringstream symbolic_input("via symlink");
    provider.write_preserving_links("/symbolic", symbolic_input, 11, provider.stat_follow("/symbolic", {}).revision, {});
    check(provider.stat("/symbolic", {}).kind == mcpp::EntryKind::symlink &&
          provider.stat("/symbolic", {}).target == "second" && read(provider, "/third") == "via symlink",
          "preserving save follows leaf symlink without replacing it");
    write(provider, "/second", "detached", true, provider.stat("/second", {}).revision);
    check(provider.stat("/second", {}).inode != original.inode && provider.hardlink_count("/second") == 1U &&
          provider.stat("/first", {}).inode == original.inode && provider.hardlink_count("/third") == 2U &&
          read(provider, "/third") == "via symlink", "ordinary write detaches only the saved member");
    check(provider.hardlink_count("/symbolic") == 1U && read(provider, "/symbolic") == "detached",
          "symlink inspection follows the detached member");
    const auto single_inode = provider.stat("/second", {}).inode;
    std::istringstream single_input("single file");
    provider.write_preserving_links("/second", single_input, std::nullopt, std::nullopt, {});
    check(read(provider, "/second") == "single file" && provider.stat("/second", {}).inode == single_inode,
          "preserving save works for a singleton with unknown input length");
    std::istringstream empty_input;
    provider.write_preserving_links("/first", empty_input, std::nullopt, std::nullopt, {});
    check(read(provider, "/first").empty() && read(provider, "/third").empty() &&
          provider.hardlink_count("/first") == 2U && read(provider, "/second") == "single file",
          "empty preserving save keeps an empty hardlink group");
    reopened = mcpp::open_archive_provider(archive, options());
    check(reopened->hardlink_count("/first") == 2U && read(*reopened, "/third").empty() &&
          reopened->hardlink_count("/second") == 1U, "empty group and detached singleton roundtrip");
    provider.rename("/first", "/folder/renamed", false);
    std::istringstream parent_alias_input("through directory alias");
    provider.write_preserving_links("/directory-alias/renamed", parent_alias_input, std::nullopt,
                                    provider.stat("/third", {}).revision, {});
    check(read(provider, "/third") == "through directory alias" && provider.hardlink_count("/folder/renamed") == 2U &&
          provider.stat("/folder/renamed", {}).inode == original.inode,
          "preserving save resolves symbolic parents after a hardlink rename");
    const auto unchanged = get(archive);
    std::istringstream short_input("short");
    rejects([&] { provider.write_preserving_links("/third", short_input, 100, std::nullopt, {}); },
            "preserving save rejects short input");
    std::istringstream long_input("too long");
    rejects([&] { provider.write_preserving_links("/third", long_input, 1, std::nullopt, {}); },
            "preserving save rejects long input");
    std::istringstream failed_input;
    failed_input.setstate(std::ios::failbit);
    rejects([&] { provider.write_preserving_links("/third", failed_input, std::nullopt, std::nullopt, {}); },
            "preserving save rejects failed input");
    std::istringstream missing_input("missing");
    rejects([&] { provider.write_preserving_links("/missing", missing_input, std::nullopt, std::nullopt, {}); },
            "preserving save cannot create a member");
    std::istringstream directory_input("directory");
    rejects([&] { provider.write_preserving_links("/folder", directory_input, std::nullopt, std::nullopt, {}); },
            "preserving save cannot replace a directory");
    mcpp::Cancellation stop;
    stop.request();
    std::istringstream cancelled_input("not saved");
    cancelled([&] { provider.write_preserving_links("/third", cancelled_input, std::nullopt, std::nullopt, stop); },
              "preserving save honors cancellation");
    check(get(archive) == unchanged && read(provider, "/third") == "through directory alias",
          "invalid and cancelled preserving saves do not publish");
    provider.set_permissions("/third", fs::perms{0444});
    const auto read_only_bytes = get(archive);
    rejects([&] { provider.ensure_writable("/directory-alias/renamed"); }, "preflight rejects a read-only member");
    std::istringstream protected_input("not saved");
    rejects([&] {
        provider.write("/third", protected_input, std::nullopt, true, std::nullopt, {}, {});
    }, "ordinary write cannot silently replace a read-only member");
    std::istringstream protected_group_input("not saved");
    rejects([&] {
        provider.write_preserving_links("/third", protected_group_input, std::nullopt, std::nullopt, {});
    }, "preserving save rejects a read-only group");
    check(protected_input.tellg() == 0 && protected_group_input.tellg() == 0 &&
          get(archive) == read_only_bytes && provider.hardlink_count("/third") == 2U,
          "read-only saves consume no input and leave the whole group unchanged");
    provider.set_permissions("/third", fs::perms{0020});
    provider.set_owner("/third", "4294967295", "4294967295");
    provider.ensure_writable("/third");
    std::istringstream writable_input("last linked save");
    provider.write_preserving_links("/third", writable_input, std::nullopt, std::nullopt, {});
    check(read(provider, "/folder/renamed") == "last linked save" &&
          provider.stat("/third", {}).permissions == fs::perms{0020} &&
          provider.stat("/third", {}).uid == std::numeric_limits<std::uint32_t>::max(),
          "write-bit preflight treats archived ownership as metadata");
    provider.create_hardlink("/third", "/fourth");
    check(provider.hardlink_count("/third") == 3U, "hardlink count includes newly linked virtual names");
    provider.remove("/fourth", false, {}, {});
    check(provider.hardlink_count("/third") == 2U, "hardlink count drops removed names");
    const auto external = get(archive) + std::string(512, '\0');
    put(archive, external);
    std::istringstream conflict_input("retained group draft");
    rejects([&] {
        provider.write_preserving_links("/third", conflict_input, std::nullopt, provider.stat("/third", {}).revision, {});
    }, "preserving publication rejects an external archive change");
    check(get(archive) == external && read(provider, "/third") == "last linked save" &&
          read(provider, "/folder/renamed") == "last linked save",
          "conflicted preserving save leaves disk and original group untouched");
    check(provider.last_draft() && fs::is_regular_file(*provider.last_draft()),
          "preserving publication failure retains a draft");
    mcpp::ArchiveProvider recovery(*provider.last_draft(), options());
    check(read(recovery, "/third") == "retained group draft" &&
          read(recovery, "/folder/renamed") == "retained group draft" &&
          recovery.hardlink_count("/third") == 2U &&
          recovery.stat("/third", {}).inode == recovery.stat("/folder/renamed", {}).inode,
          "recovery draft preserves every updated linked name as one group");
}

void native_tar_and_compression(const fs::path& root) {
    fs::create_directory("native");
    const auto long_name = std::string(200, 'n') + "\n";
    put(fs::path("native") / long_name, binary);
    put("native/plain", "native original");
    fs::create_hard_link("native/plain", "native/hard");
    fs::create_symlink("plain", "native/link");
    fs::create_directory("native/subdir");
    put("native/subdir/nested file", binary);
    fs::permissions("native", fs::perms{0710});
    for (const auto* format : {"default", "ustar", "gnu", "pax"}) {
        const auto path = root / ("native-"s + format + ".tar");
        if (std::string_view(format) == "default") {
            const auto result = mcpp::capture_command(
                {"/usr/bin/tar", "-cf", path.string(), "-C", (root / "native").string(), "."}, root
            );
            check(result.status == 0, "plain GNU tar -cf archive -C fixture .: " + result.error);
            check(get(path).starts_with("./\0"s), "plain GNU tar fixture starts with the ./ root header");
            mcpp::ArchiveProvider browser(path, options());
            check(browser.stat("/", {}).kind == mcpp::EntryKind::directory &&
                  browser.stat("/", {}).permissions == fs::perms{0710}, "GNU ./ root metadata is retained");
            check(read(browser, "/plain") == "native original" &&
                  read(browser, "/subdir/nested file") == binary, "GNU ./ member prefixes map to virtual paths");
            check(browser.stat("/plain", {}).inode == browser.stat("/hard", {}).inode &&
                  browser.hardlink_count("/hard") == 2U, "GNU ./ hardlink targets retain their group");
            for (const auto& entry : browser.list("/", {.show_hidden = true}, {})) {
                check(!entry.name.starts_with("./") && entry.name != "." &&
                      entry.path == "/" + entry.name, "GNU listing exposes canonical virtual names only");
            }
            for (const auto* alias : {"./plain", "/./plain", "/subdir/./nested file", "/subdir//nested file"}) {
                rejects([&] { static_cast<void>(browser.normalize(alias)); },
                        "archive-header ./ handling does not relax virtual API paths");
            }
        } else if (std::string_view(format) == "ustar") {
            const auto result = mcpp::capture_command({"/usr/bin/tar", "--format=ustar", "-cf", path.string(),
                                                       "-C", (root / "native").string(), "plain", "hard", "link"}, root);
            check(result.status == 0, "native ustar fixture: " + result.error);
        } else {
            const auto result = mcpp::capture_command({"/usr/bin/tar", "--format="s + format, "-cf", path.string(),
                                                       "-C", (root / "native").string(), "."}, root);
            check(result.status == 0, "native tar fixture: " + result.error);
        }
        mcpp::ArchiveProvider provider(path, options(true));
        check(read(provider, "/link") == "native original", "read native symlink");
        check(provider.stat("/hard", {}).links == 2U, "read native tar hardlink");
        if (std::string_view(format) != "ustar") check(read(provider, "/" + long_name) == binary, "native long filename");
        write(provider, "/published\nfile", binary);
        const auto result = mcpp::capture_command({"/usr/bin/tar", "-xOf", path.string(), "--", "./published\nfile"}, root);
        check(result.status == 0 && result.output == binary, "native tar reads provider output: " + result.error);
        if (std::string_view(format) != "ustar") {
            const auto long_result = mcpp::capture_command({"/usr/bin/tar", "-xOf", path.string(), "--", "./" + long_name}, root);
            check(long_result.status == 0 && long_result.output == binary, "native tar reads published long name: " + long_result.error);
        }
        const auto extracted = root / ("native-output-"s + format);
        fs::create_directory(extracted);
        fs::permissions(extracted, fs::perms{0700});
        const auto extraction = mcpp::capture_command({"/usr/bin/tar", "-xf", path.string(), "-C", extracted.string()}, root);
        check(extraction.status == 0, "native tar extracts only the controlled valid fixture: " + extraction.error);
        check(get(extracted / "plain") == "native original" && get(extracted / "hard") == "native original" &&
              fs::hard_link_count(extracted / "plain") == 2U, "native tar restores published hardlinks");
        check(fs::read_symlink(extracted / "link") == "plain", "native tar restores published symlink text");
    }
    put("compression-source.tar", tar_fixture(tar_members()));
    const std::array<std::tuple<std::string, std::string, mcpp::ArchiveCompression>, 3> tools{{
        {"gzip", "gz", mcpp::ArchiveCompression::gzip}, {"bzip2", "bz2", mcpp::ArchiveCompression::bzip2},
        {"xz", "xz", mcpp::ArchiveCompression::xz}
    }};
    for (const auto& [tool, suffix, kind] : tools) {
        const auto result = mcpp::capture_command({"/usr/bin/" + tool, "-c"}, root, {}, 1024U * 1024U,
                                                  root / "compression-source.tar");
        check(result.status == 0, "compression fixture: " + tool + ": " + result.error);
        const auto path = root / ("compressed name '\"\n.tar." + suffix);
        put(path, result.output);
        mcpp::ArchiveProvider provider(path, options(true));
        check(provider.compression() == kind && read(provider, "/plain") == "original", "compressed browse: " + tool);
        write(provider, "/saved file", binary);
        mcpp::ArchiveProvider reopened(path, options());
        check(reopened.compression() == kind && read(reopened, "/saved file") == binary, "compressed mutation roundtrip: " + tool);
        const auto extracted = mcpp::capture_command({"/usr/bin/tar", "-xOf", path.string(), "--", "./saved file"}, root);
        check(extracted.status == 0 && extracted.output == binary, "native tar reads compressed output: " + extracted.error);
        provider.create_hardlink("/saved file", "/saved alias");
        const auto linked_inode = provider.stat("/saved alias", {}).inode;
        std::istringstream linked_input("compressed linked save");
        provider.write_preserving_links("/saved file", linked_input, std::nullopt,
                                        provider.stat("/saved file", {}).revision, {});
        mcpp::ArchiveProvider linked_reopened(path, options());
        check(provider.stat("/saved file", {}).inode == linked_inode &&
              read(linked_reopened, "/saved alias") == "compressed linked save" &&
              read(linked_reopened, "/saved file") == "compressed linked save" &&
              linked_reopened.hardlink_count("/saved file") == 2U,
              "compressed preserving save keeps the hardlink group: " + tool);
        const auto linked_native = mcpp::capture_command(
            {"/usr/bin/tar", "-xOf", path.string(), "--", "./saved alias"}, root
        );
        check(linked_native.status == 0 && linked_native.output == "compressed linked save",
              "native tar reads compressed preserving save: " + linked_native.error);
    }
    put("damaged-gzip.tar.gz", "\x1f\x8bgarbage"s);
    rejects([&] { mcpp::ArchiveProvider provider("damaged-gzip.tar.gz", options()); }, "failed decompressor is explicit error");
    if (::access("/usr/bin/zstd", X_OK) != 0 && ::access("/bin/zstd", X_OK) != 0) {
        put("missing-zstd.tar.zst", "\x28\xb5\x2f\xfdinvalid"s);
        bool explicit_tool = false;
        try {
            mcpp::ArchiveProvider provider("missing-zstd.tar.zst", options());
        } catch (const std::runtime_error& error) {
            explicit_tool = std::string_view(error.what()).find("required compression tool is unavailable: zstd") != std::string_view::npos;
        }
        check(explicit_tool, "missing zstd is identified, not silently treated as tar");
    }
    put("expanded-limit.tar", tar_fixture({{"large", '0', std::string(16384, 'a'), {}}}));
    const auto bomb = mcpp::capture_command({"/usr/bin/gzip", "-c"}, root, {}, 1024U * 1024U, root / "expanded-limit.tar");
    check(bomb.status == 0, "bounded compressed limit fixture");
    put("expanded-limit.tar.gz", bomb.output);
    auto limited = options();
    limited.limits.expanded_bytes = 2048;
    rejects([&] { mcpp::ArchiveProvider provider("expanded-limit.tar.gz", limited); }, "decompression byte limit");
}

void failures() {
    put("conflict.tar", tar_fixture(tar_members()));
    fs::path retained;
    const auto external = tar_fixture({{"external", '0', "other writer", {}}});
    {
        mcpp::ArchiveProvider provider("conflict.tar", options(true));
        put("conflict.tar", external);
        rejects([&] { write(provider, "/candidate", binary); }, "external archive modification rejected");
        check(get("conflict.tar") == external, "external archive is not overwritten");
        check(!provider.exists("/candidate") && read(provider, "/plain") == "original", "failed publication keeps old snapshot");
        check(provider.last_draft().has_value(), "failed publication exposes recovery draft");
        retained = *provider.last_draft();
        check(fs::is_regular_file(retained), "recovery draft is persistent");
        check((fs::status(retained.parent_path()).permissions() & fs::perms{0077}) == fs::perms::none &&
              (fs::status(retained).permissions() & fs::perms{0077}) == fs::perms::none, "draft is private");
        mcpp::ArchiveProvider recovery(retained, options());
        check(read(recovery, "/candidate") == binary, "recovery draft contains rejected update");
    }
    check(fs::is_regular_file(retained), "recovery draft survives provider destruction");
    put("concurrent.tar", tar_fixture(tar_members()));
    mcpp::ArchiveProvider first("concurrent.tar", options(true));
    mcpp::ArchiveProvider second("concurrent.tar", options(true));
    write(first, "/first", "first writer");
    rejects([&] { write(second, "/second", "second writer"); }, "two provider snapshots detect conflict");
    mcpp::ArchiveProvider saved("concurrent.tar", options());
    check(read(saved, "/first") == "first writer" && !saved.exists("/second"), "first writer is preserved");
    std::istringstream callback_input("callback data");
    first.write("/callback", callback_input, 13, false, std::nullopt, {}, [&](const auto&) {
        check(first.exists("/plain"), "write progress can inspect provider without deadlocking");
    });
    first.copy_direct("/callback", "/callback-copy", false, {}, [&](const auto&) {
        check(first.stat("/callback", {}).size == 13U, "copy progress can stat provider");
    });
    first.remove("/callback-copy", false, {}, [&](const auto&) {
        check(first.exists("/callback-copy"), "remove progress can inspect pre-publication state");
    });
    const auto before = get("concurrent.tar");
    mcpp::Cancellation stop;
    stop.request();
    cancelled([&] { static_cast<void>(first.list("/", {}, stop)); }, "cancel listing");
    cancelled([&] { std::ostringstream output; first.read("/plain", 0, 1, output, stop); }, "cancel read");
    cancelled([&] { mcpp::ArchiveProvider provider("concurrent.tar", options(), stop); }, "cancel open");
    cancelled([&] { first.remove("/plain", false, stop, {}); }, "cancel remove");
    mcpp::Cancellation during;
    std::istringstream data("cancel this");
    cancelled([&] {
        first.write("/not-published", data, 11, false, std::nullopt, during,
                    [&](const auto&) { during.request(); });
    }, "cancel stream mutation before publication");
    check(get("concurrent.tar") == before, "cancelled operations leave archive bytes intact");
    std::istringstream short_input("short");
    rejects([&] { first.write("/short", short_input, 100, false, std::nullopt, {}, {}); }, "short input length rejected");
    std::istringstream long_input("too long");
    rejects([&] { first.write("/long", long_input, 1, false, std::nullopt, {}, {}); }, "long input length rejected");
    std::istringstream bad_input;
    bad_input.setstate(std::ios::failbit);
    rejects([&] { first.write("/bad-input", bad_input, std::nullopt, false, std::nullopt, {}, {}); }, "failed input stream rejected");
    check(get("concurrent.tar") == before, "invalid input does not modify archive");
    fs::permissions("concurrent.tar", fs::perms{0400});
    rejects([&] { first.ensure_writable("/plain"); }, "preflight checks host archive write permission");
    std::istringstream host_read_only_input("not saved");
    rejects([&] {
        first.write("/plain", host_read_only_input, std::nullopt, true, std::nullopt, {}, {});
    }, "ordinary write checks host archive write permission");
    std::istringstream host_read_only_group_input("not saved");
    rejects([&] {
        first.write_preserving_links("/plain", host_read_only_group_input, std::nullopt, std::nullopt, {});
    }, "preserving save checks host archive write permission");
    check(host_read_only_input.tellg() == 0 && host_read_only_group_input.tellg() == 0 &&
          get("concurrent.tar") == before, "host permission failures do not consume or publish input");
    fs::permissions("concurrent.tar", fs::perms{0644});
    fs::create_directory("blocked");
    put("blocked/read-only-parent.tar", tar_fixture(tar_members()));
    mcpp::ArchiveProvider blocked("blocked/read-only-parent.tar", options(true));
    fs::permissions("blocked", fs::perms{0555});
    rejects([&] { write(blocked, "/draft-survives", "recovery"); }, "publication failure in non-writable parent");
    fs::permissions("blocked", fs::perms{0755});
    check(blocked.last_draft() && fs::is_regular_file(*blocked.last_draft()), "failed filesystem publication retains draft");
    mcpp::ArchiveProvider recovery(*blocked.last_draft(), options());
    check(read(recovery, "/draft-survives") == "recovery", "failed filesystem publication draft is complete");
}

void independent_cpio_check(const fs::path& root) {
    const std::string script = R"PY(
import collections
import pathlib
import stat
import sys

for filename in sys.argv[1:]:
    data = pathlib.Path(filename).read_bytes()
    position = 0
    members = {}
    groups = collections.defaultdict(list)
    while True:
        header = data[position:position + 110]
        assert len(header) == 110 and header[:6] in (b"070701", b"070702")
        fields = [int(header[6 + i * 8:14 + i * 8], 16) for i in range(13)]
        inode, mode, uid, gid, links, mtime, size, major, minor, rmajor, rminor, namesize, checksum = fields
        position += 110
        name = data[position:position + namesize]
        assert len(name) == namesize and name[-1:] == b"\0" and b"\0" not in name[:-1]
        name = name[:-1]
        position = (position + namesize + 3) & ~3
        payload = data[position:position + size]
        assert len(payload) == size
        position = (position + size + 3) & ~3
        assert checksum == (sum(payload) & 0xffffffff if header[:6] == b"070702" else 0)
        if name == b"TRAILER!!!":
            assert size == 0 and not any(data[position:])
            break
        assert name not in members
        members[name] = (fields, payload)
        if stat.S_ISREG(mode):
            groups[(major, minor, inode)].append((fields, payload))
    assert b"./plain" in members and b"./hard" in members
    assert members[b"./plain"][0][0] == members[b"./hard"][0][0]
    original = groups[(0, 0, members[b"./plain"][0][0])]
    assert len(original) == 2 and all(item[0][4] == 2 for item in original)
    assert [item[1] for item in original if item[1]] == [b"original"]
    odd = b"./folder/line\n tab\t quote'\" back\\ $; --literal"
    assert members[odd][1] == b"zero\0one\nlast\xff"
    assert stat.S_ISLNK(members[b"./folder/up"][0][1])
    assert members[b"./folder/up"][1] == b"../plain"
print("independent newc/CRC framing, hardlinks, names and payloads verified")
)PY";
    const auto result = mcpp::capture_command(
        {"/usr/bin/python3", "-c", script, "mutations-newc.cpio", "mutations-crc.cpio"}, root
    );
    check(result.status == 0, "independent cpio format check: " + result.error);
}

void hostile_inputs() {
    put("outside-sentinel", "untouched");
    const std::vector<std::vector<TarMember>> cases{
        {{"/outside-sentinel", '0', "bad", {}}},
        {{"../outside-sentinel", '0', "bad", {}}},
        {{"safe/../outside-sentinel", '0', "bad", {}}},
        {{"safe//file", '0', "bad", {}}},
        {{"./../outside-sentinel", '0', "bad", {}}},
        {{"./safe/../outside-sentinel", '0', "bad", {}}},
        {{"./safe/./file", '0', "bad", {}}},
        {{"./safe//file", '0', "bad", {}}},
        {{"./safe//", '5', {}, {}}},
        {{"././file", '0', "bad", {}}},
        {{"same", '0', "one", {}}, {"./same", '0', "two", {}}},
        {{"./same", '0', "one", {}}, {"same", '0', "two", {}}},
        {{"./dir/", '5', {}, {}}, {"dir/", '5', {}, {}}},
        {{"./", '5', {}, {}}, {".", '5', {}, {}}},
        {{"./", '0', "not a directory", {}}},
        {{"bad\0name"s, '0', "bad", {}}},
        {{"escape", '2', {}, "../outside-sentinel"}},
        {{"escape", '2', {}, "/outside-sentinel"}},
        {{"base/", '5', {}, {}}, {"base/d", '2', {}, ".."}, {"base/link", '2', {}, "d/../outside-sentinel"}},
        {{"dir/", '5', {}, {}}, {"hop", '2', {}, "dir"}, {"hop/file", '0', "bad", {}}},
        {{"hard", '1', {}, "../outside-sentinel"}},
        {{"hard", '1', {}, "/outside-sentinel"}},
        {{"hard", '1', {}, "missing"}},
        {{"one", '1', {}, "two"}, {"two", '1', {}, "one"}},
        {{"one", '2', {}, "two"}, {"two", '2', {}, "one"}},
        {{"plain", '0', "data", {}}, {"sym", '2', {}, "plain"}, {"hard", '1', {}, "sym"}},
        {{"device", '3', {}, {}}},
        {{"fifo", '6', {}, {}}},
        {{"sparse", 'S', {}, {}}},
        {{"PaxHeaders/bad", 'x', pax("GNU.sparse.realsize", "100"), {}}, {"file", '0', "bad", {}}},
        {{"PaxHeaders/bad", 'x', pax("path", "../outside-sentinel"), {}}, {"file", '0', "bad", {}}},
        {{"PaxHeaders/bad", 'x', "999 path=bad\n", {}}, {"file", '0', "bad", {}}},
        {{"file", '0', "data", {}}, {"dir/", '5', {}, {}}, {"bad", '2', {}, "file/../dir"}},
    };
    std::size_t index = 0;
    for (const auto& members : cases) {
        const auto path = "hostile-" + std::to_string(index++) + ".tar";
        put(path, tar_fixture(members));
        rejects([&] { mcpp::ArchiveProvider provider(path, options()); }, "reject malicious tar fixture " + path);
        check(get("outside-sentinel") == "untouched", "no archive path reaches host sentinel");
    }
    auto bad_checksum = tar_fixture({{"plain", '0', "data", {}}});
    bad_checksum[0] ^= 1;
    put("bad-checksum.tar", bad_checksum);
    rejects([&] { mcpp::ArchiveProvider provider("bad-checksum.tar", options()); }, "bad tar checksum");
    auto truncated = tar_fixture({{"plain", '0', "data", {}}});
    truncated.resize(truncated.size() - 512U);
    put("truncated.tar", truncated);
    rejects([&] { mcpp::ArchiveProvider provider("truncated.tar", options()); }, "missing tar terminator");
    put("concatenated.tar", tar_fixture({{"one", '0', "one", {}}}) + tar_fixture({{"two", '0', "two", {}}}));
    rejects([&] { mcpp::ArchiveProvider provider("concatenated.tar", options()); }, "concatenated tar data is not hidden");
    put("limits.tar", tar_fixture({{"one", '0', "one", {}}, {"two", '0', "two", {}}}));
    auto limited = options();
    limited.limits.archive_bytes = 1024;
    rejects([&] { mcpp::ArchiveProvider provider("limits.tar", limited); }, "physical archive byte limit");
    limited = options();
    limited.limits.members = 1;
    rejects([&] { mcpp::ArchiveProvider provider("limits.tar", limited); }, "archive member limit");
    limited = options();
    limited.limits.path_bytes = 2;
    rejects([&] { mcpp::ArchiveProvider provider("limits.tar", limited); }, "archive path byte limit");
    const std::vector<std::vector<CpioMember>> cpio_cases{
        {{"/outside-sentinel", S_IFREG | 0644U, "bad", 1, 1}},
        {{"../outside-sentinel", S_IFREG | 0644U, "bad", 1, 1}},
        {{"same", S_IFREG | 0644U, "one", 1, 1}, {"./same", S_IFREG | 0644U, "two", 2, 1}},
        {{"bad\0name"s, S_IFREG | 0644U, "bad", 1, 1}},
        {{"link", S_IFLNK | 0777U, "../outside-sentinel", 1, 1}},
        {{"dir", S_IFLNK | 0777U, "target", 1, 1}, {"dir/file", S_IFREG | 0644U, "bad", 2, 1}},
        {{"fifo", S_IFIFO | 0644U, {}, 1, 1}},
        {{"one", S_IFREG | 0644U, "one", 1, 2}, {"two", S_IFREG | 0644U, "different", 1, 2}},
        {{"one", S_IFREG | 0644U, {}, 1, 0}},
        {{"one", S_IFREG | 0644U, {}, 1, 2}, {"two", S_IFREG | 0600U, "content", 1, 2}},
        {{"one", S_IFREG | 0644U, {}, 1, 2}, {"two", S_IFREG | 0644U, {}, 1, 2},
         {"three", S_IFREG | 0644U, "content", 1, 2}}
    };
    index = 0;
    for (const auto& members : cpio_cases) {
        const auto path = "hostile-" + std::to_string(index++) + ".cpio";
        put(path, cpio_fixture(members, false));
        rejects([&] { mcpp::ArchiveProvider provider(path, options()); }, "reject malicious cpio fixture " + path);
    }
    auto crc = cpio_fixture({{"crc", S_IFREG | 0644U, "payload", 1, 1}}, true);
    const auto data = crc.find("payload");
    check(data != std::string::npos, "CRC fixture body");
    crc[data] = 'P';
    put("bad-crc.cpio", crc);
    rejects([&] { mcpp::ArchiveProvider provider("bad-crc.cpio", options()); }, "cpio CRC verified");
    put("old-ascii.cpio", "070707unsupported");
    rejects([&] { mcpp::ArchiveProvider provider("old-ascii.cpio", options()); }, "old ASCII cpio explicit rejection");
    put("missing-trailer.cpio", cpio_fixture(cpio_members(), false).substr(0, 110));
    rejects([&] { mcpp::ArchiveProvider provider("missing-trailer.cpio", options()); }, "truncated cpio rejected");
}

void operation_engine(const fs::path& root) {
    fs::create_directory("transfers");
    check(::setenv("TMPDIR", (root / "transfers").c_str(), 1) == 0, "route existing engine staging inside disposable workspace");
    fs::create_directory("exports");
    put("engine.tar", tar_fixture(tar_members()));
    mcpp::ArchiveProvider archive("engine.tar", options(true));
    mcpp::LocalProvider local(root / "exports");
    mcpp::OperationEngine engine;
    engine.copy_exact(archive, archive.stat("/folder/" + odd, {}), local, (root / "exports" / odd).string(), {}, {}, {});
    check(get(root / "exports" / odd) == binary, "OperationEngine archive to local");
    put(root / "exports" / "import", "local import");
    engine.copy_exact(local, local.stat((root / "exports" / "import").string(), {}), archive, "/imported", {}, {}, {});
    check(read(archive, "/imported") == "local import", "OperationEngine local to archive");
    engine.copy_exact(archive, archive.stat("/folder/up", {}), local, (root / "exports" / "folder" / "relative-link").string(), {}, {}, {});
    check(fs::is_symlink(root / "exports" / "folder" / "relative-link") &&
          fs::read_symlink(root / "exports" / "folder" / "relative-link") == "../plain", "OperationEngine preserves literal symlink");
    engine.copy_exact(archive, archive.stat("/folder", {}), archive, "/folder-copy", {}, {}, {});
    check(read(archive, "/folder-copy/" + odd) == binary, "OperationEngine recursive archive copy");
    check(archive.stat("/folder-copy/up", {}).target == "../plain", "recursive copy preserves symlink text");
}

}  // namespace

int main(const int argc, char** argv) {
    try {
        if (::geteuid() == 0 || !fs::is_regular_file("/review/.operation-regressions-disposable")) {
            throw std::runtime_error("archive regressions require a non-root disposable review container");
        }
        if (argc != 2) throw std::runtime_error("usage: test_archive_provider /review/cases/agent-archives");
        const auto base = fs::absolute(argv[1]).lexically_normal();
        if (base != "/review/cases/agent-archives" || !fs::is_directory(base) || fs::is_symlink(fs::symlink_status(base))) {
            throw std::runtime_error("unexpected archive regression workspace");
        }
        const auto root = base / ("run-" + std::to_string(::getpid()));
        if (!fs::create_directory(root)) throw std::runtime_error("archive regression workspace already exists");
        fs::current_path(root);
        browse();
        mutations("mutations.tar", mcpp::ArchiveFormat::tar);
        mutations("mutations-newc.cpio", mcpp::ArchiveFormat::cpio_newc);
        mutations("mutations-crc.cpio", mcpp::ArchiveFormat::cpio_crc);
        independent_cpio_check(root);
        link_preserving_saves("links.tar", mcpp::ArchiveFormat::tar);
        link_preserving_saves("links-newc.cpio", mcpp::ArchiveFormat::cpio_newc);
        link_preserving_saves("links-crc.cpio", mcpp::ArchiveFormat::cpio_crc);
        native_tar_and_compression(root);
        failures();
        hostile_inputs();
        operation_engine(root);
        check(mcpp::is_archive_filename("file.tar.xz") && mcpp::is_archive_filename("file.cpio") &&
              !mcpp::is_archive_filename("file.zip"), "bounded filename recognition");
        std::cout << "PASS: " << checks << " archive checks; uid=" << ::geteuid() << "; workspace=" << root << '\n';
        fs::current_path(base);
        fs::remove_all(root);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " archive checks: " << error.what() << '\n';
        return 1;
    }
}
