package com.raemond.opentrailpaper

import com.raemond.opentrailpaper.map.CdnVersions
import com.raemond.opentrailpaper.map.DeviceTileVersions
import com.raemond.opentrailpaper.map.PrebuiltTiles
import com.raemond.opentrailpaper.map.PrebuiltTiles.VersionState
import com.raemond.opentrailpaper.map.TileCache
import kotlinx.coroutines.runBlocking
import org.junit.After
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Before
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Test
import java.io.File
import java.net.InetAddress
import java.net.ServerSocket
import java.util.concurrent.ConcurrentLinkedQueue
import kotlin.concurrent.thread

/**
 * The CDN-first tile source (PrebuiltTiles) against a real HTTP server in the
 * test JVM: the layout and index of docs/prebuilt-tiles.md, every fallback.
 */
class PrebuiltTilesTest {

    private var server: ServerSocket? = null
    private val seen = ConcurrentLinkedQueue<String>()

    /**
     * Serve `files` (path -> bytes, path without the query) on a free port: a
     * minimal HTTP/1.1 server on java.net (the unit-test classpath is android.jar,
     * which has no com.sun.net.httpserver), one thread per connection.
     */
    private fun serve(files: Map<String, ByteArray>): String {
        val ss = ServerSocket(0, 64, InetAddress.getByName("127.0.0.1"))
        server = ss
        thread(isDaemon = true) {
            while (!ss.isClosed) {
                val sock = try { ss.accept() } catch (_: Exception) { break }
                thread(isDaemon = true) {
                    sock.use { s ->
                        val inp = s.getInputStream().bufferedReader(Charsets.ISO_8859_1)
                        val line = inp.readLine() ?: return@use
                        var ua: String? = null
                        while (true) {
                            val h = inp.readLine() ?: break
                            if (h.isEmpty()) break
                            if (h.lowercase().startsWith("user-agent:")) ua = h.substringAfter(':').trim()
                        }
                        val uri = line.split(' ')[1]
                        seen.add("$uri UA=$ua")
                        val body = files[uri.substringBefore('?')]
                        val out = s.getOutputStream()
                        val head = if (body == null) "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n"
                                   else "HTTP/1.1 200 OK\r\nContent-Length: ${body.size}\r\n"
                        out.write((head + "Connection: close\r\n\r\n").toByteArray(Charsets.ISO_8859_1))
                        if (body != null) out.write(body)
                        out.flush()
                    }
                }
            }
        }
        return "http://127.0.0.1:${ss.localPort}/v1/"
    }

    /** Serve a directory tree (the builder's output) as is. */
    private fun serveDir(root: File): String {
        val files = HashMap<String, ByteArray>()
        root.walkTopDown().filter { it.isFile }.forEach {
            files["/" + it.relativeTo(root).path.replace(File.separatorChar, '/')] = it.readBytes()
        }
        return serve(files)
    }

    @After fun stop() { server?.close() }
    @Before fun clearIndexCache() { PrebuiltTiles.IndexCache.clear(); CdnVersions.clear() }

    private fun blob(magic: String, n: Int, seed: Int) =
        ByteArray(n) { i -> if (i < 4) magic[i].code.toByte() else ((i * 31 + seed) and 0xff).toByte() }

    private val g = "862a10"
    private val a = "862a10007ffffff"   // tile + pois
    private val b = "862a1000fffffff"   // tile, no pois
    private val c = "862a10017ffffff"   // built, empty
    private val d = "862a1001fffffff"   // tile served short (corrupt)
    private val e = "862a10027ffffff"   // tile served with the wrong magic
    private val f = "862a1002fffffff"   // not in the index
    private val other = "862a33007ffffff"   // group without an index

    private fun fixture(): Pair<String, Map<String, ByteArray>> {
        val ta = blob("EBM2", 1000, 1); val pa = blob("EPOI", 52, 2)
        val tb = blob("EBM2", 700, 3)
        val td = blob("EBM2", 500, 4); val te = blob("XXXX", 400, 5)
        val index = """{"v":1,"cells":{
            "$a":[1000,"aaaa",52,"bbbb"],
            "$b":[700,"cccc",0,""],
            "$c":[0,"",0,""],
            "$d":[600,"dddd",0,""],
            "$e":[400,"eeee",0,""]}}"""
        val files = mapOf(
            "/v1/$g/index.json" to index.toByteArray(),
            "/v1/$g/$a.ebm" to ta, "/v1/$g/$a.poi" to pa,
            "/v1/$g/$b.ebm" to tb, "/v1/$g/$d.ebm" to td, "/v1/$g/$e.ebm" to te,
        )
        return serve(files) to files
    }

