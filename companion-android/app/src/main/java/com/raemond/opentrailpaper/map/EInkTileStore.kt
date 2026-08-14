package com.raemond.opentrailpaper.map

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import com.raemond.opentrailpaper.data.BoundingBox
import com.raemond.opentrailpaper.data.LatLon
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch

/** One area of coverage: this phone holds it, the device holds it, or both. */
class OutlineHex(
    val id: String,
    val hexagon: List<LatLon>,
    val center: LatLon,
    val synced: Boolean,
    /**
     * Inverted meaning: ground with NO downloaded coverage at all. The Route
     * screen outlines the hexes a planned route crosses that nobody holds, so
     * the grid appears only where the maps run out.
     */
    val missing: Boolean = false,
)

/** A hex in the area the user is selecting for download. */
class SelectionHex(val id: String, val hexagon: List<LatLon>, val kind: Kind) {
    enum class Kind { PENDING, DONE, EXCLUDED }
}

/**
 * Which areas this phone has downloaded, so the map can show coverage.
 *
 * Two separate facts, deliberately kept apart:
 *  * [ids] — every area we hold tile DATA for. This is what "downloaded" means on
 *    the map, and it comes from [TileCache] (the same blobs streamed to the device).
 *  * `BleManager.deviceTileIds` — what the DEVICE has. The intersection is what
 *    earns a green check.
 *
 * This used to decode each area's `.ebm` and paint it in the head unit's own
 * 1-bit style — paper, screentoned water and parkland, roads by class. That is
 * gone. It was expensive in a way that could not be designed away: geometry in a
 * `Path` measures far more than the four bytes a point occupies in the tile it
 * came from, so a region-sized download ran to hundreds of megabytes at full
 * street detail and still needed an LRU, a point budget and a decode queue to
 * stay inside it. What the map is actually asked is "which ground do I have, and
 * does the device have it too" — and a hexagon with a check answers that
 * completely.
 */
object EInkTileStore {

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)

    /**
     * Bumps whenever the drawable set changes, so screens re-diff their overlays.
     *
     * COALESCED — see [bump]. A downloading run finishes areas continuously and
     * each one publishing immediately would rebuild every overlay on the map
     * several times a second.
     */
    var version by mutableStateOf(0); private set

    private var bumpPending = false

    /** Publish at most ~4x/sec. Areas land far faster than that and no rider can
     *  see the difference, but the map view certainly can. */
    private fun bump() {
        if (bumpPending) return
        bumpPending = true
        scope.launch {
            delay(250)
            bumpPending = false
            version += 1
        }
    }

    /** Areas we hold data for on disk. */
    var ids: Set<String> = emptySet()
        private set

    /** Re-read which areas exist on disk. Cheap (one directory listing). */
    fun refresh() {
        scope.launch {
            val disk = TileCache.cachedIds()
            if (disk == ids) return@launch
            ids = disk
            bump()
        }
    }

    /**
     * The hexagons to draw for [region]: everything this phone or the device
     * holds that the viewport touches, each carrying whether the DEVICE has it.
     *
     * Anything the device has but this phone has no data for is included too —
     * coverage the rider knows about must not vanish because the blob lives
     * somewhere else.
     */
    fun visibleContent(region: BoundingBox, synced: Set<String>): List<OutlineHex> {
        val all = ids + synced
        if (all.isEmpty()) return emptyList()

        // Pad by a hex so an area half off-screen still draws to the edge.
        val padded = region.expanded(0.06)

        val out = ArrayList<OutlineHex>(all.size)
        for (id in all) {
            val t = geometry(id) ?: continue
            if (!BoundingBox(t.south, t.west, t.north, t.east).intersects(padded)) continue
            out.add(OutlineHex(t.id, t.hexagon, t.center, t.id in synced))
        }
        return out
    }

    /** Called after a download so freshly built areas appear without a restart. */
    fun noteDownloaded(newIds: List<String>) {
        if (newIds.isEmpty()) return
        ids = ids + newIds
        bump()
    }

    /**
     * H3 id -> hexagon, memoised. Every id is a fixed cell on the globe, so this
     * never needs invalidating.
     */
    private val geometryCache = HashMap<String, MapTile?>()

    private fun geometry(id: String): MapTile? =
        geometryCache.getOrPut(id) { H3Tiles.tile(id) }
}
