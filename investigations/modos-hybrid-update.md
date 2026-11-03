# Replicating the Modos update on the T5S3

Experiment, started 2026-11-01. Goal: less ghosting without full refreshes, and
without giving up refresh rate. Code is in `tools/epd_hybrid_bench/`; nothing in
`src/` changes yet. **Runs on the panel; timing is measured below. Whether it
actually reduces ghosting has not been judged yet**: that takes eyes on the glass.

## What Modos actually does

Caster, the FPGA controller, differs from a normal controller in three ways
([Glider README](https://github.com/Modos-Labs/Glider)):

1. **Every pixel is its own update region.** It has its own timer, so updating
   one pixel never waits for another. Costs 16 bits of state per pixel, read and
   written every frame, which is why the board has DDR3.
2. **Early cancellation.** A pixel that changes mid-drive is sent to its new
   target straight away.
3. **Hybrid mode.** Changed pixels update in fast binary; a pixel left alone for a
   while is re-rendered in greyscale. Per pixel, automatically.

The README does not describe a ghosting mechanism as such. The inference here is
that (3) is what stands in for a full refresh: the pixels that moved are the ones
owed a clean landing, so only they get one.

## Where we stand

EPD_Painter is already a software relative of this: a delta engine, its own
tuned trains, DC-balanced transitions and a continuous row scan. What it lacks is
all of the above, because `paint()` is one global batch. Everything changed in a
frame runs one train from pass 0, and the next frame waits for it. We also clean
with a *rectangular* scrub (`epdc_clear_dirty`), which is the region-based
thinking per-pixel control replaces.

`src/smooth_epd.cpp` was the earlier attempt at this and is dead: it is built on
epdiy's LCD primitives. The two archived ghosting notes both name this as the
endgame.

## The idea: cohorts instead of per-pixel timers

A pixel only starts moving when a frame is submitted, so all the pixels of one
submission share a timer. A few overlapping **cohorts**, each a 2bpp slot plane
and a pass counter, behave like per-pixel timers. The driver's `convert_or` kernel
already merges several planes into one row transmission, and a chunk mask means an
idle cohort costs nothing.

| Modos | Here | Cost |
|---|---|---|
| Per-pixel timer | Up to 8 cohorts | 130 KB PSRAM each, sparse reads |
| Early cancel | Run the elapsed passes backwards, polarity swapped | One extra cohort per cancelled batch |
| Hybrid settle | Three ageing generations of "changed" bitplanes; survivors get a net-zero train | 3 planes |

Limits, compared with the real thing:

- **Binary only.** Levels 2-3 render black, 0-1 white. Caster's greyscale half is
  not here. The firmware's grey levels would need the direct trains, which take
  more than one sweep per cohort.
- **A frame that arrives with every cohort busy is refused**, and the newest frame
  is retried next pass. Caster has no such limit.
- **A pixel in a reversal or a settle is not cancelled**; it waits for that train
  to finish (a few tens of ms) and then takes the newest frame.
- **Submitting stretches a pass.** Discovery runs between passes, so a big change
  during a drive makes that pass over-long and over-doses it. Saturated black and
  white should tolerate this. The bench reports overruns.

## Open risks, in the order to check them

1. **Settle may crosstalk on dither.** `investigations/display-ghosting.md` §1
   found that driving adjacent dither pixels in opposite directions washes the
   tone out to grey. The settle train does exactly that where a dithered area
   moved: black pixels whiten while white neighbours darken. Check `pan` first. If
   it washes out, split the settle into two phases so the two polarities never run
   together.
2. **Settle may not clean anything.** The same notes found charge-neutral (1,2)
   pairs erase nothing, and that what works is k darkens *then* whitens. The train
   has that shape, with k=3, but k has never been tuned.
3. **Pass period.** The pure trains here are plain runs at a constant period, not
   tuned tables. 10 ms approximates 4-level NORMAL (~125 ms over 13 passes), and
   the row loop is about 5.6 ms by the driver's own numbers. Whether 8 passes
   saturates needs looking at on the glass.
4. **Visible settle flicker.** A digit that has just stopped changing will blink.
   That may or may not read better than the ghost it removes.

## What has been verified

`tools/epd_hybrid_bench/run_core_test.sh` runs the core against a model of the
glass, under ASan and UBSan. Across 20 scenarios, including a reversal at every
possible pass and a 400-frame random soak, every pixel lands on the right rail
with an exact DC ledger, no pixel is ever in two cohorts, and settle drives no
pixel that did not change. The model is idealised (one unit per pass, linear): it
proves the bookkeeping, not the optics.

## Measured on the panel (2026-11-01)

`video <mode> 15` plays a synthetic 230-frame clip at a requested 15 Hz, one
section per failure mode, holding a card between sections. Seconds taken for each
section (lower is better, `want` is on schedule):

| Section | want | shipping (NORMAL) | fast (FAST) | hybrid | nosettle |
|---|---|---|---|---|---|
| ball | 3.0 | 5.6 | 3.0 | 3.0 | 3.0 |
| text | 3.0 | 6.0 | 3.0 | 6.4 | 6.0 |
| pan | 3.0 | 5.6 | 3.0 | 3.2 | 3.0 |
| plasma | 3.0 | 5.7 | 3.0 | 3.7 | 3.4 |
| strobe | 2.0 | 3.7 | 2.0 | 2.6 | 2.3 |
| cuts | 1.3 | 2.6 | 1.4 | 2.9 | 2.8 |

Hybrid's numbers are from the final build; `nosettle`, `fast` and `shipping` are
from the build before the debug probes came out, which differs only in those.

- **Plain FAST beats hybrid on throughput.** Hybrid is ahead of shipping NORMAL
  everywhere except `text` and `cuts`, but nowhere ahead of FAST. It can only earn
  its place on ghosting and on cancellation, which is why the glass matters.
- **Submit is the bottleneck.** 50-110 ms for a big frame against a 10 ms pass. The
  likely cause, by estimate and not yet profiled, is memory bandwidth: about ten
  plane traversals of 130 KB through PSRAM per frame. `post()` waits on the same
  lock on the other core, which probably explains `text` taking twice as long. If
  so, fewer planes per frame is the fix: drop the `last_`
  copy, age by chunk instead of three per-pixel generations, keep one in-flight
  plane instead of reading every cohort.
- **Refused frames are rare**, and peak cohorts stayed at 6 of 8. The pool is not
  the limit.
- **Floats fire on nearly every frame of the heavy sections.** That keeps the dose
  right, but it is why the overrun counts are high: those are the floats.
- `pan` at full resolution, single-pixel dither: 8 Hz held, 6.0 s against 6.0 s.

## On the glass (2026-11-01, one filmed `quick` run)

Judged from a 360x640 phone recording with the contrast boosted, so this is a first
read and not a measurement. The dark rounded shape low in the dither half of every
card is a reflection of the camera side of the room, not a ghost. `nosettle` was
not filmed.

| After | shipping | fast | hybrid |
|---|---|---|---|
| ball | disc left behind | disc and a smeared trail | disc, weaker on white |
| pan | lines and coast, still there after plasma | faint lines, still there after plasma | little or none |
| strobe | clean | clean | **strong rectangles, 2 s+** |
| cuts | checker in the dither half | **checker over the whole card** | clean |

- **Hybrid is cleanest after large changes.** That may come from its longer drive
  (8 passes at 10 ms, about 80 ms, against an estimated 40 ms for FAST) rather than
  from cohorts. A longer FAST train would show which.
- **Dither held up.** The 50% half stays dark in hybrid where `fast` lands greyer.
  That is 2x2 dither, so risk 1 is not closed for single-pixel dither.
- **Hybrid fails `strobe`, the section it exists for.** The likely cause, not yet
  tested, is that reversal assumes the ink is symmetric: running the elapsed passes
  backwards is exact in the model, but the driver's tuning notes record whitens as
  much stronger than darkens, so a reversed pixel would not land where it began.
  The DC ledger balances; the optics do not.
- **Settle shows as a brief flash of the old pattern**, and after `cuts` the card
  already looked clean before it ran, so it has not yet shown any benefit. It also
  did not visibly remove the strobe rectangles.

### What is making it clean? (`compare`, one filmed run)

Two new modes separate the explanations. `long` is the existing driver at NORMAL's
13 passes with no gap (about 73 ms). `batch` is this engine with no overlap, no
cancellation and no settle: one frame at a time, 8 passes at 10 ms.

| After | fast | long | batch | hybrid |
|---|---|---|---|---|
| pan | lines, coast, grey dither | **strong lines and coast** | faint lines and coast | faint lines and coast |
| strobe | rectangles | clean | clean | **strong rectangles** |
| cuts | checker over the whole card | checker in the dither half | clean | clean |

- **Drive length alone does not explain it.** `long` drives about as long as `batch`
  and still leaves the checker. Shipping NORMAL (about 125 ms) left it too.
- **Cohorts add no visible ghosting benefit.** `batch` matches `hybrid` on `pan` and
  `cuts`.
- **Cancellation causes the strobe ghost.** The only difference between `batch` and
  `hybrid` here is overlap and reversal, and `batch` is clean. That supports the
  asymmetric-ink explanation without proving it.
- **`batch` is flattered somewhat.** It skipped frames (10-14 of 16 shown), so the
  panel took fewer transitions than under `long`, which showed all 16.
- **Why this scan loop beats the driver on the same content is unknown.** Both drive
  plain runs on black and white. Candidates are the 10 ms pass period together with
  the 8-pass train, and the frames skipped. A FAST train paced to 10 ms a pass in the
  driver would test it, and would be a one-line driver change if it holds.

### Is it the pass period? (`paced`, one filmed run, the clearest of the three)

`paced` is the driver's own FAST at a constant 10 ms a pass (`tools/epd_hybrid_patch.py`
adds two lines for it). `strict` is the same 7 passes at 10 ms in this bench's scan
loop. Both draw every frame.

| After | fast | paced | strict | batch |
|---|---|---|---|---|
| pan | coast clear, grey dither | coast fainter, dark dither | coast fainter | coast faint |
| strobe | **strong rectangles** | one faint patch | faint patches | faint patches |
| cuts | **checker over the whole card** | faint checker in the dither half | clean | clean |

- **The pass period is most of the gain.** The same FAST train goes from the worst
  mode to close to the best by adding 10 ms pacing, at about 13 fps with every frame
  drawn (1.17-1.20 s for 15 frames, against 1.41-2.27 s for `strict`).
- **Skipped frames were not flattering `batch` much.** `strict` looks about the same.
- **`strict` still edges `paced` after `cuts`.** It spends up to about 150 ms a frame
  against 80 ms, so the ink rests between inversions, which may be why. Not tested.

**Decision: stop work on the cohort engine.** It adds complexity, causes the worst
strobe ghost, and shows no visible gain over a two-line driver change.

Not yet known about `paced`: how FAST's grey levels land at 10 ms (they were tuned
with no gap, and this clip is black and white), how it behaves on single-pixel dither
(`run pan paced`) and on real dashboards, and how it compares to shipping NORMAL in
the same run. Shipping was only filmed in the first video.

