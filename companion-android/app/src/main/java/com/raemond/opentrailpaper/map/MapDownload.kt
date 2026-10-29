package com.raemond.opentrailpaper.map

import com.raemond.opentrailpaper.ble.BleManager
import com.raemond.opentrailpaper.data.BoundingBox
import com.raemond.opentrailpaper.data.LatLon
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitAll
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Semaphore
import kotlinx.coroutines.sync.withPermit
import kotlinx.coroutines.withContext
import java.io.ByteArrayOutputStream
import kotlin.math.floor
import kotlin.math.max
import kotlin.math.min

/** What a running map download reports to the Maps screen. */
class MapDownloadProgress(
    val status: (String?) -> Unit,
    /** Hexes this run will fetch and send, once the CDN index has been read. */
    val total: (Int) -> Unit,
    /** Tiles produced (cached, CDN or built) and queued for the device. */
    val built: (List<String>) -> Unit,
    /** Tiles that could not be produced (no map data). */
    val failed: (List<String>) -> Unit,
)

/**
 * One map download, as the Maps screen's Download / Send POIs / Redownload
 * buttons run it (MapsSheet.startDownload): the CDN index first, then
 * [missing0]'s tiles and [poiWork0]'s `.poi` files from the cache, the
 * pre-built CDN ([baseUrl], MapEndpoints) or Overpass, streamed to the device
 * through [ble] as they are produced.
 *
 * [redownload]: [missing0] is on the device already; only hexes whose CDN hash
 * differs from [tileRecords] (or that the CDN does not have) are sent, unless
 * [force]. [poiRecords] / [devicePois]: what the device holds, for the same
 * filter on POIs. Returns the status to show when it ends (null: nothing to
 * say); a cancellation stops the tile stream and is rethrown.
 *
 * Runs its POI pass as a child of the receiver, so a failed map pass still lets
 * the POIs already under way finish sending.
 */
suspend fun CoroutineScope.runMapDownload(
    ble: BleManager,
    missing0: List<MapTile>,
    poiWork0: List<MapTile>,
    redownload: Boolean,
    force: Boolean,
    tileRecords: Map<String, String>,
    poiRecords: Map<String, String>,
    devicePois: Set<String>,
    progress: MapDownloadProgress,
    baseUrl: String = MapEndpoints.tileBaseUrl,
): String? {
    DownloadStats.reset()
    // Ready-made tiles first (one instance: the map and POI passes share
    // the group indexes); Overpass only for what it does not have.
    val cdn = PrebuiltTiles(baseUrl)
    // The CDN's current hashes. A hex whose device record already has
    // them is skipped (unless forced): nothing is fetched or sent.
    val entries = cdn.entries((missing0 + poiWork0).map { it.id }.distinct())
    val tileHashes = entries.mapNotNull { (id, e) -> PrebuiltTiles.tileHash(e)?.let { id to it } }.toMap()
    val poiHashes = entries.mapNotNull { (id, e) -> PrebuiltTiles.poiHash(e)?.let { id to it } }.toMap()
    var missing = missing0
    var poiWork = poiWork0
    if (redownload && !force) {
        val need = PrebuiltTiles.needsSend(missing0.map { it.id }, tileRecords, tileHashes).toSet()
        missing = missing0.filter { it.id in need }
    }
    if (!force) {
        val need = PrebuiltTiles.needsSend(poiWork0.map { it.id }, poiRecords, poiHashes).toSet()
        poiWork = poiWork0.filter { it.id !in devicePois || it.id in need }
    }
    if (missing.isEmpty() && poiWork.isEmpty()) {
        return if (redownload) {
            "All ${missing0.size} hex${if (missing0.size == 1) " is" else "es are"} up to date."
        } else {
            "Everything selected is up to date."
        }
    }
    progress.total(missing.size + poiWork.size)
    if (redownload) progress.status("Fetching map data…")
    var converted = emptySet<String>()
    var failedHexes = emptySet<String>()
    ble.startTileStream(resend = redownload)   // begin sending as tiles are produced
    try {
        // Cycling POIs: their own small query and cache, started NOW so
        // they arrive while the map batches are still building. A failure
        // here never fails the map download — the map is what matters.
        val poisJob = async {
            if (poiWork.isEmpty()) return@async true
            try {
                downloadPois(
                    poiWork, cdn, poiHashes, allowAged = !redownload, onStatus = {},
                    enqueue = { files, versions -> ble.enqueuePois(files, versions) },
                )
                true
            } catch (e: CancellationException) {
                throw e
            } catch (_: Exception) {
                false
            }
        }
        if (missing.isNotEmpty()) {
            downloadTiles(
                missing = missing,
                cdn = cdn,
                hashes = tileHashes,
                allowAged = !redownload,
                onStatus = progress.status,
                onBuilt = { ids ->
                    converted = converted + ids
                    progress.built(ids)
                },
                onFailed = { ids ->
                    failedHexes = failedHexes + ids
                    progress.failed(ids)
                },
                enqueue = { tiles, versions -> ble.enqueueTiles(tiles, versions) },
            )
        }
        val poiNote = if (poisJob.await()) null
                      else "Cycling POIs could not be fetched — try again later."
        DownloadStats.log(missing.size + poiWork.size)
        ble.finishTileStream()                     // let the queue drain
        // The cache has a size ceiling and this is the only thing that
        // grows it, so this is where it gets enforced.
        TileCache.trim()
        return when {
            failedHexes.isNotEmpty() ->
                "${failedHexes.size} hex${if (failedHexes.size == 1) "" else "es"} had " +
                    "no map data — tap Select area and retry them."

            missing.isEmpty() -> poiNote
            converted.isEmpty() -> "No roads found in that area."
            else -> poiNote
        }
    } catch (e: CancellationException) {
        ble.cancelTileUpload()
        throw e
    } catch (e: Exception) {
        ble.finishTileStream()                     // send whatever built first
        return e.message ?: "Map download failed"
    }
}

