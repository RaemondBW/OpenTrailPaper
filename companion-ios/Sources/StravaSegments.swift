import Foundation

// The rider's starred Strava segments, for the device's SEGMENT page:
// parsing Strava's answers, cutting each segment's line (and its elevation)
// down to what the device keeps, and the blob the device reads
// (src/segment_match.h). Foundation only, so tools/segment_test can compile it
// on the Mac. Mirrors data/StravaSegments.kt in the Android app; both encoders
// are checked against the same bytes as the firmware's parser.

/// One starred segment, as GET /segments/starred lists it.
struct StarredSegment: Hashable, Identifiable {
    let id: UInt64
    let name: String
    let distanceMeters: Double
    let averageGrade: Double
    /// The athlete's PR elapsed time, s; nil when they have none (or the list left it out).
    let prSeconds: Int?
    let activityType: String?
    /// Only ride segments go to a bike computer; a list without the type is taken as rides.
    var isRide: Bool { activityType == nil || activityType == "Ride" || activityType == "EBikeRide" }
}

struct StarredSegmentPage {
    let segments: [StarredSegment]
    let nextPage: Int?
}

/// A segment's line as the device keeps it: at most `maxPoints` points, with
/// the altitude at each when Strava had one. Also what the app caches per id.
struct SegmentGeometry: Equatable, Codable {
    let lat: [Double]
    let lon: [Double]
    /// Metres, one per point; nil = no elevation (a polyline-only fallback).
    let alt: [Double]?

    var points: [LatLon] { zip(lat, lon).map { LatLon(lat: $0, lon: $1) } }

    init(points: [LatLon], altitudes: [Double]?) {
        lat = points.map(\.lat)
        lon = points.map(\.lon)
        alt = altitudes.flatMap { $0.count == points.count ? $0 : nil }
    }
}

/// A segment as the device gets it.
struct DeviceSegment: Equatable {
    let id: UInt64
    let name: String
    let prSeconds: Int
    let distanceMeters: Double
    let averageGrade: Double
    let points: [LatLon]
    /// Metres, one per point; nil = none.
    var elevations: [Double]? = nil
}

enum StravaSegments {
    /// Strava's largest page.
    static let pageSize = 200
    /// Starred pages read at most (600 segments is far past what is synced).
    static let maxPages = 3
    /// How many go to the device. One streams request each the first time
    /// (cached after), so this also keeps a first sync inside Strava's
    /// 100-requests-per-15-minutes limit. The firmware takes up to 64.
    static let maxSegments = 50
    /// Points per segment; the firmware's MAX_POINTS.
    static let maxPoints = 200
    /// Name bytes; the firmware's NAME_BYTES.
    static let nameBytes = 47
    /// The newest blob version the apps write; v2 carries elevation.
    static let blobVersion = 2
    /// v2 elevation: decimetres above this many metres below sea level.
    static let elevOffsetM = 1000.0
    /// How much a metre of height counts against a metre of line in the cut
    /// (see simplify): 0.5 m off the profile weighs like 5 m off the road.
    static let elevWeight = 10.0

    // MARK: Strava

    /// GET /api/v3/segments/starred?page=&per_page=: a bare array.
    static func parseStarred(_ data: Data, page: Int) -> StarredSegmentPage {
        guard let arr = (try? JSONSerialization.jsonObject(with: data)) as? [Any] else {
            return StarredSegmentPage(segments: [], nextPage: nil)
        }
        let segs = arr.compactMap { e -> StarredSegment? in
            guard let o = e as? [String: Any], let id = uint(o["id"]), id > 0 else { return nil }
            return StarredSegment(id: id, name: (o["name"] as? String)?.trimmingCharacters(in: .whitespacesAndNewlines) ?? "",
                                  distanceMeters: num(o["distance"]) ?? 0,
                                  averageGrade: num(o["average_grade"]) ?? 0,
                                  prSeconds: prSeconds(o), activityType: o["activity_type"] as? String)
        }
        return StarredSegmentPage(segments: segs, nextPage: arr.count >= pageSize ? page + 1 : nil)
    }

