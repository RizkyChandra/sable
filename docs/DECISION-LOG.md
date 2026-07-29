# Sable — Decision Log

One place where "which option did we pick, and why?" is answered. The earlier
design notes offered alternatives in several places ("C++ or Rust", "Qt or GTK",
"8-bit or 16-bit"). A developer cannot start on an alternative, so this file
picks one of each.

Status values: **Decided** (build on it), **Open** (needs an answer before the
milestone named), **Superseded** (kept for history).

---

## D-001 — Implementation language: C++23

**Status:** Decided
**Affects:** everything

C++23, compiler floor **GCC 14 / Clang 18**. No compiler-specific extensions.

**Why C++:** the widest choice of graphics and input libraries, the deepest pool
of contributors for a drawing application, and no binding layer between us and
SDL3 or Dear ImGui — both are C/C++ libraries, so C++ is their native habitat.

**Why 23 rather than 20:** one feature earns it. `std::expected<T, E>` lets the
engine return typed errors with no exceptions and no dependency, which suits the
"never lose the artist's work" principle better than an exception that can unwind
through a paint loop. See D-012. The rest is small change: `std::print` for
logging, and `views::enumerate` / `views::zip` for the interpolator and tile
loops.

**Use a conservative subset.** C++23 support is uneven, so:

| Use | Avoid |
|---|---|
| `std::expected` | modules — still not worth the build-system pain |
| `std::print` / `std::println` | `std::mdspan` — support is thin, and `y * 256 + x` in a helper is fine |
| `views::enumerate`, `views::zip` | `std::flat_map` — late support, no use here |
| `std::to_underlying`, `[[assume]]` | anything you have to check three compilers for |

**Verify the floor before committing.** Feature-to-version mapping varies between
libstdc++ and libc++, and Ubuntu LTS may ship a compiler a release behind. Check
that GCC 14 builds `std::expected` and `std::print` on your target distributions,
and drop to C++20 plus an exception boundary if it does not — the design survives
that swap, only D-012 changes.

**Cost accepted:** the memory-safety guarantees a Rust build would have given
around the renderer and the background save thread are now our responsibility.
The specific places this bites are called out in `DATA-MODEL.md`; read the
threading note there before writing the save path.

## D-002 — Windowing and input: SDL3. UI: Dear ImGui

**Status:** Decided
**Affects:** window, event loop, stylus input, every panel and dialog

This decision was made on one criterion: **does it give us pen pressure?**
Everything else about a UI layer is recoverable. Not getting pressure, tilt, and
a high enough sample rate is fatal to a painting application, and it cannot be
worked around from inside the widget layer.

### What was evaluated

| Option | Stylus pressure | Widgets | Licence | Verdict |
|---|---|---|---|---|
| **SDL3 + Dear ImGui** | 7 axes via `SDL_PenAxis` | Build on ImGui | zlib + MIT | **Chosen** |
| Qt | `QTabletEvent` | Best-in-class | LGPL / commercial | Strong runner-up |
| GTK4 | `GestureStylus` + `backlog()` | Yes | LGPL | Viable; Wayland-only in practice |
| FLTK 1.5 | Claimed, unverified | Yes, lightweight | LGPL + static exception | Unproven |
| wxWidgets | Windows only; none on Linux | Yes | permissive | Rejected |
| JUCE | Yes | Yes | **AGPLv3** / commercial | Rejected — incompatible with MIT |
| GLFW | None | None | zlib | Rejected |

### Why SDL3 wins on the criterion that matters

`SDL_PenAxis` exposes more of the stylus than any other option evaluated:

| Axis | Range |
|---|---|
| `SDL_PEN_AXIS_PRESSURE` | 0.0 – 1.0 |
| `SDL_PEN_AXIS_XTILT` / `YTILT` | -90.0 – 90.0 degrees |
| `SDL_PEN_AXIS_DISTANCE` | 0.0 – 1.0 (hover height) |
| `SDL_PEN_AXIS_ROTATION` | -180.0 – 179.9 degrees (barrel twist) |
| `SDL_PEN_AXIS_SLIDER` | airbrush wheel |
| `SDL_PEN_AXIS_TANGENTIAL_PRESSURE` | barrel squeeze |

Each pen gets a stable `SDL_PenID` for the life of the process, which is exactly
what per-device calibration profiles need (`TabletProfile::deviceKey`).

Equally important: **we pump the event queue ourselves.** GTK4 rate-limits input
delivery and needs `backlog()` to recover the dropped samples; SDL hands us every
queued event when we ask. The sample-loss problem that shapes a GTK
implementation simply does not exist here. For an application whose entire value
is stroke fidelity, owning the event loop is the right trade.

### What this costs, plainly

Dear ImGui is not a desktop widget toolkit, and choosing it means accepting four
things:

1. **No accessibility.** GTK and Qt bring AT-SPI; ImGui brings nothing. A
   screen-reader user cannot operate this application. The mitigation we *can*
   deliver is full keyboard operability of every action — that is a hard
   requirement in `USER-STORIES.md`, not a nice-to-have. Revisit if a user asks;
   the honest answer today is that this is a known gap.
2. **Weak IME and text input.** Fine for M1–M3. It becomes a real problem for the
   text tool in Stage D. Plan to lean on SDL3's text-input APIs there rather than
   ImGui's default field behaviour.
3. **We build the widgets.** Colour wheel, layer list, brush-preset grid, and
   dockable panels are ours to write. ImGui gives sliders, menus, windows, and
   drawing primitives; a colour wheel is a custom widget in any of these
   toolkits, but the layer list and preset grid would have been free in Qt.
4. **Theming and HiDPI are manual.** ImGui will not look native and will not
   follow the desktop theme. Budget time for scaling; do not discover it at M4.

**Use SDL3's native file dialogs** (`SDL_ShowOpenFileDialog` /
`SDL_ShowSaveFileDialog`), never an ImGui file browser. File dialogs are where a
non-native UI feels worst, and SDL gives us the real thing for free.

**On docking:** ImGui's docking and multi-viewport support has historically lived
on a separate branch rather than master. Check its current status before
promising dockable panels in M4 — if it is still a branch, decide early whether
to track it, because switching branches late is disruptive.

### Rendering backend

`imgui_impl_sdl3` for platform, `imgui_impl_sdlrenderer3` for rendering.
`SDL_Renderer` is enough: we are compositing on the CPU (D-007) and the GPU only
blits finished tiles. Do not reach for `SDL_GPU` or raw OpenGL — it buys nothing
for our workload and costs portability.

Canvas display: one `SDL_Texture` per visible tile,
`SDL_TEXTUREACCESS_STREAMING`, uploaded only when that tile is dirty. Pan and
zoom become the destination rectangles we draw those textures into, so the GPU
does the scaling for free. Evict textures for tiles that leave the viewport.

**Set `SDL_BLENDMODE_BLEND_PREMULTIPLIED`** when drawing canvas textures. We
store premultiplied alpha (D-004); using the default straight-alpha blend mode
produces exactly the dark-halo bug the type split in `DATA-MODEL.md` exists to
prevent, and it will look "nearly right" long enough to ship.

### Before writing Milestone 2, verify this holds

Half-day spike, specified as US-00 in `USER-STORIES.md`. Confirm pressure
actually arrives from a real tablet, **on both X11 and Wayland** — SDL's pen
support may differ between video backends, and unlike the GTK path this project
is not automatically Wayland-only. Force each backend with the `SDL_VIDEODRIVER`
environment variable and test both.

If pressure does not arrive, D-002a is the fallback.

## D-002a — Fallback: read the tablet protocols directly

**Status:** Open — only if the D-002 spike fails
**Affects:** input layer

Keep SDL3 for windowing and rendering, but read tablet input ourselves:
`zwp_tablet_v2` on Wayland, XInput2 valuators on X11.

**Cost:** two platform paths and a protocol implementation to maintain. Choose it
only if forced, and prefer fixing or reporting the SDL gap first — SDL3's pen API
is new enough that a bug is more likely than a design limitation.

## D-003 — Build: CMake + Ninja, `engine` and `app` targets

**Status:** Decided; the "links NO SDL" and "resist adding more" rules are
**Superseded by D-022**
**Affects:** repo layout

The original project scope asked for one reproducible build system to be chosen
and documented before implementation begins. This is it.

```
sable/
├── CMakeLists.txt
├── third_party/          # imgui, lodepng, miniz, json — vendored
├── engine/               # static lib. Links NO SDL, NO ImGui.
│   ├── include/sbl/
│   └── src/
├── app/                  # SDL3 + ImGui: event loop, widgets, canvas view
└── tests/
```

