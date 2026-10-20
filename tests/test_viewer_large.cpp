#include "mcpp/editor_search.hpp"
#include "mcpp/local_provider.hpp"
#include "mcpp/local_tools.hpp"
#include "mcpp/viewer.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;

void check(const bool condition, const std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

template<class Action>
void rejected(Action action, const std::string_view message) {
    bool failed = false;
    try { action(); }
    catch (const std::exception&) { failed = true; }
    check(failed, message);
}

std::string read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}

void boundary(const fs::path& root) {
    constexpr std::uint64_t materialized_limit = 64U * 1024U * 1024U;
    const auto source = root / "sparse";
    const std::string tail = "\nSTART A\bA _\bU\nDONE ending\n";
    const auto tail_position = materialized_limit - 8U;
    {
        std::ofstream output(source, std::ios::binary);
        output << "head\n";
        output.seekp(static_cast<std::streamoff>(tail_position));
        output << tail;
        check(output.good(), "write sparse boundary fixture");
    }
    struct stat status{};
    check(::stat(source.c_str(), &status) == 0 && status.st_blocks < 128,
          "boundary fixture has only a few real data pages");
    auto local = std::make_shared<mcpp::LocalProvider>();
    mcpp::ViewerModel viewer;
    viewer.open(local, source.string());
    viewer.set_wrap(false);
    viewer.goto_data_offset(tail_position + 1U);
    check(viewer.find("START", true, {}, false) == tail_position + 1U,
          "ordinary forward literals still use bounded raw reads");
    viewer.goto_data_offset(tail_position + 1U);
    const mcpp::EditorSearchOptions regex{
        .mode = mcpp::EditorSearchMode::regular_expression, .case_sensitive = true
    };
    check(viewer.find("START(.|\n)*DONE", regex, {}, false) == tail_position + 1U,
          "unbounded multiline regex addresses bytes beyond 64 MiB");
    viewer.toggle_nroff();
    check(viewer.size() > materialized_limit && viewer.data_size() == tail_position + tail.size(),
          "formatted data above 64 MiB retains the full source length");
    viewer.goto_data_offset(tail_position + 1U);
    check(viewer.lines(40, 1) == std::vector<std::string>{"START A U"},
          "large formatted source renders nroff output");
    const auto& styles = viewer.text_styles(0);
    check(styles.size() == 2U && styles[0].bold && styles[1].underline,
          "nroff attributes survive the large-file path");
    check(viewer.source_view_end() == tail_position + tail.find("DONE"),
          "large formatted view end maps to source bytes");
    auto retained = viewer;
    viewer.close();
    retained.goto_data_offset(tail_position + tail.find("DONE"));
    check(retained.lines(40, 1) == std::vector<std::string>{"DONE ending"},
          "an independent viewer retains its formatted spool");
}

