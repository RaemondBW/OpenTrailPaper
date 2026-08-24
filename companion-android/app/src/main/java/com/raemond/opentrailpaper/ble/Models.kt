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

/** A cycling sensor known to the head unit (HR / power / cadence). */
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
                if (kindsMask and 4 != 0) add("Cadence")
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
