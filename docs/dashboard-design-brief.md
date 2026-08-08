# Device dashboard — design brief and constraints

Constraints for redesigning the **configurable dashboard** on the OpenTrailPaper
head unit (e-paper bike computer). Everything below is measured from the
shipping firmware, not estimated.

The rider configures an ordered list of data fields; the firmware packs them
onto the panel. The packing works but the *typography and rhythm* need design
work. This document is the box you have to design inside.

---

## 1. The display

| | |
|---|---|
| Panel | 4.7" e-paper, 960×540 native, driven **540×960 portrait** |
| Colour | **1-bit black on off-white paper.** No colour, ever |
| Greys | Only `0x00`, `0x11`, `0x22`, `0x33` render. **`0x44` and lighter read as pure white** — mid-greys are not available |
| Refresh | ~50 ms for a full frame. Dashboard redraws at **1 Hz** |
| Animation | **None.** No transitions, no motion, no progressive reveal |
| Anti-aliasing | Effectively none at fast refresh — assume hard edges |
| Viewing | Outdoors, in sunlight, at arm's length, while moving, often through sunglasses, sometimes in rain or with gloves |

**Design implication:** hierarchy must come from *size, weight and position*
only. No colour, no tint, no shadow, and no more than four flat greys — of which
`0x22`/`0x33` are the only usable "soft" tones.

## 2. Type — four bitmap faces, no scaling

There is no vector text. These four fonts are compiled in; a size that is not
listed **does not exist**. Digit height is the `'0'` glyph's ink height, which
is the number that matters for fitting.

| Font | Ascender | Descender | Line box | **Digit ink** | `"88"` | `"1:47:12"` |
|---|---|---|---|---|---|---|
| ArialBold_14 | 27 | −7 | 34 | **21** | 32 | 100 |
| ArialBold_20 | 38 | −9 | 47 | **30** | 46 | 143 |
| Impact_40 | 85 | −18 | 103 | **68** | 88 | 214 |
| Impact_128 | 270 | −57 | 327 | **219** | 286 | 684 |

Notes that trip people up:

- **The line box is not the digit height.** Impact_40 measures 103 px
  ascender-to-descender around 68 px of actual digits. Fit against the digit
  ink, or you reject faces that fit comfortably.
- **Impact_128 `"1:47:12"` is 684 px wide on a 540 px panel.** Long values
  cannot use the hero face at all.
- Captions are drawn *tracked*: **+3 px per character** on top of the string
  width. `"MOVING TIME"` is much wider than it measures naively.
- Impact is digits-only in practice — it is a display face for numerals.
  Captions and units use ArialBold.

## 3. Fixed screen furniture

| Region | Height | Notes |
|---|---|---|
| Status bar | **64 px**, always present | clock, phone, sensor pips, battery |
| Turn banner | **138 px**, only while navigating | sits directly under the status bar |
| Dashboard area | the remainder: **896 px** normally, **758 px** while navigating | |

Panel width is **540 px**. Cells are full width or half (**270 px**), and the
current engine allows a maximum of **two columns** — three columns of Impact
digits is unreadable at this width.

## 4. What the rider configures

An ordered list, max **12 items**. Each item is:

- **field** — one of 15 (below)
- **size** — `small` | `medium` | `large` | `hero`
- **half** — shares its row with the *next* `half` item; alone, it spans

Size is a **height share, not a font**: rows divide the available height in
proportion (weights `small 2`, `medium 3`, `large 4`, `hero 8`), and the type is
then chosen to fit whatever the row got. This is what stops a layout of four
small fields leaving three-quarters of the panel empty.

**Fields:** speed · power (3 s) · power · heart rate · cadence · distance ·
ride time · moving time · climb · grade · altitude · battery · satellites ·
clock · route left

**Value shapes to design for** (real widths drive everything):

| Shape | Examples | Worst case |
|---|---|---|
| 2–3 digits | `88`, `156`, `247`, `918` | short |
| decimal | `54.8`, `4.2` | medium |
| clock | `14:25` | medium |
| duration | `1:47:12` | **long** — the case that breaks layouts |
| no data | `--` | short |

## 5. Dynamic behaviour (this is the hard part)

- A field whose **sensor is not paired is removed entirely** and the remaining
  fields re-pack to fill the panel. It returns to its configured position when
  the sensor connects. Applies to power, heart rate, cadence, and route-left.
