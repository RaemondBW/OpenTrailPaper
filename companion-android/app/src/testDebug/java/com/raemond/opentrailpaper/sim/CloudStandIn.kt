package com.raemond.opentrailpaper.sim

import java.io.BufferedInputStream
import java.io.ByteArrayOutputStream
import java.io.InputStream
import java.net.InetAddress
import java.net.ServerSocket
import java.net.Socket
import java.net.URLDecoder
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.Executors

/**
 * Stand-ins for the services a ride is uploaded to — Strava's upload API,
 * Intervals.icu's activities API and the sync-auth service (token refresh,
 * RideWithGPS trips) — on one local port, so the app's real upload code
 * (SyncAccounts, behind the debug-only DebugEndpoints override) runs end to
 * end without the internet or a real account. Answers are shaped like the
 * services' documented ones; every request is recorded for the test to read.
 */
class CloudStandIn : AutoCloseable {

    /** One request as received: method, path, query, headers (lower-case keys), body. */
    data class Request(
        val method: String,
        val path: String,
        val query: Map<String, String>,
        val headers: Map<String, String>,
        val body: ByteArray,
    ) {
        val bearer: String? get() = headers["authorization"]?.removePrefix("Bearer ")

        /** A multipart/form-data body's parts: name -> bytes. */
        val parts: Map<String, ByteArray> by lazy { multipart(headers["content-type"] ?: "", body) }

        fun field(name: String): String? = parts[name]?.toString(Charsets.UTF_8)
    }

    /** A canned answer: status and JSON body. */
    data class Answer(val status: Int, val json: String)

    // A small HTTP/1.1 server of its own: the JDK's HttpServer isn't on an
    // Android unit test's compile classpath. One request per connection.
    private val server = ServerSocket(0, 50, InetAddress.getByName("127.0.0.1"))
    private val pool = Executors.newCachedThreadPool { r -> Thread(r, "cloud-standin").apply { isDaemon = true } }

    /** Every request so far, in order. */
    val requests = CopyOnWriteArrayList<Request>()

    val origin: String get() = "http://127.0.0.1:${server.localPort}"

    /** What a debug build's files/otp-endpoints says to send uploads here. */
    val endpointsFile: String get() = "strava=$origin\nintervals=$origin\nsync=$origin/sync\n"

    /** Answers by "METHOD /path" (exact), tried before the defaults; each a queue (the last one repeats). */
    private val scripted = HashMap<String, ArrayDeque<Answer>>()

    init {
        Thread({
            while (!server.isClosed) {
                val c = runCatching { server.accept() }.getOrNull() ?: break
                pool.execute { runCatching { serve(c) }; runCatching { c.close() } }
            }
        }, "cloud-standin-accept").apply { isDaemon = true }.start()
    }

    /** The next [method] [path] requests get [answers] in turn (the last one repeats). */
    fun script(method: String, path: String, vararg answers: Answer) {
        synchronized(scripted) { scripted["$method $path"] = ArrayDeque(answers.toList()) }
    }

    fun requests(method: String, path: String): List<Request> = requests.filter { it.method == method && it.path == path }

    private fun serve(c: Socket) {
        val input = BufferedInputStream(c.getInputStream())
        val requestLine = readLine(input) ?: return
        val (method, target) = requestLine.split(' ').let { it[0] to it[1] }
        val headers = LinkedHashMap<String, String>()
        while (true) {
            val l = readLine(input) ?: return
            if (l.isEmpty()) break
            headers[l.substringBefore(':').trim().lowercase()] = l.substringAfter(':').trim()
        }
        val body = when {
            headers["transfer-encoding"]?.contains("chunked", ignoreCase = true) == true -> {
                val out = ByteArrayOutputStream()
                while (true) {
                    val n = (readLine(input) ?: "0").substringBefore(';').trim().toInt(16)
                    if (n == 0) { readLine(input); break }
                    out.write(readN(input, n))
                    readLine(input)
                }
                out.toByteArray()
            }
            headers["content-length"] != null -> readN(input, headers["content-length"]!!.toInt())
            else -> ByteArray(0)
        }
        val path = target.substringBefore('?')
        val query = target.substringAfter('?', "").split('&').filter { it.isNotEmpty() }.associate {
            URLDecoder.decode(it.substringBefore('='), "UTF-8") to URLDecoder.decode(it.substringAfter('=', ""), "UTF-8")
        }
        val req = Request(method, path, query, headers, body)
        requests += req
        val a = synchronized(scripted) {
            scripted["$method $path"]?.let { q -> if (q.size > 1) q.removeFirst() else q.first() }
        } ?: default(req)
        val out = a.json.toByteArray()
        val reason = if (a.status in 200..299) "OK" else "Error"
        c.getOutputStream().apply {
            write(("HTTP/1.1 ${a.status} $reason\r\nContent-Type: application/json\r\n" +
                "Content-Length: ${out.size}\r\nConnection: close\r\n\r\n").toByteArray())
            write(out)
            flush()
        }
    }