    /// GET /api/v3/segments/{id}: the full polyline, and the PR if the list lacked it.
    /// The fallback when a segment's streams have no latlng.
    static func parseDetail(_ data: Data) -> (polyline: String?, prSeconds: Int?) {
        guard let o = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any] else { return (nil, nil) }
        let poly = (o["map"] as? [String: Any])?["polyline"] as? String
        return (poly.flatMap { $0.isEmpty ? nil : $0 }, prSeconds(o))
    }

    /// GET /api/v3/segments/{id}/streams?keys=latlng,altitude,distance&key_by_type=true:
    /// {"latlng": {"data": [[lat, lon], ...]}, "altitude": {"data": [m, ...]}, ...},
    /// or the same streams as an array of {"type": ..., "data": [...]}.
    /// Nil without a usable latlng; the altitude only when it lines up with it.
    static func parseStreams(_ data: Data) -> (points: [LatLon], altitudes: [Double]?)? {
        guard let json = try? JSONSerialization.jsonObject(with: data) else { return nil }
        var byType: [String: [Any]] = [:]
        if let o = json as? [String: Any] {
            for (k, v) in o { if let d = (v as? [String: Any])?["data"] as? [Any] { byType[k] = d } }
        } else if let arr = json as? [Any] {
            for e in arr {
                if let s = e as? [String: Any], let t = s["type"] as? String, let d = s["data"] as? [Any] { byType[t] = d }
            }
        }
        guard let ll = byType["latlng"] else { return nil }
        let pts = ll.compactMap { e -> LatLon? in
            guard let p = e as? [Any], p.count >= 2, let la = num(p[0]), let lo = num(p[1]) else { return nil }
            return LatLon(lat: la, lon: lo)
        }
        guard pts.count >= 2, pts.count == ll.count else { return nil }
        let alt = byType["altitude"]?.compactMap { num($0) }
        return (pts, alt?.count == pts.count ? alt : nil)
    }

    /// The PR, wherever this answer put it: athlete_pr_effort (the list;
    /// pr_elapsed_time, or elapsed_time in older answers), athlete_segment_stats
    /// (the detail), or the legacy top-level pr_time.
    private static func prSeconds(_ o: [String: Any]) -> Int? {
        let effort = o["athlete_pr_effort"] as? [String: Any]
        let stats = o["athlete_segment_stats"] as? [String: Any]
        let candidates: [Any?] = [effort?["pr_elapsed_time"], effort?["elapsed_time"],
                                  stats?["pr_elapsed_time"], o["pr_time"]]
        for c in candidates {
            if let v = num(c), v > 0 { return Int(v.rounded()) }
        }
        return nil
    }

    // MARK: device

    /// The name as the device's faces can draw it: accents folded away, plain
    /// ASCII, at most `nameBytes`.
    static func deviceName(_ s: String) -> String {
        let src = s.folding(options: [.diacriticInsensitive, .widthInsensitive],
                            locale: Locale(identifier: "en_US_POSIX"))
        var out = ""
        var lastSpace = true
        for ch in src {
            guard let a = ch.asciiValue, a >= 32, a < 127 else { continue }
            if a == 32 {
                if lastSpace { continue }
                lastSpace = true
            } else {
                lastSpace = false
            }
            out.append(Character(UnicodeScalar(a)))
        }
        out = out.trimmingCharacters(in: .whitespaces)
        if out.count > nameBytes { out = String(out.prefix(nameBytes)).trimmingCharacters(in: .whitespaces) }
        return out.isEmpty ? "Segment" : out
    }

    /// The line cut to `maxPoints`, without its elevation.
    static func simplify(_ pts: [LatLon], maxPoints: Int = maxPoints) -> [LatLon] {
        simplify(pts, altitudes: nil, maxPoints: maxPoints).points
    }

    /// Cuts the line and its altitudes together, keeping the same points of
    /// both. Douglas-Peucker in three dimensions — metres east, north, and
    /// height x `elevWeight` — with the tolerance doubling from 2 m until the
    /// line fits. Why that and not points spaced evenly along it: the device
    /// matches the rider against this line, so its bends must stay where the
    /// road bends (a switchback between two even samples would be cut off), and
    /// the profile must keep its grade changes (a steep ramp on a straight
    /// road is invisible in 2D). The height weight makes a 0.5 m kink in the
    /// profile cost as much as 5 m off the road, so both survive. Drops
    /// repeated points first; keeps both ends.
    static func simplify(_ pts: [LatLon], altitudes: [Double]?, maxPoints: Int = maxPoints)
        -> (points: [LatLon], altitudes: [Double]?) {
        let alts = altitudes.flatMap { $0.count == pts.count ? $0 : nil }
        var idx: [Int] = []
        for i in pts.indices where idx.isEmpty || pts[idx.last!] != pts[i] { idx.append(i) }
        let p = idx.map { pts[$0] }
        let z = alts.map { a in idx.map { a[$0] * elevWeight } }
        guard p.count > 2 else { return (p, alts.map { a in idx.map { a[$0] } }) }
        let lat0 = p[0].lat * .pi / 180
        let kx = 111_320.0 * cos(lat0), ky = 110_540.0
        let xs = p.map { ($0.lon - p[0].lon) * kx }, ys = p.map { ($0.lat - p[0].lat) * ky }
        let zs = z ?? [Double](repeating: 0, count: p.count)
        var tol = 2.0
        while true {
            var keep = [Bool](repeating: false, count: p.count)
            keep[0] = true
            keep[p.count - 1] = true
            var stack = [(0, p.count - 1)]
            while let (a, b) = stack.popLast() {
                guard b > a + 1 else { continue }
                let dx = xs[b] - xs[a], dy = ys[b] - ys[a], dz = zs[b] - zs[a]
                let len2 = dx * dx + dy * dy + dz * dz
                var worst = -1.0, wi = a
                for i in (a + 1)..<b {
                    var t = len2 > 0 ? ((xs[i] - xs[a]) * dx + (ys[i] - ys[a]) * dy + (zs[i] - zs[a]) * dz) / len2 : 0
                    t = min(1, max(0, t))
                    let ex = xs[a] + t * dx - xs[i], ey = ys[a] + t * dy - ys[i], ez = zs[a] + t * dz - zs[i]
                    let d = ex * ex + ey * ey + ez * ez
                    if d > worst { worst = d; wi = i }
                }
                if worst > tol * tol {
                    keep[wi] = true
                    stack.append((a, wi))
                    stack.append((wi, b))
                }
            }
            let kept = p.indices.filter { keep[$0] }
            if kept.count <= maxPoints {
                return (kept.map { p[$0] }, alts.map { a in kept.map { a[idx[$0]] } })
            }
            tol *= 2
        }
    }

    /// What the device keeps of a segment's streams (or, failing those, its polyline).
    static func geometry(points: [LatLon], altitudes: [Double]?) -> SegmentGeometry {
        let cut = simplify(points, altitudes: altitudes)
        return SegmentGeometry(points: cut.points, altitudes: cut.altitudes)
    }

    /// The device's segment blob (format in src/segment_match.h). `version` 1
    /// is for firmware that predates elevation: the same lines, no heights.
    static func encode(_ segs: [DeviceSegment], version: Int = blobVersion) -> Data {
        var d = Data("OTSG".utf8)
        d.append(UInt8(version))
        d.append(0)   // flags
        le(&d, UInt16(segs.count))
        for s in segs {
            var name = Data()
            for ch in s.name {   // whole characters only
                let b = Data(String(ch).utf8)
                if name.count + b.count > nameBytes { break }
                name.append(b)
            }
            let elev = s.elevations.flatMap { $0.count == s.points.count ? $0 : nil }
            le(&d, s.id)
            le(&d, UInt32(max(0, s.prSeconds)))
            le(&d, UInt32(max(0, (s.distanceMeters * 10).rounded())))
            le(&d, UInt16(bitPattern: Int16(max(-32768, min(32767, (s.averageGrade * 10).rounded())))))
            le(&d, UInt16(s.points.count))
            if version >= 2 { d.append(elev == nil ? 0 : 1) }
            d.append(UInt8(name.count))
            d.append(name)
            for (i, q) in s.points.enumerated() {
                le(&d, UInt32(bitPattern: Int32((q.lat * 1e5).rounded())))
                le(&d, UInt32(bitPattern: Int32((q.lon * 1e5).rounded())))
                if version >= 2 {
                    let dm = elev.map { (($0[i] + elevOffsetM) * 10).rounded() } ?? 0
                    le(&d, UInt16(max(0, min(65535, dm))))
                }
            }
        }
        return d
    }

    /// The segments to send: rides only, in the order Strava listed them,
    /// each with a line of at least two points.
    static func deviceSegments(_ starred: [StarredSegment], geometry: [UInt64: SegmentGeometry],
                               prs: [UInt64: Int] = [:]) -> [DeviceSegment] {
        var out: [DeviceSegment] = []
        for s in starred where s.isRide {
            guard out.count < maxSegments, let g = geometry[s.id], g.lat.count >= 2,
                  g.lat.count <= maxPoints, g.lat.count == g.lon.count else { continue }
            out.append(DeviceSegment(id: s.id, name: deviceName(s.name), prSeconds: s.prSeconds ?? prs[s.id] ?? 0,
                                     distanceMeters: s.distanceMeters, averageGrade: s.averageGrade,
                                     points: g.points, elevations: g.alt))
        }
        return out
    }

    private static func le<T: FixedWidthInteger>(_ d: inout Data, _ v: T) {
        withUnsafeBytes(of: v.littleEndian) { d.append(contentsOf: $0) }
    }

    private static func uint(_ v: Any?) -> UInt64? {
        if let n = v as? NSNumber, CFGetTypeID(n) != CFBooleanGetTypeID() { return n.uint64Value }
        if let s = v as? String { return UInt64(s) }
        return nil
    }

    private static func num(_ v: Any?) -> Double? {
        guard let n = v as? NSNumber, CFGetTypeID(n) != CFBooleanGetTypeID() else { return nil }
        return n.doubleValue
    }
}
