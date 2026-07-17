# Map pipeline

Offline maps for the e-paper bike computer are compact vector tiles (`EBM1`
format, with a baked-in `ELV1` elevation grid) rendered as crisp 1-bit lines on
the panel. There are two ways to make them; both write the same format the
firmware reads from the SD card.

See the [main README](../README.md#map-tiles) for the format spec and the
on-device handling (`src/map_store.cpp`, `src/map_tiles.cpp`).

## 1. On the phone (primary)

The iOS app is the normal way to load maps: draw a box in the Route tab and it
downloads every [H3](https://h3geo.org/) res-6 hexagon covering it — fetching
OSM ways from Overpass and an elevation grid from Open-Meteo per hex, encoding
`EBM1`+`ELV1`, and streaming each tile to `/maps/tiles/<h3id>.ebm` over BLE.
Code: `companion-ios/Sources/MapBuilder.swift` + `H3Tiles.swift`.

## 2. On the desktop — `build_map.py` (region bake)

`build_map.py` bakes a whole bounding box into a single `EBM1` file for the
`/maps/*.ebm` fallback layer (drawn where no H3 tile covers the rider). It
fetches ways from Overpass (cached locally), classifies them into render
classes, simplifies geometry, clips into a square lat/lon grid, and writes the
binary.

```sh
python3 build_map.py --bbox 37.703 -122.525 37.835 -122.355 \
    --name sf --out ../../data/sf.ebm
```

Copy the result to `/maps/` on the SD card. `sf_overpass.json` is a cached
Overpass response for the San Francisco box so the build is reproducible
offline.

## 3. In the browser (region bake, no toolchain)

The project site's *Generate an offline map* section
([`docs/mapgen.js`](../docs/mapgen.js) + `docs/mapgen-ui.js`) is a JS port of
`build_map.py`: pick a bounding box on a Leaflet map, and it fetches Overpass
and encodes the same whole-region `EBM1` blob client-side, then downloads a
`<name>.ebm` to drop in `/maps/`. Output is byte-for-byte identical to
`build_map.py` for the same Overpass response, so the three paths stay in sync.
Like `build_map.py`, it has no `ELV1` elevation block — that's the phone's
per-hex path.

## Render classes

Both paths classify OSM tags identically (`build_map.py` and `MapBuilder.swift`
must agree):

| class | value | OSM |
|-------|-------|-----|
| 0 | major road | motorway, trunk, primary, secondary |
| 1 | minor road | tertiary, residential, unclassified, living_street, pedestrian |
| 2 | path | cycleway, footway, path, track, steps |
| 3 | rail | railway=rail / light_rail |
