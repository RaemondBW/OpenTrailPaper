package com.raemond.opentrailpaper.sim

import java.io.ByteArrayOutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder
import javax.crypto.Cipher
import javax.crypto.spec.IvParameterSpec
import javax.crypto.spec.SecretKeySpec

/**
 * Meshtastic radio frames as a stock node puts them on the air: the subset of
 * sim/client/otpsim/mesh.py (written from Meshtastic's own definitions, not
 * this repo's src/mesh_proto.cpp) the Android mesh tests need. Channel
 * encryption only — no PKI: a virtual node here never advertises a public
 * key, so the device talks to it the legacy, channel-encrypted way.
 *
 * Frame = 16-byte header (dest, sender, id: u32 LE; flags = hop_limit |
 * want_ack << 3 | hop_start << 5; channel hash; next hop; relay) + the
 * protobuf `Data` encrypted with AES-CTR under the channel key, nonce = id
 * (u64 LE) + sender (u32 LE) + 4 zero bytes, counter big-endian (mbedtls and
 * javax.crypto's CTR agree).
 */
object MeshAir {
    const val BROADCAST = -1                   // 0xFFFFFFFF

    // portnums.proto
    const val PORT_TEXT = 1
    const val PORT_POSITION = 3
    const val PORT_NODEINFO = 4
    const val PORT_ROUTING = 5

    // mesh.proto Routing.Error
    const val ROUTING_NONE = 0
    const val ROUTING_NO_RESPONSE = 8

    /** The well-known default key, "AQ==" (psk index 1). */
    val DEFAULT_PSK: ByteArray = hex("d4f1bb3a20290759f0bcffabcf4e6901")

    fun hex(s: String) = ByteArray(s.length / 2) { s.substring(2 * it, 2 * it + 2).toInt(16).toByte() }

    /** Channels.cpp getKey: 1 byte = well-known key N (0 = none), else the bytes (short ones zero-padded). */
    fun expandPsk(psk: ByteArray): ByteArray = when {
        psk.isEmpty() || (psk.size == 1 && psk[0].toInt() == 0) -> ByteArray(0)
        psk.size == 1 -> DEFAULT_PSK.copyOf().also { it[15] = (it[15] + psk[0] - 1).toByte() }
        psk.size < 16 -> psk.copyOf(16)
        else -> psk
    }

    private fun xor(b: ByteArray) = b.fold(0) { h, x -> h xor (x.toInt() and 0xFF) }

    /** Channels::generateHash: xor(name) ^ xor(expanded key). */
    fun channelHash(name: String, psk: ByteArray): Int {
        val key = expandPsk(psk)
        return if (key.isEmpty()) 0 else xor(name.toByteArray()) xor xor(key)
    }

    /** CryptoEngine's AES-CTR (encrypts and decrypts alike). */
    fun ctr(key: ByteArray, sender: Int, id: Int, data: ByteArray): ByteArray {
        if (key.isEmpty()) return data
        val nonce = ByteBuffer.allocate(16).order(ByteOrder.LITTLE_ENDIAN)
            .putLong(id.toLong() and 0xFFFF_FFFFL).putInt(sender).putInt(0).array()
        val c = Cipher.getInstance("AES/CTR/NoPadding")
        c.init(Cipher.ENCRYPT_MODE, SecretKeySpec(key, "AES"), IvParameterSpec(nonce))
        return c.doFinal(data)
    }

    // ---- protobuf wire format --------------------------------------------------------------

    class Pb {
        private val out = ByteArrayOutputStream()
        private fun raw(v: Long) {
            var x = v
            while (x and 0x7FL.inv() != 0L) { out.write(((x and 0x7F) or 0x80).toInt()); x = x ushr 7 }
            out.write(x.toInt())
        }
        fun varint(field: Int, v: Long) = apply { raw((field shl 3).toLong()); raw(v) }
        fun bytes(field: Int, b: ByteArray) = apply { raw((field shl 3 or 2).toLong()); raw(b.size.toLong()); out.write(b) }
        fun string(field: Int, s: String) = bytes(field, s.toByteArray())
        fun fixed32(field: Int, v: Int) = apply {
            raw((field shl 3 or 5).toLong())
            out.write(ByteBuffer.allocate(4).order(ByteOrder.LITTLE_ENDIAN).putInt(v).array())
        }
        fun build(): ByteArray = out.toByteArray()
    }

