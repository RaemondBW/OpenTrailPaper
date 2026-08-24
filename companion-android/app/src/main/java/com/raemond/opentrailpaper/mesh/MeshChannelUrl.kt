package com.raemond.opentrailpaper.mesh

import android.graphics.Bitmap
import android.graphics.Color
import com.google.zxing.BarcodeFormat
import com.google.zxing.EncodeHintType
import com.google.zxing.qrcode.QRCodeWriter
import com.google.zxing.qrcode.decoder.ErrorCorrectionLevel
import java.io.ByteArrayOutputStream
import java.security.SecureRandom
import java.util.Base64

// Meshtastic channel sharing: the `https://meshtastic.org/e/#…` URL behind a
// channel QR code.
//
// The payload is a `meshtastic.ChannelSet` protobuf, base64url-encoded without
// padding. Encoding it by hand rather than pulling in a protobuf library keeps
// this the same shape as the firmware's hand-written codec and as
// companion-ios/Sources/MeshChannels.swift — two messages of scalar fields — and
// the bytes are pinned against Meshtastic's own default channel URL in
// tools/mesh_test.
//
// Because it IS the standard format, a channel created here can be joined by
// somebody running the stock Meshtastic app, and theirs can be joined here.
object MeshChannelUrl {

    const val PREFIX = "https://meshtastic.org/e/#"

    // MARK: - Minimal protobuf

    private fun varint(v: Int): ByteArray {
        val out = ByteArrayOutputStream()
        var x = v.toLong() and 0xFFFF_FFFFL
        while (x >= 0x80) {
            out.write(((x and 0x7F) or 0x80).toInt())
            x = x shr 7
        }
        out.write(x.toInt())
        return out.toByteArray()
    }

    private fun lengthDelimited(field: Int, body: ByteArray): ByteArray {
        val out = ByteArrayOutputStream()
        out.write(varint(field shl 3 or 2))
        out.write(varint(body.size))
        out.write(body)
        return out.toByteArray()
    }

    /**
     * meshtastic.ChannelSettings: psk = 2 (bytes), name = 3 (string). Proto3 omits
     * empty fields, which is why an unnamed default channel is three bytes.
     */
    private fun channelSettings(name: String, psk: ByteArray): ByteArray {
        val out = ByteArrayOutputStream()
        if (psk.isNotEmpty()) out.write(lengthDelimited(2, psk))
        if (name.isNotEmpty()) out.write(lengthDelimited(3, name.toByteArray(Charsets.UTF_8)))
        return out.toByteArray()
    }

    /** One channel as it travels in an invite. */
    data class Shared(val name: String, val psk: ByteArray) {
        override fun equals(other: Any?): Boolean =
            other is Shared && name == other.name && psk.contentEquals(other.psk)

        override fun hashCode(): Int = 31 * name.hashCode() + psk.contentHashCode()
    }

    /** meshtastic.ChannelSet: `repeated ChannelSettings settings = 1`. */
    fun encode(channels: List<Shared>): ByteArray {
        val out = ByteArrayOutputStream()
        for (c in channels) out.write(lengthDelimited(1, channelSettings(c.name, c.psk)))
        return out.toByteArray()
    }

    fun decode(data: ByteArray): List<Shared> {
        val out = ArrayList<Shared>()
        val r = Reader(data, 0, data.size)
        while (r.hasMore) {
            val tag = r.varint() ?: return out
            val field = (tag shr 3).toInt()
            val wire = (tag and 7).toInt()
            if (wire != 2) {
                if (!r.skip(wire)) return out
                continue
            }
            val len = r.varint()?.toInt() ?: return out
            val end = r.pos + len
            if (len < 0 || end > r.limit) return out
            val body = Reader(data, r.pos, end)
            r.pos = end
            if (field != 1) continue                      // not a channel; skip it

            // Inner ChannelSettings.
            var name = ""
            var psk = ByteArray(0)
            while (body.hasMore) {
                val t = body.varint() ?: break
                val f = (t shr 3).toInt()
                val w = (t and 7).toInt()
                if (w == 2) {
                    val l = body.varint()?.toInt() ?: break
                    val e = body.pos + l
                    if (l < 0 || e > body.limit) break
                    if (f == 2) psk = data.copyOfRange(body.pos, e)
                    if (f == 3) name = String(data, body.pos, l, Charsets.UTF_8)
                    body.pos = e
                } else if (!body.skip(w)) {
                    break
                }
            }
            out.add(Shared(name, psk))
        }
        return out
    }