**The core 0 idle watchdog, solved.** Resets under sustained load came from the bench,
not the drive. The board file sets `ARDUINO_RUNNING_CORE=0`, so `loop()` shared core 0
with the scan task, and when a run fell behind it stopped sleeping. Every yield then
went to `loop()` and idle never ran: a probe showed 1 yield in 11 reached idle. The bench
now runs `loop()` on core 1, as the firmware does with its UI. Timings above predate
that, when drawing and scanning shared a core, so the engine's may be pessimistic.

### What is left after `paced`, and `topup`

Watching `paced` at 12 Hz, the remaining ghosts were the ball's path, the corners of
the digits, and where the lines cross the solid block. All three look like one
mechanism: only changed pixels are driven, so a pixel that stays black beside freshly
driven ones has a different history and shows. The same applies to `settle`, which
cleaned the pixels that moved and not these.

`topup` (`Params::topUp`) gives unchanged pixels in a changed chunk a few passes
towards the rail they are already on. It runs on the bench engine, and is **not
DC-balanced**: those pixels end up with extra charge each time (11 against 8 in the
host test), the same kind of imbalance that built up as a grey cast in
`investigations/display-ghosting.md` §4a. Short runs cannot show that. It has not been
judged on the glass yet.

### Why does `strict` still beat `paced`?

