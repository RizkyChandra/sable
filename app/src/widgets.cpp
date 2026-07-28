#include "widgets.hpp"

#include <algorithm>
#include <cmath>

#include "imgui.h"

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr int   kRingSegments = 96;
constexpr float kRingThickness = 0.16f;    // fraction of the radius

ImU32 hueColour(float hue) {
    float r = 0.0f, g = 0.0f, b = 0.0f;
    ImGui::ColorConvertHSVtoRGB(hue, 1.0f, 1.0f, r, g, b);
    return ImGui::GetColorU32(ImVec4(r, g, b, 1.0f));
}

}  // namespace

bool colourWheel(const char* id, float hsv[3], float diameter) {
    ImGui::PushID(id);

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##wheel", ImVec2(diameter, diameter));
    const bool active = ImGui::IsItemActive();

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 centre(origin.x + diameter * 0.5f, origin.y + diameter * 0.5f);
    const float outer = diameter * 0.5f;
    const float inner = outer * (1.0f - kRingThickness);

    // --- hue ring, as a fan of quads. AddRectFilledMultiColor cannot do a
    // radial sweep, so the ring is the one place we place vertices by hand.
    for (int i = 0; i < kRingSegments; ++i) {
        const float a0 = (static_cast<float>(i)     / kRingSegments) * 2.0f * kPi;
        const float a1 = (static_cast<float>(i + 1) / kRingSegments) * 2.0f * kPi;
        const ImU32 c0 = hueColour(static_cast<float>(i)     / kRingSegments);
        const ImU32 c1 = hueColour(static_cast<float>(i + 1) / kRingSegments);

        // AddQuadFilled takes one colour, so a segment cannot gradient across
        // itself. At 96 segments each is under 4 degrees of hue and the banding
        // is invisible; overdraw the trailing edge in the next segment's colour
        // so the seams do not read as spokes.
        draw->AddQuadFilled(
            ImVec2(centre.x + std::cos(a0) * inner, centre.y + std::sin(a0) * inner),
            ImVec2(centre.x + std::cos(a0) * outer, centre.y + std::sin(a0) * outer),
            ImVec2(centre.x + std::cos(a1) * outer, centre.y + std::sin(a1) * outer),
            ImVec2(centre.x + std::cos(a1) * inner, centre.y + std::sin(a1) * inner),
            c0);
        draw->AddLine(
            ImVec2(centre.x + std::cos(a1) * inner, centre.y + std::sin(a1) * inner),
            ImVec2(centre.x + std::cos(a1) * outer, centre.y + std::sin(a1) * outer),
            c1, 1.5f);
    }

    // --- saturation / value square, inscribed in the ring
    const float half = inner * 0.70f;
    const ImVec2 sqMin(centre.x - half, centre.y - half);
    const ImVec2 sqMax(centre.x + half, centre.y + half);
    const ImU32 pure = hueColour(hsv[0]);
    draw->AddRectFilledMultiColor(sqMin, sqMax,
                                  IM_COL32_WHITE, pure,
                                  IM_COL32(0, 0, 0, 255), IM_COL32(0, 0, 0, 255));

    // --- interaction
    bool changed = false;
    if (active) {
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const float dx = mouse.x - centre.x;
        const float dy = mouse.y - centre.y;
        const float dist = std::sqrt(dx * dx + dy * dy);

        // Which control the drag belongs to is decided once, at the press —
        // otherwise dragging out of the square jumps to the hue ring mid-edit.
        static bool draggingRing = false;
        if (ImGui::IsItemActivated()) draggingRing = dist > half * 1.05f;

        if (draggingRing) {
            float angle = std::atan2(dy, dx);
            if (angle < 0.0f) angle += 2.0f * kPi;
            hsv[0] = angle / (2.0f * kPi);
        } else {
            hsv[1] = std::clamp((mouse.x - sqMin.x) / (half * 2.0f), 0.0f, 1.0f);
            hsv[2] = std::clamp(1.0f - (mouse.y - sqMin.y) / (half * 2.0f), 0.0f, 1.0f);
        }
        changed = true;
    }

    // --- markers
    const float hueAngle = hsv[0] * 2.0f * kPi;
    const float ringMid = (inner + outer) * 0.5f;
    draw->AddCircle(ImVec2(centre.x + std::cos(hueAngle) * ringMid,
                           centre.y + std::sin(hueAngle) * ringMid),
                    5.0f, IM_COL32_WHITE, 0, 2.0f);

    const ImVec2 pick(sqMin.x + hsv[1] * half * 2.0f,
                      sqMin.y + (1.0f - hsv[2]) * half * 2.0f);
    draw->AddCircle(pick, 5.0f, IM_COL32(0, 0, 0, 255), 0, 3.0f);
    draw->AddCircle(pick, 5.0f, IM_COL32_WHITE, 0, 1.5f);

    ImGui::PopID();
    return changed;
}

