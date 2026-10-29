package com.raemond.opentrailpaper.map

import com.raemond.opentrailpaper.BuildConfig

/** Where map data comes from: the configured tile CDN and the public Overpass
 *  mirrors, unless a debug build points them elsewhere (DebugEndpoints). */
object MapEndpoints {
    val tileBaseUrl: String get() = DebugEndpoints.tileBaseUrl ?: BuildConfig.TILE_BASE_URL

    /** [mirrors], or the debug build's one stand-in endpoint. */
    fun overpass(mirrors: List<String>): List<String> = DebugEndpoints.overpassUrl?.let { listOf(it) } ?: mirrors
}
