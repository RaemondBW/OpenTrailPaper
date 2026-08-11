package com.raemond.opentrailpaper.map

import com.raemond.opentrailpaper.data.LatLon

/** Raw JNI surface over the vendored H3 C (see cpp/h3jni.c). */
internal object H3Native {
    init {
        System.loadLibrary("h3shim")
    }

    external fun coveringCells(south: Double, west: Double, north: Double, east: Double): LongArray
    external fun cellBbox(cell: Long): DoubleArray
    external fun cellBoundary(cell: Long): DoubleArray
    external fun cellId(cell: Long): String
    external fun fromId(id: String): Long
    external fun cellAt(lat: Double, lng: Double): Long
}

/**
 * One H3 res-6 map tile: its H3 id (the device's filename), the raw cell, and
 * its lat/lng bbox.
 */
data class MapTile(
    val id: String,          // H3 hex id, e.g. "86283082fffffff"
    val cell: Long,          // raw H3 index (for the hexagon boundary)
    val south: Double,
    val west: Double,
    val north: Double,
    val east: Double,
) {
    /** The tile's actual hexagon outline (not its bounding box). */
    val hexagon: List<LatLon> by lazy { H3Tiles.hexagon(cell) }

    /** Centre of the hexagon (for placing the on-device check mark). */
    val center: LatLon get() = LatLon((south + north) / 2, (west + east) / 2)
}

/**
 * Thin front for the H3 C shim. Res-6 hexagons are ~5.6 km across; the whole
 * scheme is deterministic and global, so the same ground always maps to the same
 * tile id — that's what makes dedup with the device (and with the iOS app) work.
 */
object H3Tiles {

    /** Every res-6 tile overlapping the drawn box, in a stable order. */
    fun coveringTiles(south: Double, west: Double, north: Double, east: Double): List<MapTile> =
        H3Native.coveringCells(south, west, north, east).map { tile(it) }

    fun tile(cell: Long): MapTile {
        val b = H3Native.cellBbox(cell)
        return MapTile(H3Native.cellId(cell), cell, b[0], b[1], b[2], b[3])
    }

    /**
     * Geometry for an id we already know (a cell on the device, a gap in
     * coverage) — the inverse of [idAt]. null if the string isn't a cell.
     */
    fun tile(id: String): MapTile? {
        val cell = H3Native.fromId(id)
        return if (cell == 0L) null else tile(cell)
    }

    /** The res-6 H3 id containing a coordinate (for hit-testing map taps). */
    fun idAt(coord: LatLon): String? {
        val cell = H3Native.cellAt(coord.lat, coord.lon)
        return if (cell == 0L) null else H3Native.cellId(cell)
    }

    /** The cell's hexagon outline as map coordinates. */
    fun hexagon(cell: Long): List<LatLon> {
        val flat = H3Native.cellBoundary(cell)
        return (0 until flat.size / 2).map { LatLon(flat[it * 2], flat[it * 2 + 1]) }
    }
}
