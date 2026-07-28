# Sable

[![CI](https://github.com/RizkyChandra/sable/actions/workflows/ci.yml/badge.svg)](https://github.com/RizkyChandra/sable/actions/workflows/ci.yml)

A lightweight open-source raster painting application for Linux.
C++23 · SDL3 · Dear ImGui · MIT.

Fast pen response and a small interface, on ordinary hardware. Inspired by
PaintTool SAI's feel, built from original code.

**Status: v2 — competing with PaintTool SAI 2 directly.**

Canvas with pan, zoom and rotation. Pencil, opaque, airbrush, marker,
watercolour and smudge brushes. Pen pressure with editable curves, per-device
calibration and a pulled-string stabilizer. Layers with 13 blend modes,
clipping, groups, linework and text. Rectangle, lasso and magic-wand selection.
Bucket fill, transform, perspective and symmetry rulers.

**Reads and writes other applications' files:** PSD (layered, both directions,
with layer masks), OpenRaster, and Krita `.kra` (read). Its own `.sable` is a
plain ZIP of PNG tiles you can open with `unzip`.

**Painting runs on the CPU or the GPU, your choice** (*View → Paint and
composite on the GPU*). The CPU path stays the default and the reference
implementation; the GPU is roughly 100× faster on a large multi-layer
composite. A test suite compares the two backends pixel for pixel on 32
scenarios and fails on any colour difference over one level, or any alpha
difference at all — so what you paint is what you save whichever you use. If no
GPU is available, Sable says so in the status bar and keeps working.

Crash recovery that never overwrites your file. Dockable panels, reassignable
keys for every action, interface scaling, light and dark themes.

**One requirement remains unmet, and it is the important one: US-00.** The
stylus spike has never been run, on any platform, because no tablet was
available at any point during development. Every claim about pressure, tilt and
sample rate is verified against *synthetic* input only. The engine maths has
204 tests behind it; the hardware path has none. **Run US-00 before trusting
the pen.** It is half a day, and `docs/DECISION-LOG.md` D-002a is the recorded
fallback if it fails.

Windows and macOS builds compile and pass CI, but nobody has painted a stroke
in either by hand, and neither is signed or notarised.

## Build

Needs a C++23 compiler (GCC 14 / Clang 18 or newer), CMake 3.24+, Ninja, and
SDL3. Dear ImGui and lodepng are fetched at configure time.

```sh
cmake -S . -B build -G Ninja
cmake --build build
./build/sable
```

The first configure needs network access: Dear ImGui, lodepng, miniz,
nlohmann/json and doctest are fetched by CMake rather than vendored (D-014).
SDL3 must already be installed.

To build only the engine and its tests — no SDL3, no display server, which is
what CI's fast job does:

```sh
cmake -S . -B build -G Ninja -DSABLE_BUILD_APP=OFF && cmake --build build
```

Run the tests — they are headless and need no display server:

```sh
ctest --test-dir build --output-on-failure
```

Check the SDL and ImGui wiring without a window:

```sh
SDL_VIDEODRIVER=offscreen ./build/sable --selftest
```

Install, or build a self-contained AppImage:

```sh
sudo cmake --install build            # /usr/local by default
packaging/build-appimage.sh           # -> Sable-x86_64.AppImage, ~3.7 MB
```

Sanitizers, as the cross-cutting requirements ask for:

```sh
cmake -S . -B build-asan -G Ninja -DSABLE_ASAN=ON && cmake --build build-asan
./build-asan/engine_tests
```

## Using it

| | |
|---|---|
| Draw | Left mouse button, or the pen |
| Sample a colour | `Alt` + click |
| Pan | Space + drag, or middle mouse button |
| Zoom | Scroll wheel, about the cursor |
| Fit / actual size | `Ctrl+0` / `Ctrl+Alt+0` |
| Brush / eraser | `B` / `E` — or just turn the pen over |
| Brush size | `[` and `]`, or the slider |
| Swap / reset colours | `X` / `D` |
| Fill / select tool | `G` / `M` |
| Text tool | `F` — then click, or press Enter to place it in the middle |
| Undo / redo | `Ctrl+Z` / `Ctrl+Y` |
| New canvas | `Ctrl+N` |
| Open / save project | `Ctrl+O` / `Ctrl+S` |
| Export PNG | `Ctrl+E` |
| Quit | `Ctrl+Q` |

**Every one of those is reassignable** — *View → Keyboard and interface*, which
also holds the interface scale. Those two are the accessibility commitment the
PRD makes in place of screen-reader support, so they are features rather than
preferences.

**Text** goes on a layer of its own and stays editable: select that layer, press
the tool's key, and the words come back. Typing goes through SDL's text-input
events rather than a UI text field, so an input method composes straight onto
the canvas — the half-finished characters appear underlined at the caret. A text
layer refuses paint until *Rasterise text* in the layer panel gives up the words
and keeps the picture.

Right-click a size preset to set it to the current brush size. **View** opens
the pressure calibration curve and the tablet test pad — the test pad is where
to look first if the pen feels wrong, and it says so plainly when no tablet is
present rather than showing zeros.

Projects save as `.sable`, which is a ZIP: `unzip -l yours.sable` lists a JSON
manifest and one PNG per painted tile, all openable in any image viewer. That
is deliberate (D-011) — save bugs are debuggable without writing a parser.

While a document has unsaved changes Sable writes a recovery copy under
`$XDG_STATE_HOME/sable/recovery` every couple of minutes, on a worker thread
holding its own private copy of the document. It **never** writes over your
file, and restoring is always something you choose (D-013).

Undo history is capped at 256 MB by default, adjustable in *View → Keyboard
and interface*. Ordinary work never reaches it; if it ever does, the oldest
steps go first and the status bar says how many (D-017).

Preferences live in `$XDG_DATA_HOME/sable/sable/settings.conf` (usually
`~/.local/share/...`), alongside `layout.ini` for the panel arrangement. Both
are plain text, safe to edit or delete.

Every action is reachable from the keyboard. Dear ImGui has no AT-SPI
integration, so Sable is **not screen-reader accessible** — a known gap,
accepted knowingly as the cost of the input layer, with keyboard operability
as the compensation. See `docs/PRD.md` §6.

## Layout

```
engine/     static library. Links NO SDL, NO ImGui — a stray SDL include
            here fails to build rather than being caught at review time.
app/        SDL3 + ImGui: event loop, canvas viewport, menus, dialogs.
tests/      doctest unit tests, all runnable headless.
docs/       the specification. Start with docs/README.md.
```

## Documentation

`docs/README.md` is the entry point. In short: `PRD.md` is what we are
building and what is out of scope, `DECISION-LOG.md` is which option we picked
and why, `DATA-MODEL.md` is the types the engine owns, and `USER-STORIES.md`
is the acceptance criteria this milestone was built against.

## Originality

No PaintTool SAI source, icons, brush textures, bundled assets, interface
artwork, or project-format internals. See `docs/DECISION-LOG.md` D-010.