    /** field -> values: varints and fixed ints as Long, length-delimited as ByteArray. Throws on garbage. */
    fun pbParse(b: ByteArray): Map<Int, List<Any>> {
        val out = HashMap<Int, MutableList<Any>>()
        var i = 0
        fun varint(): Long {
            var v = 0L
            var shift = 0
            while (true) {
                require(i < b.size) { "truncated varint" }
                val c = b[i++].toInt() and 0xFF
                v = v or ((c and 0x7F).toLong() shl shift)
                if (c and 0x80 == 0) return v
                shift += 7
                require(shift < 64) { "varint too long" }
            }
        }
        while (i < b.size) {
            val t = varint()
            val f = (t ushr 3).toInt()
            val v: Any = when ((t and 7).toInt()) {
                0 -> varint()
                2 -> {
                    val n = varint().toInt()
                    require(n >= 0 && i + n <= b.size) { "truncated" }
                    b.copyOfRange(i, i + n).also { i += n }
                }
                5 -> { require(i + 4 <= b.size); (ByteBuffer.wrap(b, i, 4).order(ByteOrder.LITTLE_ENDIAN).int.toLong() and 0xFFFF_FFFFL).also { i += 4 } }
                1 -> { require(i + 8 <= b.size); ByteBuffer.wrap(b, i, 8).order(ByteOrder.LITTLE_ENDIAN).long.also { i += 8 } }
                else -> throw IllegalArgumentException("wire type ${t and 7}")
            }
            out.getOrPut(f) { mutableListOf() }.add(v)
        }
        return out
    }

    /** mesh.proto Data: portnum, payload, want_response, request_id. */
    fun data(port: Int, payload: ByteArray, wantResponse: Boolean = false, requestId: Int = 0): ByteArray =
        Pb().varint(1, port.toLong()).apply { if (payload.isNotEmpty()) bytes(2, payload) }
            .apply { if (wantResponse) varint(3, 1) }
            .apply { if (requestId != 0) fixed32(6, requestId) }
            .build()

    /** mesh.proto User, without a public key (field 8): the device then sends us legacy channel-encrypted DMs. */
    fun user(num: Int, longName: String, shortName: String): ByteArray =
        Pb().string(1, "!%08x".format(num)).string(2, longName).string(3, shortName).build()

    /** mesh.proto Position: latitude_i / longitude_i are sfixed32 1e-7 degrees. */
    fun position(lat: Double, lon: Double, altM: Int? = null, sats: Int = 0, precisionBits: Int? = null): ByteArray =
        Pb().fixed32(1, Math.round(lat * 1e7).toInt()).fixed32(2, Math.round(lon * 1e7).toInt())
            .apply { if (altM != null) varint(3, altM.toLong()) }
            .apply { if (sats != 0) varint(19, sats.toLong()) }
            .apply { if (precisionBits != null) varint(23, precisionBits.toLong()) }
            .build()

    /** The 16-byte radio header. hop_start defaults to hop_limit (heard directly). */
    fun header(dest: Int, sender: Int, id: Int, hopLimit: Int = 3, hopStart: Int = hopLimit, wantAck: Boolean = false, channelHash: Int): ByteArray {
        val flags = (hopLimit and 7) or (if (wantAck) 0x08 else 0) or ((hopStart and 7) shl 5)
        return ByteBuffer.allocate(16).order(ByteOrder.LITTLE_ENDIAN)
            .putInt(dest).putInt(sender).putInt(id)
            .put(flags.toByte()).put(channelHash.toByte()).put(0).put(0).array()
    }

    /** A channel as a stock node holds it: name + key as shared. */
    class Channel(val name: String = "LongFast", val psk: ByteArray = byteArrayOf(1)) {
        val key: ByteArray = expandPsk(psk)
        val hash: Int = channelHash(name, psk)
    }

