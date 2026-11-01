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

/// What a scanned code holds: the channels, and — when the sender's app put it
/// there — the radio settings they were on.
struct MeshChannelShare {
    var channels: [(name: String, psk: Data)] = []
    var radio: MeshRadioConfig? = nil
}

/// `meshtastic.Config.LoRaConfig` as a share carries it (`ChannelSet.lora_config`).
///
/// A channel's key only matters if both radios are on the same frequency at the
/// same speed; nodes on different presets or slots cannot hear each other at all.
/// So the code says where the sender is, and the scanner can offer to go there.
/// Enums are Meshtastic's numbers, not the firmware's table indexes — the firmware
/// streams its presets and regions by name, so the two are matched by name.
struct MeshRadioConfig: Equatable {
    var usePreset = true
    /// Meshtastic `ModemPreset`.
    var modemPreset = 0
    /// Meshtastic `RegionCode`; 0 = UNSET, i.e. the code says nothing about it.
    var region = 0
    /// 3 is Meshtastic's default. Never 0: a stock node that imports 0 stops
    /// relaying.
    var hopLimit = 3
    /// Always true: a stock app importing false here stops transmitting.
    var txEnabled = true
    /// 1-based frequency slot; 0 = hash of the primary channel's name.
    var channelNum = 0

    static let presetCodes: [String: Int] = [
        "LongFast": 0, "LongSlow": 1, "VeryLongSlow": 2, "MediumSlow": 3,
        "MediumFast": 4, "ShortSlow": 5, "ShortFast": 6, "LongModerate": 7,
        "ShortTurbo": 8, "LongTurbo": 9,
    ]
    static let regionCodes: [String: Int] = [
        "US": 1, "EU_433": 2, "EU_868": 3, "CN": 4, "JP": 5, "ANZ": 6, "KR": 7,
        "TW": 8, "RU": 9, "IN": 10, "NZ_865": 11, "TH": 12, "LORA_24": 13,
        "UA_433": 14, "UA_868": 15, "MY_433": 16, "MY_919": 17, "SG_923": 18,
        "PH_433": 19, "PH_868": 20, "PH_915": 21, "ANZ_433": 22, "KZ_433": 23,
        "KZ_863": 24, "NP_865": 25, "BR_902": 26,
    ]

    var presetName: String? {
        Self.presetCodes.first { $0.value == modemPreset }?.key
    }
    var regionName: String? {
        region == 0 ? nil : Self.regionCodes.first { $0.value == region }?.key
    }

    /// The device's settings as a share carries them. Nil until the presets have
    /// loaded (or for a preset Meshtastic has no number for): the URL then goes
    /// out without lora_config, as it did before, rather than with a wrong one.
    init?(state: MeshState, presets: [MeshPreset], regions: [MeshRegion]) {
        guard let p = presets.first(where: { $0.index == state.presetIndex }),
              let code = Self.presetCodes[p.name] else { return nil }
        modemPreset = code
        if let ri = state.regionIndex, let r = regions.first(where: { $0.index == ri }) {
            region = Self.regionCodes[r.name] ?? 0
        }
        if let slot = state.activeSlot, slot > 0 { channelNum = Int(slot) }
    }

    init(usePreset: Bool = true, modemPreset: Int = 0, region: Int = 0,
         hopLimit: Int = 3, txEnabled: Bool = true, channelNum: Int = 0) {
        self.usePreset = usePreset
        self.modemPreset = modemPreset
        self.region = region
        self.hopLimit = hopLimit
        self.txEnabled = txEnabled
        self.channelNum = channelNum
    }

    /// "MediumFast · US · slot 44", the line under the QR.
    var summary: String {
        var parts = [presetName ?? "Preset \(modemPreset)"]
        if let r = regionName { parts.append(r) }
        if channelNum > 0 { parts.append("slot \(channelNum)") }
        return parts.joined(separator: " · ")
    }
}

/// Meshtastic's frequency-slot arithmetic, the same as `mesh::channelCount` /
/// `slotForName` in src/mesh_proto.cpp. Needed to tell whether a code's slot is
/// the one the device would land on by itself, so it is not pinned needlessly.
enum MeshSlots {
    /// djb2 over the name's bytes — Meshtastic's choice for the slot, not the
    /// channel hash.
    static func nameHash(_ name: String) -> UInt32 {
        var h: UInt32 = 5381
        for b in name.utf8 { h = h &* 33 &+ UInt32(b) }
        return h
    }

