package com.raemond.opentrailpaper.ble

/**
 * Album art pre-dithered on the phone, so the device gets a third of the
 * bytes: tone indices 0-4 (black, the three dark greys, white), base-5 packed
 * three pixels per byte, first pixel in the lowest digit. A 300 px square is
 * 30 KB over the air instead of 90 KB of grayscale.
 *
 * The dither is a straight port of media.cpp commitArt(): same thresholds,
 * same Floyd-Steinberg weights, same truncating integer division, so tone art
 * looks exactly like the art the device would have dithered itself.
 */
object ArtDither {

    // Approximate reflectances of the panel tones (media.cpp kLum).
    private val LUM = intArrayOf(0, 70, 120, 170, 255)

    /** 8-bit grayscale (row-major, w*h) -> one tone index (0-4) per pixel. */
    fun dither(gray: ByteArray, w: Int, h: Int): ByteArray {
        val out = ByteArray(w * h)
        // Error rows padded one cell each side, like the firmware's.
        var cur = IntArray(w + 2)
        var nxt = IntArray(w + 2)
        for (y in 0 until h) {
            for (x in 0 until w) {
                var v = (gray[y * w + x].toInt() and 0xFF) + cur[x + 1]
                if (v < 0) v = 0
                if (v > 255) v = 255
                var best = 0
                var bestD = 999
                for (k in LUM.indices) {
                    val d = kotlin.math.abs(v - LUM[k])
                    if (d < bestD) { bestD = d; best = k }
                }
                out[y * w + x] = best.toByte()
                val e = v - LUM[best]
                cur[x + 2] += e * 7 / 16
                nxt[x] += e * 3 / 16
                nxt[x + 1] += e * 5 / 16
                nxt[x + 2] += e * 1 / 16
            }
            val t = cur; cur = nxt; nxt = t
            nxt.fill(0)
        }
        return out
    }

    /** Tone indices -> base-5 triples, ceil(n/3) bytes. */
    fun pack(tones: ByteArray): ByteArray {
        val out = ByteArray((tones.size + 2) / 3)
        for (i in out.indices) {
            val a = tones[i * 3].toInt()
            val b = tones.getOrElse(i * 3 + 1) { 0 }.toInt()
            val c = tones.getOrElse(i * 3 + 2) { 0 }.toInt()
            out[i] = (a + b * 5 + c * 25).toByte()
        }
        return out
    }
}
