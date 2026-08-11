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
import kotlinx.coroutines.withContext

/** A downloaded area with geometry ready to draw. */
class EInkArea(
    val id: String,                    // H3 cell id
    val hexagon: List<LatLon>,
    val center: LatLon,
    val synced: Boolean,               // the device has it too -> green check
    val tile: Ebm.Tile,
)

/**
 * A downloaded area we can't paint right now — its geometry is still decoding,
 * it's one of many at a far-out zoom, or the phone no longer holds its tile data
 * (cache cleared, or another phone sent it). Outlined rather than dropped, so
 * coverage the user knows about never just vanishes.
 */
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
 * Holds decoded map geometry for the areas this phone has downloaded, so the map
 * can draw them in the device's own e-ink style.
 *
 * Two separate facts, deliberately kept apart:
 *  * [ids] — every area we hold tile DATA for. This is what "downloaded" means on
 *    the map, and it comes from [TileCache] (the same blobs streamed to the device).
 *  * `BleManager.deviceTileIds` — what the DEVICE has. The intersection is what
 *    earns a green check.
 *
 * Decoding happens off the main thread; the renderer never touches the disk, so
 * only tiles already decoded get an overlay. A tile appears the moment its decode
 * lands.
 */
object EInkTileStore {

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)

    /**
     * Bumps whenever the drawable set changes, so screens re-diff their overlays.
     *
     * COALESCED — see [bump]. A downloading run finishes tiles continuously and
     * each one publishing immediately would rebuild every overlay on the map
     * several times a second, which with a few hundred hexes on screen is
     * visible stutter.
     */
    var version by mutableStateOf(0); private set

    private var bumpPending = false

    /** Publish at most ~4x/sec. Tiles land far faster than that and no rider can
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

    private val loaded = HashMap<String, Ebm.Tile>()
    private val loading = HashSet<String>()
    private val recency = ArrayList<String>()   // LRU, most recent last
    private var heldPoints = 0

    /**
     * Roughly 20 MB of geometry. Well past a screenful of res-6 hexes; the LRU
     * only bites when panning across a large downloaded region.
     */
    private const val POINT_BUDGET = 1_200_000

    /**
     * Fewer tiles drawn in full at once than the eye can use. Each one decodes
     * off-main and then publishes, and at 80 a large download kept a continuous
     * stream of decodes and overlay rebuilds running. Beyond ~24 hexes on screen
     * the ink is too small to read anyway; the rest outline, which costs nothing.
     */
    private const val PREVIEW_LIMIT = 24

    /** Re-read which areas exist on disk. Cheap (one directory listing). */
    fun refresh() {
        scope.launch {
            val disk = TileCache.cachedIds()
            if (disk == ids) return@launch
            ids = disk
            // Drop geometry for areas that vanished (cache cleared).
            for (gone in loaded.keys.toList()) if (gone !in disk) evict(gone)
            bump()
        }
    }

    /** Decoded geometry, or null if it hasn't been decoded yet. */
    fun tile(id: String): Ebm.Tile? = loaded[id]

    /**
     * What the map should draw for [region], split into areas we can render in
     * the device's style and ones we can only outline.
     *
     * [synced] is the device's own tile list, so the two states the user asked
     * about — downloaded, and downloaded *and* on the device — are decided in one
     * place. Anything the device has but this phone has no data for still gets an
     * outline: coverage the user knows about must not vanish just because the
     * preview can't be drawn.
     */
    fun visibleContent(
        region: BoundingBox,
        synced: Set<String>,
    ): Pair<List<EInkArea>, List<OutlineHex>> {
        val all = ids + synced
        if (all.isEmpty()) return emptyList<EInkArea>() to emptyList()

        // Pad by a hex so an area half off-screen still draws to the edge.
        val padded = region.expanded(0.06)
        val center = region.center

        val near = ArrayList<Pair<MapTile, Double>>()
        for (id in all) {
            val t = geometry(id) ?: continue
            if (!BoundingBox(t.south, t.west, t.north, t.east).intersects(padded)) continue
            val dLat = t.center.lat - center.lat
            val dLon = t.center.lon - center.lon
            near.add(t to (dLat * dLat + dLon * dLon))
        }
        // Zoomed out far enough to see hundreds of areas, the ink is sub-pixel
        // anyway. Draw the nearest in full and outline the rest rather than
        // silently dropping them.
        near.sortBy { it.second }
        val drawable = near.take(PREVIEW_LIMIT).map { it.first }
        val rest = near.drop(PREVIEW_LIMIT).map { it.first }

        ensureLoaded(drawable.filter { it.id in ids }.map { it.id })

        val areas = ArrayList<EInkArea>(drawable.size)
        val outlines = ArrayList<OutlineHex>(rest.size)
        for (t in drawable) {
            val geo = loaded[t.id]
            if (geo != null) {
                areas.add(EInkArea(t.id, t.hexagon, t.center, t.id in synced, geo))
            } else {
                outlines.add(OutlineHex(t.id, t.hexagon, t.center, t.id in synced))
            }
        }
        for (t in rest) {
            outlines.add(OutlineHex(t.id, t.hexagon, t.center, t.id in synced))
        }
        return areas to outlines
    }

    /**
     * H3 id -> hexagon, memoised. Every id is a fixed cell on the globe, so this
     * never needs invalidating.
     */
    private val geometryCache = HashMap<String, MapTile?>()

    private fun geometry(id: String): MapTile? =
        geometryCache.getOrPut(id) { H3Tiles.tile(id) }

    /**
     * Ask for these areas to be available to draw. Already-loaded ids are just
     * marked recently used; the rest are decoded in the background.
     */
    fun ensureLoaded(wanted: List<String>) {
        for (id in wanted) {
            touch(id)
            if (loaded.containsKey(id) || id in loading || id !in ids) continue
            loading.add(id)
            scope.launch {
                val tile = withContext(Dispatchers.Default) {
                    TileCache.displayData(id)?.let { Ebm.decode(it) }
                }
                finishLoad(id, tile)
            }
        }
    }

    /** Called after a download so freshly built areas draw without a restart. */
    fun noteDownloaded(newIds: List<String>) {
        if (newIds.isEmpty()) return
        ids = ids + newIds
        // Re-decode rather than trusting a stale in-memory copy: a rebuilt area
        // can legitimately differ from the one we already drew.
        for (id in newIds) evict(id)
        bump()
    }

    private fun finishLoad(id: String, tile: Ebm.Tile?) {
        loading.remove(id)
        if (tile == null) {
            // Undecodable (truncated write, format drift): forget it so we don't
            // retry every pan, and so it draws as plain map underneath.
            ids = ids - id
            bump()
            return
        }
        loaded[id] = tile
        heldPoints += tile.pointCount
        touch(id)
        trim()
        bump()
    }

    private fun touch(id: String) {
        recency.remove(id)
        recency.add(id)
    }

    private fun evict(id: String) {
        loaded.remove(id)?.let { heldPoints -= it.pointCount }
        recency.remove(id)
    }

    /**
     * Drop least-recently-wanted geometry until back inside the budget. Never
     * drops the most recent entries, which are what's on screen right now.
     */
    private fun trim() {
        var i = 0
        while (heldPoints > POINT_BUDGET && i < recency.size && recency.size > 4) {
            val victim = recency[i]
            if (loaded.containsKey(victim)) evict(victim) else i += 1
        }
    }
}
