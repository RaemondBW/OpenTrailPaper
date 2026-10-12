package com.raemond.opentrailpaper.map

import android.content.Context
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.File

/**
 * On-disk cache of built cycling-POI files (`<h3>.poi`), kept apart from the
 * map tiles so POIs can be fetched, refreshed and sent on their own: a POI
 * query is ~1% of a map query, and water points change more often than roads.
 *
 * Entries older than [MAX_AGE_MS] are rebuilt from Overpass the next time the
 * area is synced — the same period after which a .poi already on the device is
 * re-sent (BleManager.poiNeedsSend) — unless the CDN index has the cell: then
 * only a copy with its current .poi hash is reused (`.ver` sidecars, as in
 * TileCache). Initialised by [TileCache.init].
 */
object PoiCache {

    /** POIs older than this are re-fetched and re-sent when the area is next synced. */
    const val MAX_AGE_MS = 30L * 24 * 60 * 60 * 1000   // 30 days

    private lateinit var dir: File

    fun init(context: Context) {
        dir = File(context.filesDir, "tiles/PoiCache-v1").apply { mkdirs() }
    }

    /** Tests: use [d] instead of the app's directory. */
    fun initDir(d: File) { dir = d.apply { mkdirs() } }

    private fun stem(id: String) = id.filter { it.isDigit() || it in 'a'..'f' || it in 'A'..'F' }
    private fun fileFor(id: String): File = File(dir, stem(id) + ".poi")
    private fun verFor(id: String): File = File(dir, stem(id) + ".ver")

    /** Cached file for [id] with its version, if a send may reuse it (the rules
     *  of TileCache.reusable, with [MAX_AGE_MS]). */
    suspend fun reusable(id: String, cdnHash: String?, allowAged: Boolean): Pair<ByteArray, String>? =
        withContext(Dispatchers.IO) {
            reusableFile(fileFor(id), verFor(id), cdnHash, allowAged, MAX_AGE_MS)?.takeIf { it.first.size >= 40 }
        }

    /** [versions]: each file's record; one without was built on this phone now. */
    suspend fun store(files: List<Pair<String, ByteArray>>, versions: Map<String, String> = emptyMap()) =
        withContext(Dispatchers.IO) {
            val phone = PrebuiltTiles.phoneRecord()
            for ((id, data) in files) {
                if (data.isEmpty()) continue
                writeAtomic(fileFor(id), data)
                writeAtomic(verFor(id), (versions[id] ?: phone).toByteArray())
            }
        }

    /** Which of [ids] can be sent from the cache, and which still need fetching. */
    suspend fun partition(
        ids: List<String>,
        cdnHashes: Map<String, String> = emptyMap(),
        allowAged: Boolean = true,
    ): TileCache.Cached {
        val hit = mutableListOf<Pair<String, ByteArray>>()
        val versions = HashMap<String, String>()
        val miss = mutableListOf<String>()
        for (id in ids) {
            val r = reusable(id, cdnHashes[id], allowAged)
            if (r != null) { hit.add(id to r.first); versions[id] = r.second } else miss.add(id)
        }
        return TileCache.Cached(hit, versions, miss)
    }
}