**Why the split is a CMake target and not a convention:** `engine` does not link
SDL3 or ImGui, so a stray `#include <SDL3/SDL.h>` in engine code fails to build.
That is not as airtight as a Cargo crate boundary would have been, but it catches
the mistake at compile time rather than at review time.

It also keeps engine tests fast and headless — they need no window, so they run
in CI without a display server.

**Dependencies, kept deliberately small:**

| Need | Choice | Why |
|---|---|---|
| Window, input, rendering | SDL3 | D-002; system package or `FetchContent` |
| UI | Dear ImGui | vendored source, as it is designed to be |
| PNG | lodepng | one file, encode and decode; swap to libpng only if compression measurably matters |
| ZIP | miniz | one file, deflate plus ZIP reader/writer |
| JSON | nlohmann/json | one header |
| Tests | doctest | one header |

Everything except SDL3 is a vendored single file or header. Resist adding more —
a paint application's dependency list should be boring.

*How these are actually obtained changed at implementation time — see D-014.*

## D-004 — Colour depth: 8-bit RGBA, premultiplied alpha

**Status:** Decided; 8-bit-*only* is **Superseded by D-023** — 8-bit remains the
default, and the type split below is what D-023 relies on
**Affects:** tile format, compositing, file format, SDL blend mode

**Why:** halves memory per tile (256 KB vs 512 KB) and keeps blend maths simple
while the engine is being proven.
**Cost accepted:** visible banding when many low-opacity airbrush passes stack.
**Enforcement:** premultiplied and straight colour are **distinct types**
(`PremulRgba8` / `StraightRgba8`), so the compiler finds every conversion site
when a 16-bit type is added later. See the SDL blend-mode note in D-002.

## D-005 — Canvas storage: sparse 256 × 256 tiles

**Status:** Decided
**Affects:** layers, undo, rendering, file format, texture cache

Tiles are allocated on first paint. A fully transparent tile is an absent map
entry, not a buffer of zeros.

**Why:** a 4000 × 4000 canvas is 256 tiles per layer; an artist working in one
corner allocates a handful. Undo becomes cheap for the same reason (D-006), and
the tile grid maps one-to-one onto the `SDL_Texture` cache.

## D-006 — Undo: copy-on-first-touch tile snapshots

**Status:** Decided
**Affects:** stroke handling, memory

When a stroke first writes to a tile, the tile's previous bytes move into the
stroke's undo record. Later dabs on that tile cost nothing. One record per
stroke, not per dab.

**Why:** matches how strokes behave — few tiles, touched many times. Full-canvas
snapshots are unaffordable; per-dab records are pointless.
**Open sub-question:** memory cap and eviction → D-102.

## D-007 — Rendering: CPU compositing, GPU for display only

**Status:** **Superseded by D-021** — the CPU path below is still the default
and the reference implementation; "the GPU never paints" is what changed
**Affects:** renderer, hardware requirements

Dab rasterisation and layer compositing are plain C++ on the CPU. Composited
dirty tiles are uploaded to `SDL_Texture`s; the GPU draws and scales them.

**Why:** the stated product principle is working well on modest Linux hardware.
The GPU displays finished pixels; it never paints them.
**Not now:** threading the compositor across tiles. Add it when a profiler says
compositing is the bottleneck — a thread pool over a handful of 256 KB tiles can
easily cost more in synchronisation than it saves.

## D-008 — Event-driven main loop, not a game loop

**Status:** Decided
**Affects:** the ImGui integration, battery life, the modest-hardware principle

Dear ImGui rebuilds its UI every frame, and the default SDL example loop spins at
display rate forever. A drawing application that burns a core while the artist
thinks contradicts D-007 and drains laptop batteries.

Block on `SDL_WaitEvent` (or `SDL_WaitEventTimeout`) and redraw only when there
is input, an animation in flight, or a background job reporting progress. Draw
continuously *during* a stroke, idle between them.

**Why this is a decision and not an optimisation:** it is much harder to retrofit
than to start with, because ImGui code written under a spin loop tends to
accumulate per-frame state assumptions.

## D-009 — Licence: MIT

**Status:** **Superseded by D-020** — Sable is MIT today, but a permissive
dependency is no longer a requirement

Add `LICENSE` in the first commit, before any outside contribution arrives.

Every dependency in D-003 is MIT, zlib, or public-domain-equivalent, so the
release is unencumbered. This was a live factor in D-002 — JUCE was rejected on
AGPLv3 alone. Check the licence of anything new before adding it.

## D-010 — Originality boundary

**Status:** Decided
**Affects:** assets, naming, file format

No PaintTool SAI source, icons, brush textures, bundled assets, interface
artwork, or `.sai2` internals. This project began from observation notes on SAI's
visible workflow; those were competitor research for feature planning, never a
specification, and nothing in Sable should be traceable to matching them.

**Practical rule:** if a name, icon, or preset would be recognisable as SAI's,
rename it.

## D-011 — Project file: ZIP container, extension `.sable`

**Status:** Decided
**Affects:** save/load, Milestone 3

Structure in `DATA-MODEL.md`. `format_version` present from the first release so
v1 files stay loadable.

**Why:** a ZIP of PNG tiles plus a JSON manifest is inspectable with `unzip` and
any image viewer, which makes save/load bugs debuggable without writing a parser
first.

## D-012 — Errors: `std::expected` in the engine, exceptions only at the edges

**Status:** Decided
**Affects:** every engine API that can fail

The `engine` target returns `std::expected<T, sbl::Error>` from anything that can
fail — loading, saving, decoding, validating. It throws nothing of its own, and
the paint hot path (dab generation, tile blending, compositing) neither allocates
nor throws.

```cpp
namespace sbl {

enum class ErrorKind { NotFound, Permission, Malformed, UnsupportedVersion, Io };

struct Error {
    ErrorKind kind;
    std::string detail;      // shown to the user, so write it for one
};

[[nodiscard]] std::expected<Document, Error> loadDocument(const std::filesystem::path&);
[[nodiscard]] std::expected<void, Error>     saveDocument(const Document&, const std::filesystem::path&);

}
```

**Why not exceptions throughout:** `[[nodiscard]] std::expected` makes the
failure visible in the signature, so a caller cannot forget the error path
without a compiler warning. For an application whose first principle is never
losing the artist's work, "you cannot accidentally ignore this" is worth more
than the brevity of a `try` block. It also costs no dependency, which a
third-party `Result` type would have.

**Exceptions still exist and are still caught.** `std::filesystem`,
`std::vector`, and third-party code throw regardless of our style. So:

- Prefer the `std::error_code` overloads of standard functions inside the engine
  and convert to `Error` at the point of failure.
- Keep a top-level `catch (...)` in the app around each menu action as a
  backstop. It exists to show a message and keep the process alive, not as the
  primary error path.

**Hard rule:** no path that touches a file, a device, or user input may terminate
the process. That is the path where an artist's unsaved drawing dies.

**If the C++23 floor does not hold** (see D-001), replace `std::expected` with a
throwing API and catch at the menu action. Nothing else in the design changes.

## D-013 — Autosave never overwrites the user's file

**Status:** Decided
**Affects:** recovery, Milestone 4

Recovery data goes to a separate path under the XDG state directory. Restoring is
an explicit user action.

**Why:** this is the one failure mode that destroys an artist's work permanently.
Not negotiable, and not a candidate for simplification.

## D-014 — Dependencies come from `FetchContent`, not a checked-in `third_party/`

**Status:** Decided
**Affects:** D-003's repo layout, first build on a clean machine

D-003 planned a `third_party/` directory holding vendored copies of imgui,
lodepng, miniz, and nlohmann/json. Milestone 1 fetches imgui and lodepng with
CMake's `FetchContent` at configure time instead, pinned by tag. miniz and
nlohmann/json are not fetched at all yet — nothing before the `.sable` writer
at Milestone 3 needs them, and an unused dependency is worse than a missing
one.

`FetchContent_Declare` uses `SOURCE_SUBDIR do-not-configure` for both, which
is the supported way to fetch a repository without running its own
`CMakeLists.txt`. We then compile the one source file we want. That keeps the
"one file, no build system of its own" property D-003 was actually asking for.

**Why:** the repository stays free of several megabytes of other people's
source, the pinned tag is visible in one place, and upgrading is a one-line
diff instead of a re-vendoring commit that buries the real change.