    /** A cursor over one protobuf message or a length-delimited slice of it. */
    private class Reader(val d: ByteArray, var pos: Int, val limit: Int) {
        val hasMore: Boolean get() = pos < limit

        fun varint(): Long? {
            var v = 0L
            var shift = 0
            while (pos < limit) {
                val b = d[pos].toInt() and 0xFF
                pos++
                v = v or ((b and 0x7F).toLong() shl shift)
                if (b and 0x80 == 0) return v
                shift += 7
                if (shift > 63) return null
            }
            return null
        }

        /** Steps over a field this decoder does not care about. */
        fun skip(wire: Int): Boolean = when (wire) {
            0 -> varint() != null
            1 -> { pos += 8; pos <= limit }
            5 -> { pos += 4; pos <= limit }
            2 -> {
                val l = varint()?.toInt()
                if (l == null || l < 0) false else { pos += l; pos <= limit }
            }
            else -> false
        }
    }

    // MARK: - URLs

    // java.util.Base64, not android.util's: it exists from API 26 (this app's
    // floor) and also on the plain JVM, which is what keeps this whole codec
    // reachable from a unit test with no device attached.

    /** base64url without padding, which is what Meshtastic uses. */
    private fun b64url(d: ByteArray): String =
        Base64.getUrlEncoder().withoutPadding().encodeToString(d)

    private fun unb64url(s: String): ByteArray? = runCatching {
        // People paste the standard alphabet too, so normalise before decoding.
        Base64.getUrlDecoder().decode(s.replace('+', '-').replace('/', '_').trimEnd('='))
    }.getOrNull()

    fun url(channelName: String, psk: ByteArray): String =
        PREFIX + b64url(encode(listOf(Shared(channelName, psk))))

    /**
     * Parses a scanned string. Accepts the full URL, the `?add=true` variant, and
     * a bare payload — people paste all three.
     */
    fun parse(text: String): List<Shared> {
        var payload = text.trim()
        val hash = payload.indexOf('#')
        if (hash >= 0) payload = payload.substring(hash + 1)
        // "…#PAYLOAD?add=true" — the query lives after the fragment here.
        val q = payload.indexOf('?')
        if (q >= 0) payload = payload.substring(0, q)
        if (payload.isEmpty()) return emptyList()
        val data = unb64url(payload) ?: return emptyList()
        if (data.isEmpty()) return emptyList()
        return decode(data)
    }

    /**
     * A fresh 256-bit key from the system CSPRNG. This is the whole secret behind
     * a private channel, so it must not come from anything weaker.
     */
    fun randomKey(): ByteArray = ByteArray(32).also { SecureRandom().nextBytes(it) }

    /**
     * QR bitmap for a share URL. ZXing, so no dependency on Play Services and no
     * network — the code has to be drawable on a ride with no signal, which is
     * rather the point of the mesh.
     */
    fun qrBitmap(text: String, sizePx: Int = 640): Bitmap? = runCatching {
        val hints = mapOf(
            // M: ~15% recoverable. A phone screen photographed by another phone
            // is a clean channel; higher correction only makes the code denser.
            EncodeHintType.ERROR_CORRECTION to ErrorCorrectionLevel.M,
            EncodeHintType.MARGIN to 1,
            EncodeHintType.CHARACTER_SET to "UTF-8",
        )
        val matrix = QRCodeWriter().encode(text, BarcodeFormat.QR_CODE, sizePx, sizePx, hints)
        val w = matrix.width
        val h = matrix.height
        val px = IntArray(w * h)
        for (y in 0 until h) {
            val row = y * w
            for (x in 0 until w) px[row + x] = if (matrix[x, y]) Color.BLACK else Color.WHITE
        }
        Bitmap.createBitmap(px, w, h, Bitmap.Config.ARGB_8888)
    }.getOrNull()
}