// MARK: the pipeline

/**
 * Fetch, build and stream every missing tile.
 *
 * Download, vectorization and the BLE send all run in parallel: a batch is
 * enqueued the moment it is built, so the link is busy while the next Overpass
 * request is in flight.
 */
private suspend fun downloadTiles(
    missing: List<MapTile>,
    cdn: PrebuiltTiles,
    hashes: Map<String, String>,
    allowAged: Boolean,
    onStatus: (String?) -> Unit,
    onBuilt: (List<String>) -> Unit,
    onFailed: (List<String>) -> Unit,
    enqueue: (List<Pair<String, ByteArray>>, Map<String, String>) -> Unit,
) {
    // Anything built before goes straight out — no Overpass, no elevation fetch,
    // no re-encoding. This is what makes a retry after a dropped link cheap
    // instead of a full rebuild. A hex the CDN has ([hashes]) is reused only as
    // a copy of its current hash (any age, also on a Redownload: the same
    // bytes); one it does not have only within the cache's age, and not on a
    // Redownload ([allowAged] false).
    val part = TileCache.partition(missing.map { it.id }, hashes, allowAged)
    val cached = part.tiles
    if (cached.isNotEmpty()) {
        onStatus("Reusing ${cached.size} cached tile${if (cached.size == 1) "" else "s"}…")
        onBuilt(cached.map { it.first })
        enqueue(cached, part.versions)
    }
    val cachedIds = cached.map { it.first }.toSet()
    DownloadStats.count("cached tiles", cached.size)

    // Pre-built tiles from the CDN (docs/prebuilt-tiles.md): the same bytes this
    // phone would build, so they are cached, marked and sent exactly like built
    // ones. Hexes it has not built (or a tile that fails its size/magic check)
    // fall through to Overpass below; hexes it built EMPTY do not — Overpass
    // would only drop them as empty again.
    val toServe = missing.filter { it.id !in cachedIds }
    var servedIds = emptySet<String>()
    if (toServe.isNotEmpty() && cdn.enabled) {
        onStatus("Downloading ready-made tiles…")
        val t0 = System.nanoTime()
        val r = cdn.fetchTiles(toServe.map { it.id })
        DownloadStats.add("cdn", (System.nanoTime() - t0) / 1e9)
        DownloadStats.count("cdn tiles", r.tiles.size)
        DownloadStats.count("cdn empty", r.empty.size)
        if (r.tiles.isNotEmpty()) {
            val versions = r.tiles.mapNotNull { (id, _) -> hashes[id]?.let { id to PrebuiltTiles.cdnRecord(it) } }.toMap()
            TileCache.store(r.tiles, versions)
            onBuilt(r.tiles.map { it.first })
            enqueue(r.tiles, versions)
        }
        if (r.empty.isNotEmpty()) onFailed(r.empty)
        servedIds = (r.tiles.map { it.first } + r.empty).toSet()
    }

    // Group tiles into bounded OSM fetches (~0.08° ≈ 9 km) so each Overpass query
    // stays light — big queries 504 on the busy public servers.
    val overpass = missing.filter { it.id !in cachedIds && it.id !in servedIds }
    DownloadStats.count("overpass tiles", overpass.size)
    val batches = overpass
        .groupBy { t ->
            val clat = (t.south + t.north) / 2
            val clon = (t.west + t.east) / 2
            "${floor(clat / 0.08).toInt()}_${floor(clon / 0.08).toInt()}"
        }
        .values.toList()
    if (batches.isEmpty()) return

    // ONE coastline fetch for the whole selection, padded well past it. Sea fill
    // needs the COAST, and an ocean-only selection does not contain any — the
    // batch bbox for hexes out in open water has no coastline in it, so no rings
    // could be assembled and those tiles came out blank. Padding by ~0.35° (~35 km)
    // reaches the shore from anywhere a rider would sensibly select, and is cheap
    // because it is coastline-only. It runs ALONGSIDE the first map batches; each
    // batch waits for it only after its own fetch, when it needs the sea rings.
    val all = union(overpass)
    val pad = 0.35
    coroutineScope {
        val seaRingsAsync = async {
            val coastChains = runCatching {
                val coastOsm = MapBuilder.fetchCoastline(
                    all.south - pad, all.west - pad, all.north + pad, all.east + pad,
                )
                MapBuilder.coastlineChains(coastOsm)
            }.getOrDefault(emptyList())
            // Rings are assembled against the PADDED region, not each batch's
            // bbox, so a batch sitting entirely offshore is still inside a ring.
            withContext(Dispatchers.Default) {
                MapBuilder.regionSeaPolygons(
                    coastChains,
                    all.south - pad, all.west - pad, all.north + pad, all.east + pad,
                )
            }
        }

        // Batches run MapBuilder.CONCURRENT_BATCHES at a time, each starting on
        // its own mirror; each is cached and queued for sending as soon as it is
        // built, so the link stays busy while the next fetch is in flight.
        val gate = Semaphore(MapBuilder.CONCURRENT_BATCHES)
        var done = 0
        onStatus("Fetching ${batches.size} area${if (batches.size == 1) "" else "s"}…")
        batches.mapIndexed { i, batch ->
            launch {
                gate.withPermit {
                    val produced = buildBatch(batch, i, seaRingsAsync)
                    done += 1
                    onStatus("Built $done of ${batches.size} area${if (batches.size == 1) "" else "s"}…")
                    // Mark ONLY what was actually produced. Marking every id in
                    // the batch would fill the map in as downloaded and report
                    // success while nothing had been sent or stored.
                    val producedIds = produced.map { it.first }.toSet()
                    onBuilt(producedIds.toList())
                    val missed = batch.map { it.id }.filter { it !in producedIds }
                    if (missed.isNotEmpty()) onFailed(missed)
                    // Cache BEFORE sending: if the link drops mid-transfer the
                    // expensive work survives and the retry is instant.
                    TileCache.store(produced)                 // phone-built
                    enqueue(produced, emptyMap())
                }
            }
        }
    }
}

