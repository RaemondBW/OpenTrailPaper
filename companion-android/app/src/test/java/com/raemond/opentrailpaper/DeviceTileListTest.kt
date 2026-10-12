package com.raemond.opentrailpaper

import com.raemond.opentrailpaper.map.DeviceTileList
import com.raemond.opentrailpaper.map.PrebuiltTiles
import com.raemond.opentrailpaper.map.PrebuiltTiles.VersionState
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/** The device-kept tile versions on the wire (DeviceTileList, src/tile_versions.h). */
class DeviceTileListTest {

    @Test fun requestsAskForVersions() {
        assertArrayEquals(byteArrayOf(0x07, 0x01), DeviceTileList.request(0x07))
        assertArrayEquals(byteArrayOf(0x08, 0x01), DeviceTileList.request(0x08))
    }

    @Test fun onlyAFlaggedBeginMeansVersions() {
        assertTrue(DeviceTileList.beginHasVersions(byteArrayOf(0xD0.toByte(), 0x01)))
        assertTrue(DeviceTileList.beginHasVersions(byteArrayOf(0xD3.toByte(), 0x01)))
        // Older firmware: the 1-byte begin.
        assertFalse(DeviceTileList.beginHasVersions(byteArrayOf(0xD0.toByte())))
        assertFalse(DeviceTileList.beginHasVersions(byteArrayOf(0xD0.toByte(), 0x00)))
    }

    @Test fun parsesBareAndVersionedEntries() {
        val ids = mutableListOf<String>()
        val recs = HashMap<String, String>()
        DeviceTileList.parseEntries(
            "861f8d8afffffff=cdn:0123456789abcdef,861f8d8b7ffffff,861f8d8a7ffffff=phone:1760000000",
            ids, recs,
        )
        DeviceTileList.parseEntries("861f8d8b3ffffff=cdn:,,=cdn:x,861f8d8bbffffff=", ids, recs)
        assertEquals(
            listOf("861f8d8afffffff", "861f8d8b7ffffff", "861f8d8a7ffffff", "861f8d8b3ffffff", "861f8d8bbffffff"),
            ids,
        )
        assertEquals(
            mapOf(
                "861f8d8afffffff" to "cdn:0123456789abcdef",
                "861f8d8a7ffffff" to "phone:1760000000",
                "861f8d8b3ffffff" to "cdn:",
            ),
            recs,
        )
    }

    @Test fun oldFirmwareListParsesAsBefore() {
        val ids = mutableListOf<String>()
        val recs = HashMap<String, String>()
        DeviceTileList.parseEntries("861f8d8afffffff,861f8d8b7ffffff", ids, recs)
        assertEquals(listOf("861f8d8afffffff", "861f8d8b7ffffff"), ids)
        assertTrue(recs.isEmpty())
    }

    @Test fun beginNameCarriesTheRecordAfterANul() {
        assertArrayEquals("861f8d8afffffff".toByteArray(), DeviceTileList.beginName("861f8d8afffffff", null))
        val b = DeviceTileList.beginName("861f8d8afffffff.poi", "cdn:0123456789abcdef")
        assertEquals("861f8d8afffffff.poi\u0000cdn:0123456789abcdef", String(b, Charsets.US_ASCII))
        // Fits the firmware's 51-character name field even on firmware that
        // ignores the record (it stops at the NUL).
        assertTrue(b.size <= 51)
    }

    @Test fun deviceRecordComesFirst() {
        val hash = "0123456789abcdef"
        // Sent by another phone: this phone has nothing, the device knows.
        assertEquals(VersionState.CURRENT, PrebuiltTiles.state(DeviceTileList.record("cdn:$hash", null), hash))
        // The device's record wins over a stale one of this phone's.
        assertEquals(VersionState.CURRENT,
            PrebuiltTiles.state(DeviceTileList.record("cdn:$hash", "phone:1700000000"), hash))
        assertEquals(VersionState.UPDATE,
            PrebuiltTiles.state(DeviceTileList.record("cdn:ffffffffffffffff", "cdn:$hash"), hash))
        // No device record (older firmware, saved before it kept them): this phone's.
        assertEquals(VersionState.CURRENT, PrebuiltTiles.state(DeviceTileList.record(null, "cdn:$hash"), hash))
        assertEquals(VersionState.UPDATE, PrebuiltTiles.state(DeviceTileList.record(null, null), hash))
        assertEquals(VersionState.UNKNOWN, PrebuiltTiles.state(DeviceTileList.record("cdn:$hash", null), null))
    }
}