**Alternative rejected:** checking the sources in. It survives a network
outage at configure time and pins by content rather than by tag, which is a
real advantage — take it if offline or reproducible builds ever become a
requirement. Neither is one today, and it costs a review burden on every
dependency bump.

**Cost accepted:** the first configure needs network access. Note it in the
build instructions rather than discovering it in a bug report.

## D-015 — One calibration profile, not one per device

**Status:** Decided
**Affects:** D-002, `TabletProfile::deviceKey`, US-09.6, US-10.4
**Supersedes the per-device claim in:** D-002

Writing the Milestone 2 input layer against SDL 3.4.12 turned up three facts
that contradict what D-002 and `DATA-MODEL.md` assumed:

1. **SDL reports no pen name.** There is no `SDL_GetPenName`. The only
   identifying call is `SDL_GetPenDeviceType`, which returns
   direct / indirect / unknown — and its own documentation says "many
   platforms do not supply this information".
2. **`SDL_PenID` does not survive a replug.** From `SDL_pen.h`: "Unplugging
   the digitizer and reconnecting may cause future input to have a new
   SDL_PenID, as SDL may not know that this is the same hardware."
3. **It is only stable for the life of the process**, so it cannot key
   anything persisted to disk.

D-002 said a stable `SDL_PenID` is "exactly what per-device calibration
profiles need". That is half right: it is a fine key *within one run*, and no
use at all as a durable one. `deviceKey` as specified —
"`SDL_PenID` plus whatever name SDL reports" — cannot be built.

**The decision:** v1 keeps **one** calibration profile, applied to whatever pen
is in use, persisted under the key `tablet`. Per-`SDL_PenID` axis state is
still tracked separately, because that part *is* reliable and is what
assembling an `InputSample` needs.

**Why this is the right trade rather than a shortcut:** almost every artist has
one tablet. A profile that silently resets on replug — which is what keying on
`SDL_PenID` would give — is worse than a single profile that always applies,
because the failure is invisible until the pen feels wrong.

**What US-09.6 becomes:** "settings persist and apply to any connected pen"
rather than "stored per device and reloaded when that device reconnects". The
original is not deliverable on this API.

**Revisit when:** two pens with genuinely different pressure characteristics
need different curves at once. At that point the key has to come from
somewhere other than SDL — reading `libinput` or `/dev/input` device names
directly, which is D-002a's territory and carries D-002a's costs.

## D-016 — Dockable panels: track the ImGui docking branch, pinned to a commit

**Status:** Decided
**Answers:** D-104
**Affects:** the whole panel layout, Milestone 4

D-104 asked whether to track ImGui's docking branch or keep a fixed layout, and
said to decide at M2. It was not decided then, so it was decided here.

**The facts, checked rather than assumed.** Docking is still *not* in ImGui
master. `ImGuiConfigFlags_DockingEnable` does not appear in `imgui.h` at
**v1.92.1** or at **v1.92.9**, the newest release. The docking branch remains
the only way to get it.

**Decision:** track the docking branch, pinned to commit
`b334d19b667958ed970000073644d911fae17e57`. That commit reports version
1.92.9 — the same version as the latest tagged release, so pinning it costs no
staleness against master, only the loss of a version tag.

**Why pin a commit and not the branch name:** `GIT_TAG docking` would make
every clean configure a different build. A commit is reproducible, and moving
it is a one-line diff that shows up in review — which is exactly the argument
D-014 made for pinning tags.

**What this costs:**

- The default layout uses `DockBuilder*`, which lives in `imgui_internal.h` and
  carries no stability promise. It is confined to one function; if upstream
  changes it, the *initial* arrangement breaks and nothing else does.
- Panel layout now persists to `layout.ini` in the preferences directory,
  because a layout the artist arranged and lost would be worse than no docking
  at all. **View → Reset panel layout** puts it back.

**Alternative rejected:** keeping the fixed layout. It works, and it is what
Milestones 1–3 shipped, but PRD §9 promises dockable panels at M4 and the
branch is well maintained. Revisit if the branch is ever abandoned — the fixed
layout is a small diff to return to, since the panels are ordinary windows
either way.

## D-017 — Undo memory: a fixed budget, oldest dropped first, and say so

**Status:** Decided
**Answers:** D-102
**Affects:** `UndoStack`, PRD §12

**Default budget 256 MB**, adjustable from 16 MB to 2 GB in the interface
preferences and persisted. When the history exceeds it, the *oldest* records
are dropped until it fits.

**Why a byte budget rather than a step count:** steps are wildly uneven. Fifty
pencil strokes in one corner cost a few megabytes; fifty full-canvas fills on a
4000 × 4000 document cost gigabytes. A step count bounds the wrong quantity —
it is the memory that swaps a laptop, not the number of entries.

**Why 256 MB:** a typical stroke touches a handful of 256 KiB tiles, so the
default holds hundreds of them — far more than the 50 steps US-04.3 requires.
Ordinary work never reaches the cap at all, which is the point: the budget
exists for the pathological case, not the normal one.

**Undo is dropped before redo.** The artist has already moved past undo
history; losing a redo they are actively stepping through is the more
surprising failure.

**One record always survives.** A stack that evicts to nothing turns Ctrl+Z
into a no-op exactly when a large operation has made it most valuable.

**The policy is visible**, which D-102 asked for specifically: once anything
has been dropped the status bar says how many steps and what the limit is, and
the Edit menu shows current usage against the budget. Silently shortening
someone's history is the part that feels like a bug.

**Rejected:** compressing snapshots (spends CPU on the paint path to defer a
limit that ordinary work never reaches) and spilling to disk (turns Ctrl+Z into
an I/O operation, and D-013 already puts the thing worth persisting — recovery
— somewhere safer).

## D-018 — Packaging: AppImage first, from a normal FHS install

**Status:** Decided
**Answers:** D-106
**Affects:** release, `packaging/`

`packaging/build-appimage.sh` produces a single self-contained
`Sable-x86_64.AppImage`. The build measures **3.7 MB**, because ImGui, lodepng,
miniz and nlohmann/json are compiled in and only SDL3 is bundled.

**The AppImage is built on top of `cmake --install`, not beside it.** The
install rules put the binary, `.desktop`, icon, AppStream metainfo, MIME
definition and licence in ordinary FHS locations, and the AppImage script
simply installs into an `AppDir`. A distro package therefore needs no separate
recipe, and the two cannot drift apart.

**AppRun prefers the host's SDL3** when present. A bundled SDL cannot know
about the host's graphics drivers, and a tablet that works natively must not
stop working inside an AppImage.

**The icon is generated by Sable itself** (`packaging/make-icon.cpp`), so it is
original by construction (D-010) and reproducible from source rather than being
a binary nobody can regenerate.

**Not done, and blocking a public release:** the AppStream metadata has no
homepage or bug-tracker URL, because the project is not hosted anywhere yet.
`appstreamcli validate` reports `url-homepage-missing` and will keep failing
until it is. The field is left absent rather than filled with a placeholder — a
URL that does not resolve is worse than an honestly missing one. The metainfo
file carries a comment saying exactly this.

**Flatpak next, not instead.** It is the better answer for sandboxed desktops
and software centres, but it needs a manifest, a runtime choice, and portal
handling for the file dialogs — and D-002 chose SDL's *native* dialogs, which
behave differently under a portal. That is a piece of work, not a line of
YAML.

## D-019 — The self-imposed constraints are released; the target is SAI 2

**Status:** Decided
**Affects:** PRD §3, PRD §4, and every entry named below
**Superseded by this entry:** D-003's engine and dependency rules (see D-022),
D-004's 8-bit-only stance (D-023), D-007 (D-021), D-009 (D-020), PRD §3's
non-goals and PRD §4's fourth principle

Sable's constraints were chosen for a different product. The brief was a small
Linux sketching tool that starts fast on an old laptop, and D-003, D-004, D-007
and PRD §3 all follow from it. Each was right for that brief.

The brief has changed. Sable is now meant to be the application an artist
leaves PaintTool SAI 2 for, and that is a different bar. SAI 2 runs on Windows,
paints in 16 bits per channel, uses the GPU when asked, and reads and writes
PSD. A Linux-only, 8-bit, CPU-only program that cannot open the artist's
existing files does not compete with it however good its brush feels — the
artist does not get as far as the brush.

**What is released, and where each is answered:**

