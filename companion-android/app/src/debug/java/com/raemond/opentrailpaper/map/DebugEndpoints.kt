package com.raemond.opentrailpaper.map

import android.content.Context
import android.util.Log
import java.io.File

/**
 * Debug builds: where map downloads go instead of the public tile CDN and
 * Overpass, when files/otp-endpoints exists (sim/ANDROID.md) — the stand-in
 * CDN the sim tests serve a fixture from (sim/cdn/tile_cdn.py):
 *
 *     adb shell run-as com.raemond.opentrailpaper sh -c \
 *         'printf "tiles=http://10.0.2.2:7923/v1/\noverpass=http://10.0.2.2:7923/api/interpreter\n" > files/otp-endpoints'
 *
 * and ride uploads to stand-ins for Strava, Intervals.icu and the sync
 * service (`strava=`, `intervals=`, `sync=` lines; UploadSimTest).
 *
 * Read once at start ([init]); delete the file to go back. Release builds have
 * a stub with no override (src/release), like DebugRadio.
 */
object DebugEndpoints {
    /** The tile CDN's base URL (`…/v1/`), or null for BuildConfig.TILE_BASE_URL. */
    @Volatile var tileBaseUrl: String? = null; private set
    /** The one Overpass endpoint to use instead of the public mirrors, or null. */
    @Volatile var overpassUrl: String? = null; private set

    /**
     * Ride uploads (SyncAccounts) to stand-ins for the services: Strava's and
     * Intervals.icu's API origins (`http://host:port`, no path) and the
     * sync-auth service's base URL, or null for the real ones. For tests with
     * fake tokens only.
     */
    @Volatile var stravaOrigin: String? = null; private set
    @Volatile var intervalsOrigin: String? = null; private set
    @Volatile var syncServiceUrl: String? = null; private set

    fun init(context: Context) {
        val f = File(context.filesDir, "otp-endpoints")
        tileBaseUrl = null
        overpassUrl = null
        stravaOrigin = null
        intervalsOrigin = null
        syncServiceUrl = null
        if (!f.exists()) return
        for (line in f.readLines()) {
            val k = line.substringBefore('=', "").trim()
            val v = line.substringAfter('=', "").trim()
            when (k) {
                "tiles" -> tileBaseUrl = v
                "overpass" -> overpassUrl = v.takeIf { it.isNotEmpty() }
                "strava" -> stravaOrigin = v.takeIf { it.isNotEmpty() }?.trimEnd('/')
                "intervals" -> intervalsOrigin = v.takeIf { it.isNotEmpty() }?.trimEnd('/')
                "sync" -> syncServiceUrl = v.takeIf { it.isNotEmpty() }?.trimEnd('/')
            }
        }
        Log.w(
            "DebugEndpoints",
            "map downloads use tiles=$tileBaseUrl overpass=$overpassUrl; uploads strava=$stravaOrigin " +
                "intervals=$intervalsOrigin sync=$syncServiceUrl",
        )
    }
}
