package com.raemond.opentrailpaper

import com.raemond.opentrailpaper.data.DashSync
import com.raemond.opentrailpaper.data.DeviceSettingsCache
import com.raemond.opentrailpaper.data.DeviceSettingsValues
import com.raemond.opentrailpaper.data.PendingDash
import com.raemond.opentrailpaper.data.PendingEdit
import com.raemond.opentrailpaper.data.SettingField
import com.raemond.opentrailpaper.data.SettingsMerge
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Offline settings merge. The iOS app runs the same cases against its Swift
 * twin with tools/offline_settings_test/run.sh.
 */
class DeviceSettingsSyncTest {
    private val t0 = 1_700_000_000_000L
    private fun edit(v: Int) = PendingEdit(v, t0)
    private fun bytes(vararg b: Int) = ByteArray(b.size) { b[it].toByte() }
    private fun device(b: ByteArray) = DeviceSettingsValues.decode(b)!!

    // ftp 250 (0x00FA), tz -420 (0xFE5C), km, backlight 2, 24h, usb on
    private val full = bytes(0xFA, 0x00, 0x5C, 0xFE, 0, 2, 1, 1)

    @Test fun decodeEncodeRoundTrip() {
        val d = device(full)
        assertEquals(8, d.length)
        assertEquals(250, d[SettingField.FTP])
        assertEquals(-420, d[SettingField.TZ])
        assertEquals(0, d[SettingField.USE_MILES]); assertEquals(2, d[SettingField.BACKLIGHT])
        assertEquals(1, d[SettingField.CLOCK_24H]); assertEquals(1, d[SettingField.USB_DRIVE])
        assertArrayEquals(full, d.encode())
        assertNull(DeviceSettingsValues.decode(bytes(1, 2, 3)))
    }

    @Test fun noPendingNoWrite() {
        val r = SettingsMerge.merge(device(full), device(full), emptyMap())
        assertNull(r.payload)
        assertTrue(r.pushed.isEmpty())
    }

    @Test fun offlineEditWinsUntouchedFieldsTakeDevice() {
        // Offline: backlight -> 0. Meanwhile on the device: units -> miles.
        val onDevice = full.copyOf().also { it[4] = 1 }
        val r = SettingsMerge.merge(device(onDevice), device(full), mapOf("backlight" to edit(0)))
        assertEquals(mapOf("backlight" to 0), r.pushed)
        assertArrayEquals(bytes(0xFA, 0x00, 0x5C, 0xFE, 1, 0, 1, 1), r.payload)
        assertTrue(r.replacedDeviceChange.isEmpty())
    }

    @Test fun sameFieldChangedBothSidesAppWins() {
        val onDevice = full.copyOf().also { it[5] = 3 }   // side button: bright
        val r = SettingsMerge.merge(device(onDevice), device(full), mapOf("backlight" to edit(1)))
        assertEquals(1, r.merged[SettingField.BACKLIGHT])
        assertEquals(listOf(SettingField.BACKLIGHT), r.replacedDeviceChange)
    }

    @Test fun pendingEqualToDeviceNeedsNoWrite() {
        val pending = mapOf("ftp" to edit(250))
        val r = SettingsMerge.merge(device(full), null, pending)
        assertNull(r.payload)
        assertTrue(SettingsMerge.clearSatisfied(pending, device(full)).isEmpty())
    }

    @Test fun olderFirmwareShortPayload() {
        val six = full.copyOf(6)
        val r = SettingsMerge.merge(device(six), null,
            mapOf("ftp" to edit(300), "clock24h" to edit(0)))
        assertEquals(6, r.payload?.size)
        assertEquals(listOf(SettingField.CLOCK_24H), r.dropped)
        assertEquals(mapOf("ftp" to 300), r.pushed)
    }

    @Test fun ackClearsOnlyUnchangedEdits() {
        // ftp re-edited to 310 while the write carrying 300 was in flight.
        val now = mapOf("ftp" to edit(310), "tz" to edit(60))
        val left = SettingsMerge.clearAcknowledged(now, mapOf("ftp" to 300, "tz" to 60))
        assertEquals(mapOf("ftp" to edit(310)), left)
    }

    @Test fun ackAppliesOnlyPushedFields() {
        val notified = full.copyOf().also { it[4] = 1 }   // device-side units change mid-write
        val d = SettingsMerge.applyAcknowledged(device(notified), mapOf("ftp" to 300))
        assertEquals(300, d[SettingField.FTP])
        assertEquals(1, d[SettingField.USE_MILES])
    }

    @Test fun encodeRefusesHoles() {
        val d = DeviceSettingsValues(mapOf("ftp" to 1, "tz" to 0), 8)
        assertNull(d.encode())
    }

    @Test fun effectiveValue() {
        assertNull(SettingsMerge.effective(SettingField.FTP, null, emptyMap()))
        assertEquals(250, SettingsMerge.effective(SettingField.FTP, device(full), emptyMap()))
        assertEquals(9, SettingsMerge.effective(SettingField.FTP, device(full), mapOf("ftp" to edit(9))))
    }

    @Test fun dashResolve() {
        assertEquals(DashSync.Action.ADOPT_DEVICE, DashSync.resolve("A", null))
        val p = PendingDash("B", "A", t0)
        assertEquals(DashSync.Action.ALREADY_IN_SYNC, DashSync.resolve("B", p))
        assertEquals(DashSync.Action.PUSH_PHONE, DashSync.resolve("A", p))
        assertEquals(DashSync.Action.CONFLICT, DashSync.resolve("C", p))
        assertEquals(DashSync.Action.CONFLICT, DashSync.resolve("A", PendingDash("B", null, t0)))
    }

    @Test fun dashSavingKeepsFirstBase() {
        val first = DashSync.saving("B", "A", null, t0)
        assertEquals("A", first?.baseText)
        val second = DashSync.saving("C", "A", first, t0)
        assertEquals("A", second?.baseText); assertEquals("C", second?.text)
        assertNull(DashSync.saving("A", "A", second, t0))
    }

    @Test fun firstDeviceAdoptsUnpairedEdits() {
        val orphan = DeviceSettingsCache(pending = mapOf("ftp" to edit(280)))
        val adopted = DeviceSettingsCache.adopt(DeviceSettingsCache.UNPAIRED, "dev-1",
            DeviceSettingsCache(), orphan)
        assertEquals(edit(280), adopted.pending["ftp"])
        // A second device never inherits the first one's edits.
        val second = DeviceSettingsCache.adopt("dev-1", "dev-2", DeviceSettingsCache(), orphan)
        assertTrue(second.pending.isEmpty())
    }
}
