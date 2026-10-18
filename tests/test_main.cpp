#include "mcpp/config.hpp"
#include "mcpp/compare.hpp"
#include "mcpp/editor.hpp"
#include "mcpp/editor_clipboard.hpp"
#include "mcpp/editor_search.hpp"
#include "mcpp/local_provider.hpp"
#include "mcpp/operations.hpp"
#include "mcpp/pane.hpp"
#include "mcpp/search.hpp"
#include "mcpp/syntax.hpp"
#include "mcpp/user_menu.hpp"
#include "mcpp/viewer.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace {

void check(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

template <typename Actual, typename Expected>
void check_equal(
    const Actual& actual,
    const Expected& expected,
    const std::string_view message
) {
    if (!(actual == expected)) {
        throw std::runtime_error(std::string(message));
    }
}

class TemporaryDirectory {
public:
    explicit TemporaryDirectory(
        const std::filesystem::path& directory =
            std::filesystem::temp_directory_path()
    ) {
        auto pattern =
            (directory / "mcpp-test-XXXXXX").string();
        auto buffer = std::vector<char>(
            pattern.begin(),
            pattern.end()
        );
        buffer.push_back('\0');
        const auto* created = mkdtemp(buffer.data());
        if (created == nullptr) {
            throw std::runtime_error(
                "failed to create test directory"
            );
        }
        path_ = created;
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const {
        return path_;
    }

private:
    std::filesystem::path path_;
};

void write_file(
    const std::filesystem::path& path,
    const std::string_view content
) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(
        content.data(),
        static_cast<std::streamsize>(content.size())
    );
    if (!output) {
        throw std::runtime_error("test write failed");
    }
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>(),
    };
}

struct FileSnapshot {
    struct stat status {};
    std::string bytes;
};

FileSnapshot snapshot_file(const std::filesystem::path& path) {
    FileSnapshot result;
    check(::lstat(path.c_str(), &result.status) == 0, "snapshot lstat");
    check(S_ISREG(result.status.st_mode), "snapshot regular file");
    result.bytes = read_file(path);
    return result;
}

void check_unchanged(
    const std::filesystem::path& path,
    const FileSnapshot& before
) {
    const auto after = snapshot_file(path);
    check_equal(after.bytes, before.bytes, "target bytes unchanged");
    check(
        after.status.st_dev == before.status.st_dev &&
            after.status.st_ino == before.status.st_ino &&
            after.status.st_mode == before.status.st_mode &&
            after.status.st_uid == before.status.st_uid &&
            after.status.st_gid == before.status.st_gid &&
            after.status.st_nlink == before.status.st_nlink &&
            after.status.st_mtim.tv_sec == before.status.st_mtim.tv_sec &&
            after.status.st_mtim.tv_nsec == before.status.st_mtim.tv_nsec,
        "target identity, mode, ownership, links and mtime unchanged"
    );
}

void test_local_provider() {
    TemporaryDirectory temporary;
    TemporaryDirectory outside;
    write_file(temporary.path() / "a.txt", "alpha");
    write_file(temporary.path() / ".hidden", "hidden");
    write_file(temporary.path() / "sub" / "b.bin", "bravo");
    std::filesystem::create_symlink(
        "a.txt",
        temporary.path() / "link"
    );
    write_file(outside.path() / "outside.txt", "outside");
    std::filesystem::create_directory_symlink(
        outside.path(),
        temporary.path() / "escape-link"
    );

    mcpp::LocalProvider provider(temporary.path());
    auto entries = provider.list(
        temporary.path().string(),
        {},
        {}
    );
    check_equal(entries.size(), std::size_t{4}, "visible list size");
    check(
        entries.front().kind == mcpp::EntryKind::directory ||
            entries.front().target_is_directory,
        "directories sort first"
    );
    entries = provider.list(
        temporary.path().string(),
        {.show_hidden = true},
        {}
    );
    check_equal(entries.size(), std::size_t{5}, "hidden list size");
    const auto item = provider.stat(
        (temporary.path() / "a.txt").string(),
        {}
    );
    check_equal(item.size, std::uint64_t{5}, "local file size");
    check(!item.revision.empty(), "local revision");

    std::ostringstream output;
    provider.read(item.path, 1, 3, output, {});
    check_equal(output.str(), std::string("lph"), "range read");

    std::istringstream input("replacement");
    provider.write(
        item.path,
        input,
        std::uint64_t{11},
        true,
        item.revision,
        {},
        {}
    );
    check_equal(
        read_file(temporary.path() / "a.txt"),
        std::string("replacement"),
        "atomic write"
    );
    const auto replaced = provider.stat(
        (temporary.path() / "a.txt").string(),
        {}
    );
    std::istringstream short_input("short");
    bool short_rejected = false;
    try {
        provider.write(
            replaced.path,
            short_input,
            std::uint64_t{10},
            true,
            replaced.revision,
            {},
            {}
        );
    } catch (const std::runtime_error&) {
        short_rejected = true;
    }
    check(short_rejected, "short source rejection");
    check_equal(
        read_file(temporary.path() / "a.txt"),
        std::string("replacement"),
        "short source preserves destination"
    );

    const mcpp::Cancellation publish_cancellation;
    std::istringstream one_byte("x");
    bool publish_cancelled = false;
    try {
        provider.write(
            replaced.path,
            one_byte,
            std::uint64_t{1},
            true,
            replaced.revision,
            publish_cancellation,
            [&](const mcpp::TransferProgress&) {
                publish_cancellation.request();
            }
        );
    } catch (const mcpp::OperationCancelled&) {
        publish_cancelled = true;
    }
    check(publish_cancelled, "pre-publish cancellation");
    check_equal(
        read_file(temporary.path() / "a.txt"),
        std::string("replacement"),
        "cancelled publish preserves destination"
    );
    std::istringstream blocked_input("blocked");
    bool no_clobber = false;
    try {
        provider.write(
            item.path,
            blocked_input,
            std::uint64_t{7},
            false,
            std::nullopt,
            {},
            {}
        );
    } catch (const std::runtime_error&) {
        no_clobber = true;
    }
    check(no_clobber, "atomic no-clobber");
    check_equal(
        read_file(temporary.path() / "a.txt"),
        std::string("replacement"),
        "no-clobber content"
    );

    provider.remove(
        (temporary.path() / "link").string(),
        false,
        {},
        {}
    );
    check(
        std::filesystem::exists(temporary.path() / "a.txt"),
        "removing symlink keeps target"
    );
    bool root_removal = false;
    try {
        provider.remove(temporary.path().string(), true, {}, {});
    } catch (const std::invalid_argument&) {
        root_removal = true;
    }
    check(root_removal, "provider root removal rejection");
    const auto deletion_root = temporary.path() / "cancel-delete";
    for (int index = 0; index < 32; ++index) {
        write_file(
            deletion_root / ("file-" + std::to_string(index)),
            "delete"
        );
    }
    const mcpp::Cancellation delete_cancellation;
    bool delete_cancelled = false;
    try {
        provider.remove(
            deletion_root.string(),
            true,
            delete_cancellation,
            [&](const mcpp::TransferProgress& value) {
                if (value.files_done >= 1U) {
                    delete_cancellation.request();
                }
            }
        );
    } catch (const mcpp::OperationCancelled&) {
        delete_cancelled = true;
    }
    check(delete_cancelled, "recursive delete cancellation");
    check(
        std::filesystem::exists(deletion_root),
        "cancelled recursive delete stops early"
    );
    const auto swapped = temporary.path() / "swapped-directory";
    std::filesystem::create_directory(swapped);
    std::filesystem::remove(swapped);
    std::filesystem::create_directory_symlink(
        outside.path(),
        swapped
    );
    provider.remove(swapped.string(), true, {}, {});
    check(
        std::filesystem::exists(outside.path() / "outside.txt"),
        "recursive delete does not follow swapped symlink"
    );
    bool followed_escape = false;
    try {
        static_cast<void>(provider.list(
            (temporary.path() / "escape-link").string(),
            {},
            {}
        ));
    } catch (const std::invalid_argument&) {
        followed_escape = true;
    }
    check(followed_escape, "directory symlink confinement");
    bool chmod_escape = false;
    try {
        provider.set_permissions(
            (temporary.path() / "escape-link").string(),
            std::filesystem::perms::owner_all
        );
    } catch (const std::invalid_argument&) {
        chmod_escape = true;
    }
    check(chmod_escape, "chmod symlink confinement");

    const auto unreadable = temporary.path() / "unreadable";
    std::filesystem::create_directory(unreadable);
    std::filesystem::permissions(
        unreadable,
        std::filesystem::perms::none,
        std::filesystem::perm_options::replace
    );
    bool unreadable_rejected = false;
    try {
        static_cast<void>(provider.list(unreadable.string(), {}, {}));
    } catch (const std::runtime_error&) {
        unreadable_rejected = true;
    }
    std::filesystem::permissions(
        unreadable,
        std::filesystem::perms::owner_all,
        std::filesystem::perm_options::replace
    );
    check(
        unreadable_rejected,
        "unreadable directory listing is not silently empty"
    );

    bool escaped = false;
    try {
        static_cast<void>(provider.normalize("/tmp"));
    } catch (const std::invalid_argument&) {
        escaped = true;
    }
    check(escaped, "provider root confinement");
}

void test_pane() {
    TemporaryDirectory temporary;
    write_file(temporary.path() / "one.txt", "one");
    write_file(temporary.path() / "two.txt", "two");
    write_file(temporary.path() / "sub" / "inside.txt", "inside");
    std::filesystem::create_directory_symlink(
        "sub",
        temporary.path() / "sub-link"
    );
    auto provider =
        std::make_shared<mcpp::LocalProvider>(temporary.path());
    mcpp::Pane pane(provider, temporary.path().string());
    check_equal(pane.entries().size(), std::size_t{4}, "pane entries");
    check_equal(
        pane.current()->kind,
        mcpp::EntryKind::directory,
        "pane directory first"
    );
    pane.toggle_selected();
    check_equal(
        pane.selected_entries().size(),
        std::size_t{1},
        "pane selection"
    );
    pane.clear_selection();
    pane.select_pattern("*.TXT", true);
    check(
        std::ranges::none_of(
            pane.entries(),
            [&](const auto& entry) {
                return pane.is_selected(entry);
            }
        ),
        "pane selection masks are case-sensitive"
    );
    pane.select_pattern("*.txt", true);
    check_equal(
        pane.selected_entries().size(),
        std::size_t{2},
        "pane select pattern"
    );
    pane.select_pattern("one*", false);
    check_equal(
        pane.selected_entries().size(),
        std::size_t{1},
        "pane unselect pattern"
    );
    pane.invert_selection();
    check_equal(
        pane.selected_entries().size(),
        std::size_t{2},
        "pane invert selection"
    );
    const auto marked = [&](const std::string_view name) {
        return std::ranges::any_of(
            pane.entries(),
            [&](const auto& entry) {
                return entry.name == name && pane.is_selected(entry);
            }
        );
    };
    check(
        marked("one.txt") && marked("sub-link") &&
            !marked("sub") && !marked("two.txt"),
        "inversion skips real directories but includes directory symlinks"
    );
    pane.select_pattern("sub", true);
    pane.invert_selection();
    check(
        marked("sub") && marked("two.txt") &&
            !marked("sub-link") && !marked("one.txt"),
        "inversion preserves marked directories and unmarks directory symlinks"
    );
    pane.invert_selection();
    check(
        marked("sub") && marked("sub-link") &&
            marked("one.txt") && !marked("two.txt"),
        "repeated inversion retains directory marks"
    );
    pane.clear_selection();
    pane.set_filter("one*");
    check(
        std::ranges::none_of(
            pane.entries(),
            [](const auto& entry) {
                return entry.name == "two.txt";
            }
        ),
        "pane filter hides non-matching files"
    );
    check(
        std::ranges::any_of(
            pane.entries(),
            [](const auto& entry) {
                return entry.name == "sub";
            }
        ),
        "pane filter keeps directories"
    );
    pane.set_filter("");
    auto panelized_entries = pane.entries();
    std::erase_if(
        panelized_entries,
        [](const auto& entry) {
            return entry.name != "one.txt" &&
                   entry.name != "two.txt";
        }
    );
    pane.set_panelized(std::move(panelized_entries));
    check(
        pane.panelized() && pane.entries().size() == 2U,
        "pane accepts panelized results"
    );
    pane.refresh();
    check_equal(
        pane.entries().size(),
        std::size_t{2},
        "panelized refresh preserves result set"
    );
    pane.toggle_selected(0);
    pane.set_filter("two*");
    check(
        pane.entries().size() == 1U &&
            pane.entries().front().name == "two.txt",
        "panelized filter applies to source results"
    );
    check_equal(
        pane.marked_count(),
        std::size_t{0},
        "panelized refresh prunes stale selections"
    );
    pane.set_filter("");
    pane.change_directory(temporary.path().string());
    check(!pane.panelized(), "directory change clears panelization");
    check(pane.find("two"), "pane find");
    check_equal(
        pane.current()->name,
        std::string("two.txt"),
        "pane find target"
    );
    check(
        pane.find("two", false, true) &&
            pane.current()->name == "two.txt",
        "incremental pane search keeps matching cursor"
    );
    check(
        !pane.find("link", false, true, true),
        "quick search does not match internal substring"
    );
    check(
        pane.find("sub", false, true, true) &&
            pane.current()->name.starts_with("sub"),
        "quick search matches filename prefix"
    );
    check(pane.find("two"), "restore pane find target");
    pane.move_cursor(-2);
    check(pane.enter(), "pane enter");
    check_equal(
        pane.entries().size(),
        std::size_t{2},
        "pane child listing"
    );
    pane.toggle_selected();
    check(
        pane.cursor() == 1U &&
            !pane.is_selected(*pane.current()),
        "mark-and-move skips parent entry"
    );
    pane.move_cursor(-1);
    check(pane.enter(), "pane parent entry");
    check_equal(
        pane.current()->name,
        std::string("sub-link"),
        "parent entry restores child cursor"
    );
    check(pane.history_back(), "pane history back");
    check_equal(
        pane.entries().size(),
        std::size_t{2},
        "pane history target"
    );
    check(pane.history_forward(), "pane history forward");
    check_equal(
        pane.current()->name,
        std::string("sub-link"),
        "pane history restores child cursor"
    );
    check(pane.find("sub-link"), "find symlink directory");
    check(pane.enter(), "enter symlink directory");
    check_equal(
        pane.entries().size(),
        std::size_t{2},
        "symlink directory listing"
    );
    const auto locked = temporary.path() / "locked";
    std::filesystem::create_directory(locked);
    std::filesystem::permissions(
        locked,
        std::filesystem::perms::none,
        std::filesystem::perm_options::replace
    );
    pane.up();
    check(pane.find("locked"), "find locked directory");
    const auto stable_path = pane.path();
    const auto stable_entries = pane.entries().size();
    bool locked_rejected = false;
    try {
        static_cast<void>(pane.enter());
    } catch (const std::runtime_error&) {
        locked_rejected = true;
    }
    std::filesystem::permissions(
        locked,
        std::filesystem::perms::owner_all,
        std::filesystem::perm_options::replace
    );
    check(
        locked_rejected &&
            pane.path() == stable_path &&
            pane.entries().size() == stable_entries &&
            pane.current()->name == "locked",
        "failed navigation preserves pane state"
    );
}

