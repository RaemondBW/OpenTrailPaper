package com.raemond.opentrailpaper.data

// Offline device settings: the cache, the rider's offline edits, and the merge
// that runs when the head unit comes back. Twin of the iOS app's
// DeviceSettingsSync.swift — same rules, same test cases
// (DeviceSettingsSyncTest here, tools/offline_settings_test for iOS).
//
// Pure Kotlin (no android.*), so the JVM unit tests exercise it directly.
//
// The rules:
//  * The cache holds the device's LAST REPORTED values, per device — written
//    only from what the device said (read, notify, or an acknowledged write),
//    never from app defaults.
//  * An app-side edit is a PENDING edit (value + when). The screen shows
//    pending ?: cached ?: a display default.
//  * On reconnect the device's values are read first, then merged: fields the
//    rider edited in the app take the app's value; every other field takes the
//    device's (backlight via the side button, units from the device menu...).
//  * Both sides changed one field: the device keeps no per-field change time
//    or counter, so "most recent" can't be decided — the app's edit wins, and
//    the merge reports that it replaced a device-side change.
//  * The payload is all-fields, so the push is built from the MERGED values,
//    and only as long as the device itself reported (older firmware takes
//    fewer bytes). Pending edits for fields the device lacks are dropped.
//  * Pending flags clear only on an acknowledged write, and only for fields
//    still holding the pushed value.

/** One field of the settings characteristic (src/ble_server.cpp SettingsCb).
 *  Layout, little-endian: int16 ftpW, int16 tzMin, u8 useMiles, u8 backlight,
 *  u8 clock24h, u8 usbDrive. */
enum class SettingField(val key: String, val minLength: Int, val label: String) {
    FTP("ftp", 4, "FTP"),
    TZ("tz", 4, "Timezone"),
    USE_MILES("useMiles", 6, "Units"),
    BACKLIGHT("backlight", 6, "Backlight"),
    CLOCK_24H("clock24h", 7, "Clock"),
    USB_DRIVE("usbDrive", 8, "USB drive"),
    ;

    companion object {
        const val MAX_LENGTH = 8
        fun of(key: String): SettingField? = entries.firstOrNull { it.key == key }
    }
}

/** The settings the device reported, and how many bytes it reported them in. */
data class DeviceSettingsValues(
    val values: Map<String, Int> = emptyMap(),
    /** Payload length the device sent: 4 (FTP + tz only), 6, 7 or 8. */
    val length: Int = 0,
) {
    operator fun get(f: SettingField): Int? = values[f.key]

    fun with(f: SettingField, v: Int) = copy(values = values + (f.key to v))

    fun supports(f: SettingField) = length >= f.minLength

    /** The write payload, exactly [length] bytes. null if a field inside that
     *  length is unknown — never fill a hole with a made-up value. */
    fun encode(): ByteArray? {
        if (length < 4) return null
        val out = ArrayList<Byte>()
        fun i16(v: Int) {
            val c = v.coerceIn(Short.MIN_VALUE.toInt(), Short.MAX_VALUE.toInt())
            out.add((c and 0xFF).toByte()); out.add(((c shr 8) and 0xFF).toByte())
        }
        fun flag(v: Int) { out.add(if (v != 0) 1.toByte() else 0.toByte()) }
        i16(this[SettingField.FTP] ?: return null)
        i16(this[SettingField.TZ] ?: return null)
        if (length >= 6) {
            flag(this[SettingField.USE_MILES] ?: return null)
            out.add((this[SettingField.BACKLIGHT] ?: return null).coerceIn(0, 255).toByte())
        }
        if (length >= 7) flag(this[SettingField.CLOCK_24H] ?: return null)
        if (length >= 8) flag(this[SettingField.USB_DRIVE] ?: return null)
        return out.toByteArray()
    }

    companion object {
        /** Parse a settings read/notify. null when it is too short to be one. */
        fun decode(d: ByteArray): DeviceSettingsValues? {
            if (d.size < 4) return null
            fun u(i: Int) = d[i].toInt() and 0xFF
            val m = mutableMapOf(
                SettingField.FTP.key to (u(0) or (u(1) shl 8)).toShort().toInt(),
                SettingField.TZ.key to (u(2) or (u(3) shl 8)).toShort().toInt(),
            )
            if (d.size >= 6) {
                m[SettingField.USE_MILES.key] = if (u(4) != 0) 1 else 0
                m[SettingField.BACKLIGHT.key] = u(5)
            }
            if (d.size >= 7) m[SettingField.CLOCK_24H.key] = if (u(6) != 0) 1 else 0
            if (d.size >= 8) m[SettingField.USB_DRIVE.key] = if (u(7) != 0) 1 else 0
            return DeviceSettingsValues(m, minOf(d.size, SettingField.MAX_LENGTH))
        }
    }
}

/** A change made in the app that the device has not acknowledged. */
data class PendingEdit(val value: Int, val editedAt: Long)

