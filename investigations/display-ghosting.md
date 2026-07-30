# E-paper ghosting and grey drift — mechanisms and fixes

Status as of 2026-07-27. Several distinct problems got conflated under the word
"ghosting" during development; separating them is most of the value here. Each
section states the *mechanism* first, because every wrong fix in this history
came from acting on a plausible-sounding theory instead of the actual physics.

## 0. The three facts everything follows from

**1. epdiy is fully differential.** `epd_hl_update_area()` calls
`epd_difference_image_cropped()` and drives a pixel **only where
`front_fb != back_fb`**. Consequences that repeatedly caught us out:

- Re-pushing an unchanged frame is a **no-op**, however aggressive the waveform.
  You cannot "just do a GC16" to clean the screen.
- To force a pixel to be driven you must make `back_fb` differ from `front_fb`.
  Writing white into `back_fb` gives that pixel a full white→content drive.
- A pixel whose new value equals what is already on glass is **skipped**, even
  if it is visibly wrong.

**2. The waveforms differ in kind, not just speed.**

| Mode | What it does | Cost |
|---|---|---|
| `MODE_DU` | Fast, **strictly 1-bit**, non-inverting. Not DC-balanced. | ~300 ms |
| `MODE_GL16` | 16-level greyscale, no flash. Cannot restore white that was never re-driven. | ~950 ms |
| `MODE_GC16` | Full inverting black/white/black flash. The only true clean. | slowest |

**3. Rotation.** The panel is `INVERTED_PORTRAIT`: `native_x = app_y`,
`native_y = epd_height()-1 - app_x`. A native box inverts to app coords as
`x = epd_height()-(ny+nh), y = nx, w = nh, h = nw`. Framebuffer is 4bpp
(2 px/byte), 960×540 native, 480 bytes/row, `0xFF` = two white pixels.

## 1. Trails in dark map areas after pan/zoom

**Symptom.** After panning or zooming, dark regions (ocean, parks) kept a
mottled residue with old coastlines faintly visible. Land and roads were clean.

**Mechanism.** DU leaves residue, and because the update is differential, a
pixel that is dark before *and* after gets no waveform at all — it keeps its
settled state, so the trail persists. Compounding it, the water fill is a **75%
dither**: the ghost lives in both the dark dither pixels *and* the white holes
between them, so a "redraw the dark pixels" fix cannot work.

**What did not work, and why (all of these were tried):**

- *Re-push with GL16.* No-op — unchanged frame diffs to nothing.
- *Drive all dark pixels in a bounding box.* The map is dark-dense, so this
  flashed most of the screen.
- *Drive only pixels that changed AND are dark.* Misses the drift entirely: a
  solid ocean's bytes barely change while panning, because the dither is
  screen-locked. The pixels that *do* change are near edges, so the flash
  appeared in the wrong places — the opposite of what was wanted.
- *Single-pass inversion (`back = ~front`) over the water.* Drives dark→white
  and white→black simultaneously on adjacent dither pixels; the opposing drives
  crosstalk and the dark tone comes back washed-out grey.

**What works — a two-phase flash through white**, applied only to the affected
region:

1. Force the region to white (`front = white`, `back = black`) and GC16 — one
   clean sheet, erasing residue in dark pixels and holes alike.
2. Restore the content (`front = content`, `back = white`) and GC16 — now the
   holes are white→white (**no drive**, so no crosstalk) and only the dark
   pixels drive white→black, coming back fully saturated.

Implemented as `twoPhaseFlash(bx0, bx1, temp)` and `ghostRegionClean()`.
Scoping to the map viewport columns (`STATUS_H/2` .. `MAP_STRIP_TOP/2`) keeps
the status bar and footer perfectly still.

## 2. The previous screen ghosting through on map ↔ data transitions

**Symptom.** Switching to the map showed the dashboard's big "0.0" digits and
the SPEED/DIST/TIME grid bleeding through the dark water.

**Mechanism.** A differential GC16 **skips** pixels where the new frame matches
the on-glass state. Where the dashboard's black digits landed under the map's
dark water, `front == back == dark`, so those pixels were never re-driven and
the digit shapes survived.

**Fix.** Map↔data transitions take a **whole-screen** two-phase clear
(`twoPhaseFlash(0, nwb)`) rather than a differential update. Every pixel is
driven through white, so nothing from the previous screen can survive.

## 3. Deferring the clean until the interaction ends

The clean is expensive, so doing it mid-gesture makes zooming feel slow.
`ghostCleanPending` is set on interactive DU frames and fired only after
`GHOST_CLEAN_SETTLE_MS` (900 ms) of no input — and each new tap pushes that out.
The rider sees fast DU frames with ghosting during a rapid zoom burst, and one
clean at the end.

## 4. Grey slowly bleeding into the white background

The longest-running and most-misdiagnosed problem. **Two separate mechanisms,
both real.**

**A false start worth recording:** the first explanation offered was "DU residue
accumulating", with a periodic whole-panel clean as the fix. That was wrong on
its own terms — a genuinely static frame already short-circuits in `refresh()`
(`memcmp` against `shadowFb`) and never touches the panel. The user's
observation *"it doesn't do this when the screen is off"* is what forced a real
mechanism: if the panel only drifts while powered and updating, the updates
themselves are doing it.

### 4a. Full-screen DU scans

