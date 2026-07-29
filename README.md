# Sable

**A small, fast painting program for Linux.**

[![CI](https://github.com/RizkyChandra/sable/actions/workflows/ci.yml/badge.svg)](https://github.com/RizkyChandra/sable/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/RizkyChandra/sable)](https://github.com/RizkyChandra/sable/releases/latest)
[![Licence: MIT](https://img.shields.io/badge/licence-MIT-blue)](LICENSE)

Sable opens in well under a second, gets out of your way, and runs on the laptop
you already have. It is for the artist who wants to sketch for twenty minutes
and close it again — not to configure a workspace first.

C++23 · SDL3 · Dear ImGui · MIT licensed · no account, no telemetry, no network.

---

## For artists

### Get it

Grab a build from the [latest release](https://github.com/RizkyChandra/sable/releases/latest).

| You have | Take this |
|---|---|
| **Linux**, any distribution | `Sable-x86_64.AppImage` — `chmod +x` it and run. Nothing to install. |
| **Linux**, prefer an archive | `sable-linux-x86_64.tar.gz` |
| **Windows** 10 or 11 | `sable-windows-x86_64.zip` |
| **macOS**, Apple silicon or Intel | `sable-macos-universal.zip` |

SDL3 is linked in, so there is nothing to install alongside. Check your download
against `SHA256SUMS` if you like.

### What it does

Paint with a **pencil, opaque brush, airbrush, marker, watercolour or smudge**,
in 8-bit or 16-bit colour. They make different marks, not the same mark at
different settings: the pencil catches paper tooth, the marker lays down a
chisel nib that turns as you tilt the pen. Pressure from a graphics tablet
drives brush size and density through a curve you shape yourself, with a
stabilizer for steady line art.

Work in **layers** — 13 blend modes, clipping, folders, plus text layers you can
re-edit and linework layers whose curves stay adjustable after you draw them:
close them, move a whole stroke, or recolour one long after it was drawn. Give
any layer a **mask** and paint it with the ordinary brushes, so fading something
out costs nothing you cannot undo. Lay down **linear and radial gradients**.

Select with a **rectangle, lasso or magic wand**, then fill, transform or paint
inside it — and **keep the selection**, by name, in the file. A selection you
spent five minutes on is worth more than five minutes. Take one from a layer's
alpha or from a mask, and combine them with add, subtract and intersect without
losing a soft edge. Draw along **perspective and symmetry rulers**. Rotate the
canvas to whatever angle your hand prefers.

Open **several drawings at once**, as tabs. Each keeps its own zoom, its own
undo history and its own place on the canvas, and you can copy between them.

**Your colours are the colours you chose.** Sable reads the ICC profile in a
file it opens rather than assuming everything is sRGB, so an Adobe RGB or
Display P3 document arrives looking the way it left. A new document is still
plain sRGB, and an untagged file is treated exactly as it always was.

**Your files stay yours.** Sable saves `.sable` projects, imports and exports
**Photoshop PSD** (layers, groups, masks) and **OpenRaster**, and reads **Krita
`.kra`**. A `.sable` file is an ordinary ZIP — run `unzip -l` on one and you will
find a readable JSON manifest and one PNG per painted tile. Nothing about your
artwork is locked inside this program.

**It tries hard not to lose your work.** While a drawing has unsaved changes,
Sable writes a recovery copy every couple of minutes on a background thread,
into its own directory. It never writes over your file, and restoring is always
something you choose. Undo is capped at 256 MB by default, and if it ever has to
drop old steps it says so in the status bar rather than doing it quietly.

### Getting around

| | |
|---|---|
| Draw | Left mouse button, or the pen |
| Sample a colour | `Alt` + click |
| Pan / zoom | `Space` + drag or middle-drag / scroll wheel |
| Rotate canvas | `,` and `.` — `Ctrl+Shift+0` to straighten |
| Fit / actual size | `Ctrl+0` / `Ctrl+Alt+0` |
| Brush, eraser | `B`, `E` — or just turn the pen over |
| Fill, select, lasso, wand | `G`, `M`, `L`, `W` |
| Transform, text, linework | `T`, `F`, `V` |
| Gradient | `R` |
| Brush size | `[` and `]`, or the slider |
| Swap / reset colours | `X` / `C` |
| Fill selection / clear layer | `Backspace` / `Delete` |
| Undo / redo / deselect | `Ctrl+Z` / `Ctrl+Y` / `Ctrl+D` |
| Open / save / export | `Ctrl+O` / `Ctrl+S` / `Ctrl+E` |
| New tab / close tab | `Ctrl+N` / `Ctrl+W` |
| Next / previous tab | `Ctrl+Tab` / `Ctrl+Shift+Tab` |
| Copy / paste between tabs | `Ctrl+C` / `Ctrl+V` |

**Every one of those can be changed.** *View → Keyboard and interface* rebinds
any action and scales the whole interface for a large or small display.
Right-click a brush-size preset to set it to your current size.

*View* also holds the **pressure calibration curve** and a **tablet test pad** —
if the pen feels wrong, look there first. It shows raw and normalised pressure
side by side, which is how you tell a tablet problem from a calibration one.

### Painting on the GPU

*View → Paint and composite on the GPU* moves painting and layer compositing to
your graphics card. On a large multi-layer canvas it is roughly a hundred times
faster.

It is **off by default, and the CPU stays the reference implementation.** A test
suite compares the two across 35 scenarios and fails on any colour difference
above one level, or any alpha difference at all — so a picture is the same
picture whichever one drew it. If your machine has no usable GPU, Sable says so
in the status bar and carries on.

### Things to know before you rely on it

**Tablet support has never been tested on a tablet.** Not on any platform. No
hardware was available while Sable was written, so every claim about pressure,
tilt and sample rate is verified against simulated input only. The drawing
engine has 272 automated tests behind it; the pen path has none. If your tablet
works, that is good news rather than a guarantee — please
[open an issue](https://github.com/RizkyChandra/sable/issues) either way.

**Sable is not screen-reader accessible.** Its interface toolkit provides no
AT-SPI integration, and that was accepted knowingly when the toolkit was chosen
([D-002](docs/DECISION-LOG.md)). Full keyboard operability and interface scaling
are offered instead. This is a real gap, recorded as a decision rather than left
as an oversight.

**Windows and macOS builds are unproven.** They compile and pass the automated
tests, but nobody has painted a stroke in either by hand, and neither is signed —
Windows SmartScreen will warn, and macOS needs permission under *Privacy &
Security*.

Preferences and recovery files live under `$XDG_DATA_HOME/sable` and
`$XDG_STATE_HOME/sable`: plain text and ordinary ZIPs, safe to inspect or delete.

---

## For contributors

You are welcome here, and the codebase is arranged to be readable before it is
clever.

### Build it

You need a C++23 compiler (GCC 14 / Clang 18 or newer), CMake 3.24+, Ninja and
SDL3. The first configure fetches Dear ImGui, lodepng, miniz, nlohmann/json,
Little CMS and doctest, so it needs network access once. Every one of them is
MIT, zlib or public domain.

```sh
cmake -S . -B build -G Ninja
cmake --build build
./build/sable
```

The engine builds and tests **with no SDL and no display server**. This is the
fast loop, and the first thing CI gates on:

```sh
cmake -S . -B build -G Ninja -DSABLE_BUILD_APP=OFF && cmake --build build
ctest --test-dir build --output-on-failure     # 272 cases
```

Worth running before you open a pull request:

```sh
SDL_VIDEODRIVER=offscreen ./build/sable --selftest     # drives the real app, headless
cmake -S . -B build-asan -G Ninja -DSABLE_ASAN=ON -DSABLE_BUILD_APP=OFF
cmake --build build-asan && ./build-asan/engine_tests   # ASan + UBSan
```

### How it is arranged

```
engine/     The painting engine. Pure C++23 — tiles, layers, brushes, undo,
            file formats. Testable with no window and no graphics driver.
app/        SDL3 + Dear ImGui: event loop, canvas view, panels, input.
tests/      272 doctest cases, all headless. differential.cpp compares the
            CPU and GPU backends against one another.
docs/       The specification. Start at docs/README.md.
packaging/  Desktop entry, icon, AppStream metadata, AppImage script.
```

The canvas is **sparse 256×256 tiles** in premultiplied RGBA, so memory follows
the painted area rather than the canvas size — a 4000×4000 document with a mark
in one corner allocates a handful of tiles. Compositing runs on the CPU by
default; the GPU backend is a second implementation behind the same interface.
The main loop **blocks** when nobody is drawing, so an idle window costs nothing.

### Read the decision log

`docs/DECISION-LOG.md` holds 35 entries answering *"why is it like this?"* — why
premultiplied and straight colour are different C++ types, why the undo budget
counts bytes and not steps, why the GPU is opt-in. If something looks odd, the
reason is usually written down.

It has one rule: **append, never edit a decided entry.** If a decision changes,
add a new one and mark the old superseded. That history is what lets you argue
with this codebase instead of guessing at it.

### What a change is expected to carry

- **Tests.** Non-trivial logic gets one that fails without your change. Several tests here were validated by deliberately breaking the code to confirm they caught it — that is the standard.
- **Green CI.** Five jobs: engine tests, ASan + UBSan, the application build, ThreadSanitizer on the background save, and packaging metadata.
- **No warnings.** `-Wall -Wextra -Wpedantic`, and the tree is clean today.
- **Comments that explain *why*.** The code already says what it does.
- **Agreement between backends.** If you touch a pixel path, `tests/differential.cpp` must still pass, or a drawing changes depending on a setting.
- **Your own gaps, named.** A pull request that says what it did not do is worth more than one that quietly leaves it out.

### Good places to start

Browse the [open issues](https://github.com/RizkyChandra/sable/issues). One in
particular:

- **[#16 — run the stylus spike](https://github.com/RizkyChandra/sable/issues/16).** If you own a graphics tablet you can close the single largest unknown in this project in an afternoon. `docs/USER-STORIES.md` US-00 spells out what to check, and the tablet test pad already displays most of it. Brush shape now follows the pen's tilt (D-032) and has been tuned against nothing but synthetic input, so this is more valuable than it was.
- **A brush editor.** #47 gave the engine stamp shapes and paper grain, and the interface can still only move a strength slider. Choosing a mask, or loading one from an image, is a self-contained piece of work with a visible result.

Reports are as valuable as code. If a `.psd` opens wrong or your tablet behaves
strangely, that is worth an issue.

### The documentation

`docs/README.md` is the entry point. In short: **PRD.md** is what is being built
and what is deliberately out of scope; **DECISION-LOG.md** is every choice and
its cost; **DATA-MODEL.md** is the types the engine owns and the `.sable`
format; **USER-STORIES.md** is the acceptance criteria the first milestones were
built against.

---

## Originality and licence

MIT — see [LICENSE](LICENSE). Every dependency is MIT, zlib or public domain.

Sable was inspired by PaintTool SAI's *feel* and contains none of its code,
icons, brush textures, artwork or file-format internals, and neither reads nor
writes `.sai2`. The application icon is drawn by Sable's own engine
(`packaging/make-icon.cpp`). Krita `.kra` support was written from the
documented format, never from Krita's GPL source. See
[D-010](docs/DECISION-LOG.md) and D-020.