void test_special_paths() {
    TemporaryDirectory temporary;
    const std::vector<std::string> names{
        "space name.txt",
        "-leading-dash",
        "unicode-é.txt",
        "line\nbreak.txt",
    };
    for (const auto& name : names) {
        write_file(temporary.path() / name, name);
    }
    mcpp::LocalProvider provider(temporary.path());
    const auto entries = provider.list(
        temporary.path().string(),
        {.show_hidden = true},
        {}
    );
    for (const auto& name : names) {
        check(
            std::any_of(
                entries.begin(),
                entries.end(),
                [&](const mcpp::Entry& entry) {
                    return entry.name == name;
                }
            ),
            "special filename listing"
        );
    }
    provider.create_hardlink(
        (temporary.path() / "space name.txt").string(),
        (temporary.path() / "hard link").string()
    );
    struct stat source {};
    struct stat link {};
    check(
        ::stat(
            (temporary.path() / "space name.txt").c_str(),
            &source
        ) == 0 &&
            ::stat(
                (temporary.path() / "hard link").c_str(),
                &link
            ) == 0 &&
            source.st_ino == link.st_ino,
        "hard link preservation"
    );
    provider.set_permissions(
        (temporary.path() / "-leading-dash").string(),
        std::filesystem::perms::owner_read |
            std::filesystem::perms::owner_write
    );
    const auto permissions = provider.stat(
        (temporary.path() / "-leading-dash").string(),
        {}
    ).permissions;
    check(
        (permissions & std::filesystem::perms::owner_read) !=
            std::filesystem::perms::none &&
            (permissions & std::filesystem::perms::group_read) ==
                std::filesystem::perms::none,
        "permission update"
    );
}

void test_operations() {
    TemporaryDirectory temporary;
    const auto source_dir = temporary.path() / "source";
    const auto destination_dir = temporary.path() / "destination";
    std::filesystem::create_directories(destination_dir);
    write_file(source_dir / "a.txt", "alpha");
    write_file(source_dir / "sub" / "b.txt", "bravo");
    std::filesystem::permissions(
        source_dir,
        std::filesystem::perms::owner_all,
        std::filesystem::perm_options::replace
    );

    mcpp::LocalProvider provider(temporary.path());
    const auto source = provider.stat(source_dir.string(), {});
    mcpp::OperationEngine operations;
    mcpp::TransferProgress last;
    operations.copy(
        provider,
        {source},
        provider,
        destination_dir.string(),
        {
            .overwrite = false,
            .preserve_permissions = true,
            .preserve_symlinks = true,
            .collision = {},
        },
        {},
        [&](const mcpp::TransferProgress& value) {
            last = value;
        }
    );
    check_equal(
        read_file(destination_dir / "source" / "sub" / "b.txt"),
        std::string("bravo"),
        "recursive copy"
    );
    check_equal(
        std::filesystem::status(
            destination_dir / "source"
        ).permissions(),
        std::filesystem::perms::owner_all,
        "recursive copy preserves directory permissions"
    );
    bool descendant_rejected = false;
    try {
        operations.copy(
            provider,
            {source},
            provider,
            source_dir.string(),
            {},
            {}
        );
    } catch (const std::invalid_argument&) {
        descendant_rejected = true;
    }
    check(
        descendant_rejected &&
            !std::filesystem::exists(source_dir / "source"),
        "copy rejects descendant destination"
    );

    const auto copied = provider.stat(
        (destination_dir / "source" / "a.txt").string(),
        {}
    );
    const auto moved_dir = temporary.path() / "moved";
    std::filesystem::create_directories(moved_dir);
    operations.move(
        provider,
        {copied},
        provider,
        moved_dir.string(),
        {
            .overwrite = false,
            .preserve_permissions = true,
            .preserve_symlinks = true,
            .collision = {},
        },
        {}
    );
    check(
        !std::filesystem::exists(destination_dir / "source" / "a.txt"),
        "move removes source"
    );
    check_equal(
        read_file(moved_dir / "a.txt"),
        std::string("alpha"),
        "move content"
    );

    auto moved = provider.stat(
        (moved_dir / "a.txt").string(),
        {}
    );
    operations.move_exact(
        provider,
        moved,
        provider,
        (moved_dir / "renamed.txt").string(),
        {
            .overwrite = false,
            .preserve_permissions = true,
            .preserve_symlinks = true,
            .collision = {},
        },
        {}
    );
    check(
        !std::filesystem::exists(moved_dir / "a.txt") &&
            std::filesystem::exists(moved_dir / "renamed.txt"),
        "exact rename move"
    );
    moved = provider.stat(
        (moved_dir / "renamed.txt").string(),
        {}
    );
    operations.move_exact(
        provider,
        moved,
        provider,
        moved.path,
        {
            .overwrite = true,
            .preserve_permissions = true,
            .preserve_symlinks = true,
            .collision = {},
        },
        {}
    );
    check(
        std::filesystem::exists(moved.path),
        "self move keeps source"
    );
    operations.remove(provider, {moved}, {});
    check(
        !std::filesystem::exists(moved.path),
        "remove file"
    );
    static_cast<void>(last);

    const auto large = source_dir / "large.bin";
    {
        std::ofstream output(large, std::ios::binary);
        const std::string block(1024U * 1024U, 'x');
        for (int index = 0; index < 16; ++index) {
            output.write(
                block.data(),
                static_cast<std::streamsize>(block.size())
            );
        }
    }
    const auto large_entry = provider.stat(large.string(), {});
    const mcpp::Cancellation cancellation;
    bool cancelled = false;
    try {
        operations.copy(
            provider,
            {large_entry},
            provider,
            destination_dir.string(),
            {
                .overwrite = false,
                .preserve_permissions = true,
                .preserve_symlinks = true,
                .collision = {},
            },
            cancellation,
            [&](const mcpp::TransferProgress& value) {
                if (value.bytes_done > 0U) {
                    cancellation.request();
                }
            }
        );
    } catch (const mcpp::OperationCancelled&) {
        cancelled = true;
    }
    check(cancelled, "copy cancellation");
    check(
        !std::filesystem::exists(destination_dir / "large.bin"),
        "cancelled copy leaves no destination"
    );

    write_file(source_dir / "skip.txt", "source");
    write_file(destination_dir / "skip.txt", "destination");
    mcpp::LocalProvider second_provider(temporary.path());
    const auto skipped = provider.stat(
        (source_dir / "skip.txt").string(),
        {}
    );
    operations.move_exact(
        provider,
        skipped,
        second_provider,
        skipped.path,
        {
            .overwrite = true,
            .preserve_permissions = true,
            .preserve_symlinks = true,
            .collision = {},
        },
        {}
    );
    check_equal(
        read_file(source_dir / "skip.txt"),
        std::string("source"),
        "cross-instance self move keeps source"
    );
    operations.move(
        provider,
        {skipped},
        second_provider,
        destination_dir.string(),
        {
            .overwrite = false,
            .preserve_permissions = true,
            .preserve_symlinks = true,
            .collision =
                [](const mcpp::Entry&, const mcpp::Entry&) {
                    return mcpp::CollisionDecision::skip;
                },
        },
        {}
    );
    check_equal(
        read_file(source_dir / "skip.txt"),
        std::string("source"),
        "skipped move keeps source"
    );
    check_equal(
        read_file(destination_dir / "skip.txt"),
        std::string("destination"),
        "skipped move keeps destination"
    );

    write_file(source_dir / "skip-current.txt", "skip");
    write_file(source_dir / "copy-current.txt", "copy");
    const auto skip_current = provider.stat(
        (source_dir / "skip-current.txt").string(),
        {}
    );
    const auto copy_current = provider.stat(
        (source_dir / "copy-current.txt").string(),
        {}
    );
    const auto controlled_destination =
        temporary.path() / "controlled";
    std::filesystem::create_directory(controlled_destination);
    operations.copy(
        provider,
        {skip_current, copy_current},
        provider,
        controlled_destination.string(),
        {
            .overwrite = false,
            .preserve_permissions = true,
            .preserve_symlinks = true,
            .collision = {},
        },
        {},
        [&](const mcpp::TransferProgress& value) {
            if (value.current_path == skip_current.path) {
                throw mcpp::SkipCurrent{};
            }
        }
    );
    check(
        !std::filesystem::exists(
            controlled_destination / "skip-current.txt"
        ),
        "skip-current omits selected source"
    );
    check_equal(
        read_file(controlled_destination / "copy-current.txt"),
        std::string("copy"),
        "skip-current continues later sources"
    );

    write_file(source_dir / "exact-skip.txt", "keep");
    const auto exact_skip = provider.stat(
        (source_dir / "exact-skip.txt").string(),
        {}
    );
    operations.move_exact(
        provider,
        exact_skip,
        provider,
        (moved_dir / "exact-skip.txt").string(),
        {},
        {},
        [](const mcpp::TransferProgress& value) {
            if (value.files_done == 0U) {
                throw mcpp::SkipCurrent{};
            }
        }
    );
    check(
        std::filesystem::exists(exact_skip.path) &&
            !std::filesystem::exists(
                moved_dir / "exact-skip.txt"
            ),
        "exact move skips before rename"
    );

    const auto tree = source_dir / "skip-tree";
    write_file(tree / "a.txt", "skip");
    write_file(tree / "b.txt", "copy");
    const auto tree_destination = temporary.path() / "tree-destination";
    std::filesystem::create_directory(tree_destination);
    bool skipped_child = false;
    operations.move(
        provider,
        {provider.stat(tree.string(), {})},
        second_provider,
        tree_destination.string(),
        {
            .overwrite = false,
            .preserve_permissions = false,
            .preserve_symlinks = true,
            .collision = {},
        },
        {},
        [&](const mcpp::TransferProgress& value) {
            if (!skipped_child &&
                value.current_path == (tree / "a.txt").string()) {
                skipped_child = true;
                throw mcpp::SkipCurrent{};
            }
        }
    );
    check(
        std::filesystem::exists(tree / "a.txt") &&
            std::filesystem::exists(tree / "b.txt"),
        "partial directory move keeps source tree"
    );
    check(
        !std::filesystem::exists(
            tree_destination / "skip-tree" / "a.txt"
        ) &&
            read_file(
                tree_destination / "skip-tree" / "b.txt"
            ) == "copy",
        "directory skip continues with later children"
    );

    const auto direct_tree = source_dir / "direct-tree";
    write_file(direct_tree / "a.txt", "skip");
    write_file(direct_tree / "b.txt", "copy");
    const auto direct_destination =
        destination_dir / "direct-tree";
    bool direct_skipped = false;
    const auto direct_complete = provider.copy_direct(
        direct_tree.string(),
        direct_destination.string(),
        false,
        {},
        [&](const mcpp::TransferProgress& value) {
            if (!direct_skipped &&
                std::filesystem::path(value.current_path).filename() ==
                    "a.txt") {
                direct_skipped = true;
                throw mcpp::SkipCurrent{};
            }
        }
    );
    check(
        !direct_complete &&
            !std::filesystem::exists(
                direct_destination / "a.txt"
            ) &&
            read_file(direct_destination / "b.txt") == "copy",
        "direct directory copy reports skipped children"
    );

    const auto link_target = source_dir / "link-target.txt";
    const std::string link_content(64U, 'x');
    write_file(link_target, link_content);
    const auto link = source_dir / "data-link";
    std::filesystem::create_symlink(
        link_target.filename(),
        link
    );
    operations.copy(
        provider,
        {provider.stat(link.string(), {})},
        provider,
        controlled_destination.string(),
        {
            .overwrite = false,
            .preserve_permissions = true,
            .preserve_symlinks = false,
            .collision = {},
        },
        {}
    );
    check_equal(
        read_file(controlled_destination / "data-link"),
        link_content,
        "dereferenced symlink copy is complete"
    );

    const auto fifo = source_dir / "pipe";
    check(::mkfifo(fifo.c_str(), 0600) == 0, "create FIFO fixture");
    const auto fifo_entry = provider.stat(fifo.string(), {});
    bool rejected_special = false;
    try {
        operations.copy(
            provider,
            {fifo_entry},
            provider,
            destination_dir.string(),
            {
                .overwrite = false,
                .preserve_permissions = true,
                .preserve_symlinks = true,
                .collision = {},
            },
            {}
        );
    } catch (const std::runtime_error&) {
        rejected_special = true;
    }
    check(rejected_special, "special-file copy rejection");
    check(
        !std::filesystem::exists(destination_dir / "pipe"),
        "special-file copy leaves no destination"
    );
}