Judged by eye: `strict` is the best looking, and stays better than `paced` when both
run the clip at 7 Hz. Both send the same 7-pass train at 10 ms a pass, so this is
unexplained. Ruled out so far:

- **Rest between frames.** Same frame rate, still different.
- **`setQuality()` or `clear()` side effects.** `setQuality()` only swaps tables, and
  neither touches clocks or voltages. Both modes bind the same level 3 train.
- **Longer row charging.** Measured row loop, ms, at 7 Hz:

| | ball | pan | plasma | cuts |
|---|---|---|---|---|
| paced | 5.6 | 5.8 | 5.5 | 4.8 |
| strict | 5.0 | 5.0 | 5.0 | 6.0 |

  `paced` is the last pass of the run and `strict` a mean, but `strict` is shorter in
  three of four, so this is not it.

Still open: `strict`'s first pass of every frame overruns, because discovery counts
against the period. Its first pass therefore gets about 5 ms and not 10, so the drive
is about 65 ms against `paced`'s 70 ms, and shaped differently. Setting `paced` to
9.5 ms tests the total. The trains actually installed have also not been printed from
the running driver, so a tuned NVS table has not been ruled out.

### Erase priming

Digits still ghost slightly when black goes to white. `Params::eraseK` applies what the
driver's tuned removes found: k darkens first, then the usual whitens plus k. Net charge
is unchanged (host-tested), and the pixel is already black while it darkens. Each pass of
priming adds 20 ms to a frame, and the `erase` knob sets it.