    /** A received frame: its header, and its Data when one of our channels decrypts it. */
    class Decoded(
        val dest: Int, val sender: Int, val id: Int, val hopLimit: Int, val hopStart: Int, val wantAck: Boolean,
        val channelHash: Int, val channel: Channel?,
        val port: Int, val payload: ByteArray, val wantResponse: Boolean, val requestId: Int,
    ) {
        val text: String get() = String(payload, Charsets.UTF_8)
        override fun toString() =
            "frame !%08x -> !%08x id %08x hash 0x%02x port %d%s".format(sender, dest, id, channelHash, port, if (port == PORT_TEXT) " '$text'" else "")
    }

    /** Header + decrypted Data, trying each of [channels] whose hash matches; port 0 when none does. */
    fun decode(frame: ByteArray, channels: List<Channel>): Decoded {
        val h = ByteBuffer.wrap(frame, 0, 16).order(ByteOrder.LITTLE_ENDIAN)
        val dest = h.int
        val sender = h.int
        val id = h.int
        val flags = h.get().toInt() and 0xFF
        val hash = h.get().toInt() and 0xFF
        val body = frame.copyOfRange(16, frame.size)
        for (ch in channels) {
            if (ch.hash != hash) continue
            val f = runCatching { pbParse(ctr(ch.key, sender, id, body)) }.getOrNull() ?: continue
            val port = (f[1]?.firstOrNull() as? Long)?.toInt() ?: continue
            if (port == 0) continue
            return Decoded(
                dest, sender, id, flags and 7, flags ushr 5, flags and 8 != 0, hash, ch, port,
                (f[2]?.firstOrNull() as? ByteArray) ?: ByteArray(0), (f[3]?.firstOrNull() as? Long) == 1L,
                (f[6]?.firstOrNull() as? Long)?.toInt() ?: 0,
            )
        }
        return Decoded(dest, sender, id, flags and 7, flags ushr 5, flags and 8 != 0, hash, null, 0, ByteArray(0), false, 0)
    }

    /** A stock Meshtastic node, as bytes on the air. */
    class VirtualNode(val num: Int, val longName: String, val shortName: String, val channel: Channel = Channel()) {
        private var nextId = (num * 2654435761L).toInt() or 1

        fun newId(): Int { nextId = nextId * 1103515245 + 12345; return if (nextId == 0) 1 else nextId }

        /**
         * A whole frame: header + [data] encrypted with [on]'s key. [hopStart]
         * above [hopLimit] says relays carried it (hops = start - limit); a
         * [hopStart] of 0 is a pre-2.3 sender that does not say.
         */
        fun packet(
            data: ByteArray, dest: Int = BROADCAST, on: Channel = channel, id: Int = newId(),
            hopLimit: Int = 3, hopStart: Int = hopLimit, wantAck: Boolean = false,
        ): ByteArray = header(dest, num, id, hopLimit, hopStart, wantAck, on.hash) + ctr(on.key, num, id, data)

        fun text(text: String, dest: Int = BROADCAST, on: Channel = channel, hopLimit: Int = 3, hopStart: Int = hopLimit) =
            packet(data(PORT_TEXT, text.toByteArray()), dest, on, hopLimit = hopLimit, hopStart = hopStart)

        fun nodeinfo(hopLimit: Int = 3, hopStart: Int = hopLimit) =
            packet(data(PORT_NODEINFO, user(num, longName, shortName)), hopLimit = hopLimit, hopStart = hopStart)

        fun position(lat: Double, lon: Double, altM: Int? = null, sats: Int = 0, precisionBits: Int? = null, hopLimit: Int = 3, hopStart: Int = hopLimit): ByteArray =
            packet(data(PORT_POSITION, MeshAir.position(lat, lon, altM, sats, precisionBits)), hopLimit = hopLimit, hopStart = hopStart)

        /** A ROUTING answer to packet [requestId]: an ACK, or a NAK with [error]. */
        fun ack(dest: Int, requestId: Int, error: Int = ROUTING_NONE) =
            packet(data(PORT_ROUTING, Pb().varint(3, error.toLong()).build(), requestId = requestId), dest)
    }
}