void test_operation_destinations() {
    TemporaryDirectory temporary;
    const auto root = temporary.path();
    mcpp::LocalProvider provider(root);
    mcpp::OperationEngine operations;
    mcpp::OperationOptions overwrite;
    overwrite.overwrite = true;

    const auto source = root / "source" / "a.txt";
    const auto bucket = root / "destination" / "bucket";
    write_file(source, "SOURCE\n");
    write_file(bucket / "KEEP", "DIRECTORY-SENTINEL\n");
    const auto source_before = snapshot_file(source);
    const auto keep = snapshot_file(bucket / "KEEP");
    operations.move_exact(
        provider, provider.stat(source.string(), {}), provider,
        bucket.string(), overwrite, {}
    );
    check(
        std::filesystem::is_directory(bucket) &&
            !std::filesystem::exists(source) &&
            read_file(bucket / "a.txt") == "SOURCE\n",
        "exact move interprets existing directory as destination directory"
    );
    check_unchanged(bucket / "a.txt", source_before);
    check_unchanged(bucket / "KEEP", keep);

    const auto copy_source = root / "source" / "copy.txt";
    const auto exact_target = root / "destination" / "renamed.txt";
    write_file(copy_source, "EXACT-COPY\n");
    std::filesystem::permissions(
        copy_source, std::filesystem::perms::owner_all
    );
    const auto original_copy = snapshot_file(copy_source);
    operations.copy_exact(
        provider, provider.stat(copy_source.string(), {}), provider,
        exact_target.string(), overwrite, {}
    );
    check(
        std::filesystem::is_regular_file(exact_target) &&
            read_file(exact_target) == original_copy.bytes &&
            std::filesystem::status(exact_target).permissions() ==
                std::filesystem::status(copy_source).permissions(),
        "exact copy creates named file and preserves permissions"
    );
    check_unchanged(copy_source, original_copy);
    write_file(exact_target, "OLD\n");
    operations.copy_exact(
        provider, provider.stat(copy_source.string(), {}), provider,
        exact_target.string(), overwrite, {}
    );
    check_equal(read_file(exact_target), original_copy.bytes,
                "exact copy overwrites named file");
    check_unchanged(bucket / "KEEP", keep);

    const auto tree = root / "source" / "tree";
    const auto destination_tree = root / "destination" / "tree";
    write_file(tree / "new.txt", "NEW\n");
    write_file(destination_tree / "KEEP", "MERGE-SENTINEL\n");
    const auto merge_keep = snapshot_file(destination_tree / "KEEP");
    operations.move(
        provider, {provider.stat(tree.string(), {})}, provider,
        (root / "destination").string(), overwrite, {}
    );
    check(
        !std::filesystem::exists(tree) &&
            read_file(destination_tree / "new.txt") == "NEW\n",
        "directory move merges rather than replacing unrelated subtree"
    );
    check_unchanged(destination_tree / "KEEP", merge_keep);

    const auto guarded_source = root / "guarded-source";
    const auto guarded_directory = root / "guarded-directory";
    write_file(guarded_source, "DO-NOT-REMOVE\n");
    write_file(guarded_directory / "KEEP", "PROVIDER-SENTINEL\n");
    const auto guarded_before = snapshot_file(guarded_source);
    const auto guarded_keep = snapshot_file(guarded_directory / "KEEP");
    bool rejected = false;
    try {
        provider.rename(
            guarded_source.string(), guarded_directory.string(), true
        );
    } catch (const std::exception&) {
        rejected = true;
    }
    check(rejected && std::filesystem::is_directory(guarded_directory),
          "provider overwrite refuses file over real directory");
    check_unchanged(guarded_source, guarded_before);
    check_unchanged(guarded_directory / "KEEP", guarded_keep);

    const auto race_source = root / "race-source";
    const auto race_target = root / "race-target";
    write_file(race_source, "RACE-SOURCE\n");
    write_file(race_target, "INITIAL-FILE\n");
    const auto race_before = snapshot_file(race_source);
    FileSnapshot race_keep;
    bool changed_type = false;
    rejected = false;
    try {
        operations.move_exact(
            provider, provider.stat(race_source.string(), {}), provider,
            race_target.string(), overwrite, {},
            [&](const mcpp::TransferProgress& value) {
                if (!changed_type && value.files_done == 0U) {
                    std::filesystem::remove(race_target);
                    write_file(race_target / "KEEP", "RACE-SENTINEL\n");
                    race_keep = snapshot_file(race_target / "KEEP");
                    changed_type = true;
                }
            }
        );
    } catch (const std::exception&) {
        rejected = true;
    }
    check(changed_type && rejected &&
              std::filesystem::is_directory(race_target),
          "exact move refuses target changed to directory before publication");
    check_unchanged(race_source, race_before);
    check_unchanged(race_target / "KEEP", race_keep);

    const auto alias_source = root / "alias-source" / "tree";
    const auto alias_parent = root / "alias-destination";
    const auto alias_target = alias_parent / "tree";
    write_file(alias_source / "important.txt", "ONLY-SOURCE-COPY\n");
    write_file(alias_source / "child" / "KEEP", "DESCENDANT-SENTINEL\n");
    std::filesystem::create_directory(alias_parent);
    const auto important = snapshot_file(alias_source / "important.txt");
    const auto descendant = snapshot_file(alias_source / "child" / "KEEP");
    auto marks_provider = std::make_shared<mcpp::LocalProvider>(root);
    mcpp::Pane marks(marks_provider, alias_source.parent_path().string());
    const auto alias_entry = provider.stat(alias_source.string(), {});
    for (const auto* link_target :
         {"../alias-source/tree", "../alias-source/tree/child"}) {
        std::filesystem::create_directory_symlink(link_target, alias_target);
        struct stat link_before {};
        check(::lstat(alias_target.c_str(), &link_before) == 0,
              "directory alias lstat");
        for (int method = 0; method < 4; ++method) {
            marks.select_paths({alias_entry.path});
            mcpp::OperationResult refused;
            bool alias_rejected = false;
            try {
                if (method == 0) {
                    operations.move(
                        provider, {alias_entry}, provider,
                        alias_parent.string(), overwrite, {}, {}, &refused
                    );
                } else if (method == 1) {
                    operations.move_exact(
                        provider, alias_entry, provider,
                        alias_target.string(), overwrite, {}, {}, &refused
                    );
                } else if (method == 2) {
                    mcpp::LocalProvider other(root);
                    operations.move(
                        provider, {alias_entry}, other,
                        alias_parent.string(), overwrite, {}, {}, &refused
                    );
                } else {
                    mcpp::LocalProvider confined(alias_source.parent_path());
                    operations.move(
                        confined, {alias_entry}, provider,
                        alias_parent.string(), overwrite, {}, {}, &refused
                    );
                }
            } catch (const std::exception&) {
                alias_rejected = true;
            }
            check(alias_rejected && refused.completed_sources.empty(),
                  "physical self/descendant move alias is refused");
            check_unchanged(alias_source / "important.txt", important);
            check_unchanged(alias_source / "child" / "KEEP", descendant);
            struct stat link_after {};
            check(::lstat(alias_target.c_str(), &link_after) == 0 &&
                      link_after.st_dev == link_before.st_dev &&
                      link_after.st_ino == link_before.st_ino &&
                      link_after.st_mode == link_before.st_mode &&
                      std::filesystem::read_symlink(alias_target) == link_target,
                  "refused move preserves destination symlink object");
            marks.unmark_paths(refused.completed_sources);
            marks.refresh();
            check(marks.marked_count() == 1U &&
                      marks.selected_entries().front().path == alias_entry.path,
                  "refused alias move retains the source mark");
        }
        bool direct_rejected = false;
        try {
            provider.copy_direct(
                alias_source.string(), alias_target.string(), true, {}, {}
            );
        } catch (const std::exception&) {
            direct_rejected = true;
        }
        check(direct_rejected, "direct directory copy rejects physical aliases");
        check_unchanged(alias_source / "important.txt", important);
        check_unchanged(alias_source / "child" / "KEEP", descendant);
        std::filesystem::remove(alias_target);
    }
    std::cout << "  [PASS] physical directory aliases, data, inode and marks\n";

    const auto restricted_parent = root / "restricted";
    const auto restricted_source = restricted_parent / "stuck.txt";
    const auto restricted_target = root / "restricted-target" / "stuck.txt";
    write_file(restricted_source, "RENAME-MUST-FAIL\n");
    write_file(restricted_target, "DO-NOT-COPY-OVER-ME\n");
    const auto restricted_before = snapshot_file(restricted_source);
    const auto restricted_destination = snapshot_file(restricted_target);
    std::filesystem::permissions(
        restricted_parent,
        std::filesystem::perms::owner_read |
            std::filesystem::perms::owner_exec |
            std::filesystem::perms::group_read |
            std::filesystem::perms::group_exec |
            std::filesystem::perms::others_read |
            std::filesystem::perms::others_exec
    );
    mcpp::OperationResult permission_result;
    bool permission_refused = false;
    try {
        operations.move(
            provider, {provider.stat(restricted_source.string(), {})}, provider,
            restricted_target.parent_path().string(), overwrite, {}, {},
            &permission_result
        );
    } catch (const std::exception&) {
        permission_refused = true;
    }
    std::filesystem::permissions(
        restricted_parent, std::filesystem::perms::owner_all
    );
    check(permission_refused && permission_result.completed_sources.empty(),
          "permission failure is not converted into copy fallback");
    check_unchanged(restricted_source, restricted_before);
    check_unchanged(restricted_target, restricted_destination);
    std::cout << "  [PASS] non-fallback rename errors preserve both files\n";

    if (std::filesystem::is_directory("/review/crossfs")) {
        TemporaryDirectory cross("/review/crossfs");
        struct stat source_filesystem {};
        struct stat destination_filesystem {};
        check(::stat(root.c_str(), &source_filesystem) == 0 &&
                  ::stat(cross.path().c_str(), &destination_filesystem) == 0 &&
                  source_filesystem.st_dev != destination_filesystem.st_dev,
              "cross-device regression uses a separate filesystem");
        mcpp::LocalProvider filesystem;
        const auto cross_source = root / "cross-source.txt";
        write_file(cross_source, "CROSS-DEVICE\n");
        operations.move_exact(
            filesystem, filesystem.stat(cross_source.string(), {}), filesystem,
            (cross.path() / "moved.txt").string(), overwrite, {}
        );
        check(!std::filesystem::exists(cross_source) &&
                  read_file(cross.path() / "moved.txt") == "CROSS-DEVICE\n",
              "EXDEV still permits a complete file move");
        const auto cross_tree = root / "cross-tree";
        write_file(cross_tree / "new.txt", "CROSS-TREE\n");
        write_file(cross.path() / "cross-tree" / "KEEP", "CROSS-KEEP\n");
        const auto cross_keep =
            snapshot_file(cross.path() / "cross-tree" / "KEEP");
        operations.move(
            filesystem, {filesystem.stat(cross_tree.string(), {})}, filesystem,
            cross.path().string(), overwrite, {}
        );
        check(!std::filesystem::exists(cross_tree) &&
                  read_file(cross.path() / "cross-tree" / "new.txt") ==
                      "CROSS-TREE\n",
              "EXDEV directory merge preserves unrelated destination entries");
        check_unchanged(cross.path() / "cross-tree" / "KEEP", cross_keep);
        std::cout << "  [PASS] permitted cross-device file and directory fallback\n";
    }
}

