#include "mcpp/config.hpp"
#include "mcpp/compare.hpp"
#include "mcpp/editor.hpp"
#include "mcpp/local_provider.hpp"
#include "mcpp/operations.hpp"
#include "mcpp/pane.hpp"
#include "mcpp/search.hpp"
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
    pane.move_cursor(-2);
    check(pane.enter(), "pane enter");
    check_equal(
        pane.entries().size(),
        std::size_t{2},
        "pane child listing"
    );
    check(pane.up(), "pane up");
    check(pane.history_back(), "pane history back");
    check_equal(
        pane.entries().size(),
        std::size_t{2},
        "pane history target"
    );
    check(pane.history_forward(), "pane history forward");
    check(pane.find("sub-link"), "find symlink directory");
    check(pane.enter(), "enter symlink directory");
    check_equal(
        pane.entries().size(),
        std::size_t{2},
        "symlink directory listing"
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
    operations.remove(provider, {moved}, {});
    check(
        !std::filesystem::exists(moved_dir / "a.txt"),
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
    const auto match = viewer.find("gamma", true);
    check(match.has_value(), "viewer find");
    viewer.toggle_hex();
    check(viewer.hex(), "viewer hex");

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

    editor.insert('?');
    write_file(path, "external");
    bool conflict = false;
    try {
        editor.save(false);
    } catch (const std::runtime_error&) {
        conflict = true;
    }
    check(conflict, "editor conflict");
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
    config.left_path = "/left";
    config.right_path = "/right";
    config.bookmarks = {"/one", "/two"};
    config.save(path);
    const auto loaded = mcpp::Config::load(path);
    check(loaded.show_hidden, "config hidden");
    check(!loaded.confirm_delete, "config confirmation");
    check_equal(loaded.sort, mcpp::SortField::size, "config sort");
    check(loaded.sort_descending, "config sort direction");
    check_equal(loaded.theme, std::string("blue"), "config theme");
    check_equal(
        loaded.bookmarks.size(),
        std::size_t{2},
        "config bookmarks"
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
        {"search/compare", test_search_compare},
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
