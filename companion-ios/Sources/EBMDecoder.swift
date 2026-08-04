import Foundation
import MapKit

// Reads back the `.ebm` blobs this app builds (MapBuilder.swift) and the head
// unit renders (src/map_tiles.cpp), so the phone can draw the SAME map the
// device will — roads by class, water, parks.
//
// The projection deliberately mirrors projectBlobInto() rather than the
// encoder: the device derives its scale factor from the grid header
// (midLat = gridLat0 + tileDeg * gridNy / 2) instead of the bounding box the
// encoder used, and those differ by a few metres. Reproducing what the DEVICE
// computes is what makes this an honest preview.
//
// Format (little-endian), from MapBuilder:
//   'EBM2', f64 lat0, f64 lon0, f64 tileDeg, i32 nx, i32 ny,
//   index[nx*ny] of (u32 offset, u32 length)   (0,0 = empty sub-tile),
//   sub-tiles: u16 polylineCount, per polyline { u8 class, u16 pointCount,
//              i16 x,y per point (metres E/N of the SUB-TILE's SW corner) },
//   then optional 'ELV1', then 'WTR2' and 'PRK2' polygon sections whose points
//   are metres E/N of the GRID origin (lat0, lon0).
enum EBM {

    /// Render classes, numbered as in build_map.py / mapgen.js / the firmware's
    /// MapFeatureClass enum.
    enum FeatureClass: UInt8, CaseIterable {
        case arterial = 0, primary = 1, secondary = 2, tertiary = 3, minor = 4, path = 5
    }

    /// One decoded tile, already projected into map points — the space the
    /// overlay renderer draws in, so nothing has to be re-projected per frame.
    struct Tile {
        /// Roads grouped by class: one CGPath per class, all in MKMapPoint space.
        let roads: [FeatureClass: CGPath]
        let water: CGPath?
        let parks: CGPath?
        /// Rough cost measure (total points) for the memory budget.
        let pointCount: Int
    }

    /// Decodes a blob, or nil if it isn't a well-formed EBM2.
    static func decode(_ data: Data) -> Tile? {
        var r = Reader(data)
        guard r.magic("EBM2"),
              let lat0 = r.f64(at: 4), let lon0 = r.f64(at: 12),
              let tileDeg = r.f64(at: 20),
              let nx32 = r.i32(at: 28), let ny32 = r.i32(at: 32) else { return nil }
        let nx = Int(nx32), ny = Int(ny32)
        // Bound the grid before trusting it for arithmetic: a corrupt header
        // must not turn into a multi-gigabyte index walk.
        guard nx > 0, ny > 0, nx <= 4096, ny <= 4096, tileDeg > 0,
              36 + nx * ny * 8 <= data.count else { return nil }

        // Exactly the device's scale factors (map_tiles.cpp:123).
        let midLat = lat0 + tileDeg * Double(ny) / 2
        let kx = 111320.0 * cos(midLat * .pi / 180)
        let ky = 110540.0
        guard kx > 1 else { return nil }          // degenerate near the poles
        let tileWm = tileDeg * kx, tileHm = tileDeg * ky

        // Metres E/N of the grid origin -> a map point.
        func project(_ mx: Double, _ my: Double) -> CGPoint {
            let p = MKMapPoint(CLLocationCoordinate2D(latitude: lat0 + my / ky,
                                                      longitude: lon0 + mx / kx))
            return CGPoint(x: p.x, y: p.y)
        }

        var roadPaths: [FeatureClass: CGMutablePath] = [:]
        var points = 0

        // --- roads, per sub-tile
        let indexBase = 36
        var dataEnd = indexBase + nx * ny * 8      // also where the sections start
        for ty in 0..<ny {
            for tx in 0..<nx {
                let e = indexBase + (ty * nx + tx) * 8
                guard let off32 = r.u32(at: e), let len32 = r.u32(at: e + 4) else { continue }
                let off = Int(off32), len = Int(len32)
                guard off > 0, len > 0, off + len <= data.count else { continue }
                dataEnd = max(dataEnd, off + len)

                let ox = Double(tx) * tileWm, oy = Double(ty) * tileHm
                var p = off
                guard let count = r.u16(at: p) else { continue }
                p += 2
                for _ in 0..<Int(count) {
                    guard p + 3 <= off + len, let cls = r.u8(at: p),
                          let n16 = r.u16(at: p + 1) else { break }
                    p += 3
                    let n = Int(n16)
                    guard p + n * 4 <= off + len else { break }
                    defer { p += n * 4 }
                    guard let fc = FeatureClass(rawValue: cls), n >= 2 else { continue }
                    let path = roadPaths[fc] ?? { let m = CGMutablePath(); roadPaths[fc] = m; return m }()
                    for j in 0..<n {
                        guard let mx = r.i16(at: p + j * 4),
                              let my = r.i16(at: p + j * 4 + 2) else { break }
                        let pt = project(ox + Double(mx), oy + Double(my))
                        if j == 0 { path.move(to: pt) } else { path.addLine(to: pt) }
                    }
                    points += n
                }
            }
        }

        // --- fill sections: optional ELV1 (skipped), then WTR2, then PRK2.
        var q = dataEnd
        if q + 44 <= data.count, r.magic("ELV1", at: q),
           let gw = r.i32(at: q + 4), let gh = r.i32(at: q + 8),
           gw > 0, gh > 0, gw <= 4096, gh <= 4096 {
            q += 44 + Int(gw) * Int(gh) * 2
        }
        var water: CGPath? = nil, parks: CGPath? = nil
        if let (path, next, n) = readFills(&r, at: q, magic: "WTR2", project: project) {
            water = path; q = next; points += n
        }
        if let (path, _, n) = readFills(&r, at: q, magic: "PRK2", project: project) {
            parks = path; points += n
        }

        let roads = roadPaths.mapValues { $0 as CGPath }
        guard !roads.isEmpty || water != nil || parks != nil else { return nil }
        return Tile(roads: roads, water: water, parks: parks, pointCount: points)
    }

