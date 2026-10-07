package com.raemond.opentrailpaper

import com.raemond.opentrailpaper.ble.ArtDither
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class ArtDitherTest {

    // The firmware's unpack (media.cpp commitArt, tone branch), for round-trips.
    private fun unpack(packed: ByteArray, n: Int) = ByteArray(n) { i ->
        val b = packed[i / 3].toInt() and 0xFF
        when (i % 3) {
            0 -> b % 5
            1 -> (b / 5) % 5
            else -> (b / 25) % 5
        }.toByte()
    }

    @Test fun packRoundTripsEveryToneInEverySlot() {
        val tones = ByteArray(301) { (it * 7 % 5).toByte() }
        val packed = ArtDither.pack(tones)
        assertEquals(101, packed.size)
        assertTrue(packed.all { (it.toInt() and 0xFF) <= 124 })
        assertArrayEquals(tones, unpack(packed, tones.size))
    }

    @Test fun fullSizeArtIsAThirdOfTheGrayscale() {
        val gray = ByteArray(300 * 300) { (it % 256).toByte() }
        assertEquals(30_000, ArtDither.pack(ArtDither.dither(gray, 300, 300)).size)
    }

    @Test fun flatTonesStayFlat() {
        for ((lum, tone) in listOf(0 to 0, 70 to 1, 120 to 2, 170 to 3, 255 to 4)) {
            val out = ArtDither.dither(ByteArray(16 * 16) { lum.toByte() }, 16, 16)
            assertTrue("lum $lum", out.all { it.toInt() == tone })
        }
    }

    @Test fun midGreyDithersToAMatchingAverage() {
        // 200 sits between the 170 and 255 tones; the error diffusion should
        // mix them so the mean reflectance lands near 200.
        val lum = intArrayOf(0, 70, 120, 170, 255)
        val out = ArtDither.dither(ByteArray(64 * 64) { 200.toByte() }, 64, 64)
        val mean = out.sumOf { lum[it.toInt()] } / out.size.toDouble()
        assertEquals(200.0, mean, 3.0)
    }
}
