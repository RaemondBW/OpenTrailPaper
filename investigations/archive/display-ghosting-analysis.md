# E-Paper Ghosting Analysis: solid-black-over-a-line trails

Panel: LilyGO T5S3 4.7" PRO (ED047TC1), driven by the vendored `epdiy`
high-level API. Firmware refresh policy lives in `src/ui_dashboard.cpp`.

## 1. Root cause

The trail is not primarily a DU/GL16 "weak drive" problem — it is a
**consequence of epdiy's high-level API being fully differential**, combined
with DU/GL16 being non-inverting.

### 1a. epdiy only drives pixels that changed value

Every on-screen update goes through `epd_hl_update_screen()` →
`epd_hl_update_area()`, which first computes a diff of the new frame
(`front_fb`) against what is on glass (`back_fb`):

- `vendor/.../epdiy/src/highlevel.c:118` — `epd_difference_image_cropped(front_fb, back_fb, …)`
- `vendor/.../epdiy/src/render.c:389-391` — a pixel is marked dirty **iff its
  4-bit value changed**:
  ```c
  col_dirtyness[x/2] |= (t ^ f) << (4 * (x % 2));
  dirty |= (t ^ f);           // t = new (to), f = on-glass (from)
  ```
- `vendor/.../epdiy/src/highlevel.c:127-129` — if nothing changed, it returns
  immediately and drives nothing.

Only dirty pixels are handed to the waveform (`highlevel.c:139`, via
`MODE_PACKING_1PPB_DIFFERENCE`). **A pixel that is black in both the old and new
frame has `t ^ f == 0`, so it receives no waveform at all — for ANY mode,
including `MODE_GC16`.**

### 1b. Why that produces the trail

When an area that contained a thin black mark (road line, position ring, text)
becomes **solid black**:

- The surrounding pixels go white→black. They are dirty, so they are driven
  black by the current waveform.
- The old mark's pixels go black→black. They are **not dirty**, so they are
  **never re-driven**. They keep whatever micro-reflectance their earlier drive
  history left them at.

Fresh-driven black and "sat-there" black differ slightly in reflectance, so the
old mark shows through the new fill as a faint outline. This is independent of
how strong the surrounding drive is — even a perfect GC16 on the *changed*
pixels cannot equalize a pixel it never touches.

### 1c. Why DU/GL16 make it worse, and why the current "clean" doesn't fix it

Panel modes for the ED047TC1 (`vendor/.../waveforms/epdiy_ED047TC1.h`):

| Mode | type | phases | character |
|------|------|--------|-----------|
| DU   | 1 | 5  | mono, **non-inverting**, fast |
| GC16 | 2 | 30 | grayscale, **flashing / inverting** |
| GL16 | 5 | 30 | grayscale, **non-inverting**, no flash |
| WHITE_TO_GL16 | 16 | 15 | from-white fast repaint |
| BLACK_TO_GL16 | 17 | 15 | from-black fast repaint |

DU (`ui_dashboard.cpp:226`) and GL16 (`:238`) are non-inverting: a white→black
transition is a single push, so even the *driven* pixels don't fully saturate,
leaving them a touch lighter than a GC16-cleared black and widening the
old-vs-new-black gap.

Critically, the existing "deep clean" does **not** cure this artifact. The GC16
path at `ui_dashboard.cpp:238` calls `epd_hl_update_screen(&hl, MODE_GC16, …)`,
which is still the **differential** path — it re-flashes only pixels that
changed since the last push and therefore skips the static-black old mark. The
only place the firmware truly clears it today is `epd_fullclear()` at boot /
shutdown (`ui_dashboard.cpp:122`, `highlevel.c:185`), which sets `front_fb` to
all-white *first* so that every non-white pixel becomes dirty and gets driven —
a non-differential clear. That path is never used during normal riding.

Current heuristic, for reference (`ui_dashboard.cpp`):
- `refresh()` `:216-242` chooses DU vs GL16/GC16.
- `ghostDebt` / `GHOST_GL16_EVERY` `:185-186` — DU updates before a clean.
- `ghostCleanPending` / settle timer `:191-192`, fired at `:1193-1194`.
- Map settle forces GC16 (`forceClean && screen == SCREEN_MAP`) at `:237` — but
  differentially, so it cannot reach the trail.

## 2. What paperboy (and general e-paper practice) does

zephray/Wenting Zhang's **paperboy** (gitlab.com/zephray/paperboy) is a 60 Hz
E-Ink Game Boy emulator — the same person behind the Modos 60 fps e-ink
monitor. It solves fast refresh by **bypassing the vendor waveform LUT
entirely**: it uses the panel's raw row/column interface to run a continuous
scan with its own per-pixel drive state and a greyscale + dither blend
(`main/msg/msg.c`, `main/main.c`). That is exactly the direction this repo's
`smooth_epd.cpp` M1 self-test is prototyping (Modos-style continuous drive).

Its ghosting cure is the classic, non-differential flush. `ui_clear_ghosting()`
(`main/ui.c`) drives the **whole frame** through a hard inverting cycle,
ignoring any diff:

```c
void ui_clear_ghosting(void) {
    uint8_t *fb = msg_flip();
    for (int i = 0; i < 5; i++) { memset(fb, 0x00, EPD_VIDEO_FB_SIZE); fb = msg_flip(); } // black
    for (int i = 0; i < 5; i++) { memset(fb, 0xFF, EPD_VIDEO_FB_SIZE); fb = msg_flip(); } // white
}
```

