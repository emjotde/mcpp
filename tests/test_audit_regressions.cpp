#include "mcpp/config.hpp"
#include "mcpp/compare.hpp"
#include "mcpp/editor.hpp"
#include "mcpp/local_provider.hpp"
#include "mcpp/operations.hpp"
#include "mcpp/search.hpp"
#include "mcpp/viewer.hpp"

#include <algorithm>
#include <array>
#include <endian.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

void check(const bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void write_file(const fs::path& path, const std::string& value) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output << value;
    check(static_cast<bool>(output), "write fixture");
}

std::string read_file(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    check(static_cast<bool>(input), "read fixture");
    return {std::istreambuf_iterator<char>(input), {}};
}

void run(const fs::path& root) {
    auto provider = std::make_shared<mcpp::LocalProvider>(root);
    mcpp::OperationEngine engine;
    write_file(root / "source/tree/a", "OLD SOURCE");
    write_file(root / "target/tree/a", "NEW TARGET");
    std::size_t child_queries = 0;
    mcpp::OperationResult moved;
    engine.move(
        *provider, {provider->stat((root / "source/tree").string(), {})},
        *provider, (root / "target").string(),
        {.overwrite=false, .preserve_permissions=true, .preserve_symlinks=true,
         .collision=[&](const mcpp::Entry& source, const mcpp::Entry&) {
             if (source.kind == mcpp::EntryKind::directory) {
                 return mcpp::CollisionDecision::overwrite;
             }
             ++child_queries;
             return mcpp::CollisionDecision::skip;
         }},
        {}, {}, &moved
    );
    check(child_queries == 1, "directory merge retains child collision decisions");
    check(read_file(root / "target/tree/a") == "NEW TARGET", "skip retains newer target");
    check(read_file(root / "source/tree/a") == "OLD SOURCE", "incomplete move retains source");
    check(moved.completed_sources.empty() && moved.skipped.size() == 1,
          "incomplete directory is not reported moved");

    write_file(root / "cycle/tree/data", "payload");
    fs::create_directory_symlink(".", root / "cycle/tree/self");
    std::size_t cycle_errors = 0;
    mcpp::OperationResult copied;
    engine.copy(
        *provider, {provider->stat((root / "cycle/tree").string(), {})},
        *provider, (root / "cycle-out").string(),
        {.overwrite=false, .preserve_permissions=true, .preserve_symlinks=false,
         .collision={},
         .error=[&](const mcpp::Entry&, const std::string& message) {
             check(message.find("cyclic") != std::string::npos, "explicit cycle error");
             ++cycle_errors;
             return mcpp::OperationErrorDecision::skip;
         }},
        {}, {}, &copied
    );
    check(cycle_errors == 1, "source cycle is rejected once");
    check(!fs::exists(root / "cycle-out/tree/self"), "cycle creates no repeated subtree");
    check(read_file(root / "cycle-out/tree/data") == "payload", "noncyclic siblings are copied");
    check(copied.completed_sources.empty() && copied.skipped.size() == 1, "cycle result is incomplete");

    write_file(root / "same-file", "same bytes");
    fs::create_hard_link(root / "same-file", root / "same-alias");
    mcpp::OperationResult alias;
    engine.copy_exact(
        *provider, provider->stat((root / "same-file").string(), {}),
        *provider, (root / "same-alias").string(),
        {.overwrite=true, .preserve_permissions=true, .preserve_symlinks=true, .collision={}},
        {}, {}, &alias
    );
    check(fs::equivalent(root / "same-file", root / "same-alias"), "same-inode copy retains identity");
    check(alias.completed_sources.empty() && alias.skipped.size() == 1, "same-inode copy is skipped");

    write_file(root / "config", "confirm_delete=tru\n");
    bool rejected = false;
    try {
        static_cast<void>(mcpp::Config::load(root / "config"));
    } catch (const std::runtime_error& error) {
        rejected = std::string(error.what()).find("confirm_delete=tru") != std::string::npos;
    }
    check(rejected, "invalid confirmation settings are rejected explicitly");
    write_file(root / "config", "confirm_delete=TRUE\nconfirm_overwrite=no\n");
    const auto config = mcpp::Config::load(root / "config");
    check(config.confirm_delete && !config.confirm_overwrite, "valid boolean spellings remain supported");

    write_file(root / "edit", "alpha beta\ngamma\n");
    mcpp::EditorModel editor;
    editor.open(provider, (root / "edit").string());
    editor.toggle_mark();
    editor.move(0, 5);
    editor.finish_mark();
    editor.set_position({0,6});
    editor.delete_to_end();
    check(editor.selected_text() == "alpha", "Ctrl-K retains an unrelated persistent block");
    check(editor.undo() && editor.selected_text() == "alpha", "Undo retains the block");
    editor.delete_block_or_line();
    editor.save(false);
    check(read_file(root / "edit") == " beta\ngamma\n", "F8 after kill/undo deletes only the block");

    const auto source_time = std::chrono::system_clock::time_point{std::chrono::seconds(946684800)};
    write_file(root / "timed/source", std::string(262144, 'x'));
    provider->set_modified((root / "timed/source").string(), source_time);
    const auto old_mask = umask(0027);
    std::size_t intermediate = 0;
    engine.copy_exact(
        *provider, provider->stat((root / "timed/source").string(), {}), *provider,
        (root / "timed/copy").string(),
        {.overwrite=false, .preserve_permissions=false, .preserve_symlinks=true, .collision={}},
        {}, [&](const mcpp::TransferProgress& value) {
            if (value.bytes_done > 0U && value.bytes_done < value.bytes_total) {
                ++intermediate;
            }
        }
    );
    umask(old_mask);
    check(intermediate > 0U, "generic copying reports intermediate progress");
    check(provider->stat((root / "timed/copy").string(), {}).modified == source_time,
          "copy preserves source modification time");
    check((fs::status(root / "timed/copy").permissions() & fs::perms::mask) ==
              (fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read),
          "unpreserved permissions respect the umask");
    write_file(root / "timed/private", "private data");
    fs::permissions(root / "timed/private", fs::perms::owner_read | fs::perms::owner_write);
    bool private_staging = false;
    engine.copy_exact(
        *provider, provider->stat((root / "timed/private").string(), {}), *provider,
        (root / "private-out/data").string(),
        {.overwrite=false, .preserve_permissions=true, .preserve_symlinks=true, .collision={}},
        {}, [&](const mcpp::TransferProgress& value) {
            if (value.bytes_done > 0U && fs::exists(root / "private-out")) {
                for (const auto& entry : fs::directory_iterator(root / "private-out")) {
                    if (entry.path().filename().string().starts_with(".mcpp-write-")) {
                        check((entry.status().permissions() & fs::perms::mask) ==
                                  (fs::perms::owner_read | fs::perms::owner_write),
                              "private copy never stages data with wider access");
                        private_staging = true;
                    }
                }
            }
        }
    );
    check(private_staging, "private-copy publication checkpoint was exercised");
    write_file(root / "group/a", "linked contents");
    fs::create_hard_link(root / "group/a", root / "group/b");
    engine.copy(
        *provider,
        {provider->stat((root / "group/a").string(), {}),
         provider->stat((root / "group/b").string(), {})},
        *provider, (root / "group-copy").string(),
        {.overwrite=false, .preserve_permissions=true, .preserve_symlinks=true, .collision={}}, {}
    );
    check(fs::equivalent(root / "group-copy/a", root / "group-copy/b") &&
              !fs::equivalent(root / "group/a", root / "group-copy/a"),
          "copied hardlink groups retain their own shared identity");
    write_file(root / "timed/skip", "KEEP");
    mcpp::OperationResult skip;
    engine.copy_exact(
        *provider, provider->stat((root / "timed/source").string(), {}), *provider,
        (root / "timed/skip").string(),
        {.overwrite=true, .preserve_permissions=false, .preserve_symlinks=true, .collision={}},
        {}, [](const mcpp::TransferProgress& value) {
            if (value.bytes_done > 0U) {
                throw mcpp::SkipCurrent{};
            }
        }, &skip
    );
    check(read_file(root / "timed/skip") == "KEEP" && skip.skipped.size() == 1U,
          "skip interrupts generic copying before publication");

    const auto reopen = [&](const std::string& value) {
        write_file(root / "search-edit", value);
        mcpp::EditorModel model;
        model.open(provider, (root / "search-edit").string());
        return model;
    };
    for (const auto mode : {mcpp::EditorSearchMode::regular_expression, mcpp::EditorSearchMode::wildcard}) {
        auto model = reopen("\xC3\xA9\n");
        const mcpp::EditorSearchOptions options{.mode=mode};
        check(model.find(mode == mcpp::EditorSearchMode::wildcard ? "?" : ".", options, false, false),
              "character search finds a UTF-8 character");
        check(model.selected_text() == "\xC3\xA9", "character matches do not split UTF-8");
        check(model.replace_selection("X"), "replace character match");
        model.save(false);
        check(read_file(root / "search-edit") == "X\n", "replacement preserves valid UTF-8");
    }
    const mcpp::EditorSearchOptions regex{.mode=mcpp::EditorSearchMode::regular_expression};
    auto scoped = reopen("abc def");
    std::optional<std::pair<mcpp::EditorPosition,mcpp::EditorPosition>> scope{{{0,0},{0,3}}};
    check(scoped.find(".*", regex, false, false, scope), "find scoped greedy match");
    check(scoped.replacement_for_selection(".*", "X", regex) == "X", "expand original scoped captures");
    check(scoped.replace_selection("X", &scope), "replace inside the original scope");
    scoped.save(false);
    check(read_file(root / "search-edit") == "X def", "scoped replacement does not consume boundary bytes");
    auto anchored = reopen("abc\ndef\n");
    check(anchored.find("^", regex, false, false), "find zero-width anchor");
    check(anchored.replace_selection(">"), "replace zero-width anchor");
    check(anchored.find("^", regex, false, false) && anchored.row() == 1U,
          "continue after an inserted zero-width match");
    auto prefixes = reopen("abc\ndef\n");
    check(prefixes.find("^", regex, false, false), "select first zero-width replacement");
    check(prefixes.replace_all("^", ">", regex) == 2U, "replace anchors on nonempty lines only");
    prefixes.save(false);
    check(read_file(root / "search-edit") == ">abc\n>def\n", "anchor replacement does not invent an EOF line");
    auto indentation = reopen("");
    indentation.tab(4);
    check(indentation.lines()[0] == "    ", "first indent Tab inserts a half tab");
    indentation.tab(4);
    check(indentation.lines()[0] == "\t", "second indent Tab becomes a real tab");
    check(indentation.undo() && indentation.lines()[0] == "    ", "tab collapse is one undo action");
    indentation.backspace(4);
    check(indentation.lines()[0].empty(), "Backspace removes the indentation step");
    auto wildcard = reopen("a1b2b");
    const mcpp::EditorSearchOptions glob{.mode=mcpp::EditorSearchMode::wildcard};
    check(wildcard.find("a*b", glob, false, false) && wildcard.selected_text() == "a1b2b",
          "wildcard captures are greedy");
    check(wildcard.replacement_for_selection("a*b", "x*", glob) == "x1b2",
          "wildcard replacement expands captures");
    check(mcpp::EditorSearch::replacement("abc", {0,3}, "abc", "\\n\\t", regex) == "\n\t",
          "regex replacement interprets escapes");
    check(mcpp::EditorSearch::filename_matches("a2.txt", "{a,b}[0-9].txt", true),
          "filename glob supports alternatives and character classes");
    check(mcpp::EditorSearch::filename_matches("\xC3\x89.txt", "\xC3\xA9.*", false),
          "filename glob supports UTF-8 case folding");
    check(mcpp::EditorSearch::filename_matches(std::string("bad_\xFF.txt", 9), "*.txt", false),
          "filename glob retains Linux names with non-UTF-8 bytes");

    write_file(root / "linked-edit", "original\n");
    fs::create_hard_link(root / "linked-edit", root / "linked-other");
    mcpp::EditorModel linked;
    linked.open(provider, (root / "linked-edit").string());
    check(linked.hardlink_count() == 2U, "inspect hardlink save choice");
    linked.insert("X");
    linked.save_preserving_links();
    check(fs::equivalent(root / "linked-edit", root / "linked-other") &&
              read_file(root / "linked-other") == "Xoriginal\n",
          "explicit linked save preserves shared identity and bytes");
    linked.insert("Y");
    linked.save(false);
    check(!fs::equivalent(root / "linked-edit", root / "linked-other") &&
              read_file(root / "linked-other") == "Xoriginal\n",
          "explicit atomic detach leaves the other link unchanged");

    write_file(root / "attributes", "data\n");
    check(setxattr((root / "attributes").c_str(), "user.audit", "value", 5, 0) == 0,
          "set user attribute fixture");
    std::array<std::uint32_t,11> acl{
        2, (6U << 16U) | 1U, 0xFFFFFFFFU, (4U << 16U) | 2U, 65534,
        (4U << 16U) | 4U, 0xFFFFFFFFU, (4U << 16U) | 16U, 0xFFFFFFFFU,
        32U, 0xFFFFFFFFU,
    };
    for (auto& value : acl) {
        value = htole32(value);
    }
    check(setxattr((root / "attributes").c_str(), "system.posix_acl_access",
                   acl.data(), sizeof(acl), 0) == 0, "set access ACL fixture");
    mcpp::EditorModel attributes;
    attributes.open(provider, (root / "attributes").string());
    attributes.insert("X");
    attributes.save(false);
    std::array<char,64> attribute{};
    check(getxattr((root / "attributes").c_str(), "user.audit", attribute.data(), attribute.size()) == 5,
          "atomic save preserves user attributes");
    check(getxattr((root / "attributes").c_str(), "system.posix_acl_access",
                   attribute.data(), attribute.size()) == static_cast<ssize_t>(sizeof(acl)),
          "atomic save preserves named-user access ACLs");
    fs::permissions(root / "attributes", fs::perms::owner_read);
    attributes.insert("Y");
    bool readonly_rejected = false;
    try {
        attributes.save(false);
    } catch (const std::exception&) {
        readonly_rejected = true;
    }
    fs::permissions(root / "attributes", fs::perms::owner_all);
    check(readonly_rejected && read_file(root / "attributes") == "Xdata\n",
          "ordinary save does not silently replace a read-only file");
    auto endings = reopen("a\r\nb\rc\n");
    endings.save_as((root / "crlf").string(), false, mcpp::EditorLineEnding::crlf);
    check(read_file(root / "crlf") == "a\r\nb\r\nc\r\n", "save-as conversion uses selected line endings");
    write_file(root / "save-target", "old");
    fs::create_hard_link(root / "save-target", root / "save-target-link");
    endings.save_as_preserving_links((root / "save-target").string(), true, mcpp::EditorLineEnding::lf);
    check(fs::equivalent(root / "save-target", root / "save-target-link") &&
              read_file(root / "save-target-link") == "a\nb\nc\n",
          "linked Save As converts endings and retains all target names");

    write_file(root / "compare-left/a", "AAAA");
    write_file(root / "compare-right/a", "BBBB");
    fs::create_directory(root / "compare-left/directory-only");
    provider->set_modified((root / "compare-left/a").string(), source_time);
    provider->set_modified((root / "compare-right/a").string(), source_time - std::chrono::seconds(1));
    const mcpp::DirectoryComparator comparator;
    auto differences = comparator.compare(*provider, (root / "compare-left").string(),
                                           *provider, (root / "compare-right").string(), false, {});
    check(differences.size() == 1U && differences[0].mark_left && !differences[0].mark_right,
          "quick comparison marks only the newer file and skips directories");
    provider->set_modified((root / "compare-right/a").string(), source_time);
    differences = comparator.compare(*provider, (root / "compare-left").string(),
                                      *provider, (root / "compare-right").string(), false, {},
                                      mcpp::CompareMode::thorough);
    check(differences.size() == 1U && differences[0].mark_left && differences[0].mark_right,
          "thorough comparison detects equal-size equal-time content changes");

    write_file(root / "find/a1.txt", "matching");
    write_file(root / "find/b2.txt", "matching");
    write_file(root / "find/c3.txt", "other");
    fs::create_directory(root / "find/blocked");
    fs::permissions(root / "find/blocked", fs::perms::none);
    std::size_t find_errors = 0;
    const mcpp::SearchEngine search;
    const auto found = search.search(
        *provider, (root / "find").string(),
        {.name_pattern="{a,b}[0-9].txt", .content={}}, {}, {},
        [&](const std::string&, const std::string&) { ++find_errors; }
    );
    fs::permissions(root / "find/blocked", fs::perms::owner_all);
    check(found.size() == 2U && find_errors == 1U, "Find keeps accessible matches and reports unreadable paths");
    write_file(root / "scan-source/blocked/data", "retried data");
    fs::permissions(root / "scan-source/blocked", fs::perms::none);
    std::size_t scan_errors = 0;
    mcpp::OperationResult retried;
    engine.copy(
        *provider, {provider->stat((root / "scan-source").string(), {})}, *provider,
        (root / "scan-copy").string(),
        {.overwrite=false, .preserve_permissions=true, .preserve_symlinks=true,
         .collision={},
         .error=[&](const mcpp::Entry&, const std::string&) {
             ++scan_errors;
             fs::permissions(root / "scan-source/blocked", fs::perms::owner_all);
             return mcpp::OperationErrorDecision::retry;
         }},
        {}, {}, &retried
    );
    check(scan_errors == 1U && retried.skipped.empty() &&
              read_file(root / "scan-copy/scan-source/blocked/data") == "retried data",
          "initial scan errors reach Retry and do not collide with their own new directory");
    mcpp::ViewerModel viewer;
    write_file(root / "hex-edit", "ABCD");
    viewer.open(provider, (root / "hex-edit").string());
    viewer.toggle_hex_editing();
    viewer.edit_hex_digit('5');
    viewer.edit_hex_digit('a');
    check(viewer.dirty() && read_file(root / "hex-edit") == "ABCD", "hex edits remain staged");
    viewer.save_changes();
    check(read_file(root / "hex-edit") == "ZBCD" && !viewer.dirty(), "hex save publishes staged edits");
    check(viewer.find("Z", true, {}, false) == 0U, "viewer search reads the edited data");
    check(!viewer.find("Z", true, {}, false) && viewer.search_can_wrap(), "viewer reports exhausted search origin");
    viewer.go_to_start();
    check(viewer.find("Z", true, {}, false) == 0U, "approved search wrap restarts at zero");

    auto large = reopen(std::string(16U * 1024U * 1024U, 'x'));
    for (int count = 0; count < 6; ++count) {
        large.insert('y');
    }
    for (int count = 0; count < 6; ++count) {
        check(large.undo(), "large document retains every edit in the delta undo budget");
    }
    check(!large.undo() && large.byte_size() == 16U * 1024U * 1024U, "large undo returns to original bytes");
    for (int count = 0; count < 6; ++count) {
        check(large.redo(), "delta redo restores large-file edits");
    }
}

}  // namespace

int main() {
    if (!fs::exists("/.dockerenv") ||
        !fs::exists("/review/.operation-regressions-disposable") || geteuid() == 0) {
        std::cerr << "Audit regression checks require the non-root disposable container\n";
        return 2;
    }
    std::string pattern = "/tmp/mcpp-audit-regressions-XXXXXX";
    const auto* created = mkdtemp(pattern.data());
    if (created == nullptr) {
        std::cerr << "Cannot create audit regression directory\n";
        return 2;
    }
    const fs::path root(created);
    try {
        run(root);
        fs::remove_all(root);
        std::cout << "[PASS] audited file safety, metadata, controls, search, saves, viewer and undo\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] " << error.what() << "\nArtifacts: " << root << '\n';
        return 1;
    }
}