/**
 * Fetch, encode and finish one batch of hexes: roads (+ way flags), DEM
 * elevation (a few hexes at a time), water and sea fill, parks. Returns the
 * non-empty tiles.
 */
private suspend fun buildBatch(
    batch: List<MapTile>,
    index: Int,
    seaRingsAsync: kotlinx.coroutines.Deferred<List<List<DoubleArray>>>,
): List<Pair<String, ByteArray>> {
    val u = union(batch)
    val osm = MapBuilder.fetchOsm(u.south, u.west, u.north, u.east, startMirror = index)
    val t0 = System.nanoTime()
    val encoded = withContext(Dispatchers.Default) { MapBuilder.encodeTiles(osm, batch) }
    // natural=water and park polygons for this region, resolved once and
    // appended per tile as WTR2 / PRK2 sections (after any ELV1 block).
    val waterWays = withContext(Dispatchers.Default) { MapBuilder.waterWays(osm) }
    val parkWays = withContext(Dispatchers.Default) { MapBuilder.parkWays(osm) }
    DownloadStats.add("encode", (System.nanoTime() - t0) / 1e9)
    val seaRings = seaRingsAsync.await()
    // Bake a DEM elevation grid into each tile (best-effort) so the device has
    // elevation without GPS altitude or the phone.
    val elevGate = Semaphore(MapBuilder.CONCURRENT_ELEVATION)
    val grids = coroutineScope {
        batch.map { t ->
            async {
                elevGate.withPermit {
                    t.id to runCatching {
                        MapBuilder.fetchElevationGrid(t.south, t.west, t.north, t.east)
                    }.getOrNull()
                }
            }
        }.awaitAll().toMap()
    }
    val t1 = System.nanoTime()
    val produced = ArrayList<Pair<String, ByteArray>>(encoded.size)
    withContext(Dispatchers.Default) {
        for ((id, roads) in encoded) {
            val tile = batch.first { it.id == id }
            val out = ByteArrayOutputStream(roads.size + 4096)
            out.write(roads)
            grids[id]?.let { grid ->
                MapBuilder.appendElevation(
                    out, tile.south, tile.west, tile.north, tile.east, grid, MapBuilder.ELEVATION_GRID,
                )
            }
            MapBuilder.appendWater(out, waterWays, seaRings, tile.south, tile.west, tile.north, tile.east)
            MapBuilder.appendParks(out, parkWays, tile.south, tile.west, tile.north, tile.east)
            val data = out.toByteArray()
            // Decide emptiness only now, with water/parks/sea/elevation already
            // appended — a hex can be pure water and still be worth storing.
            if (MapBuilder.isEmpty(data, tile)) continue
            produced.add(id to data)
        }
    }
    DownloadStats.add("encode", (System.nanoTime() - t1) / 1e9)
    return produced
}

