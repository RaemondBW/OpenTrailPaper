package com.raemond.opentrailpaper

import com.raemond.opentrailpaper.data.*
import org.junit.Assert.*
import org.junit.Test

class RadarLayoutTest {
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
    @Test fun `first vertical wins and radar cannot occupy the map strip`() {
        val layout = DashLayout.parse("speed small vertical\npower medium vertical\nradar medium\n")
        assertEquals(2, layout.items.size)
        assertEquals("speed", layout.verticalItem?.field)
        assertEquals("power", layout.rows[0][0].field)
        val strip = DashConfig.parse("map radar hr clock\nspeed hero\n")
        assertEquals(listOf("speed", "hr", "clock"), strip.mapFields)
        val only = DashLayout.parse("radar medium\n")
        assertEquals("radar", only.verticalItem?.field)
        assertEquals("speed", only.rows[0][0].field)
    }
}
