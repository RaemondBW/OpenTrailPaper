package com.raemond.opentrailpaper

import com.raemond.opentrailpaper.ble.BikeSensor
import com.raemond.opentrailpaper.ble.WheelSize
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/** Sensor kind bits from the head unit, and the speed-sensor wheel-size table. */
class SensorKindsTest {
    private fun kinds(mask: Int) =
        BikeSensor("aa:bb:cc:dd:ee:ff", "x", mask, connected = false, paired = false, rssi = 0).kindsText

    @Test
    fun speedAndCadenceBits() {
        assertEquals("Cadence", kinds(4))
        assertEquals("Speed", kinds(16))
        // Unidentified 0x1816 device, or a combo: both bits.
        assertEquals("Speed & cadence", kinds(4 or 16))
        assertEquals("Power + Speed", kinds(2 or 16))
        assertEquals("Sensor", kinds(0))
    }

    @Test
    fun wheelPresetsWithinFirmwareLimits() {
        assertTrue(WheelSize.presets.all { it.mm in WheelSize.MIN_MM..WheelSize.MAX_MM })
        assertEquals("700x25c", WheelSize.presetName(WheelSize.DEFAULT_MM))
        assertEquals(WheelSize.MIN_MM, WheelSize.clamp(10))
        assertEquals(WheelSize.MAX_MM, WheelSize.clamp(9000))
    }
}
