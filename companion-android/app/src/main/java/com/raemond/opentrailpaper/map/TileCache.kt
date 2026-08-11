package com.raemond.opentrailpaper.map

import android.content.Context
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import java.io.File

/**
 * On-disk cache of built `.ebm` tile blobs, keyed by H3 cell id.
 *
 * Building a tile is expensive and fragile: an Overpass fetch (retried across
 * public mirrors, paced a second apart, and prone to 504s on busy servers), an
 * elevation fetch, then encoding roads, water, sea rings and parks. All of that
 * used to be thrown away the moment anything went wrong — a dropped BLE link
 * mid-send cleared the queue, and re-sending meant re-fetching and re-encoding
 * the whole area from scratch.
 *
 * Blobs are immutable for a given cell (the encoders are byte-identical across
 * mapgen.js / build_map.py / MapBuilder by design), so caching them by id is safe
 * and needs no invalidation beyond age.
 *
 * Lives in `filesDir`, NOT the cache directory. It started as a build cache on
 * the theory that it is reconstructible from the network — true, but it is no
 * longer only that: these blobs are what the map draws downloaded areas from
 * ([EInkTileStore]), so an eviction would silently blank out areas the user
 * downloaded, with an Overpass round-trip to get them back.
 */
object TileCache {

    /**
     * Tiles older than this are re-fetched, so OSM edits eventually land.
     * Applies to REUSE only — [displayData] ignores it, since drawing a slightly
     * stale area beats drawing nothing.
     */
    private const val MAX_AGE_MS = 90L * 24 * 60 * 60 * 1000   // 90 days

    /** Rough ceiling before the oldest entries are dropped. */
    private const val MAX_BYTES = 256L * 1024 * 1024

    private lateinit var dir: File
    private val lock = Mutex()

    fun init(context: Context) {
        // Versioned. Bump when a builder change alters what a tile SHOULD
        // contain, so stale blobs cannot mask the fix. v2 matches the iOS cache
        // generation: tiles built before the padded-coastline fetch have no sea
        // fill, and reusing one would look exactly like the bug still being there.
        dir = File(context.filesDir, "tiles/TileCache-v2").apply { mkdirs() }
    }

    private fun fileFor(id: String): File {
        // Ids are hex H3 strings, so they are already filename-safe; filter
        // anyway so a malformed id can never escape the directory.
        val safe = id.filter { it.isDigit() || it in 'a'..'f' || it in 'A'..'F' }
        return File(dir, "$safe.ebm")
    }

    /** Cached blob for [id], or null if absent or too old. */
    suspend fun data(id: String): ByteArray? = withContext(Dispatchers.IO) {
        val f = fileFor(id)
        if (!f.exists()) return@withContext null
        if (System.currentTimeMillis() - f.lastModified() >= MAX_AGE_MS) return@withContext null
        f.readBytes().takeIf { it.isNotEmpty() }
    }

    /**
     * Blob for [id] with no age check, for DRAWING the area on the map.
     * [data] expires tiles so a rebuild picks up OSM edits; expiring what the map
     * draws would instead make downloaded areas quietly disappear.
     */
    suspend fun displayData(id: String): ByteArray? = withContext(Dispatchers.IO) {
        runCatching { fileFor(id).readBytes() }.getOrNull()?.takeIf { it.isNotEmpty() }
    }

    /**
     * Every H3 id this phone holds tile data for — the set of areas the map can
     * draw in the device's own style.
     */
    suspend fun cachedIds(): Set<String> = withContext(Dispatchers.IO) {
        (dir.listFiles() ?: emptyArray())
            .filter { it.extension == "ebm" }
            .map { it.nameWithoutExtension }
            .toSet()
    }

    suspend fun store(id: String, data: ByteArray) = withContext(Dispatchers.IO) {
        if (data.isEmpty()) return@withContext
        lock.withLock {
            // Write-then-rename: a process death mid-write would otherwise leave
            // a truncated blob that decodes to a half-drawn area.
            val target = fileFor(id)
            val tmp = File(target.parentFile, "${target.name}.tmp")
            tmp.writeBytes(data)
            if (!tmp.renameTo(target)) {
                target.writeBytes(data)
                tmp.delete()
            }
        }
    }

    suspend fun store(tiles: List<Pair<String, ByteArray>>) {
        for ((id, data) in tiles) store(id, data)
    }

    /** Which of [ids] are already built, and which still need fetching. */
    suspend fun partition(ids: List<String>): Pair<List<Pair<String, ByteArray>>, List<String>> {
        val hit = mutableListOf<Pair<String, ByteArray>>()
        val miss = mutableListOf<String>()
        for (id in ids) {
            val d = data(id)
            if (d != null) hit.add(id to d) else miss.add(id)
        }
        return hit to miss
    }

    /** Total bytes held, for the Settings readout. */
    suspend fun sizeBytes(): Long = withContext(Dispatchers.IO) {
        (dir.listFiles() ?: emptyArray()).sumOf { it.length() }
    }

    suspend fun clear() = withContext(Dispatchers.IO) {
        dir.deleteRecursively()
        dir.mkdirs()
        Unit
    }

    /** Drop the oldest entries until the cache is back under [MAX_BYTES]. */
    suspend fun trim() = withContext(Dispatchers.IO) {
        val files = dir.listFiles() ?: return@withContext
        var total = files.sumOf { it.length() }
        if (total <= MAX_BYTES) return@withContext
        for (f in files.sortedBy { it.lastModified() }) {
            if (total <= MAX_BYTES) break
            val size = f.length()
            if (f.delete()) total -= size
        }
    }
}
