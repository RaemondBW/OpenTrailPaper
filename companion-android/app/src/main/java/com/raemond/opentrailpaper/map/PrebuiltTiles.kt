package com.raemond.opentrailpaper.map

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitAll
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.Semaphore
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.sync.withPermit
import kotlinx.coroutines.withContext
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL

/**
 * Ready-made tiles from the CDN (docs/prebuilt-tiles.md), tried before the
 * on-phone Overpass build.
 *
 * Layout under [baseUrl] (`…/v1/`), with g = the first 6 characters of a res-6
 * H3 id (exactly its res-3 ancestor):
 *   g/index.json   {"v":1,"cells":{"<id>":[ebmSize,"ebmHash",poiSize,"poiHash"],…}}
 *   g/<id>.ebm?v=<ebmHash>   g/<id>.poi?v=<poiHash>
 * A hex missing from its group's index (or a group with no index) was not
 * built: it goes to Overpass. ebmSize 0 = built and empty (nothing to draw);
 * poiSize 0 = no POIs. The bytes are the same ones the phone would build
 * (tools/tiles/test/equivalence.sh), so they are cached exactly like built ones.
 *
 * One instance per download, so the map and POI passes share the index fetches.
 * An empty [baseUrl] disables it; if the host cannot be reached at all (the
 * domain is not live yet, no network) the rest of the download skips it.
 */
