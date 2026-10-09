package com.raemond.opentrailpaper.data

import android.content.Context
import android.content.SharedPreferences

/// The handful of things that outlive a launch: the units preference (shared
/// with the device), whether the tutorial has been seen, and the last-known list
/// of tiles the head unit holds.
///
/// The tile list is cached because the Settings and Maps screens both want to
/// say what the device is carrying while disconnected. Without it they would
/// claim the device holds nothing, which is worse than slightly stale.
object Prefs {
    private const val FILE = "opentrailpaper"
    private const val KEY_MILES = "useMiles"
    private const val KEY_ONBOARDED = "didOnboard"
    private const val KEY_DEVICE_TILES = "deviceTileIds"
    private const val KEY_ASKED = "askedPermissions"
    private const val KEY_FLAGGED_TILES = "flaggedTileIds"
    private const val KEY_POI_SENT = "poiSentAt"
    private const val KEY_TILE_SENT = "tileSentAt"

    private lateinit var sp: SharedPreferences

    fun init(context: Context) {
        sp = context.applicationContext.getSharedPreferences(FILE, Context.MODE_PRIVATE)
    }

    var useMiles: Boolean
        get() = sp.getBoolean(KEY_MILES, false)
        set(v) = sp.edit().putBoolean(KEY_MILES, v).apply()

    var onboarded: Boolean
        get() = sp.getBoolean(KEY_ONBOARDED, false)
        set(v) = sp.edit().putBoolean(KEY_ONBOARDED, v).apply()

    var deviceTileIds: Set<String>
        get() = sp.getStringSet(KEY_DEVICE_TILES, emptySet()) ?: emptySet()
        set(v) = sp.edit().putStringSet(KEY_DEVICE_TILES, v).apply()

    /** Map tiles this phone sent WITH the bike-route way flags (see BleManager.tileIsCurrent). */
    var flaggedTileIds: Set<String>
        get() = sp.getStringSet(KEY_FLAGGED_TILES, emptySet()) ?: emptySet()
        set(v) = sp.edit().putStringSet(KEY_FLAGGED_TILES, v).apply()

    /** When this phone last sent each map tile (epoch ms), as "id=ms" entries. */
    var tileSentAt: Map<String, Long>
        get() = stamps(KEY_TILE_SENT)
        set(v) = sp.edit().putStringSet(KEY_TILE_SENT, v.map { "${it.key}=${it.value}" }.toSet()).apply()

    private fun stamps(key: String): Map<String, Long> =
        (sp.getStringSet(key, emptySet()) ?: emptySet()).mapNotNull {
            val i = it.indexOf('=')
            val ms = if (i > 0) it.substring(i + 1).toLongOrNull() else null
            if (ms == null) null else it.substring(0, i) to ms
        }.toMap()

    /** When this phone last sent each tile's .poi (epoch ms), as "id=ms" entries. */
    var poiSentAt: Map<String, Long>
        get() = (sp.getStringSet(KEY_POI_SENT, emptySet()) ?: emptySet()).mapNotNull {
            val i = it.indexOf('=')
            if (i <= 0) null else it.substring(0, i) to (it.substring(i + 1).toLongOrNull() ?: return@mapNotNull null)
        }.toMap()
        set(v) = sp.edit().putStringSet(KEY_POI_SENT, v.map { "${it.key}=${it.value}" }.toSet()).apply()

    /**
     * Permissions we have already put a system dialog in front of.
     *
     * Android has no equivalent of iOS's `notDetermined` vs `denied`: a refused
     * permission and one never asked for look identical to
     * `checkSelfPermission`. Pairing this with
     * `shouldShowRequestPermissionRationale` is the only way to tell them apart —
     * and the app has to, because one calls for a prompt and the other for a trip
     * to system Settings.
     */
    var askedPermissions: Set<String>
        get() = sp.getStringSet(KEY_ASKED, emptySet()) ?: emptySet()
        set(v) = sp.edit().putStringSet(KEY_ASKED, v).apply()

    fun noteAsked(permissions: Collection<String>) {
        askedPermissions = askedPermissions + permissions
    }

    /** Lets a composable re-read [useMiles] whenever anything writes it. */
    fun registerListener(l: SharedPreferences.OnSharedPreferenceChangeListener) =
        sp.registerOnSharedPreferenceChangeListener(l)

    fun unregisterListener(l: SharedPreferences.OnSharedPreferenceChangeListener) =
        sp.unregisterOnSharedPreferenceChangeListener(l)
}
