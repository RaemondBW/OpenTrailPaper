# Map pipeline (v2 — planned)

Goal: offline vector maps on the SD card, rendered as crisp 1-bit lines on
the e-paper.

## Planned pipeline

1. Download an OSM extract for your region (e.g. from
   [Geofabrik](https://download.geofabrik.de/)).
2. A converter script (Python + pyosmium or osmium-tool) filters to what a
   bike computer needs: `highway=*` ways, waterways, rail, place names.
3. Ways are clipped into fixed-size tiles (e.g. 0.02° squares ≈ 2 km),
   coordinates delta-encoded as 16-bit offsets within the tile, classified
   into ~6 render classes (major road / minor road / path / water / rail /
   label) that map to line widths and dash styles.
4. Output: one binary file per zoom bucket with a tile index header, copied
   to `/maps/` on the SD card.

On device: the renderer loads the 3×3 tiles around the GPS fix into PSRAM,
projects lat/lon → screen with a simple equirectangular transform centered
on the rider, and draws with epdiy line primitives. Redraw only when the
rider moves > ~1/4 screen or rotates the map.

Prior art worth reading before building this:
[bike-computer-32](https://github.com/lspr98/bike-computer-32) ships a
similar converter (`map-converter/`) and on-device renderer for an
ESP32-C3 — the format and clipping approach can be adapted almost directly.
