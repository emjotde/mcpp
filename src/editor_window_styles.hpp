#pragma once

#include "viewer_help_styles.hpp"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

#include <array>
#include <memory>
#include <utility>

namespace mcpp::detail {

class EditorWindowFrame final : public ftxui::Node {
public:
    EditorWindowFrame(
        ftxui::Element border, const SkinPair* frame, const SkinPair* corner
    )
        : Node({std::move(border)}),
          frame_(frame ? paint_skin_style(ftxui::filler(), *frame) : ftxui::Element{}),
          corner_(corner ? paint_skin_style(ftxui::filler(), *corner) : ftxui::Element{}) {}

    void SetBox(const ftxui::Box box) override {
        Node::SetBox(box);
        children_.front()->SetBox(box);
    }

    void Render(ftxui::Screen& screen) override {
        if (box_.x_min < box_.x_max && box_.y_min < box_.y_max) {
            const auto paint = [&](const ftxui::Element& style, const ftxui::Box area) {
                if (!style) return;
                const auto visible = ftxui::Box::Intersection(
                    ftxui::Box::Intersection(area, screen.stencil),
                    {0, screen.dimx() - 1, 0, screen.dimy() - 1});
                if (visible.IsEmpty()) return;
                style->SetBox(visible);
                style->Render(screen);
            };
            if (frame_) {
                const std::array strips{
                    ftxui::Box{box_.x_min, box_.x_max, box_.y_min, box_.y_min},
                    ftxui::Box{box_.x_min, box_.x_max, box_.y_max, box_.y_max},
                    ftxui::Box{box_.x_min, box_.x_min, box_.y_min, box_.y_max},
                    ftxui::Box{box_.x_max, box_.x_max, box_.y_min, box_.y_max},
                };
                for (const auto strip : strips) paint(frame_, strip);
            }
            paint(corner_, {box_.x_max, box_.x_max, box_.y_max, box_.y_max});
        }
        Node::Render(screen);
    }

private:
    ftxui::Element frame_;
    ftxui::Element corner_;
};

[[nodiscard]] inline ftxui::Element editor_window_frame(
    ftxui::Element border, const Skin& skin, const bool focused, const bool dragging
) {
    const auto* frame = find_skin_color(
        skin, "editor", dragging ? "editframedrag" : focused ? "editframeactive" : "editframe");
    const auto* corner = dragging ? nullptr : find_skin_color(skin, "editor", "editframedrag");
    if (!frame && !corner) return border;
    return std::make_shared<EditorWindowFrame>(std::move(border), frame, corner);
}

[[nodiscard]] inline ftxui::Element editor_window_controls(
    ftxui::Element controls, const Skin& skin, const bool focused, const bool dragging
) {
    const auto* colors = find_skin_color(
        skin, "editor", dragging ? "editframedrag" : focused ? "editframeactive" : "editframe");
    return paint_skin_style(std::move(controls), colors ? *colors : skin.menu);
}

[[nodiscard]] inline ftxui::Element editor_desktop(ftxui::Element blank, const Skin& skin) {
    const auto* colors = find_skin_color(skin, "editor", "editbg");
    return colors ? paint_skin_style(std::move(blank), *colors) : std::move(blank);
}

}  // namespace mcpp::detail
