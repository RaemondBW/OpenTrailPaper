# Vector fonts for the head unit

Branch `vector-font`. Status: **host prototype working; not wired into the
dashboard yet.**

## Why

Text on the device is drawn from epdiy 4-bit bitmap fonts pre-rendered from
two proprietary faces:

| role   | source        | faces | flash   |
|--------|---------------|-------|---------|
| values | Impact        | 9 (digit heights 158/120/95/81/69/58/46/30/15) | ~6.9 MB of headers |
| labels | Arial Bold    | 4     | ~0.35 MB |

`ui::valueFontIndex()` walks that ladder and picks the first face that fits
the cell, so a cell that could take a 105 px digit gets 95, and every new
size or weight is another `fontconvert.py` run and another chunk of flash.
The faces are also not redistributable, which matters now that the project
is public.

## What this is

Outlines instead of bitmaps, rasterised on the device at whatever size a cell
wants, from open fonts that look like what we have:

| role   | open font | licence | why |
|--------|-----------|---------|-----|
| values | **Anton** | OFL | Designed as an Impact re-cut; at equal digit height it is a near drop-in (a hair narrower). Single weight. |
| values | **Oswald** (variable, wght 200–700) | OFL | Same condensed-grotesque family as Impact but with a real weight axis; 600–700 reads close to Impact, 400 is a lighter option for secondary fields. |
| labels | **Arimo** (variable, wght 400–700) | OFL | Metric-compatible Arial clone (Liberation Sans lineage). 700 replaces Arial Bold; 400 is free. |

Sources and OFL texts are in `tools/vfont/src-fonts/`.

### Pipeline

```
TTF ──vfont_build.py──▶ src/fonts/vf/<name>.h (VfFont: quadratic outlines)
                                    │
             vf_face(font, px) ─────┴──▶ EpdFont (glyphs rasterised lazily)
                                              │
                    existing ui::text / textWidth / epdc_digit_height ...
```

* **`tools/vfont/vfont_build.py`** — fontTools. Instances variable fonts at a
  given `--wght` (so weight is a *build-time* choice, each one ~30 KB of
  header / ~10 KB of outline data), converts CFF cubics to quadratics, and
  emits the 96-glyph charset the bitmap fonts carry (ASCII + `·`). Points are
  `int16` pairs in font units with on-curve / contour-end flags in the low
  bits.
* **`src/vfont.cpp`** — rasteriser + face cache.
  * Rasteriser is the font-rs signed-area accumulation scheme: each edge
    deposits coverage deltas into a float buffer, a per-row prefix sum yields
    exact-area AA coverage, quantised to the 4-bit epdiy glyph layout. No edge
    lists, O(edge length + area). Host: 10 digits at 158 px in 0.5 ms; expect
    ~10–20 ms cold on the S3 (FPU, PSRAM scratch), then cached.
  * `vf_face(font, emPx)` / `vf_face_digit(font, digitPx)` /
    `vf_face_cap(font, capPx)` return a cached `const EpdFont*`. Sizes are
    quantised to ¼ px. Glyphs are rendered on first use into a per-face
    arena in PSRAM. 16-face LRU pool; `vf_cache_bytes()` for diag.
  * `vf_fit_digit(font, str, availW, availH, unitW)` binary-searches the
    largest digit height that fits — the replacement for the ladder walk.
* **`src/epd_compat.cpp`** — one-line hook: `findGlyph()` asks `vf_glyph()`
  first. That is the whole integration surface; every existing text call
  path works with a vector face because it *is* an `EpdFont`.

### Specimen

`tools/vfont/run.sh` builds a host binary that renders
`out/specimen_compare.png` (bitmap Impact vs Anton vs Oswald 600 at each
ladder height; Arial Bold vs Arimo) and `out/specimen_sizes.png` (weight ramp,
15→95 px in 10 px steps, `vf_fit_digit` in a 240×150 cell, small text
12–18 px). Check both before changing the rasteriser.

## Decisions / open questions

1. **Anton or Oswald for values?** Anton is the safe like-for-like swap.
   Oswald buys the weight axis (e.g. 700 for the primary field, 500 for
   secondary) at the cost of looking a little less like today's screen.
   Recommendation: ship Anton first, keep Oswald headers in the tree, decide
   on weight use once the dashboard is drawn with it.
2. **Threshold vs AA.** The bitmap path thresholds coverage at 8/15
   (`EPDC_TEXT_ANTIALIAS` off) because 1-bit reads crisper at 14–20 px on
   this panel. The vector glyphs carry full 4-bit coverage, so AA can be
   flipped on per call site later (labels stay crisp, 150 px digits get
   smooth diagonals). Panel greys 0x44+ read as white, so the AA ramp needs
   the 0x22–0x33 tones — see `panel-usable-greys`.
3. **Hinting.** None. At ≥ 30 px it doesn't matter; at 12–14 px stems land
   on half pixels and the threshold path makes them uneven. If labels look
   worse than Arial_B, options are: snap the scale so cap height is an
   integer (done by `vf_face_cap`), or keep the two small Arial bitmaps for
   labels and go vector for values only.
4. **Kerning** — ignored, same as the bitmap fonts. Digits are tabular in all
   three fonts anyway.
5. **Memory.** Worst case a face at 158 px with all 10 digits is ~65 KB;
   16 faces ≈ 1 MB of PSRAM if every slot were that big — in practice the
   dashboard uses 4–6 sizes. Scratch for one glyph raster is
   `(w+2)(h+2)·5` bytes, freed immediately.
6. **Weight at runtime** is *not* free-form: each weight is an instanced
   header. If a continuous axis is wanted later, the alternative is storing
   two masters and interpolating point-wise on device (Oswald's masters are
   compatible, so that is a small change to `vfont_build.py` + a lerp in
   `traceGlyph`).

## Next steps

- [ ] Device compile passes (`pio run -e t5s3-painter`); measure cold
      render time and `vf_cache_bytes()` on hardware via the console.
- [ ] Wire `ui::cell()` / `valueWithUnit()` to `vf_fit_digit` + `vf_face_digit`
      behind a build flag, render `tools/preview` and compare against main
      (check overlaps — `check-text-overlaps`).
- [ ] Decide Anton vs Oswald, pick label weight, drop the Impact/Arial
      headers (≈7 MB flash back).
- [ ] Emulator (`tools/preview`, web emulator) picks the change up for free
      since both compile `epd_compat.cpp` + `vfont.cpp`.