data class SettingsMergeResult(
    /** What the device holds once the push lands (or already holds). */
    val merged: DeviceSettingsValues,
    /** The write to send; null when nothing needs pushing. */
    val payload: ByteArray?,
    /** Fields carried by the push because the rider edited them. */
    val pushed: Map<String, Int>,
    /** Pending edits the device can't take (older firmware): drop them. */
    val dropped: List<SettingField>,
    /** Fields the device ALSO changed since the cache last saw it. */
    val replacedDeviceChange: List<SettingField>,
)

object SettingsMerge {
    /**
     * Merge the device's current values with the rider's pending edits.
     * [lastKnown] is the cache from before this report (null if never seen),
     * used only to tell whether the device side changed a field too.
     */
    fun merge(
        device: DeviceSettingsValues,
        lastKnown: DeviceSettingsValues?,
        pending: Map<String, PendingEdit>,
    ): SettingsMergeResult {
        var merged = device
        val pushed = linkedMapOf<String, Int>()
        val dropped = mutableListOf<SettingField>()
        val replaced = mutableListOf<SettingField>()
        for (f in SettingField.entries) {
            val edit = pending[f.key] ?: continue
            if (!device.supports(f)) { dropped += f; continue }
            val before = lastKnown?.get(f)
            val now = device[f]
            if (before != null && now != null && before != now && now != edit.value) replaced += f
            merged = merged.with(f, edit.value)
            if (now != edit.value) pushed[f.key] = edit.value
        }
        val payload = if (pushed.isEmpty()) null else merged.encode()
        return SettingsMergeResult(merged, payload, pushed, dropped, replaced)
    }

    /** Pending edits the device already matches: no write needed, clear them. */
    fun clearSatisfied(pending: Map<String, PendingEdit>, device: DeviceSettingsValues) =
        pending.filter { (k, e) -> SettingField.of(k)?.let { device[it] != e.value } ?: false }

    /** After an acknowledged write: clear each pushed field whose pending value
     *  is still the one sent; a field re-edited mid-write keeps its newer edit. */
    fun clearAcknowledged(pending: Map<String, PendingEdit>, pushed: Map<String, Int>) =
        pending.filter { (k, e) -> pushed[k] != e.value }

    /** The device's values with the acknowledged push applied — only the
     *  pushed fields, so a notify that arrived during the write isn't rolled
     *  back for fields the rider didn't touch. */
    fun applyAcknowledged(device: DeviceSettingsValues, pushed: Map<String, Int>) =
        device.copy(values = device.values + pushed)

    /** What the screen shows: pending, else device, else null (display default). */
    fun effective(f: SettingField, device: DeviceSettingsValues?, pending: Map<String, PendingEdit>) =
        pending[f.key]?.value ?: device?.get(f)
}

/** A layout saved in the app that the device hasn't taken yet. */
data class PendingDash(
    /** The edited layout, normalized config text. */
    val text: String,
    /** The device layout this edit started from (null: never seen — ask). */
    val baseText: String?,
    val editedAt: Long,
)

object DashSync {
    enum class Action { ADOPT_DEVICE, ALREADY_IN_SYNC, PUSH_PHONE, CONFLICT }

    /** Texts are normalized config text (DashConfig.configText) on both sides. */
    fun resolve(device: String, pending: PendingDash?): Action = when {
        pending == null -> Action.ADOPT_DEVICE
        pending.text == device -> Action.ALREADY_IN_SYNC
        pending.baseText != null && pending.baseText == device -> Action.PUSH_PHONE
        else -> Action.CONFLICT
    }

    /** Save an edit on top of whatever is pending. The base stays the one the
     *  FIRST unsynced edit started from; editing back to it cancels. */
    fun saving(text: String, deviceText: String?, over: PendingDash?, now: Long): PendingDash? {
        if (over == null && deviceText != null && deviceText == text) return null
        val base = over?.baseText ?: deviceText
        if (base != null && base == text) return null
        return PendingDash(text, base, now)
    }
}

/** Everything remembered about one device's settings. */
data class DeviceSettingsCache(
    val device: DeviceSettingsValues? = null,
    val pending: Map<String, PendingEdit> = emptyMap(),
    /** The device's last reported layout (normalized config text). */
    val dashText: String? = null,
    val pendingDash: PendingDash? = null,
) {
    val hasPending: Boolean get() = pending.isNotEmpty() || pendingDash != null

    companion object {
        const val UNPAIRED = "_unpaired"

        /**
         * Switching the current device to [id]: offline edits made before any
         * device was known ([previousId] == UNPAIRED) move to the first device
         * — they were made for "my device", and this is the first there is.
         * Returns the cache to use for [id] and whether the orphan was adopted.
         */
        fun adopt(previousId: String, id: String, target: DeviceSettingsCache,
                  orphan: DeviceSettingsCache): DeviceSettingsCache {
            if (previousId != UNPAIRED || id == UNPAIRED) return target
            if (!orphan.hasPending) return target
            val pending = orphan.pending + target.pending
            val dash = target.pendingDash ?: orphan.pendingDash?.copy(baseText = null)
            return target.copy(pending = pending, pendingDash = dash)
        }
    }
}
