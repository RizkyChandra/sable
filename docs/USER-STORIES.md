# Sable — User Stories and Acceptance Criteria

Scope: **Milestone 1 and Milestone 2 only.** Stories for M3 and M4 are
deliberately unwritten — their shape depends on what M2 teaches about brush feel,
and writing them now would be fiction.

Acceptance criteria are written to be checkable. If a criterion cannot be
demonstrated in the running application or asserted in a test, it is not a
criterion — it is a wish, and it does not belong here.

Priority: **P0** blocks the milestone. **P1** ships if it fits.

---

## US-00 — Spike: confirm stylus pressure reaches us — P0, do this first

As the developer, I need to prove SDL3 delivers pen pressure on Linux before
building on it, so that a fatal input problem surfaces in half a day rather than
after Milestone 2.

Not a product feature. This is the gate on D-002, and it comes before US-08 —
ideally before US-01.

**Acceptance criteria**

1. A throwaway SDL3 program logs, per event: `SDL_EVENT_PEN_MOTION` position,
   and every `SDL_EVENT_PEN_AXIS` with its axis and value.
2. On a real tablet, `SDL_PEN_AXIS_PRESSURE` varies between 0.0 and 1.0 as the
   pen is pressed.
3. `SDL_PEN_AXIS_XTILT` / `YTILT` report plausible values on a device that
   supports tilt, and are absent or zero — without crashing — on one that does
   not.
4. `SDL_PenID` is stable for a pen across the run, and distinct between two pens
   if you have them. This is what per-device calibration keys on.
5. A fast stroke produces **many** motion events per frame when the queue is
   drained. Log the count per iteration — if it is 1, you are losing samples and
   stroke quality will suffer.
6. **Synthetic mouse events are off.** A single pen stroke produces zero
   `SDL_EVENT_MOUSE_MOTION` events. Find the SDL hint that controls this, set it,
   and assert it here — left on, every stroke gets painted twice.
7. **Test on both video backends.** Force each with `SDL_VIDEODRIVER=x11` and
   `SDL_VIDEODRIVER=wayland` and record which axes arrive on each. Pen support
   may differ, and the answer determines what we promise in the README.
8. If 2, 5, or 6 cannot be made to pass, stop and reopen D-002a before writing
   any engine input code.

Keep this program. It becomes the test pad in US-10.

---

## Milestone 1 — Proof of drawing

**Goal:** a 1024 × 1024 white canvas where the mouse draws smooth black strokes,
with undo, redo, clear, and PNG export.

**Done when:** an artist can draw a recognisable doodle, undo half of it, and
export a PNG that opens correctly elsewhere.

### Window layout for Milestone 1

Deliberately minimal. Docking arrives at M4 — do not build a panel system now.

```
┌──────────────────────────────────────────────────┐
│ File   Edit   View                               │  ImGui main menu bar
├──────────────────────────────────────────────────┤
│                                                  │
│              ┌──────────────────┐                │
│              │                  │                │
│              │   white canvas   │                │  pannable /
│              │   1024 × 1024    │                │  zoomable
│              │                  │                │  viewport
│              └──────────────────┘                │
│                                                  │
├──────────────────────────────────────────────────┤
│ 100%   1024×1024                                 │  status line
└──────────────────────────────────────────────────┘
```

---

### US-01 — Start a new canvas — P0

As an artist, I want a white canvas when the application opens, so that I can
draw immediately without setup.

1. Launching with no arguments shows a 1024 × 1024 white canvas, centred and
   fully visible.
2. `File > New` (`Ctrl+N`) asks for width, height, and background (white or
   transparent), then replaces the canvas.
3. Creating a new canvas with unsaved strokes prompts to discard; cancelling
   leaves the current canvas untouched.
4. Canvas dimensions appear in the status line.
5. Process start to drawable canvas is under 1 second on a mid-range laptop.

### US-02 — Draw a stroke with the mouse — P0

As an artist, I want to press and drag to paint, so that I can put marks on the
canvas.