void test_atomic_symlink_copy() {
    TemporaryDirectory temporary;
    const auto root = temporary.path();
    const auto source = root / "source" / "link";
    const auto destination = root / "destination" / "link";
    const auto new_target = root / "targets" / "new";
    const auto old_target = root / "targets" / "old";
    write_file(new_target, "NEW-TARGET\n");
    write_file(old_target, "OLD-TARGET\n");
    std::filesystem::permissions(
        new_target,
        std::filesystem::perms::owner_read |
            std::filesystem::perms::owner_write
    );
    const auto new_before = snapshot_file(new_target);
    const auto old_before = snapshot_file(old_target);
    std::filesystem::create_directories(source.parent_path());
    std::filesystem::create_directories(destination.parent_path());
    std::filesystem::create_symlink("../targets/new", source);
    std::filesystem::create_symlink("../targets/old", destination);
    mcpp::LocalProvider provider(root);
    mcpp::LocalProvider second_provider(root);
    mcpp::OperationEngine operations;
    mcpp::OperationOptions options;
    options.overwrite = true;
    options.preserve_permissions = false;
    operations.copy(
        provider, {provider.stat(source.string(), {})}, provider,
        destination.parent_path().string(), options, {}
    );
    check_equal(
        std::filesystem::read_symlink(destination),
        std::filesystem::path("../targets/new"),
        "permissions-off overwrite replaces only the symbolic link"
    );
    check_unchanged(new_target, new_before);
    check_unchanged(old_target, old_before);

    options.preserve_permissions = true;
    operations.copy(
        provider, {provider.stat(source.string(), {})}, second_provider,
        destination.parent_path().string(), options, {}
    );
    check_unchanged(new_target, new_before);
    check_unchanged(old_target, old_before);

    const mcpp::Cancellation cancellation;
    cancellation.request();
    bool cancelled = false;
    try {
        provider.create_symlink(
            "../targets/old", destination.string(), true, cancellation
        );
    } catch (const mcpp::OperationCancelled&) {
        cancelled = true;
    }
    check(cancelled &&
              std::filesystem::read_symlink(destination) == "../targets/new",
          "cancelled symlink publication retains old link");
    bool rejected = false;
    try {
        provider.create_symlink(
            "../targets/old", destination.string(), false
        );
    } catch (const std::exception&) {
        rejected = true;
    }
    check(rejected &&
              std::filesystem::read_symlink(destination) == "../targets/new",
          "no-clobber symbolic link leaves existing link intact");

    write_file(root / "directory" / "KEEP", "LINK-DIRECTORY-SENTINEL\n");
    const auto keep = snapshot_file(root / "directory" / "KEEP");
    rejected = false;
    try {
        provider.create_symlink(
            "../targets/new", (root / "directory").string(), true
        );
    } catch (const std::exception&) {
        rejected = true;
    }
    check(rejected && std::filesystem::is_directory(root / "directory"),
          "symlink publication cannot replace a real directory");
    check_unchanged(root / "directory" / "KEEP", keep);

    std::atomic_bool missing{false};
    std::atomic_uint observations{0};
    std::jthread observer([&](const std::stop_token stop) {
        while (!stop.stop_requested()) {
            std::error_code error;
            const auto target = std::filesystem::read_symlink(
                destination, error
            );
            observations.fetch_add(1U, std::memory_order_relaxed);
            if (error ||
                (target != "../targets/new" && target != "../targets/old")) {
                missing.store(true, std::memory_order_relaxed);
                return;
            }
        }
    });
    while (observations.load(std::memory_order_relaxed) == 0U) {
        std::this_thread::yield();
    }
    for (unsigned int index = 0; index < 64U; ++index) {
        provider.create_symlink(
            index % 2U == 0U ? "../targets/new" : "../targets/old",
            source.string(),
            true
        );
        options.preserve_permissions = index % 2U == 0U;
        operations.copy(
            provider, {provider.stat(source.string(), {})}, provider,
            destination.parent_path().string(), options, {}
        );
    }
    observer.request_stop();
    observer.join();
    check(!missing.load(std::memory_order_relaxed),
          "atomic link replacement never exposes a missing destination");
    check_unchanged(new_target, new_before);
    check_unchanged(old_target, old_before);
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        check(!entry.path().filename().string().starts_with(".mcpp-"),
              "symlink staging is cleaned after success and failure");
    }
}

void test_operation_results() {
    TemporaryDirectory temporary;
    const auto root = temporary.path();
    write_file(root / "a" / "same.txt", "FIRST\n");
    write_file(root / "b" / "same.txt", "SECOND\n");
    std::filesystem::create_directory(root / "destination");
    auto provider = std::make_shared<mcpp::LocalProvider>(root);
    mcpp::OperationEngine operations;
    const std::vector<mcpp::Entry> sources{
        provider->stat((root / "a" / "same.txt").string(), {}),
        provider->stat((root / "b" / "same.txt").string(), {}),
    };
    const auto first = snapshot_file(sources[0].path);
    const auto second = snapshot_file(sources[1].path);
    mcpp::OperationOptions options;
    options.collision = [](const mcpp::Entry&, const mcpp::Entry&) {
        return mcpp::CollisionDecision::skip;
    };
    mcpp::OperationResult result;
    operations.copy(
        *provider, sources, *provider, (root / "destination").string(),
        options, {}, {}, &result
    );
    check_equal(result.completed_sources,
                std::vector<std::string>{sources[0].path},
                "duplicate basename reports only the completed source");
    check(result.skipped.size() == 1U &&
              result.skipped[0].path == sources[1].path &&
              result.skipped[0].reason.find("destination exists:") !=
                  std::string::npos,
          "late collision reports source and reason");
    check_equal(read_file(root / "destination" / "same.txt"), first.bytes,
                "late skip preserves first published bytes");
    check_unchanged(sources[0].path, first);
    check_unchanged(sources[1].path, second);
    mcpp::Pane pane(provider, root.string());
    pane.set_panelized(sources);
    pane.select_all();
    pane.unmark_paths(result.completed_sources);
    check(pane.marked_count() == 1U &&
              pane.selected_entries().front().path == sources[1].path,
          "only completed source marks are removed");

    const auto fifo = root / "pipe";
    check(::mkfifo(fifo.c_str(), 0600) == 0, "result FIFO fixture");
    std::filesystem::create_directory(root / "partial");
    mcpp::OperationResult partial;
    bool failed = false;
    try {
        operations.copy(
            *provider,
            {sources[0], provider->stat(fifo.string(), {})},
            *provider, (root / "partial").string(),
            options, {}, {}, &partial
        );
    } catch (const std::exception&) {
        failed = true;
    }
    check(failed &&
              partial.completed_sources ==
                  std::vector<std::string>{sources[0].path} &&
              read_file(root / "partial" / "same.txt") == first.bytes &&
              !std::filesystem::exists(root / "partial" / "pipe"),
          "completed sources survive a later operation error");
    check_unchanged(sources[0].path, first);
    check_unchanged(sources[1].path, second);

    const auto hard_source = root / "hard-source" / "a.txt";
    const auto hard_parent = root / "hard-destination";
    const auto hard_target = hard_parent / "a.txt";
    write_file(hard_source, "HARDLINK-SOURCE\n");
    std::filesystem::create_directory(hard_parent);
    std::filesystem::create_hard_link(hard_source, hard_target);
    const auto hard_before = snapshot_file(hard_source);
    check(hard_before.status.st_nlink == 2U, "hardlink fixture link count");
    const auto hard_entry = provider->stat(hard_source.string(), {});
    mcpp::Pane hard_marks(provider, hard_source.parent_path().string());
    mcpp::OperationOptions overwrite;
    overwrite.overwrite = true;
    for (int method = 0; method < 4; ++method) {
        hard_marks.select_paths({hard_entry.path});
        mcpp::OperationResult skipped;
        if (method == 0) {
            operations.move(
                *provider, {hard_entry}, *provider, hard_parent.string(),
                overwrite, {}, {}, &skipped
            );
        } else if (method == 1) {
            operations.move_exact(
                *provider, hard_entry, *provider, hard_target.string(),
                overwrite, {}, {}, &skipped
            );
        } else {
            mcpp::LocalProvider other(root);
            if (method == 2) {
                operations.move(
                    *provider, {hard_entry}, other, hard_parent.string(),
                    overwrite, {}, {}, &skipped
                );
            } else {
                operations.move_exact(
                    *provider, hard_entry, other, hard_target.string(),
                    overwrite, {}, {}, &skipped
                );
            }
        }
        check(skipped.completed_sources.empty() && skipped.skipped.size() == 1U &&
                  skipped.skipped.front().path == hard_entry.path &&
                  skipped.skipped.front().reason.find("same file") !=
                      std::string::npos,
              "same-inode move reports skip rather than successful no-op");
        check_unchanged(hard_source, hard_before);
        check_unchanged(hard_target, hard_before);
        hard_marks.unmark_paths(skipped.completed_sources);
        hard_marks.refresh();
        check(hard_marks.marked_count() == 1U &&
                  hard_marks.selected_entries().front().path == hard_entry.path,
              "same-inode move retains its original source mark");
    }
    bool direct_refused = false;
    try {
        provider->rename(hard_source.string(), hard_target.string(), true);
    } catch (const std::exception&) {
        direct_refused = true;
    }
    check(direct_refused, "provider refuses a same-inode rename no-op");
    check_unchanged(hard_source, hard_before);
    check_unchanged(hard_target, hard_before);
    std::cout << "  [PASS] hardlink normal/exact moves retain inode, links and marks\n";

    const auto late_source = root / "late-source";
    const auto late_target = root / "late-target";
    write_file(late_source, "LATE-HARDLINK\n");
    write_file(late_target, "REPLACEABLE-FIXTURE\n");
    FileSnapshot late_before;
    bool linked_late = false;
    bool late_refused = false;
    mcpp::OperationResult late_result;
    try {
        operations.move_exact(
            *provider, provider->stat(late_source.string(), {}), *provider,
            late_target.string(), overwrite, {},
            [&](const mcpp::TransferProgress& value) {
                if (!linked_late && value.files_done == 0U) {
                    std::filesystem::remove(late_target);
                    std::filesystem::create_hard_link(late_source, late_target);
                    late_before = snapshot_file(late_source);
                    linked_late = true;
                }
            },
            &late_result
        );
    } catch (const std::exception&) {
        late_refused = true;
    }
    check(linked_late && late_refused && late_result.completed_sources.empty(),
          "same-inode alias introduced after progress is refused");
    check_unchanged(late_source, late_before);
    check_unchanged(late_target, late_before);

    const auto object_target = root / "object-target";
    write_file(object_target / "KEEP", "SYMLINK-TARGET\n");
    const auto object_keep = snapshot_file(object_target / "KEEP");
    const auto object_file = root / "object-file";
    write_file(object_file, "SYMLINK-FILE-TARGET\n");
    const auto object_file_before = snapshot_file(object_file);
    for (const bool exact : {false, true}) {
        const auto name = exact ? "exact-symbol" : "normal-symbol";
        const auto source_link = root / "hard-source" / name;
        const auto target_link = hard_parent / name;
        const auto target = exact ? object_file : object_target;
        std::filesystem::create_symlink(target, source_link);
        std::filesystem::create_symlink(target, target_link);
        struct stat source_link_before {};
        check(::lstat(source_link.c_str(), &source_link_before) == 0,
              "symlink object source lstat");
        mcpp::OperationResult moved;
        if (exact) {
            operations.move_exact(
                *provider, provider->stat(source_link.string(), {}), *provider,
                target_link.string(),
                overwrite, {}, {}, &moved
            );
        } else {
            operations.move(
                *provider, {provider->stat(source_link.string(), {})}, *provider,
                hard_parent.string(), overwrite, {}, {}, &moved
            );
        }
        struct stat published_link {};
        check(moved.skipped.empty() && moved.completed_sources.size() == 1U &&
                  !std::filesystem::exists(
                      std::filesystem::symlink_status(source_link)
                  ) &&
                  ::lstat(target_link.c_str(), &published_link) == 0 &&
                  S_ISLNK(published_link.st_mode) &&
                  published_link.st_ino == source_link_before.st_ino &&
                  std::filesystem::read_symlink(target_link) == target,
              "symlink objects move without following equal targets");
        check_unchanged(object_target / "KEEP", object_keep);
        check_unchanged(object_file, object_file_before);
    }
    for (const bool exact : {false, true}) {
        const auto name = exact ? "hardlinked-exact-symbol" : "hardlinked-normal-symbol";
        const auto source_link = root / "hard-source" / name;
        const auto target_link = hard_parent / name;
        std::filesystem::create_symlink(object_file, source_link);
        check(::link(source_link.c_str(), target_link.c_str()) == 0,
              "hardlink symbolic link object");
        struct stat before {};
        check(::lstat(source_link.c_str(), &before) == 0 &&
                  S_ISLNK(before.st_mode) && before.st_nlink == 2U,
              "hardlinked symlink fixture identity");
        const auto entry = provider->stat(source_link.string(), {});
        mcpp::Pane marks(provider, source_link.parent_path().string());
        marks.select_paths({entry.path});
        mcpp::OperationResult skipped;
        if (exact) {
            operations.move_exact(
                *provider, entry, *provider, target_link.string(),
                overwrite, {}, {}, &skipped
            );
        } else {
            operations.move(
                *provider, {entry}, *provider, hard_parent.string(),
                overwrite, {}, {}, &skipped
            );
        }
        check(skipped.completed_sources.empty() && skipped.skipped.size() == 1U &&
                  skipped.skipped.front().path == entry.path,
              "hardlinked symlink move reports a same-object skip");
        for (const auto& path : {source_link, target_link}) {
            struct stat after {};
            check(::lstat(path.c_str(), &after) == 0 &&
                      after.st_dev == before.st_dev &&
                      after.st_ino == before.st_ino &&
                      after.st_mode == before.st_mode &&
                      after.st_uid == before.st_uid &&
                      after.st_gid == before.st_gid &&
                      after.st_nlink == before.st_nlink &&
                      after.st_mtim.tv_sec == before.st_mtim.tv_sec &&
                      after.st_mtim.tv_nsec == before.st_mtim.tv_nsec &&
                      std::filesystem::read_symlink(path) == object_file,
                  "hardlinked symlink names, identity and target remain unchanged");
        }
        marks.unmark_paths(skipped.completed_sources);
        marks.refresh();
        check(marks.marked_count() == 1U &&
                  marks.selected_entries().front().path == entry.path,
              "hardlinked symlink move retains its source mark");
        check_unchanged(object_file, object_file_before);
    }
    std::cout << "  [PASS] hardlinked symlink normal/exact moves retain identity and marks\n";
}

