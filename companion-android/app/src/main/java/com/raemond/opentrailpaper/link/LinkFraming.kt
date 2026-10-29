package com.raemond.opentrailpaper.link

import java.io.ByteArrayOutputStream

// Framing for the phone link (docs/ble-link.md, "Framing"). One protobuf
// message is split into fragments, one ATT write or notification each:
//
//   byte 0   header: b7 FIRST, b6 LAST, b5-b4 reserved (0), b3-b0 counter
//   byte 1.. up to maxWrite - 1 bytes of the encoded message
//
// The counter runs 0-15 per direction across ALL fragments and wraps. It is
// there to catch bugs, not to retransmit: a receiver that sees a gap drops the
// partial message and starts clean at the next FIRST.
//
// Pure Kotlin (no android.*) so the JVM tests drive it directly.

object LinkFrame {
    const val FIRST = 0x80
    const val LAST = 0x40
    const val COUNTER_MASK = 0x0F

    fun isFirst(header: Int) = header and FIRST != 0
    fun isLast(header: Int) = header and LAST != 0
    fun counter(header: Int) = header and COUNTER_MASK
}

/**
 * Splits outgoing messages into fragments and owns this side's fragment
 * counter. One per connection; [reset] when the link starts over.
 */
class LinkFragmenter {
    private var counter = 0

    fun reset() { counter = 0 }

    /**
     * [msg] as fragments of at most [maxWrite] bytes (header included). An
     * empty message is still one fragment: a header with FIRST|LAST.
     */
    fun fragment(msg: ByteArray, maxWrite: Int): List<ByteArray> {
        val room = maxOf(1, maxWrite - 1)
        val out = ArrayList<ByteArray>(msg.size / room + 1)
        var off = 0
        do {
            val n = minOf(room, msg.size - off)
            var header = counter and LinkFrame.COUNTER_MASK
            if (off == 0) header = header or LinkFrame.FIRST
            if (off + n >= msg.size) header = header or LinkFrame.LAST
            val frag = ByteArray(n + 1)
            frag[0] = header.toByte()
            System.arraycopy(msg, off, frag, 1, n)
            out += frag
            counter = (counter + 1) and LinkFrame.COUNTER_MASK
            off += n
        } while (off < msg.size)
        return out
    }
}

/**
 * Rebuilds incoming messages from fragments. [feed] returns a whole message
 * when a fragment with LAST completes one, else null. A counter gap, a FIRST
 * in the middle of a message or a continuation without a FIRST drops the
 * partial message (reported through [onDrop]); the next FIRST starts clean.
 */
class LinkReassembler(
    private val maxMessage: Int = 64 * 1024,
    private val onDrop: (String) -> Unit = {},
) {
    private var expected: Int? = null        // next counter, null before the first fragment
    private var partial: ByteArrayOutputStream? = null
    /** The rest of a message already reported as dropped: skip it quietly. */
    private var discarding = false

    fun reset() {
        expected = null
        partial = null
        discarding = false
    }

    fun feed(fragment: ByteArray): ByteArray? {
        if (fragment.isEmpty()) {
            onDrop("empty fragment")
            return null
        }
        val header = fragment[0].toInt() and 0xFF
        val count = LinkFrame.counter(header)
        val gap = expected != null && count != expected
        expected = (count + 1) and LinkFrame.COUNTER_MASK

        if (gap && partial != null) {
            onDrop("counter gap mid-message")
            partial = null
            discarding = true
        }
        if (LinkFrame.isFirst(header)) {
            if (partial != null) onDrop("FIRST inside a message")
            discarding = false
            partial = ByteArrayOutputStream(maxOf(32, fragment.size * 2))
        } else if (partial == null) {
            if (!discarding) {
                onDrop(if (gap) "continuation after a counter gap" else "continuation without FIRST")
                discarding = true
            }
            if (LinkFrame.isLast(header)) discarding = false
            return null
        }
        val buf = partial!!
        if (buf.size() + fragment.size - 1 > maxMessage) {
            onDrop("message over $maxMessage bytes")
            partial = null
            discarding = !LinkFrame.isLast(header)
            return null
        }
        buf.write(fragment, 1, fragment.size - 1)
        if (!LinkFrame.isLast(header)) return null
        partial = null
        return buf.toByteArray()
    }
}

/** UTF-8 helpers for the link's string fields (nanopb max_size limits). */
object LinkText {
    /** The longest prefix of [s] that encodes to at most [maxBytes] bytes,
     *  cut on a code-point boundary so the device never gets half a glyph. */
    fun cut(s: String, maxBytes: Int): String {
        if (maxBytes <= 0) return ""
        val bytes = s.toByteArray(Charsets.UTF_8)
        if (bytes.size <= maxBytes) return s
        var end = 0
        var used = 0
        while (end < s.length) {
            val cp = s.codePointAt(end)
            val w = when {
                cp < 0x80 -> 1
                cp < 0x800 -> 2
                cp < 0x10000 -> 3
                else -> 4
            }
            if (used + w > maxBytes) break
            used += w
            end += Character.charCount(cp)
        }
        return s.substring(0, end)
    }
}