1. Press, drag, release paints one continuous black stroke.
2. The stroke follows the cursor with no visible lag at normal drawing speed.
3. A single click without movement paints one dab — a click never does nothing.
4. Dragging off the canvas edge and back paints only the on-canvas part. No
   crash, no wrap-around.
5. Releasing outside the window ends the stroke cleanly; the next press starts a
   new one.
6. Stroke edges are anti-aliased, not stair-stepped.
7. Only touched tiles are recomposited **and re-uploaded to their
   `SDL_Texture`** — verify by logging the touched `TileKey` set and the upload
   count per stroke. They must match.
8. Canvas textures are drawn with `SDL_BLENDMODE_BLEND_PREMULTIPLIED`. A soft
   stroke over the white background shows no grey fringe.
9. No allocation on the hot path. Check with a counter or a profiler, not by eye.

### US-03 — Fast strokes have no gaps — P0

As an artist, I want quick strokes to be solid, so that fast sketching does not
produce dotted lines.

The acceptance test for the interpolator, and the single most likely thing to be
wrong in the whole engine.

1. A stroke flicked across the full canvas in under 200 ms is continuous, with no
   gap along its length.
2. A very slow stroke gives even coverage — no dark clumps where samples bunched.
3. A sharp V does not round off or overshoot at the corner.
4. **Test:** feed a synthetic `std::vector<InputSample>` with large positional
   jumps into the interpolator and assert consecutive dab centres are never
   further apart than `spacingPx`, **including across segment boundaries**. This
   is what catches a reset `leftoverDistance`.

### US-04 — Undo and redo — P0

As an artist, I want to take back mistakes, so that I can experiment freely.

1. `Ctrl+Z` removes the last complete stroke; `Ctrl+Y` restores it.
2. One stroke is exactly one undo step, however long it was or however many tiles
   it touched.
3. At least 50 consecutive undo steps are available.
4. Undoing to the beginning leaves the canvas identical to a fresh one. Further
   undo is a no-op, not a crash.
5. Drawing after undoing discards the redo stack.
6. Both actions appear in the Edit menu with correct enabled/disabled state.
7. **Test:** draw → hash canvas → undo → redo → hash canvas; hashes match.
8. **Test:** undoing the first stroke on a tile *erases* the tile from the map
   rather than zeroing it — assert the map size returns to its prior value, and
   that any cached `SDL_Texture` for it is released.

### US-05 — Pan, zoom and rotate the view — P0

As an artist, I want to move, magnify and turn the canvas, so that I can work on
detail, still see the whole piece, and draw every stroke at a comfortable angle.

1. Holding `Space` and dragging pans; the cursor shows it.
2. The scroll wheel zooms about the cursor position, not the window centre.
3. Zoom range at least 10%–800%, shown in the status line.
4. `Ctrl+0` fits to window; `Ctrl+Alt+0` sets 100%.
5. Panning and zooming never alter pixels — undo state and dirty flag unchanged,
   and no tile is re-uploaded.
6. Drawing at 400% zoom lands the stroke at the correct canvas coordinates, not
   offset by the zoom factor. Screen-to-canvas conversion happens in the app
   layer before the `InputSample` is built.
7. At 100%, canvas pixels map 1:1 to screen pixels with no resampling blur —
   check the texture scale mode is nearest at 100% and above.
8. Bound keys turn the view left and right in 15° steps about the middle of the
   viewport, and reset it; the angle shows in the status line beside the zoom.
   Rotation obeys point 5 as well — it alters no pixels.
9. **Test:** screen → canvas → screen is the identity at several angles,
   including negative ones and past a half turn. A conversion that drops the
   rotation paints in the wrong place and still looks nearly right at 10°.

### US-06 — Clear the canvas — P1

As an artist, I want to wipe the canvas, so that I can restart without losing my
window setup.

1. `Edit > Clear` fills with the background colour.
2. Clear is a single undoable action.
3. Clearing an already empty canvas is harmless.

### US-07 — Export PNG — P0

As an artist, I want to save my drawing as a PNG, so that I can share it.

1. `File > Export > PNG` opens **SDL3's native save dialog**
   (`SDL_ShowSaveFileDialog`), not an ImGui file browser.