class CapturedProvider final : public mcpp::Provider {
public:
    explicit CapturedProvider(std::string bytes) : bytes_(std::move(bytes)) {}
    [[nodiscard]] std::string id() const override { return "captured-archive-member"; }
    [[nodiscard]] std::string display_name() const override { return id(); }
    [[nodiscard]] mcpp::ProviderCapability capabilities() const override { return mcpp::ProviderCapability::read; }
    [[nodiscard]] std::string normalize(const std::string_view path) const override {
        check(path == "/file", "captured provider path must not become a host path");
        return std::string(path);
    }
    [[nodiscard]] std::string join(std::string_view, std::string_view) const override { return "/file"; }
    [[nodiscard]] std::string parent(std::string_view) const override { return "/"; }
    [[nodiscard]] std::string basename(std::string_view) const override { return "file"; }
    [[nodiscard]] std::vector<mcpp::Entry> list(
        std::string_view, const mcpp::ListRequest&, const mcpp::Cancellation&
    ) const override { return {}; }
    [[nodiscard]] mcpp::Entry stat(const std::string_view path, const mcpp::Cancellation& cancellation) const override {
        cancellation.throw_if_requested();
        mcpp::Entry entry;
        entry.path = normalize(path);
        entry.name = "file";
        entry.kind = mcpp::EntryKind::file;
        entry.size = bytes_.size();
        entry.revision = "captured-revision";
        return entry;
    }
    [[nodiscard]] mcpp::Entry stat_follow(
        const std::string_view path, const mcpp::Cancellation& cancellation
    ) const override { return stat(path, cancellation); }
    [[nodiscard]] bool exists(const std::string_view path) const override { return path == "/file"; }
    [[nodiscard]] mcpp::SpaceInfo space(std::string_view) const override { return {}; }
    void read(const std::string_view path, const std::uint64_t offset, const std::uint64_t length,
              std::ostream& output, const mcpp::Cancellation& cancellation) const override {
        static_cast<void>(normalize(path));
        cancellation.throw_if_requested();
        if (offset >= bytes_.size()) return;
        const auto count = std::min<std::uint64_t>(length, bytes_.size() - offset);
        output.write(bytes_.data() + offset, static_cast<std::streamsize>(count));
    }
    void write(std::string_view, std::istream&, std::optional<std::uint64_t>, bool,
               std::optional<std::string>, const mcpp::Cancellation&, const mcpp::ProgressCallback&) override { readonly(); }
    void create_directory(std::string_view) override { readonly(); }
    void remove(std::string_view, bool, const mcpp::Cancellation&, const mcpp::ProgressCallback&) override { readonly(); }
    void rename(std::string_view, std::string_view, bool) override { readonly(); }
    bool copy_direct(std::string_view, std::string_view, bool,
                     const mcpp::Cancellation&, const mcpp::ProgressCallback&) override { readonly(); }
    void set_permissions(std::string_view, fs::perms) override { readonly(); }
    void set_owner(std::string_view, std::optional<std::string>, std::optional<std::string>) override { readonly(); }
    void create_symlink(std::string_view, std::string_view, bool, const mcpp::Cancellation&) override { readonly(); }
    void create_hardlink(std::string_view, std::string_view) override { readonly(); }
private:
    [[noreturn]] static void readonly() { throw std::runtime_error("captured member is read-only"); }
    std::string bytes_;
};

void parsed(const fs::path& root) {
    const std::string text = "captured member\nA\bA _\bZ\n";
    const auto plain = root / "plain";
    std::ofstream(plain, std::ios::binary) << text;
    const auto compressed = mcpp::capture_command({"gzip", "-c", "--", plain.string()}, root, {}, 4096);
    check(compressed.status == 0, "compress the tiny captured member");
    const auto local_source = root / "encoded.gz";
    std::ofstream(local_source, std::ios::binary) << compressed.output;
    mcpp::ViewerModel writable_source;
    writable_source.open(std::make_shared<mcpp::LocalProvider>(), local_source.string());
    rejected([&] { writable_source.toggle_hex_editing(); }, "parsed local files cannot be hex-edited");
    check(!writable_source.dirty() && read(local_source) == compressed.output,
          "parsed Save guards retain writable encoded source bytes");
    auto provider = std::make_shared<CapturedProvider>(compressed.output);
    std::weak_ptr<mcpp::Provider> lifetime = provider;
    mcpp::ViewerModel viewer;
    viewer.open(provider, "/file");
    check(viewer.parsed() && viewer.size() == text.size() && viewer.path() == "/file" &&
          viewer.provider() == provider, "parsed viewing stays in the captured provider");
    viewer.toggle_nroff();
    check(viewer.lines(40, 2) == std::vector<std::string>{"captured member", "A Z"},
          "captured compressed member formats through the seekable decoder");
    rejected([&] { viewer.toggle_hex_editing(); }, "parsed data cannot replace compressed bytes on Save");
    auto retained = viewer;
    viewer.close();
    provider.reset();
    check(!lifetime.expired(), "independent viewer retains its provider");
    retained.toggle_magic();
    check(!retained.parsed() && retained.data_size() == compressed.output.size(),
          "Raw selects captured encoded bytes, not a host /file");
    retained.toggle_magic();
    check(retained.parsed() && retained.lines(40, 1).front() == "captured member",
          "parsed spool can be rebuilt after the first viewer closes");
}

