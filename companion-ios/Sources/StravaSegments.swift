import Foundation

// The rider's starred Strava segments, for the device's SEGMENT page:
// parsing Strava's answers, cutting each polyline down to what the device
// keeps, and the blob the device reads (src/segment_match.h). Foundation only,
// so tools/segment_test can compile it on the Mac. Mirrors
// data/StravaSegments.kt in the Android app; both encoders are checked against
// the same bytes as the firmware's parser.

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

/// A segment as the device gets it.
struct DeviceSegment: Equatable {
    let id: UInt64
    let name: String
    let prSeconds: Int
    let distanceMeters: Double
    let averageGrade: Double
    let points: [LatLon]
}

enum StravaSegments {
    /// Strava's largest page.
    static let pageSize = 200
    /// Starred pages read at most (600 segments is far past what is synced).
    static let maxPages = 3
    /// How many go to the device. One detail request each the first time
    /// (cached after), so this also keeps a first sync inside Strava's
    /// 100-requests-per-15-minutes limit. The firmware takes up to 64.
    static let maxSegments = 50
    /// Polyline points per segment; the firmware's MAX_POINTS.
    static let maxPoints = 200
    /// Name bytes; the firmware's NAME_BYTES.
    static let nameBytes = 47

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
    static func parseDetail(_ data: Data) -> (polyline: String?, prSeconds: Int?) {
        guard let o = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any] else { return (nil, nil) }
        let poly = (o["map"] as? [String: Any])?["polyline"] as? String
        return (poly.flatMap { $0.isEmpty ? nil : $0 }, prSeconds(o))
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

    /// Douglas-Peucker in metres, the tolerance doubling from 2 m until the
    /// line fits `maxPoints`. Keeps both ends; drops repeated points first.
    static func simplify(_ pts: [LatLon], maxPoints: Int = maxPoints) -> [LatLon] {
        var p: [LatLon] = []
        for q in pts where p.last != q { p.append(q) }
        guard p.count > 2 else { return p }
        let lat0 = p[0].lat * .pi / 180
        let kx = 111_320.0 * cos(lat0), ky = 110_540.0
        let xs = p.map { ($0.lon - p[0].lon) * kx }, ys = p.map { ($0.lat - p[0].lat) * ky }
        var tol = 2.0
        while true {
            var keep = [Bool](repeating: false, count: p.count)
            keep[0] = true
            keep[p.count - 1] = true
            var stack = [(0, p.count - 1)]
            while let (a, b) = stack.popLast() {
                guard b > a + 1 else { continue }
                let dx = xs[b] - xs[a], dy = ys[b] - ys[a]
                let len2 = dx * dx + dy * dy
                var worst = -1.0, wi = a
                for i in (a + 1)..<b {
                    var t = len2 > 0 ? ((xs[i] - xs[a]) * dx + (ys[i] - ys[a]) * dy) / len2 : 0
                    t = min(1, max(0, t))
                    let ex = xs[a] + t * dx - xs[i], ey = ys[a] + t * dy - ys[i]
                    let d = ex * ex + ey * ey
                    if d > worst { worst = d; wi = i }
                }
                if worst > tol * tol {
                    keep[wi] = true
                    stack.append((a, wi))
                    stack.append((wi, b))
                }
            }
            let out = p.indices.filter { keep[$0] }.map { p[$0] }
            if out.count <= maxPoints { return out }
            tol *= 2
        }
    }

    /// The device's segment blob (format in src/segment_match.h).
    static func encode(_ segs: [DeviceSegment]) -> Data {
        var d = Data("OTSG".utf8)
        d.append(1)   // version
        d.append(0)   // flags
        le(&d, UInt16(segs.count))
        for s in segs {
            var name = Data()
            for ch in s.name {   // whole characters only
                let b = Data(String(ch).utf8)
                if name.count + b.count > nameBytes { break }
                name.append(b)
            }
            le(&d, s.id)
            le(&d, UInt32(max(0, s.prSeconds)))
            le(&d, UInt32(max(0, (s.distanceMeters * 10).rounded())))
            le(&d, UInt16(bitPattern: Int16(max(-32768, min(32767, (s.averageGrade * 10).rounded())))))
            le(&d, UInt16(s.points.count))
            d.append(UInt8(name.count))
            d.append(name)
            for q in s.points {
                le(&d, UInt32(bitPattern: Int32((q.lat * 1e5).rounded())))
                le(&d, UInt32(bitPattern: Int32((q.lon * 1e5).rounded())))
            }
        }
        return d
    }

    /// The segments to send: rides only, in the order Strava listed them,
    /// each with a polyline of at least two points.
    static func deviceSegments(_ starred: [StarredSegment], polylines: [UInt64: String],
                               prs: [UInt64: Int] = [:]) -> [DeviceSegment] {
        var out: [DeviceSegment] = []
        for s in starred where s.isRide {
            guard out.count < maxSegments, let poly = polylines[s.id],
                  let pts = CloudRoutes.decodePolyline(poly) else { continue }
            let thin = simplify(pts)
            guard thin.count >= 2 else { continue }
            out.append(DeviceSegment(id: s.id, name: deviceName(s.name), prSeconds: s.prSeconds ?? prs[s.id] ?? 0,
                                     distanceMeters: s.distanceMeters, averageGrade: s.averageGrade, points: thin))
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