void test_viewer_text_windows() {
    TemporaryDirectory temporary;
    auto provider =
        std::make_shared<mcpp::LocalProvider>(temporary.path());
    const auto path = temporary.path() / "viewer-windows.txt";
    mcpp::ViewerModel viewer;

    write_file(path, "A\tB\nAFTER-TAB\nLAST\n");
    viewer.open(provider, path.string());
    check_equal(
        viewer.lines(80, 3),
        std::vector<std::string>{"A       B", "AFTER-TAB", "LAST"},
        "viewer expands tabs without hiding following lines"
    );
    check_equal(viewer.view_end(), viewer.size(), "tabbed displayed end");
    viewer.page(1, 80, 1);
    check_equal(viewer.offset(), std::uint64_t{4}, "tabbed next page offset");
    check_equal(
        viewer.lines(80, 1).front(),
        std::string("AFTER-TAB"),
        "tabbed page continues after its newline"
    );
    viewer.page(-1, 80, 1);
    viewer.set_wrap(false);
    viewer.scroll_columns(4);
    check_equal(
        viewer.lines(5, 1).front(),
        std::string("    B"),
        "unwrapped horizontal clipping inside a tab"
    );
    check_equal(viewer.view_end(), std::uint64_t{4}, "clipped tab byte count");

    write_file(path, "1234567\tX\nNEXT\n");
    viewer.open(provider, path.string());
    viewer.set_wrap(true);
    check_equal(
        viewer.lines(8, 2),
        std::vector<std::string>{"1234567 ", "X"},
        "tab at exact row width wraps without an extra row"
    );
    check_equal(viewer.view_end(), std::uint64_t{10}, "wrapped tab byte count");
    viewer.page(1, 8, 2);
    check_equal(viewer.offset(), std::uint64_t{10}, "wrapped tab page continuity");
    check_equal(
        viewer.lines(8, 1).front(),
        std::string("NEXT"),
        "wrapped tab page preserves the following line"
    );
    viewer.page(-1, 8, 2);
    check_equal(viewer.offset(), std::uint64_t{0}, "wrapped tab page up");

    constexpr std::size_t window_bytes = 256U * 1024U;
    constexpr std::size_t width = 80U;
    constexpr std::size_t height = 22U;
    std::string content(window_bytes + 2U * width * height, 'x');
    for (std::size_t offset = 0; offset + width <= content.size();
         offset += width) {
        const auto marker = std::to_string(offset);
        content.replace(offset, marker.size(), marker);
    }
    write_file(path, content + "\nTAIL\n");
    viewer.open(provider, path.string());
    for (int page = 0; page < 148; ++page) {
        viewer.page(1, width, height);
    }
    const auto check_block = [&](const std::size_t columns,
                                 const std::size_t rows) {
        const auto begin = viewer.offset();
        const auto displayed = viewer.lines(columns, rows);
        check_equal(displayed.size(), rows, "viewer fills page across cache edge");
        for (std::size_t row = 0; row < rows; ++row) {
            check_equal(
                displayed[row],
                content.substr(
                    static_cast<std::size_t>(begin) + row * columns,
                    columns
                ),
                "viewer preserves every byte across cache edge"
            );
        }
        check_equal(
            viewer.view_end(),
            begin + columns * rows,
            "viewer displayed end crosses cache edge"
        );
        return displayed;
    };
    const auto before_boundary = viewer.offset();
    check_equal(before_boundary, std::uint64_t{260480}, "boundary page start");
    const auto first_page = check_block(width, height);
    const auto displayed_end = viewer.view_end();
    viewer.page(1, width, height);
    check_equal(viewer.offset(), displayed_end, "next page starts at displayed end");
    static_cast<void>(check_block(width, height));
    viewer.page(-1, width, height);
    check_equal(viewer.offset(), before_boundary, "cache-edge page up offset");
    check_equal(check_block(width, height), first_page, "cache-edge page up content");

    viewer.open(provider, path.string());
    viewer.scroll_lines(static_cast<int>(window_bytes / 64U - 1U), 64);
    check_equal(viewer.offset(), window_bytes - 64U, "exact 256 KiB boundary start");
    static_cast<void>(check_block(64, 2));
    viewer.page(1, 64, 2);
    check_equal(viewer.offset(), window_bytes + 64U, "exact boundary page continuity");
    static_cast<void>(check_block(64, 2));

    viewer.open(provider, path.string());
    viewer.set_wrap(false);
    check_equal(
        viewer.lines(width, 2),
        std::vector<std::string>{content.substr(0, width), "TAIL"},
        "unwrapped huge line refills without hiding following lines"
    );
    check_equal(viewer.view_end(), viewer.size(), "unwrapped huge displayed end");
    viewer.scroll_lines(1, width);
    check_equal(
        viewer.offset(),
        content.size() + 1U,
        "unwrapped scroll skips the logical line, not the read window"
    );

    write_file(
        path,
        std::string(window_bytes - 2U, 'x') + "\n界e\u0301\tZ\nTAIL\n"
    );
    viewer.open(provider, path.string());
    viewer.scroll_lines(1, 16);
    check_equal(viewer.offset(), window_bytes - 1U, "UTF-8 straddles cache edge");
    viewer.set_wrap(true);
    check_equal(
        viewer.lines(16, 2),
        std::vector<std::string>{"界e\u0301     Z", "TAIL"},
        "cache refill preserves UTF-8, combining marks, and tabs"
    );
    check_equal(viewer.view_end(), viewer.size(), "UTF-8 source byte count");

    write_file(path, std::string(window_bytes + 32U, 'h'));
    mcpp::ViewerModel hex_viewer;
    hex_viewer.open(provider, path.string());
    hex_viewer.toggle_hex();
    hex_viewer.scroll_lines(static_cast<int>(window_bytes / 16U - 1U), width);
    const auto hex_rows = hex_viewer.lines(width, 2);
    check_equal(hex_rows.size(), std::size_t{2}, "hex fills page across cache edge");
    check(hex_rows[1].starts_with("00040000"), "hex row after exact cache boundary");
    check_equal(hex_viewer.view_end(), window_bytes + 16U, "hex displayed byte count");
}

void test_viewer_home_end() {
    TemporaryDirectory temporary;
    auto provider =
        std::make_shared<mcpp::LocalProvider>(temporary.path());
    const auto path = temporary.path() / "viewer-home-end.txt";
    std::string wrapped;
    for (int row = 0; row < 13; ++row) {
        const auto number = std::to_string(row);
        wrapped += std::string(3U - number.size(), '0') + number +
                   ":" + std::string(36, '.');
    }
    write_file(path, wrapped + "FINAL\n");
    mcpp::ViewerModel viewer;
    viewer.open(provider, path.string());
    viewer.go_to_end(40, 6);
    check_equal(viewer.offset(), std::uint64_t{320}, "End aligns wrapped last page");
    const auto last_page = viewer.lines(40, 6);
    check_equal(last_page.size(), std::size_t{6}, "End fills the final page");
    check_equal(last_page.back(), std::string("FINAL"), "End reveals final content");
    check_equal(viewer.view_end(), viewer.size(), "End displayed end");
    viewer.page(-1, 40, 6);
    check_equal(viewer.offset(), std::uint64_t{80}, "PageUp after End");
    viewer.go_to_start();
    check_equal(viewer.offset(), std::uint64_t{0}, "Home seeks first byte");
    check_equal(viewer.lines(40, 1).front(), wrapped.substr(0, 40), "Home first content");

    std::string many_lines;
    for (int row = 0; row < 20000; ++row) {
        const auto number = std::to_string(row);
        many_lines += number + std::string(15U - number.size(), 'x') + "\n";
    }
    write_file(path, many_lines);
    viewer.open(provider, path.string());
    viewer.go_to_end(80, 3);
    check_equal(viewer.offset(), std::uint64_t{19997U * 16U}, "End seeks file tail");
    check_equal(
        viewer.lines(80, 3).back(),
        many_lines.substr(many_lines.size() - 16U, 15),
        "End reveals content beyond the initial read window"
    );
    check_equal(viewer.view_end(), viewer.size(), "large End displayed end");

    write_file(path, wrapped + "FINAL\nAFTER\nLAST\n");
    viewer.open(provider, path.string());
    viewer.set_wrap(false);
    viewer.scroll_columns(12);
    viewer.go_to_end(40, 2);
    check_equal(viewer.horizontal_offset(), std::size_t{0}, "End resets column");
    check_equal(
        viewer.lines(40, 2),
        std::vector<std::string>{"AFTER", "LAST"},
        "unwrapped End uses logical last lines"
    );
    viewer.go_to_start();
    check_equal(viewer.offset(), std::uint64_t{0}, "unwrapped Home offset");

    mcpp::ViewerModel hex_viewer;
    hex_viewer.open(provider, path.string());
    hex_viewer.toggle_hex();
    hex_viewer.go_to_end(80, 2);
    check_equal(
        hex_viewer.offset(),
        ((hex_viewer.size() - 1U) / 16U - 1U) * 16U,
        "hex End aligns the last two rows"
    );
    static_cast<void>(hex_viewer.lines(80, 2));
    check_equal(hex_viewer.view_end(), hex_viewer.size(), "hex End shows final byte");
    hex_viewer.go_to_start();
    check(hex_viewer.lines(80, 1).front().starts_with("00000000"), "hex Home");

    write_file(path, "");
    viewer.open(provider, path.string());
    viewer.go_to_end(40, 6);
    viewer.go_to_start();
    check_equal(viewer.offset(), std::uint64_t{0}, "empty Home/End offset");
    check(viewer.lines(40, 6).empty(), "empty Home/End viewport");
}

