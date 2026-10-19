package com.raemond.opentrailpaper.data

import android.content.Context
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import com.raemond.opentrailpaper.ble.BleManager
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.io.File

/**
 * Starred Strava segments -> the device's SEGMENT page. One action, "Sync",
 * under the Strava row in Settings > Accounts: a Strava thing that only makes
 * sense with that account connected, done once in a while like a map download.
 *
 * The polylines are cached by segment id (a segment's line never changes), so
 * after the first sync only the starred list is fetched again — one or two
 * requests, well inside Strava's rate limits. Mirrors SegmentSync.swift.
 */
object SegmentSync {
    var busy by mutableStateOf(false); private set
    var status by mutableStateOf<String?>(null); private set
    var failed by mutableStateOf(false); private set
    var lastSyncMs by mutableStateOf(0L); private set

    private val cache = HashMap<Long, String>()
    private var loaded = false

    private fun cacheFile(ctx: Context) = File(ctx.filesDir, "segment-polylines.json")
    private fun prefs(ctx: Context) = ctx.getSharedPreferences("segments", Context.MODE_PRIVATE)

    fun load(ctx: Context) {
        if (loaded) return
        loaded = true
        lastSyncMs = prefs(ctx).getLong("lastSync", 0L)
        runCatching {
            val o = JSONObject(cacheFile(ctx).readText())
            for (k in o.keys()) k.toLongOrNull()?.let { cache[it] = o.getString(k) }
        }
    }

    private fun saveCache(ctx: Context) = runCatching {
        val o = JSONObject()
        for ((k, v) in cache) o.put(k.toString(), v)
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
                    val (poly, pr) = SyncAccounts.segmentDetail(s.id)
                    if (poly != null) cache[s.id] = poly
                    if (pr != null) prs[s.id] = pr
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
            if (!ble.uploadSegments(StravaSegments.encode(segs))) {
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
