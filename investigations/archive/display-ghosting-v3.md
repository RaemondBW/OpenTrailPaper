# E-Paper Ghosting V3: region-scoped ghost clear

Panel: LilyGO T5S3 4.7" PRO (ED047TC1), vendored `epdiy`. Rotation
`EPD_ROT_INVERTED_PORTRAIT`: native fb is 960×540 (landscape), the app draws in
540×960 (portrait). Refresh policy: `src/ui_dashboard.cpp`.

Symptom (unchanged): when a map area that held a thin black mark (road, position
ring, text) becomes SOLID BLACK, a faint trail of the old mark stays visible
inside the black, in dark regions.

## Why v1 and v2 both missed

- **v1 (differential GC16 on settle):** epdiy is fully differential — a pixel
  that is black before AND after gets no waveform (`render.c:390`,
  `dirty |= (t ^ f)`), and re-pushing an unchanged settled frame diffs to nothing
  and drives *nothing at all* (`highlevel.c:127` early-out). So the trail's
  static-black pixels were never touched.
- **v2 (full `epd_fullclear` on settle):** correct mechanism (flush to uniform
  white, then repaint from that origin so every pixel is driven), but applied to
  the WHOLE PANEL on every map settle. The user rejected it: far too many
  full-screen flashes, and it needlessly flashed the status bar and footer.

Both got the physics right eventually; the miss was **scope**. v3 keeps the
"drive every pixel from a uniform white origin" mechanism but applies it to only
the rectangle that changed.

## What v3 does

Three parts, all in `src/ui_dashboard.cpp` (see the diff at the end):

### 1. Track the actual dirty region (per interaction burst)

`ghostRectAccumulate(prev, cur)` diffs the just-rendered frame against the
previous one — `shadowFb` still holds the previous frame at that point, so the
diff is free of extra buffers — and unions the changed **native-fb** bounding box
into an accumulator (`ghMinX/Y..ghMaxX/Y`). It runs only when
`screen == SCREEN_MAP && du && active` (an active interactive map DU frame), so
hands-off riding, idle, and every other screen never pay the O(fb) scan and never
schedule a clean. Across a zoom/pan burst the accumulator grows to cover exactly
where the marker/roads swept and left a trail.

### 2. Convert to app coords and clamp to the map viewport

`ghostRectToAppClamped()` inverts the `INVERTED_PORTRAIT` rotation
(`native_x = app_y`, `native_y = epd_height()-1 - app_x`, from `epdiy.c` `_rotate`):
a native box `(nx,ny,nw,nh)` becomes app rect
`{ x = epd_height()-(ny+nh), y = nx, w = nh, h = nw }`. It then clamps the top to
`ui::STATUS_H` (64) so **the status bar is never in the rect**, and to
`W×H = 540×960` on the other edges. Result: an app-coordinate rectangle covering
only the changed map content.

### 3. Region-scoped non-differential clear + repaint

`regionClean(r, temp)` — the paperboy "drive every pixel" lesson applied to one
rectangle, entirely through the high-level area API so rotation and `back_fb`
stay correct automatically:

1. `epd_fill_rect(r, 0xFF, front)` — white-out the rect in `front_fb`
   (app coords, rotation handled by epdiy).
2. `epd_hl_update_area(&hl, MODE_GC16, temp, r)` — GC16 the rect: drives every
   non-white pixel in it to white AND, crucially, the high-level buffer-update
   loop (`highlevel.c:150-170`) copies `front → back_fb` for the rect, so
   **epdiy's `back_fb` for the rect becomes white**. This is the fix for the v1
   failure mode: without resetting `back_fb`, the repaint would diff to nothing
   and drive nothing.
3. `memcpy(front, shadowFb)` — restore the real frame.
4. `epd_hl_update_area(&hl, MODE_GL16, temp, r)` — GL16 repaint: with `back_fb`
   now uniform white in the rect, every target pixel differs from white and is
   driven from the identical origin, so the old mark and its surroundings end
   identically black. Trail gone.

Because `epd_difference_image_cropped` resets and scopes the dirty
lines/columns to the rect (`render.c:456-495`) and the app only wrote white
inside the rect, the update is masked to the rect — **only the rect is ever
driven; the status bar and footer are never touched.**

**Why the high-level area API instead of the low-level `epd_clear_area` +
manual `back_fb` poke:** `epd_clear_area` operates on the raw panel in native
coordinates and bypasses `back_fb`, so pairing it with the repaint would require
hand-writing `back_fb` in native/rotated layout — exactly the desync that
caused v1's no-op. Routing the clear through `epd_hl_update_area(GC16)` instead
gives the same non-differential region flush (GC16 is the inverting global-clean
waveform) while epdiy keeps `back_fb` correct under rotation for free. Same
guarantee, none of the manual-coordinate risk.

### Dispatch

In `refresh()`, the map settle-clean (`forceClean && screen == SCREEN_MAP`) now
calls `regionClean(ghostRectToAppClamped())` when a dirty rect was accumulated,
instead of the old full-screen GC16. `ghostRectReset()` clears the accumulator on
every clean (and on page-out GL16), so stale rects never leak across screens. The
nav-prompt modal keeps its intentional full GC16; all other cleans stay on the
no-flash GL16.

## What it clears, how big, how often

- **Clears:** the union of what changed on the map during the interactive burst
  (marker sweep + newly-solid-black areas), clamped to the map viewport. Drives
  every pixel in that rect from uniform white, so the solid-black-over-a-line
  trail is equalised away.
- **Flashed rect size:** tight for a small change (a marker nudge → a box around
  the marker), up to the full map viewport (`540 × up to ~896 px`, from
  `STATUS_H` down) when a pan/zoom changes everything. **Never** the full panel,
  and **never** the `540×64` status bar or anything above the map. Typical
  zoom/pan lands near the viewport; small pans/marker moves are much smaller.
- **How often:** once per interactive map burst, ~900 ms after the last tap
  (`ghostCleanPending` + `GHOST_CLEAN_SETTLE_MS`), same trigger as the previous
  settle-clean. **Not** per DU frame, **not** on idle, and **not** during
  hands-off riding (no tap ⇒ `ghostCleanPending` never set ⇒ no scan, no clean).

Versus v2: v2 flashed the entire 540×960 panel (incl. status bar + footer) via
`epd_fullclear` (which also runs a 3-cycle `epd_clear`) on every settle. v3
flashes only the changed sub-rect of the map, with a scoped GC16 clear + GL16
repaint — far less area, no 3-cycle full clear, and the static chrome never
moves.

## Riding (hands-off) — deliberately not flashed

As before, passive riding never schedules a clean (it would mean a flash while
moving). If the trail is ever reported specifically during untouched riding, the
minimal follow-up is to also accumulate + schedule on passive map DU; left out
here to honor "riding shouldn't flash" and to keep the change interaction-scoped.

## Verification

Builds clean: `pio run -e t5s3-pro` → SUCCESS (RAM 37.2%, Flash 18.3%). Not
flashed / not tested on glass — validate the rect size and the settle flash on
hardware. If a full pan/zoom's viewport-sized flash still feels heavy, the rect
is already the minimum that covers the change; the next lever is tightening the
accumulation (e.g. clip to the marker's swept path) rather than the viewport.

## Diff (v3 vs current `main` `src/ui_dashboard.cpp`)

Region-clear helpers added before `refresh()`; `refresh()` reordered so the dirty
rect is accumulated before `shadowFb` is overwritten; the map settle branch now
calls `regionClean()` instead of a full-screen GC16. No other behavior changed.