void test_viewer_editor() {
    TemporaryDirectory temporary;
    const auto path = temporary.path() / "document.txt";
    write_file(path, "alpha\nbeta\ngamma\n");
    auto provider =
        std::make_shared<mcpp::LocalProvider>(temporary.path());

    mcpp::ViewerModel viewer;
    viewer.open(provider, path.string());
    const auto lines = viewer.lines(80, 10);
    check_equal(lines.size(), std::size_t{3}, "viewer lines");
    check_equal(lines[1], std::string("beta"), "viewer content");
    check(viewer.wrap(), "viewer wraps by default");
    check_equal(
        viewer.view_end(),
        viewer.size(),
        "viewer reports displayed end"
    );
    const auto match = viewer.find("gamma", true);
    check(match.has_value(), "viewer find");
    viewer.toggle_hex();
    check(viewer.hex(), "viewer hex");

    const auto link_target = temporary.path() / "linked.txt";
    const auto link_path = temporary.path() / "linked-alias";
    write_file(link_target, "linked content");
    std::filesystem::create_symlink(
        link_target.filename(),
        link_path
    );
    mcpp::ViewerModel linked_viewer;
    linked_viewer.open(provider, link_path.string());
    check_equal(
        linked_viewer.lines(80, 10).front(),
        std::string("linked content"),
        "viewer follows file symlink"
    );
    const auto wide_path = temporary.path() / "wide.txt";
    write_file(wide_path, "测试abc\n");
    mcpp::ViewerModel wide_viewer;
    wide_viewer.open(provider, wide_path.string());
    check_equal(
        wide_viewer.lines(4, 10).front(),
        std::string("测试"),
        "viewer clips at UTF-8 cell boundary"
    );
    wide_viewer.set_wrap(true);
    const auto wrapped_wide = wide_viewer.lines(4, 10);
    check_equal(
        wrapped_wide.size(),
        std::size_t{2},
        "viewer wraps UTF-8 by cell width"
    );
    check_equal(
        wrapped_wide[1],
        std::string("abc"),
        "viewer retains text after UTF-8 wrap"
    );
    const auto wrapped_path = temporary.path() / "wrapped.txt";
    write_file(
        wrapped_path,
        "abcdefghijklmnopqrstuvwxyzABCDEFGHI\nend\n"
    );
    mcpp::ViewerModel wrapped_viewer;
    wrapped_viewer.open(provider, wrapped_path.string());
    const auto first_wrapped_page = wrapped_viewer.lines(10, 2);
    check_equal(
        first_wrapped_page[0],
        std::string("abcdefghij"),
        "wrapped viewer first visual row"
    );
    check_equal(
        first_wrapped_page[1],
        std::string("klmnopqrst"),
        "wrapped viewer second visual row"
    );
    check_equal(
        wrapped_viewer.view_end(),
        std::uint64_t{20},
        "wrapped viewer reports displayed bytes only"
    );
    wrapped_viewer.page(1, 10, 2);
    check_equal(
        wrapped_viewer.lines(10, 2)[0],
        std::string("uvwxyzABCD"),
        "wrapped viewer page continues without skipping"
    );
    wrapped_viewer.page(-1, 10, 2);
    check_equal(
        wrapped_viewer.offset(),
        std::uint64_t{0},
        "wrapped viewer page up restores prior offset"
    );
    wrapped_viewer.scroll_lines(1, 10);
    check_equal(
        wrapped_viewer.lines(10, 2)[0],
        std::string("klmnopqrst"),
        "wrapped viewer scrolls by visual row"
    );
    const auto exact_width_path =
        temporary.path() / "exact-width.txt";
    write_file(exact_width_path, "abcdefghij\nnext\n");
    mcpp::ViewerModel exact_width_viewer;
    exact_width_viewer.open(provider, exact_width_path.string());
    static_cast<void>(exact_width_viewer.lines(10, 1));
    exact_width_viewer.scroll_lines(1, 10);
    check_equal(
        exact_width_viewer.lines(10, 1)[0],
        std::string("next"),
        "exact-width viewer row consumes its newline"
    );
    exact_width_viewer.set_wrap(false);
    exact_width_viewer.scroll_lines(-1, 10);
    exact_width_viewer.scroll_columns(3);
    check_equal(
        exact_width_viewer.lines(5, 1)[0],
        std::string("defgh"),
        "unwrapped viewer scrolls horizontally"
    );
    exact_width_viewer.scroll_columns(-2);
    check_equal(
        exact_width_viewer.horizontal_offset(),
        std::size_t{1},
        "viewer tracks horizontal offset"
    );
    const auto hex_scroll_path =
        temporary.path() / "hex-scroll.txt";
    write_file(
        hex_scroll_path,
        "0123456789abcdef0123456789abcdef"
    );
    mcpp::ViewerModel hex_scroll_viewer;
    hex_scroll_viewer.open(provider, hex_scroll_path.string());
    hex_scroll_viewer.scroll_lines(3, 8);
    check_equal(
        hex_scroll_viewer.offset(),
        std::uint64_t{24},
        "viewer reaches wrapped byte offset"
    );
    hex_scroll_viewer.toggle_hex();
    hex_scroll_viewer.scroll_lines(-1, 8);
    check_equal(
        hex_scroll_viewer.offset(),
        std::uint64_t{8},
        "hex viewer scrolls backward by sixteen bytes"
    );
    const auto repeat_search_path =
        temporary.path() / "viewer-search.txt";
    write_file(
        repeat_search_path,
        "prefix needle suffix\nsecond needle\n"
    );
    mcpp::ViewerModel repeat_search_viewer;
    repeat_search_viewer.open(provider, repeat_search_path.string());
    const auto first_viewer_match =
        repeat_search_viewer.find("needle", true);
    check(first_viewer_match.has_value(), "viewer first search");
    check_equal(
        repeat_search_viewer.lines(80, 1)[0],
        std::string("prefix needle suffix"),
        "viewer search retains line prefix"
    );
    const auto second_viewer_match =
        repeat_search_viewer.find("needle", true);
    check(
        second_viewer_match.has_value() &&
            *second_viewer_match > *first_viewer_match,
        "viewer search repeats forward"
    );
    const auto wrapped_viewer_match =
        repeat_search_viewer.find("needle", true);
    check_equal(
        wrapped_viewer_match,
        first_viewer_match,
        "viewer search wraps to beginning"
    );

    mcpp::EditorModel editor;
    editor.open(provider, path.string());
    editor.move(1, 4);
    editor.insert('!');
    editor.insert("é");
    editor.backspace();
    check(editor.undo(), "editor UTF-8 undo");
    check(editor.redo(), "editor UTF-8 redo");
    check(editor.dirty(), "editor dirty");
    check(editor.undo(), "editor undo");
    check(editor.redo(), "editor redo");
    editor.save(false);
    check_equal(
        read_file(path),
        std::string("alpha\nbeta!\ngamma\n"),
        "editor save"
    );

    const auto rich_path = temporary.path() / "rich.txt";
    write_file(rich_path, "alpha beta gamma\nsecond line\n");
    editor.open(provider, rich_path.string());
    editor.toggle_mark();
    editor.move(0, 5);
    editor.toggle_mark();
    editor.end();
    check(editor.copy_selection(), "editor block copy");
    check_equal(
        editor.lines()[0],
        std::string("alpha beta gammaalpha"),
        "editor copied block at cursor"
    );
    check(editor.selection().has_value(), "editor keeps copied block");
    check(editor.copy_selection(), "editor repeats block copy");
    check_equal(
        editor.lines()[0],
        std::string("alpha beta gammaalphaalpha"),
        "editor repeated persistent block copy"
    );
    check(editor.undo(), "editor repeated copy undo");
    check(editor.undo(), "editor block copy undo");
    editor.insert('!');
    check_equal(
        editor.lines()[0],
        std::string("alpha beta gamma!"),
        "typing does not replace persistent block"
    );
    check(
        editor.selection().has_value(),
        "typing preserves marked block"
    );
    check(editor.undo(), "editor persistent block typing undo");
    editor.backspace();
    check_equal(
        editor.lines()[0],
        std::string("alpha beta gamm"),
        "backspace edits outside persistent block"
    );
    check(
        editor.selection().has_value(),
        "backspace preserves marked block"
    );
    check(editor.undo(), "editor persistent block backspace undo");
    check(
        editor.replace("beta", "BETA", true),
        "editor replace"
    );
    check_equal(
        editor.lines()[0],
        std::string("alpha BETA gamma"),
        "editor replaced text"
    );
    editor.home();
    editor.toggle_mark();
    editor.move(0, 5);
    editor.toggle_mark();
    editor.goto_line(2);
    editor.end();
    check(editor.move_selection(), "editor block move");
    check_equal(
        editor.lines()[0],
        std::string(" BETA gamma"),
        "editor move removed source block"
    );
    check_equal(
        editor.lines()[1],
        std::string("second linealpha"),
        "editor move inserted block"
    );
    const auto saved_as = temporary.path() / "saved-as.txt";
    editor.save_as(saved_as.string(), false);
    check_equal(
        read_file(saved_as),
        std::string(" BETA gamma\nsecond linealpha\n"),
        "editor save as"
    );
    editor.insert('!');
    editor.save_as("relative.txt", false);
    check_equal(
        read_file(temporary.path() / "relative.txt"),
        std::string(" BETA gamma\nsecond linealpha!\n"),
        "relative save as uses edited file directory"
    );
    const auto created_path = temporary.path() / "created.txt";
    editor.create(provider, created_path.string());
    editor.insert("created\n");
    editor.save(false);
    check_equal(
        read_file(created_path),
        std::string("created\n"),
        "editor creates new file"
    );
    const auto new_target = temporary.path() / "new-target.txt";
    const auto saved_target = temporary.path() / "saved-target.txt";
    editor.create(provider, new_target.string());
    editor.insert("first");
    editor.save_as(saved_target.string(), false);
    editor.insert(" second");
    editor.save(false);
    check_equal(
        read_file(saved_target),
        std::string("first second"),
        "save as establishes normal save target"
    );
    const auto stable_path = temporary.path() / "stable.txt";
    write_file(stable_path, "stable");
    editor.open(provider, stable_path.string());
    bool missing_rejected = false;
    try {
        editor.open(
            provider,
            (temporary.path() / "missing.txt").string()
        );
    } catch (const std::runtime_error&) {
        missing_rejected = true;
    }
    check(
        missing_rejected &&
            editor.path() == stable_path.string() &&
            editor.lines()[0] == "stable",
        "failed open preserves current buffer identity"
    );
    editor.end();
    editor.insert('!');
    editor.save(false);
    check_equal(
        read_file(stable_path),
        std::string("stable!"),
        "failed open preserves save target"
    );

    const auto utf_path = temporary.path() / "utf.txt";
    write_file(utf_path, "abc\néxyz\n");
    editor.open(provider, utf_path.string());
    editor.move(0, 1);
    editor.goto_line(2);
    check_equal(
        editor.column(),
        std::size_t{0},
        "goto aligns UTF-8 cursor"
    );
    editor.erase();
    check_equal(
        editor.lines()[1],
        std::string("xyz"),
        "UTF-8 erase after goto"
    );

    const auto mark_path = temporary.path() / "mark.txt";
    write_file(mark_path, "abc\nxyz\n");
    editor.open(provider, mark_path.string());
    editor.goto_line(2);
    editor.home();
    editor.toggle_mark();
    editor.backspace();
    editor.move(0, -1);
    editor.erase();
    check_equal(
        editor.lines()[0],
        std::string("abxyz"),
        "backspace clears collapsed mark"
    );

    const auto overwrite_path = temporary.path() / "overwrite.txt";
    write_file(overwrite_path, "abc\n");
    editor.open(provider, overwrite_path.string());
    editor.toggle_overwrite();
    check(editor.overwrite(), "editor overwrite mode");
    editor.insert('X');
    check_equal(
        editor.lines()[0],
        std::string("Xbc"),
        "editor overwrite replaces character"
    );
    editor.end();
    editor.insert('!');
    check_equal(
        editor.lines()[0],
        std::string("Xbc!"),
        "editor overwrite inserts at end"
    );
    const auto column_path = temporary.path() / "columns.txt";
    write_file(column_path, "123456\nx\n123456\n");
    editor.open(provider, column_path.string());
    editor.move(0, 5);
    editor.move(1, 0);
    check_equal(
        editor.column(),
        std::size_t{1},
        "vertical move clamps on short line"
    );
    editor.move(1, 0);
    check_equal(
        editor.column(),
        std::size_t{5},
        "vertical move restores preferred column"
    );
    write_file(column_path, "abcde\nééééé\nabcde\n");
    editor.open(provider, column_path.string());
    editor.end();
    editor.move(1, 0);
    check_equal(
        editor.column(),
        std::size_t{10},
        "vertical move preserves UTF-8 display column"
    );
    editor.move(1, 0);
    check_equal(
        editor.column(),
        std::size_t{5},
        "vertical move restores ASCII display column"
    );
    const auto bracket_path = temporary.path() / "brackets.txt";
    write_file(bracket_path, "call(one[two]);\n");
    editor.open(provider, bracket_path.string());
    editor.move(0, 4);
    check(editor.match_bracket(), "editor matching bracket");
    check_equal(
        editor.column(),
        std::size_t{13},
        "editor matching bracket target"
    );
    editor.select_all();
    check_equal(
        editor.selection_size(),
        editor.byte_size(),
        "editor mark all"
    );
    editor.clear_selection();
    check(
        !editor.selection().has_value(),
        "editor unmark"
    );
    const auto column_block_path =
        temporary.path() / "column-block.txt";
    write_file(column_block_path, "abcdef\nab\na bcdef\n");
    editor.open(provider, column_block_path.string());
    editor.move(0, 1);
    editor.toggle_column_mark();
    editor.move(0, 2);
    editor.move(2, 0);
    editor.toggle_column_mark();
    check(editor.column_selection(), "editor column block mode");
    check_equal(
        *editor.selected_text(),
        std::string("bc\nb\n b"),
        "editor rectangular block text"
    );
    check(editor.delete_selection(), "delete rectangular block");
    check_equal(
        editor.lines()[0],
        std::string("adef"),
        "column delete first row"
    );
    check_equal(
        editor.lines()[1],
        std::string("a"),
        "column delete short row"
    );
    check_equal(
        editor.lines()[2],
        std::string("acdef"),
        "column delete final row"
    );
    check(editor.undo(), "undo rectangular delete");
    editor.clear_selection();
    editor.goto_line(1);
    editor.home();
    editor.paste_column("X\nY\nZ");
    check_equal(
        editor.lines()[0],
        std::string("Xabcdef"),
        "column paste first row"
    );
    check_equal(
        editor.lines()[2],
        std::string("Za bcdef"),
        "column paste final row"
    );
    write_file(column_block_path, "abcdef\nuvwxyz\n");
    editor.open(provider, column_block_path.string());
    editor.move(0, 2);
    editor.toggle_column_mark();
    editor.move(0, 2);
    editor.move(1, 0);
    editor.toggle_column_mark();
    editor.set_position({.row = 0, .column = 0});
    check(editor.copy_selection(), "first column copy");
    check(editor.copy_selection(), "repeated column copy");
    check_equal(
        editor.lines()[0],
        std::string("cdcdabcdef"),
        "repeated column copy first row"
    );
    check_equal(
        editor.lines()[1],
        std::string("wxwxuvwxyz"),
        "repeated column copy second row"
    );
    write_file(column_block_path, "abc\ndef\n");
    editor.open(provider, column_block_path.string());
    editor.move(0, 1);
    editor.toggle_column_mark();
    editor.move(1, 0);
    editor.toggle_column_mark();
    check(
        !editor.copy_selection(),
        "zero-width column copy is rejected"
    );
    check(
        editor.delete_selection(),
        "zero-width column delete is handled"
    );
    check_equal(
        editor.lines()[0],
        std::string("abc"),
        "zero-width column delete keeps text"
    );
    write_file(column_block_path, "abcdefgh\n\tfoo\n");
    editor.open(provider, column_block_path.string());
    editor.set_tab_width(8);
    editor.move(0, 6);
    editor.paste_column("X\nY");
    check_equal(
        editor.lines()[1],
        std::string("      Y\tfoo"),
        "column paste uses configured tab width"
    );
    check_equal(
        editor.tab_width(),
        std::size_t{8},
        "editor reports configured tab width"
    );
    write_file(column_block_path, "é\tX\nabcdefgh\n");
    editor.open(provider, column_block_path.string());
    editor.move(0, 2);
    editor.move(1, 0);
    check_equal(
        editor.column(),
        std::size_t{8},
        "vertical move preserves UTF-8 tab display column"
    );
    editor.move(-1, 0);
    check_equal(
        editor.column(),
        std::size_t{3},
        "vertical move returns to configured tab boundary"
    );
    editor.set_tab_width(4);
    const auto indent_path = temporary.path() / "indent.txt";
    write_file(indent_path, "    alpha");
    editor.open(provider, indent_path.string());
    editor.end();
    editor.newline(true);
    editor.insert('x');
    check_equal(
        editor.lines()[1],
        std::string("    x"),
        "auto-indent copies leading whitespace"
    );
    editor.open(provider, indent_path.string());
    editor.end();
    editor.newline(false);
    editor.insert('x');
    check_equal(
        editor.lines()[1],
        std::string("x"),
        "disabled auto-indent starts at column zero"
    );
    write_file(overwrite_path, "abcdef");
    editor.open(provider, overwrite_path.string());
    editor.move(0, 2);
    editor.toggle_overwrite();
    editor.paste("ab");
    check_equal(
        editor.lines()[0],
        std::string("ababcdef"),
        "clipboard paste ignores overwrite mode"
    );

    const auto selection_path = temporary.path() / "selection.txt";
    write_file(selection_path, "abcd\n");
    editor.open(provider, selection_path.string());
    editor.move(0, 1);
    editor.toggle_mark();
    editor.move(0, 2);
    editor.toggle_mark();
    editor.move(0, -2);
    editor.insert('X');
    check(editor.delete_selection(), "delete typed-in block");
    check_equal(
        editor.lines()[0],
        std::string("ad"),
        "typing at block start extends selection"
    );
    editor.open(provider, selection_path.string());
    editor.move(0, 1);
    editor.toggle_mark();
    editor.move(0, 2);
    editor.toggle_mark();
    check(editor.copy_selection(), "copy at block end");
    check(editor.copy_selection(), "repeat copy at block end");
    check(editor.delete_selection(), "delete copied block");
    check_equal(
        editor.lines()[0],
        std::string("ad"),
        "copy at block end extends selection"
    );

    const auto active_mark_path =
        temporary.path() / "active-mark.txt";
    write_file(active_mark_path, "abcde\n");
    editor.open(provider, active_mark_path.string());
    editor.toggle_mark();
    editor.move(0, 2);
    editor.insert('X');
    check(editor.marking(), "editing keeps active mark");
    editor.move(0, 1);
    editor.toggle_mark();
    check(editor.delete_selection(), "delete completed active mark");
    check_equal(
        editor.lines()[0],
        std::string("de"),
        "active mark tracks edits"
    );

    const auto line_path = temporary.path() / "lines.txt";
    write_file(line_path, "ab\néé");
    editor.open(provider, line_path.string());
    editor.move(0, 1);
    editor.delete_line();
    editor.insert('X');
    check_equal(
        editor.lines()[0],
        std::string("Xéé"),
        "line deletion aligns UTF-8 cursor"
    );
    write_file(line_path, "ab\ncd");
    editor.open(provider, line_path.string());
    editor.goto_line(2);
    editor.delete_line();
    editor.save(false);
    check_equal(
        read_file(line_path),
        std::string("ab\n"),
        "last-line deletion keeps preceding newline"
    );

    const auto cross_line_path =
        temporary.path() / "cross-line.txt";
    write_file(cross_line_path, "abc\ndef");
    editor.open(provider, cross_line_path.string());
    editor.end();
    editor.begin_mark();
    editor.move(0, 2);
    editor.finish_mark();
    check(editor.delete_selection(), "delete cross-line mark");
    check_equal(
        editor.lines()[0],
        std::string("abcef"),
        "horizontal marking crosses newline"
    );

    const auto overlap_path = temporary.path() / "overlap.txt";
    write_file(overlap_path, "ababa");
    editor.open(provider, overlap_path.string());
    check(editor.find("aba", true), "find first overlap");
    check(
        editor.find("aba", true, false, true),
        "find overlapping match"
    );
    check_equal(
        editor.column(),
        std::size_t{2},
        "overlapping search starts after match start"
    );

    const auto replace_all_path =
        temporary.path() / "replace-all.txt";
    std::string many_matches;
    for (int index = 0; index < 120; ++index) {
        many_matches += "a ";
    }
    write_file(replace_all_path, many_matches);
    editor.open(provider, replace_all_path.string());
    check(editor.find("a", true), "find replace-all start");
    check_equal(
        editor.replace_all("a", "b", true),
        std::size_t{120},
        "replace all count"
    );
    check(editor.undo(), "replace all single undo");
    editor.save(false);
    check_equal(
        read_file(replace_all_path),
        many_matches,
        "replace all undo restores every match"
    );

    const auto scoped_path = temporary.path() / "scoped.txt";
    write_file(scoped_path, "a a a");
    editor.open(provider, scoped_path.string());
    std::optional<std::pair<
        mcpp::EditorPosition,
        mcpp::EditorPosition
    >> scope{
        std::pair{
            mcpp::EditorPosition{.row = 0, .column = 0},
            mcpp::EditorPosition{.row = 0, .column = 3},
        }
    };
    const mcpp::EditorSearchOptions scoped_options{
        .case_sensitive = true,
    };
    check(
        editor.find(
            "a",
            scoped_options,
            false,
            false,
            scope
        ),
        "find first scoped match"
    );
    check(
        editor.replace_selection("", &scope),
        "replace first scoped match"
    );
    check(
        editor.find(
            "a",
            scoped_options,
            false,
            false,
            scope
        ),
        "find rebased scoped match"
    );
    check(
        editor.replace_selection("", &scope),
        "replace second scoped match"
    );
    check(
        !editor.find(
            "a",
            scoped_options,
            false,
            false,
            scope
        ),
        "scoped replacement stays inside original block"
    );
    check_equal(
        editor.lines()[0],
        std::string("  a"),
        "scoped replacement keeps outside match"
    );

    editor.open(provider, path.string());
    editor.insert('?');
    write_file(path, "external");
    bool conflict = false;
    try {
        editor.save(false);
    } catch (const std::runtime_error&) {
        conflict = true;
    }
    check(conflict, "editor conflict");

    editor.open(provider, link_path.string());
    editor.end();
    editor.insert('!');
    editor.save(false);
    check(
        std::filesystem::is_symlink(
            std::filesystem::symlink_status(link_path)
        ),
        "editor preserves file symlink"
    );
    check_equal(
        read_file(link_target),
        std::string("linked content!"),
        "editor saves through file symlink"
    );
}

