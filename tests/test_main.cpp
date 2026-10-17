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
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
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
    TemporaryDirectory() {
        auto pattern =
            (std::filesystem::temp_directory_path() /
             "mcpp-test-XXXXXX").string();
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
        std::size_t{3},
        "pane invert selection"
    );
    pane.clear_selection();
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
        std::string("bc\nb \n b"),
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
    const std::vector<std::pair<
        std::string_view,
        void (*)()
    >> tests{
        {"local provider", test_local_provider},
        {"pane", test_pane},
        {"special paths", test_special_paths},
        {"operations", test_operations},
        {"viewer/editor", test_viewer_editor},
        {"config", test_config},
        {"editor clipboard", test_editor_clipboard},
        {"search/compare", test_search_compare},
        {"syntax", test_syntax},
        {"editor search", test_editor_search},
        {"user menu", test_user_menu},
    };
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