bool pressureCurveEditor(const char* id, sbl::PressureCurve& curve, float size,
                         float marker) {
    ImGui::PushID(id);

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##curve", ImVec2(size, size));
    ImDrawList* draw = ImGui::GetWindowDrawList();

    const ImVec2 lo(origin.x, origin.y);
    const ImVec2 hi(origin.x + size, origin.y + size);
    const auto toScreen = [&](float x, float y) {
        return ImVec2(lo.x + x * size, hi.y - y * size);
    };

    draw->AddRectFilled(lo, hi, IM_COL32(28, 28, 32, 255));
    for (int i = 1; i < 4; ++i) {
        const float t = static_cast<float>(i) / 4.0f;
        draw->AddLine(toScreen(t, 0.0f), toScreen(t, 1.0f), IM_COL32(60, 60, 66, 255));
        draw->AddLine(toScreen(0.0f, t), toScreen(1.0f, t), IM_COL32(60, 60, 66, 255));
    }
    // The diagonal is "no curve at all" — worth seeing what you deviate from.
    draw->AddLine(toScreen(0.0f, 0.0f), toScreen(1.0f, 1.0f), IM_COL32(70, 70, 78, 255));

    // The curve itself, sampled through eval() so the plot cannot disagree
    // with what the engine actually applies.
    constexpr int kSamples = 64;
    ImVec2 points[kSamples + 1];
    for (int i = 0; i <= kSamples; ++i) {
        const float t = static_cast<float>(i) / kSamples;
        points[i] = toScreen(t, curve.eval(t));
    }
    draw->AddPolyline(points, kSamples + 1, IM_COL32(120, 190, 255, 255), 0, 2.0f);

    // --- draggable control points
    bool changed = false;
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    static int dragging = -1;
    if (!ImGui::IsItemActive()) dragging = -1;

    for (std::size_t i = 0; i < curve.points.size(); ++i) {
        const ImVec2 at = toScreen(curve.points[i].first, curve.points[i].second);
        const float dx = mouse.x - at.x;
        const float dy = mouse.y - at.y;
        const bool near = (dx * dx + dy * dy) < 100.0f;

        if (ImGui::IsItemActivated() && near && dragging < 0)
            dragging = static_cast<int>(i);

        draw->AddCircleFilled(at, near || dragging == static_cast<int>(i) ? 6.0f : 4.0f,
                              IM_COL32(255, 255, 255, 255));
    }

    if (dragging >= 0 && ImGui::IsItemActive()) {
        auto& point = curve.points[static_cast<std::size_t>(dragging)];
        point.second = std::clamp((hi.y - mouse.y) / size, 0.0f, 1.0f);
        // The endpoints anchor the curve at 0 and 1; only their height moves,
        // or the curve stops covering the full pressure range.
        const bool interior = dragging > 0 &&
                              dragging + 1 < static_cast<int>(curve.points.size());
        if (interior) {
            const float lower = curve.points[static_cast<std::size_t>(dragging) - 1].first;
            const float upper = curve.points[static_cast<std::size_t>(dragging) + 1].first;
            point.first = std::clamp((mouse.x - lo.x) / size, lower + 0.02f, upper - 0.02f);
        }
        changed = true;
    }

    if (marker >= 0.0f) {
        const ImVec2 at = toScreen(marker, curve.eval(marker));
        draw->AddCircleFilled(at, 5.0f, IM_COL32(255, 190, 90, 255));
    }

    draw->AddRect(lo, hi, IM_COL32(90, 90, 96, 255));
    ImGui::PopID();
    return changed;
}
