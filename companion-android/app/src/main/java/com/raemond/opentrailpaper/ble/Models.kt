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

/** The phone link (docs/ble-link.md): one service, RX (phone -> device) and
 *  TX (device -> phone notifications), carrying proto/link.proto messages. */
object LinkUuid {
    val service: UUID = UUID.fromString("B1C51000-9E0F-4B7A-9C6D-1F2E3A4B5C6D")
    val rx: UUID = UUID.fromString("B1C51001-9E0F-4B7A-9C6D-1F2E3A4B5C6D")
    val tx: UUID = UUID.fromString("B1C51002-9E0F-4B7A-9C6D-1F2E3A4B5C6D")
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

/**
 * The device's SD card, from status flag bits 3-5. Firmware from before it was
 * reported never sets bit 3, so that reads as [UNKNOWN], not as a missing card.
 */
enum class SdCardState {
    UNKNOWN, OK, MISSING, UNREADABLE;

    /** No usable card: nothing records and map tiles have nowhere to go. */
    val isProblem: Boolean get() = this == MISSING || this == UNREADABLE
}

/**
 * Live status pushed by the device once a second. The first nine fields are
 * the legacy STATUS packet; the rest only come over the phone link
 * (LiveFrame.ride) and stay null on older firmware.
 */
data class DeviceStatus(
    val sdCard: SdCardState = SdCardState.UNKNOWN,
    val gpsFix: Boolean = false,
    val recording: Boolean = false,
    val hasRoute: Boolean = false,
    val battery: Int = 0,
    val sats: Int = 0,
    val heartRate: Int? = null,
    val power: Int? = null,
    val speedKmh: Double = 0.0,
    val remainingKm: Double = 0.0,
    val cadence: Int? = null,
    val distanceM: Long? = null,
    val elapsedSec: Long? = null,
    val movingSec: Long? = null,
    val climbM: Int? = null,
    val autoPaused: Boolean = false,
    val charging: Boolean = false,
    val gradePct: Double? = null,
    val altitudeM: Int? = null,
) {
    /** Ride average over moving time, when the device reports both. */
    val avgSpeedKmh: Double?
        get() {
            val d = distanceM ?: return null
            val t = movingSec ?: return null
            return if (t > 0) d / 1000.0 / (t / 3600.0) else null
        }
}

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
 * Speed-sensor wheel size from rim and tyre: circumference = π × (rim bead seat
 * diameter + 2 × tyre width), the tyre's height taken as its width. Within
 * about 0.5% of a measured roll-out for road and MTB tyres. The device only
 * stores the mm; limits match the firmware's wheel_speed::kMinCircMm..kMaxCircMm.
 */
object WheelSize {
    const val DEFAULT_MM = 2105
    const val MIN_MM = 800
    const val MAX_MM = 3500
    const val MIN_TYRE_MM = 18
    const val MAX_TYRE_MM = 120
    const val DEFAULT_RIM = 622

    /** A rim by its ISO bead seat diameter (mm). */
    data class Rim(val bsd: Int, val name: String)

    val rims = listOf(
        Rim(622, "700c / 29\""),
        Rim(584, "650b / 27.5\""),
        Rim(571, "650c"),
        Rim(559, "26\""),
        Rim(507, "24\""),
        Rim(406, "20\""),
        Rim(349, "16\" (Brompton)"),
    )

    fun rim(bsd: Int): Rim = rims.firstOrNull { it.bsd == bsd } ?: rims[0]

    fun clamp(mm: Int) = mm.coerceIn(MIN_MM, MAX_MM)

    fun circumference(bsd: Int, tyreMm: Int): Int =
        clamp(Math.round(Math.PI * (bsd + 2 * tyreMm)).toInt())

    /** The rim and tyre width that give exactly [mm], trying [preferredRim]
     *  first (650b × 47 and 700c × 28 are the same wheel). null when no
     *  combination does: a size set over serial or by an older app. */
    fun decode(mm: Int, preferredRim: Int): Pair<Int, Int>? {
        for (r in listOf(rim(preferredRim)) + rims) {
            val tyre = Math.round((mm / Math.PI - r.bsd) / 2).toInt()
            if (tyre in MIN_TYRE_MM..MAX_TYRE_MM && circumference(r.bsd, tyre) == mm) return r.bsd to tyre
        }
        return null
    }

    /** "28 mm", or "56 mm · 2.2\"" for tyres sized in inches. */
    fun tyreLabel(mm: Int): String =
        if (mm >= 45) "$mm mm · ${"%.1f".format(java.util.Locale.US, mm / 25.4)}\"" else "$mm mm"
}
