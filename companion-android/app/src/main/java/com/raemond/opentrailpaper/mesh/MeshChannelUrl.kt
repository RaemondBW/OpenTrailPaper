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

    /**
     * The radio half of an invite: meshtastic.Config.LoRaConfig, which the
     * ChannelSet carries as `lora_config = 2`. A channel's name and key say
     * nothing about the modem or the frequency, and two nodes that differ in
     * either hear nothing of each other, so an invite without these only works
     * between nodes that happen to be set up alike already.
     *
     * [preset] and [region] are Meshtastic's enum NAMES ("MediumFast", "US"), the
     * same names the firmware streams its tables under; null when the code does
     * not say or uses a value this app does not know. [slot] is channel_num:
     * 1-based, 0 = the hash of the primary channel's name.
     */
    data class Radio(val preset: String?, val region: String?, val slot: Int)

    // meshtastic.Config.LoRaConfig.ModemPreset, by enum value.
    private val PRESETS = listOf(
        "LongFast", "LongSlow", "VeryLongSlow", "MediumSlow", "MediumFast",
        "ShortSlow", "ShortFast", "LongModerate", "ShortTurbo", "LongTurbo",
    )

    // meshtastic.Config.LoRaConfig.RegionCode, by enum value (0 = UNSET).
    private val REGIONS = listOf(
        null, "US", "EU_433", "EU_868", "CN", "JP", "ANZ", "KR", "TW", "RU", "IN",
        "NZ_865", "TH", "LORA_24", "UA_433", "UA_868", "MY_433", "MY_919", "SG_923",
        "PH_433", "PH_868", "PH_915", "ANZ_433", "KZ_433", "KZ_863", "NP_865", "BR_902",
    )

    private fun varintField(field: Int, v: Int): ByteArray = varint(field shl 3) + varint(v)

    /**
     * Always use_preset, tx_enabled and a hop limit of 3 (Meshtastic's default):
     * the official app applies an invite's LoRa config wholesale, and a zero in
     * either of the last two would leave the other person unable to send or relay.
     */
    private fun loraConfig(r: Radio): ByteArray {
        val out = ByteArrayOutputStream()
        out.write(varintField(1, 1))
        val p = PRESETS.indexOf(r.preset)
        if (p > 0) out.write(varintField(2, p))
        val g = REGIONS.indexOf(r.region)
        if (g > 0) out.write(varintField(7, g))
        out.write(varintField(8, 3))
        out.write(varintField(9, 1))
        if (r.slot > 0) out.write(varintField(11, r.slot))
        return out.toByteArray()
    }

    /** An invite as scanned: its channels and, if it carries one, its radio. */
    class Invite(val channels: List<Shared>, val radio: Radio?)

    /**
     * meshtastic.ChannelSet: `repeated ChannelSettings settings = 1`,
     * `LoRaConfig lora_config = 2`.
     */
    fun encode(channels: List<Shared>, radio: Radio? = null): ByteArray {
        val out = ByteArrayOutputStream()
        for (c in channels) out.write(lengthDelimited(1, channelSettings(c.name, c.psk)))
        if (radio != null) out.write(lengthDelimited(2, loraConfig(radio)))
        return out.toByteArray()
    }

    fun decode(data: ByteArray): List<Shared> = decodeInvite(data).channels

    fun decodeInvite(data: ByteArray): Invite {
        val out = ArrayList<Shared>()
        var radio: Radio? = null
        val r = Reader(data, 0, data.size)
        while (r.hasMore) {
            val tag = r.varint() ?: return Invite(out, radio)
            val field = (tag shr 3).toInt()
            val wire = (tag and 7).toInt()
            if (wire != 2) {
                if (!r.skip(wire)) return Invite(out, radio)
                continue
            }
            val len = r.varint()?.toInt() ?: return Invite(out, radio)
            val end = r.pos + len
            if (len < 0 || end > r.limit) return Invite(out, radio)
            val body = Reader(data, r.pos, end)
            r.pos = end
            if (field == 2) {
                radio = decodeLora(body)
                continue
            }
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
        return Invite(out, radio)
    }

    /** LoRaConfig's three fields an invite acts on; anything else is skipped. */
    private fun decodeLora(body: Reader): Radio? {
        var preset = 0
        var region = 0
        var slot = 0
        while (body.hasMore) {
            val t = body.varint() ?: return null
            val f = (t shr 3).toInt()
            val w = (t and 7).toInt()
            if (w != 0) {
                if (!body.skip(w)) return null
                continue
            }
            val v = body.varint()?.toInt() ?: return null
            when (f) {
                2 -> preset = v
                7 -> region = v
                11 -> slot = v
            }
        }
        return Radio(PRESETS.getOrNull(preset), REGIONS.getOrNull(region), maxOf(slot, 0))
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

    fun url(channelName: String, psk: ByteArray, radio: Radio? = null): String =
        PREFIX + b64url(encode(listOf(Shared(channelName, psk)), radio))

    /**
     * Parses a scanned string. Accepts the full URL, the `?add=true` variant, and
     * a bare payload — people paste all three.
     */
    fun parse(text: String): List<Shared> = parseInvite(text).channels

    fun parseInvite(text: String): Invite {
        val none = Invite(emptyList(), null)
        var payload = text.trim()
        val hash = payload.indexOf('#')
        if (hash >= 0) payload = payload.substring(hash + 1)
        // "…#PAYLOAD?add=true" — the query lives after the fragment here.
        val q = payload.indexOf('?')
        if (q >= 0) payload = payload.substring(0, q)
        if (payload.isEmpty()) return none
        val data = unb64url(payload) ?: return none
        if (data.isEmpty()) return none
        return decodeInvite(data)
    }

    /**
     * The 1-based slot a node lands on when its slot is automatic: Meshtastic's
     * djb2 hash of the primary channel's name (the preset's name when the
     * channel has none) over the slots the band holds. mesh::slotForName.
     */
    fun automaticSlot(primaryName: String, bwKhz: Double, startMHz: Double, endMHz: Double, spacingMHz: Double): Int {
        var h = 5381L
        for (b in primaryName.toByteArray(Charsets.UTF_8)) h = (h * 33 + (b.toInt() and 0xFF)) and 0xFFFF_FFFFL
        val width = spacingMHz + bwKhz / 1000.0
        val n = if (width <= 0) 1L else maxOf(1L, kotlin.math.floor((endMHz - startMHz) / width).toLong())
        return (h % n).toInt() + 1
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
