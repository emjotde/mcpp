#include "mcpp/config.hpp"
#include "mcpp/local_provider.hpp"
#include "mcpp/pane.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
using mcpp::Pane;

void check(const bool condition, const std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

class Fixtures {
public:
    Fixtures() : root_("panel-torben-preferences-" + std::to_string(getpid())) {
        check(fs::create_directory(root_), "create owned relative preference fixtures");
        try {
            fs::create_directory(root_ / "listing");
            for (int index = 1; index <= 24; ++index) {
                write("listing/f" + std::string(index < 10 ? "0" : "") + std::to_string(index), "x\n");
            }
            write("absent.ini", "[Panels]\npanel_scroll_pages=true\n");
            write("disabled.ini", "[Panels]\ntorben_fj_mode=false\n");
            write("enabled.ini", "[Panels]\ntorben_fj_mode=true\npanel_scroll_pages=true\n");
        } catch (...) {
            std::error_code error;
            fs::remove_all(root_, error);
            throw;
        }
    }

    ~Fixtures() {
        std::error_code error;
        fs::remove_all(root_, error);
    }

    Fixtures(const Fixtures&) = delete;
    Fixtures& operator=(const Fixtures&) = delete;

    [[nodiscard]] fs::path path(const std::string_view name) const {
        return fs::absolute(root_ / name);
    }

private:
    void write(const fs::path& name, const std::string_view contents) const {
        std::ofstream output(root_ / name);
        output << contents;
        output.flush();
        check(static_cast<bool>(output), "write original preference fixture");
    }

    fs::path root_;
};

void state(
    const Pane& pane, const std::size_t cursor, const std::size_t first,
    const std::size_t requested_rows
) {
    const auto rows = std::max<std::size_t>(1U, requested_rows);
    const auto count = pane.entries().size();
    check(pane.cursor() == cursor && pane.first_row() == first, "expected cursor and viewport");
    if (count == 0U) {
        check(cursor == 0U && first == 0U && pane.current() == nullptr, "empty pane bounds");
    } else {
        check(cursor < count && first <= (count > rows ? count - rows : 0U),
              "cursor and viewport bounds");
        check(cursor >= first && cursor - first < rows &&
              pane.current() == &pane.entries()[cursor], "cursor remains visible");
    }
}

void same(const Pane& first, const Pane& second) {
    check(first.cursor() == second.cursor() && first.first_row() == second.first_row(),
          "existing movement must be independent of torben mode");
    check(first.marked_count() == second.marked_count() &&
          first.marked_size() == second.marked_size(), "existing movement preserves marks");
    const auto a = first.selected_entries();
    const auto b = second.selected_entries();
    check(a.size() == b.size(), "existing movement preserves selected entry count");
    for (std::size_t index = 0; index < a.size(); ++index) {
        check(a[index].path == b[index].path, "existing movement preserves selected paths");
    }
}

void native_navigation(const Pane& original) {
    struct Navigation {
        bool pages;
        bool center;
        std::size_t first;
        std::size_t home;
        std::size_t second_home;
    };
    // Expected positions were measured with an original 25-entry MC 4.8.30 listing.
    constexpr std::array navigation{
        Navigation{false, false, 3U, 8U, 3U},
        Navigation{true, false, 5U, 10U, 5U},
        Navigation{false, true, 7U, 7U, 0U},
    };
    const std::vector<std::string> selected{original.entries()[2].path, original.entries()[7].path};
    for (const auto settings : navigation) {
        Pane pane = original;
        pane.set_viewport_rows(10U);
        pane.set_navigation(settings.pages, settings.center);
        pane.set_torben_fj_mode(true);
        pane.select_paths(selected);
        const auto marked_state = [&] {
            check(pane.marked_count() == 2U && pane.marked_size() == 4U, "Home/End preserve marks");
            for (const auto& path : selected) {
                const auto entry = std::ranges::find(pane.entries(), path, &mcpp::Entry::path);
                check(entry != pane.entries().end() && pane.is_selected(*entry), "Home/End preserve selection");
            }
        };
        const auto expect = [&](const std::size_t cursor, const std::size_t first, const std::size_t rows = 10U) {
            state(pane, cursor, first, rows);
            marked_state();
        };
        for (int count = 0; count < 12; ++count) pane.move_cursor(1);
        expect(12U, settings.first);
        pane.home_cursor();
        expect(settings.home, settings.first);
        pane.home_cursor();
        expect(settings.second_home, settings.second_home);
        pane.home_cursor();
        expect(0U, 0U);
        pane.end_cursor();
        expect(5U, 0U);
        pane.end_cursor();
        expect(9U, 0U);
        pane.end_cursor();
        expect(24U, 15U);
        for (int count = 0; count < 3; ++count) pane.move_cursor(-1);
        expect(21U, 15U);
        pane.home_cursor();
        expect(20U, 15U);
        pane.home_cursor();
        expect(15U, 15U);
        pane.home_cursor();
        expect(0U, 0U);
        pane.page_cursor(5);
        expect(5U, 5U);
        pane.end_cursor();
        expect(10U, 5U);
        pane.end_cursor();
        expect(14U, 5U);
        pane.end_cursor();
        expect(24U, 15U);
        pane.set_viewport_rows(11U);
        expect(24U, 14U, 11U);
        pane.home_cursor();
        expect(19U, 14U, 11U);
        pane.home_cursor();
        expect(14U, 14U, 11U);
        pane.home_cursor();
        expect(0U, 0U, 11U);
        pane.end_cursor();
        expect(5U, 0U, 11U);
        pane.end_cursor();
        expect(10U, 0U, 11U);
        pane.end_cursor();
        expect(24U, 14U, 11U);
    }
}

void existing_movement(const Pane& original) {
    Pane legacy = original;
    legacy.set_viewport_rows(10U);
    legacy.set_navigation(true, true);
    legacy.select_paths({legacy.entries()[2].path, legacy.entries()[7].path});
    Pane enabled = legacy;
    enabled.set_torben_fj_mode(true);
    for (const auto amount : {1, 1, 10, -3}) {
        legacy.move_cursor(amount);
        enabled.move_cursor(amount);
        same(legacy, enabled);
    }
    for (const auto amount : {10, 10, -10, -10}) {
        legacy.page_cursor(amount);
        enabled.page_cursor(amount);
        same(legacy, enabled);
    }
    for (const auto amount : {std::numeric_limits<int>::max(), std::numeric_limits<int>::min()}) {
        legacy.move_cursor(amount);
        enabled.move_cursor(amount);
        same(legacy, enabled);
    }
    legacy.move_cursor(12);
    Pane home = legacy;
    legacy.move_cursor(-static_cast<int>(legacy.cursor()));
    home.home_cursor();
    same(legacy, home);
    legacy.move_cursor(static_cast<int>(legacy.entries().size()));
    home.end_cursor();
    same(legacy, home);
}

Pane synthetic(const Pane& original, const std::size_t count) {
    Pane pane = original;
    std::vector<mcpp::Entry> entries;
    for (std::size_t index = 0; index < count; ++index) {
        mcpp::Entry entry;
        entry.name = "item" + std::to_string(index);
        entry.path = "/original-model/" + entry.name;
        entry.kind = mcpp::EntryKind::file;
        entries.push_back(std::move(entry));
    }
    pane.set_panelized(std::move(entries));
    pane.set_torben_fj_mode(true);
    return pane;
}

void viewport_bounds(const Pane& original) {
    for (const auto count : {0U, 1U}) {
        auto pane = synthetic(original, count);
        pane.set_viewport_rows(0U);
        pane.end_cursor();
        pane.home_cursor();
        state(pane, 0U, 0U, 1U);
    }
    auto short_list = synthetic(original, 4U);
    short_list.set_viewport_rows(10U);
    short_list.end_cursor();
    state(short_list, 3U, 0U, 10U);
    short_list.home_cursor();
    state(short_list, 0U, 0U, 10U);
    auto narrow = synthetic(original, 25U);
    narrow.set_viewport_rows(3U);
    narrow.set_navigation(false, true);
    narrow.end_cursor();
    state(narrow, 1U, 0U, 3U);
    narrow.end_cursor();
    state(narrow, 2U, 0U, 3U);
    narrow.end_cursor();
    state(narrow, 24U, 22U, 3U);
    narrow.home_cursor();
    state(narrow, 23U, 22U, 3U);
    narrow.home_cursor();
    state(narrow, 22U, 22U, 3U);
    narrow.home_cursor();
    state(narrow, 0U, 0U, 3U);
    const auto maximum = std::numeric_limits<std::size_t>::max();
    narrow.set_viewport_rows(maximum);
    narrow.end_cursor();
    state(narrow, 24U, 0U, maximum);
    narrow.home_cursor();
    state(narrow, 0U, 0U, maximum);
}

void preferences(
    const Pane& original, const mcpp::ProviderPtr& provider, const Fixtures& fixtures
) {
    static_assert(noexcept(std::declval<Pane&>().set_torben_fj_mode(true)));
    const auto absent = mcpp::Config::load(fixtures.path("absent.ini"));
    const auto disabled = mcpp::Config::load(fixtures.path("disabled.ini"));
    const auto enabled = mcpp::Config::load(fixtures.path("enabled.ini"));
    check(!absent.torben_fj_mode.has_value() && disabled.torben_fj_mode == false &&
          enabled.torben_fj_mode == true, "native preference must retain absence and both boolean values");
    Pane configured = original;
    configured.set_viewport_rows(10U);
    configured.set_navigation(enabled.scroll_pages, enabled.scroll_center);
    configured.set_torben_fj_mode(enabled.torben_fj_mode.value_or(false));
    Pane inherited(provider, fixtures.path("listing").string(), &configured);
    inherited.end_cursor();
    state(inherited, 5U, 0U, 10U);
    inherited.set_torben_fj_mode(disabled.torben_fj_mode.value_or(false));
    inherited.end_cursor();
    state(inherited, 24U, 15U, 10U);
    configured.swap_contents(inherited);
    configured.home_cursor();
    configured.home_cursor();
    configured.home_cursor();
    configured.end_cursor();
    state(configured, 5U, 0U, 10U);
    inherited.home_cursor();
    inherited.end_cursor();
    state(inherited, 24U, 15U, 10U);
}

}  // namespace

int main() try {
    const Fixtures fixtures;
    const auto provider = std::make_shared<mcpp::LocalProvider>();
    const Pane original(provider, fixtures.path("listing").string());
    check(original.entries().size() == 25U && original.entries().front().name == "..",
          "original short listing must include a parent and 24 files");
    native_navigation(original);
    existing_movement(original);
    viewport_bounds(original);
    preferences(original, provider, fixtures);
    std::cout << "panel torben preferences passed\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