    @Test fun servesIndexedTilesAndFallsBackForTheRest() = runBlocking {
        val (base, files) = fixture()
        val cdn = PrebuiltTiles(base)
        val r = cdn.fetchTiles(listOf(a, b, c, d, e, f, other))
        assertEquals(setOf(a, b), r.tiles.map { it.first }.toSet())
        for ((id, bytes) in r.tiles) assertArrayEquals(files["/v1/$g/$id.ebm"], bytes)
        assertEquals(listOf(c), r.empty)
        assertEquals(setOf(d, e, f, other), r.fallback.toSet())
        // Hash on the URL; the app's User-Agent on every request.
        assertTrue(seen.any { it.startsWith("/v1/$g/$a.ebm?v=aaaa ") })
        assertTrue(seen.all { it.contains("UA=OpenTrailPaper/") })
        // POIs share the index: no second index fetch.
        val p = cdn.fetchPois(listOf(a, b, c, f))
        assertEquals(listOf(a), p.files.map { it.first })
        assertArrayEquals(files["/v1/$g/$a.poi"], p.files[0].second)
        assertEquals(setOf(b, c), p.none.toSet())
        assertEquals(listOf(f), p.fallback)
        assertEquals(1, seen.count { it.startsWith("/v1/$g/index.json") })
        assertTrue(cdn.enabled)
    }

    @Test fun unreachableHostFallsBackFastAndSwitchesOff() = runBlocking {
        val port = ServerSocket(0).use { it.localPort }   // nothing listens here
        val cdn = PrebuiltTiles("http://127.0.0.1:$port/v1/")
        val t0 = System.nanoTime()
        val r = cdn.fetchTiles(listOf(a, other))
        assertTrue((System.nanoTime() - t0) / 1e9 < 3.0)
        assertEquals(setOf(a, other), r.fallback.toSet())
        assertFalse(cdn.enabled)
        assertEquals(listOf(a), cdn.fetchPois(listOf(a)).fallback)
    }

    @Test fun emptyBaseUrlDisables() = runBlocking {
        val cdn = PrebuiltTiles("")
        assertFalse(cdn.enabled)
        assertEquals(listOf(a), cdn.fetchTiles(listOf(a)).fallback)
    }

    @Test fun parsesTheBuilderIndex() {
        val m = PrebuiltTiles.parseIndex(
            """{"v":1,"cells":{"862a33007ffffff":[91021,"4271255cafb108cc",0,""],""" +
                """"862a3300fffffff":[137922,"55e163fcbc98de3b",312,"0011aabb"]}}""",
        )
        assertEquals(PrebuiltTiles.Entry(91021, "4271255cafb108cc", 0, ""), m["862a33007ffffff"])
        assertEquals(312, m["862a3300fffffff"]!!.poiSize)
    }

    /**
     * End to end against the builder's real output (tools/tiles/build_region.mjs
     * + merge_index.mjs). Runs when OTP_TILE_FIXTURE (env or system property)
     * points at an output directory holding v1/.
     */
    @Test fun realBuilderOutput() = runBlocking {
        val dir = (System.getProperty("OTP_TILE_FIXTURE") ?: System.getenv("OTP_TILE_FIXTURE"))
            ?.let(::File)
        assumeTrue("OTP_TILE_FIXTURE not set", dir != null && File(dir, "v1").isDirectory)
        val base = serveDir(dir!!)
        val all = File(dir, "v1").listFiles()!!.filter { File(it, "index.json").isFile }.flatMap { gd ->
            PrebuiltTiles.parseIndex(File(gd, "index.json").readText()).keys
        }.sorted()
        assertTrue(all.isNotEmpty())
        val missingHex = "862a10007ffffff".takeIf { it !in all } ?: "8f2a10007ffffff"
        val cdn = PrebuiltTiles(base)
        val t0 = System.nanoTime()
        val r = cdn.fetchTiles(all + missingHex)
        val ms = (System.nanoTime() - t0) / 1e6
        assertEquals(listOf(missingHex), r.fallback)
        var bytes = 0L
        for ((id, data) in r.tiles) {
            assertArrayEquals(File(dir, "v1/${id.take(6)}/$id.ebm").readBytes(), data)
            bytes += data.size
        }
        val p = cdn.fetchPois(all)
        assertTrue(p.fallback.isEmpty())
        for ((id, data) in p.files) assertArrayEquals(File(dir, "v1/${id.take(6)}/$id.poi").readBytes(), data)
        println(
            "fixture: ${r.tiles.size} tiles (%.1f MB) + ${r.empty.size} empty in %.0f ms; ".format(bytes / 1048576.0, ms) +
                "${p.files.size} .poi served, ${p.none.size} without POIs; fallback ${r.fallback}",
        )
    }

    // MARK: versions (docs/prebuilt-tiles.md#versions)