2. Output is exactly the canvas dimensions — not the viewport, and unaffected by
   zoom.
3. A white background exports opaque. A transparent background exports with a
   correct alpha channel and **no dark halo** at stroke edges — this is the
   `PremulRgba8 → StraightRgba8` conversion, and getting it wrong greys every
   soft edge.
4. The file opens correctly in at least two other viewers.
5. Exporting does not modify the document or clear the dirty flag.
6. Overwriting an existing file asks first.
7. A failed write (no permission, disk full) shows an error and loses nothing.
   The engine returns `std::expected` and the app renders the `Error::detail`
   text; the process does not terminate.

### US-14 — The application idles quietly — P0

As a laptop user, I want the application to stop working when I stop drawing, so
that it does not drain my battery while I think.

Small story, listed late, but it constrains how the whole loop is written (D-008)
— so implement it with US-01, not after.

1. With the window open and no input, CPU use is near zero. Confirm with `top`.
2. The event loop blocks on `SDL_WaitEvent` rather than spinning at display rate.
3. Drawing is continuous and smooth *during* a stroke — idling must not cost
   latency when input resumes.
4. A background save reports progress without a spin loop.

---

## Milestone 2 — Tablet feel

**Goal:** pressure drives size and density, the stabilizer works, and a test
panel confirms input quality.

**Gated on US-00 passing.**

**Done when:** an artist with a tablet can draw a tapered line whose weight
follows their hand, and can tune the response without editing a file.

### US-08 — Draw with pen pressure — P0

As an artist with a graphics tablet, I want light pressure to make thinner and
fainter marks, so that my strokes have natural weight.

1. Pen input arrives as `SDL_EVENT_PEN_DOWN` / `MOTION` / `UP`, with axes from
   `SDL_EVENT_PEN_AXIS`.
2. The app maintains per-`SDL_PenID` axis state and builds one `InputSample` per
   motion event using the latest known axes. SDL does not deliver position and
   pressure in the same event — see `DATA-MODEL.md`.
3. The whole event queue is drained each iteration; no queued motion event is
   discarded.
4. Pressure maps to brush size and to density; each mapping switches off
   independently.
5. A stroke that starts light, presses hard, and lifts produces a tapered mark.
6. Pen buttons and the eraser end are distinguished from the tip.
7. With no tablet connected, mouse drawing still works at full pressure. The
   application never requires a tablet.
8. Hovering without contact does not paint.
9. Pen-to-pixel latency is not noticeably worse than the mouse path.

### US-09 — Calibrate pressure response — P0

As an artist, I want to adjust how my pressure is interpreted, so that the brush
suits my hand rather than the driver's defaults.

1. Soft, Normal, and Hard presets, plus an editable custom curve.
2. A minimum-pressure threshold ignores faint accidental contact.
3. A maximum-pressure clamp reaches full value before the physical maximum.
4. A smoothing control reduces jitter from noisy devices.
5. Any change affects the next stroke immediately — no restart.
6. Settings persist across restarts and apply to whatever pen is connected.
   *Revised during implementation:* this criterion originally read "stored per
   device, keyed on `SDL_PenID` plus the device name, and reload when that
   device reconnects". SDL exposes no pen name and its `SDL_PenID` does not
   survive a replug, so that is not buildable on this API — see **D-015**.
7. **Test:** assert the normalisation order is deadzone → rescale → smoothing →
   curve. Reversing the first and last steps produces a device-specific bug that
   is close to undiagnosable from a user's description, and a unit test costs
   almost nothing.

### US-10 — See what the tablet is doing — P1

As an artist whose pen feels wrong, I want a test pad, so that I can tell whether
the problem is my tablet, my calibration, or the application.

Grows directly out of the US-00 spike — do not throw that code away.

1. Live readout of raw pressure, normalised pressure, position, tilt, and the
   active `SDL_PenID`.
2. Plots the active pressure curve with current input marked on it.
3. A scratch area that paints with the current brush and clears.
4. Shows device name and whether input is pen or mouse.
5. Shows the motion-events-per-frame count — the number that tells you whether
   samples are being lost.
