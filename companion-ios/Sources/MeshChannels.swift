import CoreImage.CIFilterBuiltins
import Foundation
import SwiftUI
import UIKit

// Meshtastic channel sharing: the `https://meshtastic.org/e/#…` URL behind a
// channel QR code.
//
// The payload is a `meshtastic.ChannelSet` protobuf, base64url-encoded without
// padding. Encoding it ourselves rather than pulling in a protobuf library keeps
// this the same shape as the firmware's hand-written codec — two messages of
// scalar fields — and the bytes are pinned against Meshtastic's own default
// channel URL in tools/mesh_test.
//
// Because it IS the standard format, a channel created here can be joined by
// somebody running the stock Meshtastic app, and theirs can be joined here.

/// One channel the device holds. Slot 0 is the primary: its name sets the
/// frequency, so it is the one that decides which mesh the device is on.
struct MeshChannel: Identifiable, Equatable {
    let index: UInt8
    let name: String
    let hash: UInt8
    /// The key as it is SHARED: empty = unencrypted, 1 byte = one of Meshtastic's
    /// well-known keys, 16 or 32 bytes = a real key.
    let psk: Data
    /// Whether the device broadcasts our position on this channel. Off by default
    /// and per channel, so it can be on with a ride group and off in public.
    let sharesLocation: Bool

    var id: UInt8 { index }
    var isPrimary: Bool { index == 0 }
    var displayName: String { name.isEmpty ? "(unnamed)" : name }

    /// A channel only outsiders cannot read — i.e. not one of the published keys.
    var isPrivate: Bool { psk.count >= 16 }

    var keyDescription: String {
        switch psk.count {
        case 0: return "unencrypted"
        case 1: return "well-known key \(psk[psk.startIndex])"
        default: return "\(psk.count * 8)-bit key"
        }
    }
}

enum MeshChannelURL {
    static let prefix = "https://meshtastic.org/e/#"

    // MARK: - Minimal protobuf

    private static func varint(_ v: Int) -> Data {
        var out = Data()
        var x = UInt64(v)
        while x >= 0x80 {
            out.append(UInt8(x & 0x7F) | 0x80)
            x >>= 7
        }
        out.append(UInt8(x))
        return out
    }

    private static func lengthDelimited(field: Int, _ body: Data) -> Data {
        var out = Data()
        out.append(varint(field << 3 | 2))
        out.append(varint(body.count))
        out.append(body)
        return out
    }

    /// meshtastic.ChannelSettings: psk = 2 (bytes), name = 3 (string). Proto3
    /// omits empty fields, which is why an unnamed default channel is three bytes.
    private static func channelSettings(name: String, psk: Data) -> Data {
        var out = Data()
        if !psk.isEmpty { out.append(lengthDelimited(field: 2, psk)) }
        if !name.isEmpty { out.append(lengthDelimited(field: 3, Data(name.utf8))) }
        return out
    }

    /// meshtastic.ChannelSet: `repeated ChannelSettings settings = 1`.
    static func encode(_ channels: [(name: String, psk: Data)]) -> Data {
        var out = Data()
        for c in channels {
            out.append(lengthDelimited(field: 1, channelSettings(name: c.name, psk: c.psk)))
        }
        return out
    }

