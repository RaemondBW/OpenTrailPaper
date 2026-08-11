package com.raemond.opentrailpaper.data

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.io.ByteArrayOutputStream
import java.net.HttpURLConnection
import java.net.URL

/**
 * Fetches firmware from GitHub Releases at runtime, instead of baking a copy
 * into the app.
 *
 * WHY NOT A BUNDLED BINARY. The scheme this replaced on iOS shipped a
 * firmware.bin plus a version string that had to be kept in step with
 * src/config.h by hand. Three things had to move together for every firmware
 * change, and when they didn't the failure was invisible: the version string
 * still matched, the bytes didn't, and `updateAvailable` compares strings — so
 * the app would happily "update" a device to a *different* binary wearing the
 * same version. Now there is one source of truth (the release), the app carries
 * no firmware, and shipping firmware no longer means shipping an app build.
 */
object FirmwareRelease {

    data class Release(
        val tag: String,          // "v1.05"
        val assetUrl: String,     // firmware.bin download
        val size: Int,
        val notes: String,
    )

    var latest by mutableStateOf<Release?>(null); private set
    var checking by mutableStateOf(false); private set
    var error by mutableStateOf<String?>(null); private set

    /** Download progress while fetching the binary, null when not downloading. */
    var downloadProgress by mutableStateOf<Double?>(null); private set

    private const val REPO = "RaemondBW/OpenTrailPaper"
    private var cached: Pair<String, ByteArray>? = null

    /**
     * Ask GitHub what the newest release is. Cheap (one JSON request) and safe
     * to call on every appearance of the firmware screen.
     */
    suspend fun check() {
        if (checking) return
        checking = true
        error = null
        try {
            val result = withContext(Dispatchers.IO) {
                val conn = (URL("https://api.github.com/repos/$REPO/releases/latest")
                    .openConnection() as HttpURLConnection).apply {
                    setRequestProperty("Accept", "application/vnd.github+json")
                    setRequestProperty("User-Agent", "OpenTrailPaper-Android")
                    connectTimeout = 15_000
                    readTimeout = 15_000
                }
                try {
                    val code = conn.responseCode
                    if (code != 200) return@withContext Result.failure(HttpError(code))
                    Result.success(conn.inputStream.readBytes().toString(Charsets.UTF_8))
                } finally {
                    conn.disconnect()
                }
            }

            result.onFailure { e ->
                // 403 here is nearly always the unauthenticated rate limit
                // (60/hour/IP), which is worth naming rather than reporting as a
                // generic failure.
                val code = (e as? HttpError)?.code ?: 0
                error = if (code == 403) {
                    "GitHub rate limit — try again shortly"
                } else {
                    "Couldn't reach GitHub ($code)"
                }
            }.onSuccess { body ->
                val obj = JSONObject(body)
                val tag = obj.optString("tag_name")
                val assets = obj.optJSONArray("assets")
                var found: Release? = null
                if (tag.isNotEmpty() && assets != null) {
                    for (i in 0 until assets.length()) {
                        val a = assets.getJSONObject(i)
                        if (a.optString("name") != "firmware.bin") continue
                        val url = a.optString("browser_download_url")
                        if (url.isEmpty()) continue
                        found = Release(tag, url, a.optInt("size", 0), obj.optString("body"))
                        break
                    }
                }
                if (found == null) error = "That release has no firmware.bin" else latest = found
            }
        } catch (_: Exception) {
            error = "Couldn't reach GitHub"
        } finally {
            checking = false
        }
    }

    /**
     * The firmware image for [release], downloading it if it isn't already in
     * hand. Kept in memory only: it is ~1.8 MB, wanted once per update, and
     * caching it on disk would just be another copy to go stale.
     */
    suspend fun image(release: Release): ByteArray {
        cached?.let { if (it.first == release.tag) return it.second }
        downloadProgress = 0.0
        try {
            val data = withContext(Dispatchers.IO) {
                val conn = (URL(release.assetUrl).openConnection() as HttpURLConnection).apply {
                    setRequestProperty("User-Agent", "OpenTrailPaper-Android")
                    instanceFollowRedirects = true
                    connectTimeout = 20_000
                    readTimeout = 60_000
                }
                try {
                    if (conn.responseCode != 200) throw HttpError(conn.responseCode)
                    val expected = if (release.size > 0) release.size else conn.contentLength
                    val out = ByteArrayOutputStream(maxOf(expected, 1 shl 16))
                    val buf = ByteArray(16 * 1024)
                    conn.inputStream.use { input ->
                        while (true) {
                            val n = input.read(buf)
                            if (n <= 0) break
                            out.write(buf, 0, n)
                            if (expected > 0) {
                                val fraction = out.size().toDouble() / expected
                                withContext(Dispatchers.Main) { downloadProgress = fraction }
                            }
                        }
                    }
                    out.toByteArray()
                } finally {
                    conn.disconnect()
                }
            }
            // A truncated download would be flashed to the device and brick the
            // slot, so check the length the API told us to expect.
            if (release.size > 0 && data.size != release.size) {
                throw IllegalStateException("firmware.bin is ${data.size} B, expected ${release.size}")
            }
            cached = release.tag to data
            return data
        } finally {
            downloadProgress = null
        }
    }

    private class HttpError(val code: Int) : Exception("HTTP $code")
}
