package com.raemond.opentrailpaper.map

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.delay
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.io.ByteArrayOutputStream
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL
import java.net.URLEncoder
import java.util.Locale
import kotlin.math.abs
import kotlin.math.ceil
import kotlin.math.cos
import kotlin.math.floor
import kotlin.math.hypot
import kotlin.math.max
import kotlin.math.min

/**
 * Builds `.ebm` vector-map blobs (the format src/map_tiles.cpp reads) from
 * OpenStreetMap data for a chosen bounding box, entirely on the phone. A port of
 * companion-ios/Sources/MapBuilder.swift — itself a port of
 * tools/maps/build_map.py — so the output is byte-compatible with what the
 * device already renders and with what the iOS app produces for the same hexagon.
 *
 * Format (little-endian):
 *   'EBM2', f64 lat0, f64 lon0, f64 tileDeg, i32 nx, i32 ny,
 *   index[nx*ny] of (u32 offset, u32 length)  (0,0 = empty tile),
 *   tiles: u16 polylineCount, per polyline { u8 class, u16 pointCount,
 *          i16 x,y per point (metres E/N of the tile SW corner) }.
 *   class: 0 arterial, 1 primary, 2 secondary, 3 tertiary, 4 minor, 5 path.
 *   then an optional 'ELV1' block, a 'WTR2' water section, then a 'PRK2' park
 *   section: 'WTR2'/'PRK2', u16 polygonCount, per polygon { u16 pointCount,
 *          i16 x,y per point (metres E/N of the grid SW origin lat0,lon0) }.
 *
 * Geometry is passed around as `DoubleArray(2)`. Geographic points are
 * `[lat, lon]` and projected ones `[x, y]` in metres — the same two orders the
 * Swift tuples use, kept apart by which function is being called rather than by
 * a type, exactly as in the original.
 */
object MapBuilder {

    /**
     * Public Overpass instances (verified reachable) — the main one 504s under
     * load, so we rotate through these on failure. Don't add a mirror without
     * checking it actually responds; a hung endpoint just wastes the timeout.
     */
    private val OVERPASS_ENDPOINTS = listOf(
        "https://overpass-api.de/api/interpreter",
        "https://maps.mail.ru/osm/tools/overpass/api/interpreter",
    )

    const val TILE_DEG = 0.02
    const val SIMPLIFY_M = 3.0

    /** gw = gh; ~20 samples over a ~7 km tile ≈ 350 m. */
    const val ELEVATION_GRID = 20

    class BuildException(message: String) : Exception(message)

    // MARK: Overpass

    private val QUERY_TEMPLATE = """
        [out:json][timeout:90];
        (
          way["highway"~"^(motorway|trunk|primary|secondary|tertiary|residential|unclassified|living_street|pedestrian|cycleway|footway|path|track|steps)"](%1${'$'}s);
          way["natural"="water"](%1${'$'}s);
          way["natural"="coastline"](%1${'$'}s);
          way["leisure"="park"](%1${'$'}s);
          way["landuse"~"^(grass|forest|meadow|recreation_ground|cemetery|village_green)${'$'}"](%1${'$'}s);
          way["natural"~"^(wood|scrub|grassland|heath)${'$'}"](%1${'$'}s);
        );
        out body;
        >;
        out skel qt;
    """.trimIndent()

    private fun bbox(s: Double, w: Double, n: Double, e: Double) =
        "${num(s)},${num(w)},${num(n)},${num(e)}"

    /** Plain decimal, never the host locale's comma — Overpass rejects "37,7". */
    private fun num(v: Double) = java.math.BigDecimal(v).toPlainString()

    suspend fun fetchOsm(
        south: Double,
        west: Double,
        north: Double,
        east: Double,
        onProgress: ((String) -> Unit)? = null,
    ): OsmData {
        val q = String.format(QUERY_TEMPLATE, bbox(south, west, north, east))
        return overpassPost(q, onProgress)
    }

    /**
     * Coastline ways alone, over a deliberately generous bbox.
     *
     * Sea fill needs the COAST, and a selection out in open water does not
     * contain any: the batch bbox for a set of ocean-only hexes has no coastline
     * in it at all, so no rings could be assembled and those tiles came out
     * blank. Fetching coastline on its own lets the box be padded far past the
     * tiles without dragging in every road for that wider area.
     */
    suspend fun fetchCoastline(
        south: Double,
        west: Double,
        north: Double,
        east: Double,
    ): OsmData {
        val q = "[out:json][timeout:60];" +
            "way[\"natural\"=\"coastline\"](${bbox(south, west, north, east)});" +
            "(._;>;);out body;"
        return overpassPost(q, null)
    }

