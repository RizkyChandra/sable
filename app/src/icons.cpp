#include "icons.hpp"

#include <algorithm>
#include <cmath>

namespace {

/// Icons are described in a 0..1 box so one description works at every size.
struct Pen {
    ImDrawList* draw;
    ImVec2      origin;
    float       size;
    ImU32       colour;

    [[nodiscard]] ImVec2 at(float x, float y) const {
        return ImVec2(origin.x + x * size, origin.y + y * size);
    }
    /// Stroke weight scales with the icon, with a floor so it never vanishes.
    [[nodiscard]] float stroke(float w = 0.09f) const {
        return std::max(1.0f, size * w);
    }

    void line(float x0, float y0, float x1, float y1, float w = 0.09f) const {
        draw->AddLine(at(x0, y0), at(x1, y1), colour, stroke(w));
    }
    void rect(float x0, float y0, float x1, float y1, float w = 0.09f) const {
        draw->AddRect(at(x0, y0), at(x1, y1), colour, size * 0.06f, 0, stroke(w));
    }
    void fillRect(float x0, float y0, float x1, float y1) const {
        draw->AddRectFilled(at(x0, y0), at(x1, y1), colour, size * 0.06f);
    }
    void tri(float x0, float y0, float x1, float y1, float x2, float y2) const {
        draw->AddTriangleFilled(at(x0, y0), at(x1, y1), at(x2, y2), colour);
    }
    void quad(float x0, float y0, float x1, float y1,
              float x2, float y2, float x3, float y3) const {
        draw->AddQuadFilled(at(x0, y0), at(x1, y1), at(x2, y2), at(x3, y3), colour);
    }
    void circle(float cx, float cy, float r, float w = 0.09f) const {
        draw->AddCircle(at(cx, cy), r * size, colour, 0, stroke(w));
    }
    void disc(float cx, float cy, float r) const {
        draw->AddCircleFilled(at(cx, cy), r * size, colour);
    }
    /// A dashed run, for the marquee.
    void dashes(float x0, float y0, float x1, float y1, int count) const {
        for (int i = 0; i < count; ++i) {
            const float a = static_cast<float>(i) / count;
            const float b = a + 0.55f / count;
            line(x0 + (x1 - x0) * a, y0 + (y1 - y0) * a,
                 x0 + (x1 - x0) * b, y0 + (y1 - y0) * b, 0.08f);
        }
    }
    /// An arrowhead pointing along (dx, dy).
    void arrow(float x, float y, float dx, float dy, float len) const {
        const float n = std::sqrt(dx * dx + dy * dy);
        if (n <= 0.0f) return;
        const float ux = dx / n, uy = dy / n;
        const float px = -uy, py = ux;
        tri(x, y,
            x - ux * len + px * len * 0.6f, y - uy * len + py * len * 0.6f,
            x - ux * len - px * len * 0.6f, y - uy * len - py * len * 0.6f);
    }
};

}  // namespace

