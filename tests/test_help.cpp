#include "mcpp/help.hpp"

#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {

void check(const bool condition, const std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

void check_browser() {
    using mcpp::HelpBrowser;
    using mcpp::HelpTopic;
    HelpBrowser help(HelpTopic::manager);
    help.resize(38, 5);
    help.page(1);
    check(help.first_line() == 4U, "paging moves by a visible page");
    help.scroll(-1);
    const auto first = help.first_line();
    const auto text = help.lines()[first].text;
    help.resize(38, 5);
    check(help.first_line() == first && help.lines()[first].text == text,
          "rendering keeps the scroll position");
    help.open(HelpTopic::index);
    help.next_link();
    const auto selected = help.selected_link();
    const auto index_top = help.first_line();
    check(selected.has_value(), "link navigation selects a link");
    const auto target = help.links()[*selected].target;
    check(help.follow() && help.topic() == target && target != HelpTopic::index,
          "index links open their actual topics");
    check(help.back() && help.topic() == HelpTopic::index &&
          help.first_line() == index_top && help.selected_link() == selected,
          "back restores the topic, viewport and selected link");
    help.bottom();
    help.resize(24, 4);
    check(help.first_line() + help.page_rows() <= help.lines().size(),
          "resizing clamps the viewport");
    for (const auto& line : help.lines()) check(line.text.size() <= 24U, "help wraps at the available width");
    for (const auto& page : mcpp::help_pages()) {
        for (const auto& block : page.blocks) {
            if (block.target) check(!mcpp::help_page(*block.target).title.empty(), "help links have destinations");
        }
    }
}

}  // namespace

int main() {
    try {
        check_browser();
        std::cout << "help browser: ok\n";
    } catch (const std::exception& error) {
        std::cerr << "help browser: " << error.what() << '\n';
        return 1;
    }
}