void searches(const fs::path& root) {
    std::string content = "abababa\none ONE_one one\nhead\nmiddle\ntail\n\xc5\x81 \xa3\n";
    constexpr std::size_t crossing = 64U * 1024U - 1U;
    content.resize(crossing, ' ');
    content += "Łword\n";
    const auto source = root / "search";
    std::ofstream(source, std::ios::binary) << content;
    auto local = std::make_shared<mcpp::LocalProvider>();
    mcpp::ViewerModel viewer;
    viewer.open(local, source.string());
    viewer.set_wrap(false);
    const mcpp::EditorSearchOptions backwards{
        .mode = mcpp::EditorSearchMode::regular_expression, .case_sensitive = true, .backwards = true
    };
    const auto scoped = mcpp::EditorSearch::find("baX", "a*", 3, 0, 2, backwards, false, false);
    const auto scoped_wrap = mcpp::EditorSearch::find("baX", "a*", 0, 0, 2, backwards, true, false);
    check(scoped && scoped->begin == 2U && scoped->end == 2U &&
          scoped_wrap && scoped_wrap->begin == 2U && scoped_wrap->end == 2U,
          "bounded backward regex retains scoped zero-length boundary matches");
    viewer.goto_offset(7);
    const auto expected = mcpp::EditorSearch::find(content, "aba", 7, 0, content.size(), backwards, false, false);
    check(expected && viewer.find("aba", backwards, {}, false) == expected->begin,
          "backward regex retains non-overlapping match order");
    viewer.goto_offset(7);
    check(viewer.find("aba", {.case_sensitive = true, .backwards = true}, {}, false) == expected->begin,
          "backward literals retain the same non-overlapping order");
    viewer.goto_offset(content.find("head"));
    check(viewer.find("^head$\nmiddle\n^tail$",
                      {.mode = mcpp::EditorSearchMode::regular_expression, .case_sensitive = true},
                      {}, false) == content.find("head"), "mapped regex keeps complete newline-anchor context");
    const mcpp::EditorSearchOptions words{
        .mode = mcpp::EditorSearchMode::normal, .case_sensitive = false, .whole_words = true
    };
    viewer.goto_offset(8);
    check(viewer.find("one", words, {}, false) == 8U && viewer.find("one", words, {}, false) == 20U &&
          viewer.find("one", words, {}, true) == 8U, "whole-word search preserves direction and wrap boundaries");
    const mcpp::EditorSearchOptions wildcard{
        .mode = mcpp::EditorSearchMode::wildcard, .case_sensitive = true
    };
    viewer.goto_offset(23);
    const auto star = mcpp::EditorSearch::find(content, "h*d", 23, 0, content.size(), wildcard, false, false);
    check(star && viewer.find("h*d", wildcard, {}, false) == star->begin, "wildcards keep the existing POSIX semantics");
    const auto encoded = content.find('\xa3');
    viewer.goto_offset(encoded);
    check(viewer.find("Ł", {.mode = mcpp::EditorSearchMode::normal, .case_sensitive = true}, {}, false, true) == encoded,
          "all-charsets search returns the encoded source byte");
    viewer.go_to_start();
    check(viewer.find("łword", words, {}, false) == crossing,
          "case-folded whole-word search crosses a UTF-8 range boundary");
    auto reverse_words = words;
    reverse_words.backwards = true;
    viewer.goto_offset(content.size());
    check(viewer.find("łword", reverse_words, {}, false) == crossing,
          "backward whole-word search retains the boundary-spanning hit");
}

