// The app's ArtDither (companion-ios/Sources/ArtDither.swift) on the same
// images as firmware_dither.cpp. Usage: sw <w> <h> <recipe> [pack]
import Foundation

func px(_ recipe: Int, _ x: Int, _ y: Int, _ w: Int, _ h: Int, _ rng: inout UInt32) -> UInt8 {
    switch recipe {
    case 0: return UInt8((x * 255) / (w > 1 ? w - 1 : 1))
    case 1: return UInt8(((x + y) * 255) / (w + h - 2 > 0 ? w + h - 2 : 1))
    case 2: rng = rng &* 1103515245 &+ 12345; return UInt8(truncatingIfNeeded: rng >> 16)
    case 3: return 200
    default: return UInt8((x * 31 + y * 17) & 0xFF)
    }
}

let a = CommandLine.arguments
let w = Int(a[1])!, h = Int(a[2])!, recipe = Int(a[3])!
var rng: UInt32 = 1
var g = [UInt8](repeating: 0, count: w * h)
for y in 0..<h { for x in 0..<w { g[y * w + x] = px(recipe, x, y, w, h, &rng) } }
let tones = ArtDither.dither(g, width: w, height: h)
if a.count > 4 {
    FileHandle.standardOutput.write(Data(ArtDither.pack(tones)))
} else {
    print(String(tones.map { Character(String($0)) }), terminator: "")
}
