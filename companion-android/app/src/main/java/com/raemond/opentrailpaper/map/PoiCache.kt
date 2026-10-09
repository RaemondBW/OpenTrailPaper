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
 * re-sent (BleManager.poiNeedsSend). Initialised by [TileCache.init].
 */
object PoiCache {

    /** POIs older than this are re-fetched and re-sent when the area is next synced. */
    const val MAX_AGE_MS = 30L * 24 * 60 * 60 * 1000   // 30 days

    private lateinit var dir: File

    fun init(context: Context) {
        dir = File(context.filesDir, "tiles/PoiCache-v1").apply { mkdirs() }
    }

    private fun fileFor(id: String): File =
        File(dir, id.filter { it.isDigit() || it in 'a'..'f' || it in 'A'..'F' } + ".poi")

    /** Cached file for [id], or null if absent or older than [MAX_AGE_MS]. */
    suspend fun data(id: String): ByteArray? = withContext(Dispatchers.IO) {
        val f = fileFor(id)
        if (!f.exists() || System.currentTimeMillis() - f.lastModified() >= MAX_AGE_MS) {
            return@withContext null
        }
        f.readBytes().takeIf { it.size >= 40 }
    }

    suspend fun store(files: List<Pair<String, ByteArray>>) = withContext(Dispatchers.IO) {
        for ((id, data) in files) {
            if (data.isEmpty()) continue
            val target = fileFor(id)
            val tmp = File(target.parentFile, "${target.name}.tmp")
            tmp.writeBytes(data)
            if (!tmp.renameTo(target)) { target.writeBytes(data); tmp.delete() }
        }
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
}
