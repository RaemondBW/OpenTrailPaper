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
 * Each blob carries its version in a `<id>.ver` sidecar: "cdn:<hash>" for a
 * tile from the pre-built set, "phone:<secs>" for one built here from Overpass
 * (PrebuiltTiles.cdnRecord / phoneRecord). When the CDN index has the hex, only
 * a copy with exactly its current hash is reused, at any age; a stale or
 * phone-built copy is fetched again. Without an index entry the old rule
 * applies: any copy younger than [MAX_AGE_MS].
 *
 * Lives in `filesDir`, NOT the cache directory. It started as a build cache on
 * the theory that it is reconstructible from the network — true, but an eviction
 * would silently un-download areas the rider chose, with an Overpass round-trip
 * to get them back.
 */
object TileCache {

    /** What [partition] found: reusable blobs, their version records, and the
     *  ids that still need fetching. */
    class Cached(
        val tiles: List<Pair<String, ByteArray>>,
        val versions: Map<String, String>,
        val missing: List<String>,
    )

    /** Tiles older than this are re-fetched, so OSM edits eventually land. */
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
        // v3: tiles carry the bike-route / cycleway / bike-lane way flags.
        dir = File(context.filesDir, "tiles/TileCache-v3").apply { mkdirs() }
        // v2 tiles move in already EXPIRED (mtime 1970): data() never reuses
        // one, so the next sync of that area rebuilds it with the flags, but
        // cachedIds() still lists it, so the map keeps showing the areas this
        // phone downloaded. trim() drops them first.
        val old = File(context.filesDir, "tiles/TileCache-v2")
        old.listFiles()?.filter { it.extension == "ebm" }?.forEach { f ->
            val to = File(dir, f.name)
            if (!to.exists() && f.renameTo(to)) to.setLastModified(0)
        }
        old.deleteRecursively()
        PoiCache.init(context)
    }

    /** Tests: use [d] instead of the app's directory. */
    fun initDir(d: File) { dir = d.apply { mkdirs() } }

    private fun fileFor(id: String): File {
        // Ids are hex H3 strings, so they are already filename-safe; filter
        // anyway so a malformed id can never escape the directory.
        val safe = id.filter { it.isDigit() || it in 'a'..'f' || it in 'A'..'F' }
        return File(dir, "$safe.ebm")
    }

    private fun verFor(id: String): File = File(dir, fileFor(id).nameWithoutExtension + ".ver")

    /**
     * Cached blob for [id] with its version, if a send may reuse it: with
     * [cdnHash] only the copy of exactly that hash (any age); without, any copy
     * younger than [MAX_AGE_MS] when [allowAged].
     */
    suspend fun reusable(id: String, cdnHash: String?, allowAged: Boolean): Pair<ByteArray, String>? =
        withContext(Dispatchers.IO) {
            reusableFile(fileFor(id), verFor(id), cdnHash, allowAged, MAX_AGE_MS)
                ?.takeIf { it.first.isNotEmpty() }
        }

    /** Every H3 id this phone holds tile data for — the areas the map shows as
     *  downloaded. */
    suspend fun cachedIds(): Set<String> = withContext(Dispatchers.IO) {
        (dir.listFiles() ?: emptyArray())
            .filter { it.extension == "ebm" }
            .map { it.nameWithoutExtension }
            .toSet()
    }

    suspend fun store(id: String, data: ByteArray, version: String) = withContext(Dispatchers.IO) {
        if (data.isEmpty()) return@withContext
        lock.withLock {
            // Write-then-rename: a process death mid-write would otherwise leave
            // a truncated blob that decodes to a half-drawn area.
            writeAtomic(fileFor(id), data)
            writeAtomic(verFor(id), version.toByteArray())
        }
    }

    /** [versions]: each tile's record; a tile without one was built on this phone now. */
    suspend fun store(tiles: List<Pair<String, ByteArray>>, versions: Map<String, String> = emptyMap()) {
        val phone = PrebuiltTiles.phoneRecord()
        for ((id, data) in tiles) store(id, data, versions[id] ?: phone)
    }

    /**
     * Which of [ids] can be sent from the cache (with their versions), and which
     * still need fetching. [cdnHashes]: the CDN's current .ebm hash for the hexes
     * it has (see [reusable]); [allowAged] false (a Redownload) reuses only exact
     * CDN copies.
     */
    suspend fun partition(
        ids: List<String>,
        cdnHashes: Map<String, String> = emptyMap(),
        allowAged: Boolean = true,
    ): Cached {
        val hit = mutableListOf<Pair<String, ByteArray>>()
        val versions = HashMap<String, String>()
        val miss = mutableListOf<String>()
        for (id in ids) {
            val r = reusable(id, cdnHashes[id], allowAged)
            if (r != null) { hit.add(id to r.first); versions[id] = r.second } else miss.add(id)
        }
        return Cached(hit, versions, miss)
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

/** Write-then-rename, so a process death mid-write never leaves a truncated file. */
internal fun writeAtomic(target: File, data: ByteArray) {
    val tmp = File(target.parentFile, "${target.name}.tmp")
    tmp.writeBytes(data)
    if (!tmp.renameTo(target)) {
        target.writeBytes(data)
        tmp.delete()
    }
}

/** The shared reuse rule of TileCache / PoiCache (see TileCache.reusable). */
internal fun reusableFile(f: File, ver: File, cdnHash: String?, allowAged: Boolean, maxAgeMs: Long): Pair<ByteArray, String>? {
    if (!f.exists()) return null
    val version = if (ver.exists()) ver.readText() else PrebuiltTiles.phoneRecord(f.lastModified())
    if (cdnHash != null) {
        if (version != PrebuiltTiles.cdnRecord(cdnHash)) return null
    } else if (!allowAged || System.currentTimeMillis() - f.lastModified() >= maxAgeMs) {
        return null
    }
    return f.readBytes() to version
}
