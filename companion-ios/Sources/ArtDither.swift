import Foundation

/// Album art pre-dithered on the phone, so the device gets a third of the
/// bytes: tone indices 0-4 (black, the three dark greys, white), base-5 packed
/// three pixels per byte, first pixel in the lowest digit. A 300 px square is
/// 30 KB over the air instead of 90 KB of grayscale.
///
/// A straight port of media.cpp commitArt() (and the Android app's
/// ArtDither.kt): same thresholds, same Floyd-Steinberg weights, same
/// truncating integer division, so tone art looks exactly like the art the
/// device would have dithered itself. tools/art_dither_parity checks this
/// file byte-for-byte against the firmware's loop.
enum ArtDither {

    /// Approximate reflectances of the panel tones (media.cpp kLum).
    static let lum = [0, 70, 120, 170, 255]

    /// 8-bit grayscale (row-major, w*h) -> one tone index (0-4) per pixel.
    static func dither(_ gray: [UInt8], width w: Int, height h: Int) -> [UInt8] {
        var out = [UInt8](repeating: 0, count: w * h)
        // Error rows padded one cell each side, like the firmware's.
        var cur = [Int](repeating: 0, count: w + 2)
        var nxt = [Int](repeating: 0, count: w + 2)
        for y in 0..<h {
            for x in 0..<w {
                var v = Int(gray[y * w + x]) + cur[x + 1]
                if v < 0 { v = 0 }
                if v > 255 { v = 255 }
                var best = 0, bestD = 999
                for k in 0..<lum.count {
                    let d = abs(v - lum[k])
                    if d < bestD { bestD = d; best = k }
                }
                out[y * w + x] = UInt8(best)
                let e = v - lum[best]
                // Swift's / truncates toward zero, like C and Kotlin.
                cur[x + 2] += e * 7 / 16
                nxt[x] += e * 3 / 16
                nxt[x + 1] += e * 5 / 16
                nxt[x + 2] += e * 1 / 16
            }
            swap(&cur, &nxt)
            for i in nxt.indices { nxt[i] = 0 }
        }
        return out
    }

    /// Tone indices -> base-5 triples, ceil(n/3) bytes.
    static func pack(_ tones: [UInt8]) -> [UInt8] {
        var out = [UInt8](repeating: 0, count: (tones.count + 2) / 3)
        for i in out.indices {
            let a = Int(tones[i * 3])
            let b = i * 3 + 1 < tones.count ? Int(tones[i * 3 + 1]) : 0
            let c = i * 3 + 2 < tones.count ? Int(tones[i * 3 + 2]) : 0
            out[i] = UInt8(a + b * 5 + c * 25)
        }
        return out
    }
}