    private fun readLine(i: InputStream): String? {
        val b = ByteArrayOutputStream()
        while (true) {
            val ch = i.read()
            if (ch < 0) return if (b.size() == 0) null else b.toString("UTF-8")
            if (ch == '\n'.code) return b.toString("UTF-8").trimEnd('\r')
            b.write(ch)
        }
    }

    private fun readN(i: InputStream, n: Int): ByteArray {
        val out = ByteArray(n)
        var got = 0
        while (got < n) {
            val r = i.read(out, got, n - got)
            if (r < 0) break
            got += r
        }
        return out
    }

    /** The services' usual answers. */
    private fun default(r: Request): Answer = when {
        // Strava: POST /api/v3/uploads answers an Upload (nullable fields
        // present, as Strava sends them); polling it gives the activity.
        r.method == "POST" && r.path == "/api/v3/uploads" -> Answer(
            201,
            """{"id":8150,"id_str":"8150","external_id":"ride.fit",""" +
                """"error":null,"status":"Your activity is still being processed.","activity_id":null}""",
        )
        r.method == "GET" && r.path.startsWith("/api/v3/uploads/") -> Answer(
            200,
            """{"id":8150,"id_str":"8150","external_id":"ride.fit","error":null,""" +
                """"status":"Your activity is ready.","activity_id":4242}""",
        )
        // Intervals.icu: the created activity (or the duplicate) in activities[].
        r.method == "POST" && r.path == "/api/v1/athlete/0/activities" ->
            Answer(201, """{"icu_athlete_id":"i1","id":"i777","activities":[{"icu_athlete_id":"i1","id":"i777"}]}""")
        // The sync service: RideWithGPS trips go through it; refreshes too.
        r.method == "POST" && r.path == "/sync/v1/rwgps/trips" -> Answer(200, """{"trip":{"id":9090,"name":"x"}}""")
        r.method == "POST" && r.path.startsWith("/sync/v1/auth/") && r.path.endsWith("/refresh") ->
            Answer(200, """{"access_token":"test-refreshed-token-not-real","refresh_token":"test-rotated-refresh-not-real","expires_at":${System.currentTimeMillis() / 1000 + 21_600}}""")
        else -> Answer(404, """{"message":"Record Not Found","errors":[]}""")
    }

    override fun close() {
        runCatching { server.close() }
        pool.shutdownNow()
    }

    companion object {
        /** Splits a multipart/form-data body on its boundary. */
        fun multipart(contentType: String, body: ByteArray): Map<String, ByteArray> {
            val boundary = contentType.substringAfter("boundary=", "").trim().trim('"')
            if (boundary.isEmpty()) return emptyMap()
            val delim = "--$boundary".toByteArray()
            val out = LinkedHashMap<String, ByteArray>()
            var i = indexOf(body, delim, 0)
            while (i >= 0) {
                val start = i + delim.size
                if (start + 2 <= body.size && body[start] == '-'.code.toByte() && body[start + 1] == '-'.code.toByte()) break
                val headEnd = indexOf(body, "\r\n\r\n".toByteArray(), start)
                if (headEnd < 0) break
                val head = String(body, start, headEnd - start, Charsets.UTF_8)
                val next = indexOf(body, delim, headEnd + 4)
                if (next < 0) break
                val name = Regex("name=\"([^\"]*)\"").find(head)?.groupValues?.get(1) ?: ""
                // The part's data ends with the CRLF before the next delimiter.
                out[name] = body.copyOfRange(headEnd + 4, next - 2)
                i = next
            }
            return out
        }

        private fun indexOf(h: ByteArray, n: ByteArray, from: Int): Int {
            outer@ for (i in from..h.size - n.size) {
                for (j in n.indices) if (h[i + j] != n[j]) continue@outer
                return i
            }
            return -1
        }
    }
}