    static func decode(_ data: Data) -> [(name: String, psk: Data)] {
        var out: [(name: String, psk: Data)] = []
        var i = data.startIndex

        func readVarint() -> UInt64? {
            var v: UInt64 = 0
            var shift: UInt64 = 0
            while i < data.endIndex {
                let b = data[i]
                i = data.index(after: i)
                v |= UInt64(b & 0x7F) << shift
                if b & 0x80 == 0 { return v }
                shift += 7
                if shift > 63 { return nil }
            }
            return nil
        }

        while i < data.endIndex {
            guard let tag = readVarint() else { return out }
            let field = Int(tag >> 3), wire = Int(tag & 7)
            guard wire == 2, let len = readVarint(),
                  let end = data.index(i, offsetBy: Int(len), limitedBy: data.endIndex)
            else { return out }
            let body = data[i..<end]
            i = end
            guard field == 1 else { continue }     // not a channel; skip it

            // Inner ChannelSettings.
            var j = body.startIndex
            var name = ""
            var psk = Data()
            func innerVarint() -> UInt64? {
                var v: UInt64 = 0
                var shift: UInt64 = 0
                while j < body.endIndex {
                    let b = body[j]
                    j = body.index(after: j)
                    v |= UInt64(b & 0x7F) << shift
                    if b & 0x80 == 0 { return v }
                    shift += 7
                    if shift > 63 { return nil }
                }
                return nil
            }
            while j < body.endIndex {
                guard let t = innerVarint() else { break }
                let f = Int(t >> 3), w = Int(t & 7)
                if w == 2 {
                    guard let l = innerVarint(),
                          let e = body.index(j, offsetBy: Int(l), limitedBy: body.endIndex)
                    else { break }
                    if f == 2 { psk = Data(body[j..<e]) }
                    if f == 3 { name = String(data: Data(body[j..<e]), encoding: .utf8) ?? "" }
                    j = e
                } else if w == 0 {
                    _ = innerVarint()
                } else if w == 5 {
                    j = body.index(j, offsetBy: 4, limitedBy: body.endIndex) ?? body.endIndex
                } else if w == 1 {
                    j = body.index(j, offsetBy: 8, limitedBy: body.endIndex) ?? body.endIndex
                } else {
                    break
                }
            }
            out.append((name: name, psk: psk))
        }
        return out
    }

    // MARK: - URLs

    /// base64url without padding, which is what Meshtastic uses.
    private static func b64url(_ d: Data) -> String {
        d.base64EncodedString()
            .replacingOccurrences(of: "+", with: "-")
            .replacingOccurrences(of: "/", with: "_")
            .replacingOccurrences(of: "=", with: "")
    }

    private static func unb64url(_ s: String) -> Data? {
        var t = s.replacingOccurrences(of: "-", with: "+")
                 .replacingOccurrences(of: "_", with: "/")
        // Restore the padding Meshtastic strips.
        while t.count % 4 != 0 { t.append("=") }
        return Data(base64Encoded: t)
    }

    static func url(forChannelNamed name: String, psk: Data) -> String {
        prefix + b64url(encode([(name: name, psk: psk)]))
    }

    /// Parses a scanned string. Accepts the full URL, the `?add=true` variant, and
    /// a bare payload — people paste all three.
    static func parse(_ text: String) -> [(name: String, psk: Data)] {
        var payload = text.trimmingCharacters(in: .whitespacesAndNewlines)
        if let hash = payload.firstIndex(of: "#") {
            payload = String(payload[payload.index(after: hash)...])
        }
        // "…#PAYLOAD?add=true" — the query lives after the fragment here.
        if let q = payload.firstIndex(of: "?") { payload = String(payload[..<q]) }
        guard !payload.isEmpty, let data = unb64url(payload) else { return [] }
        return decode(data)
    }

    /// A fresh 256-bit key from the system CSPRNG. This is the whole secret behind
    /// a private channel, so it must not come from anything weaker.
    static func randomKey() -> Data {
        var bytes = [UInt8](repeating: 0, count: 32)
        let status = SecRandomCopyBytes(kSecRandomDefault, bytes.count, &bytes)
        guard status == errSecSuccess else {
            // Refusing is the only safe answer: a predictable "private" channel is
            // worse than no channel, because it looks encrypted.
            return Data()
        }
        return Data(bytes)
    }

    /// QR image for a share URL. CoreImage, so no dependency and no network.
    static func qrImage(for string: String, scale: CGFloat = 10) -> UIImage? {
        let filter = CIFilter.qrCodeGenerator()
        filter.message = Data(string.utf8)
        // M: ~15% recoverable. A phone screen photographed by another phone is a
        // clean channel; higher correction only makes the code denser to scan.
        filter.correctionLevel = "M"
        guard let out = filter.outputImage?.transformed(
            by: CGAffineTransform(scaleX: scale, y: scale)) else { return nil }
        let ctx = CIContext()
        guard let cg = ctx.createCGImage(out, from: out.extent) else { return nil }
        return UIImage(cgImage: cg)
    }
}