    /**
     * POST an Overpass QL query, walking the mirror list twice so a transient
     * failure on one server is retried elsewhere. Shared by every fetch — this
     * retry logic used to live inside the road fetch, so any new query either
     * duplicated it or went without.
     */
    private suspend fun overpassPost(
        query: String,
        onProgress: ((String) -> Unit)?,
    ): OsmData {
        val body = ("data=" + URLEncoder.encode(query, "UTF-8")).toByteArray(Charsets.UTF_8)
        var lastStatus = 0
        var lastError: Exception? = null
        val total = OVERPASS_ENDPOINTS.size * 2

        for (attempt in 0 until total) {
            currentCoroutineContext().ensureActive()
            val urlStr = OVERPASS_ENDPOINTS[attempt % OVERPASS_ENDPOINTS.size]
            val host = runCatching { URL(urlStr).host }.getOrNull() ?: urlStr
            onProgress?.invoke("server $host (try ${attempt + 1}/$total)")

            val result = withContext(Dispatchers.IO) {
                var conn: HttpURLConnection? = null
                try {
                    conn = (URL(urlStr).openConnection() as HttpURLConnection).apply {
                        requestMethod = "POST"
                        doOutput = true
                        setRequestProperty("User-Agent", "OpenTrailPaper-Android")
                        setRequestProperty("Content-Type", "application/x-www-form-urlencoded")
                        connectTimeout = 45_000
                        readTimeout = 45_000
                    }
                    conn.outputStream.use { it.write(body) }
                    val code = conn.responseCode
                    if (code != 200) return@withContext Result.failure(HttpStatus(code))
                    // Parsed straight off the socket: an Overpass region is far
                    // too big to hold as a String and then again as a tree.
                    Result.success(OsmData.parse(conn.inputStream))
                } catch (e: Exception) {
                    Result.failure(e)
                } finally {
                    conn?.disconnect()
                }
            }

            result.onSuccess { return it }
            result.onFailure { e ->
                if (e is HttpStatus) lastStatus = e.code else lastError = e as? Exception
            }
            // 429 (rate limit) / 504 (timeout) / 5xx (overload) → back off, try
            // the next mirror.
            delay(800)
        }

        throw when {
            lastStatus == 429 ->
                BuildException("map servers are rate-limiting — wait a minute and retry")

            lastStatus >= 500 ->
                BuildException("map servers are busy ($lastStatus) — try again, or draw a smaller area")

            lastError != null ->
                BuildException("Map download failed: ${lastError?.message ?: "no response"}")

            else -> BuildException("no response from map servers")
        }
    }

    private class HttpStatus(val code: Int) : Exception("HTTP $code")

    // MARK: many-tile encode

    /**
     * Encode many H3 tiles' `.ebm` blobs from already-parsed region data.
     *
     * Emits EVERY tile, including ones with no roads. Water, sea rings, parks and
     * elevation are appended by the caller AFTER this, so dropping a road-empty
     * tile here would throw away the only chance an all-water or all-park hex had
     * to become anything — which is why a bay hex with no streets in it never
     * saved as water. The caller decides what is empty once everything has been
     * appended (see [isEmpty]).
     */
    fun encodeTiles(osm: OsmData, tiles: List<MapTile>): List<Pair<String, ByteArray>> =
        tiles.map { t -> t.id to encode(osm, t.south, t.west, t.north, t.east) }

    /**
     * True when a finished tile carries nothing but its header — no roads, no
     * water, no parks, no elevation. Only then is it not worth storing.
     */
    fun isEmpty(data: ByteArray, tile: MapTile): Boolean =
        data.size <= headerOnly(tile.south, tile.west, tile.north, tile.east)

    private fun headerOnly(s: Double, w: Double, n: Double, e: Double): Int {
        val lat0 = floor(s / TILE_DEG) * TILE_DEG
        val lon0 = floor(w / TILE_DEG) * TILE_DEG
        val nx = ceil((e - lon0) / TILE_DEG).toInt()
        val ny = ceil((n - lat0) / TILE_DEG).toInt()
        return 36 + nx * ny * 8
    }