void test_config() {
    TemporaryDirectory temporary;
    const auto path = temporary.path() / "config";
    mcpp::Config config;
    config.show_hidden = true;
    config.confirm_delete = false;
    config.sort = mcpp::SortField::size;
    config.sort_descending = true;
    config.theme = "blue";
    config.editor_syntax = false;
    config.editor_line_numbers = false;
    config.editor_tab_width = 8;
    config.editor_indent_width = 2;
    config.editor_auto_indent = false;
    config.editor_show_whitespace = true;
    config.bookmarks = {
        "/one",
        "/two\nconfirm_delete=true",
    };
    config.editor_history = {"/edited/one", "/edited/two"};
    config.save(path);
    const auto loaded = mcpp::Config::load(path);
    check(loaded.show_hidden, "config hidden");
    check(!loaded.confirm_delete, "config confirmation");
    check_equal(loaded.sort, mcpp::SortField::size, "config sort");
    check(loaded.sort_descending, "config sort direction");
    check_equal(loaded.theme, std::string("blue"), "config theme");
    check(
        !loaded.editor_syntax && !loaded.editor_line_numbers,
        "config editor display settings"
    );
    check_equal(
        loaded.editor_tab_width,
        std::size_t{8},
        "config editor tab width"
    );
    check_equal(
        loaded.editor_indent_width,
        std::size_t{2},
        "config editor indent width"
    );
    check(
        !loaded.editor_auto_indent &&
            loaded.editor_show_whitespace,
        "config editor whitespace settings"
    );
    check_equal(
        loaded.bookmarks.size(),
        std::size_t{2},
        "config bookmarks"
    );
    check(
        !loaded.confirm_delete &&
            loaded.bookmarks[1] ==
                "/two\nconfirm_delete=true",
        "config escapes record separators"
    );
    check(
        loaded.editor_history == config.editor_history,
        "config editor history"
    );
    struct stat status {};
    check(
        ::stat(path.c_str(), &status) == 0 &&
            (status.st_mode & 0777) == 0600,
        "config file permissions"
    );
    const auto victim = temporary.path() / "victim";
    const auto linked_config = temporary.path() / "linked-config";
    write_file(victim, "victim");
    std::filesystem::create_symlink(victim, linked_config);
    config.save(linked_config);
    check_equal(
        read_file(victim),
        std::string("victim"),
        "config save does not follow destination symlink"
    );
    check(
        std::filesystem::is_regular_file(
            std::filesystem::symlink_status(linked_config)
        ),
        "config save replaces symlink itself"
    );
}

void test_editor_clipboard() {
    TemporaryDirectory temporary;
    const auto path = temporary.path() / "cache" / "clip";
    mcpp::save_editor_clipboard(path, "alpha\nbeta");
    check_equal(
        mcpp::load_editor_clipboard(path),
        std::string("alpha\nbeta"),
        "persistent editor clipboard"
    );
    mcpp::save_editor_clipboard(path, "left\nright", true);
    bool column = false;
    check_equal(
        mcpp::load_editor_clipboard(path, &column),
        std::string("left\nright"),
        "persistent column clipboard"
    );
    check(column, "persistent clipboard column metadata");
    const std::string maximum_clipboard(
        16U * 1024U * 1024U,
        'x'
    );
    mcpp::save_editor_clipboard(
        path,
        maximum_clipboard,
        true
    );
    bool maximum_column = false;
    const auto loaded_maximum =
        mcpp::load_editor_clipboard(path, &maximum_column);
    check_equal(
        loaded_maximum.size(),
        maximum_clipboard.size(),
        "maximum clipboard payload remains loadable"
    );
    check(
        maximum_column,
        "maximum clipboard retains column metadata"
    );
    bool oversized_rejected = false;
    try {
        mcpp::save_editor_clipboard(
            path,
            std::string(maximum_clipboard.size() + 1U, 'x')
        );
    } catch (const std::runtime_error&) {
        oversized_rejected = true;
    }
    check(
        oversized_rejected,
        "oversized clipboard is rejected before saving"
    );
    struct stat status {};
    check(
        ::stat(path.c_str(), &status) == 0 &&
            (status.st_mode & 0777) == 0600,
        "editor clipboard permissions"
    );
    const auto victim = temporary.path() / "victim";
    write_file(victim, "victim");
    std::filesystem::remove(path);
    std::filesystem::create_symlink(victim, path);
    mcpp::save_editor_clipboard(path, "replacement");
    check_equal(
        read_file(victim),
        std::string("victim"),
        "clipboard save does not follow symlink"
    );
    check_equal(
        mcpp::load_editor_clipboard(path),
        std::string("replacement"),
        "clipboard save replaces symlink"
    );
}

