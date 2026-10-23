package com.raemond.opentrailpaper.ble

import com.raemond.opentrailpaper.data.DeviceSettingsValues
import com.raemond.opentrailpaper.data.SettingField
import com.raemond.opentrailpaper.link.proto.MapCoverage
import com.raemond.opentrailpaper.link.proto.RideState
import com.raemond.opentrailpaper.link.proto.Sensor
import com.raemond.opentrailpaper.link.proto.Settings
import com.raemond.opentrailpaper.link.proto.MeshChannel as PMeshChannel
import com.raemond.opentrailpaper.link.proto.MeshMessage as PMeshMessage
import com.raemond.opentrailpaper.link.proto.MeshNode as PMeshNode
import com.raemond.opentrailpaper.link.proto.MeshPreset as PMeshPreset
import com.raemond.opentrailpaper.link.proto.MeshRegion as PMeshRegion
import com.raemond.opentrailpaper.link.proto.MeshState as PMeshState
import com.raemond.opentrailpaper.link.proto.MeshStats as PMeshStats
import com.raemond.opentrailpaper.link.proto.WorkoutStatus as PWorkoutStatus

/**
 * The phone link's messages (proto/link.proto) as the app's own models — the
 * same ones the legacy byte parsers in BleManager fill, so every screen works
 * the same over either protocol. Pure (no android.*), so the JVM tests cover it.
 */
object LinkCodec {

    /** LiveFrame.ride as the status the app has always shown. */
    fun status(r: RideState): DeviceStatus = DeviceStatus(
        gpsFix = r.fix,
        recording = r.recording,
        hasRoute = r.routeActive,
        battery = r.batteryPct,
        sats = r.sats,
        heartRate = if (r.hasHrBpm()) r.hrBpm else null,
        power = if (r.hasPowerW()) r.powerW else null,
        speedKmh = if (r.hasSpeedDkmh()) r.speedDkmh / 10.0 else 0.0,
        remainingKm = if (r.hasRouteLeftM()) r.routeLeftM / 1000.0 else 0.0,
        cadence = if (r.hasCadenceRpm()) r.cadenceRpm else null,
        distanceM = r.distanceM.toLong() and 0xFFFF_FFFFL,
        elapsedSec = r.elapsedS.toLong() and 0xFFFF_FFFFL,
        movingSec = r.movingS.toLong() and 0xFFFF_FFFFL,
        climbM = r.climbM,
        autoPaused = r.autoPaused,
        charging = r.charging,
        gradePct = if (r.hasGradeDpct()) r.gradeDpct / 10.0 else null,
        altitudeM = if (r.hasAltitudeM()) r.altitudeM else null,
    )

    /**
     * Settings as the cache's device values. The link always carries every
     * field the firmware has, so the "length" the legacy cache keys support on
     * is 10 with a wheel size (FEATURE_WHEEL_SIZE) and 8 without. Fields the
     * message leaves out (a partial push) keep their [previous] value.
     */
    fun settings(s: Settings, wheel: Boolean, previous: DeviceSettingsValues?): DeviceSettingsValues {
        val m = HashMap(previous?.values ?: emptyMap())
        if (s.hasFtpW()) m[SettingField.FTP.key] = s.ftpW
        if (s.hasTzMin()) m[SettingField.TZ.key] = s.tzMin
        if (s.hasUseMiles()) m[SettingField.USE_MILES.key] = if (s.useMiles) 1 else 0
        if (s.hasBacklight()) m[SettingField.BACKLIGHT.key] = s.backlight
        if (s.hasClock24H()) m[SettingField.CLOCK_24H.key] = if (s.clock24H) 1 else 0
        if (s.hasUsbDrive()) m[SettingField.USB_DRIVE.key] = if (s.usbDrive) 1 else 0
        if (wheel && s.hasWheelMm()) m[SettingField.WHEEL_CIRC.key] = s.wheelMm
        if (!wheel) m.remove(SettingField.WHEEL_CIRC.key)
        return DeviceSettingsValues(m, if (wheel) SettingField.MAX_LENGTH else 8)
    }

    /** A set_settings carrying only [changed] — the link sends just what moved. */
    fun settingsChange(changed: Map<String, Int>): Settings {
        val b = Settings.newBuilder()
        for ((k, v) in changed) {
            when (SettingField.of(k)) {
                SettingField.FTP -> b.setFtpW(v)
                SettingField.TZ -> b.setTzMin(v)
                SettingField.USE_MILES -> b.setUseMiles(v != 0)
                SettingField.BACKLIGHT -> b.setBacklight(v)
                SettingField.CLOCK_24H -> b.setClock24H(v != 0)
                SettingField.USB_DRIVE -> b.setUsbDrive(v != 0)
                SettingField.WHEEL_CIRC -> b.setWheelMm(v)
                null -> Unit
            }
        }
        return b.build()
    }

