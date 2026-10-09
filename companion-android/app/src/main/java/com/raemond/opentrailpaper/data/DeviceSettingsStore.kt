package com.raemond.opentrailpaper.data

import android.content.Context
import android.content.SharedPreferences
import org.json.JSONObject

/**
 * Per-device settings cache on disk (see DeviceSettingsSync.kt), keyed by the
 * device's Bluetooth address. "Current device" is a single key: the last
 * device that connected or — with a fixed pairing — the paired one
 * ([setCurrent]). Edits made before any device was known live under
 * [DeviceSettingsCache.UNPAIRED] and move to the first device.
 */
object DeviceSettingsStore {
    private const val FILE = "deviceSettings"
    private const val KEY_CURRENT = "current"

    private lateinit var sp: SharedPreferences

    fun init(context: Context) {
        sp = context.applicationContext.getSharedPreferences(FILE, Context.MODE_PRIVATE)
    }

    val currentDevice: String
        get() = sp.getString(KEY_CURRENT, null) ?: DeviceSettingsCache.UNPAIRED

    fun load(id: String): DeviceSettingsCache =
        sp.getString("device.$id", null)?.let { runCatching { decode(JSONObject(it)) }.getOrNull() }
            ?: DeviceSettingsCache()

    fun save(c: DeviceSettingsCache, id: String) {
        sp.edit().putString("device.$id", encode(c).toString()).apply()
    }

    /** Make [id] the current device and return its cache. */
    fun setCurrent(id: String): DeviceSettingsCache {
        val previous = currentDevice
        sp.edit().putString(KEY_CURRENT, id).apply()
        val target = load(id)
        if (previous != DeviceSettingsCache.UNPAIRED || id == DeviceSettingsCache.UNPAIRED) return target
        val adopted = DeviceSettingsCache.adopt(previous, id, target, load(DeviceSettingsCache.UNPAIRED))
        if (adopted != target) save(adopted, id)
        sp.edit().remove("device.${DeviceSettingsCache.UNPAIRED}").apply()
        return adopted
    }

    private fun encode(c: DeviceSettingsCache) = JSONObject().apply {
        c.device?.let { d ->
            put("device", JSONObject().apply {
                put("length", d.length)
                put("values", JSONObject(d.values as Map<*, *>))
            })
        }
        put("pending", JSONObject().apply {
            c.pending.forEach { (k, e) ->
                put(k, JSONObject().put("value", e.value).put("at", e.editedAt))
            }
        })
        c.dashText?.let { put("dashText", it) }
        c.pendingDash?.let { p ->
            put("pendingDash", JSONObject().apply {
                put("text", p.text)
                p.baseText?.let { put("base", it) }
                put("at", p.editedAt)
            })
        }
    }

    private fun decode(o: JSONObject): DeviceSettingsCache {
        val device = o.optJSONObject("device")?.let { d ->
            val vals = d.getJSONObject("values")
            DeviceSettingsValues(vals.keys().asSequence().associateWith { vals.getInt(it) },
                d.getInt("length"))
        }
        val pending = o.optJSONObject("pending")?.let { p ->
            p.keys().asSequence().associateWith {
                val e = p.getJSONObject(it)
                PendingEdit(e.getInt("value"), e.getLong("at"))
            }
        } ?: emptyMap()
        val pd = o.optJSONObject("pendingDash")?.let {
            PendingDash(it.getString("text"),
                if (it.has("base")) it.getString("base") else null, it.getLong("at"))
        }
        return DeviceSettingsCache(device, pending,
            if (o.has("dashText")) o.getString("dashText") else null, pd)
    }
}