Judged by eye on `digits`: **2 passes is perfect.** Best settings so far: `strict`, 7
passes, 10 ms a pass, erase priming 2, which is about 110 ms a frame on the engine.
Judged on digits only, and in black and white. Before this goes into `src/`: rerun with
the core 0 idle watchdog on, test greys, and try it on real screens with BLE and GPS
running.

## In the firmware

`src/epd_scan.*` and `src/scan_core.h` carry the settled result into the real firmware,
behind a setting that is off by default: `panelscan on|off` on the console, saved to NVS
and applied at boot. It is not the bench engine. There are no cohorts, overlap,
cancellation or settle: one frame at a time, newest wins, 10 ms a pass. Priming 2 was
perfect on the bench's digits but real screens still ghosted. Priming 4 helped that, and
blacks stopped looking as black: the same 7 darkens were following a longer whiten. So
draw and erase now move together: 9 passes to draw, priming 3, 15 passes to erase,
balanced as before. `panelscan tune <draw> <prime>` changes them live, unsaved.

- **State:** it uses the driver's own screenbuffer and takes the driver's paint lock for
  each frame, so `clear()` and the dirty-rect scrubs work unchanged, and it can be switched
  between frames with no handover.
- **Greys:** the UI only uses them for static art, and the changing parts are 1-bit tone.
  Any transition involving a grey takes the driver's tuned NORMAL and direct trains in
  the same drive. Those were tuned at about 9.6 ms a pass, so this runs them at 10 ms.
- **Tests:** `tools/scan_test/run.sh` checks that all 16 transitions send each pixel exactly
  its train. On the board, with BLE, GPS and the UI live: 58 frames of flipping every
  screen and zooming the map, 0 overruns, slowest pass 7.4 ms, slowest discovery 25 ms, no
  crash, setting kept across a reboot, and switching on and off mid-run.

Ghosts of the boot logo, and of the map on a greyed-out sensor cell, turned out to be a
wiring bug: `epdc_clear_dirty()` still erased through the driver's plain trains, so the
transitions that most need the primed erase never got it, and the driver then recorded
those pixels as white. Scrubs now erase through the scan and keep the driver's kick.
The boot screen also gets a full clear at handover.

That left the map showing on a greyed-out cell. The cell is a 25% dot tone, so its dots
sample whatever was underneath pixel by pixel, and a dot on an ex-black pixel lands
differently from one on an always-white pixel. The scrub's kick is too short to level
that, so when toned cells arrive they alone get the driver's HARD clear
(`epdc_clear_rects`, charge-balanced, about 200 ms), which flashes those cells.

Settings toggles then showed inside the start button after backing out: an unscrubbed
transition, and the old pixels-that-stay-black problem again. So on the scan path every
change of screen now scrubs, about 330 ms before drawing. Slowest pass has reached 9.96 ms
of the 10 ms budget on these screens, so that margin is the next thing to watch.

Not yet checked: how greys look on the glass, the shutdown screen, how it looks on real
dashboards and the map, battery draw against the driver's path, and a long ride.

## Map speed