| Constraint | Came from | Now |
|---|---|---|
| Windows and macOS are not targets | PRD §3 | Goals. The build already produces both; this entry stops the document contradicting the code |
| The GPU never paints | D-007, PRD §3 | An opt-in GPU compositing backend — D-021 |
| The engine links no SDL | D-003 | Released — D-022 |
| Resist adding dependencies | D-003 | Released. The licence rule in D-020 is the filter now |
| 8-bit only | D-004 | Released — D-023 |
| No format compatibility | PRD §3 | PSD, ORA and KRA import and export are goals. `.sable` stays the working format (D-011) |

**Principle 4 restated, honestly.** "Modest hardware is the target, not the
fallback" is kept, but it no longer means the weakest machine sets the ceiling
for everyone. What preserves it is that each capability added under this entry
is a **runtime** switch and never a build-time one: CPU compositing stays the
default and the reference (D-021), 8-bit stays the default depth (D-023). The
student on the old laptop runs the same binary as everyone else and leaves the
switches alone. So the principle reads: *Sable must stay usable on modest
hardware at its default settings.* Test on the old laptop with the defaults;
test the switches on the machine that has the hardware.

**Cost accepted, and it is the real one:** the constraints were also a scope
fence. PRD §17 lists "scope creep toward Krita" as a risk and names the §3
non-goals as the response — that argument-ender is now gone, and what is left
in its place is milestone ordering and D-020's licence rule, which are weaker
fences. Expect to have to say "not yet" in review where the document used to
say "never", and expect that to be argued with.

**Alternative rejected:** keep the constraints and compete on feel alone. It is
a coherent product, and it is the product Sable already is. It has no answer
for an artist with three years of PSD files and a Windows desktop.

## D-020 — Any open source licence; `LICENSE` follows the code, in the same commit

**Status:** Decided
**Affects:** every dependency choice, `LICENSE`, release
**Supersedes:** D-009

**The rule, in full:** any open source licence may be used. If copyleft code
lands in the tree, Sable's `LICENSE` changes to match it, in the same commit.

**Why the same commit, and not "before the release":** GPL code inside a binary
distributed under MIT is not untidiness, it is copyright infringement. The MIT
text grants recipients sublicensing and proprietary-relicensing rights that the
GPL never gave us to grant, so the moment the code is in the tree the LICENSE
file is making a claim we have no right to make. The commit that creates the
false claim is the commit that has to fix it — anything later means every clone
taken in between carries it.

**And enforcement is real, not theoretical.** Copyleft violations have been
litigated to judgment and settled repeatedly; the standard remedy is an
injunction against further distribution, which arrives at precisely the moment
a project has enough users to be noticed. Treat a licence mismatch as a defect
of the same severity as data loss, because it can end the project just as
finally.

**What this releases:** D-009 required every dependency to be MIT, zlib or
public-domain-equivalent, and D-002 rejected JUCE on AGPLv3 alone. That
automatic filter is gone. If the best answer to a problem is GPL — a
colour-management library, a PSD reader, a brush engine — take it, and change
`LICENSE` with it.

**Cost accepted, and it is not small:** relicensing is one-way in practice.
Once Sable ships copyleft it cannot come back without every contributor's
agreement, and downstream users who could have embedded an MIT Sable no longer
can. So check the licence *before* adding the dependency and price "this pulls
the whole project to GPL" into the trade as a real cost. Rejecting JUCE would
still be a reasonable call today; the difference is that it would be a call,
made on merits and price, rather than an automatic no.

**Forbidden regardless of any licence, and not a trade:**

- `.sai2` support, in either direction.
- Any PaintTool SAI source, asset, icon, brush texture or interface artwork.

Neither is an open source licensing question at all. Those things are
proprietary — no licence we adopt makes them available to us — and SAI's EULA
almost certainly forbids the reverse engineering that reading `.sai2` would
require. D-010 stands unchanged and this entry does not touch it: studying
SAI's publicly visible workflow is still fine, and always was.

## D-021 — Compositing: an opt-in GPU backend, CPU stays the reference

**Status:** Decided
**Affects:** renderer, D-002's rendering-backend note, hardware requirements
**Supersedes:** D-007

D-007 said dab rasterisation and layer compositing are plain CPU C++ and the
GPU never paints. A GPU compositing backend may now be added. It is selected at
runtime and it defaults to off.

**The CPU path stays, and stays the reference implementation.** It is what the
headless engine tests composite with, it is what defines the correct answer
when the two disagree, and it is what runs when there is no usable GPU or the
driver is one of the ones that is wrong. A GPU backend that is merely faster is
worth having; a GPU backend that became the only implementation would put
D-019's modest-hardware promise behind a driver.

**Why the runtime switch is the load-bearing part:** a build-time option means
two binaries, and the bug report you cannot reproduce because the reporter has
the other one. One binary with a toggle also gives a free bisect — every
"the colours are wrong" report is one switch away from saying which side is
wrong.

**Cost accepted:** two implementations of the same blend maths, which will
drift. The mitigation is that the CPU path is what the blend-mode tests assert
against, and any GPU path is checked against it pixel-for-pixel rather than by
eye. A one-level fringe is invisible on screen and obvious in a diff, which is
the wrong way round for a mistake to behave.

**Alternative rejected:** GPU by default with a CPU fallback. The default is
the path that gets tested, and the fallback is the path that quietly rots until
someone without a GPU tries to draw with it.

## D-022 — The engine may link SDL3; the boundary becomes a rule, not a link error

**Status:** Decided
**Affects:** repo layout, D-003
**Supersedes:** D-003's "links NO SDL" rule and its "resist adding more"
dependency rule

D-003 enforced the engine/app split by simply not linking SDL into `engine`, so
a stray `#include <SDL3/SDL.h>` failed to build. That was cheap and it worked.
It also blocks what D-021 needs: `SDL_GPU`, and SDL's threading and timing
primitives, all of which belong under the compositor rather than beside it.

`engine` may link SDL3. It still must not link Dear ImGui — that half of the
boundary is the one that matters and it costs nothing to keep, because ImGui is
the interface and interface code inside the engine is what makes an engine
impossible to test.

**What replaces the link failure:** the engine tests. They are headless, they
link `engine` with no window, no renderer and no display server, and they must
stay that way in CI. Engine code that needs a window or an event loop fails
there instead of at the link step. The rule, stated so a reviewer can apply it:
**the engine may call SDL, but nothing under `engine/` may create a window,
open an event queue, or read an input device.** Those stay in `app/`.

**Cost accepted:** this is a weaker fence, and it will be climbed. A compile
error is not arguable; a rule has to be noticed by a human at review time, and
sooner or later it will not be. Accepted because the alternative is a GPU
backend living in `app/` and reaching back into engine internals to composite,
which does not remove the boundary problem — it moves it somewhere worse.

## D-023 — Colour depth: 16-bit per channel as an option, 8-bit still the default

**Status:** Decided
**Affects:** tile format, compositing, `.sable` manifest, memory, undo budget
**Supersedes:** D-004's 8-bit-only stance ("Decided for v1")

D-004 chose 8-bit premultiplied RGBA and accepted visible banding when many
low-opacity airbrush passes stack. That banding is exactly what an artist
arriving from SAI 2 — which paints in 16 bits — will notice first, and it
appears in the soft rendering work Sable is meant to be good at. A 16-bit tile
format may now be added, chosen per document. 8-bit remains the default.

**D-004's enforcement is what makes this affordable, and it stays.**
`PremulRgba8` and `StraightRgba8` are distinct types precisely so the compiler
finds every conversion site when a wider type arrives. D-004 said "when a
16-bit type is added later"; this is later. Add the 16-bit types the same way,
and do not reach for a template over the channel type until there are two
working implementations to generalise from — one of them is not a pattern.

**Cost accepted:** double the memory per tile, 512 KB against 256 KB, and
double the cost of every undo snapshot against D-017's budget, which means a
16-bit document holds roughly half the history at the same setting. That is
exactly why the default does not change: an artist who does not need 16 bits
should not pay for it, and on D-019's modest hardware the difference is between
comfortable and swapping.

### What #21 built, and the two things it deliberately did not

**Compositing stays 8-bit at both depths.** The screen is 8-bit and so is PNG
export, so a 16-bit tile has to be narrowed somewhere. Narrowing per pixel at
the blend (`compositeLevel` in `engine/src/io.cpp`) means a 16-bit document's
smooth ramp arrives as the *correct* 8-bit ramp rather than the plateaued one
8-bit storage would have held — which is the whole of the visible benefit, and
it is measured rather than claimed. Compositing wide would additionally help a
STACK of semi-transparent 16-bit layers, whose intermediate results are
currently rounded twice. That is a second change and is not this one.

