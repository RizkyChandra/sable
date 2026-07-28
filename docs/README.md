# Sable — Developer Documentation

A lightweight open-source raster painting application for Linux, Windows, and
macOS. C++23 · SDL3 · Dear ImGui · MIT today, but see D-020 — any open source
licence may be used, and `LICENSE` changes with the code. Inspired by PaintTool
SAI's feel, built from original code.

## Read in this order

| # | Document | What it answers |
|---|---|---|
| 1 | [PRD.md](PRD.md) | What we are building, for whom, and what is out of scope |
| 2 | [DECISION-LOG.md](DECISION-LOG.md) | Which option we picked and why — including the full UI toolkit comparison |
| 3 | [DATA-MODEL.md](DATA-MODEL.md) | The types the engine owns and the `.sable` file format |
| 4 | [USER-STORIES.md](USER-STORIES.md) | Milestone 1–2 stories with testable acceptance criteria |

**Where the code is:** v2 is built — see the root `README.md`. The four
milestones this specification describes were finished at v1.0.0; v2 dropped the
scope limits it set (D-019 to D-023) and added file-format interoperability, an
opt-in GPU backend, and the feature parity work in Workstream 4. `engine/` and `app/` are the D-003 split; `tests/` holds the doctest
suite the cross-cutting requirements ask for.

Three decisions were added while implementing. Two of them record something
the original plan got wrong rather than merely elaborating it: **D-014**
(dependencies are fetched, not vendored) and **D-015** (SDL exposes no usable
per-device pen key, so there is one calibration profile). **D-016** answers the
long-open D-104 by tracking ImGui's docking branch at a pinned commit —
docking is still not in ImGui master, checked at v1.92.9.

**D-019 to D-023 release the original constraints**, because the target is now
PaintTool SAI 2 rather than a small Linux sketching tool. Read D-019 first: it
lists what each of D-003, D-004, D-007, D-009 and PRD §3 gave up and what that
cost. **D-020 is the one live constraint left** — any open source licence, and
`LICENSE` changes in the same commit as any copyleft code. What stays forbidden
regardless is `.sai2` and anything of SAI's (D-010, D-020).

**US-00 has still never been run**, because no tablet was available at any
point. It is the one v1 requirement outstanding. Everything downstream of it —
the pressure curve, the per-axis handling, the stabilizer — is built and unit
tested against synthetic input, which proves the maths and proves nothing about
the hardware. Run it before promising anyone that the pen works.

## The short version

- **C++23** (floor GCC 14 / Clang 18), CMake + Ninja. Two targets: `engine` and
  `app`. The engine links no ImGui and creates no window, event queue, or input
  device — it may call SDL (D-022). Engine APIs return
  `std::expected<T, sbl::Error>`; nothing on a file or device path may terminate
  the process.
- **SDL3** for window, input, and rendering — chosen over Qt, GTK4, FLTK,
  wxWidgets, and JUCE on one criterion: `SDL_PenAxis` exposes seven stylus axes
  and we own the event queue, so no toolkit rate-limiting drops samples. See
  D-002.
- **Dear ImGui** for the interface, over `imgui_impl_sdl3` +
  `imgui_impl_sdlrenderer3`. Native file dialogs come from SDL3, not ImGui.
- **CPU-first tiled renderer**, 256 × 256 sparse tiles, premultiplied RGBA8, one
  streaming texture per visible tile. CPU compositing is the default and the
  reference; a GPU backend is opt-in at runtime (D-021). That backend is
  SDL_GPU compute shaders over an arena of tiles in VRAM, and it is off unless
  the artist turns it on in View — D-025 has the reasoning, the limits and the
  measurements.
- **Event-driven loop**, not a game loop. Near-zero CPU when nobody is drawing.
- **Milestone 1** is a window, a canvas, a brush, undo, and PNG export. Nothing
  else.

## Three things that will bite you

Listed here because each one produces a bug that looks like something else:

1. **SDL sends position and pressure in separate events.** Keep per-`SDL_PenID`
   axis state and snapshot it on motion. See `DATA-MODEL.md`.
2. **SDL can synthesise mouse events from the pen.** Left on, every stroke paints
   twice. Disable the hint at startup and assert it in US-00.
3. **Premultiplied alpha needs `SDL_BLENDMODE_BLEND_PREMULTIPLIED`.** The default
   blend mode renders "nearly right" — grey fringes on soft edges — for as long
   as it takes someone to notice.

## Known limitation

Dear ImGui has no AT-SPI integration, so the application is **not screen-reader
accessible**. This was accepted knowingly as the cost of the input layer.
Full keyboard operability with reassignable bindings is the compensation we
commit to. See PRD §6 and D-002.

## Provenance

These four documents are the whole specification. They replace an earlier set of
planning drafts (a project scope, an engine design, an implementation roadmap, a
pen-pressure design) and a set of PaintTool SAI observation notes gathered from
screenshots. Everything from those that survived scrutiny is folded in here; the
originals have been deleted.

Two things worth knowing about what came from where:

- The SAI notes were **competitor observations, never a specification.** Nothing
  in this project should be traced back to matching them (D-010).
- One of those notes recorded an individual artist's customised key bindings
  rather than any default, and contradicted the general shortcut reference. That
  conflict is preserved in D-101 — read it before choosing a default key map, and
  do not treat any SAI binding as authoritative.
