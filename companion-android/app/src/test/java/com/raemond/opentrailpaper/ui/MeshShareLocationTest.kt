package com.raemond.opentrailpaper.ui

import com.raemond.opentrailpaper.ble.MeshChannel
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/** Where the chat's "share my location" row appears and what it says. */
class MeshShareLocationTest {
    private val primary = MeshChannel(0, "MediumFast", 0x1f, byteArrayOf(1), false)
    private val ride = MeshChannel(1, "Saturday Ride", 0x8c, ByteArray(32) { it.toByte() }, false)
    private val weak = MeshChannel(2, "Default", 0x08, byteArrayOf(1), false)

    @Test fun shownOnlyForAPrivateChannelsWholeThread() {
        assertTrue(showsShareLocationInThread(ride, direct = false))
        assertFalse(showsShareLocationInThread(ride, direct = true))
        assertFalse(showsShareLocationInThread(primary, direct = false))
        assertFalse(showsShareLocationInThread(weak, direct = false))
        assertFalse(showsShareLocationInThread(null, direct = false))
    }

    @Test fun captionFollowsTheLiveSwitchState() {
        assertEquals(SHARE_LOCATION_KEY_HOLDERS, shareLocationCaption(ride))
        assertEquals(SHARE_LOCATION_CADENCE, shareLocationCaption(ride.copy(sharesLocation = true)))
        assertEquals(SHARE_LOCATION_PUBLIC, shareLocationCaption(primary.copy(sharesLocation = true)))
    }
}
