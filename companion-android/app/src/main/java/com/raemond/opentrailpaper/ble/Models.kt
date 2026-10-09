package com.raemond.opentrailpaper.ble

import com.raemond.opentrailpaper.data.LatLon
import java.util.UUID

// GATT UUIDs — must match src/ble_server.cpp on the device, and companion-ios
// Sources/BLEManager.swift.
object BikeUuid {
    val service: UUID = UUID.fromString("B1C50000-9E0F-4B7A-9C6D-1F2E3A4B5C6D")
    val settings: UUID = UUID.fromString("B1C50001-9E0F-4B7A-9C6D-1F2E3A4B5C6D")
    val status: UUID = UUID.fromString("B1C50002-9E0F-4B7A-9C6D-1F2E3A4B5C6D")
    val route: UUID = UUID.fromString("B1C50003-9E0F-4B7A-9C6D-1F2E3A4B5C6D")
    val rides: UUID = UUID.fromString("B1C50004-9E0F-4B7A-9C6D-1F2E3A4B5C6D")
    val ota: UUID = UUID.fromString("B1C50005-9E0F-4B7A-9C6D-1F2E3A4B5C6D")
    val sensors: UUID = UUID.fromString("B1C50006-9E0F-4B7A-9C6D-1F2E3A4B5C6D")
    val map: UUID = UUID.fromString("B1C50007-9E0F-4B7A-9C6D-1F2E3A4B5C6D")
    val agnss: UUID = UUID.fromString("B1C50008-9E0F-4B7A-9C6D-1F2E3A4B5C6D")
    val dash: UUID = UUID.fromString("B1C50009-9E0F-4B7A-9C6D-1F2E3A4B5C6D")
    val mesh: UUID = UUID.fromString("B1C5000A-9E0F-4B7A-9C6D-1F2E3A4B5C6D")
    val media: UUID = UUID.fromString("B1C5000B-9E0F-4B7A-9C6D-1F2E3A4B5C6D")
    val workout: UUID = UUID.fromString("B1C5000C-9E0F-4B7A-9C6D-1F2E3A4B5C6D")

    /** The standard Client Characteristic Configuration descriptor. */
    val cccd: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
}

/**
 * A cycling sensor known to the head unit. kindsMask bits match the firmware's
 * ble_sensors::Kind: 1 HR, 2 power, 4 cadence, 8 radar, 16 speed. Cadence and
 * speed share one BLE service, so a device the head unit hasn't connected to
 * yet carries both ("speed & cadence"); once it has read the sensor's feature
 * list a speed sensor shows as Speed, a cadence sensor as Cadence, a combo as
 * both.
 */
data class BikeSensor(
    val addr: String,
    val name: String,
    val kindsMask: Int,
    val connected: Boolean,
    val paired: Boolean,
    val rssi: Int,
) {
    val kindsText: String
        get() {
            val parts = buildList {
                if (kindsMask and 1 != 0) add("Heart rate")
                if (kindsMask and 2 != 0) add("Power")
                if (kindsMask and 4 != 0 && kindsMask and 16 != 0) {
                    add("Speed & cadence")
                } else {
                    if (kindsMask and 4 != 0) add("Cadence")
                    if (kindsMask and 16 != 0) add("Speed")
                }
                if (kindsMask and 8 != 0) add("Radar")
            }
            return if (parts.isEmpty()) "Sensor" else parts.joinToString(" + ")
        }
}

/** A vector map already stored on the device (its coverage bounds). */
data class DeviceMap(
    val south: Double,
    val west: Double,
    val north: Double,
    val east: Double,
    val builtin: Boolean,
) {
    val corners: List<LatLon>
        get() = listOf(
            LatLon(south, west), LatLon(south, east),
            LatLon(north, east), LatLon(north, west),
        )
}

/** A recorded ride file on the device. */
data class RideFile(val name: String, val size: Int)

/** A per-day diagnostics log file on the device (/logs/YYYYMMDD.log). */
data class LogFile(val name: String, val size: Int)

/** Live status pushed by the device once a second. */
data class DeviceStatus(
    val gpsFix: Boolean = false,
    val recording: Boolean = false,
    val hasRoute: Boolean = false,
    val battery: Int = 0,
    val sats: Int = 0,
    val heartRate: Int? = null,
    val power: Int? = null,
    val speedKmh: Double = 0.0,
    val remainingKm: Double = 0.0,
)

/**
 * How a system permission stands right now.
 *
 * Kept as four cases rather than a Bool because each one calls for different
 * UI: [NOT_DETERMINED] means we still owe the user the system prompt, [DENIED]
 * can only be undone in Settings, and [UNAVAILABLE] (blocked by device policy)
 * can't be undone at all — sending someone to Settings for that is a dead end.
 */
enum class PermissionState {
    NOT_DETERMINED, GRANTED, DENIED, UNAVAILABLE;

    val isGranted get() = this == GRANTED

    /** Whether the system Settings app can actually change this. */
    val fixableInSettings get() = this == DENIED
}

/** One turn cue: where it happens + what to do. */
data class Maneuver(val lat: Double, val lon: Double, val text: String)

/**
 * Speed-sensor wheel circumferences for common tyres (mm), the usual head-unit
 * table. Limits match the firmware's wheel_speed::kMinCircMm..kMaxCircMm.
 */
object WheelSize {
    const val DEFAULT_MM = 2105
    const val MIN_MM = 800
    const val MAX_MM = 3500

    data class Preset(val name: String, val mm: Int)

    val presets = listOf(
        Preset("700x23c", 2096),
        Preset("700x25c", 2105),
        Preset("700x28c", 2136),
        Preset("700x32c", 2155),
        Preset("700x35c", 2168),
        Preset("700x40c", 2200),
        Preset("650b x 47", 2081),
        Preset("26 x 2.1", 2068),
        Preset("27.5 x 2.2", 2148),
        Preset("29 x 2.2", 2298),
        Preset("29 x 2.4", 2326),
        Preset("20 x 1.75 (406)", 1515),
        Preset("16 x 1.35 (349)", 1272),
    )

    fun clamp(mm: Int) = mm.coerceIn(MIN_MM, MAX_MM)
    fun presetName(mm: Int): String? = presets.firstOrNull { it.mm == mm }?.name
}