void drawIcon(ImDrawList* draw, Icon icon, ImVec2 topLeft, float size, ImU32 colour) {
    const Pen p{draw, topLeft, size, colour};

    switch (icon) {
        case Icon::Brush:
            // Handle running down-left, with a loaded tapering tip.
            p.line(0.16f, 0.86f, 0.55f, 0.44f, 0.11f);
            p.quad(0.50f, 0.38f, 0.62f, 0.28f, 0.90f, 0.08f, 0.66f, 0.52f);
            break;

        case Icon::Eraser:
            // A block held at an angle, with the worn edge marked.
            p.quad(0.16f, 0.70f, 0.54f, 0.24f, 0.86f, 0.44f, 0.48f, 0.90f);
            p.line(0.35f, 0.47f, 0.67f, 0.67f, 0.07f);
            break;

        case Icon::Fill:
            // A tipped bucket with a drop leaving it.
            p.quad(0.14f, 0.44f, 0.46f, 0.14f, 0.74f, 0.42f, 0.42f, 0.72f);
            p.line(0.46f, 0.14f, 0.60f, 0.06f, 0.07f);
            p.disc(0.82f, 0.72f, 0.10f);
            p.tri(0.82f, 0.50f, 0.73f, 0.74f, 0.91f, 0.74f);
            break;

        case Icon::Select:
            // Marching-ants rectangle.
            p.dashes(0.14f, 0.18f, 0.86f, 0.18f, 4);
            p.dashes(0.86f, 0.18f, 0.86f, 0.82f, 4);
            p.dashes(0.86f, 0.82f, 0.14f, 0.82f, 4);
            p.dashes(0.14f, 0.82f, 0.14f, 0.18f, 4);
            break;

        case Icon::Lasso: {
            // A dashed loop with a tail, drawn as a closed run of dashes so it
            // reads as the same "marching ants" idea as the marquee.
            constexpr int kSteps = 14;
            for (int i = 0; i < kSteps; i += 2) {
                const auto angleAt = [](int k) {
                    return 6.2831853f * static_cast<float>(k) / kSteps;
                };
                const float a0 = angleAt(i), a1 = angleAt(i + 1);
                p.line(0.50f + 0.33f * std::cos(a0), 0.42f + 0.30f * std::sin(a0),
                       0.50f + 0.33f * std::cos(a1), 0.42f + 0.30f * std::sin(a1), 0.08f);
            }
            p.line(0.50f, 0.72f, 0.36f, 0.92f, 0.08f);
            break;
        }

        case Icon::Hand:
            // A palm with three fingers and a thumb: enough of a hand at
            // sixteen pixels, and nothing at all below that.
            p.rect(0.34f, 0.46f, 0.72f, 0.86f, 0.08f);
            p.line(0.40f, 0.48f, 0.40f, 0.22f, 0.08f);
            p.line(0.53f, 0.48f, 0.53f, 0.14f, 0.08f);
            p.line(0.66f, 0.48f, 0.66f, 0.24f, 0.08f);
            p.line(0.34f, 0.62f, 0.18f, 0.50f, 0.08f);
            break;

        case Icon::Wand:
            // A wand on the diagonal with a spark at the tip.
            p.line(0.16f, 0.86f, 0.62f, 0.40f, 0.11f);
            p.line(0.62f, 0.40f, 0.78f, 0.24f, 0.14f);
            p.line(0.86f, 0.10f, 0.86f, 0.30f, 0.06f);
            p.line(0.76f, 0.20f, 0.96f, 0.20f, 0.06f);
            break;

        case Icon::Transform:
            // A frame with corner handles — what you grab, not what it does.
            p.rect(0.22f, 0.22f, 0.78f, 0.78f, 0.07f);
            p.fillRect(0.10f, 0.10f, 0.30f, 0.30f);
            p.fillRect(0.70f, 0.10f, 0.90f, 0.30f);
            p.fillRect(0.10f, 0.70f, 0.30f, 0.90f);
            p.fillRect(0.70f, 0.70f, 0.90f, 0.90f);
            break;

        case Icon::Text:
            // A serifed I: the one letterform that reads as "type" at 16 px
            // without being any particular alphabet's letter A.
            p.line(0.24f, 0.16f, 0.76f, 0.16f, 0.11f);   // top bar
            p.line(0.50f, 0.16f, 0.50f, 0.84f, 0.11f);   // stem
            p.line(0.32f, 0.84f, 0.68f, 0.84f, 0.11f);   // foot
            break;

        case Icon::Linework:
            // A curve with its control points on it — the thing the tool makes,
            // and the one part of it that is not just a stroke of paint.
            p.line(0.16f, 0.70f, 0.40f, 0.30f, 0.08f);
            p.line(0.40f, 0.30f, 0.62f, 0.66f, 0.08f);
            p.line(0.62f, 0.66f, 0.86f, 0.28f, 0.08f);
            p.disc(0.16f, 0.70f, 0.10f);
            p.disc(0.40f, 0.30f, 0.10f);
            p.disc(0.86f, 0.28f, 0.10f);
            break;

        case Icon::Gradient:
            // A framed ramp, drawn as bars that thin out. One colour is all a
            // Pen has, so the fade has to be made of coverage rather than of
            // value — which is what a dithered gradient looks like anyway.
            p.rect(0.12f, 0.16f, 0.88f, 0.84f, 0.06f);
            p.fillRect(0.18f, 0.22f, 0.40f, 0.78f);
            p.line(0.47f, 0.22f, 0.47f, 0.78f, 0.09f);
            p.line(0.58f, 0.22f, 0.58f, 0.78f, 0.06f);
            p.line(0.68f, 0.22f, 0.68f, 0.78f, 0.04f);
            break;

        case Icon::Plus:
            p.line(0.50f, 0.14f, 0.50f, 0.86f, 0.15f);
            p.line(0.14f, 0.50f, 0.86f, 0.50f, 0.15f);
            break;

        case Icon::Duplicate:
            p.rect(0.10f, 0.10f, 0.60f, 0.60f, 0.10f);
            p.fillRect(0.40f, 0.40f, 0.90f, 0.90f);
            break;

        case Icon::Delete:
            // A bin: lid, body, and two ribs.
            // Lid and a tapering body. The ribs a bigger bin icon would have
            // are dropped: below about 16 px they read as noise, and the
            // silhouette alone is unmistakable.
            p.line(0.12f, 0.28f, 0.88f, 0.28f, 0.11f);
            p.fillRect(0.40f, 0.10f, 0.60f, 0.22f);
            p.quad(0.22f, 0.34f, 0.78f, 0.34f, 0.70f, 0.90f, 0.30f, 0.90f);
            break;

        case Icon::Merge:
            // Two layers collapsing onto one.
            p.fillRect(0.16f, 0.10f, 0.84f, 0.22f);
            p.arrow(0.50f, 0.62f, 0.0f, 1.0f, 0.30f);
            p.fillRect(0.16f, 0.74f, 0.84f, 0.90f);
            break;

        case Icon::Group:
            // Folder.
            p.quad(0.10f, 0.24f, 0.42f, 0.24f, 0.50f, 0.36f, 0.10f, 0.36f);
            p.fillRect(0.10f, 0.32f, 0.90f, 0.84f);
            break;

        case Icon::Ungroup:
            // The same folder, with something leaving it.
            p.fillRect(0.08f, 0.46f, 0.60f, 0.88f);
            p.line(0.58f, 0.40f, 0.80f, 0.22f, 0.12f);
            p.arrow(0.90f, 0.12f, 1.0f, -0.82f, 0.30f);
            break;

        case Icon::Raise:
            p.line(0.50f, 0.90f, 0.50f, 0.42f, 0.16f);
            p.arrow(0.50f, 0.08f, 0.0f, -1.0f, 0.34f);
            break;

        case Icon::Lower:
            p.line(0.50f, 0.10f, 0.50f, 0.58f, 0.16f);
            p.arrow(0.50f, 0.92f, 0.0f, 1.0f, 0.34f);
            break;

        case Icon::Eye:
            // Lens as two arcs meeting at the corners, plus a pupil.
            // The path starts at the current point, so seed it with PathLineTo
            // and give each curve its two controls and an end point.
            draw->PathClear();
            draw->PathLineTo(p.at(0.08f, 0.50f));
            draw->PathBezierCubicCurveTo(p.at(0.30f, 0.16f), p.at(0.70f, 0.16f),
                                         p.at(0.92f, 0.50f));
            draw->PathBezierCubicCurveTo(p.at(0.70f, 0.84f), p.at(0.30f, 0.84f),
                                         p.at(0.08f, 0.50f));
            draw->PathStroke(colour, ImDrawFlags_Closed, p.stroke(0.08f));
            p.disc(0.50f, 0.50f, 0.15f);
            break;

        case Icon::EyeClosed:
            // A closed lid reads better than a struck-through eye at 16 px.
            draw->PathClear();
            draw->PathLineTo(p.at(0.10f, 0.42f));
            draw->PathBezierCubicCurveTo(p.at(0.32f, 0.70f), p.at(0.68f, 0.70f),
                                         p.at(0.90f, 0.42f));
            draw->PathStroke(colour, 0, p.stroke(0.08f));
            p.line(0.22f, 0.60f, 0.16f, 0.72f, 0.07f);
            p.line(0.50f, 0.66f, 0.50f, 0.80f, 0.07f);
            p.line(0.78f, 0.60f, 0.84f, 0.72f, 0.07f);
            break;

        case Icon::Lock:
            p.fillRect(0.22f, 0.46f, 0.78f, 0.86f);
            draw->PathClear();
            draw->PathArcTo(p.at(0.50f, 0.46f), 0.20f * size, 3.14159265f, 0.0f, 16);
            draw->PathStroke(colour, 0, p.stroke(0.09f));
            break;

        case Icon::Swap:
            p.rect(0.10f, 0.10f, 0.52f, 0.52f, 0.08f);
            p.fillRect(0.48f, 0.48f, 0.90f, 0.90f);
            p.line(0.62f, 0.30f, 0.84f, 0.30f, 0.07f);
            p.arrow(0.90f, 0.30f, 1.0f, 0.0f, 0.16f);
            break;

        case Icon::Reset:
            // A loop returning to where it started.
            draw->PathClear();
            draw->PathArcTo(p.at(0.50f, 0.52f), 0.30f * size, 0.6f, 5.4f, 24);
            draw->PathStroke(colour, 0, p.stroke(0.09f));
            p.arrow(0.74f, 0.20f, 0.6f, -1.0f, 0.22f);
            break;
    }
}

