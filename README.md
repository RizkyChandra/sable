# Sable

[![CI](https://github.com/RizkyChandra/sable/actions/workflows/ci.yml/badge.svg)](https://github.com/RizkyChandra/sable/actions/workflows/ci.yml)

A lightweight open-source raster painting application for Linux.
C++23 · SDL3 · Dear ImGui · MIT.

Fast pen response and a small interface, on ordinary hardware. Inspired by
PaintTool SAI's feel, built from original code.

**Status: Milestones 1–4 feature-complete for v1.** Canvas, seven brushes,
undo, PNG export, pen pressure with calibration, stabilizer, colour, layers
with blend modes, clipping and groups, selection, bucket fill, transform, the
`.sable` project format, PSD import, crash recovery, dockable panels,
reassignable keys, interface scaling, and light/dark themes.

**One v1 requirement is not met, and cannot be met here: US-00.** The stylus
spike needs a real tablet, and none was available. Every pressure claim in
D-002 — that `SDL_PenAxis` delivers pressure and tilt on both X11 and Wayland,
at a usable sample rate, without synthetic mouse duplicates — is verified only
against synthetic input. **Run US-00 before trusting the pen path.** It is half
a day, and D-002a is the fallback if it fails.

Deferred to Stage D by the PRD, not missing from v1: JPEG/WebP export, layer
masks, freehand selection, rulers, text, and colour management.

Before a public release the AppStream metadata needs a homepage and bug-tracker
URL — `appstreamcli validate` fails on that until the project is hosted
somewhere. See the comment in `packaging/org.sable.Sable.metainfo.xml`.

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
| Undo / redo | `Ctrl+Z` / `Ctrl+Y` |
| New canvas | `Ctrl+N` |
| Open / save project | `Ctrl+O` / `Ctrl+S` |
| Export PNG | `Ctrl+E` |
| Quit | `Ctrl+Q` |

**Every one of those is reassignable** — *View → Keyboard and interface*, which
also holds the interface scale. Those two are the accessibility commitment the
PRD makes in place of screen-reader support, so they are features rather than
preferences.

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