**The GPU backend declines a 16-bit document.** Its arena, both compute
shaders and every transfer are 8-bit RGBA (D-025), so `applyDab` and
`compositeRect` hand a 16-bit document to `cpuBackend()` and the application
unticks the View-menu toggle with a notice. Declining rather than converting:
painting a document at half its depth without saying so is worse than not
using the GPU. `tests/differential.cpp` keeps its ±1 colour, 0 alpha
tolerances untouched.

**The hook D-004 left worked exactly as written.** Adding the wider types
turned every conversion site into a compile error — forty-odd across ten files
— rather than a silent truncation. The identity that made the change small is
`narrow(widen(c)) == c` for every byte, which let the cold paths (fills, the
transform, text, importers) be written once instead of twice; only the dab
loop, `over`, `blendOver` and `mergeLayerDown` have a second implementation,
as this entry asked. Two working implementations now exist, so the template
this entry declined is finally a decision someone could make on evidence — and
it should still wait for a third reason.

## D-024 — File formats: one registry, extension first and content second

**Status:** Decided
**Affects:** `engine/include/sbl/format.hpp`, `engine/src/format.cpp`, the File menu

Every reader and writer is a `Format` in one table
(`builtinFormats()` in `engine/src/format.cpp`). The app never names a format:
`importDocument()` and `exportDocument()` dispatch, and the file dialogs build
their filters from `dialogFilters()`. Adding PSD, ORA or KRA is one entry plus
the reader itself.

**Extension first, content second.** `.sable`, `.ora` and `.kra` are all ZIP
containers, so an extension is a hint, not an identification. A format may
supply a `sniff`; when the extension matches but the content disagrees, the
registry asks every format that can answer, and only then falls back to the
reader the name promised — so the artist gets a specific complaint about the
file rather than a generic one about its name.

**`Document::path` belongs to the registry, not to importers.** `importDocument()`
sets it for the native project format and clears it for everything else. This is
not tidiness: Ctrl+S writes a `.sable` archive straight to `Document::path`, so
an importer that left `painting.psd` there would destroy the artist's PSD on the
next save. Closing the trap once, centrally, is the only version of this that
stays closed as importers are added by different hands.

**Rejected:** self-registering static initialisers (an importer that fails to
link is then silently absent, and static-init order across translation units is
not worth the convenience), and `std::function` members (every reader is a free
function; plain function pointers keep the table allocation-free).

## D-025 — The GPU backend is SDL_GPU compute, host tiles stay the storage of record

**Status:** Decided
**Affects:** `engine/src/gpu.cpp`, `engine/src/shaders/`, `Tile`, D-002's
"do not reach for SDL_GPU" note
**Implements:** D-021, D-022

### SDL_GPU, not SDL_Renderer's render-to-texture

SDL_Renderer can already draw into a texture, and a dab is a textured quad, so
half of #13 would fit. Compositing does not: `blendOver` is the W3C separable
set — Overlay, Colour Dodge, Soft Light — and `SDL_ComposeCustomBlendMode` only
offers factor-and-operation combinations. Multiply is expressible; Soft Light
is not expressible at all. A renderer-based compositor would therefore have to
be *two* compositors, GPU for the modes it can do and CPU for the rest, which
is the divergence #13 exists to close rather than a fix for it.

SDL_GPU costs a shader toolchain, which SDL_Renderer would not: SPIR-V is
generated by `glslc` and **committed** as `engine/src/shaders/*.spv.inc`.
Committed rather than built, because SABLE_GPU turns on wherever SDL3 is found
and requiring a shader compiler as well would stop the application CI job —
SDL3, no glslc — from building the backend at all.

**Cost accepted: SPIR-V only.** `SDL_CreateGPUDevice` is asked for
`SDL_GPU_SHADERFORMAT_SPIRV`, so the backend appears on Vulkan and is simply
absent on a Direct3D or Metal machine. That is the same "no usable GPU" answer
those machines would get from a driver blocklist, and D-021 already requires
that answer to be a working paint program rather than an error. DXIL and
metallib are a build-step change, not a design change.

### Host tiles stay the storage of record; VRAM is a cache

The device copy is authoritative only between a dab and the next sync point.
Everywhere else the host `Tile` is the truth, which is what lets undo, save,
`cloneDocument` and the whole existing test suite keep working untouched.

