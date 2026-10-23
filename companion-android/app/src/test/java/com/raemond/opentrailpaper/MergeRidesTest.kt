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

    // The device's saved (last-known) list shows straight away, before any
    // reconnect, rides downloaded or not, merged with the phone's cache.
    @Test fun persistedListingShowsWhileDisconnected() {
        val saved = listOf(RideFile("20260719-071500.fit", 30), RideFile("20260718-163000.fit", 11))
        val cached = listOf(RideFile("20260718-163000.fit", 10))
        assertEquals(
            listOf(RideFile("20260719-071500.fit", 30), RideFile("20260718-163000.fit", 11)),
            mergeRides(saved, cached),
        )
    }

    // A completed check adds new rides at the top and drops rides the device
    // no longer has — unless they're cached on the phone.
    @Test fun newListingAddsNewAndDropsGoneUnlessCached() {
        val saved = listOf(
            RideFile("20260719-071500.fit", 30),   // deleted on the device, not cached
            RideFile("20260718-163000.fit", 11),   // deleted on the device, cached
            RideFile("20260716-090000.fit", 20),   // still there
        )
        val cached = listOf(RideFile("20260718-163000.fit", 10))
        val fresh = listOf(RideFile("20260720-080000.fit", 40), RideFile("20260716-090000.fit", 20))
        val before = mergeRides(saved, cached)
        val after = mergeRides(fresh, cached)
        assertEquals(
            listOf(
                RideFile("20260720-080000.fit", 40),
                RideFile("20260718-163000.fit", 10),
                RideFile("20260716-090000.fit", 20),
            ),
            after,
        )
        // Rows that survive keep their relative order (no reshuffle).
        val kept = after.map { it.name }.filter { n -> before.any { it.name == n } }
        assertEquals(before.map { it.name }.filter { n -> kept.contains(n) }, kept)
    }

    // The device refused to list (mid-ride) or nothing is saved yet: the cache
    // alone still shows; an empty listing never hides cached rides.
    @Test fun emptyListingNeverHidesCachedRides() {
        val cached = listOf(RideFile("20260718-163000.fit", 10))
        assertEquals(cached, mergeRides(emptyList(), cached))
        assertEquals(emptyList<RideFile>(), mergeRides(emptyList(), emptyList()))
    }

    // Same inputs, same list — an unchanged re-check doesn't reorder anything.
    @Test fun mergeIsStable() {
        val saved = listOf(RideFile("20260716-090000.fit", 20), RideFile("20260719-071500.fit", 30))
        val cached = listOf(RideFile("20260718-163000.fit", 10))
        assertEquals(mergeRides(saved, cached), mergeRides(saved.reversed(), cached))
    }
}
