package com.raemond.opentrailpaper.data

import android.content.Context
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.setValue
import org.json.JSONArray
import org.json.JSONObject

/**
 * Which services each ride has been uploaded to from this phone, by ride file
 * name, so the Rides list can tag them (✓ STRAVA) and a ride page shows its
 * tile as done after it is reopened. One JSON object in its own preferences
 * file; [version] lets screens recompose when an upload lands. Mirrors
 * UploadLog in companion-ios/Sources/SyncAccounts.swift.
 */
object UploadLog {
    private const val FILE = "rideUploads"
    private const val KEY = "rideUploads"

    /** Bumped on every [record]; read it to recompose when an upload lands. */
    var version by mutableIntStateOf(0)
        private set

    private fun prefs(context: Context) =
        context.applicationContext.getSharedPreferences(FILE, Context.MODE_PRIVATE)

    private fun all(context: Context): JSONObject =
        runCatching { JSONObject(prefs(context).getString(KEY, null) ?: "{}") }.getOrDefault(JSONObject())

    fun providers(context: Context, name: String): List<SyncAccounts.Provider> {
        val list = all(context).optJSONArray(name) ?: return emptyList()
        return (0 until list.length()).mapNotNull { SyncAccounts.Provider.byId(list.optString(it)) }
    }

    fun record(context: Context, p: SyncAccounts.Provider, name: String) {
        val all = all(context)
        val list = all.optJSONArray(name) ?: JSONArray()
        if ((0 until list.length()).none { list.optString(it) == p.id }) list.put(p.id)
        all.put(name, list)
        prefs(context).edit().putString(KEY, all.toString()).apply()
        version++
    }

    /** The tag's word for a service: STRAVA, INTERVALS, RWGPS. */
    fun shortTitle(p: SyncAccounts.Provider): String = when (p) {
        SyncAccounts.Provider.STRAVA -> "Strava"
        SyncAccounts.Provider.INTERVALS -> "Intervals"
        SyncAccounts.Provider.RIDEWITHGPS -> "RWGPS"
    }
}
