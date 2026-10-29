package com.raemond.opentrailpaper.map

import android.content.Context

/** Release builds always use the configured CDN, the public Overpass mirrors
 *  and the real upload services; the override is debug-only (src/debug). */
object DebugEndpoints {
    val tileBaseUrl: String? get() = null
    val overpassUrl: String? get() = null
    val stravaOrigin: String? get() = null
    val intervalsOrigin: String? get() = null
    val syncServiceUrl: String? get() = null

    @Suppress("UNUSED_PARAMETER")
    fun init(context: Context) = Unit
}