class PrebuiltTiles(
    baseUrl: String,
    private val fetcher: Fetcher = HttpFetcher,
    private val concurrency: Int = 6,
    private val indexTimeoutMs: Int = 8_000,
    private val fileTimeoutMs: Int = 20_000,
) {
    class Response(val status: Int, val body: ByteArray)

    /** Blocking GET; throws IOException when no HTTP answer arrives. */
    fun interface Fetcher {
        fun get(url: String, timeoutMs: Int): Response
    }

    data class Entry(val ebmSize: Int, val ebmHash: String, val poiSize: Int, val poiHash: String)

    /** [tiles]: verified .ebm blobs; [empty]: built and empty (no Overpass);
     *  [fallback]: not served, build these on the phone. */
    class TileResult(
        val tiles: List<Pair<String, ByteArray>>,
        val empty: List<String>,
        val fallback: List<String>,
    )

    /** [files]: verified .poi files; [none]: built with no POIs (synthesise the
     *  empty file); [fallback]: ask Overpass. */
    class PoiResult(
        val files: List<Pair<String, ByteArray>>,
        val none: List<String>,
        val fallback: List<String>,
    )

    private val base = baseUrl.trim().let { if (it.isEmpty() || it.endsWith("/")) it else "$it/" }
    private val indexes = HashMap<String, Map<String, Entry>?>()
    private val lock = Any()
    /** The map and POI passes run at once; one of them fetches a group's index. */
    private val indexMutex = Mutex()
    @Volatile private var unreachable = base.isEmpty()
    private val host = runCatching { URL(base).host }.getOrDefault(base)

    val enabled: Boolean get() = !unreachable

    /** The group indexes for [ids], fetched (once each) [concurrency] at a time. */
    private suspend fun entries(ids: List<String>): Map<String, Entry> {
        if (unreachable) return emptyMap()
        val groups = ids.map { it.take(6) }.toSet()
        indexMutex.withLock {
            val need = synchronized(lock) { groups.filter { it !in indexes } }
            val gate = Semaphore(concurrency)
            coroutineScope {
                need.map { g -> async { gate.withPermit { g to fetchIndex(g) } } }.awaitAll()
            }.let { got -> synchronized(lock) { for ((g, m) in got) indexes[g] = m } }
        }
        val out = HashMap<String, Entry>()
        synchronized(lock) {
            for (id in ids) indexes[id.take(6)]?.get(id)?.let { out[id] = it }
        }
        return out
    }

    private suspend fun fetchIndex(g: String): Map<String, Entry>? {
        if (unreachable) return null
        val r = get("cdn-index", "$base$g/index.json", indexTimeoutMs) ?: return null
        if (r.status != 200) return null
        return runCatching { parseIndex(r.body.toString(Charsets.UTF_8)) }.getOrNull()
    }

    /** GET with stats; null (and the CDN switched off) when the host is unreachable. */
    private suspend fun get(kind: String, url: String, timeoutMs: Int): Response? {
        if (unreachable) return null
        val t0 = System.nanoTime()
        return try {
            val r = withContext(Dispatchers.IO) { fetcher.get(url, timeoutMs) }
            DownloadStats.request(kind, host, r.status, (System.nanoTime() - t0) / 1e9, r.body.size.toLong())
            r
        } catch (e: IOException) {
            DownloadStats.request(kind, host, -1, (System.nanoTime() - t0) / 1e9, 0)
            // No answer from the index at all: the domain is not live or there is
            // no network — don't spend a timeout per group on it.
            if (kind == "cdn-index") unreachable = true
            null
        }
    }

    suspend fun fetchTiles(ids: List<String>): TileResult = fetchFiles(ids, ".ebm", EBM_MAGIC).let { (got, empty, fb) ->
        TileResult(got, empty, fb)
    }

    suspend fun fetchPois(ids: List<String>): PoiResult = fetchFiles(ids, ".poi", POI_MAGIC).let { (got, none, fb) ->
        PoiResult(got, none, fb)
    }

    private suspend fun fetchFiles(
        ids: List<String>, ext: String, magic: ByteArray,
    ): Triple<List<Pair<String, ByteArray>>, List<String>, List<String>> {
        if (ids.isEmpty()) return Triple(emptyList(), emptyList(), emptyList())
        val index = entries(ids)
        val isEbm = ext == ".ebm"
        val empty = ArrayList<String>()
        val fallback = ArrayList<String>()
        val want = ArrayList<Pair<String, Entry>>()
        for (id in ids) {
            val e = index[id]
            when {
                e == null -> fallback.add(id)
                (if (isEbm) e.ebmSize else e.poiSize) == 0 -> empty.add(id)
                else -> want.add(id to e)
            }
        }
        val gate = Semaphore(concurrency)
        val results = coroutineScope {
            want.map { (id, e) ->
                async {
                    gate.withPermit {
                        val size = if (isEbm) e.ebmSize else e.poiSize
                        val hash = if (isEbm) e.ebmHash else e.poiHash
                        val r = get(if (isEbm) "cdn-tile" else "cdn-poi",
                            "$base${id.take(6)}/$id$ext?v=$hash", fileTimeoutMs)
                        id to r?.takeIf { valid(it, size, magic) }?.body
                    }
                }
            }.awaitAll()
        }
        val got = ArrayList<Pair<String, ByteArray>>()
        for ((id, body) in results) if (body != null) got.add(id to body) else fallback.add(id)
        return Triple(got, empty, fallback)
    }

    companion object {
        val EBM_MAGIC = "EBM2".toByteArray(Charsets.US_ASCII)
        val POI_MAGIC = "EPOI".toByteArray(Charsets.US_ASCII)

        fun valid(r: Response, size: Int, magic: ByteArray): Boolean =
            r.status == 200 && r.body.size == size && r.body.size >= 4 &&
                (0 until 4).all { r.body[it] == magic[it] }

        /**
         * `{"v":1,"cells":{"<id>":[n,"h",n,"h"],…}}` — a fixed shape, parsed by
         * hand so it runs in JVM unit tests too (org.json is a stub there).
         */
        fun parseIndex(s: String): Map<String, Entry> {
            val cellsAt = s.indexOf("\"cells\"")
            require(cellsAt >= 0) { "no cells" }
            val re = Regex("\"([0-9a-fA-F]{15})\"\\s*:\\s*\\[\\s*(\\d+)\\s*,\\s*\"([0-9a-fA-F]*)\"\\s*,\\s*(\\d+)\\s*,\\s*\"([0-9a-fA-F]*)\"\\s*]")
            val out = HashMap<String, Entry>()
            for (m in re.findAll(s, cellsAt)) {
                val (id, es, eh, ps, ph) = m.destructured
                out[id] = Entry(es.toInt(), eh, ps.toInt(), ph)
            }
            return out
        }

        /** HttpURLConnection with the app's User-Agent (gzip is decoded transparently). */
        val HttpFetcher = Fetcher { url, timeoutMs ->
            val conn = (URL(url).openConnection() as HttpURLConnection).apply {
                setRequestProperty("User-Agent", MapBuilder.USER_AGENT)
                connectTimeout = timeoutMs
                readTimeout = timeoutMs
            }
            try {
                val code = conn.responseCode
                val body = if (code == 200) conn.inputStream.use { it.readBytes() } else ByteArray(0)
                Response(code, body)
            } finally {
                conn.disconnect()
            }
        }
    }
}