    static func channelCount(startHz: UInt32, endHz: UInt32, spacingHz: UInt32,
                             bandwidthKhz: Double) -> Int {
        let slot = Double(spacingHz) + bandwidthKhz * 1000
        guard slot > 0, endHz > startHz else { return 1 }
        return max(1, Int((Double(endHz - startHz) / slot).rounded(.down)))
    }

    /// 0-based, as the hash gives it; Meshtastic's channel_num is this + 1.
    static func slot(forName name: String, count: Int) -> Int {
        Int(nameHash(name) % UInt32(max(1, count)))
    }

    static func frequencyMHz(slot: Int, startHz: UInt32, spacingHz: UInt32,
                             bandwidthKhz: Double) -> Double {
        Double(startHz) / 1e6 + bandwidthKhz / 2000
            + Double(slot) * (Double(spacingHz) / 1e6 + bandwidthKhz / 1000)
    }
}

/// What it takes to put this device on a scanned code's radio settings.
struct MeshRadioMatch: Equatable {
    var preset: MeshPreset? = nil
    var region: MeshRegion? = nil
    /// Slot override to send: N pins slot N, 0 hands it back to the name hash.
    var slotOverride: UInt8? = nil
    /// "Speed: LongFast → MediumFast", one per setting that moves.
    var changes: [String] = []
    /// Settings the code asks for that this firmware does not have.
    var unsupported: [String] = []

    var isEmpty: Bool { changes.isEmpty }