    fun sensor(s: Sensor): BikeSensor? {
        if (s.address.isEmpty()) return null
        return BikeSensor(
            addr = s.address,
            name = s.name.ifEmpty { s.address },
            kindsMask = s.kinds,
            connected = s.connected,
            paired = s.paired,
            rssi = s.rssi,
        )
    }

    fun deviceMap(c: MapCoverage) = DeviceMap(c.south, c.west, c.north, c.east, c.builtin)

    fun workoutStatus(w: PWorkoutStatus) = BleManager.WorkoutStatus(
        loaded = w.loaded,
        running = w.running,
        paused = w.paused,
        done = w.done,
        blockIndex = w.segment,
        blockCount = w.segmentCount,
        elapsedSec = w.elapsedS.toLong() and 0xFFFF_FFFFL,
        totalSec = w.totalS.toLong() and 0xFFFF_FFFFL,
        targetW = w.targetW,
        ftpW = w.ftpW,
        pauseEachBlock = w.pauseEach,
        name = w.name,
    )

    // MARK: mesh

    /** [radioConfig]: FEATURE_MESH_RADIO_CONFIG — region, power and slot are real. */
    fun meshState(s: PMeshState, radioConfig: Boolean) = MeshState(
        enabled = s.enabled,
        radioOk = s.radioOk,
        channelFollowsPreset = s.channelFollowsPreset,
        nodeNum = s.nodeNum,
        frequencyHz = s.freqHz.toLong() and 0xFFFF_FFFFL,
        channelKey = s.pskIndex,
        nodeCount = s.nodeCount,
        unread = s.unread,
        channel = s.channelName,
        shortName = s.shortName,
        longName = s.longName,
        presetIndex = s.preset,
        regionIndex = if (radioConfig) s.region else null,
        txPowerDbm = if (radioConfig) s.txPowerDbm else null,
        slotOverride = if (radioConfig) s.slotOverride else null,
        slotCount = if (radioConfig) s.slotCount else null,
        activeSlot = if (radioConfig) s.activeSlot else null,
    )

    /** Hops the legacy way: 0xFF (or anything over 7) = unknown. */
    private fun hops(has: Boolean, v: Int) = if (has) v else 0xFF

    fun meshMessage(m: PMeshMessage, nowMs: Long): MeshMessage {
        val utc = m.utc.toLong() and 0xFFFF_FFFFL
        val age = m.ageMs.toLong() and 0xFFFF_FFFFL
        return MeshMessage(
            id = m.id,
            from = m.from,
            to = m.to,
            outgoing = m.outgoing,
            channel = m.channel,
            status = MeshMessage.Status.from(m.deliveryValue),
            text = m.text,
            // The GPS stamp when the device has one, else the phone's clock less the age.
            timeMs = if (utc > 0) utc * 1000 else nowMs - age,
            rssi = m.rssi,
            snr = m.snr,
            hops = hops(m.hasHops(), m.hops),
        )
    }

    fun meshNode(n: PMeshNode, nowMs: Long): MeshNode {
        val pos = if (n.hasPosition()) {
            val p = n.position
            MeshNodePosition(
                latitude = p.latE7 / 1e7,
                longitude = p.lonE7 / 1e7,
                altitudeM = p.altM,
                satsInView = p.sats,
                precisionBits = p.precisionBits,
                receivedMs = nowMs - (p.ageMs.toLong() and 0xFFFF_FFFFL),
            )
        } else {
            null
        }
        return MeshNode(
            num = n.num,
            shortName = n.shortName,
            longName = n.longName,
            lastHeardMs = nowMs - (n.ageMs.toLong() and 0xFFFF_FFFFL),
            rssi = n.rssi,
            snr = n.snr,
            hops = hops(n.hasHops(), n.hops),
            position = pos,
        )
    }

    fun meshPreset(p: PMeshPreset) = MeshPreset(
        index = p.index,
        name = p.name,
        sf = p.sf,
        bandwidthKhz = p.bwKhzX10 / 10.0,
        codingRate = p.cr,
    )

    fun meshRegion(r: PMeshRegion) = MeshRegion(
        index = r.index,
        name = r.name,
        startHz = r.startHz.toLong() and 0xFFFF_FFFFL,
        endHz = r.endHz.toLong() and 0xFFFF_FFFFL,
        spacingHz = r.spacingHz.toLong() and 0xFFFF_FFFFL,
        powerLimitDbm = r.powerLimitDbm,
    )

    fun meshChannel(c: PMeshChannel) = MeshChannel(
        index = c.index,
        name = c.name,
        hash = c.hash,
        psk = c.psk.toByteArray(),
        sharesLocation = c.shareLocation,
    )

    fun meshStats(s: PMeshStats) = MeshStats(
        rx = s.rx, rxDropped = s.rxDropped, rxOtherChannel = s.rxOtherChannel,
        rxDuplicate = s.rxDuplicate, tx = s.tx, txFailed = s.txFailed, acksRx = s.acksRx,
    )
}