6. Reports plainly when no tablet is detected, rather than showing zeros.

### US-11 — Steady my lines with the stabilizer — P0

As an artist, I want shaky strokes smoothed, so that I can draw clean line art.

1. Off, Low, Medium, High selectable from the toolbar.
2. Off adds no measurable latency and no smoothing.
3. Higher levels visibly reduce wobble on a slow curve.
4. The stroke still ends at the pen's release point — a smoothed line must not
   stop short of where the artist lifted.
5. The level is saved per brush preset.
6. Positions only. Pressure response is untouched.

### US-12 — Change brush size quickly — P0

As an artist, I want fast size changes, so that I can move between broad and fine
work without breaking flow.

1. A size slider and numeric field, at least 1–500 px.
2. `[` and `]` step through size presets.
3. Presets are editable and persist between sessions.
4. The brush cursor is a circle showing the true painted diameter at the current
   zoom.
5. Size changes apply to the next stroke, never mid-stroke.

### US-13 — Pick a colour — P0

As an artist, I want to choose and sample colours, so that I can draw in
something other than black.

The colour wheel is a custom ImGui widget — there is no built-in one. Prototype
it early; it is the best calibration of how much custom widget work M3 and M4
will need.

1. A colour wheel plus HSV sliders set the foreground colour.
2. Hex entry is accepted and displayed.
3. `Alt` + click samples the colour under the cursor from the canvas.
4. Sampling a semi-transparent pixel yields the colour actually visible there.
5. Foreground and background swatches swap and reset to black/white.
6. **Test:** the centre pixel of a solid dab at full density equals the selected
   RGB exactly, after unpremultiplying. Off-by-one here means every colour the
   artist picks is subtly wrong.

---

## Cross-cutting requirements

Apply to every story. They do not get their own tickets, and they are not
candidates for later simplification.

| Area | Requirement |
|---|---|
| Never lose work | No path touching a file, a device, or user input may terminate the process. Engine failures come back as `std::expected`; a top-level `catch (...)` per menu action is the backstop, not the primary path (D-012). |
| Input trust | Canvas dimensions, paths, and loaded files are validated before use. A malformed `.sable` fails with a message — never a crash, never a silent partial load. |
| No tablet required | Every feature reachable with a mouse. |
| Offline | No network calls, no telemetry, no account. |
| Keyboard | **Every action reachable by keyboard, and every binding reassignable.** With no screen-reader support, this is the only accessibility affordance we ship — treat it as a hard requirement, not a convenience. |
| Idle cost | Event-driven loop (US-14). No spin loop, ever. |
| Performance | No allocation on the dab path. Painting must not stutter on 4000 × 4000 on modest hardware. |
| Engine purity | The `engine` target never links SDL3 or ImGui. If a story seems to need it, the boundary is in the wrong place — fix the boundary. |
| Memory | Run the paint and undo paths under AddressSanitizer in CI. Run the save path under ThreadSanitizer. C++ gives no guarantees here; the sanitizers are the substitute. |
| Tests | Dab spacing, tile compositing, premultiply round-trip, pressure normalisation, and undo/redo symmetry carry doctest unit tests, all runnable headless. UI tests are not needed yet. |

---

## Suggested build order

Each step is demonstrable on its own.

1. **US-00** spike (gate on D-002) — before anything else if a tablet is available
2. US-01 canvas + US-14 event loop → US-02 mouse stroke → US-03 interpolation —
   the engine core, with the loop shape correct from the start
3. US-04 undo → US-07 export — now it is a usable toy
4. US-05 pan/zoom/rotate → US-06 clear — **Milestone 1 complete**
5. US-08 pressure → US-09 calibration → US-11 stabilizer — M2 core
6. US-12 size → US-13 colour → US-10 test pad — **Milestone 2 complete**

Do not start layers, selections, or save/load until US-01 to US-14 pass. `Tile`,
`UndoRecord`, and `Dab` are what everything else is built on; changing them once
layers exist costs several times more.
