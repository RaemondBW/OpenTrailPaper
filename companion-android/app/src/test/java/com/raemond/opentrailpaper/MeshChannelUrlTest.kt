package com.raemond.opentrailpaper

import com.raemond.opentrailpaper.mesh.MeshChannelUrl
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The channel-invite codec, checked against the same pinned values as
 * tools/mesh_test/mesh_test.cpp.
 *
 * Round-tripping this file against itself would only prove it agrees with
 * itself. What matters is that a code made here opens in the stock Meshtastic
 * app and vice versa, so the bytes are pinned against Meshtastic's own published
 * share URL.
 */
class MeshChannelUrlTest {

    @Test
    fun `the default channel encodes to Meshtastic's own published bytes`() {
        // base64url of 0a 03 12 01 01 is "CgMSAQE", and
        // https://meshtastic.org/e/#CgMSAQE is Meshtastic's share URL for an
        // unconfigured default channel — matching these bytes is matching the
        // real thing, not this file's opinion of it.
        val encoded = MeshChannelUrl.encode(
            listOf(MeshChannelUrl.Shared(name = "", psk = byteArrayOf(0x01))),
        )
        assertArrayEquals(byteArrayOf(0x0a, 0x03, 0x12, 0x01, 0x01), encoded)
        assertEquals(
            "https://meshtastic.org/e/#CgMSAQE",
            MeshChannelUrl.url("", byteArrayOf(0x01)),
        )
    }

    @Test
    fun `a private channel round-trips through its own URL`() {
        val key = ByteArray(32) { it.toByte() }
        val url = MeshChannelUrl.url("Trail", key)
        val back = MeshChannelUrl.parse(url)
        assertEquals(1, back.size)
        assertEquals("Trail", back[0].name)
        assertArrayEquals(key, back[0].psk)
    }

    @Test
    fun `the published default URL parses back to the well-known key`() {
        val back = MeshChannelUrl.parse("https://meshtastic.org/e/#CgMSAQE")
        assertEquals(1, back.size)
        assertEquals("", back[0].name)
        assertArrayEquals(byteArrayOf(0x01), back[0].psk)
    }

    @Test
    fun `the add=true variant and a bare payload are both accepted`() {
        // People paste all three forms; the query lives AFTER the fragment in
        // Meshtastic's own links, which is what trips a naive URL parser.
        for (text in listOf(
            "https://meshtastic.org/e/#CgMSAQE?add=true",
            "CgMSAQE",
            "  https://meshtastic.org/e/#CgMSAQE  ",
        )) {
            val back = MeshChannelUrl.parse(text)
            assertEquals("failed on <$text>", 1, back.size)
            assertArrayEquals(byteArrayOf(0x01), back[0].psk)
        }
    }

    @Test
    fun `an unknown field in the middle does not derail the decode`() {
        // Proto3 lets a newer sender add fields. Skipping them is what keeps a
        // channel made by a future Meshtastic release joinable here.
        //   ChannelSettings { varint field 1 = 7, psk = 0x01, name = "Hi" }
        val inner = byteArrayOf(0x08, 0x07, 0x12, 0x01, 0x01, 0x1a, 0x02, 0x48, 0x69)
        val msg = byteArrayOf(0x0a, inner.size.toByte()) + inner
        val back = MeshChannelUrl.decode(msg)
        assertEquals(1, back.size)
        assertEquals("Hi", back[0].name)
        assertArrayEquals(byteArrayOf(0x01), back[0].psk)
    }

    @Test
    fun `junk is refused rather than decoded into a plausible channel`() {
        assertTrue(MeshChannelUrl.parse("").isEmpty())
        assertTrue(MeshChannelUrl.parse("https://example.com/not-a-channel").isEmpty())
        assertTrue(MeshChannelUrl.parse("#####").isEmpty())
    }

    @Test
    fun `a truncated payload stops instead of running off the end`() {
        // Length says 9 bytes of ChannelSettings, only 2 follow.
        assertTrue(MeshChannelUrl.decode(byteArrayOf(0x0a, 0x09, 0x12, 0x01)).isEmpty())
    }

    @Test
    fun `a generated key is 256 bits and not the same twice`() {
        val a = MeshChannelUrl.randomKey()
        val b = MeshChannelUrl.randomKey()
        assertEquals(32, a.size)
        assertEquals(32, b.size)
        assertTrue("two draws from the CSPRNG matched", !a.contentEquals(b))
    }

    @Test
    fun `the radio settings encode to Meshtastic's own default URL`() {
        // https://meshtastic.org/e/#CgMSAQESBggBQANIAQ is what the official app
        // shares for an unconfigured node: the default channel plus
        // lora_config {use_preset, hop_limit 3, tx_enabled}, LongFast and no
        // region being the zero values proto3 leaves out.
        assertEquals(
            "https://meshtastic.org/e/#CgMSAQESBggBQANIAQ",
            MeshChannelUrl.url("", byteArrayOf(0x01), MeshChannelUrl.Radio("LongFast", null, 0)),
        )
        val back = MeshChannelUrl.parseInvite("https://meshtastic.org/e/#CgMSAQESBggBQANIAQ")
        assertEquals(1, back.channels.size)
        assertEquals(MeshChannelUrl.Radio("LongFast", null, 0), back.radio)
    }

    @Test
    fun `speed, region and slot round-trip`() {
        val radio = MeshChannelUrl.Radio("MediumFast", "US", 45)
        val back = MeshChannelUrl.parseInvite(MeshChannelUrl.url("Trail", ByteArray(32), radio))
        assertEquals("Trail", back.channels.single().name)
        assertEquals(radio, back.radio)
    }

    @Test
    fun `an invite without radio settings has none`() {
        assertEquals(null, MeshChannelUrl.parseInvite("https://meshtastic.org/e/#CgMSAQE").radio)
    }

    @Test
    fun `lora fields this app does not read are stepped over`() {
        // lora_config {use_preset 1, frequency_offset (fixed32) 1.0f, modem_preset 4}
        val lora = byteArrayOf(0x08, 0x01, 0x35, 0x00, 0x00, 0x80.toByte(), 0x3f, 0x10, 0x04)
        val msg = byteArrayOf(0x0a, 0x03, 0x12, 0x01, 0x01, 0x12, lora.size.toByte()) + lora
        val back = MeshChannelUrl.decodeInvite(msg)
        assertEquals(1, back.channels.size)
        assertEquals("MediumFast", back.radio?.preset)
    }

    @Test
    fun `the automatic slot matches the firmware's frequencies`() {
        // US, 250 kHz: 104 slots from 902.125 MHz. config.h: LongFast on
        // 906.875 MHz, MediumFast on 913.125 — slots 19 and 44 counted from 0.
        assertEquals(20, MeshChannelUrl.automaticSlot("LongFast", 250.0, 902.0, 928.0, 0.0))
        assertEquals(45, MeshChannelUrl.automaticSlot("MediumFast", 250.0, 902.0, 928.0, 0.0))
    }
}