    /// `<magic><u16 count>`, then each polygon as `<u16 pointCount><i16 x,y …>`
    /// in metres E/N of the grid origin. Returns the combined path (rings are
    /// closed, as the device closes them), where the section ends, and its size.
    private static func readFills(_ r: inout Reader, at start: Int, magic: String,
                                  project: (Double, Double) -> CGPoint)
        -> (CGPath, Int, Int)? {
        guard start >= 0, start + 6 <= r.count, r.magic(magic, at: start),
              let count = r.u16(at: start + 4) else { return nil }
        var q = start + 6
        let path = CGMutablePath()
        var total = 0
        for _ in 0..<Int(count) {
            guard let n16 = r.u16(at: q) else { break }
            q += 2
            let n = Int(n16)
            guard q + n * 4 <= r.count else { break }
            defer { q += n * 4 }
            guard n >= 3 else { continue }
            for j in 0..<n {
                guard let mx = r.i16(at: q + j * 4),
                      let my = r.i16(at: q + j * 4 + 2) else { break }
                let pt = project(Double(mx), Double(my))
                if j == 0 { path.move(to: pt) } else { path.addLine(to: pt) }
            }
            path.closeSubpath()
            total += n
        }
        return (path, q, total)
    }

    /// Bounds-checked little-endian reads. Every accessor returns nil rather
    /// than trapping, so a truncated or corrupt tile degrades to "draws less"
    /// instead of crashing the map.
    private struct Reader {
        let d: [UInt8]
        init(_ data: Data) { d = [UInt8](data) }
        var count: Int { d.count }

        func u8(at i: Int) -> UInt8? { i >= 0 && i < d.count ? d[i] : nil }
        func u16(at i: Int) -> UInt16? {
            guard i >= 0, i + 2 <= d.count else { return nil }
            return UInt16(d[i]) | (UInt16(d[i + 1]) << 8)
        }
        func i16(at i: Int) -> Int16? { u16(at: i).map { Int16(bitPattern: $0) } }
        func u32(at i: Int) -> UInt32? {
            guard i >= 0, i + 4 <= d.count else { return nil }
            var v: UInt32 = 0
            for k in (0..<4).reversed() { v = (v << 8) | UInt32(d[i + k]) }
            return v
        }
        func i32(at i: Int) -> Int32? { u32(at: i).map { Int32(bitPattern: $0) } }
        func f64(at i: Int) -> Double? {
            guard i >= 0, i + 8 <= d.count else { return nil }
            var bits: UInt64 = 0
            for k in (0..<8).reversed() { bits = (bits << 8) | UInt64(d[i + k]) }
            return Double(bitPattern: bits)
        }
        func magic(_ s: String, at i: Int = 0) -> Bool {
            let m = Array(s.utf8)
            guard i >= 0, i + m.count <= d.count else { return false }
            return Array(d[i..<(i + m.count)]) == m
        }
    }
}
