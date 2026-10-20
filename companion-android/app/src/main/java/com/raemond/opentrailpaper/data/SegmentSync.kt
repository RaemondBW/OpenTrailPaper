package com.raemond.opentrailpaper.data

import android.content.Context
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import com.raemond.opentrailpaper.ble.BleManager
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONObject
import java.io.File

/**
 * Starred Strava segments -> the device's SEGMENT page. One action, "Sync",
 * under the Strava row in Settings > Accounts: a Strava thing that only makes
 * sense with that account connected, done once in a while like a map download.
 *
 * Each segment's line and altitude come from its streams (one request), cut
 * to the device's 200 points and cached by segment id (a segment never
 * changes), so after the first sync only the starred list is fetched again —
 * one or two requests, well inside Strava's rate limits. A segment whose
 * streams have no line falls back to its detail's polyline, without a
 * profile. Mirrors SegmentSync.swift.
 */
object SegmentSync {
    var busy by mutableStateOf(false); private set
    var status by mutableStateOf<String?>(null); private set
    var failed by mutableStateOf(false); private set
    var lastSyncMs by mutableStateOf(0L); private set

    private val cache = HashMap<Long, SegmentGeometry>()
    private var loaded = false

    private fun cacheFile(ctx: Context) = File(ctx.filesDir, "segment-geometry.json")
    private fun prefs(ctx: Context) = ctx.getSharedPreferences("segments", Context.MODE_PRIVATE)

    fun load(ctx: Context) {
        if (loaded) return
        loaded = true
        lastSyncMs = prefs(ctx).getLong("lastSync", 0L)
        runCatching {
            val o = JSONObject(cacheFile(ctx).readText())
            for (k in o.keys()) {
                val id = k.toLongOrNull() ?: continue
                val g = o.getJSONObject(k)
                val lat = g.getJSONArray("lat")
                val lon = g.getJSONArray("lon")
                val alt = g.optJSONArray("alt")
                val pts = (0 until lat.length()).map { LatLon(lat.getDouble(it), lon.getDouble(it)) }
                cache[id] = SegmentGeometry(pts, alt?.let { a -> (0 until a.length()).map { a.getDouble(it) } })
            }
        }
    }

    private fun saveCache(ctx: Context) = runCatching {
        val o = JSONObject()
        for ((k, g) in cache) {
            o.put(
                k.toString(),
                JSONObject()
                    .put("lat", JSONArray(g.points.map { it.lat }))
                    .put("lon", JSONArray(g.points.map { it.lon }))
                    .apply { g.altitudes?.let { put("alt", JSONArray(it)) } },
            )
        }
        cacheFile(ctx).writeText(o.toString())
    }

    suspend fun sync(ctx: Context, ble: BleManager) {
        if (busy) return
        load(ctx)
        busy = true
        failed = false
        status = "Reading your starred segments…"
        try {
            val starred = SyncAccounts.starredSegments()
            val rides = starred.filter { it.isRide }
            val wanted = rides.take(StravaSegments.MAX_SEGMENTS)
            val missing = wanted.filter { cache[it.id] == null }
            val prs = HashMap<Long, Int>()
            var limited = false
            for ((i, s) in missing.withIndex()) {
                status = "Fetching segment ${i + 1} of ${missing.size}…"
                try {
                    val st = SyncAccounts.segmentStreams(s.id)
                    if (st != null) {
                        cache[s.id] = StravaSegments.simplify(st.first, st.second)
                    } else {
                        // No line in the streams: the detail's polyline, no profile.
                        val (poly, pr) = SyncAccounts.segmentDetail(s.id)
                        poly?.let(CloudRoutes::decodePolyline)?.takeIf { it.size >= 2 }?.let {
                            cache[s.id] = StravaSegments.simplify(it, null)
                        }
                        if (pr != null) prs[s.id] = pr
                    }
                } catch (e: SyncHttpException) {
                    when (e.code) {
                        404 -> continue          // gone from Strava: leave it out
                        429 -> { limited = true; break }   // send what there is
                        else -> throw e
                    }
                }
            }
            withContext(Dispatchers.IO) { saveCache(ctx) }
            val segs = StravaSegments.deviceSegments(wanted, cache, prs)
            if (ble.state != BleManager.ConnState.CONNECTED) {
                fail("Connect your OpenTrailPaper, then sync again.")
                return
            }
            status = "Sending ${segs.size} segment${if (segs.size == 1) "" else "s"} to the device…"
            // Elevation (v2) only to firmware that said it reads it.
            val version = ble.deviceSegmentVersion.coerceIn(1, StravaSegments.BLOB_VERSION)
            if (!ble.uploadSegments(StravaSegments.encode(segs, version))) {
                fail(
                    if (ble.segmentsSupported == false) "The device needs firmware v1.23 or newer for segments."
                    else "The device did not confirm the segments. Try again.",
                )
                return
            }
            lastSyncMs = System.currentTimeMillis()
            prefs(ctx).edit().putLong("lastSync", lastSyncMs).apply()
            val sb = StringBuilder("${segs.size} starred segment${if (segs.size == 1) "" else "s"} on the device.")
            if (rides.size > StravaSegments.MAX_SEGMENTS) {
                sb.append(" The first ${StravaSegments.MAX_SEGMENTS} of your ${rides.size} are kept.")
            }
            val other = starred.size - rides.size
            if (other > 0) sb.append(" $other non-ride segment${if (other == 1) " was" else "s were"} left out.")
            if (limited) sb.append(" Strava is limiting requests, so some are missing; sync again in 15 minutes.")
            if (version < 2) sb.append(" Update the device firmware to see their elevation profiles.")
            status = sb.toString()
        } catch (e: Exception) {
            fail(e.message ?: "Sync failed.")
        } finally {
            busy = false
        }
    }

    private fun fail(message: String) {
        failed = true
        status = message
    }
}