It is triggered **on demand** (a touch action `TP_ACTION_CLEAR_SCREEN`), not
per-region and not on a fixed cadence. General e-paper practice agrees:

- The reliable ghost killer is a **non-differential full/inverting drive** (GC16
  "global clean", or an explicit black→white flash) that touches *every* pixel
  in the region, including unchanged ones.
- Partial/DU updates accumulate "ghost debt"; a periodic full clear pays it off.
- Higher-end controllers (and paperboy/Modos) track **per-pixel** drive state so
  they can selectively re-saturate; that requires owning the waveform, which the
  epdiy high-level API does not give us.

Takeaway for us: within the epdiy diff model, the fix must **break the diff** so
the static-black pixels get driven — i.e., flash the region to a uniform state
first, then repaint.

## 3. Ranked fix options

### A. True non-differential clean, full-screen, gated (RECOMMENDED)
Replace the ineffective differential GC16 settle-clean with a real
`epd_fullclear`-style clean: set `front_fb` all-white, GC16 (glass + `back_fb`
→ uniform white), restore the frame, repaint with GL16 from that known white
state. Because the repaint drives every target-black pixel from the *same* white
origin, the old mark and its surroundings end identically black — trail gone.
- Visual quality: fully fixes the reported artifact.
- Cost: one white flash + one non-flash repaint, ~1.4 s; only when triggered.
- Battery: one GC16 flash worth of TPS current per clean — acceptable if rare.
- Complexity: low (mirrors the proven boot path). Risk: low.
- Mitigation for the flash/over-clean concern: gate it behind `ghostDebt`
  (fire only after a real interaction has built up ghosting), and only on the
  map screen where the artifact occurs. Quick taps keep the cheap GL16.

### B. Scoped local clear-to-white then repaint (best quality/cost, more work)
Same idea as A but limited to a bounding box (e.g. the map viewport, or the
union of epdiy's own dirty rects since the last clean): `epd_clear_area(box)`
→ resync `back_fb` for the box to white → `epd_hl_update_area(GL16, box)`.
Flashes only the changed region, so it is faster (~0.5–0.8 s), lower power, and
far less visually jarring (status bar / footer never flash).
- Visual quality: fixes the artifact, less flash than A.
- Cost/battery: lower than A (region-scoped).
- Complexity: medium — must keep epdiy's `back_fb` in sync after the low-level
  `epd_clear_area` (which bypasses the high-level state). Risk: medium
  (back_fb desync → wrong subsequent diffs).

### C. Per-pixel ghost-debt / continuous drive (paperboy/Modos style)
Extend `smooth_epd` M2 into the real path: own the waveform, track per-pixel DU
accumulation, force a GC16 cycle on pixels whose debt is high. Highest quality
(could also kill DU tail-ghosting generally) but a large, risky rewrite that
replaces epdiy's update path. Long-term direction, not a near-term fix.

### D. Just raise GC16 clean frequency / lower the ghostDebt threshold
Cheapest to type, but **ineffective for this specific artifact**: the current
GC16 clean is differential and never touches the static-black mark. Only worth
doing *in combination with* making the clean non-differential (i.e., it reduces
to A). By itself it only adds flashes without fixing the trail.

**Recommendation: A now, with B as the follow-up optimization.** A is the
minimal, safe, self-contained change that actually removes the trail; B then
trims its flash/battery cost by scoping it once A is validated on-glass. C is
the eventual endgame via `smooth_epd`.

## 4. Drafted fix (uncommitted)

Implemented option A in `src/ui_dashboard.cpp`:

1. Added `deepClean(int temp)` (before `refresh()`): saves the current frame to
   `shadowFb`, sets `front_fb` all-white, runs a GC16 (drives glass and epdiy's
   `back_fb` to uniform white), restores the frame, then repaints with GL16 from
   that uniform white origin — non-flashing and outline-free. Falls back to a
   plain GC16 if `shadowFb` is unallocated. This is the `epd_fullclear` pattern
   already trusted at boot/shutdown, so it is safe.

2. Added `GHOST_DEEP_MIN = 4` and routed the map settle-clean through
   `deepClean()` only when `forceClean && screen == SCREEN_MAP && ghostDebt >=
   GHOST_DEEP_MIN`. Light interactions (a couple of DU frames) still take the
   cheap non-flash GL16; a clean only flashes once real DU ghosting has
   accumulated. Raise `GHOST_DEEP_MIN` to flash less (more lingering ghost,
   better battery); lower it for a cleaner panel.

Net behaviour: during active tapping the fast DU path is unchanged; when the map
settles after a real zoom/pan, the panel does one brief white flash and repaints
clean, eliminating the solid-black-over-a-line trail. Idle/riding battery draw is
unchanged (deepClean only runs on a map settle with built-up debt). Not flashed
or tested on hardware — validate the flash cadence on-glass, and if it feels too
frequent or too heavy, either bump `GHOST_DEEP_MIN` or move to option B
(region-scoped clear).

### Follow-up ideas
- Scope `deepClean` to the map viewport rect (option B) to avoid flashing the
  static status bar/footer.
- Consider `MODE_EPDIY_WHITE_TO_GL16` (type 16, 15 phases) for the repaint pass —
  it is the from-white fast path and could shave the repaint time.