void test_search_compare() {
    TemporaryDirectory temporary;
    const auto left = temporary.path() / "left";
    const auto right = temporary.path() / "right";
    write_file(left / "a.txt", "alpha needle");
    write_file(left / "sub" / "b.txt", "bravo");
    write_file(right / "a.txt", "alpha needle");
    write_file(right / "sub" / "b.txt", "changed");
    write_file(right / "extra.txt", "extra");
    mcpp::LocalProvider provider(temporary.path());

    const mcpp::SearchEngine search;
    const auto results = search.search(
        provider,
        left.string(),
        {
            .name_pattern = "*.txt",
            .content = "needle",
            .case_sensitive = false,
            .recursive = true,
            .include_hidden = false,
        },
        {}
    );
    check_equal(results.size(), std::size_t{1}, "search result count");
    check_equal(results[0].name, std::string("a.txt"), "search result");

    auto deep = left / "deep";
    std::filesystem::create_directories(deep);
    for (int depth = 0; depth < 128; ++depth) {
        deep /= "d";
        std::filesystem::create_directory(deep);
    }
    write_file(deep / "leaf.match", "deep");
    const auto deep_results = search.search(
        provider,
        left.string(),
        {
            .name_pattern = "leaf.match",
            .content = {},
            .case_sensitive = true,
            .recursive = true,
            .include_hidden = false,
        },
        {}
    );
    check_equal(
        deep_results.size(),
        std::size_t{1},
        "iterative search handles deep trees"
    );

    const mcpp::DirectoryComparator comparator;
    const auto differences = comparator.compare(
        provider,
        left.string(),
        provider,
        right.string(),
        true,
        {}
    );
    check(
        std::any_of(
            differences.begin(),
            differences.end(),
            [](const mcpp::Difference& difference) {
                return difference.relative_path == "extra.txt" &&
                       difference.kind ==
                           mcpp::DifferenceKind::right_only;
            }
        ),
        "compare right-only"
    );
    check(
        std::any_of(
            differences.begin(),
            differences.end(),
            [](const mcpp::Difference& difference) {
                return difference.relative_path == "sub/b.txt" &&
                       difference.kind ==
                           mcpp::DifferenceKind::size;
            }
        ),
        "compare changed size"
    );
    check(
        comparator.compare(
            provider,
            left.string(),
            provider,
            left.string(),
            true,
            {}
        ).empty(),
        "identical directories compare equal"
    );
    const auto left_alias = temporary.path() / "left-alias";
    std::filesystem::create_directory_symlink(left, left_alias);
    check(
        comparator.compare(
            provider,
            left_alias.string(),
            provider,
            left.string(),
            true,
            {}
        ).empty(),
        "directory symlink roots compare by relative names"
    );
}

void test_syntax() {
    const mcpp::SyntaxHighlighter highlighter;
    const auto cpp = highlighter.highlight(
        "sample.cpp",
        {
            "const int value = 42; // note",
            "/* block",
            "comment */ return \"done\";",
        }
    );
    const auto has_style = [](const std::vector<mcpp::SyntaxSpan>& spans,
                              const mcpp::SyntaxStyle style) {
        return std::ranges::any_of(
            spans,
            [&](const mcpp::SyntaxSpan& span) {
                return span.style == style;
            }
        );
    };
    check(
        has_style(cpp[0], mcpp::SyntaxStyle::keyword) &&
            has_style(cpp[0], mcpp::SyntaxStyle::type) &&
            has_style(cpp[0], mcpp::SyntaxStyle::number) &&
            has_style(cpp[0], mcpp::SyntaxStyle::comment),
        "C++ syntax classes"
    );
    check(
        has_style(cpp[1], mcpp::SyntaxStyle::comment) &&
            has_style(cpp[2], mcpp::SyntaxStyle::comment) &&
            has_style(cpp[2], mcpp::SyntaxStyle::string_literal),
        "multiline syntax state"
    );
    const auto directive_comment = highlighter.highlight(
        "directive.cpp",
        {"/* banner", "# text */", "int value = 42;"}
    );
    check(
        has_style(
            directive_comment[1],
            mcpp::SyntaxStyle::comment
        ) &&
            !has_style(
                directive_comment[2],
                mcpp::SyntaxStyle::comment
            ) &&
            has_style(
                directive_comment[2],
                mcpp::SyntaxStyle::type
            ),
        "preprocessor line closes block comment"
    );
    const auto python = highlighter.highlight(
        "sample.py",
        {"def greet(name): # comment", "    return \"hi\""}
    );
    check(
        has_style(python[0], mcpp::SyntaxStyle::keyword) &&
            has_style(python[0], mcpp::SyntaxStyle::comment) &&
            has_style(python[1], mcpp::SyntaxStyle::string_literal),
        "Python syntax classes"
    );
}

void test_editor_search() {
    const std::string text = "Alpha alpha\nfoobar foo\n";
    const auto normal = mcpp::EditorSearch::find(
        text,
        "alpha",
        0,
        0,
        text.size(),
        {},
        false,
        false
    );
    check(
        normal.has_value() && normal->begin == 0U,
        "case-insensitive editor search"
    );
    const mcpp::EditorSearchOptions case_sensitive{
        .case_sensitive = true,
    };
    const auto exact = mcpp::EditorSearch::find(
        text,
        "alpha",
        0,
        0,
        text.size(),
        case_sensitive,
        false,
        false
    );
    check(
        exact.has_value() && exact->begin == 6U,
        "case-sensitive editor search"
    );
    const mcpp::EditorSearchOptions backwards{
        .backwards = true,
    };
    const auto reverse = mcpp::EditorSearch::find(
        text,
        "alpha",
        text.size(),
        0,
        text.size(),
        backwards,
        false,
        false
    );
    check(
        reverse.has_value() && reverse->begin == 6U,
        "backwards editor search"
    );
    const mcpp::EditorSearchOptions whole{
        .case_sensitive = true,
        .whole_words = true,
    };
    const auto word = mcpp::EditorSearch::find(
        text,
        "foo",
        0,
        0,
        text.size(),
        whole,
        false,
        false
    );
    check(
        word.has_value() && word->begin == 19U,
        "whole-word editor search"
    );
    const mcpp::EditorSearchOptions regex{
        .mode = mcpp::EditorSearchMode::regular_expression,
        .case_sensitive = true,
    };
    const auto expression = mcpp::EditorSearch::find(
        text,
        "f(o+)bar",
        0,
        0,
        text.size(),
        regex,
        false,
        false
    );
    check(expression.has_value(), "regular-expression search");
    check_equal(
        mcpp::EditorSearch::find_all(
            "foofoo",
            "^foo",
            0,
            6,
            regex
        ).size(),
        std::size_t{1},
        "regular-expression anchors"
    );
    check_equal(
        mcpp::EditorSearch::replacement(
            "foobar",
            {.begin = 0, .end = 6},
            "f(o+)bar",
            "b$1m",
            regex
        ),
        std::string("boom"),
        "regular-expression replacement"
    );
    const mcpp::EditorSearchOptions wildcard{
        .mode = mcpp::EditorSearchMode::wildcard,
        .case_sensitive = true,
    };
    check(
        mcpp::EditorSearch::find(
            text,
            "f*bar",
            0,
            0,
            text.size(),
            wildcard,
            false,
            false
        ).has_value(),
        "wildcard editor search"
    );
    const std::string long_line(100000U, 'a');
    check(
        mcpp::EditorSearch::find(
            long_line,
            "*",
            0,
            0,
            long_line.size(),
            wildcard,
            false,
            false
        ).has_value(),
        "stack-safe wildcard search"
    );
    const mcpp::EditorSearchOptions hexadecimal{
        .mode = mcpp::EditorSearchMode::hexadecimal,
    };
    const auto bytes = mcpp::EditorSearch::find(
        text,
        "41 6c",
        0,
        0,
        text.size(),
        hexadecimal,
        false,
        false
    );
    check(
        bytes.has_value() && bytes->begin == 0U,
        "hexadecimal editor search"
    );
}

void test_user_menu() {
    TemporaryDirectory temporary;
    const auto menu = temporary.path() / "menu";
    write_file(
        menu,
        "# comment\n"
        "x|Example command|printf '%s' %f\n"
        "invalid\n"
    );
    std::filesystem::permissions(
        menu,
        std::filesystem::perms::owner_read |
            std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace
    );
    const auto entries = mcpp::load_user_menu(menu);
    check_equal(entries.size(), std::size_t{1}, "user menu count");
    check_equal(entries[0].key, 'x', "user menu key");
    const auto expanded = mcpp::expand_user_menu_command(
        entries[0].command,
        "/tmp/it's here",
        "/tmp/current",
        "/tmp/other"
    );
    check(
        expanded.find("'/tmp/it'\\''s here'") !=
            std::string::npos,
        "user menu shell quoting"
    );
    const auto token_path = "/tmp/literal-%d-%D";
    const auto token_expanded = mcpp::expand_user_menu_command(
        "printf %f",
        token_path,
        "/current",
        "/other"
    );
    check(
        token_expanded.find(token_path) != std::string::npos,
        "user menu does not re-expand inserted paths"
    );
    bool quoted_placeholder_rejected = false;
    try {
        static_cast<void>(mcpp::expand_user_menu_command(
            "printf \"%f\"",
            "$(printf INJECTED)",
            "/current",
            "/other"
        ));
    } catch (const std::invalid_argument&) {
        quoted_placeholder_rejected = true;
    }
    check(
        quoted_placeholder_rejected,
        "quoted user-menu placeholder rejection"
    );
    std::filesystem::permissions(
        menu,
        std::filesystem::perms::owner_read |
            std::filesystem::perms::owner_write |
            std::filesystem::perms::group_write,
        std::filesystem::perm_options::replace
    );
    bool insecure_rejected = false;
    try {
        static_cast<void>(mcpp::load_user_menu(menu));
    } catch (const std::runtime_error&) {
        insecure_rejected = true;
    }
    check(insecure_rejected, "insecure user menu rejection");

    const auto fifo = temporary.path() / "menu-fifo";
    check(::mkfifo(fifo.c_str(), 0600) == 0, "create menu FIFO");
    bool fifo_rejected = false;
    try {
        static_cast<void>(mcpp::load_user_menu(fifo));
    } catch (const std::runtime_error&) {
        fifo_rejected = true;
    }
    check(fifo_rejected, "menu FIFO rejection");
}

}  // namespace

int main() {
    std::vector<std::pair<
        std::string_view,
        void (*)()
    >> tests{
        {"local provider", test_local_provider},
        {"pane", test_pane},
        {"special paths", test_special_paths},
        {"operations", test_operations},
        {"viewer text windows", test_viewer_text_windows},
        {"viewer Home/End", test_viewer_home_end},
        {"viewer/editor", test_viewer_editor},
        {"config", test_config},
        {"editor clipboard", test_editor_clipboard},
        {"search/compare", test_search_compare},
        {"syntax", test_syntax},
        {"editor search", test_editor_search},
        {"user menu", test_user_menu},
    };
    const auto* disposable = std::getenv("MCPP_DISPOSABLE_TESTS");
    const auto* temporary_root = std::getenv("TMPDIR");
    if (disposable != nullptr &&
        std::string_view(disposable) == "1" &&
        temporary_root != nullptr &&
        std::filesystem::path(temporary_root).lexically_normal() ==
            std::filesystem::path("/review/runtime") &&
        std::filesystem::exists("/.dockerenv") &&
        std::filesystem::exists(
            "/review/.operation-regressions-disposable"
        )) {
        tests.emplace_back("operation destinations", test_operation_destinations);
        tests.emplace_back("atomic symbolic links", test_atomic_symlink_copy);
        tests.emplace_back("operation results", test_operation_results);
    } else {
        std::cout
            << "[SKIP] operation regression groups require "
               "tests/run_operation_regressions.sh\n";
    }
    int failures = 0;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "[PASS] " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[FAIL] " << name << ": "
                      << error.what() << '\n';
        }
    }
    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
    }
    return failures == 0 ? 0 : 1;
}
