package com.raemond.opentrailpaper

import com.raemond.opentrailpaper.data.DashConfig
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The page carousel's wire format against src/dash_layout.cpp: `page <kind>`
 * lines, and the rule that an unknown kind is dropped rather than breaking the
 * layout around it (how a newer page reaches older firmware or apps).
 */
class DashPagesTest {

    @Test
    fun `stopwatch page parses and round trips in place`() {
        val cfg = DashConfig.parse("speed hero\npage stopwatch\npage map\n")
        assertEquals(
            listOf(
                DashConfig.PageKind.FIELDS,
                DashConfig.PageKind.STOPWATCH,
                DashConfig.PageKind.MAP,
            ),
            cfg.pages.map { it.kind },
        )
        assertTrue(cfg.hasStopwatchPage)
        assertTrue(cfg.configText.contains("page stopwatch\n"))
        assertEquals(cfg, DashConfig.parse(cfg.configText))
    }

    @Test
    fun `unknown page kind is dropped and the rest kept`() {
        val cfg = DashConfig.parse("speed hero\npage someday\npage\nhr medium\n")
        assertEquals(
            listOf(
                DashConfig.PageKind.FIELDS,
                DashConfig.PageKind.FIELDS,
                DashConfig.PageKind.MAP,
            ),
            cfg.pages.map { it.kind },
        )
        assertFalse(cfg.hasStopwatchPage)
    }

    @Test
    fun `default config has no stopwatch`() {
        assertFalse(DashConfig.deviceDefault.hasStopwatchPage)
    }
}