**Undo snapshots stay on the host** (#12 asked for this decision explicitly).
`TileSnapshot::before` is still a host `Tile` and `UndoStack::memoryBytes()`
still counts exactly what it always counted, so the 256 MB the status bar shows
the artist keeps meaning host RAM. Snapshots on the device would have put
history and art in competition for the scarcer memory, and made the number in
the status bar a lie about which memory it was. The price is one tile download
per tile per stroke, on first touch — not per dab. The status bar gained a
second, separate figure for VRAM, because one number covering two memories is
how the first one stops being true.

### `Tile::stamp()` replaces the call-site audit

#12 asked for an audit of the fourteen places that take a raw pixel pointer.
An audit is a list that rots. Instead, every non-const handle to a tile's
pixels — `pixels()`, `setPixel`, `fill`, construction — stamps the tile from a
process-wide counter. A cache entry is valid only while the tile is at the same
address *and* carries the same stamp, so no host write can be missed: obtaining
the means to write is what moves the stamp. Undo needs no hook, the file
loader needs no hook, and a future operation cannot forget one.

The counter is a plain `std::uint64_t`, not an atomic: tiles belong to the
thread that owns the document, which is the discipline `cloneDocument` and the
autosave hand-off already depend on. Measured cost of the increment on the
hottest path — a four-megapixel flood fill, one stamp per pixel — is nil.

**What the stamp does not cover:** host code that *reads* pixels while the
device copy is ahead. Those callers must ask first, which is why `swapRecord`,
`deleteLayer`, `duplicateLayer` and `exportDocument` each call
`PaintBackend::readback` and say why. That is four sites, not fourteen, and
they are the four that move or copy tiles behind the backend's back.

**Rejected:** invalidating the whole cache from a new `PaintBackend::invalidate`
hook called by undo and the layer operations. It is the same audit with an
extra virtual function, and the first caller to forget it paints the wrong
picture silently.

### How far the two are allowed to drift, measured

The shader is `canvas.cpp` ported line for line, integers kept as integers:
`mul255`, `over`, `unpremultiply` and the `+ 0.5` roundings are the same
arithmetic, and float appears only inside `blendChannel`, where the CPU uses it
too. That was worth the effort — the alternative, a compositor written in 0..1
floats, disagrees with `mul255` at nearly every level.

Measured on radv, `engine_tests`:

| what | divergence |
|---|---|
| one layer, each of the 13 blend modes | ≤ 1 level |
| 8 layers, folders, clipping, opacity | ≤ 1 level, 9 pixels in 262'144 |
| 12 layers of dodge/burn/soft-light | ≤ 7 levels, 125 pixels per million |
| a stroke: pencil, airbrush, opaque | ≤ 1 level, 1–8 pixels per stroke |
| erase and preserve-opacity | 0 |
| bucket fill | 0 |
| #14's 32 scenarios, whole harness | ≤ 1 level, 5 pixels in 570'368 |

The last row is the one that counts: `kColourTolerance` is 1 and
`kAlphaTolerance` is 0 in `tests/differential.cpp`, and neither was touched to
make this pass.

**The deep-stack number is accumulation, not a wrong formula.** Each blend
level unpremultiplies to 8-bit, blends in float and rounds back, so a one-level
difference at the bottom is an input to the next level — and Colour Dodge and
Colour Burn amplify their inputs by construction. The rate of disagreement is
identical at four times the canvas area, which is what says it is drift and not
a tile served from the wrong slot; that is what the test asserts, rather than a
maximum that would only be measuring how many pixels were sampled.

Dabs cannot be exact and are not claimed to be: the CPU measures the distance
from a dab's centre in `double` and the shader in `float`, so a pixel whose
coverage lands within an ULP of a rounding boundary tips the other way. That is
one fringe pixel of an anti-aliased edge.

### One submission point, because a download cannot see a queue

Uploads and dabs are both queued on the host and only reach the device when a
command buffer is submitted. Everything that reads the arena or dispatches into
it therefore goes through `submitPending()` first. This is not an optimisation
detail: the first version let the undo snapshot at the start of a second stroke
download a tile whose own upload was still sitting in the queue, so the slot
read back in whatever state the *previous document* had left it, and that
document's pixels ended up underneath the stroke.

It was invisible in isolation — an untouched slot reads as zeroes, which is
exactly what an empty tile should give — and only appeared once #14's harness
ran thirty scenarios through one backend. Two lessons worth keeping: a device
cache has to be tested across documents, not within one; and a harness that
reuses one backend for a long sequence finds what a per-test fixture cannot.

### The ceiling, stated

The compositor replays a flattened layer program in the shader: at most 192 ops
and 4 levels of folder nesting. Beyond either, `compositeRect` composites on
the CPU instead — the same answer, slower. The arena holds 512 tiles (128 MiB)
and evicts least-recently-used clean ones; a stroke that fills it entirely
finishes on the CPU.

---

## D-025 — Rulers sit beside the stabilizer, and symmetry one step below it

**Status:** Decided
**Affects:** `engine/include/sbl/input.hpp`, `engine/src/input.cpp`,
`paintWith` in `app/src/main.cpp`, `.sable` format version 2

A perspective ruler is a function from a proposed sample to a constrained one —
the same shape as `Stabilizer::apply` — so it goes in the same place in the
pipeline and nothing downstream of the interpolator changes.

**The order is stabilizer, then perspective.** Smoothing a point that has
already been projected drags it back off its own guide line, so the two would
fight; projecting the smoothed point cannot. That ordering is what the
"stabilised stroke under symmetry" test pins down.

**Symmetry is not in that chain.** It does not move a sample, it multiplies the
dabs a sample produces, so it applies after the interpolator has decided where
the dabs go, and every image is stamped into the stroke's existing
`UndoRecord`. One stroke stays one undo step however many copies it painted.

**Vanishing points are document state; the rulers are not.** A scene's horizon
belongs to the drawing and comes back with the file (`vanishing_points`, format
version 2 — optional, so v1 files load unchanged). Whether a ruler is switched
on, and where the symmetry axes sit, are preferences.

**Alternative rejected:** running a whole mirrored `Stroke` per axis. Each copy
would carry its own spacing carry-over and its own `UndoRecord`, so undo would
need N records collapsed into one, and the spacing walks would drift apart at
speed — two problems bought in exchange for reusing `paintSample`.

---

## D-026 — Text: rasterised into an ordinary layer, with the words kept beside it

**Status:** Decided
**Affects:** `engine/include/sbl/text.hpp`, `engine/src/text.cpp`,
`app/src/text_tool.cpp`, `LayerKind`, `.sable` format version 3

A text layer holds normal tiles, rasterised from a `TextContent` that is stored
next to them. The pixels are what renders — everywhere, screen and export alike.
The words are what edits.

**Why not a layer the compositor renders on demand:** `sbl::compositeRect` is
the one compositor both the screen and the export go through, and #1 is what
happened the last time there were two. Teaching it to rasterise glyphs would
mean shaping outlines inside the hot path of every tile upload, with a glyph
cache to make it bearable — and a document whose appearance depends on a font
file still being installed. Rasterising on edit costs one pass over a few tiles
per keystroke and needs no compositor change at all.

**Why not rasterise-on-commit, with no text kept:** that is a text tool you can
use once. The whole value of the feature is the second visit, when a caption
needs a word changed at the size and position it was already placed at.

**What it costs the artist, stated plainly:** re-editing redraws from the font
named in the file. If that font is gone, the drawing is unchanged — it is
pixels — but resuming the text substitutes another face and says so, and
finishing that edit redraws in the substitute. Type set on a machine with the
font is never silently altered on a machine without it; only editing it is.

**Why `LayerKind::Text` and not just the optional:** `applyDab`, `bucketFill`,
`fillSelection`, `transformRegion` and `mergeLayerDown` already refuse anything
that is not `Raster`, so a text layer is protected from paint that the next
keystroke would throw away without one line added to any of them. `applyProps`
keeps the kind and the text in step, which is what makes "rasterise text" — give
up the words, keep the picture — a property change, and so undoable.

**Rasteriser: stb_truetype, fetched from upstream.** Public domain, one header,
and the same one Dear ImGui bundles. Not taken from inside ImGui's tree because
D-022 lets the engine link SDL3 but not ImGui, and the engine has to build with
`SABLE_BUILD_APP=OFF`. Note stb_truetype's own warning that it does no range
checking on font files; Sable only ever loads a font the artist chose or one
already installed on the machine.

**Input: SDL3, exactly as D-002 planned.** `SDL_StartTextInput`,
`SDL_EVENT_TEXT_INPUT`, `SDL_EVENT_TEXT_EDITING`, `SDL_SetTextInputArea` — no
`ImGui::InputText` anywhere near the canvas. The preedit is drawn into the layer
so the artist sees real glyphs at the real size while composing; only the caret
and the composition underline are overlays.

**Alternative rejected:** word wrapping inside a text box. Explicit line breaks
only, for now: a wrap needs a box, a box needs handles, and handles need a
transform mode of their own.

---

## D-027 — PSD layer masks are multiplied into the pixels on import

**Status:** **Superseded by D-031** — `Layer` grows a mask, which is the
alternative this entry rejected and named; the import no longer bakes
**Affects:** `engine/src/psd.cpp`, `tests/data/make_psd_fixtures.py`, #35

A PSD layer mask is read and multiplied into the layer's alpha as the pixels are
written. Sable's `Layer` grows no mask field, the `.sable` format does not
change, and the compositor is untouched.

**Why:** the bug was that the import showed content the file hides — the pixels
arrived and the mask that hid half of them did not. A mask is exactly an alpha
multiplier, so folding it in at the boundary makes the import faithful with no
new document state, no format version, and no second compositor path to keep in
step with `compositeRect` on the GPU.

**What it costs the artist, stated plainly:** the mask is no longer editable
after the import. What was a mask is now the shape of the layer's alpha, and
re-exporting to PSD writes it that way. That is a lost *capability*; dropping
the mask was a lost *drawing*, which is the worse of the two.

**Alternative rejected — `Layer` grows a mask.** The substantial fix, and the
one that unblocks masks as a Sable feature. It is not this issue: it needs a
format version, `cloneDocument`, PSD export, and — the part that makes it large
rather than merely long — the GPU compositor, or the two backends diverge and
#1 comes back through the other door. Worth doing on its own terms, as a
feature, not as the fix for a file that opens wrong today.

**Alternative rejected — detect masks and warn.** The interim the issue offered,
and what #40 shipped first. It leaves the drawing wrong and asks the artist to
know what to do about it. Baking is not harder and leaves nothing to explain, so
that warning is replaced rather than kept: the file now looks right, and what
the artist is told instead is that the masks are no longer editable — once for
the file, because a PSD from real work can carry forty of them.

**Not covered:** a mask on a *group*, which would have to apply to the folder's
composited result and cannot be baked into any one child. Those are still
dropped, and #40's channel says so per group, naming it. Mask density and
feathering are likewise ignored — the mask is used at the coverage the file
stores.

## D-028 — Linework: curves rasterised into an ordinary layer, kept beside it

**Status:** Decided
**Affects:** `LayerKind`, `engine/include/sbl/linework.hpp`,
`engine/src/linework.cpp`, `app/src/linework_tool.cpp`, `.sable` format
version 5

A linework layer holds normal tiles, rasterised from a `LineworkContent` stored
next to them. D-026's bargain, unchanged, because the shape of the problem is
the same one: the pixels are what renders, everywhere; the curves are what
edits.

**Why not a layer the compositor draws on demand:** `sbl::compositeRect` is the
one compositor the screen and the export both go through, and #1 is what
happened the last time there were two. Curves in the compositor would mean
tessellating splines inside the hot path of every tile upload — and, worse,
teaching the **GPU** compositor to do it identically, or the two backends
disagree on any document with a line in it. Rasterising on edit costs one pass
over a few tiles per drag and needs no compositor change at all. The test for
this is the blunt one: turning a finished linework layer into a plain raster
layer changes not one pixel.

**Curve: centripetal Catmull-Rom, through the control points.** Interpolating,
so the artist drags the point they can see and the line goes there — no
off-curve handles to store, to draw, or to explain. Centripetal rather than
uniform because pen input is never evenly spaced, and uniform Catmull-Rom
answers uneven spacing with a loop: a curve that visibly leaves the points it
was given.

**Coverage accumulated with max, composited once per stroke.** A curve that
doubles back over itself, or simply slows down, must not come out darker there.
That is the difference between a drawn line and a painted one, and the reason
this is not `applyDab` in a loop.

**Pressure is per control point and stays editable.** It is the artist's own
record of how hard they pressed, so it travels with the point, and it is
interpolated linearly along a segment rather than splined — a spline through
pressure can overshoot past 1 between two points that never did, which reads as
a line getting thicker where nobody pressed harder.

**Why `LayerKind::Linework` and not just the optional:** `applyDab`,
`bucketFill`, `fillSelection`, `transformRegion` and `mergeLayerDown` already
refuse anything that is not `Raster`, so a linework layer is protected from
paint the next redraw would wipe without one line added to any of them.
`applyProps` keeps the kind and the curves in step, which is what makes
"rasterise linework" — give up the curves, keep the line — a property change,
and so undoable.

**What it costs the artist, stated plainly:** the curves are Sable's own. PSD,
ORA and KRA have nowhere to put them, so a linework layer exported to any of
those is the finished line and nothing more. That is the same trade text
already makes, and the `.sable` file keeps the editable version.

**Alternative rejected:** Bezier handles. Two extra points per control point to
store, to hit-test, to draw and to teach, for a curve an interpolating spline
already produces. If a use turns up that the spline genuinely cannot express,
the format version is the hook — the same one this used.

---

## D-029 — Linework editing: one selection model, one transform, one flag

**Status:** Decided
**Affects:** `engine/include/sbl/linework.hpp`, `engine/src/linework.cpp`,
`app/src/linework_tool.cpp`, `LineStroke`, #51

D-028 stands unchanged; this is what was built on top of it, and the four
choices it needed.

**The stabiliser sits in the same place it does for paint.** `Stabilizer` is a
pure sample-to-sample function, so a freehand curve runs its samples through one
before deciding where to put a control point — the same class, the same 0..3
levels, the same `finish()` at pen-up so the line ends where the pen lifted. A
level of its own rather than the brush's: a line wants more smoothing than a
sketch, and one shared number would have the artist resetting it on every
switch.

**`appendFreehand` is in the engine, not the tool.** Smoothing and point spacing
only mean anything together — the stabiliser removes the wobble and the spacing
decides how many handles the artist is left holding — and the pair is the thing
worth a test. Split across a header and a window, it could only be tested by a
window.

**Whole-stroke editing reuses `Transform` from sbl/paint.hpp.** A second
transform type would be a second definition of what an angle means, and the day
they drift the artist finds out by turning a selection the wrong way. The
selection itself is a list of stroke indices held by the TOOL, not the document:
it is not part of the drawing, it does not survive a save, and it must not cost
an undo step. `transformStrokes` scales the stroke width with the geometry —
a curve blown up to twice the size that kept its 4 px line is a different
drawing, not the same one enlarged.

**`closed` is a flag, not a repeated end point.** A duplicate would give the
artist two handles on one corner and leave the join the one place the spline is
not smooth. The rasteriser needed nothing: coverage is already accumulated with
max and composited once per stroke (D-028), which is exactly the artefact a
closed curve would otherwise show as a dark dot where the ends meet.

**No format version bump for `closed`.** It is one optional key in the manifest,
and an older reader that ignores it still sees the closed shape, because the
tiles are what renders. Bumping would lock every ordinary painting out of an
older Sable for a flag it is not using — the same reasoning that keeps an 8-bit
document declaring version 5.

**What was left undone, stated plainly:** scale and rotate are panel controls
with an Apply button, not on-canvas handles — the transform tool's handle
machinery is built around a pixel selection and reusing it is its own change.
There is no marquee for selecting several strokes at once; Shift-click adds one
at a time. And nothing auto-closes a curve whose ends meet: closing is an
explicit toggle on a selected stroke.
---

## D-030 — Gradients: premultiplied interpolation, ordered dither at 8 bits, preview through the undo stack

**Status:** Decided
**Affects:** `sbl::Gradient` and `gradientFill` in `engine/include/sbl/paint.hpp`,
`engine/src/paint.cpp`, the gradient tool in `app/src/main.cpp` (#49)

**Interpolated PREMULTIPLIED, and this is the whole feature.** The two ends are
widened, premultiplied once, and every pixel between them is a lerp of those
premultiplied values. Straight alpha is the version that looks right until
somebody drags a foreground-to-transparent gradient: the far end is transparent
*black*, so a straight-alpha ramp pulls the colour channels toward zero
alongside the alpha and the whole fade comes out with a grey haze down it.
D-004 keeps the two spaces apart as types, and `transformRegion` already made
this same argument for bilinear sampling. Two tests hold it — one on a
transparent document checking the colour stays the foreground's exactly, one
over white checking the red channel never dips.

**Composited `over`, not written through.** A gradient fades into the art
beneath it rather than punching a hole through it, which is what makes
foreground-to-transparent the useful half of the feature and not just a slow
eraser. Filling the layer instead would make the faded end erase.

**Dithered on 8-bit documents, not on 16-bit ones.** A ramp across a wide canvas
has far more pixels than it has levels: 256 levels over 2000 px is a visible
band every eight pixels, and the artist's next move is to blur it. An 8 x 8
ordered (Bayer) matrix, amplitude under half of one 8-bit step, breaks the band
edges into a texture that disappears at any zoom an artist works at.

- *Under half a step* is the correctness condition, and the reason it is safe to
  apply to every pixel including the flat ends: a value already exactly
  representable at 8 bits rounds back to itself from every cell of the matrix.
  A dither that reached a whole step would speckle solid colour, which is noise
  added where there was no banding to remove. Tested both ways.
- *One offset for all four channels*, never four independent ones: nudging a
  colour channel up while its alpha goes down produces a premultiplied value
  with colour outside its own alpha, which is not a colour.
- *Ordered rather than error diffusion.* Diffusion makes each pixel depend on
  the one before it — serial, and impossible to compute for one tile without its
  neighbours, which is exactly what a GPU implementation of this would need.
- *16-bit does not get it* by default: 65536 levels over the same span is under
  a level of error per pixel, and the artist who paid double the memory for
  smooth ramps should not have noise added to them.
- The artist can turn it off. Tests pin the exact ramp with it off.

**A non-pure `PaintBackend::gradientFill`.** Every other writer on that
interface is pure virtual; this one has a body — `readback`, then the host
implementation. That body is what `GpuBackend` writes by hand for `fillSelection`
and `transformRegion` anyway, so making it pure would buy one more copy of a
delegation in three files (the GPU backend and two test fakes) and nothing else.
A GPU override stays a pure optimisation, and forgetting to write one is a slow
gradient rather than a missing tool.

**The live preview goes through the undo stack, not beside it.** Each motion
event takes the previous preview back off the stack — which restores exactly the
pixels the next one must be computed from — and pushes the new one, which clears
the redo entry the undo just made. The drag therefore ends with the finished
gradient already on the stack as one step, and there is nothing to commit on
release. **Alternative rejected:** a copy of the layer held beside the document
for the duration of the drag. That is a second copy of the pixels that has to be
kept in step with undo, with autosave and with the texture cache, and it is
wrong in a way that survives a crash — the stack version is a real finished edit
at every instant.

**Not done, and deliberately:** angle/reflected/diamond shapes, multi-stop
ramps, and an editable gradient that can be re-dragged after the fact. A
gradient here is pixels once it lands, the same bargain every fill in Sable
makes. The issue asked for what covers the overwhelming majority of use.
---

## D-031 — Layer masks: sparse tiles of ordinary grey, coverage in the red channel

**Status:** Decided
**Affects:** `Layer`, `LayerProps`, `TileSnapshot`, `compositeLevel` and
`pickLevel` in `engine/src/io.cpp`, `engine/src/gpu.cpp`,
`engine/src/shaders/composite.comp`, `engine/src/psd.cpp`,
`engine/src/openraster.cpp`, `.sable` format version 7
**Supersedes:** D-027

A `Layer` grows `std::optional<LayerMask>`: a `TileMap` of ordinary 8-bit tiles,
a `bool enabled`, and a `uint8_t outside` giving the coverage where no tile has
been allocated. Coverage is the RED channel of an opaque grey pixel.

D-027 baked PSD masks into the alpha because there was nowhere to put them, and
named this as the alternative: "the substantial fix, and the one that unblocks
masks as a Sable feature… worth doing on its own terms, as a feature". #48 is
that feature, so the bake goes.

**Why a mask tile is an ordinary `Tile` and not a byte plane.** A byte plane is
four times smaller and needs its own everything: its own paint path, its own
undo snapshot, its own PNG codec, its own GPU upload, its own byte accounting.
Reusing `Tile` makes "paint into the mask with the ordinary brush tools" one
flag on `PaintTarget` — every preset, every pressure curve, the stabilizer, the
rulers and the whole `UndoRecord` work on a mask because they cannot tell. The
cost is stated plainly: **a mask tile is 256 KiB where 64 would do**, and it is
paid only for tiles the artist has actually painted into. If mask memory ever
shows up in a profile, a narrow tile type is the change, and it is confined to
`LayerMask` and `maskCoverage`.

**Why the red channel and not the alpha.** The brush paints premultiplied
colour, so black at half pressure over white gives grey — which is the value an
artist expects a mask to hold. Reading alpha instead would make every colour
reveal and only the eraser hide. As a bonus the eraser still hides, because
erasing takes every channel to zero, so the two obvious ways to make a hole
agree.

**Why `outside` rather than "absent means hidden".** The first mask an artist
adds is the one that hides nothing, and a reveal-all mask made of stored tiles
costs 64 MiB on a 4000 x 4000 canvas to say "no change yet". `outside` also is
PSD's mask default colour, so an imported mask needs no expansion. The price is
one line in `LayerMask::tileFor`: a new tile is filled with `outside`, because a
tile that arrived at zero would turn 256 pixels black the moment a brush touched
the corner of it.

**Masks are always 8-bit, at both document depths.** Coverage is eight bits
everywhere it is stored, read or exported, and a 16-bit mask would double the
undo cost of a mask stroke to record shades nothing can show. This is D-023's
own argument, applied to the one channel that does not need the width.

**Coverage folds into the same `scale` as opacity and the clip mask** — one
rounding, not three — and the alpha a masked layer publishes to the clip mask is
the MASKED one, so a clipped layer cannot show through a hole its base does not
have. The GPU shader is that arithmetic line for line, in the same order.

**A folder may carry a mask.** It costs nothing extra: a folder's op already has
a `src` in both compositors, so the mask applies to the group's composited
result — the one thing a mask on each child could never express, and what D-027
had to drop with a warning. That warning is gone.

**The GPU composites masks; it does not paint them.** A mask dab is handed to
`cpuBackend()`, exactly as a 16-bit document already is (D-023). The batch in
`gpu.cpp` is keyed on a layer id alone, so a mask stroke and a pixel stroke on
one layer would land in a single dispatch on the wrong slot; a second key is a
change to every dab path, and the CPU finishes a mask stroke in milliseconds.
Compositing is the half that had to be on the device, because that is what runs
on every frame of every stroke — and `tests/differential.cpp` gained a masked
scenario without either tolerance being touched (colour ≤ 1, alpha exactly 0).

**A mask slot rides in the op list's spare bits.** `ops[i].y` becomes two
sixteen-bit slots and `ops[i].z` gains a flag and the default colour, so the
uniform block keeps its 192-op ceiling. Doubling the op width would have halved
how much of a document the GPU can composite before falling back, in exchange
for nothing an arena of 512 slots can use.

**Format version 7, written only by a document that has a mask.** Same deal as
`SABLE_FORMAT_VERSION_8BIT` (D-023), and masks need it more than depth did: an
older Sable would open a masked document, show every layer unmasked, and write
the masks away on the next save.

**What this costs, stated plainly:**

- **ORA export bakes the mask into the alpha.** OpenRaster has no mask element
  — a layer is a PNG and nothing else — so the choice is between a file that
  looks like the painting and one that shows what the artist masked away, and
  D-027 already answered that. The `.sable` keeps the editable version. KRA
  export does not exist, so there is nothing to bake there.
- **Merging down bakes the upper layer's mask** into the pixels it contributes,
  because the layer it belonged to stops existing. The LOWER layer keeps its own
  mask, which therefore also applies to what was merged into it.
- **Mask tiles are not selections and selections are not masks yet.**
  `maskCoverage()` and `Selection::coverage()` deliberately answer in the same
  units, with the same convention — 0 hides, 255 shows, in between is the soft
  edge — so #52 can convert either way without a new definition of coverage. The
  conversion itself is not written; writing it before #52 knows what it needs is
  guessing.

**Alternative rejected — a mask as a hidden `Layer`.** It would have made the
paint path free (`PaintTarget` already takes a `Layer&`) but every layer walk in
the program — `levelOf`, the layer panel, `layerById`, the PSD and ORA writers —
would have had to learn to skip it, and the day one forgot, the mask would
composite as a grey rectangle over the artist's drawing.

---

## Open decisions

These blocked the milestone named. They have all been answered — the entries
above (D-014 to D-018) carry the reasoning.

| ID | Question | Blocks | Leaning |
|---|---|---|---|
All of these were answered during Milestones 1–4. Kept here with their
resolutions, because the questions are the useful part of the record.

| ID | Question | Resolved as |
|---|---|---|
| ~~D-100~~ | Tile bytes on disk: PNG per tile, or raw + deflate? | **PNG per tile, stored not deflated.** Implemented in `project.cpp`. Deflating an already-compressed PNG wastes save time; storing it keeps `unzip` and any image viewer working on the contents |
| ~~D-101~~ | Default keyboard map | **Our own defaults, every one reassignable.** See `app/src/shortcuts.cpp`. No observed SAI binding was treated as authoritative — the conflict note below is why |
| ~~D-102~~ | Undo memory cap and eviction | **Answered by D-017** — 256 MB, oldest dropped first, stated in the UI |
| ~~D-103~~ | Stabilizer algorithm | **Pulled-string**, as the leaning said. It holds corners, which a moving average rounds off — and that is what line art cannot afford. See `input.cpp` |
| ~~D-104~~ | ImGui docking branch: track it, or fixed layout? | **Answered by D-016** — track the branch, pinned to a commit |
| ~~D-105~~ | Colour management (assume sRGB vs ICC) | **Assume sRGB**, recorded in every `.sable` manifest as `"colour": {"space": "sRGB"}` so a future ICC-aware version can tell v1 files apart |
| ~~D-106~~ | Packaging: Flatpak, AppImage, distro packages | **Answered by D-018** — AppImage, built from the normal install tree |

### Note on D-101 — the shortcut references disagree

The SAI shortcut observations this project started from contradicted each other,
so none of them is usable as a default map. Recorded here because the finding
outlives the notes it came from:

- A general shortcut reference had `B` = Brush tool, `E` = Eraser.
- A capture of an actual Shortcut Key Assignments screen had `B` = AirBrush
  preset, `C` = a preset named after that artist's own custom brush, and
  `D` = Erase layer.

The second was one artist's customised setup, not a default — which is exactly
why observed key maps cannot be copied. Pick our own defaults from what Sable
actually has, and make every binding reassignable; that is the real requirement.

Keyboard defaults carry extra weight here: with no screen-reader support
(D-002), the keyboard is the only accessibility affordance we ship.

---

## How to add an entry

Append; never edit a Decided entry in place. If a decision changes, add a new one
and mark the old **Superseded by D-0xx**. Cheap format, and it preserves the
reasoning a future contributor would otherwise re-litigate:

```
## D-0xx — <one-line decision>
**Status:** Decided | Open | Superseded by D-0yy
**Affects:** <areas>
<the decision>
**Why:** <the reason>
**Alternative rejected:** <what, and why not>
```

---

Sources for the D-002 evaluation:
[SDL3 Pen API](https://wiki.libsdl.org/SDL3/CategoryPen) ·
[SDL_PenAxis](https://wiki.libsdl.org/SDL3/SDL_PenAxis) ·
[SDL_pen.h](https://github.com/libsdl-org/SDL/blob/main/include/SDL3/SDL_pen.h) ·
[SDL_PenAxisEvent](https://wiki.libsdl.org/SDL3/SDL_PenAxisEvent) ·
[GTK4 GestureStylus](https://gtk-rs.org/gtk4-rs/git/docs/gtk4/struct.GestureStylus.html) ·
[FLTK 1.5 events](https://www.fltk.org/doc-1.5/events.html) ·
[wxWidgets pen input](https://forums.wxwidgets.org/viewtopic.php?t=35023) ·
[JUCE MouseEvent](https://docs.juce.com/master/classMouseEvent.html) ·
[JUCE licence](https://github.com/juce-framework/JUCE/blob/master/LICENSE.md) ·
[libinput tablet support](https://wayland.freedesktop.org/libinput/doc/latest/tablet-support.html)