    /** Index A, then next week's index B where [a]'s tile changed. */
    private fun versionIndex(aHash: String) = """{"v":1,"cells":{
        "$a":[1000,"$aHash",52,"bbbb"],
        "$b":[700,"cccc",0,""],
        "$c":[0,"",0,""]},"regions":["rhode-island","rhode-island.border"]}"""

    private fun serveVersions(aHash: String, seed: Int): String = serve(
        mapOf(
            "/v1/$g/index.json" to versionIndex(aHash).toByteArray(),
            "/v1/$g/$a.ebm" to blob("EBM2", 1000, seed), "/v1/$g/$a.poi" to blob("EPOI", 52, 2),
            "/v1/$g/$b.ebm" to blob("EBM2", 700, 3),
            "/v1/meta.json" to """{"version":"v1","fragments":{
                "rhode-island":{"region":"rhode-island","phase":"interior","osm":"2026-10-05T20:21:02Z","cells":3},
                "rhode-island.border":{"region":"rhode-island","phase":"border","osm":"2026-10-06T20:21:02Z"}}}""".toByteArray(),
        ),
    )

    @Test fun sameHashSkippedChangedHashResentPhoneBuiltIsUpdate() = runBlocking {
        val base = serveVersions("aaaa", 1)
        val cdn = PrebuiltTiles(base)
        val ids = listOf(a, b, c, f)
        val hashes = cdn.entries(ids).mapNotNull { (id, e) -> PrebuiltTiles.tileHash(e)?.let { id to it } }.toMap()
        val phone = PrebuiltTiles.phoneRecord(1_790_000_000_000)
        // a: sent from the CDN with today's hash; b: built on the phone;
        // c: built empty (nothing to compare); f: not pre-built.
        val records = mapOf(a to PrebuiltTiles.cdnRecord("aaaa"), b to phone, c to phone, f to phone)
        assertEquals(
            listOf(VersionState.CURRENT, VersionState.UPDATE, VersionState.UNKNOWN, VersionState.UNKNOWN),
            ids.map { PrebuiltTiles.state(records[it], hashes[it]) },
        )
        assertEquals(1_790_000_000_000, PrebuiltTiles.phoneBuiltAt(phone))
        // Redownload fetches only b (and rebuilds c, f as before); a is never requested.
        val need = PrebuiltTiles.needsSend(ids, records, hashes)
        assertEquals(listOf(b, c, f), need)
        val r = cdn.fetchTiles(need)
        assertEquals(listOf(b), r.tiles.map { it.first })
        assertTrue(seen.none { it.startsWith("/v1/$g/$a.ebm") })
        // POIs by their own hash; "" (no POIs) recorded as "cdn:" is current too.
        val poiHashes = cdn.entries(ids).mapNotNull { (id, e) -> PrebuiltTiles.poiHash(e)?.let { id to it } }.toMap()
        val poiRecords = mapOf(a to PrebuiltTiles.cdnRecord("bbbb"), b to PrebuiltTiles.cdnRecord(""), c to phone)
        assertEquals(listOf(c), PrebuiltTiles.needsSend(listOf(a, b, c), poiRecords, poiHashes))

        // Next week a's tile changed: it alone is re-sent.
        server?.close()
        val base2 = serveVersions("a2a2", 7)
        val cdn2 = PrebuiltTiles(base2)
        val hashes2 = cdn2.entries(ids).mapNotNull { (id, e) -> PrebuiltTiles.tileHash(e)?.let { id to it } }.toMap()
        val sent = records + (b to PrebuiltTiles.cdnRecord("cccc"))
        assertEquals(listOf(a), PrebuiltTiles.needsSend(listOf(a, b), sent, hashes2))
        assertEquals(VersionState.UPDATE, PrebuiltTiles.state(sent[a], hashes2[a]))
        val r2 = cdn2.fetchTiles(listOf(a))
        assertArrayEquals(blob("EBM2", 1000, 7), r2.tiles.single().second)
    }

    @Test fun cdnUnreachableLeavesEveryHexToTheHeuristic() = runBlocking {
        val port = ServerSocket(0).use { it.localPort }
        val cdn = PrebuiltTiles("http://127.0.0.1:$port/v1/")
        val ids = listOf(a, b)
        val entries = cdn.entries(ids)
        assertTrue(entries.isEmpty())
        val records = mapOf(a to PrebuiltTiles.cdnRecord("aaaa"), b to PrebuiltTiles.phoneRecord())
        assertTrue(ids.all { PrebuiltTiles.state(records[it], PrebuiltTiles.tileHash(entries[it])) == VersionState.UNKNOWN })
        assertEquals(ids, PrebuiltTiles.needsSend(ids, records, emptyMap()))
        // The Maps screen's store knows nothing either.
        CdnVersions.load(ids, "http://127.0.0.1:$port/v1/")
        assertNull(CdnVersions.entry(a))
    }