/**
 * Build (or reuse) and queue the `.poi` files for [work]. One POI query per
 * ~0.25° group of tiles: the query is light, so groups can be far bigger than
 * the map batches. Each POI is stored in the one H3 cell containing it.
 */
private suspend fun downloadPois(
    work: List<MapTile>,
    cdn: PrebuiltTiles,
    hashes: Map<String, String>,
    allowAged: Boolean,
    onStatus: (String?) -> Unit,
    enqueue: (List<Pair<String, ByteArray>>, Map<String, String>) -> Unit,
) {
    // Cached copies: of the CDN's current hash at any age; others only within
    // MAX_AGE_MS and not on a Redownload (fresh data by definition).
    val part = PoiCache.partition(work.map { it.id }, hashes, allowAged)
    val missingIds = part.missing
    if (part.tiles.isNotEmpty()) enqueue(part.tiles, part.versions)
    var need = missingIds.toSet()
    // Pre-built .poi files first (also on a Redownload: the CDN is fresh data).
    if (need.isNotEmpty() && cdn.enabled) {
        val r = cdn.fetchPois(missingIds)
        val byId = work.associateBy { it.id }
        val files = r.files + r.none.mapNotNull { id ->
            byId[id]?.let { t ->
                id to Cycling.buildPoi(emptyList(), t.south, t.west, t.north, t.east, t.id) { _, _ -> true }
            }
        }
        DownloadStats.count("cdn pois", files.size)
        if (files.isNotEmpty()) {
            val versions = files.mapNotNull { (id, _) -> hashes[id]?.let { id to PrebuiltTiles.cdnRecord(it) } }.toMap()
            PoiCache.store(files, versions)
            enqueue(files, versions)
        }
        need = r.fallback.toSet()
    }
    DownloadStats.count("overpass pois", need.size)
    val groups = work.filter { it.id in need }.groupBy { t ->
        val clat = (t.south + t.north) / 2
        val clon = (t.west + t.east) / 2
        "${floor(clat / 0.25).toInt()}_${floor(clon / 0.25).toInt()}"
    }.values.toList()
    for ((i, group) in groups.withIndex()) {
        if (i > 0) delay(1000)   // pace the servers
        onStatus("Fetching cycling POIs ${i + 1}/${groups.size}…")
        val u = union(group)
        val osm: OsmData = MapBuilder.fetchPois(u.south, u.west, u.north, u.east)
        val files = withContext(Dispatchers.Default) {
            // Which cell each POI is in, once (not once per tile).
            val byCell = osm.pois.groupBy { H3Tiles.idAt(LatLon(it.lat, it.lon)) }
            group.map { t ->
                t.id to Cycling.buildPoi(
                    byCell[t.id] ?: emptyList(), t.south, t.west, t.north, t.east, t.id,
                ) { _, _ -> true }
            }
        }
        PoiCache.store(files)                         // phone-built
        enqueue(files, emptyMap())
    }
}


/** Bounding box enclosing a set of tiles, padded slightly so roads at tile edges
 *  are present in the fetch. */
private fun union(ts: List<MapTile>): BoundingBox {
    var s = 90.0; var w = 180.0; var n = -90.0; var e = -180.0
    for (t in ts) {
        s = min(s, t.south); w = min(w, t.west); n = max(n, t.north); e = max(e, t.east)
    }
    val pad = 0.003
    return BoundingBox(s - pad, w - pad, n + pad, e + pad)
}

