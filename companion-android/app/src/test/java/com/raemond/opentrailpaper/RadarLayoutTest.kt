package com.raemond.opentrailpaper

import com.raemond.opentrailpaper.data.*
import org.junit.Assert.*
import org.junit.Test

class RadarLayoutTest {
    @Test fun `bottom radar spans the final rows and position survives sync`() {
        val layout = DashLayout.parse("radar medium half vertical height=50 position=bottom")
        assertEquals(true, layout.verticalItem?.bottomAligned)
        assertEquals(364, layout.verticalHeight(listOf(484, 181, 171), 12))
        assertEquals(387, layout.verticalHeight(listOf(257, 192, 192, 183), 12))
        assertEquals(323, layout.verticalHeight(listOf(214, 161, 161, 150), 12))
        assertEquals(layout, DashLayout.parse(layout.configText))
        val config = DashConfig.parse(layout.configText)
        assertEquals(config, DashConfig.parse(config.configText))
        assertEquals(false, DashLayout.parse("radar medium position=top").verticalItem?.bottomAligned)
        assertEquals(false, DashLayout.parse("power hero position=bottom").items[0].bottomAligned)
    }

    @Test fun `vertical tiles fill whole weighted rows including navigation and gutter boundaries`() {
        val half = DashLayout.parse("radar medium vertical height=50")
        val threeQuarter = DashLayout.parse("radar medium vertical height=75")
        val full = DashLayout.parse("radar medium vertical")
        val grid = listOf(257, 192, 192, 183)
        val navGrid = listOf(214, 161, 161, 150)
        assertEquals(461, half.verticalHeight(grid, 12))
        assertEquals(665, threeQuarter.verticalHeight(grid, 12))
        assertEquals(860, full.verticalHeight(grid, 12))
        assertEquals(387, half.verticalHeight(navGrid, 12))
        assertEquals(560, threeQuarter.verticalHeight(navGrid, 12))
        assertEquals(722, full.verticalHeight(navGrid, 12))
        assertEquals(424, half.verticalHeight(listOf(424, 424), 12))
        assertEquals(860, half.verticalHeight(listOf(860), 12))
        assertEquals(0, half.verticalHeight(emptyList(), 12))
    }

    @Test fun `tile heights survive sync and legacy layouts remain full height`() {
        for (height in listOf(50, 75, 100)) {
            val text = "speed large\nradar medium half vertical height=$height\n"
            val layout = DashLayout.parse(text)
            assertEquals(height, layout.verticalItem?.heightPercent)
            assertEquals(false, layout.verticalItem?.half)
            assertEquals(layout, DashLayout.parse(layout.configText))
            val config = DashConfig.parse(text)
            assertEquals(height, config.pages[0].layout.verticalItem?.heightPercent)
            assertEquals(config, DashConfig.parse(config.configText))
        }
        assertEquals(100, DashLayout.parse("radar medium").verticalItem?.heightPercent)
        assertEquals(100, DashLayout.parse("radar medium vertical height=12").verticalItem?.heightPercent)
        val normalized = DashLayout.parse("speed small height=50\nhr medium vertical height=75\npower medium vertical height=50")
        assertEquals(listOf(100, 100, 100), normalized.items.map { it.heightPercent })
    }

    @Test fun `radar shares the page with existing numeric rows and round trips`() {
        val config = DashConfig.parse("speed large\nhr medium half\ncadence medium half\nradar medium vertical\npage map\n")
        assertEquals(2, config.pages.size)
        val layout = config.pages[0].layout
        assertEquals(4, layout.items.size)
        assertEquals("radar", layout.verticalItem?.field)
        assertEquals(2, layout.rows.size)
        assertEquals(2, layout.rows[1].size)
        assertEquals(config, DashConfig.parse(config.configText))
    }
    @Test fun `numeric tiles migrate to rows and radar alone takes the side column`() {
        val layout = DashLayout.parse("speed small vertical\npower medium vertical\nradar medium\n")
        assertEquals(3, layout.items.size)
        assertEquals("radar", layout.verticalItem?.field)
        assertEquals("speed", layout.rows[0][0].field)
        assertEquals("power", layout.rows[1][0].field)
        val duplicates = DashLayout.parse("radar medium height=50\nradar medium height=75")
        assertEquals(1, duplicates.items.size)
        assertEquals(50, duplicates.verticalItem?.heightPercent)
        val strip = DashConfig.parse("map radar hr clock\nspeed hero\n")
        assertEquals(listOf("speed", "hr", "clock"), strip.mapFields)
        val only = DashLayout.parse("radar medium\n")
        assertEquals("radar", only.verticalItem?.field)
        assertEquals("speed", only.rows[0][0].field)
    }
}
