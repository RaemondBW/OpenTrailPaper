package com.raemond.opentrailpaper.ble

import com.raemond.opentrailpaper.data.LatLon
import java.util.Locale
import kotlin.math.pow

// The Meshtastic mesh, as the device reports it. Mirrors the types at the top of
// companion-ios/Sources/BLEManager.swift; the wire format both sides parse is
// documented in src/ble_server.cpp next to the opcodes.
//
// Node numbers are Meshtastic u32s carried in an Int. Kotlin's UInt would have to
// be unwrapped at every Java interop boundary in this file's callers, and the
// only two things done with a node number are equality and "%08x" — and Java's
// %x already formats an Int as unsigned two's complement, so !ffffffff comes out
// right without the ceremony.

/** One message in the mesh conversation, sent or received. */
data class MeshMessage(
    /**
     * The Meshtastic packet id, which is what makes this identifiable at all: the
     * device streams its whole history whenever something changes, so the list is
     * rebuilt from scratch each time and rows have to survive that.
     */
    val id: Int,
    val from: Int,
    val to: Int,
    val outgoing: Boolean,
    /**
     * Which channel it belongs to. 0 is the primary (public) channel; anything
     * else is a private one, and this is what keeps those threads apart.
     */
    val channel: Int,
    val status: Status,
    val text: String,
    /**
     * When it happened, in epoch millis. The device often has no wall clock (no
     * GPS fix yet, no RTC set), so the phone's own clock minus the age it
     * reported is the only date either end can agree on.
     */
    val timeMs: Long,
    val rssi: Int,
    val snr: Int,
    val hops: Int,
) {
    enum class Status { PENDING, SENT, ACKED, FAILED;

        companion object {
            fun from(raw: Int): Status = entries.getOrElse(raw) { PENDING }
        }
    }

    val isBroadcast: Boolean get() = to == MeshState.BROADCAST_ADDR
}

/** A position a node broadcast over the mesh. */
data class MeshNodePosition(
    val latitude: Double,
    val longitude: Double,
    val altitudeM: Int,
    val satsInView: Int,
    /**
     * How much of the coordinate the sender chose to publish; 32 is full
     * precision. Meshtastic lets a node blur its position on purpose, so a coarse
     * fix must not be drawn as though it were exact.
     */
    val precisionBits: Int,
    val receivedMs: Long,
) {
    val coordinate: LatLon get() = LatLon(latitude, longitude)
    val isImprecise: Boolean get() = precisionBits < 32

    /**
     * Rough radius the sender's blurring leaves, for describing the uncertainty.
     * Each bit dropped from a coordinate doubles the box it could be in.
     */
    val uncertaintyM: Double?
        get() {
            if (!isImprecise) return null
            // 2^(32 - bits) steps of ~360/2^32 degrees, taken at the equator.
            val steps = 2.0.pow((32 - precisionBits).toDouble())
            return steps * (40_075_000 / 2.0.pow(32.0))
        }

    val shortText: String
        get() = String.format(Locale.US, "%.5f, %.5f", latitude, longitude)
}

/** A neighbour the device has heard on the mesh. */
data class MeshNode(
    val num: Int,
    val shortName: String,
    val longName: String,
    val lastHeardMs: Long,
    val rssi: Int,
    val snr: Int,
    val hops: Int,
    /**
     * Where it last said it was. null is the common case and means "has not sent
     * a position", which is different from being at 0,0 — nodes broadcast
     * position on their own schedule, and many never do.
     */
    val position: MeshNodePosition?,
) {
    /** "!a4c1380c" — how Meshtastic writes a node number everywhere. */
    val nodeId: String get() = meshNodeId(num)
    val displayName: String get() = longName.ifEmpty { nodeId }
}

/** "!a4c1380c" — the one place a node number becomes text. */
fun meshNodeId(num: Int): String = "!" + String.format(Locale.US, "%08x", num)

/** The device's mesh configuration, as it reports it. */
data class MeshState(
    val enabled: Boolean = false,
    val radioOk: Boolean = false,
    /**
     * No explicit channel name is set, so [channel] is the modem preset's name —
     * what a stock Meshtastic node does, and what keeps the two in step.
     */
    val channelFollowsPreset: Boolean = true,
    val nodeNum: Int = 0,
    val frequencyHz: Long = 0,
    val channel: String = "",
    val channelKey: Int = 1,
    val longName: String = "",
    val shortName: String = "",
    val nodeCount: Int = 0,
    val unread: Int = 0,
    /** Index into `BleManager.meshPresets`. */
    val presetIndex: Int = 0,
) {
    val nodeId: String get() = meshNodeId(nodeNum)
    val frequencyMHz: Double get() = frequencyHz / 1_000_000.0

    /** Nothing has been heard from the device yet. */
    val isUnknown: Boolean get() = nodeNum == 0

    companion object {
        const val BROADCAST_ADDR: Int = -1        // 0xFFFFFFFF
    }
}

/**
 * A modem preset the device supports: how fast it talks, as opposed to the
 * channel, which is where. Streamed from the device rather than hard-coded here,
 * because the firmware owns which modems exist (`mesh::kPresets`) and a copy of
 * that table on the phone is a copy that drifts.
 */
data class MeshPreset(
    val index: Int,
    val name: String,
    val sf: Int,
    val bandwidthKhz: Double,
    val codingRate: Int,
) {
    val detail: String get() = String.format(Locale.US, "SF%d · %.0f kHz", sf, bandwidthKhz)
}

/**
 * One channel the device holds. Slot 0 is the primary: its name sets the
 * frequency, so it is the one that decides which mesh the device is on.
 */
data class MeshChannel(
    val index: Int,
    val name: String,
    val hash: Int,
    /**
     * The key as it is SHARED: empty = unencrypted, 1 byte = one of Meshtastic's
     * well-known keys, 16 or 32 bytes = a real key.
     */
    val psk: ByteArray,
    /**
     * Whether the device broadcasts our position on this channel. Off by default
     * and per channel, so it can be on with a ride group and off in public.
     */
    val sharesLocation: Boolean,
) {
    val isPrimary: Boolean get() = index == 0
    val displayName: String get() = name.ifEmpty { "(unnamed)" }

    /** A channel only outsiders cannot read — i.e. not one of the published keys. */
    val isPrivate: Boolean get() = psk.size >= 16

    val keyDescription: String
        get() = when (psk.size) {
            0 -> "unencrypted"
            1 -> "well-known key ${psk[0].toInt() and 0xFF}"
            else -> "${psk.size * 8}-bit key"
        }

    // A ByteArray member means the generated equals/hashCode compare identities,
    // which would make every channel look changed on every refresh and rebuild
    // the whole channel bar four times a second during a download.
    override fun equals(other: Any?): Boolean {
        if (this === other) return true
        if (other !is MeshChannel) return false
        return index == other.index && name == other.name && hash == other.hash &&
            sharesLocation == other.sharesLocation && psk.contentEquals(other.psk)
    }

    override fun hashCode(): Int {
        var h = index
        h = 31 * h + name.hashCode()
        h = 31 * h + hash
        h = 31 * h + sharesLocation.hashCode()
        h = 31 * h + psk.contentHashCode()
        return h
    }
}

/** Packet counters, for the diagnostics view. */
data class MeshStats(
    val rx: Int = 0,
    val rxDropped: Int = 0,
    val rxOtherChannel: Int = 0,
    val rxDuplicate: Int = 0,
    val tx: Int = 0,
    val txFailed: Int = 0,
    val acksRx: Int = 0,
)
