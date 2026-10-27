package com.raemond.opentrailpaper.sim

import com.google.gson.JsonArray
import com.google.gson.JsonObject
import com.google.gson.JsonParser
import com.raemond.opentrailpaper.map.MapTile
import java.io.BufferedReader
import java.io.File
import java.io.InputStreamReader
import java.net.HttpURLConnection
import java.net.URL
import java.util.concurrent.TimeUnit

/**
 * The stand-in tile CDN (sim/cdn/tile_cdn.py) as a child process, serving a
 * fixture directory (default sim/fixtures/appmaps), and its /_ctl/ test
 * controls: the request log, new versions of a file, faults.
 */
class TileCdn private constructor(val fixture: File, private val proc: Process, val port: Int) : AutoCloseable {

    /** One fixture cell: its id, bbox and its CDN index entry. */
    class Cell(val tile: MapTile, val ebmSize: Int, val ebmHash: String, val poiSize: Int, val poiHash: String)

    companion object {
        /** The repository root (has sim/cdn/tile_cdn.py) above the test's working directory. */
        val root: File? by lazy {
            generateSequence(File(System.getProperty("user.dir")).absoluteFile) { it.parentFile }
                .firstOrNull { File(it, "sim/cdn/tile_cdn.py").isFile }
        }

        fun start(fixture: File = File(root ?: error("sim/cdn not found"), "sim/fixtures/appmaps")): TileCdn {
            val script = File(root, "sim/cdn/tile_cdn.py")
            val p = ProcessBuilder("python3", script.path, "--dir", fixture.path, "--port", "0")
                .redirectErrorStream(true).start()
            Runtime.getRuntime().addShutdownHook(Thread { p.destroyForcibly() })
            val out = BufferedReader(InputStreamReader(p.inputStream))
            val first = out.readLine() ?: error("tile_cdn.py exited: ${p.waitFor()}")
            val port = Regex("listening on [0-9.]+:(\\d+)").find(first)?.groupValues?.get(1)?.toInt()
                ?: error("tile_cdn.py: $first")
            Thread { runCatching { while (out.readLine() != null) Unit } }.apply { isDaemon = true }.start()
            return TileCdn(fixture, p, port)
        }
    }

    val baseUrl: String get() = "http://127.0.0.1:$port/v1/"
    val overpassUrl: String get() = "http://127.0.0.1:$port/api/interpreter"

    /** area.json's cells, with the fixture's index entries. */
    val cells: List<Cell> by lazy {
        val area = JsonParser.parseString(File(fixture, "area.json").readText()).asJsonObject
        area.getAsJsonArray("cells").map { it.asJsonObject }.map { c ->
            val id = c.get("id").asString
            val e = index(id)
            Cell(
                MapTile(id, 0L, c.get("s").asDouble, c.get("w").asDouble, c.get("n").asDouble, c.get("e").asDouble),
                e[0].asInt, e[1].asString, e[2].asInt, e[3].asString,
            )
        }
    }

    val center: Pair<Double, Double> by lazy {
        val c = JsonParser.parseString(File(fixture, "area.json").readText()).asJsonObject.getAsJsonArray("center")
        c[0].asDouble to c[1].asDouble
    }

    /** The index entry the CDN serves now ([ebmSize, ebmHash, poiSize, poiHash]). */
    fun index(id: String): JsonArray {
        val idx = JsonParser.parseString(String(get("v1/${id.take(6)}/index.json"))).asJsonObject
        return idx.getAsJsonObject("cells").getAsJsonArray(id)
    }

    /** The bytes the CDN serves now for a fixture path ("v1/<g>/<id>.ebm"),
     *  read past the request log. */
    fun get(path: String): ByteArray {
        val (code, body) = request("GET", "_ctl/file?path=/$path")
        check(code == 200) { "tile_cdn: no $path" }
        return body
    }

    /** Every request so far: method, path (no query), status, fault. */
    fun log(): List<JsonObject> =
        JsonParser.parseString(String(request("GET", "_ctl/log").second)).asJsonArray.map { it.asJsonObject }

    fun paths(log: List<JsonObject> = log()): List<String> = log.map { it.get("path").asString }

    fun reset() { ctl("_ctl/reset") }

    /** Publishes a new version of `<id>.<ext>`; returns its new size and hash. */
    fun bump(id: String, ext: String = "ebm"): JsonObject =
        JsonParser.parseString(String(ctl("_ctl/bump?id=$id&ext=$ext"))).asJsonObject

    /** Misbehave on paths matching [match]: "truncate", "status:<code>" or "close". */
    fun fault(match: String, kind: String) { ctl("_ctl/fault?match=$match&kind=$kind") }

    private fun ctl(path: String): ByteArray {
        val (code, body) = request("POST", path)
        check(code == 200) { "tile_cdn $path: $code ${String(body)}" }
        return body
    }

    private fun request(method: String, path: String): Pair<Int, ByteArray> {
        val c = URL("http://127.0.0.1:$port/$path").openConnection() as HttpURLConnection
        c.requestMethod = method
        c.connectTimeout = 5_000
        c.readTimeout = 5_000
        return try {
            val code = c.responseCode
            code to ((if (code < 400) c.inputStream else c.errorStream)?.use { it.readBytes() } ?: ByteArray(0))
        } finally {
            c.disconnect()
        }
    }

    override fun close() {
        proc.destroy()
        if (!proc.waitFor(3, TimeUnit.SECONDS)) proc.destroyForcibly()
    }
}
