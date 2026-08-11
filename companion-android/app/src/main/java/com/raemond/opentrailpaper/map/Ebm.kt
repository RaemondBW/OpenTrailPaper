package com.raemond.opentrailpaper.map

import android.graphics.Path
import kotlin.math.cos

/**
 * Reads back the `.ebm` blobs this app builds ([MapBuilder]) and the head unit
 * renders (src/map_tiles.cpp), so the phone can draw the SAME map the device
 * will — roads by class, water, parks.
 *
 * The projection deliberately mirrors the firmware's projectBlobInto() rather
 * than the encoder: the device derives its scale factor from the grid header
 * (midLat = gridLat0 + tileDeg * gridNy / 2) instead of the bounding box the
 * encoder used, and those differ by a few metres. Reproducing what the DEVICE
 * computes is what makes this an honest preview.
 *
 * Format (little-endian), from MapBuilder:
 *   'EBM2', f64 lat0, f64 lon0, f64 tileDeg, i32 nx, i32 ny,
 *   index[nx*ny] of (u32 offset, u32 length)   (0,0 = empty sub-tile),
 *   sub-tiles: u16 polylineCount, per polyline { u8 class, u16 pointCount,
 *              i16 x,y per point (metres E/N of the SUB-TILE's SW corner) },
 *   then optional 'ELV1', then 'WTR2' and 'PRK2' polygon sections whose points
 *   are metres E/N of the GRID origin (lat0, lon0).
 */
object Ebm {

    /**
     * Render classes, numbered as in build_map.py / mapgen.js / the firmware's
     * MapFeatureClass enum.
     */
    enum class FeatureClass { ARTERIAL, PRIMARY, SECONDARY, TERTIARY, MINOR, PATH;

        companion object {
            fun of(raw: Int): FeatureClass? = entries.getOrNull(raw)
        }
    }

    /**
     * One decoded tile, already projected into [MercatorWorld] units relative to
     * [originX] / [originY] — the space the overlay renderer draws in, so
     * nothing has to be re-projected per frame.
     */
    class Tile(
        val roads: Map<FeatureClass, Path>,
        val water: Path?,
        val parks: Path?,
        /** World coordinates the paths above are relative to. */
        val originX: Double,
        val originY: Double,
        /** The same origin as a coordinate, for probing the live map's projection. */
        val originLat: Double,
        val originLon: Double,
        /** Rough cost measure (total points) for the memory budget. */
        val pointCount: Int,
    )

    /** Decodes a blob, or null if it isn't a well-formed EBM2. */
    fun decode(data: ByteArray): Tile? {
        val r = Reader(data)
        if (!r.magic("EBM2", 0)) return null
        val lat0 = r.f64(4) ?: return null
        val lon0 = r.f64(12) ?: return null
        val tileDeg = r.f64(20) ?: return null
        val nx = r.i32(28) ?: return null
        val ny = r.i32(32) ?: return null

        // Bound the grid before trusting it for arithmetic: a corrupt header must
        // not turn into a multi-gigabyte index walk.
        if (nx <= 0 || ny <= 0 || nx > 4096 || ny > 4096 || tileDeg <= 0) return null
        if (36 + nx * ny * 8 > data.size) return null

        // Exactly the device's scale factors (map_tiles.cpp:123).
        val midLat = lat0 + tileDeg * ny / 2
        val kx = 111320.0 * cos(midLat * Math.PI / 180)
        val ky = 110540.0
        if (kx <= 1) return null          // degenerate near the poles
        val tileWm = tileDeg * kx
        val tileHm = tileDeg * ky

        val originX = MercatorWorld.x(lon0)
        val originY = MercatorWorld.y(lat0)

        // Metres E/N of the grid origin -> world units, relative to the origin.
        val px: (Double) -> Float = { mx -> (MercatorWorld.x(lon0 + mx / kx) - originX).toFloat() }
        val py: (Double) -> Float = { my -> (MercatorWorld.y(lat0 + my / ky) - originY).toFloat() }

        val roadPaths = HashMap<FeatureClass, Path>()
        var points = 0

        // --- roads, per sub-tile
        val indexBase = 36
        var dataEnd = indexBase + nx * ny * 8      // also where the sections start
        for (ty in 0 until ny) {
            for (tx in 0 until nx) {
                val e = indexBase + (ty * nx + tx) * 8
                val off = r.u32(e) ?: continue
                val len = r.u32(e + 4) ?: continue
                if (off <= 0 || len <= 0 || off + len > data.size) continue
                if (off + len > dataEnd) dataEnd = off + len

                val ox = tx * tileWm
                val oy = ty * tileHm
                var p = off
                val count = r.u16(p) ?: continue
                p += 2
                for (k in 0 until count) {
                    if (p + 3 > off + len) break
                    val cls = r.u8(p) ?: break
                    val n = r.u16(p + 1) ?: break
                    p += 3
                    if (p + n * 4 > off + len) break
                    val fc = FeatureClass.of(cls)
                    if (fc != null && n >= 2) {
                        val path = roadPaths.getOrPut(fc) { Path() }
                        for (j in 0 until n) {
                            val mx = r.i16(p + j * 4) ?: break
                            val my = r.i16(p + j * 4 + 2) ?: break
                            val x = px(ox + mx)
                            val y = py(oy + my)
                            if (j == 0) path.moveTo(x, y) else path.lineTo(x, y)
                        }
                        points += n
                    }
                    p += n * 4
                }
            }
        }

        // --- fill sections: optional ELV1 (skipped), then WTR2, then PRK2.
        var q = dataEnd
        if (q + 44 <= data.size && r.magic("ELV1", q)) {
            val gw = r.i32(q + 4) ?: 0
            val gh = r.i32(q + 8) ?: 0
            if (gw in 1..4096 && gh in 1..4096) q += 44 + gw * gh * 2
        }
        var water: Path? = null
        var parks: Path? = null
        readFills(r, q, "WTR2", px, py)?.let { (path, next, n) ->
            water = path; q = next; points += n
        }
        readFills(r, q, "PRK2", px, py)?.let { (path, _, n) ->
            parks = path; points += n
        }

        if (roadPaths.isEmpty() && water == null && parks == null) return null
        return Tile(roadPaths, water, parks, originX, originY, lat0, lon0, points)
    }

