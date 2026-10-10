package com.raemond.opentrailpaper.map

import java.io.ByteArrayOutputStream
import kotlin.math.cos
import kotlin.math.max
import kotlin.math.min

/**
 * The cycling layer of the map builder: bike-route / cycleway / bike-lane way
 * flags (a trailer on each .ebm sub-tile) and the per-tile `.poi` file of
 * drinking water, toilets, repair stations and bike shops.
 *
 * A port of docs/mapgen.js (wayFlags, classifyRouteMember, poiOf, buildPoi),
 * which is the reference: the bytes must match it and the iOS MapBuilder for
 * the same Overpass input (tools/map_test/run_crossport.sh, CrossPortTest).
 * Format and design: investigations/osm-pois-bike-routes.md.
 */
object Cycling {
    const val WAY_ROUTE_MASK = 0x03   // 0 none, 1 local, 2 regional, 3 national/intl
    const val WAY_CYCLEWAY = 0x04
    const val WAY_BIKE_LANE = 0x08
    private val LANE_VALUES = setOf("lane", "track", "opposite_lane", "opposite_track")
    private val PATHLIKE = setOf("path", "footway", "bridleway", "track", "pedestrian")
    private val LANE_KEYS = listOf("cycleway", "cycleway:both", "cycleway:left", "cycleway:right")

    /** Flags for a way from its own tags plus its best route-network level. */
    fun wayFlags(tags: Map<String, String>, routeLevel: Int): Int {
        var f = routeLevel and WAY_ROUTE_MASK
        val hw = tags["highway"] ?: ""
        if (hw == "cycleway" || (hw in PATHLIKE && tags["bicycle"] == "designated")) f = f or WAY_CYCLEWAY
        for (k in LANE_KEYS) {
            if ((tags[k] ?: "") in LANE_VALUES) { f = f or WAY_BIKE_LANE; break }
        }
        return f
    }

    /** A route member the highway filter does not classify still has to draw. */
    fun classifyRouteMember(tags: Map<String, String>): Int {
        val hw = tags["highway"] ?: ""
        if (hw.isEmpty() || hw == "proposed" || hw == "construction" || hw == "platform") return -1
        return if (hw == "bridleway" || hw == "corridor") 5 else 4
    }

    // POI types (the on-file bytes; src/map_view.h MapPoiType).
    const val POI_WATER = 1
    const val POI_TOILETS = 2
    const val POI_REPAIR = 3
    const val POI_BIKE_SHOP = 4

    class Poi(val type: Int, val flags: Int, val lat: Double, val lon: Double)

    /** OSM tags -> (type, flags), or null if this is not a POI we keep. */
    fun poiOf(tags: Map<String, String>): Pair<Int, Int>? {
        val a = tags["amenity"] ?: ""
        fun yes(k: String): Boolean { val v = tags[k]; return v == "yes" || v == "only" }
        val acc = tags["access"] ?: ""
        if (acc == "private" || acc == "no") return null
        val seasonal = tags["seasonal"]
        var restricted = acc == "customers" || acc == "permissive_customers" ||
            tags["fee"] == "yes" || (!seasonal.isNullOrEmpty() && seasonal != "no")
        val type: Int
        var f = 0
        if (a == "drinking_water" ||
            ((tags["man_made"] == "water_tap" || a == "fountain") && tags["drinking_water"] == "yes")
        ) {
            if (tags["drinking_water"] == "no") return null
            type = POI_WATER
        } else if (a == "toilets") {
            type = POI_TOILETS
            if (tags["drinking_water"] == "yes") f = f or 0x01
        } else if (a == "bicycle_repair_station") {
            type = POI_REPAIR
            if (yes("service:bicycle:pump")) f = f or 0x01
            if (yes("service:bicycle:tools")) f = f or 0x02
            if (yes("service:bicycle:chain_tool")) f = f or 0x04
            if (yes("service:bicycle:stand")) f = f or 0x08
        } else if (tags["shop"] == "bicycle") {
            type = POI_BIKE_SHOP
            if (yes("service:bicycle:pump")) f = f or 0x01
            if (yes("service:bicycle:repair") || yes("service:bicycle:diy")) f = f or 0x02
            if (yes("service:bicycle:rental")) f = f or 0x04
            if (yes("service:bicycle:retail") || yes("service:bicycle:parts")) f = f or 0x08
            if (yes("service:bicycle:second_hand")) f = f or 0x10
            if (yes("service:bicycle:ebike") || yes("service:bicycle:charging")) f = f or 0x20
            restricted = false   // a shop is "customers only" by nature
        } else {
            return null
        }
        if (restricted) f = f or 0x80
        return type to f
    }

    /**
     * One tile's `.poi` file (format: src/map_tiles.cpp poiFileValid). Keeps the
     * POIs [contains] accepts — pass the H3 cell test so each POI lives in
     * exactly one tile — or, without it, those inside the bbox. Always returns a
     * file: an empty one means "built, and there is nothing here".
     */
    fun buildPoi(
        pois: List<Poi>,
        s: Double, w: Double, n: Double, e: Double,
        cell: String?,
        contains: ((Double, Double) -> Boolean)? = null,
    ): ByteArray {
        val lat0 = (s + n) / 2
        val lon0 = (w + e) / 2
        val kx = 111320.0 * cos((lat0 * Math.PI) / 180)
        val ky = 110540.0
        val fk = kx.toFloat().toDouble()
        val fky = ky.toFloat().toDouble()
        val list = ArrayList<IntArray>()
        for (p in pois) {
            val inside = contains?.invoke(p.lat, p.lon)
                ?: (p.lat >= s && p.lat <= n && p.lon >= w && p.lon <= e)
            if (!inside) continue
            // Math.rint is round-half-to-even — mapgen.js pyRound.
            val x = max(-32000, min(32000, Math.rint((p.lon - lon0) * fk).toInt()))
            val y = max(-32000, min(32000, Math.rint((p.lat - lat0) * fky).toInt()))
            list.add(intArrayOf(p.type, p.flags, x, y))
        }
        // Stable order (y, x, type), as mapgen.js sorts.
        val sorted = list.sortedWith(compareBy<IntArray>({ it[3] }, { it[2] }, { it[0] }))
            .take(0xFFFF)
        val out = ByteArrayOutputStream(40 + sorted.size * 6)
        out.write("EPOI".toByteArray(Charsets.US_ASCII))
        out.write(1)                       // version
        out.write(6)                       // record size
        le(out, sorted.size.toLong(), 2)
        val c = cell?.let { runCatching { java.lang.Long.parseUnsignedLong(it, 16) }.getOrNull() } ?: 0L
        le(out, c, 8)
        le(out, lat0.toRawBits(), 8)
        le(out, lon0.toRawBits(), 8)
        le(out, kx.toFloat().toRawBits().toLong(), 4)
        le(out, ky.toFloat().toRawBits().toLong(), 4)
        for (r in sorted) {
            out.write(r[0]); out.write(r[1])
            le(out, r[2].toLong(), 2); le(out, r[3].toLong(), 2)
        }
        return out.toByteArray()
    }

    private fun le(out: ByteArrayOutputStream, v: Long, bytes: Int) {
        for (i in 0 until bytes) out.write(((v ushr (8 * i)) and 0xFF).toInt())
    }
}
