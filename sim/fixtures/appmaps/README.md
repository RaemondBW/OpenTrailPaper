# Map fixture for the app sim tests

Seven real pre-built tiles (H3 res 6) around Woodstock, Connecticut
(41.9697, -72.0397), fetched once from `https://tiles.opentrailpaper.com/v1/`
by [`sim/cdn/fetch_fixture.py`](../../cdn/fetch_fixture.py) and served to the
apps by the stand-in CDN, [`sim/cdn/tile_cdn.py`](../../cdn/tile_cdn.py)
(see [IOS.md](../../IOS.md), [ANDROID.md](../../ANDROID.md)).

- `v1/` mirrors the CDN's layout: `meta.json`, and `862a33/index.json` with
  the `.ebm` / `.poi` files. The index and `meta.json` are cut down to these
  cells and their fragments. Only one cell has a `.poi`; the rest have none
  (`poiSize` 0, which the apps fill in with an empty `.poi`).
- `area.json` gives the centre, the box (centre ±0.04° lat, ±0.05° lon) and
  every cell the apps' H3 cover gives for that box, with its bounding box.
  Any selection inside the box gets only these hexes, so the apps never fall
  back to Overpass for it.

## Cycling POIs (`pois/`)

The real area has one POI, so the POI tests use made-up ones (not OSM data):
`pois/<set>/` holds `overpass.json`, what Overpass would answer the apps' POI
query with for the whole area, and each cell's `.poi` as the reference
writer (`docs/mapgen.js` buildPoi) builds it from that, with each POI in the
one H3 cell containing it. [`sim/cdn/build_poi_fixture.py`](../../cdn/build_poi_fixture.py)
writes them (node + cc).

- `woodstock`: eleven elements — drinking water (one 150 m north of the
  centre), toilets (one a building with a centre, seasonal), a repair stand,
  bike shops, a water tap and a fountain, one customers-only water point —
  plus a private toilet and a non-drinking tap that no builder keeps. Five
  cells get POIs, two none.
- `changed`: the same with one cell edited upstream (a new repair stand, the
  old one's pump gone).
- `none`: no POIs anywhere.

The stand-in CDN serves a set on request (`/_ctl/pois?set=<name>`): its
non-empty files in place of the fixture's `.poi`, with their sizes and
hashes in the index (the empty ones as `poiSize` 0), and its elements as
Overpass's answer to the POI query.

## Attribution

Map data © OpenStreetMap contributors, available under the
[Open Database License (ODbL) 1.0](https://opendatacommons.org/licenses/odbl/1-0/)
(<https://www.openstreetmap.org/copyright>). The tiles are a derived database
of it (OSM snapshot 2026-10-08, built by OpenTrailPaper's tile pipeline,
`tools/tiles/`); the elevation inside them is Copernicus GLO-90 DEM
© DLR e.V. 2010-2014 and © Airbus Defence and Space GmbH 2014-2018,
provided under COPERNICUS by the European Union and ESA.
