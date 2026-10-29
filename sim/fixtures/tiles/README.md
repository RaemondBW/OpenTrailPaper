# Real map tiles for the sim tests

Sixteen H3 res-6 cells around downtown Hood River, Oregon (45.7054, -121.5215),
exactly as the apps download them from the prebuilt-tile CDN
(`https://tiles.opentrailpaper.com/v1/`, docs/prebuilt-tiles.md): the town's
street grid, I-84 and SR-35, the Columbia River across the north (water fill,
with the Washington shore), parks, and trails to the south and west. Every
cell within 9 km of the centre is here, so a map page anywhere within ~5 km of
the centre has all its neighbours. 375 KB of `.ebm` plus the cells' `.poi`
files.

| file | what |
|---|---|
| `<h3>.ebm` | the tile, byte for byte what the CDN serves and the apps send |
| `<h3>.poi` | its cycling POIs (cells that have any) |
| `manifest.json` | centre point and cell, bounding box, per cell sizes, CDN hashes and version records (`cdn:<hash>`, what the apps send as `tile_version`) and bbox; a 10 km west-to-east `track` through the centre with the cells it crosses; an `outside` point no tile covers; the OSM snapshot dates |
| `build_fixture.py` | rebuilds all of it (read-only GETs of the index, tile and POI URLs the apps use; ids from the apps' own H3 C code) |

Tests use it through `otpsim.tiles` (`fixture_tiles()`, `fixture_pois()`,
`fixture_manifest()`, `install_on_card()`), offline.

## Licence

Map data © OpenStreetMap contributors, available under the
[Open Database License (ODbL) 1.0](https://opendatacommons.org/licenses/odbl/1-0/)
(<https://www.openstreetmap.org/copyright>). The tiles are a Produced Work
derived from the OSM snapshot named in `manifest.json` (`osm_snapshot`);
elevation in the tiles is Copernicus GLO-90 (© DLR e.V. 2010-2014 and ©
Airbus Defence and Space GmbH 2014-2018, provided under COPERNICUS by the
European Union and ESA).
