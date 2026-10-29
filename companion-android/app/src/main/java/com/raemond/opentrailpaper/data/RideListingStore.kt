package com.raemond.opentrailpaper.data

import android.content.Context
import android.content.SharedPreferences
import com.raemond.opentrailpaper.ble.RideFile
import org.json.JSONArray
import org.json.JSONObject

/**
 * The device's ride list as last seen, saved per device (keyed like
 * [DeviceSettingsStore], by Bluetooth address) so the Rides tab can show it at
 * launch and while disconnected, before the device is asked again.
 */
object RideListingStore {
    private const val FILE = "rideListings"

    private var sp: SharedPreferences? = null

    fun init(context: Context) {
        sp = context.applicationContext.getSharedPreferences(FILE, Context.MODE_PRIVATE)
    }

    fun load(id: String): List<RideFile> {
        val raw = sp?.getString("device.$id", null) ?: return emptyList()
        return runCatching {
            val a = JSONArray(raw)
            List(a.length()) { i ->
                val o = a.getJSONObject(i)
                RideFile(o.getString("name"), o.getInt("size"))
            }
        }.getOrDefault(emptyList())
    }

    fun save(list: List<RideFile>, id: String) {
        val a = JSONArray()
        list.forEach { a.put(JSONObject().put("name", it.name).put("size", it.size)) }
        sp?.edit()?.putString("device.$id", a.toString())?.apply()
    }
}
