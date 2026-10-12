package com.raemond.opentrailpaper

import com.raemond.opentrailpaper.ble.WheelSize
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class WheelSizeTest {
    @Test fun circumferenceFromRimAndTyre() {
        // Within ~0.5% of the measured head-unit table.
        assertEquals(2111, WheelSize.circumference(622, 25))   // table 2105
        assertEquals(2130, WheelSize.circumference(622, 28))   // table 2136
        assertEquals(2306, WheelSize.circumference(622, 56))   // 29 x 2.2, table 2298
        assertEquals(2089, WheelSize.circumference(559, 53))   // 26 x 2.1, table 2068
    }

    @Test fun decodeRoundTrips() {
        for (r in WheelSize.rims) for (t in WheelSize.MIN_TYRE_MM..WheelSize.MAX_TYRE_MM) {
            val mm = WheelSize.circumference(r.bsd, t)
            assertEquals(r.bsd to t, WheelSize.decode(mm, r.bsd))
        }
    }

    @Test fun sharedSizePrefersTheChosenRim() {
        val mm = WheelSize.circumference(584, 47)
        assertEquals(mm, WheelSize.circumference(622, 28))
        assertEquals(584 to 47, WheelSize.decode(mm, 584))
        assertEquals(622 to 28, WheelSize.decode(mm, 622))
    }

    @Test fun firmwareDefaultReadsAs700c() {
        assertEquals(622 to 24, WheelSize.decode(2105, 622))
    }

    @Test fun serialSizeIsCustom() {
        assertNull(WheelSize.decode(2106, 622))
        assertEquals("56 mm · 2.2\"", WheelSize.tyreLabel(56))
        assertEquals("28 mm", WheelSize.tyreLabel(28))
    }
}
