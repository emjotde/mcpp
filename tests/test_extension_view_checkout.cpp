#include "../src/extension_view_checkout.hpp"
#include "../src/local_tools_stream.hpp"
#include "mcpp/archive_provider.hpp"
#include "mcpp/native_menu.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
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
void rejects(Function function, const std::string_view diagnostic) {
    try {
        function();
    } catch (const std::exception& error) {
        if (std::string_view(error.what()).find(diagnostic) != std::string_view::npos) return;
        throw std::runtime_error(std::string("unexpected rejection: ") + error.what());
    }
    throw std::runtime_error("expected rejection: " + std::string(diagnostic));
}

void put(const fs::path& path, const std::string_view bytes) {
    std::ofstream output;
    output.exceptions(std::ios::badbit | std::ios::failbit);
    output.open(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
}

std::string get(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    check(input.good(), "open original test bytes");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

class Source final : public mcpp::Provider {
public:
    enum class Fault { none, revision, size, cancel, short_read, read_error, output_error };

    Source(std::string path, std::string bytes) : bound_path_(std::move(path)), bytes_(std::move(bytes)) {
        entry.path = bound_path_;
        entry.name = basename(bound_path_);
        entry.kind = mcpp::EntryKind::file;
        entry.mode = S_IFREG | 0644U;
        entry.revision = "captured-original-revision";
        entry.size = bytes_.size();
    }

    mutable mcpp::Entry entry;
    Fault fault{Fault::none};
    std::string invalid_basename;
    mutable std::size_t reads{0};
    mutable std::size_t id_reads{0};
    mutable std::uint64_t largest_read{0};
    std::size_t mutations{0};

    std::string id() const override {
        ++id_reads;
        return "opaque:NOT_A_HOST_PATH:///do-not-parse";
    }
    std::string display_name() const override { return "original checkout source"; }
    mcpp::ProviderCapability capabilities() const override { return mcpp::ProviderCapability::read; }
    std::string normalize(const std::string_view path) const override {
        check(path == bound_path_, "all reads and metadata stay bound to the captured path");
        return std::string(path);
    }
    std::string join(std::string_view, const std::string_view name) const override { return normalize(name); }
    std::string parent(std::string_view) const override { return "/"; }
    std::string basename(const std::string_view path) const override {
        static_cast<void>(normalize(path));
        if (!invalid_basename.empty()) return invalid_basename;
        return std::string(path.substr(path.find_last_of('/') + 1U));
    }
    std::vector<mcpp::Entry> list(
        std::string_view, const mcpp::ListRequest&, const mcpp::Cancellation&
    ) const override { throw std::runtime_error("not a directory"); }
    mcpp::Entry stat(const std::string_view path, const mcpp::Cancellation& cancellation) const override {
        static_cast<void>(normalize(path));
        cancellation.throw_if_requested();
        return entry;
    }
    mcpp::Entry stat_follow(const std::string_view path, const mcpp::Cancellation& cancellation) const override {
        return stat(path, cancellation);
    }
    bool exists(const std::string_view path) const override { return path == bound_path_; }
    mcpp::SpaceInfo space(std::string_view) const override { return {}; }
    void read(
        const std::string_view path, const std::uint64_t offset, const std::uint64_t length,
        std::ostream& output, const mcpp::Cancellation& cancellation
    ) const override {
        static_cast<void>(normalize(path));
        cancellation.throw_if_requested();
        ++reads;
        largest_read = std::max(largest_read, length);
        check(offset <= bytes_.size() && length <= bytes_.size() - offset, "bounded source read");
        const auto count = fault == Fault::short_read && length != 0U ? length - 1U : length;
        output.write(bytes_.data() + static_cast<std::size_t>(offset), static_cast<std::streamsize>(count));
        if (fault == Fault::revision) entry.revision += ":changed";
        if (fault == Fault::size) ++entry.size;
        if (fault == Fault::cancel) cancellation.request();
        if (fault == Fault::read_error) throw std::runtime_error("original source read failure");
        if (fault == Fault::output_error) output.setstate(std::ios::badbit);
    }
    void write(
        std::string_view, std::istream&, std::optional<std::uint64_t>, bool,
        std::optional<std::string>, const mcpp::Cancellation&, const mcpp::ProgressCallback&
    ) override { mutation(); }
    void create_directory(std::string_view) override { mutation(); }
    void remove(
        std::string_view, bool, const mcpp::Cancellation&, const mcpp::ProgressCallback&
    ) override { mutation(); }
    void rename(std::string_view, std::string_view, bool) override { mutation(); }
    bool copy_direct(
        std::string_view, std::string_view, bool, const mcpp::Cancellation&, const mcpp::ProgressCallback&
    ) override { mutation(); }
    void set_permissions(std::string_view, fs::perms) override { mutation(); }
    void set_owner(std::string_view, std::optional<std::string>, std::optional<std::string>) override { mutation(); }
    void create_symlink(std::string_view, std::string_view, bool, const mcpp::Cancellation&) override { mutation(); }
    void create_hardlink(std::string_view, std::string_view) override { mutation(); }

private:
    [[noreturn]] void mutation() {
        ++mutations;
        throw std::runtime_error("checkout must never mutate its source");
    }
    std::string bound_path_;
    std::string bytes_;
};

std::string tar_member(
    const std::string_view name, const std::string_view bytes,
    const char type = '0', const std::string_view target = {}
) {
    std::string header(512U, '\0');
    check(name.size() < 100U && target.size() < 100U, "small original tar fixture");
    header.replace(0U, name.size(), name);
    const auto octal = [&](const std::size_t offset, const std::size_t width, const std::uint64_t value) {
        std::ostringstream text;
        text << std::oct << std::setfill('0') << std::setw(static_cast<int>(width - 1U)) << value;
        check(text.str().size() == width - 1U, "tar fixture field fits");
        header.replace(offset, width - 1U, text.str());
    };
    octal(100U, 8U, 0644U);
    octal(108U, 8U, 0U);
    octal(116U, 8U, 0U);
    octal(124U, 12U, bytes.size());
    octal(136U, 12U, 0U);
    header.replace(148U, 8U, 8U, ' ');
    header[156U] = type;
    header.replace(157U, target.size(), target);
    header.replace(257U, 6U, std::string_view("ustar\0", 6U));
    header.replace(263U, 2U, "00");
    std::uint64_t checksum = 0;
    for (const unsigned char byte : header) checksum += byte;
    octal(148U, 7U, checksum);
    header[155U] = ' ';
    header += bytes;
    header.append((512U - bytes.size() % 512U) % 512U, '\0');
    return header;
}

void success(const fs::path& root) {
    const std::string name = "odd \xff ' $(touch BAD).case";
    const std::string original(65553U, '\x9a');
    auto source = std::make_shared<Source>("/virtual/" + name, original);
    const auto captured = source->entry;
    std::weak_ptr<Source> held = source;
    auto output = std::make_shared<mcpp::TemporaryFile>("");
    const auto output_path = output->path();
    fs::path directory;
    std::string expected;
    {
        mcpp::ExtensionViewCheckout checkout(source, captured, {});
        directory = checkout.directory();
        check(checkout.path().is_absolute() && checkout.path().parent_path() == directory &&
              checkout.path().filename().native() == name, "preserve the actual basename bytes in a private directory");
        struct stat file{}, folder{};
        check(::lstat(checkout.path().c_str(), &file) == 0 && ::lstat(directory.c_str(), &folder) == 0 &&
              S_ISREG(file.st_mode) && (file.st_mode & 0777U) == 0600U && file.st_uid == ::geteuid() &&
              S_ISDIR(folder.st_mode) && (folder.st_mode & 0777U) == 0700U && folder.st_uid == ::geteuid(),
              "checkout is an owned 0600 regular file in an owned 0700 directory");
        check(get(checkout.path()) == original && source->reads == 2U && source->largest_read == 65536U &&
              source->mutations == 0U && source->id_reads == 0U, "stream exact bounded provider reads without IDs or mutations");
        source.reset();
        check(!held.expired(), "checkout strongly retains its captured provider");
        checkout.validate_source({});

        mcpp::NativeMenuContext context;
        context.active.local = false;
        context.active.file = captured;
        context.active.directory = "original-archive:///virtual directory";
        context.other.local = false;
        const std::string command =
            "%view{hex,nroff} test -f %f && printf '%%s\\000' %f %p %d %{Checkout note}";
        const std::vector<std::string> answers{"safe ' $(touch BAD); *"};
        check(mcpp::native_menu_has_view_filter(command) &&
              !mcpp::native_menu_has_view_filter(" %view{ascii,unform} ") &&
              !mcpp::native_menu_has_view_filter("printf ignored"),
              "only a nonempty parsed view directive enables checkout");
        rejects([&] { static_cast<void>(mcpp::expand_native_menu(command, context, answers)); }, "copy archive");
        rejects([&] { static_cast<void>(mcpp::expand_native_view_filter(
            "%view printf %F", context, {}, checkout.path().native())); }, "other-pane");
        rejects([&] { static_cast<void>(mcpp::expand_native_view_filter(
            "printf %f", context, {}, checkout.path().native())); }, "nonempty %view");
        const auto expansion = mcpp::expand_native_view_filter(command, context, answers, checkout.path().native());
        const auto plain = mcpp::expand_native_view_filter(
            "%view{ascii,unform} printf %f", context, {}, checkout.path().native());
        check(expansion.action == mcpp::NativeMenuAction::view && expansion.view_hex == true &&
              expansion.view_nroff == true && plain.view_hex == false && plain.view_nroff == false &&
              expansion.untag_active.empty() && expansion.untag_other.empty(),
              "view flags and existing nonlocal guards stay separate from the local filename override");
        for (const auto& value : {checkout.path().native(), captured.name, context.active.directory, answers.front()}) {
            expected += value;
            expected += '\0';
        }
        std::ofstream stream;
        stream.exceptions(std::ios::badbit | std::ios::failbit);
        stream.open(output_path, std::ios::binary | std::ios::trunc);
        const auto result = mcpp::capture_shell_to(stream, expansion.command, directory);
        stream.close();
        check(result.status == 0 && result.output.empty() && result.error.empty() &&
              get(output_path) == expected, "actual shell argv keeps local percent-f and original percent-p/d and safe prompt bytes");
        check(context.active.file->path == captured.path && context.active.file->revision == captured.revision &&
              context.active.directory == "original-archive:///virtual directory" && !fs::exists(directory / "BAD"),
              "the override never rewrites original context or executes prompt and filename metacharacters");
    }
    check(held.expired() && !fs::exists(directory) && get(output_path) == expected,
          "checkout cleanup releases its provider while independent filtered output remains alive");
    output.reset();
    check(!fs::exists(output_path), "filtered output has its own existing temporary-file lifetime");

    const std::string member = "ARCHIVE SOURCE, NOT HOST SHADOW\n";
    const auto archive_bytes = tar_member("file", member) + tar_member("empty", {}) +
        tar_member("link", {}, '2', "file") + std::string(1024U, '\0');
    const auto archive_path = root / "original.tar";
    put(archive_path, archive_bytes);
    put(root / "file", "HOST SHADOW UNCHANGED\n");
    auto virtual_source = std::make_shared<Source>("/file", archive_bytes);
    const mcpp::ArchiveOpenOptions options{.draft_directory = root / "archive-storage"};
    for (const bool nested : {false, true}) {
        auto archive = nested
            ? std::make_shared<mcpp::ArchiveProvider>(virtual_source, "/file", options)
            : std::make_shared<mcpp::ArchiveProvider>(archive_path, options);
        fs::path copied_directory;
        {
            mcpp::ExtensionViewCheckout checkout(archive, archive->stat("/file", {}), {});
            copied_directory = checkout.directory();
            check(checkout.path().filename() == "file" && get(checkout.path()) == member,
                  "host-backed and virtual-source archives read only their captured member");
            mcpp::ExtensionViewCheckout empty(archive, archive->stat("/empty", {}), {});
            check(fs::file_size(empty.path()) == 0U, "empty regular members are valid checkouts");
            rejects([&] { mcpp::ExtensionViewCheckout link(archive, archive->stat("/link", {}), {}); }, "regular");
        }
        check(!fs::exists(copied_directory), "archive-member checkout releases its private directory");
    }
    check(get(archive_path) == archive_bytes && get(root / "file") == "HOST SHADOW UNCHANGED\n" &&
          virtual_source->mutations == 0U, "archive originals and host shadows remain untouched");
}

void failure(const fs::path& root, const fs::path& runtime) {
    const auto clean = [&] { check(fs::is_empty(runtime), "failed checkout removes every partial private file and directory"); };
    auto source = std::make_shared<Source>("/file", "ORIGINAL SOURCE\n");
    const auto captured = source->entry;
    source->entry.revision += ":changed";
    rejects([&] { mcpp::ExtensionViewCheckout checkout(source, captured, {}); }, "source changed");
    check(source->reads == 0U, "a stale captured revision fails before source reads");
    source->entry = captured;
    for (const auto fault : {Source::Fault::revision, Source::Fault::size, Source::Fault::short_read,
                             Source::Fault::read_error, Source::Fault::output_error}) {
        source->fault = fault;
        rejects([&] { mcpp::ExtensionViewCheckout checkout(source, captured, {}); }, "");
        source->entry = captured;
        clean();
    }
    source->fault = Source::Fault::none;
    mcpp::Cancellation cancelled;
    cancelled.request();
    rejects([&] { mcpp::ExtensionViewCheckout checkout(source, captured, cancelled); }, "cancelled");
    source->fault = Source::Fault::cancel;
    rejects([&] { mcpp::ExtensionViewCheckout checkout(source, captured, {}); }, "cancelled");
    source->fault = Source::Fault::none;
    clean();
    {
        mcpp::ExtensionViewCheckout checkout(source, captured, {});
        ++source->entry.size;
        rejects([&] { checkout.validate_source({}); }, "source changed");
    }
    source->entry = captured;
    clean();

    source->invalid_basename = "../outside";
    rejects([&] { mcpp::ExtensionViewCheckout checkout(source, captured, {}); }, "basename");
    source->invalid_basename.clear();
    auto nonregular = captured;
    nonregular.kind = mcpp::EntryKind::symlink;
    rejects([&] { mcpp::ExtensionViewCheckout checkout(source, nonregular, {}); }, "regular");
    auto unversioned = captured;
    unversioned.revision.clear();
    rejects([&] { mcpp::ExtensionViewCheckout checkout(source, unversioned, {}); }, "revision");
    clean();

    const auto name_max = ::pathconf(runtime.c_str(), _PC_NAME_MAX);
    check(name_max > 0 && name_max < 4096, "query the real checkout filesystem's basename limit");
    const std::string boundary(static_cast<std::size_t>(name_max), 'n');
    auto longest = std::make_shared<Source>("/" + boundary, "");
    {
        mcpp::ExtensionViewCheckout checkout(longest, longest->entry, {});
        check(checkout.path().filename().native() == boundary, "a basename at NAME_MAX is not truncated or renamed");
    }
    auto oversized = std::make_shared<Source>("/" + boundary + "x", "");
    rejects([&] { mcpp::ExtensionViewCheckout checkout(oversized, oversized->entry, {}); },
            "create extension view checkout file");
    clean();

    put(root / "blocked-runtime", "NOT A DIRECTORY\n");
    check(::setenv("TMPDIR", (root / "blocked-runtime").c_str(), 1) == 0, "set the original IO failure fixture");
    rejects([&] { mcpp::ExtensionViewCheckout checkout(source, captured, {}); }, "");
    check(::setenv("TMPDIR", runtime.c_str(), 1) == 0, "restore the private checkout directory");
    clean();
    check(source->mutations == 0U && source->id_reads == 0U && oversized->reads == 0U,
          "all revision, cancellation and IO failures stay read-only and never fall back to host paths");
}

}  // namespace

int main(const int argc, char** argv) {
    try {
        if (argc != 3 || ::geteuid() != 1001U || !fs::is_regular_file("/.dockerenv") ||
            !fs::is_regular_file("/review/.operation-regressions-disposable")) {
            throw std::runtime_error("checkout tests require the owned UID1001 disposable container and case directory");
        }
        const auto base = fs::absolute(argv[2]).lexically_normal();
        check(base == "/review/cases/l21-view-checkout/model", "invalid owned checkout case path");
        fs::create_directories(base);
        check(!fs::is_symlink(fs::symlink_status(base)), "checkout fixture directory is not a symlink");
        const auto root = base / ("run-" + std::to_string(::getpid()));
        check(fs::create_directory(root), "checkout fixture directory is new");
        const auto runtime = root / "runtime";
        fs::create_directory(runtime);
        check(::setenv("TMPDIR", runtime.c_str(), 1) == 0, "use only the owned case directory for private files");
        if (std::string_view(argv[1]) == "success") success(root);
        else if (std::string_view(argv[1]) == "failure") failure(root, runtime);
        else throw std::runtime_error("expected success or failure case");
        fs::remove_all(root);
        std::cout << "extension view checkout " << argv[1] << ": passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