void mapping(const fs::path& root) {
    const std::string encoded = "\xa3\b\xa3 _\bZ\nlast\rLAST\n";
    const auto source = root / "encoded";
    std::ofstream(source, std::ios::binary) << encoded;
    auto local = std::make_shared<mcpp::LocalProvider>();
    mcpp::ViewerModel viewer;
    viewer.open(local, source.string());
    viewer.set_encoding("CP1250");
    viewer.toggle_nroff();
    check(viewer.lines(40, 1) == std::vector<std::string>{"Ł Z"}, "codepage and nroff text compose");
    check(viewer.source_view_end() == encoded.find("LAST"),
          "codepage and nroff byte maps retain the next visible raw cell");
    viewer.goto_line(2);
    check(viewer.lines(40, 1) == std::vector<std::string>{"LAST"} && viewer.source_view_end() == encoded.size(),
          "carriage-return overstrikes keep line navigation and raw byte ends");
    viewer.goto_data_offset(3);
    check(viewer.offset() == 2U, "raw offsets map back to decoded formatted text");
    viewer.toggle_hex();
    viewer.toggle_hex_editing();
    viewer.set_hex_cursor(0, true);
    viewer.edit_ascii_byte('B');
    viewer.toggle_hex();
    viewer.save_changes();
    auto saved_encoded = encoded;
    saved_encoded[0] = 'B';
    check(read(source) == saved_encoded && viewer.size() == 10U && !viewer.dirty(),
          "saving from formatted text writes staged raw bytes, not the transformed spool");
    std::ofstream(source, std::ios::app) << "grown\n";
    static_cast<void>(viewer.refresh());
    check(!viewer.source_error().empty() && viewer.size() == 10U,
          "formatted snapshots stay frozen when their source grows");
    viewer.toggle_nroff();
    viewer.set_encoding("UTF-8");
    check(viewer.source_error().empty() && viewer.data_size() == encoded.size() + 6U,
          "explicit unformat reloads the current source");
    viewer.toggle_hex();
    viewer.toggle_hex_editing();
    viewer.set_hex_cursor(0, true);
    viewer.edit_ascii_byte('X');
    std::ofstream(source, std::ios::app) << "new\n";
    static_cast<void>(viewer.refresh());
    rejected([&] { viewer.save_changes(); }, "source conflicts still reject raw hex saves");
    check(read(source).starts_with(saved_encoded) && viewer.dirty(), "failed save preserves encoded bytes and staged edits");
    viewer.discard_changes();
}

}  // namespace

int main(const int argc, const char* const* argv) {
    try {
        check(argc == 3, "usage: mcpp_viewer_large_tests boundary|parsed|search|mapping CASE_DIRECTORY");
        check(::geteuid() == 1001 && fs::exists("/.dockerenv") &&
              fs::exists("/review/.operation-regressions-disposable"),
              "viewer checks require the owned UID1001 disposable container");
        const auto root = fs::absolute(argv[2]).lexically_normal();
        check(root.string().starts_with("/review/cases/viewer-large-") &&
              fs::weakly_canonical(root) == root, "use a dedicated viewer-large case directory without symlinks");
        fs::create_directories(root);
        struct stat directory{};
        check(::lstat(root.c_str(), &directory) == 0 && S_ISDIR(directory.st_mode) &&
              directory.st_uid == ::geteuid(), "case directory must be owned by the test user");
        check(::setenv("XDG_CACHE_HOME", (root / "cache").c_str(), 1) == 0, "set private viewer cache");
        const std::string_view selected(argv[1]);
        if (selected == "boundary") boundary(root);
        else if (selected == "parsed") parsed(root);
        else if (selected == "search") searches(root);
        else if (selected == "mapping") mapping(root);
        else throw std::invalid_argument("unknown large-viewer case");
        std::cout << "PASS viewer large " << selected << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "viewer large: " << error.what() << '\n';
        return 1;
    }
}