- Grade/altitude/climb stay put even when they read `--`, because their source
  (the map elevation grid) flickers as the rider crosses tile boundaries.
- **Any reflow is a full 1 Hz repaint.** There is no partial update and no
  animation, so the panel simply *becomes* the new layout between frames.
- The same configured layout must therefore look deliberate with **1 to 12
  fields**, and at any mix of sizes.

## 6. Current implementation — what it does now

Per cell, top to bottom: **caption band → value → unit band**, with the caption
pinned to the cell top and the unit to the cell bottom.

- Padding `pad = clamp(cellHeight / 12, 6, 14)`
- Caption band = `pad + font ascender`; unit band = `pad + 20`
- Usable width = cell width − 20
- Value picks the largest of Impact_40 → ArialBold_20 → ArialBold_14 whose
  **digit ink + 4 ≤ value height** *and* **string width ≤ usable width**
- Caption uses ArialBold_20 only if the cell is ≥ 200 px and it fits, else
  ArialBold_14
- Unit is dropped when the cell is too short, then the caption is dropped
- Cells taller than **260 px** get a 260 px band centred inside them, so a cell
  that inherits half the panel does not strand its unit hundreds of pixels below
  its value
- Hero cells use Impact_128 with the caption above, plus an optional 7-segment
  FTP power-zone bar beneath
- 3 px black rules divide rows; 3 px vertical rule between paired cells

## 7. Known problems to solve

These are the reasons this brief exists. A good redesign addresses them.

1. **The size ladder is coarse.** Adjacent rows can differ by more than 2× in
   digit height (68 → 30) because a cell was a few pixels short, so the visual
   hierarchy does not match the *configured* hierarchy. A `large` and a `medium`
   row can end up with identical type, or invert.
2. **Captions and units drop at different thresholds**, so two cells side by
   side in the same row can disagree — one shows its unit, the other does not —
   and their value baselines no longer align.
3. **Hero values have no width fallback.** Impact_128 is used unconditionally;
   anything longer than ~3 digits overflows the panel. `1:47:12` as a hero is
   684 px on a 540 px screen.
4. **Sparse layouts look empty.** With two fields left after sensors drop out,
   the 260 px band centred in a 896 px panel leaves large voids with no
   structure.
5. **Units are inconsistently valuable.** `BPM` under a heart rate is noise;
   `KM` under a distance is not. Everything currently gets the same treatment.
6. **No vertical rhythm across rows.** Each row is sized independently, so
   caption baselines do not form a grid down the panel.
7. **Reflow is unsignalled.** A strap disconnecting silently rearranges the
   screen mid-ride, and the rider has no cue that the layout changed rather than
   the data.

## 8. What a proposal must satisfy

- Uses **only** the four fonts and four greys listed — or explicitly states
  which new font sizes it needs and why the trade (each embedded face costs
  flash and must be generated).
- Legible at arm's length, in sunlight, while moving. The primary number should
  be readable in a glance measured in **tenths of a second**.
- Every layout from **1 to 12 fields** looks intentional, including all-small
  and all-hero.
- Handles `1:47:12` and `--` in every cell size without overflow or collision.
- Degrades when a field is removed mid-ride without the result looking broken.
- No reliance on animation, colour, partial refresh, or greys beyond the four.
- Touch targets, if any are proposed, are **≥ 60 px**; the dashboard body
  currently ignores taps deliberately (stray glove contact used to change
  screens).

## 9. Deliverables that would be most useful

1. A **cell anatomy** spec: caption/value/unit placement as a function of cell
   height, with the exact thresholds at which each element drops.
2. A **type ramp**: which face at which cell height, and the rule for choosing
   it, including the long-value case.
3. **Row rhythm** rules — how baselines and rules align down the panel.
4. Worked examples at 1, 2, 4, 6 and 9 fields, in portrait 540×960, in pure
   black on white.
5. A rule for the **sparse case** (2 fields on a full panel) that does not look
   like a rendering failure.

---

Reference renders of the current state are in `tools/preview/out/`:
`dashboard.png` (default), `dashboard_custom.png` (6 fields, mixed sizes),
`dashboard_no_sensors.png` (same layout with nothing paired),
`dashboard_nav.png` (turn banner taking the top 138 px). These are produced by
the firmware's own rasteriser, so they are pixel-accurate to the panel.
