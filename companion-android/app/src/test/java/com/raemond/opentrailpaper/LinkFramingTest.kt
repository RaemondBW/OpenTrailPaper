package com.raemond.opentrailpaper

import com.raemond.opentrailpaper.link.LinkFrame
import com.raemond.opentrailpaper.link.LinkFragmenter
import com.raemond.opentrailpaper.link.LinkReassembler
import com.raemond.opentrailpaper.link.LinkText
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/** The link's 1-byte fragment header (docs/ble-link.md, "Framing"). */
class LinkFramingTest {

    private fun bytes(n: Int, seed: Int = 0) = ByteArray(n) { ((it * 31 + seed) and 0xFF).toByte() }

    @Test fun singleFragmentHasFirstAndLast() {
        val f = LinkFragmenter()
        val frags = f.fragment(bytes(10), maxWrite = 20)
        assertEquals(1, frags.size)
        assertEquals(0xC0, frags[0][0].toInt() and 0xFF)
        assertEquals(11, frags[0].size)
        val r = LinkReassembler()
        assertArrayEquals(bytes(10), r.feed(frags[0]))
    }

    @Test fun emptyMessageIsOneFragment() {
        val frags = LinkFragmenter().fragment(ByteArray(0), maxWrite = 20)
        assertEquals(1, frags.size)
        assertEquals(1, frags[0].size)
        assertArrayEquals(ByteArray(0), LinkReassembler().feed(frags[0]))
    }

    @Test fun multiFragmentRoundTrip() {
        val msg = bytes(1000, 7)
        val frags = LinkFragmenter().fragment(msg, maxWrite = 20)
        assertEquals(53, frags.size)                     // 19 payload bytes each
        frags.forEachIndexed { i, fr ->
            val h = fr[0].toInt() and 0xFF
            assertEquals(i == 0, LinkFrame.isFirst(h))
            assertEquals(i == frags.lastIndex, LinkFrame.isLast(h))
            assertEquals(i and 0x0F, LinkFrame.counter(h))
            assertEquals(0, h and 0x30)                  // reserved bits
            assertTrue(fr.size <= 20)
        }
        val r = LinkReassembler()
        for (fr in frags.dropLast(1)) assertNull(r.feed(fr))
        assertArrayEquals(msg, r.feed(frags.last()))
    }

    @Test fun counterRunsAcrossMessagesAndWraps() {
        val f = LinkFragmenter()
        val r = LinkReassembler(onDrop = { throw AssertionError("dropped: $it") })
        repeat(40) { n ->
            val msg = bytes(5 + n * 3, n)
            val frags = f.fragment(msg, maxWrite = 12)
            var out: ByteArray? = null
            for (fr in frags) out = r.feed(fr)
            assertArrayEquals(msg, out)
        }
    }

    @Test fun counterGapDropsThePartialMessageOnce() {
        val f = LinkFragmenter()
        val drops = mutableListOf<String>()
        val r = LinkReassembler(onDrop = { drops += it })
        val first = f.fragment(bytes(80, 1), maxWrite = 20)   // 5 fragments
        assertNull(r.feed(first[0]))
        assertNull(r.feed(first[1]))
        // first[2] lost on the way
        assertNull(r.feed(first[3]))
        assertNull(r.feed(first[4]))
        assertEquals(1, drops.size)
        // The next message starts clean at its FIRST.
        val next = bytes(30, 2)
        var out: ByteArray? = null
        for (fr in f.fragment(next, maxWrite = 20)) out = r.feed(fr)
        assertArrayEquals(next, out)
        assertEquals(1, drops.size)
    }

    @Test fun continuationWithoutFirstIsDropped() {
        val f = LinkFragmenter()
        val frags = f.fragment(bytes(50), maxWrite = 20)
        val drops = mutableListOf<String>()
        val r = LinkReassembler(onDrop = { drops += it })
        assertNull(r.feed(frags[1]))
        assertNull(r.feed(frags[2]))
        assertEquals(1, drops.size)
        val next = bytes(8, 3)
        assertArrayEquals(next, r.feed(f.fragment(next, 20).single()))
    }

    @Test fun firstInsideAMessageStartsOver() {
        val f = LinkFragmenter()
        val a = f.fragment(bytes(50, 1), maxWrite = 20)
        val b = f.fragment(bytes(10, 2), maxWrite = 20)
        val drops = mutableListOf<String>()
        val r = LinkReassembler(onDrop = { drops += it })
        assertNull(r.feed(a[0]))
        // a's tail is lost and b's FIRST follows: a counter gap AND a new FIRST.
        assertArrayEquals(bytes(10, 2), r.feed(b[0]))
        assertEquals(1, drops.size)
    }

    @Test fun utf8CutNeverSplitsACharacter() {
        assertEquals("abc", LinkText.cut("abc", 10))
        assertEquals("ab", LinkText.cut("abc", 2))
        // "é" is two bytes, "😀" four.
        assertEquals("a", LinkText.cut("aé", 2))
        assertEquals("aé", LinkText.cut("aé", 3))
        assertEquals("x", LinkText.cut("x😀", 4))
        assertEquals("x😀", LinkText.cut("x😀", 5))
        assertTrue(LinkText.cut("ÅÅÅÅÅÅÅÅÅÅ", 7).toByteArray().size <= 7)
    }
}