    // MARK: elevation
    //
    // A DEM baked into the tile so the device has elevation without GPS altitude
    // or the phone — see the ELV1 block the device reads back.

    /**
     * A gridN×gridN grid of int16 elevations (metres) over [s,w]-[n,e], row 0 =
     * south, west→east within a row. Sampled from Open-Meteo (free, no key,
     * 100 points per request).
     */
    suspend fun fetchElevationGrid(
        south: Double,
        west: Double,
        north: Double,
        east: Double,
        gridN: Int = ELEVATION_GRID,
    ): ShortArray {
        val lats = ArrayList<Double>(gridN * gridN)
        val lons = ArrayList<Double>(gridN * gridN)
        for (i in 0 until gridN) {
            val lat = south + (north - south) * i / (gridN - 1)
            for (j in 0 until gridN) {
                lats.add(lat)
                lons.add(west + (east - west) * j / (gridN - 1))
            }
        }

        val out = ShortArray(gridN * gridN)
        var idx = 0
        while (idx < lats.size) {
            val end = min(idx + 100, lats.size)
            val la = (idx until end).joinToString(",") { fmt5(lats[it]) }
            val lo = (idx until end).joinToString(",") { fmt5(lons[it]) }
            val body = withContext(Dispatchers.IO) {
                val url = URL(
                    "https://api.open-meteo.com/v1/elevation" +
                        "?latitude=${URLEncoder.encode(la, "UTF-8")}" +
                        "&longitude=${URLEncoder.encode(lo, "UTF-8")}",
                )
                val conn = (url.openConnection() as HttpURLConnection).apply {
                    setRequestProperty("User-Agent", "OpenTrailPaper-Android")
                    connectTimeout = 30_000
                    readTimeout = 30_000
                }
                try {
                    if (conn.responseCode != 200) throw IOException("elevation server error")
                    conn.inputStream.readBytes().toString(Charsets.UTF_8)
                } finally {
                    conn.disconnect()
                }
            }
            val arr = JSONObject(body).optJSONArray("elevation")
            if (arr != null) {
                for (k in 0 until arr.length()) {
                    if (idx + k >= out.size) break
                    val v = if (arr.isNull(k)) 0.0 else arr.optDouble(k, 0.0)
                    out[idx + k] = rnd(v).coerceIn(-2000, 9000).toShort()
                }
            }
            idx = end
        }
        return out
    }

    private fun fmt5(v: Double) = String.format(Locale.US, "%.5f", v)

    /**
     * Append an ELV1 elevation block to an already-encoded tile.
     *   'ELV1', i32 gw, i32 gh, f64 s,w,n,e, i16 elev[gh*gw]  (row 0 = south)
     */
    fun appendElevation(
        out: ByteArrayOutputStream,
        south: Double,
        west: Double,
        north: Double,
        east: Double,
        grid: ShortArray,
        gridN: Int,
    ) {
        if (grid.size != gridN * gridN) return
        out.write("ELV1".toByteArray(Charsets.US_ASCII))
        out.i32(gridN); out.i32(gridN)
        out.f64(south); out.f64(west); out.f64(north); out.f64(east)
        for (v in grid) out.i16(v.toInt())
    }

    // MARK: region-level geometry extraction

    /** natural=water ways as lists of [lat, lon]. */
    fun waterWays(osm: OsmData): List<List<DoubleArray>> =
        osm.ways.filter { it.isWater }.map { osm.coords(it) }

    /** Park / green-area ways as lists of [lat, lon]. */
    fun parkWays(osm: OsmData): List<List<DoubleArray>> =
        osm.ways.filter { it.isPark }.map { osm.coords(it) }

