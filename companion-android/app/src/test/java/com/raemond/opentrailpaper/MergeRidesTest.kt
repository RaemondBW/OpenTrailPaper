package com.raemond.opentrailpaper

import com.raemond.opentrailpaper.ble.RideFile
import com.raemond.opentrailpaper.ui.mergeRides
import org.junit.Assert.assertEquals
import org.junit.Test

class MergeRidesTest {
    @Test fun cachedOnlyBeforeDeviceAnswers() {
        val cached = listOf(RideFile("20260718-163000.fit", 10), RideFile("20260716-090000.fit", 20))
        assertEquals(cached, mergeRides(emptyList(), cached))
    }

    @Test fun deviceRidesMergeInNewestFirstWithoutDuplicates() {
        val cached = listOf(RideFile("20260718-163000.fit", 10), RideFile("20260716-090000.fit", 20))
        val device = listOf(RideFile("20260719-071500.fit", 30), RideFile("20260718-163000.fit", 11))
        assertEquals(
            listOf(
                RideFile("20260719-071500.fit", 30),
                RideFile("20260718-163000.fit", 11),   // device entry wins
                RideFile("20260716-090000.fit", 20),
            ),
            mergeRides(device, cached),
        )
    }
}