namespace {

/// Draws the icon centred inside the item just submitted.
void paintOverLastItem(Icon icon, float inset) {
    const ImVec2 lo = ImGui::GetItemRectMin();
    const ImVec2 hi = ImGui::GetItemRectMax();
    const float box = std::min(hi.x - lo.x, hi.y - lo.y) - inset * 2.0f;
    if (box <= 0.0f) return;

    const ImVec2 centre((lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f);
    // GetColorU32 folds in style.Alpha, which BeginDisabled lowers — so a
    // disabled icon dims along with its frame without any special case.
    const ImU32 colour = ImGui::GetColorU32(ImGuiCol_Text);
    drawIcon(ImGui::GetWindowDrawList(), icon,
             ImVec2(centre.x - box * 0.5f, centre.y - box * 0.5f), box, colour);
}

}  // namespace

bool iconButton(Icon icon, const char* id, const char* tooltip, float size) {
    const float side = size > 0.0f ? size : ImGui::GetFrameHeight();
    const bool pressed = ImGui::Button(id, ImVec2(side, side));
    paintOverLastItem(icon, side * 0.13f);
    if (tooltip != nullptr && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tooltip);
    return pressed;
}

bool iconToggle(Icon icon, const char* id, bool active, const char* tooltip,
                float size) {
    if (active) ImGui::PushStyleColor(ImGuiCol_Button,
                                      ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    const bool pressed = iconButton(icon, id, tooltip, size);
    if (active) ImGui::PopStyleColor();
    return pressed;
}

bool iconRadio(Icon icon, const char* label, bool active) {
    const float side = ImGui::GetFrameHeight();
    const float gap  = ImGui::GetStyle().ItemInnerSpacing.x;
    const float box  = side * 0.78f;

    // One Selectable spanning icon and label, so the whole row is the click
    // target rather than a fiddly glyph. The label is drawn by hand beside the
    // icon instead of by Selectable, which would put it underneath.
    ImGui::PushID(label);
    const bool clicked = ImGui::Selectable("##row", active, 0, ImVec2(0.0f, side));
    ImGui::PopID();

    const ImVec2 lo = ImGui::GetItemRectMin();
    const ImU32 colour = ImGui::GetColorU32(ImGuiCol_Text);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    drawIcon(draw, icon, ImVec2(lo.x + gap, lo.y + (side - box) * 0.5f), box, colour);

    const float textY = lo.y + (side - ImGui::GetFontSize()) * 0.5f;
    draw->AddText(ImVec2(lo.x + gap * 2.0f + box, textY), colour, label);
    return clicked;
}