    /// `primaryName` is the device's primary channel name when it set one
    /// explicitly; nil when it follows the preset, as a stock node does.
    init(_ code: MeshRadioConfig, state: MeshState, presets: [MeshPreset],
         regions: [MeshRegion], primaryName: String?) {
        let curPreset = presets.first { $0.index == state.presetIndex }
        var newPreset = curPreset
        if !code.usePreset {
            // Hand-set SF/bandwidth: the firmware only runs named presets.
            unsupported.append("This device can't use a custom modem setting")
        } else if let name = code.presetName, let p = presets.first(where: { $0.name == name }) {
            if p.index != state.presetIndex {
                preset = p
                newPreset = p
                changes.append("Speed: \(curPreset?.name ?? "?") → \(p.name)")
            }
        } else {
            unsupported.append("This device can't use \(code.presetName ?? "preset \(code.modemPreset)")")
        }

        // Region only when both sides have one: a code with UNSET says nothing,
        // and firmware that compiled its region in cannot change it.
        var newRegion: MeshRegion? = nil
        if let ri = state.regionIndex {
            let cur = regions.first { $0.index == ri }
            newRegion = cur
            if code.region != 0 {
                let name = code.regionName ?? "region \(code.region)"
                if let r = regions.first(where: { $0.name == name }) {
                    if r.index != ri {
                        region = r
                        newRegion = r
                        changes.append("Region: \(cur?.name ?? "?") → \(r.name)")
                    }
                } else {
                    unsupported.append("This device can't use \(name)")
                }
            }
        }

        // Slot: where the device would land on its own (the hash of its primary
        // name, in the new band at the new bandwidth) decides between unpinning
        // and pinning. Without the tables, or with no slot in the code, leave it.
        let n = code.channelNum
        guard n > 0, n <= 255, let pinned = state.slotOverride,
              let band = newRegion, let modem = newPreset else { return }
        let count = MeshSlots.channelCount(startHz: band.startHz, endHz: band.endHz,
                                           spacingHz: band.spacingHz,
                                           bandwidthKhz: modem.bandwidthKhz)
        let auto = MeshSlots.slot(forName: primaryName ?? modem.name, count: count) + 1
        if n == auto {
            if pinned != 0 { slotOverride = 0 }
        } else if Int(pinned) != n {
            slotOverride = UInt8(n)
        }
        if let now = state.activeSlot, Int(now) != n {
            changes.append("Slot: \(now) → \(n)")
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

    /// meshtastic.Config.LoRaConfig, only the fields a share carries. All varints,
    /// and proto3 drops zeros, so Meshtastic's default (LongFast, no region, slot
    /// from the name) is just use_preset + hop_limit + tx_enabled.
    private static func loraConfig(_ r: MeshRadioConfig) -> Data {
        var out = Data()
        func put(_ field: Int, _ v: Int) {
            guard v != 0 else { return }
            out.append(varint(field << 3))
            out.append(varint(v))
        }
        put(1, r.usePreset ? 1 : 0)
        put(2, r.modemPreset)
        put(7, r.region)
        put(8, r.hopLimit)
        put(9, r.txEnabled ? 1 : 0)
        put(11, r.channelNum)
        return out
    }

    /// meshtastic.ChannelSet: `repeated ChannelSettings settings = 1`,
    /// `LoRaConfig lora_config = 2`. Settings first, as Meshtastic writes them.
    static func encode(_ channels: [(name: String, psk: Data)],
                       radio: MeshRadioConfig? = nil) -> Data {
        var out = Data()
        for c in channels {
            out.append(lengthDelimited(field: 1, channelSettings(name: c.name, psk: c.psk)))
        }
        if let radio { out.append(lengthDelimited(field: 2, loraConfig(radio))) }
        return out
    }

    /// One protobuf field. Fixed-width values are skipped, not kept: nothing a
    /// share carries that we read is fixed32/64 (frequency_offset is, and is
    /// ignored).
    private enum Value {
        case varint(UInt64)
        case bytes(Data)
        case fixed
    }

    /// Walks a message's fields, calling `each` for every one. Returns false if
    /// the bytes stop making sense part-way; the fields before that were seen.
    @discardableResult
    private static func forEachField(_ data: Data, _ each: (Int, Value) -> Void) -> Bool {
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
        func skip(_ n: Int) -> Bool {
            guard let e = data.index(i, offsetBy: n, limitedBy: data.endIndex) else { return false }
            i = e
            return true
        }
        while i < data.endIndex {
            guard let tag = readVarint() else { return false }
            let field = Int(tag >> 3)
            switch tag & 7 {
            case 0:
                guard let v = readVarint() else { return false }
                each(field, .varint(v))
            case 2:
                guard let len = readVarint(), len <= UInt64(Int.max),
                      let e = data.index(i, offsetBy: Int(len), limitedBy: data.endIndex)
                else { return false }
                each(field, .bytes(Data(data[i..<e])))
                i = e
            case 5:
                guard skip(4) else { return false }
                each(field, .fixed)
            case 1:
                guard skip(8) else { return false }
                each(field, .fixed)
            default:
                return false                        // groups: nothing here uses them
            }
        }
        return true
    }

    private static func decodeLora(_ data: Data) -> MeshRadioConfig? {
        var r = MeshRadioConfig(usePreset: false, modemPreset: 0, region: 0,
                                hopLimit: 0, txEnabled: false, channelNum: 0)
        let ok = forEachField(data) { field, value in
            guard case .varint(let v) = value else { return }
            let n = Int(truncatingIfNeeded: v)
            switch field {
            case 1: r.usePreset = v != 0
            case 2: r.modemPreset = n
            case 7: r.region = n
            case 8: r.hopLimit = n
            case 9: r.txEnabled = v != 0
            case 11: r.channelNum = n
            default: break
            }
        }
        // A half-read radio config is worse than none: acting on it could move the
        // radio somewhere the sender never was.
        return ok ? r : nil
    }

    static func decode(_ data: Data) -> MeshChannelShare {
        var share = MeshChannelShare()
        forEachField(data) { field, value in
            guard case .bytes(let body) = value else { return }
            switch field {
            case 1:
                var name = ""
                var psk = Data()
                forEachField(body) { f, v in
                    guard case .bytes(let b) = v else { return }
                    if f == 2 { psk = b }
                    if f == 3 { name = String(data: b, encoding: .utf8) ?? "" }
                }
                share.channels.append((name: name, psk: psk))
            case 2:
                share.radio = decodeLora(body)
            default:
                break                               // a field we do not know; skip it
            }
        }
        return share
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

    /// `radio` nil leaves lora_config out, which is what the URL was before it
    /// carried the radio settings — the receiver then keeps its own.
    static func url(forChannelNamed name: String, psk: Data,
                    radio: MeshRadioConfig? = nil) -> String {
        prefix + b64url(encode([(name: name, psk: psk)], radio: radio))
    }

    /// Parses a scanned string. Accepts the full URL, the `?add=true` variant, and
    /// a bare payload — people paste all three.
    static func parse(_ text: String) -> MeshChannelShare {
        var payload = text.trimmingCharacters(in: .whitespacesAndNewlines)
        if let hash = payload.firstIndex(of: "#") {
            payload = String(payload[payload.index(after: hash)...])
        }
        // "…#PAYLOAD?add=true" — the query lives after the fragment here.
        if let q = payload.firstIndex(of: "?") { payload = String(payload[..<q]) }
        guard !payload.isEmpty, let data = unb64url(payload) else { return MeshChannelShare() }
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