The panel is not what limits the map. Measured: 860 features take 360 ms to draw, 2,550
take 680 ms, and 11,250 take 1,370 ms, against 100-150 ms on the panel, all single
threaded on core 1. And the map redraws every second whether or not anything moved.

`src/map_base_cache.h` keeps finished base layers (fills and roads) in PSRAM: the view on
screen, so the 1 Hz redraw stops rebuilding it, and one zoom either side, rendered in the
UI task's idle time once the view has held for 3 s, cut short by a touch. Views within 2 px
and 1.5 degrees count as the same, or a stationary GPS would never hit. `mapcache` on the
console reports it and turns it off.

On the board that first took draw from 360-1,370 ms to about 75 ms but sent project from
about 100 ms to 1.8-2.6 s: prerendering the neighbours pushed this view's tiles out of RAM,
and every frame went back to the card for tiles it no longer needed. A hit now skips the
tiles altogether, with each cached layer keeping its own POIs.

Measured on the board, parked, zooming from 2 to 16 m/px and back with 9 s between taps:
54 hits, 2 misses, 7 prerendered, 0 cut short, and every zoom cost about 1 ms of project
and 70-85 ms of draw. Not yet measured: quick repeat taps, or a moving position.

Zooming far out left old roads in the water, whose dots stay black between zooms. A full
scrub fixed it but took the whole map through white, so a zoom now scrubs only the water:
the renderer's mask, kept per cached layer as a bit per framebuffer byte, goes to
`epdc_scrub_bits()`. Roads and land change in place, and parks are not included.

Zooming out then stalled, and the power-off map only drew the middle tile. Both were
one regression: the tile cache sizes itself from free PSRAM, and the panel scan and the
first map layers left it at its 512 KB floor, so a 2 MB wide view streamed from the card
at 5 s a frame, past the 4 s power-off limit. Map layers are now 1 bit per pixel, there
are 3 of them, the cache's reserve went from 2.5 MB to 1.1 MB (OTA still releases the
cache before staging), and the power-off frame gets 12 s.

The power-off map still did not fill the screen, and that was not time: `farewell` on the
console showed the frame finishing with 14 of its 23 tiles, because the other 9 are not
on the card. It covers 16 m/px fully and 32 m/px only in part. The frame now takes the
widest zoom the card can fill, per `map_store::coversView()`.

`maptest` on the console checks rotation: a frame from scratch and one from its cached
layer must match byte for byte at each heading, 1 degree must reuse the layer and 3 must
not.

The firmware sim caught two things the bench could not: a cached frame reported zero
roads and tiles, and a layer outlived a tile or POI changing on the card. Each layer now
keeps what its tile pass found, and `map_store::generation()` is part of its key.

## Tuning from a web page

```bash
/Users/raemond/.espressif/python_env/idf5.4_py3.13_env/bin/python tools/epd_hybrid_bench/tuner.py
```

Then open http://localhost:8765. It sets the knobs on the board, runs the chosen tests,
shows timing, and keeps a rated list of runs in the browser. It needs the bench
firmware, and it holds the serial port, so stop it before uploading or using
`console.py`.

## Running it

```bash
pio run -e epd-hybrid-bench -t upload
```

Then on the console (115200), by hand or with
`tools/epd_hybrid_bench/console.py <log> <quiet seconds> <command>...`:

```
video all 15
video hybrid 15 strobe
run trail shipping
run trail hybrid
run pan hybrid
set k 5
set settle 800
run digits nosettle
```

Each run clears the panel, plays the scene and leaves its last frame up. `trail`
ends on its thin lines, with white between them where solid black has just been,
which makes it the direct ghost test. In `video`, each section is followed by a 2.5 s card, white
on one half and 50% dither on the other: whatever shows on it is left over from
that section. Compare `shipping`, `fast`, `nosettle` and `hybrid`. `set` changes
`fast` (passes), `k`, `settle` (ms) and `period` (us).

The bench answers `bootloader`, so the next upload stays hands-free. To go back,
upload the normal firmware from a normal checkout.

## If it works

Fix the watchdog question and make submit cheaper first. Then move the engine into
`src/epd_compat.cpp` behind `epdc_paint()` (the interface
already suits it: the UI hands over whole frames), retire the
`epdc_clear_dirty()` scrub, then add greys using the direct trains so a settle
goes binary to true level, which is the rest of Caster's hybrid mode.
