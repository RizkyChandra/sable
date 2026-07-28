// Generates the application icon by painting it with Sable's own engine.
//
// The icon is therefore original by construction (D-010) — it is a brush
// stroke this program drew, not artwork traced from anywhere. It also means
// the icon is reproducible from source rather than being a binary blob nobody
// can regenerate.
//
// Not part of the default build; the PNG it produces is committed. Rebuild with:
//   g++ -std=c++23 -Iengine/include packaging/make-icon.cpp \
//       -Lbuild -lengine -llodepng -lminiz -o /tmp/make-icon && /tmp/make-icon
#include <cmath>
#include <cstdio>
#include <vector>

#include "sbl/io.hpp"
#include "sbl/paint.hpp"

namespace {

void stroke(sbl::Document& doc, const sbl::BrushPreset& brush, sbl::StraightRgba8 colour,
            double x0, double y0, double x1, double y1, double bow) {
    sbl::Layer* layer = doc.active();
    sbl::Stroke s;
    std::vector<sbl::Dab> scratch;
    sbl::beginStroke(s, brush, colour, layer->id);
    sbl::PaintTarget target{*layer, s.pending, s.touched, doc.width, doc.height};

    for (double t = 0.0; t <= 1.0; t += 0.004) {
        sbl::InputSample sample;
        // A quadratic bow, so the mark is a sweep rather than a ruler line.
        sample.x = x0 + (x1 - x0) * t + bow * std::sin(t * 3.14159265358979);
        sample.y = y0 + (y1 - y0) * t - bow * std::sin(t * 3.14159265358979) * 0.6;
        // Pressure rises and falls, which is what gives the taper — the same
        // path a real stroke takes through the engine.
        sample.pressure = static_cast<float>(std::sin(t * 3.14159265358979));
        sbl::paintSample(s, target, sample, scratch);
    }
    doc.undo.push(std::move(s.pending));
}

}  // namespace

int main() {
    // Transparent background: an icon must not carry a white box into every
    // dark launcher that shows it.
    sbl::Document doc = sbl::makeDocument(256, 256, sbl::StraightRgba8{0, 0, 0, 0});

    sbl::BrushPreset brush = sbl::defaultPencil();
    brush.size          = 52.0f;
    brush.minSizeRatio  = 0.12f;
    brush.hardness      = 0.85f;
    brush.spacingFactor = 0.04f;

    // Sable: a dark sable-brush sweep, with a lighter one behind it for depth.
    stroke(doc, brush, sbl::StraightRgba8{120, 92, 74, 235}, 44.0, 196.0, 214.0, 74.0, 26.0);
    brush.size = 34.0f;
    stroke(doc, brush, sbl::StraightRgba8{58, 44, 38, 255}, 40.0, 210.0, 206.0, 62.0, 18.0);

    if (const auto result = sbl::exportPng(doc, "packaging/sable.png");
        !result.has_value()) {
        std::fprintf(stderr, "could not write the icon: %s\n",
                     result.error().detail.c_str());
        return 1;
    }
    std::puts("wrote packaging/sable.png");
    return 0;
}
