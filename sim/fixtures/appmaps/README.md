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

## Attribution

Map data © OpenStreetMap contributors, available under the
[Open Database License (ODbL) 1.0](https://opendatacommons.org/licenses/odbl/1-0/)
(<https://www.openstreetmap.org/copyright>). The tiles are a derived database
of it (OSM snapshot 2026-10-08, built by OpenTrailPaper's tile pipeline,
`tools/tiles/`); the elevation inside them is Copernicus GLO-90 DEM
© DLR e.V. 2010-2014 and © Airbus Defence and Space GmbH 2014-2018,
provided under COPERNICUS by the European Union and ESA.