    @Test fun indexCachedAcrossDownloadsAndDataDateFromMeta() = runBlocking {
        val base = serveVersions("aaaa", 1)
        PrebuiltTiles(base).entries(listOf(a))
        PrebuiltTiles(base).entries(listOf(b))
        assertEquals(1, seen.count { it.startsWith("/v1/$g/index.json") })
        val idx = PrebuiltTiles(base).groupIndexes(listOf(a))[g]!!
        assertEquals(listOf("rhode-island", "rhode-island.border"), idx.regions)
        val meta = PrebuiltTiles(base).fetchMeta()!!
        assertEquals(java.time.Instant.parse("2026-10-05T20:21:02Z").toEpochMilli(), PrebuiltTiles.dataDate(listOf(idx), meta))
        // The Maps screen's store, against the same server.
        CdnVersions.load(listOf(a, b), base)
        assertEquals("aaaa", CdnVersions.entry(a)?.ebmHash)
        assertEquals(java.time.Instant.parse("2026-10-05T20:21:02Z").toEpochMilli(), CdnVersions.dataDate(listOf(a)))
    }

    @Test fun tileCacheReusesOnlyTheCurrentHash() = runBlocking {
        val dir = kotlin.io.path.createTempDirectory("tilecache").toFile()
        TileCache.initDir(dir)
        val da = blob("EBM2", 1000, 1)
        val db = blob("EBM2", 700, 3)
        TileCache.store(listOf(a to da), mapOf(a to PrebuiltTiles.cdnRecord("aaaa")))
        TileCache.store(listOf(b to db))                                  // phone-built
        val p1 = TileCache.partition(listOf(a, b), mapOf(a to "aaaa", b to "cccc"))
        assertEquals(listOf(a), p1.tiles.map { it.first })
        assertEquals(listOf(b), p1.missing)
        assertEquals(PrebuiltTiles.cdnRecord("aaaa"), p1.versions[a])
        assertTrue(TileCache.partition(listOf(a), mapOf(a to "a2a2")).tiles.isEmpty())   // stale hash
        val p3 = TileCache.partition(listOf(a, b))                         // CDN down: age rule
        assertEquals(2, p3.tiles.size)
        assertTrue(p3.versions[b]!!.startsWith("phone:"))
        assertEquals(listOf(a), TileCache.partition(listOf(a, b), mapOf(a to "aaaa"), allowAged = false).tiles.map { it.first })
        dir.deleteRecursively()
        Unit
    }

    @Test fun deviceRecordsPersistPerDevice() {
        val file = File.createTempFile("versions", ".tsv")
        val v = DeviceTileVersions(file)
        v.setTile("AA:BB", a, PrebuiltTiles.cdnRecord("aaaa"))
        v.setTile("AA:BB", b, PrebuiltTiles.phoneRecord(1_790_000_000_000))
        v.setPoi("AA:BB", a, PrebuiltTiles.cdnRecord(""))
        v.setTile("CC:DD", a, PrebuiltTiles.phoneRecord(1_790_000_000_000))
        v.setTile(null, c, "cdn:x")                                         // no device: ignored
        v.pruneTiles("AA:BB", setOf(a))
        assertTrue(v.dirty)
        v.save()
        val w = DeviceTileVersions(file)
        assertEquals(mapOf(a to "cdn:aaaa"), w.tiles("AA:BB"))
        assertEquals("cdn:", w.poi("AA:BB", a))
        assertEquals("phone:1790000000", w.tile("CC:DD", a))
        assertNull(w.tile(null, a))
        file.delete()
    }

    @Test fun parsesRegionsAndMeta() {
        val idx = PrebuiltTiles.parseIndexFile(versionIndex("aaaa"))
        assertEquals(3, idx.cells.size)
        assertEquals(listOf("rhode-island", "rhode-island.border"), idx.regions)
        assertTrue(PrebuiltTiles.parseIndexFile("""{"v":1,"cells":{}}""").regions.isEmpty())
        val meta = PrebuiltTiles.parseMeta(
            "{\n \"version\": \"v1\",\n \"fragments\": {\n  \"monaco\": {\n   \"region\": \"monaco\",\n" +
                "   \"phase\": \"interior\",\n   \"osm\": \"2026-10-05T20:21:02Z\",\n   \"cells\": 12\n  },\n" +
                "  \"x.border\": {\"region\": \"x\", \"osm\": null}\n }\n}",
        )
        assertEquals(mapOf("monaco" to java.time.Instant.parse("2026-10-05T20:21:02Z").toEpochMilli()), meta)
    }
}
