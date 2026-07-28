// Which backend the free functions go to, and nothing else. Every CPU
// implementation still lives beside the code it grew up with — paint.cpp,
// canvas.cpp, io.cpp — so this file stays readable as a routing table.
#include "sbl/backend.hpp"

namespace sbl {
namespace {

/// Null means "the CPU backend", so the reference implementation needs no
/// initialisation and works before anything has run.
PaintBackend* g_backend = nullptr;

}  // namespace

CpuBackend& cpuBackend() noexcept {
    static CpuBackend cpu;
    return cpu;
}

PaintBackend& paintBackend() noexcept {
    return g_backend != nullptr ? *g_backend : cpuBackend();
}

void setPaintBackend(PaintBackend* backend) noexcept { g_backend = backend; }

// ------------------------------------------------------------------ dispatch

void applyDab(PaintTarget& t, const Dab& dab) {
    // A stroke may name its own backend; #14's differential harness paints the
    // same stroke through two of them without touching the process default.
    (t.backend != nullptr ? *t.backend : paintBackend()).applyDab(t, dab);
}

UndoRecord bucketFill(Document& doc, LayerId target, std::int32_t x, std::int32_t y,
                      StraightRgba8 colour, int tolerance) {
    return paintBackend().bucketFill(doc, target, x, y, colour, tolerance);
}

UndoRecord fillSelection(Document& doc, LayerId target, StraightRgba8 colour) {
    return paintBackend().fillSelection(doc, target, colour);
}

UndoRecord transformRegion(Document& doc, LayerId target, const Selection& source,
                           const Transform& transform) {
    return paintBackend().transformRegion(doc, target, source, transform);
}

UndoRecord clearLayer(Layer& layer) { return paintBackend().clearLayer(layer); }

UndoRecord mergeLayerDown(Document& doc, LayerId id) {
    return paintBackend().mergeLayerDown(doc, id);
}

std::vector<PremulRgba8> compositeRect(const Document& doc, std::int32_t x,
                                       std::int32_t y, std::int32_t w, std::int32_t h) {
    return paintBackend().compositeRect(doc, x, y, w, h);
}

StraightRgba8 pickColour(const Document& doc, std::int32_t x, std::int32_t y) {
    return paintBackend().pickColour(doc, x, y);
}

}  // namespace sbl
