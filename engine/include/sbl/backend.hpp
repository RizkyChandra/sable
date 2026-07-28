// The seam every pixel writer goes through, so that a GPU backend has
// somewhere to live (D-021). The CPU implementation is the default and the
// reference: when a backend and the CPU disagree, the CPU is right.
#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "sbl/canvas.hpp"
#include "sbl/io.hpp"
#include "sbl/paint.hpp"

namespace sbl {

/// Every operation in the engine that writes pixels, plus the reads that force
/// the pixels to be on the host.
///
/// The public free functions (`applyDab`, `bucketFill`, `flatten`, ...) are
/// still the API everything calls; each one dispatches here. Adding an
/// operation means adding it in both places, which is deliberate — the free
/// function is where the contract and the doc comment live.
///
/// **Failure.** No operation returns `std::expected`. A backend that fails
/// calls `recordFailure` and returns whatever an empty result is; the caller
/// picks the failure up later with `takeError`. This is a deliberate departure
/// from D-012's default shape, for two reasons:
///
///  1. GPU work is queued. The dispatch that fails is not the call that
///     submitted it, so a per-dab `std::expected` would name the wrong dab —
///     it would be a lie with a `[[nodiscard]]` on it.
///  2. `applyDab` runs hundreds of times per stroke and must not allocate
///     (US-02.9). Failure is a per-stroke event, so it is reported once per
///     stroke rather than once per dab.
///
/// What D-012 actually asks for is that a failure cannot be silently dropped.
/// `takeError` is `[[nodiscard]]`, the app checks it when the stroke ends, and
/// the readback paths that cannot continue without their pixels return
/// `std::expected` in the usual way.
///
/// **Threading.** A backend belongs to the thread that installed it. Engine
/// code that may run on the autosave worker — which has no device context —
/// names `cpuBackend()` explicitly instead of taking the process default; see
/// `flatten(doc, backend)` and its use in `encodeThumbnail`.
class PaintBackend {
public:
    virtual ~PaintBackend() = default;

    /// Short, stable, and written into bug reports: "cpu", "gpu".
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;

    // ------------------------------------------------------------- writers

    /// Not `noexcept`, unlike the CPU-only version this replaced: a backend
    /// that fails has to be able to build an error message, and building one
    /// may allocate. The CPU path still allocates nothing per dab.
    virtual void applyDab(PaintTarget& target, const Dab& dab) = 0;

    [[nodiscard]] virtual UndoRecord bucketFill(Document& doc, LayerId target,
                                                std::int32_t x, std::int32_t y,
                                                StraightRgba8 colour, int tolerance) = 0;
    [[nodiscard]] virtual UndoRecord fillSelection(Document& doc, LayerId target,
                                                   StraightRgba8 colour) = 0;
    [[nodiscard]] virtual UndoRecord transformRegion(Document& doc, LayerId target,
                                                     const Selection& source,
                                                     const Transform& transform) = 0;
    [[nodiscard]] virtual UndoRecord clearLayer(Layer& layer) = 0;
    [[nodiscard]] virtual UndoRecord mergeLayerDown(Document& doc, LayerId id) = 0;

    // --------------------------------------------------------------- reads

    [[nodiscard]] virtual std::vector<PremulRgba8> compositeRect(
        const Document& doc, std::int32_t x, std::int32_t y,
        std::int32_t w, std::int32_t h) = 0;

    /// The single-pixel mirror of `compositeRect`; the two must agree.
    [[nodiscard]] virtual StraightRgba8 pickColour(const Document& doc,
                                                   std::int32_t x, std::int32_t y) = 0;

    /// Makes `Tile::pixels()` the truth again for every tile in `doc`, on the
    /// calling thread.
    ///
    /// Host code that reads pixels without going through this interface —
    /// `encodeTile` on save, and `cloneDocument` handing a document to the
    /// autosave thread — is only correct after this has returned. A backend
    /// that keeps its pixels on a device downloads them here; the CPU backend
    /// does nothing, because the host copy never stopped being the truth.
    ///
    /// `const Document&` because a backend that uploaded a tile kept a `Tile*`
    /// to it and writes back through that, not through the document.
    [[nodiscard]] virtual std::expected<void, Error> readback(const Document& doc) = 0;

    // ------------------------------------------------------------- failure

    /// The first failure since the last call, which this clears. First rather
    /// than last: the first one is the cause, the rest are its wreckage.
    [[nodiscard]] std::optional<Error> takeError() noexcept {
        return std::exchange(failure_, std::nullopt);
    }

protected:
    void recordFailure(Error e) {
        if (!failure_.has_value()) failure_ = std::move(e);
    }

    /// The scanline flood fill, matched against what `*this` composites.
    ///
    /// Not virtual and not the whole of `bucketFill`: the flood itself is host
    /// work on host tiles either way (#12 — it is click-frequency, so the
    /// stall is affordable), but the image it matches against must be the one
    /// the artist clicked on. A GPU backend that let the CPU compositor
    /// decide the region would fill to a boundary half a level away from the
    /// one on screen.
    ///
    /// The caller is responsible for the tiles being on the host first.
    [[nodiscard]] UndoRecord floodFill(Document& doc, LayerId target,
                                       std::int32_t x, std::int32_t y,
                                       StraightRgba8 colour, int tolerance);

private:
    std::optional<Error> failure_;
};

/// Plain C++ over the host tile storage, with no device and no display server
/// behind it. D-021: this is the reference implementation, and it is what the
/// headless engine tests exercise.
///
/// It never calls `recordFailure` — there is nothing here that can fail that
/// would not also have taken the process with it.
class CpuBackend final : public PaintBackend {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "cpu"; }

    void applyDab(PaintTarget& target, const Dab& dab) override;

    [[nodiscard]] UndoRecord bucketFill(Document& doc, LayerId target,
                                        std::int32_t x, std::int32_t y,
                                        StraightRgba8 colour, int tolerance) override;
    [[nodiscard]] UndoRecord fillSelection(Document& doc, LayerId target,
                                           StraightRgba8 colour) override;
    [[nodiscard]] UndoRecord transformRegion(Document& doc, LayerId target,
                                             const Selection& source,
                                             const Transform& transform) override;
    [[nodiscard]] UndoRecord clearLayer(Layer& layer) override;
    [[nodiscard]] UndoRecord mergeLayerDown(Document& doc, LayerId id) override;

    [[nodiscard]] std::vector<PremulRgba8> compositeRect(
        const Document& doc, std::int32_t x, std::int32_t y,
        std::int32_t w, std::int32_t h) override;
    [[nodiscard]] StraightRgba8 pickColour(const Document& doc,
                                           std::int32_t x, std::int32_t y) override;

    /// Nothing to do: the host tiles are the only copy there has ever been.
    [[nodiscard]] std::expected<void, Error> readback(const Document&) override {
        return {};
    }
};

/// The reference backend. Always available, on any machine, with no display.
[[nodiscard]] CpuBackend& cpuBackend() noexcept;

/// The backend the free functions dispatch to. `cpuBackend()` until something
/// installs another one.
[[nodiscard]] PaintBackend& paintBackend() noexcept;

/// Installs the process default; `nullptr` restores the CPU backend. The
/// caller owns the backend and must keep it alive until it swaps it back.
///
/// D-021 wants one binary with a runtime switch, so this is process-wide state
/// rather than a parameter threaded through forty call sites. It is not
/// synchronised: swap it from the UI thread between strokes, which is what a
/// settings toggle is, and never from a worker.
void setPaintBackend(PaintBackend* backend) noexcept;

}  // namespace sbl
