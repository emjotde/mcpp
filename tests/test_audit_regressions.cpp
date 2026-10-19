#include "mcpp/config.hpp"
#include "mcpp/compare.hpp"
#include "mcpp/editor.hpp"
#include "mcpp/local_provider.hpp"
#include "mcpp/local_tools.hpp"
#include "mcpp/tree_cache.hpp"
#include "mcpp/listing.hpp"
#include "mcpp/operations.hpp"
#include "mcpp/pane.hpp"
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
    const auto full_listing = mcpp::parse_listing_format(
        mcpp::listing_format(mcpp::ListingFormat::full, 2, ""));
    check(!full_listing.full_width && full_listing.columns == 1U &&
              full_listing.fields.size() == 6U &&
              mcpp::listing_field_widths(full_listing, 48) == std::vector<int>{1, 26, 1, 7, 1, 12},
          "native Full format uses type/name, size and modification time");
    const auto brief_listing = mcpp::parse_listing_format(
        mcpp::listing_format(mcpp::ListingFormat::brief, 3, ""));
    check(brief_listing.columns == 3U && brief_listing.fields.size() == 2U,
          "native Brief accepts its configured column count");
    const auto long_listing = mcpp::parse_listing_format(
        mcpp::listing_format(mcpp::ListingFormat::long_list, 2, ""));
    check(long_listing.full_width && long_listing.fields.front().name == "perm" &&
              long_listing.fields.back().name == "name",
          "native Long is full-width with permission/link/owner/group metadata in one row");
    const auto user_listing = mcpp::parse_listing_format(
        "full 2 <name:12+ | >size:7 =owner:8 perm mode nlink inode nuid ngid atime ctime mark dot");
    check(user_listing.full_width && user_listing.columns == 2U &&
              user_listing.fields.front().expand &&
              user_listing.fields[2].alignment == mcpp::ListingAlignment::right &&
              user_listing.fields[3].alignment == mcpp::ListingAlignment::center,
          "native display format handles frame, columns, alignment, widths and expansion");
    bool bad_listing = false;
    try { static_cast<void>(mcpp::parse_listing_format("half unknown")); }
    catch (const std::runtime_error&) { bad_listing = true; }
    check(bad_listing, "unknown display tags fail explicitly");
    std::vector<mcpp::Entry> sortable(3);
    for (std::size_t index = 0; index < sortable.size(); ++index) sortable[index].kind = mcpp::EntryKind::file;
    sortable[0].name = "file10"; sortable[1].name = "file2"; sortable[2].name = "file1";
    sortable[0].size = 10; sortable[1].size = 2; sortable[2].size = 1;
    sortable[0].modified = std::chrono::system_clock::time_point{std::chrono::seconds(3)};
    sortable[1].modified = std::chrono::system_clock::time_point{std::chrono::seconds(1)};
    sortable[2].modified = std::chrono::system_clock::time_point{std::chrono::seconds(2)};
    sortable[0].accessed = sortable[1].modified;
    sortable[1].accessed = sortable[0].modified;
    sortable[2].accessed = sortable[2].modified;
    sortable[0].changed = sortable[2].modified;
    sortable[1].changed = sortable[0].modified;
    sortable[2].changed = sortable[1].modified;
    sortable[0].inode = 3; sortable[1].inode = 1; sortable[2].inode = 2;
    const std::vector<std::pair<mcpp::SortField, std::vector<std::string>>> sort_cases{
        {mcpp::SortField::unsorted, {"file10", "file2", "file1"}},
        {mcpp::SortField::name, {"file1", "file10", "file2"}},
        {mcpp::SortField::version, {"file1", "file2", "file10"}},
        {mcpp::SortField::size, {"file1", "file2", "file10"}},
        {mcpp::SortField::modified, {"file2", "file1", "file10"}},
        {mcpp::SortField::accessed, {"file10", "file1", "file2"}},
        {mcpp::SortField::changed, {"file1", "file10", "file2"}},
        {mcpp::SortField::inode, {"file2", "file1", "file10"}}
    };
    for (const auto& [field, expected] : sort_cases) {
        auto values = sortable;
        mcpp::sort_entries(values, {.directories_first=false, .sort=field});
        std::vector<std::string> names;
        for (const auto& entry : values) names.push_back(entry.name);
        check(names == expected, "native sort field orders its actual metadata");
    }
    auto extensions = sortable;
    extensions[0].name = "left.z"; extensions[1].name = "middle.a"; extensions[2].name = "right.m";
    mcpp::sort_entries(extensions, {.sort=mcpp::SortField::extension});
    check(extensions[0].name == "middle.a" && extensions[1].name == "right.m" &&
              extensions[2].name == "left.z", "Extension sort uses the filename extension");
    auto executable = sortable;
    executable[1].permissions = fs::perms::owner_exec;
    mcpp::sort_entries(executable, {.directories_first=false, .executables_first=true});
    check(executable.front().name == "file2", "Executable first groups executable files before name order");
    auto folded = sortable;
    folded[0].name = "é"; folded[1].name = "É"; folded[2].name = "a";
    mcpp::sort_entries(folded, {.case_sensitive=false});
    check(folded[0].name == "a" && folded[1].name == "é" && folded[2].name == "É",
          "case-insensitive filename sorting folds UTF-8 without changing names");
    const auto command = mcpp::capture_shell("printf 'first\\000second'; printf problem >&2; exit 7", root);
    check(command.status == 7 && command.output == std::string("first\0second", 12) &&
              command.error == "problem",
          "local command capture retains binary output, stderr and nonzero status");
    const auto argument = mcpp::capture_command({"/usr/bin/printf", "%s", "a b'\"c"}, root);
    check(argument.status == 0 && argument.output == "a b'\"c",
          "direct command arguments are not interpreted by a shell");
    check(mcpp::convert_encoding("\xe9", "ISO-8859-1", "UTF-8") == "é",
          "local encoding conversion preserves the represented character");
    bool invalid_encoding = false;
    try {
        static_cast<void>(mcpp::convert_encoding("\xff", "UTF-8", "UTF-8"));
    } catch (const std::system_error&) {
        invalid_encoding = true;
    }
    check(invalid_encoding, "invalid encoding input fails explicitly");
    write_file(root / "panel-options/a.txt", "a");
    write_file(root / "panel-options/b.txt~", "backup");
    write_file(root / "panel-options/c.txt", "ccc");
    for (int index = 0; index < 20; ++index) {
        write_file(root / "panel-options" / ("item-" + std::to_string(index)), "x");
    }
    auto panel_provider = std::make_shared<mcpp::LocalProvider>(root);
    mcpp::Pane preferences(panel_provider, (root / "panel-options").string());
    preferences.set_show_backups(false);
    check(std::ranges::none_of(preferences.entries(), [](const mcpp::Entry& entry) { return entry.name.ends_with('~'); }),
          "Show backup files controls tilde-suffixed entries");
    preferences.set_viewport_rows(5);
    preferences.set_navigation(true, false);
    preferences.move_cursor(-100);
    for (int step = 0; step < 5; ++step) preferences.move_cursor(1);
    check(preferences.first_row() == 2U, "native Page scrolling advances the viewport by half a screen");
    preferences.move_cursor(-100);
    preferences.set_navigation(false, true);
    for (int step = 0; step < 3; ++step) preferences.move_cursor(1);
    check(preferences.first_row() == 1U, "Center scrolling starts when the cursor passes halfway");
    preferences.set_fast_reload(true);
    write_file(root / "panel-options/a.txt", "longer");
    preferences.refresh();
    const auto cached = std::ranges::find(preferences.entries(), std::string("a.txt"), &mcpp::Entry::name);
    check(cached != preferences.entries().end() && cached->size == 1U,
          "fast reload reuses a directory whose revision has not changed");
    preferences.refresh({}, true);
    const auto rescanned = std::ranges::find(preferences.entries(), std::string("a.txt"), &mcpp::Entry::name);
    check(rescanned != preferences.entries().end() && rescanned->size == 6U,
          "explicit Rescan bypasses the fast-reload cache");
    write_file(root / "panel-other/z.txt", "zz");
    mcpp::Pane other_panel(panel_provider, (root / "panel-other").string());
    other_panel.set_sort(mcpp::SortField::size, true);
    preferences.set_sort(mcpp::SortField::name, false);
    preferences.find("a.txt", true);
    preferences.toggle_selected(0);
    const auto original_directory = preferences.path();
    preferences.swap_contents(other_panel);
    check(preferences.path() == (root / "panel-other").string() &&
              other_panel.path() == original_directory &&
              preferences.sort_field() == mcpp::SortField::name &&
              other_panel.sort_field() == mcpp::SortField::size &&
              other_panel.marked_count() == 1U,
          "native content swap preserves physical sort settings and moves marks with the files");
    other_panel.set_panelized(other_panel.selected_entries());
    other_panel.change_directory((root / "panel-other").string());
    other_panel.restore_panelized();
    check(other_panel.panelized() && other_panel.entries().size() == 1U &&
              other_panel.entries().front().path == (root / "panel-options/a.txt").string() &&
              other_panel.path() == (root / "panel-other").string(),
          "Panelize restores the saved listing without changing the current directory");
    preferences.change_directory((root / "panel-options").string());
    preferences.set_shell_patterns(false);
    preferences.clear_selection();
    preferences.select_pattern("^a[.]txt$", true);
    check(preferences.marked_count() == 1U, "Shell patterns off uses regular expressions for selection");
    fs::create_directories(root / "cd-links/actual/child");
    fs::create_directory_symlink("actual/child", root / "cd-links/alias");
    mcpp::Pane links(panel_provider, (root / "cd-links").string());
    links.change_directory((root / "cd-links/alias").string());
    check(links.path().ends_with("/alias"), "logical cd retains the directory symlink");
    links.set_follow_links(false);
    links.up();
    check(links.path() == (root / "cd-links/actual").string(),
          "Cd follows links off uses the physical parent");
    write_file(root / "totals-source/one", "one");
    write_file(root / "totals-source/two", "two!");
    fs::create_directories(root / "totals-destination");
    std::vector<mcpp::TransferProgress> unmeasured;
    engine.copy(*panel_provider,
        {panel_provider->stat((root / "totals-source/one").string(), {}),
         panel_provider->stat((root / "totals-source/two").string(), {})},
        *panel_provider, (root / "totals-destination").string(),
        {.collision={}, .compute_totals=false}, {},
        [&](const mcpp::TransferProgress& value) { unmeasured.push_back(value); });
    check(!unmeasured.empty() && unmeasured.front().files_total <= 1U &&
              read_file(root / "totals-destination/one") == "one" &&
              read_file(root / "totals-destination/two") == "two!",
          "Compute totals off skips the pre-count while preserving copied bytes");
    panel_provider->set_preallocate(true);
    write_file(root / "preallocate", "original");
    std::istringstream short_preallocation("abc");
    bool preallocation_failed = false;
    try {
        panel_provider->write((root / "preallocate").string(), short_preallocation, 4U, true,
                             std::nullopt, {}, {});
    } catch (const std::exception&) {
        preallocation_failed = true;
    }
    check(preallocation_failed && read_file(root / "preallocate") == "original",
          "preallocation never publishes a short source");
    std::istringstream complete_preallocation("complete");
    panel_provider->write((root / "preallocate").string(), complete_preallocation, 8U, true,
                         std::nullopt, {}, {});
    check(read_file(root / "preallocate") == "complete", "Preallocate space preserves exact output bytes");
    panel_provider->set_preallocate(false);
    const std::set<std::string> cached_directories{
        (root / "cached tree").string(), (root / "cached\nnewline").string()
    };
    const auto cache_path = root / "tree-cache/Tree";
    mcpp::save_tree_cache(cache_path, cached_directories);
    check(mcpp::load_tree_cache(cache_path) == cached_directories,
          "persistent Tree cache round-trips spaces and control characters without splitting paths");
    fs::remove(cache_path);
    fs::create_symlink(root / "preallocate", cache_path);
    bool unsafe_cache = false;
    try { static_cast<void>(mcpp::load_tree_cache(cache_path)); }
    catch (const std::exception&) { unsafe_cache = true; }
    check(unsafe_cache && read_file(root / "preallocate") == "complete", "Tree cache never follows a symlink");
    const auto nroff_path = root / "viewer-nroff";
    const std::string nroff_bytes = "B\bBo\bol\bld\bd _\bU_\bN_\bD_\bE_\bR\n";
    write_file(nroff_path, nroff_bytes);
    mcpp::ViewerModel formatted;
    formatted.open(panel_provider, nroff_path.string());
    formatted.toggle_nroff();
    check(formatted.lines(80, 4).front() == "Bold UNDER" &&
              std::ranges::any_of(formatted.text_styles(0), [](const mcpp::ViewerTextStyle& style) { return style.bold; }) &&
              std::ranges::any_of(formatted.text_styles(0), [](const mcpp::ViewerTextStyle& style) { return style.underline; }),
          "nroff formatting combines overstrikes and retains bold/underline spans");
    check(formatted.data_size() == nroff_bytes.size() && formatted.source_view_end() == nroff_bytes.size(),
          "formatted progress still counts physical source bytes");
    formatted.goto_data_offset(2);
    check(formatted.offset() == 0U, "physical offsets inside an overstrike map to its visible character");
    formatted.goto_data_offset(0);
    formatted.toggle_hex();
    formatted.toggle_hex_editing();
    formatted.edit_hex_digit('5'); formatted.edit_hex_digit('a');
    formatted.toggle_hex();
    formatted.save_changes();
    auto changed_nroff = nroff_bytes; changed_nroff[0] = 'Z';
    check(read_file(nroff_path) == changed_nroff, "saving staged raw bytes from formatted text never writes presentation bytes");
    const auto compressed = mcpp::capture_command({"gzip","-c","--",nroff_path.string()}, root);
    check(compressed.status == 0, "standard gzip control produces the parsed fixture");
    write_file(root / "viewer-compressed.gz", compressed.output);
    mcpp::ViewerModel parsed;
    parsed.open(panel_provider, (root / "viewer-compressed.gz").string());
    check(parsed.parsed() && parsed.size() == changed_nroff.size(), "Parse mode exposes the decompressed data");
    parsed.toggle_hex();
    bool parsed_edit_rejected = false;
    try { parsed.toggle_hex_editing(); } catch (const std::exception&) { parsed_edit_rejected = true; }
    check(parsed_edit_rejected && !parsed.dirty(), "parsed data cannot be published as a damaged compressed file");
    parsed.toggle_magic();
    check(!parsed.parsed() && parsed.size() == compressed.output.size(), "Raw restores physical compressed bytes");
    write_file(root / "viewer-modes", "catalog cat\nAlpha42\nalpha17\n");
    mcpp::ViewerModel searched;
    searched.open(panel_provider, (root / "viewer-modes").string());
    mcpp::EditorSearchOptions expression{.mode=mcpp::EditorSearchMode::regular_expression};
    const auto match_one = searched.find("alpha[0-9]+", expression, {}, false);
    const auto match_two = searched.find("alpha[0-9]+", expression, {}, false);
    check(match_one && match_two && *match_two > *match_one, "viewer regex continuation advances to the next match");
    expression.backwards = true;
    check(searched.find("alpha[0-9]+", expression, {}, false) == match_one,
          "backwards continuation uses the current hit, not the line-start viewport");
    searched.go_to_start();
    check(searched.find("cat", mcpp::EditorSearchOptions{.whole_words=true}, {}, false) == 8U,
          "viewer whole-word search rejects a longer containing word");
    searched.goto_line(2);
    const auto marked_offset = searched.offset();
    searched.select_bookmark(3); searched.set_bookmark(); searched.go_to_start(); searched.goto_bookmark();
    check(searched.offset() == marked_offset, "viewer bookmarks retain exact displayed positions");
    write_file(root / "viewer-codepage", std::string("caf\xe9\n", 5));
    searched.open(panel_provider, (root / "viewer-codepage").string());
    check(searched.find("café", mcpp::EditorSearchOptions{}, {}, false, true) == 0U,
          "All charsets searches legacy codepages and maps the hit to raw bytes");
    searched.set_encoding("ISO-8859-1");
    check(searched.lines(80, 3).front() == "café" && read_file(root / "viewer-codepage") == std::string("caf\xe9\n", 5),
          "viewer codepage changes presentation without converting the file");
    check(searched.data_size() == 5U && searched.source_view_end() == 5U,
          "decoded codepage progress retains physical byte counters");
    mcpp::TemporaryFile command_input("mail body\n");
    const auto input_result = mcpp::capture_command({"cat"}, root, {}, 1024, command_input.path());
    check(input_result.status == 0 && input_result.output == "mail body\n", "local argv commands receive exact regular-file stdin");
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
        check(model.search_selection() ==
                  std::optional<std::pair<mcpp::EditorPosition, mcpp::EditorPosition>>{
                      {{0, 0}, {0, 2}}
                  }, "character matches do not split UTF-8");
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
    check(wildcard.find("a*b", glob, false, false) && wildcard.search_selection() ==
              std::optional<std::pair<mcpp::EditorPosition, mcpp::EditorPosition>>{
                  {{0, 0}, {0, 5}}
              },
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
    viewer.go_to_start();
    check(viewer.find("Z", true, {}, false) == 0U, "viewer search reads the edited data");
    check(!viewer.find("Z", true, {}, false) && viewer.search_can_wrap(), "viewer reports exhausted search origin");
    viewer.go_to_start();
    check(viewer.find("Z", true, {}, false) == 0U, "approved search wrap restarts at zero");

    write_file(root / "viewer-linked", "ABCD");
    fs::create_hard_link(root / "viewer-linked", root / "viewer-alias");
    mcpp::ViewerModel linked_view;
    linked_view.open(provider, (root / "viewer-linked").string());
    linked_view.toggle_hex_editing();
    linked_view.edit_ascii_byte('Z');
    bool link_choice_required = false;
    try {
        linked_view.save_changes();
    } catch (const std::runtime_error& error) {
        link_choice_required = std::string(error.what()).find("choice") != std::string::npos;
    }
    check(link_choice_required && linked_view.dirty() &&
              read_file(root / "viewer-alias") == "ABCD",
          "hex save requires an explicit hardlink policy and retains staged bytes");
    linked_view.save_changes({}, mcpp::ViewerLinkedSave::preserve_links);
    check(read_file(root / "viewer-linked") == "ZBCD" &&
              read_file(root / "viewer-alias") == "ZBCD" &&
              fs::equivalent(root / "viewer-linked", root / "viewer-alias"),
          "preserve-links hex save updates every alias without replacing the inode");
    linked_view.edit_ascii_byte('Y');
    linked_view.save_changes({}, mcpp::ViewerLinkedSave::detach);
    check(read_file(root / "viewer-linked") == "ZYCD" &&
              read_file(root / "viewer-alias") == "ZBCD" &&
              !fs::equivalent(root / "viewer-linked", root / "viewer-alias"),
          "explicit detach changes this name only");

    for (const auto& name : {"a.txt", "é.txt", "界.txt", "ab.txt"}) {
        write_file(root / "unicode-mask" / name, "x");
    }
    mcpp::Pane unicode_mask(provider, (root / "unicode-mask").string());
    unicode_mask.select_pattern("?.txt", true);
    check(unicode_mask.marked_count() == 3U, "question-mark masks count Unicode code points");
    unicode_mask.clear_selection();
    unicode_mask.select_pattern("??.txt", true);
    check(unicode_mask.marked_count() == 1U &&
              unicode_mask.selected_entries().front().name == "ab.txt",
          "a multibyte character is not treated as several question marks");
    unicode_mask.clear_selection();
    unicode_mask.select_pattern("*?.txt", true);
    check(unicode_mask.marked_count() == 4U, "star backtracking retains Unicode boundaries");

    mcpp::ViewerModel eof_view;
    write_file(root / "viewer-eof", "ONE\nTWO\nTHREE\n");
    eof_view.open(provider, (root / "viewer-eof").string());
    static_cast<void>(eof_view.lines(100, 24));
    eof_view.scroll_lines(40, 100);
    check(eof_view.offset() == 0U && eof_view.lines(100, 24).front() == "ONE",
          "a complete short file cannot be scrolled into a blank EOF page");
    const auto far_text = "BEGIN|" + std::string(6500, 'x') +
                          "NEEDLE|" + std::string(1500, 'y') + "\nEND\n";
    write_file(root / "viewer-far-match", far_text);
    eof_view.open(provider, (root / "viewer-far-match").string());
    static_cast<void>(eof_view.lines(100, 24));
    check(eof_view.find("NEEDLE", true, {}, false) == 6506U,
          "wrapped far text match is found at its original byte offset");
    auto revealed = eof_view.lines(100, 24);
    check(revealed.front().find("NEEDLE") != std::string::npos,
          "wrapped search reveals the matching screen row");
    eof_view.go_to_start();
    eof_view.set_wrap(false);
    check(eof_view.find("NEEDLE", true, {}, false) == 6506U,
          "unwrapped far text match is found");
    revealed = eof_view.lines(100, 24);
    check(revealed.front().find("NEEDLE") != std::string::npos &&
              eof_view.text_highlight(0).has_value(),
          "unwrapped search reveals and highlights the actual match");
    std::string cursor_matches(4096, '.');
    cursor_matches.replace(32, 6, "needle");
    cursor_matches.replace(160, 6, "needle");
    cursor_matches.replace(480, 6, "needle");
    write_file(root / "viewer-cursor-search", cursor_matches);
    mcpp::ViewerModel cursor_search;
    cursor_search.open(provider, (root / "viewer-cursor-search").string());
    cursor_search.toggle_hex();
    static_cast<void>(cursor_search.lines(100, 24));
    cursor_search.set_hex_cursor(192, false);
    check(cursor_search.find("needle", true, {}, false) == 480U,
          "fresh hex search starts at the current byte, not the viewport");
    cursor_search.set_hex_cursor(224, false);
    check(cursor_search.find("needle", true, {}, false) == 480U,
          "manual cursor movement resets repeat origin");
    cursor_search.goto_offset(4095);
    const auto failed_offset = cursor_search.offset();
    check(!cursor_search.find("absent", true, {}, false) &&
              !cursor_search.find("absent", true, {}, true) &&
              cursor_search.offset() == failed_offset && cursor_search.hex_cursor() == 4095U,
          "failed wrapped search preserves the previous cursor and viewport");
    check(cursor_search.position_percent() == 100U, "the final hex byte reports 100 percent");
    std::string text_matches;
    for (std::size_t row = 0; row < 120U; ++row) {
        text_matches += "ROW_" + std::to_string(row) +
                        (row == 0U || row == 50U || row == 100U ? " HIT\n" : "\n");
    }
    write_file(root / "viewer-repeat-text", text_matches);
    mcpp::ViewerModel text_repeat;
    text_repeat.open(provider, (root / "viewer-repeat-text").string());
    static_cast<void>(text_repeat.lines(100, 24));
    check(text_repeat.find("HIT", true, {}, false).has_value(), "find first text hit");
    for (int row = 0; row < 60; ++row) {
        text_repeat.scroll_lines(1, 100);
    }
    check(text_repeat.find("HIT", true, {}, false).has_value() &&
              text_repeat.lines(100, 24).front() == "ROW_100 HIT",
          "single-line manual scrolling resets text repeat origin");

    mcpp::ViewerModel text_search;
    write_file(root / "viewer-highlights", "aaaaa\nab\té界tail\n");
    text_search.open(provider, (root / "viewer-highlights").string());
    const auto span = [](const std::size_t begin, const std::size_t end) {
        return std::optional<std::pair<std::size_t, std::size_t>>{{begin, end}};
    };
    check(text_search.find("aa", true, {}, false) == 0U, "first text match starts at zero");
    static_cast<void>(text_search.lines(80, 4));
    check(text_search.text_highlight(0) == span(0, 2) && !text_search.text_highlight(1),
          "only the current occurrence has a text highlight");
    check(text_search.find("aa", true, {}, false) == 2U, "text continuation advances to next occurrence");
    static_cast<void>(text_search.lines(80, 4));
    check(text_search.text_highlight(0) == span(2, 4), "repeated search moves the displayed match");
    check(text_search.find("é界", true, {}, false) == 9U, "text match retains original UTF-8 offsets");
    auto highlighted = text_search.lines(80, 4);
    check(highlighted[0] == "ab      é界tail" && text_search.text_highlight(0) == span(8, 13),
          "expanded tabs and wide UTF-8 map to the rendered byte span");
    highlighted = text_search.lines(9, 2);
    check(highlighted == std::vector<std::string>{"ab      é", "界tail"} &&
              text_search.text_highlight(0) == span(8, 10) &&
              text_search.text_highlight(1) == span(0, 3),
          "a match crossing a wrapped row highlights both visible parts");
    text_search.set_wrap(false);
    text_search.scroll_columns(9);
    highlighted = text_search.lines(80, 1);
    check(highlighted[0] == "界tail" && text_search.text_highlight(0) == span(0, 3) &&
              !text_search.text_highlight(1),
          "horizontal cropping and reduced height do not reuse stale highlight spans");
    text_search.scroll_columns(-2);
    highlighted = text_search.lines(80, 1);
    check(highlighted[0] == " é界tail" && text_search.text_highlight(0) == span(1, 6),
          "a partially cropped tab keeps highlight alignment");
    check(!text_search.find("absent", true, {}, false), "new search can fail");
    static_cast<void>(text_search.lines(80, 1));
    check(!text_search.text_highlight(0), "failed new pattern clears the previous pattern's highlight");
    text_search.go_to_start();
    text_search.close();
    check(!text_search.text_highlight(0), "closed viewer retains no rendered highlights");

    mcpp::ViewerModel text_navigation;
    write_file(root / "goto-lines", "one\r\ntwo\nthree\n");
    text_navigation.open(provider, (root / "goto-lines").string());
    text_navigation.goto_line(2);
    check(text_navigation.offset() == 5U && text_navigation.line_number() == 2U,
          "Go to uses logical CRLF/LF lines, not wrapped screen rows");
    text_navigation.goto_line(3);
    bool invalid_line = false;
    try {
        text_navigation.goto_line(4);
    } catch (const std::out_of_range&) {
        invalid_line = true;
    }
    check(invalid_line && text_navigation.offset() == 9U,
          "out-of-range Go to leaves the visible position unchanged");
    invalid_line = false;
    try {
        text_navigation.goto_line(0);
    } catch (const std::invalid_argument&) {
        invalid_line = true;
    }
    check(invalid_line, "Go to rejects zero-based line numbers explicitly");
    write_file(root / "goto-long-line", std::string(300000U, 'x') + "\nsecond\n");
    text_navigation.open(provider, (root / "goto-long-line").string());
    text_navigation.goto_line(2);
    check(text_navigation.offset() == 300001U && text_navigation.line_number() == 2U,
          "line navigation crosses bounded read windows");
    text_navigation.goto_percent(50);
    check(text_navigation.offset() == text_navigation.size() / 2U,
          "percentage navigation uses byte position without integer overflow");
    bool invalid_percent = false;
    try {
        text_navigation.goto_percent(101);
    } catch (const std::out_of_range&) {
        invalid_percent = true;
    }
    check(invalid_percent, "Go to rejects invalid percentages explicitly");
    write_file(root / "goto-empty", "");
    text_navigation.open(provider, (root / "goto-empty").string());
    text_navigation.goto_line(1);
    text_navigation.goto_percent(100);
    check(text_navigation.offset() == 0U, "Go to handles an empty file");

    std::string hex_bytes(261U, '\0');
    for (std::size_t index = 0; index < hex_bytes.size(); ++index) {
        hex_bytes[index] = static_cast<char>(index % 256U);
    }
    write_file(root / "hex-navigation", hex_bytes);
    mcpp::ViewerModel hex_navigation;
    hex_navigation.open(provider, (root / "hex-navigation").string());
    hex_navigation.toggle_hex();
    static_cast<void>(hex_navigation.lines(80, 8));
    check(hex_navigation.position_percent() == 0U,
          "hex progress reports the active byte, not the visible range end");
    hex_navigation.set_hex_cursor(16, false);
    check(hex_navigation.position_percent() == 6U, "hex percentage follows the cursor");
    hex_navigation.scroll_hex_lines(3);
    check(hex_navigation.offset() == 48U && hex_navigation.hex_cursor() == 64U,
          "hex wheel scrolls immediately and retains the cursor's relative row");
    hex_navigation.scroll_hex_lines(-1000000);
    check(hex_navigation.offset() == 0U && hex_navigation.hex_cursor() == 16U,
          "hex wheel clamps at the start");
    hex_navigation.scroll_hex_lines(1000000);
    check(hex_navigation.offset() == 256U && hex_navigation.hex_cursor() == 260U,
          "hex wheel clamps at the final partial row");
    hex_navigation.toggle_hex_editing();
    hex_navigation.set_hex_cursor(16, false);
    hex_navigation.edit_hex_digit('a');
    check(hex_navigation.hex_low_nibble(), "high-nibble editing advances the nibble cursor");
    hex_navigation.set_hex_cursor(17, false, true);
    hex_navigation.edit_hex_digit('f');
    hex_navigation.set_hex_cursor(32, true);
    hex_navigation.edit_ascii_byte('Z');
    hex_navigation.set_hex_cursor(260, false, true);
    hex_navigation.edit_hex_digit('f');
    check(read_file(root / "hex-navigation") == hex_bytes, "pointed edits remain staged");
    hex_navigation.save_changes();
    const auto pointed = read_file(root / "hex-navigation");
    check(pointed[0] == '\0' && static_cast<unsigned char>(pointed[16]) == 0xA0U &&
              pointed[17] == '\x1f' && pointed[32] == 'Z' && pointed[260] == '\x0f',
          "byte, nibble and ASCII targets preserve all other bytes");

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
