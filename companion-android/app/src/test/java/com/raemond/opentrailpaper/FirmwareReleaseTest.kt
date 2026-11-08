package com.raemond.opentrailpaper

import com.raemond.opentrailpaper.data.FirmwareRelease
import org.junit.Assert.assertEquals
import org.junit.Test

/**
 * The version the app compares with the device's. A version release is tagged
 * with it; the nightly's tag never moves, so CI titles it with the version
 * instead (.github/workflows/build.yml, "Publish nightly").
 */
class FirmwareReleaseTest {
    @Test fun aVersionReleaseIsItsTag() {
        assertEquals("v1.23", FirmwareRelease.version("v1.23", "Firmware v1.23"))
    }

    @Test fun theNightlyIsItsTitlesVersion() {
        assertEquals(
            "v1.23-nightly.81dbf91",
            FirmwareRelease.version("nightly", "Firmware v1.23-nightly.81dbf91"),
        )
    }

    @Test fun aNightlyTitledAnyOtherWayIsTheWholeTitle() {
        assertEquals("v1.24-nightly.abc1234", FirmwareRelease.version("nightly", "v1.24-nightly.abc1234"))
    }

    @Test fun switchingChannelDropsTheOtherChannelsRelease() {
        val start = FirmwareRelease.channel
        val other = if (start == FirmwareRelease.Channel.STABLE) FirmwareRelease.Channel.NIGHTLY
                    else FirmwareRelease.Channel.STABLE
        try {
            FirmwareRelease.setChannel(other)
            assertEquals(other, FirmwareRelease.channel)
            assertEquals(null, FirmwareRelease.latest)
        } finally {
            FirmwareRelease.setChannel(start)
        }
    }
}