    /**
     * Resolve natural=coastline ways and assemble them into maximal chains of
     * [lat, lon], preserving direction (LAND on the LEFT, SEA on the RIGHT).
     */
    fun coastlineChains(osm: OsmData): List<List<DoubleArray>> {
        val coastWays = osm.ways.filter { it.isCoastline }
        val chains: MutableList<LongArray?> = coastWays.map { it.nodes }.toMutableList()
        var changed = true
        while (changed) {
            changed = false
            outer@ for (i in chains.indices) {
                val a = chains[i] ?: continue
                for (j in chains.indices) {
                    if (j == i) continue
                    val b = chains[j] ?: continue
                    if (a.isEmpty() || b.isEmpty()) continue
                    // Forward joins only. Reversing a coastline way flips its
                    // direction and thus which side is water (OSM: water on the
                    // right), which inverts the sea fill. Valid coastlines chain
                    // head-to-tail, so forward joins suffice.
                    if (a.last() == b.first()) {
                        chains[i] = a + b.copyOfRange(1, b.size)
                        chains[j] = null
                        changed = true
                    } else if (a.first() == b.last()) {
                        chains[i] = b + a.copyOfRange(1, a.size)
                        chains[j] = null
                        changed = true
                    }
                    if (changed) break@outer
                }
            }
        }

        return chains.filterNotNull()
            .map { osm.coords(it) }
            .filter { it.size >= 2 }
    }

    // MARK: coastline sea-fill

    /**
     * Assemble SEA rings for the box [s,w,n,e] the osmcoastline way: clip every
     * chain into boundary-to-boundary sub-chains, then trace rings by following
     * each coast forward (OSM: water on the right) and, at its exit, walking the
     * box boundary CLOCKWISE (interior on the right = water) to the NEXT coast's
     * entry. Uses the real topology, so a peninsula never encloses land.
     */
    fun regionSeaPolygons(
        chains: List<List<DoubleArray>>,
        south: Double,
        west: Double,
        north: Double,
        east: Double,
    ): List<List<DoubleArray>> {
        val subs = ArrayList<List<DoubleArray>>()
        for (chain in chains) {
            for ((pts, startOnBoundary, endOnBoundary) in clipChain(chain, south, west, north, east)) {
                if (startOnBoundary && endOnBoundary && pts.size >= 2) subs.add(pts)
            }
        }
        if (subs.isEmpty()) return emptyList()

        val ww = east - west
        val hh = north - south
        val total = 2 * ww + 2 * hh
        val entries = subs.map { perimPos(it.first(), south, west, north, east) }
        val exits = subs.map { perimPos(it.last(), south, west, north, east) }
        val used = BooleanArray(subs.size)
        val rings = ArrayList<List<DoubleArray>>()

        for (start in subs.indices) {
            if (used[start]) continue
            val ring = ArrayList<DoubleArray>()
            var i = start
            var guard = 0
            while (!used[i] && guard < 4 * subs.size + 8) {
                guard += 1
                used[i] = true
                ring.addAll(subs[i])                        // coast A->B (water right)
                val ex = exits[i]
                var best = -1
                var bestGap = Double.MAX_VALUE
                for (j in subs.indices) {
                    var gap = pmod(ex - entries[j], total)  // CW ex -> entry
                    if (gap <= 1e-12) gap += total          // not the same point
                    if (gap < bestGap) { bestGap = gap; best = j }
                }
                if (best < 0) break
                ring.addAll(closing(ex, entries[best], south, west, north, east, false))  // CW
                i = best
            }
            if (ring.size >= 3) rings.add(ring)
        }
        return rings
    }

    /**
     * Clip segment a->b ([lat, lon]) to the rectangle. Returns the inside
     * parameter interval (t0, t1) with 0<=t0<=t1<=1, or null if outside.
     */
    private fun liangBarsky(
        a: DoubleArray,
        b: DoubleArray,
        s: Double,
        w: Double,
        n: Double,
        e: Double,
    ): DoubleArray? {
        val ax = a[1]; val ay = a[0]      // x=lon, y=lat
        val bx = b[1]; val by = b[0]
        val dx = bx - ax; val dy = by - ay
        val p = doubleArrayOf(-dx, dx, -dy, dy)
        val q = doubleArrayOf(ax - w, e - ax, ay - s, n - ay)
        var t0 = 0.0
        var t1 = 1.0
        for (i in 0 until 4) {
            if (p[i] == 0.0) {
                if (q[i] < 0.0) return null
            } else {
                val t = q[i] / p[i]
                if (p[i] < 0.0) {
                    if (t > t1) return null
                    if (t > t0) t0 = t
                } else {
                    if (t < t0) return null
                    if (t < t1) t1 = t
                }
            }
        }
        return doubleArrayOf(t0, t1)
    }

    private fun lerp(a: DoubleArray, b: DoubleArray, t: Double) =
        doubleArrayOf(a[0] + t * (b[0] - a[0]), a[1] + t * (b[1] - a[1]))