    /**
     * `<magic><u16 count>`, then each polygon as `<u16 pointCount><i16 x,y …>` in
     * metres E/N of the grid origin. Returns the combined path (rings are closed,
     * as the device closes them), where the section ends, and its size.
     */
    private fun readFills(
        r: Reader,
        start: Int,
        magic: String,
        px: (Double) -> Float,
        py: (Double) -> Float,
    ): Triple<Path, Int, Int>? {
        if (start < 0 || start + 6 > r.size || !r.magic(magic, start)) return null
        val count = r.u16(start + 4) ?: return null
        var q = start + 6
        val path = Path()
        var total = 0
        for (i in 0 until count) {
            val n = r.u16(q) ?: break
            q += 2
            if (q + n * 4 > r.size) break
            if (n >= 3) {
                for (j in 0 until n) {
                    val mx = r.i16(q + j * 4) ?: break
                    val my = r.i16(q + j * 4 + 2) ?: break
                    val x = px(mx.toDouble())
                    val y = py(my.toDouble())
                    if (j == 0) path.moveTo(x, y) else path.lineTo(x, y)
                }
                path.close()
                total += n
            }
            q += n * 4
        }
        return Triple(path, q, total)
    }

    /**
     * Bounds-checked little-endian reads. Every accessor returns null rather than
     * throwing, so a truncated or corrupt tile degrades to "draws less" instead
     * of crashing the map.
     */
    private class Reader(private val d: ByteArray) {
        val size get() = d.size

        fun u8(i: Int): Int? = if (i >= 0 && i < d.size) d[i].toInt() and 0xFF else null

        fun u16(i: Int): Int? =
            if (i >= 0 && i + 2 <= d.size) {
                (d[i].toInt() and 0xFF) or ((d[i + 1].toInt() and 0xFF) shl 8)
            } else {
                null
            }

        fun i16(i: Int): Int? = u16(i)?.toShort()?.toInt()

        fun u32(i: Int): Int? {
            if (i < 0 || i + 4 > d.size) return null
            var v = 0L
            for (k in 3 downTo 0) v = (v shl 8) or (d[i + k].toLong() and 0xFF)
            // Offsets and lengths inside a tile are far below 2 GB; anything
            // larger is corruption, and clamping it to a negative Int would turn
            // a bad header into a wild read.
            return if (v > Int.MAX_VALUE) null else v.toInt()
        }

        fun i32(i: Int): Int? {
            if (i < 0 || i + 4 > d.size) return null
            var v = 0
            for (k in 3 downTo 0) v = (v shl 8) or (d[i + k].toInt() and 0xFF)
            return v
        }

        fun f64(i: Int): Double? {
            if (i < 0 || i + 8 > d.size) return null
            var bits = 0L
            for (k in 7 downTo 0) bits = (bits shl 8) or (d[i + k].toLong() and 0xFF)
            return Double.fromBits(bits)
        }

        fun magic(s: String, i: Int): Boolean {
            val m = s.toByteArray(Charsets.US_ASCII)
            if (i < 0 || i + m.size > d.size) return false
            for (k in m.indices) if (d[i + k] != m[k]) return false
            return true
        }
    }
}
