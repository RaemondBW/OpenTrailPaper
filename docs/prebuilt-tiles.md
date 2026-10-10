# Pre-built map tiles

The companion apps used to build every map tile on the phone from live
Overpass queries plus Open-Meteo elevation. The public Overpass servers turn
most of those queries away, so a 15-hex download took 5–10 minutes, and
Open-Meteo's daily quota left tiles without elevation. A GitHub Actions
workflow now builds the tiles for the **whole planet** every week and
publishes them to Cloudflare R2. The apps (and the website's generator)
download ready-made tiles first. They fall back to the on-phone Overpass
build only for hexes that are not published.

| | Overpass on the phone | pre-built (CDN) |
|---|---|---|
| 15 hexes around Providence, RI (measured) | **333 s**: 8 of 18 map requests refused, all 6 coastline requests failed, so no sea fill | **0.03 s** from a local server; over a phone link it is bandwidth-bound, ~1.7 MB |
| elevation | Open-Meteo, ~250 hexes/day per IP, then tiles ship without it | always (Copernicus GLO-90) |
| data age | live | at most ~8 days |

Contents: [How it works](#how-it-works) · [Versions](#versions) ·
[Setup](#setup-one-time) · [Verifying](#verifying) · [Costs](#costs) ·
[Freshness](#freshness) ·
[Running locally](#running-locally) · [Tests](#tests) · [Limits](#known-limits)

## How it works

### What is published

Everything is under a version prefix, so a future format can live next to
this one (`v2/…`) while old apps keep reading `v1/`.

```
https://tiles.opentrailpaper.com/v1/
  <g>/index.json             which cells of group g exist (g = first 6 chars of the H3 id)
  <g>/<h3>.ebm               the tile: the exact bytes the apps write for a one-hex download
  <g>/<h3>.poi               cycling POIs, only for cells that have any
  regions/<region>.json      per-region manifest (cell -> sizes + hashes), and <region>.border.json
  meta.json                  run time, OSM snapshot per region, cell counts
```

The first 6 characters of a res-6 H3 id are exactly its **res-3 ancestor**
(resolution, base cell, digits 1–3). So a group directory holds at most 343
tiles of one ~12 000 km² area, and a selection on the phone almost always
needs only one or two `index.json` files. They are the cheap global "does
this hex exist" lookup: ~30 KB each, ~12 KB gzipped.

```json
{"v":1,"cells":{"862a33157ffffff":[91021,"4271255cafb108cc",612,"9be0c4d1a2f3e4b5"], ...},
 "regions":["us_rhode-island","us_rhode-island.border"]}
```

Each cell maps to `[ebmSize, ebmHash, poiSize, poiHash]`:

- **Cell missing from the index**, or the group has no `index.json`: not
  built. The apps use Overpass for it.
- **`ebmSize` 0**: built, and there is nothing to draw (open sea). The apps
  do not ask Overpass. They handle it like an Overpass build that came out
  empty.
- **`poiSize` 0**: no POIs. The apps write the empty `.poi`, as they do when
  Overpass returns none.
- **The hash** is sent as `?v=<hash>`, so a changed tile gets a new URL at
  the CDN. The apps also check that the body length equals the index size and
  that it starts with `EBM2` / `EPOI`. Anything else falls back to Overpass
  for that hex. The apps also record the hash of everything they send, so
  they never re-download a tile that has not changed
  ([Versions](#versions)).
- **`regions`** lists the fragments (the `meta.json` `fragments` keys) whose
  cells are in the group. With `meta.json` it gives the OSM date of the
  group's data, without fetching the large region manifests. It changes only
  when a group's owners change, so the weekly OSM date does not turn every
  `index.json` into an upload.

## Versions

Each hex's content hash is in its group's `index.json`. The apps record which
version each device holds and compare it with that hash. So they show an
update only when the published data really changed, and they never send a
hex the device already has in its current version.

### What is recorded

When the device acknowledges that it saved a file, the app records a
**version record** for that hex, separately for the tile and the `.poi`:

| record | meaning |
|---|---|
| `cdn:<hash>` | the CDN file with that index hash (`ebmHash` / `poiHash`). A cell without POIs has `poiHash` `""`; the app writes its empty `.poi` itself and records it as `cdn:` |
| `phone:<unix secs>` | built on the phone from Overpass at that time (the fallback) |

The records are kept **per device**, under the same key as the device's
map-layer settings: the peripheral UUID on iOS, the Bluetooth address on
Android.

- **iOS:** `Application Support/device-tile-versions.json`
  (`DeviceTileVersions` in `TileVersions.swift`).
- **Android:** `filesDir/device-tile-versions.tsv`
  (`DeviceTileVersions` in `map/TileVersions.kt`).

When the device's tile or POI list arrives, records for hexes it no longer
lists are dropped. A copy that later arrives from elsewhere is then not
mistaken for this phone's. The older per-phone tracking from the bike-route
work (`tileSentAt`, `flaggedTileIds`, `poiSentAt`) is kept for the fallback
heuristic.

The phone's own caches (`TileCache`, `PoiCache`) store the same record in a
`<id>.ver` file next to each blob. When the index has the hex, only a cached
copy with exactly the current hash is reused, at any age. A stale or
phone-built copy is fetched again. Without an index entry, the old rule
applies: any copy younger than 90 days for tiles, 30 days for POIs, and none
on a Redownload.

### Comparison rules

`PrebuiltTiles.state(record, cdnHash)` is the same in both apps:

| index entry for the hex | device record | state |
|---|---|---|
| has the hash `h` | `cdn:h` | **current** |
| has the hash `h` | `cdn:<other>`, `phone:…`, or none (a tile from the website ZIP or another phone) | **update** |
| none: CDN unreachable, hex not pre-built, or tile built empty (`ebmSize` 0) | anything | **unknown** → old heuristic |

- **Tiles** compare `ebmHash`; **POIs** compare `poiHash`.
- **The heuristic** applies only to *unknown* hexes. A tile counts as an
  update if it was made before bike routes, or this phone sent it over 90
  days ago. POIs count if this phone sent them over 30 days ago.
- **Index cache.** The Maps screen looks up the indexes of the device's hexes
  on screen and of the selection (`CdnVersions`, at most 64 groups per look).
  They come from the same one-hour index cache the downloads use.
- **CDN unreachable.** The screen knows nothing for 5 minutes and falls back
  to the heuristic. The map colours and legend are unchanged; only what feeds
  them is new.

### What the buttons do

| | current hexes | changed hexes (update) | hexes the CDN does not have |
|---|---|---|---|
| **Download** (new hexes) | – (already on the device) | – | Overpass, as before |
| **Send POIs** | skipped | fetched and sent | Overpass, as before |
| **Redownload** | skipped | fetched and sent | rebuilt from Overpass, as before |

- **Redownload** is labelled with the number of hexes it would send. When
  every selected hex is current, tapping it says *"All N hexes are up to
  date"* and offers **Re-send anyway**, which sends all N for repairing a
  card. A cached copy with the current hash is the same bytes, so it is used
  instead of a download.
- **The download re-checks.** It looks up the hashes again before fetching,
  so a stale screen cannot make it re-send a current hex.
- **The selected-area card** shows the data date, e.g. *"Map data from
  Oct 5"*: the oldest OSM timestamp in `meta.json` of the fragments named by
  the selection's indexes. Under the counts it shows *"N hexes have newer
  data"*, plus the heuristic reason for any unknown updates.

### The builder (`tools/tiles/`)

The tiles must be byte-identical to what the apps build, or the published set
would drift from the phones and the website. So the builder **reuses the
apps' own logic** instead of re-implementing it:

- **Roads and POIs** come from `docs/mapgen.js`. It is the reference that
  `MapBuilder.swift` and `MapBuilder.kt` already match byte for byte
  (`tools/map_test/run_crossport.sh`).
- **ELV1 / WTR2 / PRK2** come from `apptile.mjs`, a line-by-line port of the
  apps' `appendElevation` / `appendWater` / `appendParks` / `isEmpty`,
  including Swift's rounding.
- **H3 math** comes from `h3tool`, built from the apps' vendored H3 C
  (`companion-ios/Sources/H3`). The cell bbox feeds header bytes, and h3-js
  rounds the last bit differently.

What feeds those builders is **the Overpass JSON a phone would have
received**, answered from an OSM extract (`overpass.mjs`). The extract goes
through osmium in two steps:

- `osmium tags-filter` keeps a superset of what the queries match, plus
  every member way of a `route=bicycle` relation.
- `osmium add-locations-to-ways` writes ways with their coordinates as OPL,
  which `osmstore.mjs` streams into typed arrays.

`overpass.mjs` then applies the queries' exact predicates and Overpass's
bbox semantics (a node inside, or a segment crossing). It also applies the
route levels (`icn`/`ncn` 3, `rcn` 2, other 1, highest wins) and the output
order. Per cell it answers:

- the map query over the cell bbox ± 0.003° (`MapBuilder.query`);
- the coastline fetch over that box ± 0.35° (`MapsView.download`);
- the POIs in the cell (`poiQuery`, assigned by H3 cell).

*Why osmium, not a Node PBF reader.*
- osmium-tool is a C++ streaming filter, about 1 s per 100 MB of PBF.
- It resolves way node locations, and it is one `apt install` on the runner.
- A pure-JS PBF decoder would need its own node-location index, which
  osmium already is, and it would run 10–20× slower on the 78 GB planet.

*Elevation.*
- Copernicus GLO-90 from the AWS open-data bucket (`dem.mjs`). It is the
  dataset Open-Meteo serves.
- Each point is sampled the way Open-Meteo answers: the post whose cell holds
  the point, rounded to whole metres.
- A check against the live API gave **300 of 300** grid points identical, over
  Manhattan, Lake Neuchâtel and San Francisco.

*Open sea.* A cell with no way, no coastline within 0.35° and no DEM would
come out of the app as a tile holding only a zero elevation grid. The builder
publishes it as built-and-empty (`ebmSize` 0) instead. Geofabrik polygons
reach far offshore, so this matters: the plan covers 7.5 M cells, of which
millions are ocean.

### Regions, ownership and borders

The planet is built from **Geofabrik extracts**. `plan.mjs` starts from the
continents and splits any extract over 1.5 GB, or over 600 k cells, into its
subregions, recursively. It skips Geofabrik's overlapping aggregates
(`us-west`, `dach`, `alps`, …). Today that is **434 regions, 77.5 GB of PBF,
7.5 M cells**.

Who builds a cell is decided deterministically, so every job agrees without
talking to the others (`regions.mjs`):

- **owner**: the first region, in `plan.json` order, whose polygon contains
  the cell's centre.
- **interior**: the owner's polygon also contains the cell's whole fetch box.
  Geofabrik cuts with complete ways, so every way the query returns is in
  that one extract, and the owner builds the cell alone.
- **deferred**: interior, but the 0.35° coastline box reaches into another
  extract, and the owner either sees coastline there or sees nothing at all.
  The apps assemble sea rings from every coastline way in that box, so a
  neighbour's coast changes the bytes.
- **border**: everything else, roughly 5–15 % of a country's cells. Each
  region job also writes a **strip** (`strip.mjs`): the ways, coastline and
  POIs of its extract that border and deferred cells (its own and its
  neighbours') need. The border phase merges the strips that reach a cell
  (the highest OSM version wins, route levels are combined) and builds it
  from that.

### The workflow (`.github/workflows/prebuilt-tiles.yml`)

It runs **only when started by hand** (*Actions* → *Pre-built map tiles* → *Run workflow*), and only for the repository owner: the `plan` job checks `github.actor` / `github.triggering_actor`, so a run started by anyone else (even a collaborator with write access) stops before downloading or uploading anything. Forks cannot trigger it or read its secrets. Inputs:

- `only`: a subset of Geofabrik ids;
- `jobs`;
- `resume_run`: earlier run ids, newest first, comma-separated (a resume of a resume lists both). Redoes only the jobs whose newest result failed (plus border jobs that read a rebuilt interior job), with today's code, the newest run's plan, and each reused job's strips from the run that built them — so it must start within 2 days of that run, while the strips are kept. The merge is partial: everything else stays as published (`tools/tiles/resume.mjs`);
- `dry_run`.

Every step is `tools/tiles/pipeline.sh`, which also runs locally.

```
plan ──► interior × 20 ──► border × 20 ──► merge
         download extract,  strips of this   index.json for changed groups,
         build, upload      job + neighbour  meta.json, delete what is gone
         changed tiles,     jobs; build the
         write strip        border cells
```

- **Only changed objects are uploaded.** Each fragment records every cell's
  hashes. A job fetches last week's fragment and uploads only tiles whose hash
  changed.
- **No listing, no per-object HEAD.** Uploads use
  `rclone copy --files-from --no-traverse --no-check-dest`, because each
  listing or HEAD is a billed operation.
- **Deletions are global**, worked out by `merge` from the old and new
  fragments, because a cell can change owner.
- **Failures are contained.** A border job refuses to build from an
  incomplete set of strips. A region whose job failed keeps last week's
  fragment and tiles, so a bad week never unpublishes a country.
- **Headers per object:**

  | Object | `Cache-Control` | `Content-Type` |
  |---|---|---|
  | `.ebm` / `.poi` | `public, max-age=604800` (the URL carries `?v=`) | `application/octet-stream` |
  | `index.json` | `public, max-age=3600` | `application/json` |
  | `meta.json`, `regions/*` | `public, max-age=300` | `application/json` |

- **Geofabrik etiquette.** A planet run downloads every extract once,
  ~78 GB from download.geofabrik.de, with a descriptive User-Agent. Weekly is
  considerate; do not re-run the full planet back to back (use `only` for
  tests).
- **Sizing.** A job holds runs of regions in Geofabrik URL order, so
  neighbours share a job, balanced on estimated time. One region peaks at a
  1.5 GB extract: ~6–8 GB RAM and under 10 GB disk, with the DEM cache capped
  at 5 GB. That fits the standard runner's 16 GB and 14 GB.

## Setup (one time)

Nothing below has been done yet. The apps already point at
`https://tiles.opentrailpaper.com/v1/`. Until that name exists they get a DNS
failure on the first index request, switch the CDN off for the rest of that
download, and build from Overpass exactly as before.

1. **Create the bucket.**
   - Cloudflare dashboard → *R2 Object Storage*. R2 needs a payment method
     on file, even inside the free tier.
   - *Create bucket*. Name: `opentrailpaper-tiles`. Location: Automatic.
     Storage class: Standard.
2. **Public custom domain.**
   - Bucket → *Settings* → *Custom Domains* → *Connect Domain* →
     `tiles.opentrailpaper.com`.
   - The `opentrailpaper.com` zone is already on Cloudflare (it serves
     `sync.opentrailpaper.com`), so Cloudflare adds the DNS record and the
     certificate itself. Wait for the status to read *Active*.
   - *Alternative for a trial:* enable the *Public Development URL*
     (`https://pub-<id>.r2.dev`). It is rate-limited and not meant for
     production. If you use it, set the apps' base URL to
     `https://pub-<id>.r2.dev/v1/` (step 8).
3. **CORS**, for the website's generator. Bucket → *Settings* → *CORS
   Policy* → *Add CORS policy*:
   ```json
   [{
     "AllowedOrigins": ["https://opentrailpaper.com", "https://www.opentrailpaper.com", "http://localhost:8000"],
     "AllowedMethods": ["GET", "HEAD"],
     "AllowedHeaders": ["*"],
     "MaxAgeSeconds": 86400
   }]
   ```
4. **Caching.**
   - Cloudflare caches only known file extensions by default, and `.ebm` /
     `.poi` are not among them.
   - Zone `opentrailpaper.com` → *Caching* → *Cache Rules* → *Create rule*:
     - **When**: Hostname equals `tiles.opentrailpaper.com`.
     - **Then**: *Eligible for cache*.
     - **Edge TTL**: *Use cache-control header if present*.
     - **Cache key**: leave the query string **included** (the default). The
       `?v=<hash>` is what makes a changed tile a new cache entry.
5. **Compression** (optional).
   - JSON is compressed by default. `.ebm` is `application/octet-stream` and
     is not.
   - *Rules* → *Compression Rules* → *Create rule*: Hostname equals
     `tiles.opentrailpaper.com` → *Enable Brotli and Gzip compression*. That
     covers all content types, and gzip saves ~21 % on tiles.
   - Both apps' HTTP stacks decode gzip transparently. The size check uses
     the decoded body.
5b. **Who may read** (zone → *Security* → *Security rules*).
   - **Custom rule "Tiles: apps and website only"**, action *Block*:
     ```
     (http.host eq "tiles.opentrailpaper.com"
      and not starts_with(http.user_agent, "OpenTrailPaper/")
      and not any(http.request.headers["origin"][*] in {"https://opentrailpaper.com" "https://www.opentrailpaper.com" "https://raemondbw.github.io" "http://localhost:8000"})
      and not starts_with(http.referer, "https://opentrailpaper.com/") and not starts_with(http.referer, "https://www.opentrailpaper.com/")
      and not starts_with(http.referer, "https://raemondbw.github.io/") and not starts_with(http.referer, "http://localhost:8000/"))
     ```
     The apps send `OpenTrailPaper/<version> (iOS|Android)`; the website's
     `fetch` sends its Origin. Anything else gets 403. It keeps crawlers,
     hotlinkers and casual scripts off; a deliberately spoofed User-Agent
     still gets through (the data is ODbL anyway — this is about cost).
     The workflow uploads through R2's S3 API, not this hostname, so it is
     unaffected.
   - **Rate limiting rule "Tiles: per-IP rate limit"**: hostname equals
     `tiles.opentrailpaper.com`, per IP, 600 requests / 10 s → block 10 s
     (the free plan's only window). A large app download stays well below.
   - **Errors are never cached**: the cache rule's Edge TTL has a status-code
     TTL of *No store* for ≥ 400, so a hex published after someone asked for
     it is not stuck behind a cached 404.

6. **API token.**
   - *R2* → *Manage R2 API Tokens* (or *Account API tokens*) → *Create API
     token*.
   - Permission **Object Read & Write**, applied to **specific bucket:
     `opentrailpaper-tiles`** only. TTL: forever (or set a reminder).
   - Copy the **Access Key ID** and **Secret Access Key**, which are shown
     once. The **Account ID** is on the R2 overview page, and is also the
     `<id>` in `https://<id>.r2.cloudflarestorage.com`.
   - The workflow never lists or creates buckets, so no admin permission is
     needed (`no_check_bucket` is set).
7. **GitHub secrets.** Repo → *Settings* → *Secrets and variables* →
   *Actions* → *New repository secret*, four times:

   | Secret | Value |
   |---|---|
   | `R2_ACCOUNT_ID` | the Cloudflare account id |
   | `R2_ACCESS_KEY_ID` | from step 6 |
   | `R2_SECRET_ACCESS_KEY` | from step 6 |
   | `R2_BUCKET` | `opentrailpaper-tiles` |

   Without them the workflow still runs, as a dry run that uploads nothing.
8. **App base URL** (only if not using `tiles.opentrailpaper.com`).
   - **iOS:** `OTP_TILE_BASE_URL` in `companion-ios/project.yml`. It flows
     into `Info.plist` as `OTPTileBaseURL`. Run xcodegen afterwards.
   - **Android:** `TILE_BASE_URL` in `companion-android/app/build.gradle.kts`,
     or `tiles.url=` in `local.properties`, or env `OTP_TILE_BASE_URL` at
     build time.
   - An empty value switches the CDN off. Always keep the trailing `/v1/`.
9. **First run.**
   - The workflow must be on `main` for the *Run workflow* button and the
     schedule to exist.
   - Actions → *Pre-built map tiles* → *Run workflow* with `only` =
     `monaco`. It is tiny: one job, about two minutes. Then check it as in
     [Verifying](#verifying).
   - Then *Run workflow* with `only` empty, for the planet. The first planet
     run uploads everything (~6–8 M objects, see [Costs](#costs)) and takes
     ~1.5–2 h end to end.
   - After that, re-run it whenever you want fresher data. Only changed tiles are uploaded, so a re-run costs a fraction of the first one (see [Costs](#costs)).

## Verifying

```sh
B=https://tiles.opentrailpaper.com/v1
alias curl='curl -A "OpenTrailPaper/verify"'   # the firewall rule blocks other agents
curl -s $B/meta.json | head -20                 # merged time, per-region OSM timestamps
H=862a33157ffffff; G=${H:0:6}                   # downtown Providence, RI (any hex you know)
curl -s $B/$G/index.json | python3 -m json.tool | grep -A1 $H
V=$(curl -s $B/$G/index.json | python3 -c "import json,sys;print(json.load(sys.stdin)['cells']['$H'][1])")
curl -sI "$B/$G/$H.ebm?v=$V" | grep -iE 'HTTP/|content-length|cache-control|cf-cache-status'
curl -sI "$B/$G/$H.ebm?v=$V" | grep -i cf-cache-status        # second time: HIT
curl -s "$B/$G/$H.ebm?v=$V" | head -c 4; echo                  # EBM2
curl -sI -H 'Origin: https://opentrailpaper.com' $B/$G/index.json | grep -i access-control-allow-origin
```

In the apps:
- Download a few hexes. The download summary shows where each hex came from,
  with the CDN's request counts and timings next to Overpass's:
  - **iOS** (Xcode console): `hexes by source: cdn 15, cdn poi 15, …`
    plus `cdn-index:` / `cdn:` request lines.
  - **Android** (logcat tag `Maps`): `hexes: 15 cdn tiles, 15 cdn pois, …`
    plus `cdn-index:` / `cdn-tile:` / `cdn-poi:` lines and `cdn wall:`.
- On the website, the generator's log says
  `N tiles from the pre-built set`. Add `?tiles=off` to the page URL to
  compare against the Overpass path.

## Costs

R2 has no egress charge. Prices are the R2 list prices, as of 2026:

- storage $0.015/GB-month (10 GB free);
- Class A (writes) $4.50/M (1 M/month free);
- Class B (reads) $0.36/M (10 M/month free).

| | estimate | cost |
|---|---|---|
| **Storage** | 7.5 M cells. Measured tile averages range from 87 KB (Switzerland) and 55 KB (Connecticut) down to 6.8 KB (Wyoming) and 1–3 KB (Greenland, Iceland: mostly sea and ice). Extrapolated **~20–40 GB** | **≈ $0.20–0.50 / month** |
| **First upload** | ~5–6 M `.ebm` + ~1.5 M `.poi` + ~25 k `index.json` ≈ **6–8 M PUTs** | **≈ $25–35 once** |
| **Weekly updates** | Only changed tiles are uploaded. One week of real Geofabrik diffs touched at most **47 %** of Switzerland's cells and **2.4 %** of Wyoming's; that is an upper bound, since edits to tags the tiles do not use change nothing. Weighted by where cells are: ~0.5–0.9 M PUTs a week, ~2–4 M a month | **≈ $5–14 / month** |
| **Reads** | Mostly served from Cloudflare's cache. A miss costs one Class B. Even 1 M hex downloads a month stays inside the free 10 M | **≈ $0** |
| **Actions** | 20 interior jobs, ~20–45 min each (download, build, DEM, upload); 20 border jobs, ~10–15 min; merge, ~5 min. **≈ 1 000–1 500 runner-minutes a week** | **$0**: standard runners are free for public repositories. A private repo would be ~5 000 min/month, ~$25 over the free 2 000 |

Re-running monthly rather than weekly costs about a third as much: busy areas change the same cells over and over, so a month has far fewer than four weeks' worth of PUTs, ≈ $2–5 per run.

## Freshness

- Geofabrik refreshes its extracts daily. The workflow builds when you run it, so
  published data is as old as your last run (plus up to a day for Geofabrik). `meta.json` records each
  region's OSM timestamp.
- The apps keep a downloaded tile for 90 days (`TileCache`), as before. A
  pre-built hex is cached by hash: its copy is reused while the hash is
  current, at any age.
- *Redownload* fetches only hexes whose published hash differs from the one
  the device has ([Versions](#versions)). For hexes that are not published it
  rebuilds from Overpass, as before.
- The CDN holds a tile for at most a week, but a changed tile has a new
  `?v=` URL, so the apps always get the version their `index.json` names. An
  `index.json` itself is cached for an hour.
- Elevation never changes. Roads, water and POIs follow OSM with that one
  week of lag, which riders do not notice for roads and is acceptable for
  water points.

## Running locally

Needs Node ≥ 20, `osmium-tool` (`brew install osmium-tool`) and a C compiler.

```sh
npm ci --prefix tools/tiles
# One extract, interior cells only, into ./out (bbox ownership, no planner):
node tools/tiles/build_region.mjs --region test --bbox 41.1,-71.9,42.05,-71.1 \
     --pbf rhode-island-latest.osm.pbf --out out
# The whole pipeline for a few regions into a local "bucket":
export LOCAL_BUCKET=$PWD/bucket
tools/tiles/pipeline.sh plan work --only us/rhode-island,us/connecticut --jobs 2
for j in j01 j02; do tools/tiles/pipeline.sh interior work $j; done
for j in j01 j02; do tools/tiles/pipeline.sh border work $j; done
tools/tiles/pipeline.sh merge work
python3 -m http.server 8000 --directory bucket
#   apps: base URL http://<this machine>:8000/v1/ (Android also needs a
#         cleartext exception for a plain-http host)
# The website needs the tiles on its own origin (http.server sends no CORS):
#   ln -s $PWD/bucket/v1 docs/v1 && python3 -m http.server 8001 --directory docs
#   open http://localhost:8001/?tiles=/v1/    (remove the docs/v1 link afterwards)
```

The Connecticut + Rhode Island run above takes ~70 s for the interior phase,
downloads included, then 8 s for the border phase. It publishes 516 tiles,
231 `.poi` files and 5 `index.json`. An immediate second run uploads no
tiles.

## Tests

- **Unit tests:** `npm test --prefix tools/tiles`. They cover the fast
  coastline join against `mapgen.assembleCoastline`, merge and deletion
  rules, and strip merging.
- **Builder against the apps' own code:** `tools/tiles/test/equivalence.sh <pbf> <s,w,n,e>`.
  - From the **same extract**, it writes the raw Overpass JSON a phone would
    receive for each one-hex download, plus the elevation grid.
  - It runs the iOS app's `MapBuilder.swift` / `H3Tiles.swift`, compiled for
    the host, on that JSON, as `MapsView` does.
  - It runs `docs/mapgen.js` on the same JSON, and compares all three.
  - Results: San Francisco 12/12, Rhode Island 157/157 (coast and sea fill),
    Bern–Zurich 229/229 (lakes, parks, Alps). Every `.ebm`, `.poi` and road
    section is identical.
- **Border phase:** `tools/tiles/test/border_equivalence.sh`. It builds two
  neighbouring extracts the planet way and compares every cell with a build
  from the two merged into one file. Connecticut + Rhode Island: 516/516
  identical, including the 226 border and deferred cells.
- **App CDN path:**
  - iOS: `tools/tiles/test/cdn_ios/run.sh <bucket>` (host-compiled
    `PrebuiltTiles.swift`).
  - Android: `PrebuiltTilesTest`. Set `OTP_TILE_FIXTURE=<bucket>` to also run
    it against real output.
  - Both check byte-identity, fallback for missing hexes and groups,
    rejection of truncated or bad-magic tiles, and a fast fallback when the
    host does not resolve.
- **Versions:**
  - iOS: `tools/tiles/test/cdn_ios/run.sh --versions`. It needs no bucket:
    it serves two generated states of one group.
  - Android: `PrebuiltTilesTest`.
  - Both check that a hex with the same hash is skipped and never requested,
    a changed hash is re-sent, a phone-built hex is an update, and an
    unreachable CDN leaves every hex to the heuristic.
  - They also check the hash-keyed tile cache, the per-device record store,
    the index cache across downloads, and the data date from `meta.json`.

## Known limits

- **Byte-identity holds against an app build of the same snapshot.** A phone
  fetching live Overpass sees newer data, and a phone whose coastline fetch
  failed has no sea fill. Both were behind 14 of 15 Providence tiles
  differing from the phone's own build in the timing run.
- **Multi-hex selections.** On the phone, the sea rings of a multi-hex
  selection are assembled over the selection's union box. The published
  tiles match a one-hex download. Where a long coastline crosses that union
  box, ring vertex order can differ. The filled area is the same.
- **Floating point.** The `ELV1` header stores the cell bbox as doubles from
  the platform's libm. Linux (the runners) and Apple libm can differ in the
  last bit on rare cells, which has no visible effect.
- **Unpublished cells.** Cells whose centre is in no extract polygon (open
  ocean far from land) and cells of a region whose jobs failed for weeks are
  simply not published. The apps build those from Overpass.
- **DEM downloads.** The DEM (~1 GB per 100 k high-latitude cells) is
  downloaded fresh every run. If that ever matters, publish each region's
  elevation grids once and reuse them; the DEM does not change.
