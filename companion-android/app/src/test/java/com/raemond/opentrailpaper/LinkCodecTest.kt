package com.raemond.opentrailpaper

import com.raemond.opentrailpaper.ble.LinkCodec
import com.raemond.opentrailpaper.ble.SdCardState
import com.raemond.opentrailpaper.data.DashConfig
import com.raemond.opentrailpaper.data.SettingField
import com.raemond.opentrailpaper.link.proto.MeshMessage
import com.raemond.opentrailpaper.link.proto.RideState
import com.raemond.opentrailpaper.link.proto.SdCard
import com.raemond.opentrailpaper.link.proto.Settings
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/** The link's messages as the app's models (ble/LinkCodec.kt). */
class LinkCodecTest {

    @Test fun rideStateAbsentMeansNoReading() {
        val s = LinkCodec.status(
            RideState.newBuilder().setFix(true).setBatteryPct(80).setSats(9)
                .setSpeedDkmh(253).setPowerW(0).setDistanceM(12_000).setMovingS(1_800).build(),
        )
        assertTrue(s.gpsFix)
        assertEquals(25.3, s.speedKmh, 1e-9)
        assertEquals(0, s.power)            // a real zero, not "none"
        assertNull(s.heartRate)             // absent = no reading
        assertNull(s.cadence)
        assertEquals(24.0, s.avgSpeedKmh!!, 1e-9)
    }

    @Test fun sdCardUnknownFromOlderFirmwareIsNotMissing() {
        assertEquals(SdCardState.UNKNOWN, LinkCodec.status(RideState.getDefaultInstance()).sdCard)
        assertFalse(SdCardState.UNKNOWN.isProblem)
        val missing = LinkCodec.status(RideState.newBuilder().setSdCard(SdCard.SD_CARD_MISSING).build())
        assertEquals(SdCardState.MISSING, missing.sdCard)
        assertTrue(missing.sdCard.isProblem)
        assertEquals(SdCardState.UNREADABLE, LinkCodec.sdCard(SdCard.SD_CARD_UNREADABLE))
        assertEquals(SdCardState.OK, LinkCodec.sdCard(SdCard.SD_CARD_OK))
    }

    @Test fun settingsChangeCarriesOnlyTheEditedFields() {
        val m = LinkCodec.settingsChange(mapOf("ftp" to 260, "clock24h" to 0))
        assertTrue(m.hasFtpW())
        assertEquals(260, m.ftpW)
        assertTrue(m.hasClock24H())
        assertFalse(m.clock24H)
        assertFalse(m.hasTzMin())
        assertFalse(m.hasWheelMm())
    }

    @Test fun settingsLengthFollowsTheWheelFeature() {
        val full = Settings.newBuilder().setFtpW(250).setTzMin(-420).setUseMiles(true).setBacklight(2)
            .setClock24H(true).setUsbDrive(false).setWheelMm(2105).build()
        val withWheel = LinkCodec.settings(full, wheel = true, previous = null)
        assertTrue(withWheel.supports(SettingField.WHEEL_CIRC))
        assertEquals(2105, withWheel[SettingField.WHEEL_CIRC])
        assertEquals(-420, withWheel[SettingField.TZ])
        val without = LinkCodec.settings(full, wheel = false, previous = null)
        assertFalse(without.supports(SettingField.WHEEL_CIRC))
        assertTrue(without.supports(SettingField.USB_DRIVE))
        // A partial push keeps what was known.
        val partial = LinkCodec.settings(Settings.newBuilder().setBacklight(0).build(), true, withWheel)
        assertEquals(0, partial[SettingField.BACKLIGHT])
        assertEquals(250, partial[SettingField.FTP])
    }

    @Test fun meshHopsAbsentIsUnknown() {
        val m = LinkCodec.meshMessage(MeshMessage.newBuilder().setText("hi").setAgeMs(5_000).build(), nowMs = 100_000)
        assertEquals(0xFF, m.hops)
        assertEquals(95_000, m.timeMs)
        val h = LinkCodec.meshMessage(MeshMessage.newBuilder().setHops(0).setUtc(1_700_000_000).build(), nowMs = 0)
        assertEquals(0, h.hops)
        assertEquals(1_700_000_000_000L, h.timeMs)
    }

    @Test fun dashConfigKeepsSevenPagesAndTheMap() {
        val text = buildString {
            append("speed large\n")
            repeat(7) { append("page\npower medium\n") }
            append("page map\n")
        }
        val cfg = DashConfig.parse(text)
        assertEquals(DashConfig.PARSE_MAX_PAGES, cfg.contentPages)
        assertEquals(1, cfg.pages.count { it.isMap })
    }
}