    private fun same(a: DoubleArray, b: DoubleArray) = a[0] == b[0] && a[1] == b[1]

    /** Split a chain into rectangle-clipped sub-chains. */
    private fun clipChain(
        chain: List<DoubleArray>,
        s: Double,
        w: Double,
        n: Double,
        e: Double,
    ): List<Triple<List<DoubleArray>, Boolean, Boolean>> {
        val subs = ArrayList<Triple<List<DoubleArray>, Boolean, Boolean>>()
        var cur: ArrayList<DoubleArray>? = null
        var startB = false
        if (chain.size >= 2) {
            for (k in 0 until chain.size - 1) {
                val a = chain[k]
                val b = chain[k + 1]
                val lb = liangBarsky(a, b, s, w, n, e)
                if (lb == null) {
                    cur?.let { subs.add(Triple(it, startB, false)) }
                    cur = null
                    continue
                }
                val (t0, t1) = lb[0] to lb[1]
                // Use the exact chain endpoint when unclipped, so a shared point
                // matches bit-for-bit across adjacent segments (avoids a
                // degenerate zero-length segment from float drift in lerp).
                val p0 = if (t0 == 0.0) a else lerp(a, b, t0)
                val p1 = if (t1 == 1.0) b else lerp(a, b, t1)
                val c = cur
                if (c == null) {
                    cur = arrayListOf(p0)
                    startB = t0 > 0.0
                } else if (!same(c.last(), p0)) {
                    c.add(p0)
                }
                cur!!.let { if (!same(it.last(), p1)) it.add(p1) }
                if (t1 < 1.0) {
                    subs.add(Triple(cur!!, startB, true))
                    cur = null
                }
            }
        }
        cur?.let { subs.add(Triple(it, startB, false)) }
        return subs
    }

    /** Position of a boundary point along the perimeter, CCW from the SW corner. */
    private fun perimPos(pt: DoubleArray, s: Double, w: Double, n: Double, e: Double): Double {
        val lat = pt[0]
        val lon = pt[1]
        val ww = e - w
        val hh = n - s
        val db = abs(lat - s)
        val dr = abs(lon - e)
        val dt = abs(lat - n)
        val dl = abs(lon - w)
        val mn = min(min(db, dr), min(dt, dl))
        if (mn == db) return lon - w
        if (mn == dr) return ww + (lat - s)
        if (mn == dt) return ww + hh + (e - lon)
        return ww + hh + ww + (n - lat)
    }

    /** Positive modulo (matches Python's % for the perimeter math). */
    private fun pmod(a: Double, b: Double): Double {
        val r = a % b
        return if (r < 0) r + b else r
    }

    /**
     * Corner points passed walking the perimeter from fromPos to toPos, CCW
     * (increasing) or CW (decreasing).
     */
    private fun closing(
        fromPos: Double,
        toPos: Double,
        s: Double,
        w: Double,
        n: Double,
        e: Double,
        ccw: Boolean,
    ): List<DoubleArray> {
        val ww = e - w
        val hh = n - s
        val total = 2 * ww + 2 * hh
        val corners = listOf(
            Triple(s, w, 0.0),
            Triple(s, e, ww),
            Triple(n, e, ww + hh),
            Triple(n, w, ww + hh + ww),
        )
        val res = ArrayList<Pair<Double, DoubleArray>>()
        if (ccw) {
            val d = pmod(toPos - fromPos, total)
            for ((clat, clon, cpos) in corners) {
                val cd = pmod(cpos - fromPos, total)
                if (cd > 0.0 && cd < d) res.add(cd to doubleArrayOf(clat, clon))
            }
        } else {
            val d = pmod(fromPos - toPos, total)
            for ((clat, clon, cpos) in corners) {
                val cd = pmod(fromPos - cpos, total)
                if (cd > 0.0 && cd < d) res.add(cd to doubleArrayOf(clat, clon))
            }
        }
        res.sortBy { it.first }
        return res.map { it.second }
    }

