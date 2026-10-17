#include "mcpp/config.hpp"
#include "mcpp/editor.hpp"
#include "mcpp/local_provider.hpp"
#include "mcpp/operations.hpp"
#include "mcpp/pane.hpp"
#include "mcpp/viewer.hpp"

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
    write_file(temporary.path() / "a.txt", "alpha");
    write_file(temporary.path() / ".hidden", "hidden");
    write_file(temporary.path() / "sub" / "b.bin", "bravo");
    std::filesystem::create_symlink(
        "a.txt",
        temporary.path() / "link"
    );

    mcpp::LocalProvider provider(temporary.path());
    auto entries = provider.list(
        temporary.path().string(),
        {},
        {}
    );
    check_equal(entries.size(), std::size_t{3}, "visible list size");
    check_equal(
        entries.front().kind,
        mcpp::EntryKind::directory,
        "directories sort first"
    );
    entries = provider.list(
        temporary.path().string(),
        {.show_hidden = true},
        {}
    );
    check_equal(entries.size(), std::size_t{4}, "hidden list size");
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
    auto provider =
        std::make_shared<mcpp::LocalProvider>(temporary.path());
    mcpp::Pane pane(provider, temporary.path().string());
    check_equal(pane.entries().size(), std::size_t{3}, "pane entries");
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
        std::size_t{1},
        "pane child listing"
    );
    check(pane.up(), "pane up");
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

    const auto moved = provider.stat(
        (moved_dir / "a.txt").string(),
        {}
    );
    operations.remove(provider, {moved}, {});
    check(
        !std::filesystem::exists(moved_dir / "a.txt"),
        "remove file"
    );
    static_cast<void>(last);
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
    check(editor.dirty(), "editor dirty");
    editor.save(false);
    check_equal(
        read_file(path),
        std::string("alpha\nbeta!\ngamma"),
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
    config.theme = "blue";
    config.left_path = "/left";
    config.right_path = "/right";
    config.save(path);
    const auto loaded = mcpp::Config::load(path);
    check(loaded.show_hidden, "config hidden");
    check(!loaded.confirm_delete, "config confirmation");
    check_equal(loaded.sort, mcpp::SortField::size, "config sort");
    check_equal(loaded.theme, std::string("blue"), "config theme");
}

}  // namespace

int main() {
    const std::vector<std::pair<
        std::string_view,
        void (*)()
    >> tests{
        {"local provider", test_local_provider},
        {"pane", test_pane},
        {"operations", test_operations},
        {"viewer/editor", test_viewer_editor},
        {"config", test_config},
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
