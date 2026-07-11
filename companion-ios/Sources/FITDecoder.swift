import Foundation
import CoreLocation

// A parsed ride, ready to preview.
struct RidePreview {
    struct Point {
        let coordinate: CLLocationCoordinate2D
        let altitude: Double?
        let speedKmh: Double?
        let power: Int?
        let heartRate: Int?
        let cadence: Int?
    }
    var points: [Point] = []
    var start: Date?
    var duration: TimeInterval = 0
    var distanceKm: Double = 0
    var avgSpeedKmh: Double = 0
    var maxSpeedKmh: Double = 0
    var avgPower: Int?
    var avgHeartRate: Int?
    var ascentM: Double = 0

    var coordinates: [CLLocationCoordinate2D] { points.map(\.coordinate) }
}

// Minimal FIT decoder for the files this device writes (see
// firmware src/fit_writer.cpp). It reads definition + data messages
// generically and interprets the record message (global 20).
enum FITDecoder {
    private static let epochOffset: TimeInterval = 631_065_600  // 1989-12-31
    private static let semicircle = 180.0 / 2147483648.0

    struct FieldDef { let num: Int; let size: Int }
    struct MsgDef { let global: Int; let fields: [FieldDef] }

    static func decode(_ data: Data) -> RidePreview? {
        let b = [UInt8](data)
        guard b.count > 14 else { return nil }

        let headerSize = Int(b[0])
        guard b.count >= headerSize else { return nil }
        let dataSize = Int(b[4]) | (Int(b[5]) << 8) | (Int(b[6]) << 16) | (Int(b[7]) << 24)
        var i = headerSize
        // A ride that lost power mid-recording never got its header patched
        // (data_size = 0) or trailer/CRC written. In that case scan to the end
        // of the file — the record messages before the cut are still valid, and
        // the bounds/undefined-type checks below stop cleanly at any trailing
        // partial write or padding.
        var end = headerSize + dataSize
        if dataSize == 0 || end > b.count || end < headerSize {
            end = b.count
        }

        var defs: [Int: MsgDef] = [:]
        var preview = RidePreview()
        var firstTime: UInt32? = nil
        var lastTime: UInt32 = 0
        var powerSum = 0, powerN = 0, hrSum = 0, hrN = 0
        var climbBase: Double? = nil   // 3 m-hysteresis ascent, like the device

        while i < end {
            let rec = b[i]; i += 1
            if rec & 0x40 != 0 {                    // definition message
                let local = Int(rec & 0x0F)
                guard i + 5 <= end else { break }
                // b[i]=reserved, b[i+1]=arch (0=LE), b[i+2..3]=global, b[i+4]=count
                let global = Int(b[i + 2]) | (Int(b[i + 3]) << 8)
                let count = Int(b[i + 4])
                i += 5
                var fields: [FieldDef] = []
                for _ in 0..<count {
                    guard i + 3 <= end else { break }
                    fields.append(FieldDef(num: Int(b[i]), size: Int(b[i + 1])))
                    i += 3
                }
                // Developer fields (0x20) — this device never emits them.
                defs[local] = MsgDef(global: global, fields: fields)
            } else {                                // data message
                let local = Int(rec & 0x0F)
                guard let def = defs[local] else { break }
                var off = i
                var lat: Int32? = nil, lon: Int32? = nil
                var alt: Double? = nil, speed: Double? = nil
                var power: Int? = nil, hr: Int? = nil, cad: Int? = nil
                for f in def.fields {
                    guard off + f.size <= end else { break }
                    if def.global == 20 {           // record
                        switch f.num {
                        case 253: lastTime = u32(b, off)
                                  if firstTime == nil { firstTime = lastTime }
                        case 0:   let v = Int32(bitPattern: u32(b, off))
                                  if v != Int32(bitPattern: 0x7FFFFFFF) { lat = v }
                        case 1:   let v = Int32(bitPattern: u32(b, off))
                                  if v != Int32(bitPattern: 0x7FFFFFFF) { lon = v }
                        case 2:   let v = u16(b, off)
                                  if v != 0xFFFF { alt = Double(v) / 5.0 - 500.0 }
                        case 6:   let v = u16(b, off)
                                  if v != 0xFFFF { speed = Double(v) / 1000.0 * 3.6 }
                        case 7:   let v = u16(b, off); if v != 0xFFFF { power = Int(v) }
                        case 3:   let v = b[off]; if v != 0xFF { hr = Int(v) }
                        case 4:   let v = b[off]; if v != 0xFF { cad = Int(v) }
                        default: break
                        }
                    }
                    off += f.size
                }
                i = off
                if def.global == 20, let lat, let lon {
                    let coord = CLLocationCoordinate2D(
                        latitude: Double(lat) * semicircle,
                        longitude: Double(lon) * semicircle)
                    preview.points.append(.init(
                        coordinate: coord, altitude: alt, speedKmh: speed,
                        power: power, heartRate: hr, cadence: cad))
                    if let s = speed { preview.maxSpeedKmh = max(preview.maxSpeedKmh, s) }
                    if let p = power { powerSum += p; powerN += 1 }
                    if let h = hr { hrSum += h; hrN += 1 }
                    if let a = alt {
                        if let base = climbBase {
                            if a > base + 3 { preview.ascentM += a - base; climbBase = a }
                            else if a < base - 3 { climbBase = a }
                        } else {
                            climbBase = a
                        }
                    }
                }
            }
        }

        guard preview.points.count >= 2 else { return nil }

        if let ft = firstTime {
            preview.start = Date(timeIntervalSince1970: epochOffset + Double(ft))
            preview.duration = Double(lastTime - ft)
        }
        preview.distanceKm = trackDistanceKm(preview.coordinates)
        if preview.duration > 0 {
            preview.avgSpeedKmh = preview.distanceKm / (preview.duration / 3600)
        }
        preview.avgPower = powerN > 0 ? powerSum / powerN : nil
        preview.avgHeartRate = hrN > 0 ? hrSum / hrN : nil
        return preview
    }

    private static func u16(_ b: [UInt8], _ o: Int) -> UInt16 {
        UInt16(b[o]) | (UInt16(b[o + 1]) << 8)
    }
    private static func u32(_ b: [UInt8], _ o: Int) -> UInt32 {
        UInt32(b[o]) | (UInt32(b[o + 1]) << 8) | (UInt32(b[o + 2]) << 16) | (UInt32(b[o + 3]) << 24)
    }

    private static func trackDistanceKm(_ c: [CLLocationCoordinate2D]) -> Double {
        guard c.count > 1 else { return 0 }
        var m = 0.0
        for i in 1..<c.count {
            m += CLLocation(latitude: c[i-1].latitude, longitude: c[i-1].longitude)
                .distance(from: CLLocation(latitude: c[i].latitude, longitude: c[i].longitude))
        }
        return m / 1000
    }
}