    /**
     * Sutherland–Hodgman clip of a polygon ([lat, lon]) to the rectangle
     * [s,w,n,e]. Needed because a region sea ring can enclose a fully-ocean tile
     * without placing any vertex inside it — the tile must still fill.
     */
    private fun clipToBox(
        poly: List<DoubleArray>,
        s: Double,
        w: Double,
        n: Double,
        e: Double,
    ): List<DoubleArray> {
        fun clip(
            pts: List<DoubleArray>,
            inside: (DoubleArray) -> Boolean,
            isect: (DoubleArray, DoubleArray) -> DoubleArray,
        ): List<DoubleArray> {
            if (pts.isEmpty()) return emptyList()
            val res = ArrayList<DoubleArray>(pts.size + 4)
            val m = pts.size
            for (i in 0 until m) {
                val cur = pts[i]
                val prev = pts[(i + m - 1) % m]
                val curIn = inside(cur)
                val prevIn = inside(prev)
                if (curIn) {
                    if (!prevIn) res.add(isect(prev, cur))
                    res.add(cur)
                } else if (prevIn) {
                    res.add(isect(prev, cur))
                }
            }
            return res
        }

        var p = poly
        p = clip(p, { it[1] >= w }) { a, b ->
            val t = (w - a[1]) / (b[1] - a[1]); doubleArrayOf(a[0] + t * (b[0] - a[0]), w)
        }
        p = clip(p, { it[1] <= e }) { a, b ->
            val t = (e - a[1]) / (b[1] - a[1]); doubleArrayOf(a[0] + t * (b[0] - a[0]), e)
        }
        p = clip(p, { it[0] >= s }) { a, b ->
            val t = (s - a[0]) / (b[0] - a[0]); doubleArrayOf(s, a[1] + t * (b[1] - a[1]))
        }
        p = clip(p, { it[0] <= n }) { a, b ->
            val t = (n - a[0]) / (b[0] - a[0]); doubleArrayOf(n, a[1] + t * (b[1] - a[1]))
        }
        return p
    }

    // MARK: fill sections

    /**
     * Append a WTR2 water section to an already-encoded tile (after its ELV1
     * block, if any). Points are metres E/N of the tile's snapped grid origin
     * (lat0, lon0), radially decimated at [SIMPLIFY_M] and i16-clamped. A polygon
     * is included if any of its points fall in [s,w,n,e]; the whole simplified
     * ring is stored (>= 3 points, else it is skipped). Always writes the "WTR2"
     * magic + count (0 if no polygons).
     */
    fun appendWater(
        out: ByteArrayOutputStream,
        waterWays: List<List<DoubleArray>>,
        seaRings: List<List<DoubleArray>>,
        south: Double,
        west: Double,
        north: Double,
        east: Double,
    ) {
        val polys = ArrayList<List<IntArray>>()
        val proj = projector(south, west, north, east)

        for (pts in waterWays) {
            val inBox = pts.any { it[0] >= south && it[0] <= north && it[1] >= west && it[1] <= east }
            if (!inBox) continue
            // Radial decimation (NOT RDP) so closed rings survive; the implicit
            // closing point (equal to the first) drops at distance 0. The device
            // closes the ring, so we never append a closing point.
            val m = decimate(pts.map(proj), SIMPLIFY_M)
            if (m.size < 3) continue
            polys.add(quantise(m))
        }

        // Coastline sea-fill: clip each region-level sea ring to this tile, then
        // project/decimate/i16 like a water polygon. Clipping (not a vertex test)
        // is required so a tile fully inside the sea still fills.
        for (ring in seaRings) {
            val clipped = clipToBox(ring, south, west, north, east)
            if (clipped.size < 3) continue
            val m = decimate(clipped.map(proj), SIMPLIFY_M)
            if (m.size < 3) continue
            polys.add(quantise(m))
        }

        writeFillSection(out, "WTR2", polys)
    }

    /**
     * Append a PRK2 park section (after the WTR2 block). Same encoding as
     * [appendWater]'s water polygons; parks are already closed rings so there is
     * no coastline assembly. Always writes the "PRK2" magic + count.
     */
    fun appendParks(
        out: ByteArrayOutputStream,
        parkWays: List<List<DoubleArray>>,
        south: Double,
        west: Double,
        north: Double,
        east: Double,
    ) {
        val polys = ArrayList<List<IntArray>>()
        val proj = projector(south, west, north, east)
        for (pts in parkWays) {
            val inBox = pts.any { it[0] >= south && it[0] <= north && it[1] >= west && it[1] <= east }
            if (!inBox) continue
            val m = decimate(pts.map(proj), SIMPLIFY_M)
            if (m.size < 3) continue
            polys.add(quantise(m))
        }
        writeFillSection(out, "PRK2", polys)
    }

