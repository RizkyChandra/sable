// The optional second PaintBackend: tiles in VRAM, dabs and compositing in
// compute shaders. D-021 (opt-in, CPU stays the reference) and D-025 (why
// SDL_GPU and not SDL_Renderer).
//
// Nothing here exposes SDL. A caller that wants the GPU asks for one and gets
// a backend or a nullptr; if it gets a nullptr the CPU backend is already the
// process default and there is nothing to do.
#pragma once

#include <memory>
#include <string>

#include "sbl/backend.hpp"

namespace sbl {

/// Whether this build has the GPU backend compiled in at all. False when SDL3
/// was not found at configure time — the headless `engine` CI job builds that
/// way, and so does anyone building only the library.
[[nodiscard]] bool gpuBackendCompiledIn() noexcept;

/// A GPU backend, or nullptr when this machine cannot provide one.
///
/// Returning nullptr rather than an error is the point: "no usable GPU" is the
/// normal case on the modest hardware D-019 targets, and it is not a failure —
/// the CPU backend paints. `why` is filled in with a one-line reason either
/// way, for the About box and for bug reports.
///
/// Creating one initialises SDL's video subsystem, which is refcounted, so an
/// application that has already done so pays nothing. It creates no window and
/// no event queue (D-022).
[[nodiscard]] std::unique_ptr<PaintBackend> makeGpuBackend(std::string* why = nullptr);

/// How much device memory the backend is holding tiles in, or 0 for a backend
/// that is not the GPU one. The undo budget is host memory and `memoryBytes()`
/// still tells the truth about it (#12) — this is the other number, and the
/// status bar shows both so that neither is a surprise.
[[nodiscard]] std::size_t gpuDeviceBytes(const PaintBackend& backend) noexcept;

}  // namespace sbl
