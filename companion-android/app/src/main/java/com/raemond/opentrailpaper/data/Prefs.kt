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
    private const val KEY_PAIRED_ADDR = "pairedDeviceAddress"
    private const val KEY_PAIRED_NAME = "pairedDeviceName"

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

    /**
     * Fixed pairing: the MAC of the OpenTrailPaper this phone owns (null until
     * one has admitted us). The device's address is fixed, so this stays valid
     * for as long as the bond does; the app only ever connects to it.
     */
    var pairedDeviceAddress: String?
        get() = sp.getString(KEY_PAIRED_ADDR, null)
        set(v) = sp.edit().putString(KEY_PAIRED_ADDR, v).apply()

    var pairedDeviceName: String?
        get() = sp.getString(KEY_PAIRED_NAME, null)
        set(v) = sp.edit().putString(KEY_PAIRED_NAME, v).apply()

    var deviceTileIds: Set<String>
        get() = sp.getStringSet(KEY_DEVICE_TILES, emptySet()) ?: emptySet()
        set(v) = sp.edit().putStringSet(KEY_DEVICE_TILES, v).apply()

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
