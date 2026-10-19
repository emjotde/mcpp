#pragma once

#include <ftxui/screen/color.hpp>

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace mcpp {

struct SkinPair {
    ftxui::Color foreground;
    ftxui::Color background;
};

struct Skin {
    std::string id;
    std::string label;
    bool light{false};
    SkinPair normal{ftxui::Color::Grey85, ftxui::Color::Grey3};
    SkinPair menu{ftxui::Color::Grey11, ftxui::Color::Grey78};
    SkinPair menu_selected{ftxui::Color::Grey100, ftxui::Color::Grey23};
    SkinPair popup_selected{ftxui::Color::Grey78, ftxui::Color::Grey11};
    SkinPair dialog{ftxui::Color::Grey11, ftxui::Color::Grey82};
    SkinPair dialog_focus{ftxui::Color::Grey82, ftxui::Color::Grey11};
    SkinPair error{ftxui::Color::Grey100, ftxui::Color::Grey19};
    SkinPair error_focus{ftxui::Color::Grey19, ftxui::Color::Grey100};
    SkinPair selected{ftxui::Color::Grey0, ftxui::Color::Grey85};
    SkinPair marked_selected{ftxui::Color::Grey0, ftxui::Color::Grey85};
    SkinPair inactive_selected{ftxui::Color::Grey93, ftxui::Color::Grey27};
    SkinPair inactive_marked_selected{ftxui::Color::Grey93, ftxui::Color::Grey27};
    SkinPair search{ftxui::Color::Grey0, ftxui::Color::Grey62};
    SkinPair viewer_cursor{ftxui::Color::Grey11, ftxui::Color::Grey62};
    SkinPair hint{ftxui::Color::Grey11, ftxui::Color::Grey62};
    SkinPair key_number{ftxui::Color::Grey0, ftxui::Color::Grey85};
    SkinPair key_label{ftxui::Color::Grey85, ftxui::Color::Grey11};
    SkinPair progress{ftxui::Color::Grey11, ftxui::Color::Grey70};
    SkinPair diff_changed{ftxui::Color::Grey93, ftxui::Color::Grey19};
    ftxui::Color menu_hot{ftxui::Color::Grey11};
    ftxui::Color bright{ftxui::Color::Grey100};
    ftxui::Color dim{ftxui::Color::Grey50};
    ftxui::Color inactive_title{ftxui::Color::Grey58};
    ftxui::Color column_header{ftxui::Color::Grey93};
    ftxui::Color directory{ftxui::Color::Grey100};
    ftxui::Color symlink{ftxui::Color::Grey66};
    ftxui::Color executable{ftxui::Color::Grey100};
    ftxui::Color special{ftxui::Color::Grey46};
    ftxui::Color inactive_tree{ftxui::Color::Grey70};
    ftxui::Color disabled{ftxui::Color::Grey66};
    ftxui::Color shadow{ftxui::Color::Grey11};
    std::array<ftxui::Color, 7> syntax{
        ftxui::Color::Grey85,
        ftxui::Color::RGB(86, 156, 214), ftxui::Color::RGB(78, 201, 176),
        ftxui::Color::RGB(206, 145, 120), ftxui::Color::RGB(106, 153, 85),
        ftxui::Color::RGB(181, 206, 168), ftxui::Color::RGB(197, 134, 192)
    };
};

[[nodiscard]] const std::vector<Skin>& builtin_skins();
[[nodiscard]] const Skin& find_skin(std::string_view id);

}  // namespace mcpp