**Mechanism.** Small things *do* change on an idle screen — the clock minute, a
GPS signal dot crossing a satellite threshold, the battery percent. Each was
pushed as a **full-screen** DU update. DU is not DC-balanced, so pixels holding
still receive no intended transition but still sit through the scan's drive
voltages. Over hundreds of updates that imbalance accumulates as a grey cast.
One GPS dot appearing was electrically disturbing all 518,400 pixels.

**Fix.** Scope each DU to the rows that actually changed
(`changedNativeBox()` → `nativeBoxToAppFull()`). A clock tick touches the clock,
not the map. Faster and lower power as a side effect.

> **Critical detail:** diff against **`hl.back_fb`**, not our `shadowFb`.
> `epd_hl_update_area()` only syncs `back_fb` for lines inside the rect it is
> given. Scoping by `shadowFb` lets the two records drift apart, and a pixel
> that changes outside the rect becomes "already drawn" to us and "never drawn"
> to epdiy — invisible forever.

> Use `nativeBoxToAppFull()` here, not `nativeBoxToApp()`: the latter clamps to
> the map viewport, and the clock and GPS dots that change on an idle screen
> live in the status bar.

### 4b. Anti-aliased fonts

**Mechanism.** The bundled fonts are **4bpp anti-aliased**. Measured on
`ArialBold_14`, **~20% of glyph pixels sit at intermediate levels (1..14)**; a
rendered dashboard carries ~6,300 mid-grey pixels (1.2% of the panel),
concentrated on exactly the text that redraws every second. DU is strictly
1-bit and *cannot represent those values*, so every update drives each
anti-aliased edge pixel toward black or white with no stable target — a
continuous unresolved disturbance.

**Fix.** `flattenTo1Bit()` snaps every nibble to `0x0` or `0xF` before anything
reads the frame, so the framebuffer is genuinely 1-bit: DU always has an exact
target and GL16 renders the same thing. Text loses anti-aliasing and reads
harder-edged — on a 1-bit panel that is the honest rendering, since those greys
were only ever approximated.

This is the "screentone" approach the map already used for water (75% dot
dither) and parks (50% diagonal hatch), applied everywhere.

**Open question:** 1.2% of the panel is a modest area to explain a whole-screen
cast. If grey still accumulates, the next suspect is the periodic
`GHOST_GL16_EVERY` (24) pass, which re-pushes the **whole panel** and may be
doing its own DC damage.

## 5. Design rules worth keeping

- **Everything drawn must be pure black or white.** Greys are not displayable in
  DU, only approximated, and approximation is what causes drift. Use dithers for
  tone: water 75% dots, parks 50% diagonal hatch, trails a checkerboard.
- **Never assume a re-push cleans anything.** Differential means unchanged
  pixels are not driven. To clean, make `back_fb` differ.
- **Clean through white in two phases.** A single inverting pass crosstalks on
  adjacent dither pixels and washes out the dark tone.
- **Scope every update to what changed.** Both for drift and for power.
- **The status bar is not static.** The clock and GPS dots change; any rect
  logic that clamps it away will silently stop updating them.

## 6. Verification

`tools/preview/render_preview.sh` builds the renderers for the host and writes
`tools/preview/out/*.png` — pixel-identical to the device. Always render and
inspect after UI changes.

Note it exercises the **render** path only, not `refresh()`, so waveform and
panel-driver behaviour still needs on-device testing.

### How the EPD_Painter port was verified against epdiy

Worth recording because it worked far better than expected. `src/epd_compat.cpp`
replaces epdiy's thirteen drawing functions with from-scratch implementations, and
the risk in that is not "does it compile" but "does anything move by a pixel". The
test: render all 18 screens with epdiy, port the rasteriser, render again, and
diff every pixel — with `-DEPDC_TEXT_ANTIALIAS`, which restores epdiy's glyph
blending so text is comparable.

It caught four real defects that reading the code had not:

| Symptom in the diff | Cause |
|---|---|
| 286k px, long runs on every screen | alignment flags masked `& 0x3`, but `ALIGN_RIGHT` is `0x4` and `CENTER` is `0x8` — so every right/centre-aligned string silently rendered left-aligned. On the dashboard it put the battery percentage on top of the battery icon. |
| ~110k px at glyph edges | the AA threshold (expected; this is what the flag isolates) |
| 64 px per dashboard, x 123–181 | `epd_fill_circle` used the exact disc `dx²+dy² ≤ r²`. Mathematically rounder, ~12 px different at r=4, and it made the GPS signal dots visibly thinner. Deriving the spans from the midpoint outline instead is identical to epdiy for every r in 1..40. |
| ~90 px per map screen, isolated | `epd_draw_line` used the symmetric two-error Bresenham; epdiy uses the steep-swap left-to-right form. Both draw valid lines, and they disagree on about half of all diagonals. |

Final state: **0 differing pixels across all 18 screens.**

The first of those was a genuine visual regression that no amount of "it builds
and it looks about right" would have surfaced. If the drawing layer is ever
touched again, re-run this diff — keep a copy of `out/*.png` first, since the
script overwrites it.

## 7. Testing caveat on this history

A stretch of display work (`power-quick-wins`) was compiled by a **corrupted
local toolchain** (see `investigations/battery-life.md` §4). Results from that period —
including at least one round of "the ghosting fix didn't work" — are unreliable,
because the binary was mis-built rather than the logic wrong. The fixes were
re-applied in isolation on a known-good base afterwards
(branch `display-du-scope`).

**If a display fix appears not to work, flash the CI-built artifact of the same
commit before rewriting the logic.**
