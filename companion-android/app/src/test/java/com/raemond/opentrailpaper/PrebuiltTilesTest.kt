package com.raemond.opentrailpaper

import com.raemond.opentrailpaper.map.PrebuiltTiles
import kotlinx.coroutines.runBlocking
import org.junit.After
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
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
}
