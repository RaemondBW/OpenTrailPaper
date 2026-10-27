package com.raemond.opentrailpaper.map

import android.content.Context

/** Release builds always use the configured CDN and the public Overpass
 *  mirrors; the override is debug-only (src/debug). */
object DebugEndpoints {
    val tileBaseUrl: String? get() = null
    val overpassUrl: String? get() = null

    @Suppress("UNUSED_PARAMETER")
    fun init(context: Context) = Unit
}