    /** [lat, lon] -> metres E/N of the tile's snapped grid origin. */
    private fun projector(s: Double, w: Double, n: Double, e: Double): (DoubleArray) -> DoubleArray {
        val midLat = (s + n) / 2
        val kx = 111320.0 * cos(midLat * Math.PI / 180)
        val ky = 110540.0
        val lat0 = floor(s / TILE_DEG) * TILE_DEG
        val lon0 = floor(w / TILE_DEG) * TILE_DEG
        return { p -> doubleArrayOf((p[1] - lon0) * kx, (p[0] - lat0) * ky) }
    }

    private fun quantise(m: List<DoubleArray>): List<IntArray> =
        m.map { intArrayOf(rnd(it[0]).coerceIn(-32000, 32000), rnd(it[1]).coerceIn(-32000, 32000)) }

    private fun writeFillSection(
        out: ByteArrayOutputStream,
        magic: String,
        polys: List<List<IntArray>>,
    ) {
        out.write(magic.toByteArray(Charsets.US_ASCII))
        out.u16(min(polys.size, 0xFFFF))
        for (poly in polys) {
            out.u16(min(poly.size, 0xFFFF))
            for (p in poly) { out.i16(p[0]); out.i16(p[1]) }
        }
    }

    // MARK: encode

    private fun encode(osm: OsmData, s: Double, w: Double, n: Double, e: Double): ByteArray {
        val midLat = (s + n) / 2
        val kx = 111320.0 * cos(midLat * Math.PI / 180)
        val ky = 110540.0
        val lat0 = floor(s / TILE_DEG) * TILE_DEG
        val lon0 = floor(w / TILE_DEG) * TILE_DEG
        val nx = ceil((e - lon0) / TILE_DEG).toInt()
        val ny = ceil((n - lat0) / TILE_DEG).toInt()
        if (nx <= 0 || ny <= 0) throw BuildException("No roads found in that area.")

        val tileWm = TILE_DEG * kx
        val tileHm = TILE_DEG * ky

        // sub-tile key (ty*nx+tx) -> polylines [(cls, [(x,y) tile-local metres])]
        val subTiles = HashMap<Int, MutableList<Pair<Int, List<IntArray>>>>()

        fun emit(tx: Int, ty: Int, cls: Int, run: List<DoubleArray>) {
            if (run.size < 2 || tx < 0 || tx >= nx || ty < 0 || ty >= ny) return
            val ox = tx * tileWm
            val oy = ty * tileHm
            val pts = ArrayList<IntArray>(run.size)
            for (p in run) {
                val lx = rnd(p[0] - ox).coerceIn(-32000, 32000)
                val ly = rnd(p[1] - oy).coerceIn(-32000, 32000)
                val last = pts.lastOrNull()
                if (last != null && last[0] == lx && last[1] == ly) continue
                pts.add(intArrayOf(lx, ly))
            }
            if (pts.size >= 2) {
                subTiles.getOrPut(ty * nx + tx) { mutableListOf() }.add(cls to pts)
            }
        }

        for (way in osm.ways) {
            if (way.roadClass < 0) continue
            val geo = osm.coords(way)
            if (geo.size < 2) continue
            val projected = geo.map { doubleArrayOf((it[1] - lon0) * kx, (it[0] - lat0) * ky) }
            val m = rdp(projected, SIMPLIFY_M)
            if (m.size < 2) continue

            var run = arrayListOf(m[0])
            var curTx = floor(m[0][0] / tileWm).toInt()
            var curTy = floor(m[0][1] / tileHm).toInt()
            for (i in 1 until m.size) {
                val p = m[i]
                val tx = floor(p[0] / tileWm).toInt()
                val ty = floor(p[1] / tileHm).toInt()
                run.add(p)
                if (tx != curTx || ty != curTy) {
                    emit(curTx, curTy, way.roadClass, run)
                    // Carry the crossing segment into the next tile, so a road
                    // does not gain a gap at every tile seam.
                    run = arrayListOf(run[run.size - 2], p)
                    curTx = tx
                    curTy = ty
                }
            }
            emit(curTx, curTy, way.roadClass, run)
        }

        // Serialize
        val out = ByteArrayOutputStream(1 shl 16)
        out.write("EBM2".toByteArray(Charsets.US_ASCII))
        out.f64(lat0); out.f64(lon0); out.f64(TILE_DEG)
        out.i32(nx); out.i32(ny)

        // Build each sub-tile's blob first, then the index, then concatenate.
        val blobs = HashMap<Int, ByteArray>(subTiles.size)
        for ((key, polys) in subTiles) {
            val b = ByteArrayOutputStream(1 shl 12)
            b.u16(min(polys.size, 0xFFFF))
            for ((cls, pts) in polys) {
                b.write(cls and 0xFF)
                b.u16(min(pts.size, 0xFFFF))
                for (p in pts) { b.i16(p[0]); b.i16(p[1]) }
            }
            blobs[key] = b.toByteArray()
        }

        var off = 36 + nx * ny * 8
        val index = ByteArrayOutputStream(nx * ny * 8)
        val ordered = ArrayList<ByteArray>(blobs.size)
        for (ty in 0 until ny) {
            for (tx in 0 until nx) {
                val b = blobs[ty * nx + tx]
                if (b != null && b.isNotEmpty()) {
                    index.i32(off); index.i32(b.size)
                    ordered.add(b)
                    off += b.size
                } else {
                    index.i32(0); index.i32(0)
                }
            }
        }
        out.write(index.toByteArray())
        for (b in ordered) out.write(b)
        return out.toByteArray()
    }

