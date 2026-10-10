package com.raemond.opentrailpaper.map

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import com.raemond.opentrailpaper.BuildConfig
import java.io.File

/**
 * What this phone sent to each device, per hex: the version record of the tile
 * (.ebm) and of its .poi — "cdn:<hash>" for a file from the pre-built set,
 * "phone:<secs>" for one built here from Overpass ([PrebuiltTiles.cdnRecord] /
 * [PrebuiltTiles.phoneRecord]). Written when the device acknowledges the save,
 * keyed by device identity (the device address, like the map-layer store), and
 * compared with the CDN index to tell current hexes from updates
 * (docs/prebuilt-tiles.md#versions).
 *
 * A tab-separated file (`device kind id record` per line) rather than
 * SharedPreferences: a device can hold thousands of hexes. Not thread-safe; the
 * BLE manager uses it on the main thread.
 */
class DeviceTileVersions(private val file: File) {
    private val tiles = HashMap<String, HashMap<String, String>>()
    private val pois = HashMap<String, HashMap<String, String>>()
    /** Changed since the last [save]. */
    var dirty = false; private set

    init {
        if (file.exists()) {
            runCatching {
                file.forEachLine { line ->
                    val p = line.split('\t')
                    if (p.size == 4) (if (p[1] == "p") pois else tiles).getOrPut(p[0]) { HashMap() }[p[2]] = p[3]
                }
            }
        }
    }

    fun tiles(device: String?): Map<String, String> = device?.let { tiles[it] } ?: emptyMap()
    fun pois(device: String?): Map<String, String> = device?.let { pois[it] } ?: emptyMap()
    fun tile(device: String?, id: String): String? = device?.let { tiles[it]?.get(id) }
    fun poi(device: String?, id: String): String? = device?.let { pois[it]?.get(id) }

    fun setTile(device: String?, id: String, record: String) {
        device ?: return
        tiles.getOrPut(device) { HashMap() }[id] = record
        dirty = true
    }

    fun setPoi(device: String?, id: String, record: String) {
        device ?: return
        pois.getOrPut(device) { HashMap() }[id] = record
        dirty = true
    }

    /** Forget hexes the device no longer lists (deleted on the card, so a later
     *  copy from elsewhere is not mistaken for ours). */
    fun pruneTiles(device: String?, keep: Set<String>) = prune(tiles, device, keep)
    fun prunePois(device: String?, keep: Set<String>) = prune(pois, device, keep)

    private fun prune(m: HashMap<String, HashMap<String, String>>, device: String?, keep: Set<String>) {
        val d = device?.let { m[it] } ?: return
        if (d.keys.retainAll(keep)) dirty = true
    }

    fun save() {
        val sb = StringBuilder()
        for ((kind, m) in listOf("t" to tiles, "p" to pois)) {
            for ((dev, ids) in m) for ((id, rec) in ids) {
                sb.append(dev).append('\t').append(kind).append('\t').append(id).append('\t').append(rec).append('\n')
            }
        }
        runCatching {
            file.parentFile?.mkdirs()
            writeAtomic(file, sb.toString().toByteArray())
            dirty = false
        }
    }
}

/**
 * The CDN's group indexes for the hexes on screen, for the Maps screen's
 * "update available" state and the selection's data date. Backed by
 * [PrebuiltTiles.IndexCache] (an hour, index.json's max-age) and meta.json (OSM
 * snapshot per fragment, also an hour). When the CDN cannot be reached it knows
 * nothing for five minutes, and every hex falls back to the heuristic.
 * Main thread only.
 */
object CdnVersions {
    /** Bumped whenever what is known changes, so composables can redraw. */
    var revision by mutableStateOf(0); private set

    private val groups = HashMap<String, PrebuiltTiles.Index>()
    private val groupsAt = HashMap<String, Long>()
    private val inFlight = HashSet<String>()
    private var meta: Map<String, Long> = emptyMap()
    private var metaAt = 0L
    private var failedAt = 0L
    private const val RETRY_AFTER_MS = 5L * 60 * 1000
    /** Groups looked up per call: a zoomed-out view over a big download should
     *  not fetch a country's worth of indexes at once. */
    private const val MAX_GROUPS = 64

    /** Forget everything (tests). */
    fun clear() {
        groups.clear(); groupsAt.clear(); inFlight.clear()
        meta = emptyMap(); metaAt = 0L; failedAt = 0L
    }

    /** The index entry for [id], if its group's index is known. */
    fun entry(id: String): PrebuiltTiles.Entry? = groups[id.take(6)]?.cells?.get(id)

    /** Fetch the indexes for [ids] that are not known or are over an hour old,
     *  and meta.json. */
    suspend fun load(ids: Collection<String>, baseUrl: String = BuildConfig.TILE_BASE_URL) {
        if (baseUrl.isBlank()) return
        val now = System.currentTimeMillis()
        if (now - failedAt < RETRY_AFTER_MS) return
        val want = ids.map { it.take(6) }.toSet()
            .filter { it !in inFlight && now - (groupsAt[it] ?: 0L) >= PrebuiltTiles.IndexCache.TTL_MS }
            .sorted().take(MAX_GROUPS)
        val needMeta = now - metaAt >= PrebuiltTiles.IndexCache.TTL_MS
        if (want.isEmpty() && !needMeta) return
        inFlight += want
        val cdn = PrebuiltTiles(baseUrl)
        val got = try { cdn.groupIndexes(want) } finally { inFlight -= want.toSet() }
        if (!cdn.enabled) {
            // Unreachable: forget what is known, so nothing claims "current" from
            // an index that may have moved on.
            failedAt = System.currentTimeMillis()
            groups.clear(); groupsAt.clear()
        } else {
            val at = System.currentTimeMillis()
            for (g in want) {
                got[g]?.let { groups[g] = it } ?: groups.remove(g)
                groupsAt[g] = at
            }
            // Tried once an hour, answered or not, so a failing meta.json cannot
            // turn the revision bump below into a refetch loop.
            if (needMeta) { cdn.fetchMeta()?.let { meta = it }; metaAt = at }
        }
        revision++
    }

    /** The OSM snapshot (epoch ms) the CDN's tiles for [ids] were built from: the
     *  oldest of their groups' fragments. null until meta.json and the indexes load. */
    fun dataDate(ids: Collection<String>): Long? =
        PrebuiltTiles.dataDate(ids.map { it.take(6) }.toSet().mapNotNull { groups[it] }, meta)
}
