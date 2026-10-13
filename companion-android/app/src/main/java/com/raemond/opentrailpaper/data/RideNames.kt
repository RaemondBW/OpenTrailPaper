package com.raemond.opentrailpaper.data

import android.content.Context
import android.content.SharedPreferences
import androidx.compose.runtime.mutableStateMapOf

/**
 * Names the rider gave rides, keyed by the ride's file name on the device
 * (which is also its cached file's name). Set when a ride is renamed before an
 * upload, so the next upload (to another account) starts from the same name
 * and the ride list and ride page show it. Kept on this phone only; the device
 * and its files are untouched. Mirrors RideNames in the iOS app.
 */
object RideNames {
    private const val FILE = "opentrailpaper.ridenames"

    /** Comfortably inside what Strava, Intervals.icu and RideWithGPS accept. */
    const val MAX_LENGTH = 100

    private var sp: SharedPreferences? = null

    /** Compose-observable copy of the store: file name -> the rider's name. */
    val names = mutableStateMapOf<String, String>()

    fun init(context: Context) {
        val prefs = context.applicationContext.getSharedPreferences(FILE, Context.MODE_PRIVATE)
        sp = prefs
        for ((k, v) in prefs.all) if (v is String) names[k] = v
    }

    /** The rider's name for this ride, or [default] when it has none. */
    fun name(file: String, default: String): String = names[file] ?: default

    /**
     * Saves what the rider typed and returns the name to upload under. Blank
     * (or the default itself) means the default, and forgets any earlier rename.
     */
    fun rename(file: String, input: String, default: String): String {
        val name = cleaned(input) ?: default
        if (name == default) forget(file) else {
            names[file] = name
            sp?.edit()?.putString(file, name)?.apply()
        }
        return name
    }

    fun forget(file: String) {
        names.remove(file)
        sp?.edit()?.remove(file)?.apply()
    }

    /** One line, trimmed, at most [MAX_LENGTH] characters; null when nothing is left. */
    fun cleaned(input: String): String? {
        val line = input.split(Regex("\\s+")).filter { it.isNotEmpty() }.joinToString(" ")
        if (line.isEmpty()) return null
        return line.take(MAX_LENGTH).trimEnd()
    }
}