    // MARK: simplification

    /** Ramer–Douglas–Peucker on projected metre coords. */
    private fun rdp(points: List<DoubleArray>, eps: Double): List<DoubleArray> {
        if (points.size < 3) return points
        val keep = BooleanArray(points.size)
        keep[0] = true
        keep[points.size - 1] = true
        val stack = ArrayDeque<Pair<Int, Int>>()
        stack.addLast(0 to points.size - 1)
        while (stack.isNotEmpty()) {
            val (a, b) = stack.removeLast()
            val ax = points[a][0]; val ay = points[a][1]
            val bx = points[b][0]; val by = points[b][1]
            val dx = bx - ax; val dy = by - ay
            val norm = max(hypot(dx, dy), 1e-9)
            var worst = 0.0
            var wi = -1
            for (i in a + 1 until b) {
                val px = points[i][0]; val py = points[i][1]
                val d = abs(dx * (ay - py) - dy * (ax - px)) / norm
                if (d > worst) { worst = d; wi = i }
            }
            if (worst > eps && wi >= 0) {
                keep[wi] = true
                stack.addLast(a to wi)
                stack.addLast(wi to b)
            }
        }
        return points.filterIndexed { i, _ -> keep[i] }
    }

    /**
     * Radial decimation for closed rings (water, parks): keep the first point,
     * then each point only if it's > eps metres from the last kept point. Unlike
     * RDP this survives closed rings (the implicit closing point drops at
     * distance 0).
     */
    private fun decimate(points: List<DoubleArray>, eps: Double): List<DoubleArray> {
        val first = points.firstOrNull() ?: return points
        val kept = ArrayList<DoubleArray>(points.size)
        kept.add(first)
        var lx = first[0]
        var ly = first[1]
        for (i in 1 until points.size) {
            val p = points[i]
            if (hypot(p[0] - lx, p[1] - ly) > eps) {
                kept.add(p)
                lx = p[0]
                ly = p[1]
            }
        }
        return kept
    }

    /**
     * Half-away-from-zero rounding, which is what Swift's `.rounded()` does.
     * Kotlin's `roundToInt` rounds half UP, so a coordinate landing exactly on
     * .5 south or west of the origin would quantise one metre away from the
     * value the iOS builder produces — and these blobs are meant to be
     * byte-identical for the same hexagon whichever phone built it.
     */
    private fun rnd(v: Double): Int =
        if (v < 0) -floor(-v + 0.5).toInt() else floor(v + 0.5).toInt()
}

// Little-endian writers, matching the format the firmware reads.
private fun ByteArrayOutputStream.u16(v: Int) {
    write(v and 0xFF)
    write((v ushr 8) and 0xFF)
}

private fun ByteArrayOutputStream.i16(v: Int) = u16(v and 0xFFFF)

private fun ByteArrayOutputStream.i32(v: Int) {
    for (s in 0 until 32 step 8) write((v ushr s) and 0xFF)
}

private fun ByteArrayOutputStream.f64(v: Double) {
    var bits = v.toRawBits()
    for (i in 0 until 8) {
        write((bits and 0xFF).toInt())
        bits = bits ushr 8
    }
}
