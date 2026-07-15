import Foundation

// Builds an .ebm vector-map blob (the format src/map_tiles.cpp reads) from
// OpenStreetMap data for a chosen bounding box, entirely on the phone. This is
// a straight port of tools/maps/build_map.py so the output is byte-compatible
// with what the device already renders.
//
// Format (little-endian):
//   'EBM1', f64 lat0, f64 lon0, f64 tileDeg, i32 nx, i32 ny,
//   index[nx*ny] of (u32 offset, u32 length)  (0,0 = empty tile),
//   tiles: u16 polylineCount, per polyline { u8 class, u16 pointCount,
//          i16 x,y per point (meters E/N of the tile SW corner) }.
//   class: 0 major road, 1 minor road, 2 path, 3 rail.

enum MapBuilder {
    // Public Overpass instances (verified reachable) — the main one 504s under
    // load, so we rotate through these on failure. Don't add a mirror without
    // checking it actually responds; a hung endpoint just wastes the timeout.
    static let overpassEndpoints = [
        "https://overpass-api.de/api/interpreter",
        "https://maps.mail.ru/osm/tools/overpass/api/interpreter",
    ]
    static let tileDeg = 0.02
    static let simplifyM = 3.0

    struct Progress {
        var stage: String
        var fraction: Double   // 0…1, or -1 for indeterminate
    }

    enum BuildError: Error, LocalizedError {
        case overpass(String)
        case empty
        var errorDescription: String? {
            switch self {
            case .overpass(let m): return "Map download failed: \(m)"
            case .empty: return "No roads found in that area."
            }
        }
    }

    // s,w,n,e bounding box → an .ebm blob. `progress` is called on the main actor.
    static func build(south s: Double, west w: Double, north n: Double, east e: Double,
                      progress: @escaping (Progress) -> Void) async throws -> Data {
        await MainActor.run { progress(.init(stage: "Downloading map data…", fraction: -1)) }
        let json = try await fetchOverpass(s: s, w: w, n: n, e: e)

        await MainActor.run { progress(.init(stage: "Building map…", fraction: -1)) }
        let data = try encode(json: json, s: s, w: w, n: n, e: e)
        if data.count <= headerOnly(s: s, w: w, n: n, e: e) { throw BuildError.empty }
        return data
    }

    // MARK: Overpass

    private static let query = """
    [out:json][timeout:90];
    (
      way["highway"~"^(motorway|trunk|primary|secondary|tertiary|residential|unclassified|living_street|pedestrian|cycleway|footway|path|track|steps)"](%@,%@,%@,%@);
      way["railway"~"^(rail|light_rail)$"](%@,%@,%@,%@);
    );
    out body;
    >;
    out skel qt;
    """

    private static func fetchOverpass(s: Double, w: Double, n: Double, e: Double) async throws -> OverpassJSON {
        let data = try await fetchOSM(south: s, west: w, north: n, east: e)
        do { return try JSONDecoder().decode(OverpassJSON.self, from: data) }
        catch { throw BuildError.overpass("bad response") }
    }

    // Raw Overpass JSON for a region. Tries each mirror with retry/backoff —
    // 504/timeout on the busy public instance is common, so a retry on another
    // endpoint usually succeeds. `onProgress` reports which mirror is being
    // tried so the UI never looks frozen. Honors Task cancellation.
    static func fetchOSM(south s: Double, west w: Double, north n: Double, east e: Double,
                         onProgress: (@Sendable (String) -> Void)? = nil) async throws -> Data {
        let bbox = [s, w, n, e].map { String($0) }
        let q = String(format: query, bbox[0], bbox[1], bbox[2], bbox[3],
                       bbox[0], bbox[1], bbox[2], bbox[3])
        let body = ("data=" + (q.addingPercentEncoding(withAllowedCharacters: .alphanumerics) ?? q))
            .data(using: .utf8)

        var lastStatus = 0
        var lastError: Error? = nil
        let total = overpassEndpoints.count * 2
        // Two passes over the mirror list (so a transient failure gets retried).
        for attempt in 0..<total {
            try Task.checkCancellation()
            let urlStr = overpassEndpoints[attempt % overpassEndpoints.count]
            guard let url = URL(string: urlStr) else { continue }
            let host = url.host ?? urlStr
            onProgress?("server \(host) (try \(attempt + 1)/\(total))")
            var req = URLRequest(url: url)
            req.httpMethod = "POST"
            req.httpBody = body
            req.setValue("eink-bike-gps", forHTTPHeaderField: "User-Agent")
            req.timeoutInterval = 45          // fail a hung mirror fast, move on
            do {
                let (data, resp) = try await URLSession.shared.data(for: req)
                guard let http = resp as? HTTPURLResponse else { continue }
                if http.statusCode == 200 { return data }
                lastStatus = http.statusCode
                // 429 (rate limit) / 504 (timeout) / 5xx (overload) → back off, try next mirror.
            } catch is CancellationError {
                throw CancellationError()
            } catch {
                lastError = error
            }
            try? await Task.sleep(nanoseconds: 800_000_000)
        }
        if lastStatus == 429 {
            throw BuildError.overpass("map servers are rate-limiting — wait a minute and retry")
        } else if lastStatus == 504 || lastStatus >= 500 {
            throw BuildError.overpass("map servers are busy (\(lastStatus)) — try again, or draw a smaller area")
        } else if let lastError {
            throw BuildError.overpass(lastError.localizedDescription)
        }
        throw BuildError.overpass("no response from map servers")
    }

    // Encode many H3 tiles' .ebm blobs from already-fetched region JSON, parsing
    // the (large) JSON only once. Tiles with no roads are dropped. `progress` is
    // called on the main actor as tiles are encoded.
    static func encodeTiles(regionJSON: Data, tiles: [MapTile],
                            progress: @escaping (Int, Int) -> Void = { _, _ in }
    ) throws -> [(id: String, data: Data)] {
        let json = try JSONDecoder().decode(OverpassJSON.self, from: regionJSON)
        var out: [(id: String, data: Data)] = []
        for (i, t) in tiles.enumerated() {
            let data = try encode(json: json, s: t.south, w: t.west, n: t.north, e: t.east)
            if data.count > headerOnly(s: t.south, w: t.west, n: t.north, e: t.east) {
                out.append((id: t.id, data: data))
            }
            let done = i + 1
            Task { @MainActor in progress(done, tiles.count) }
        }
        return out
    }

    // Test hook: encode a raw Overpass JSON payload (bypasses the network).
    static func encodeForTest(jsonData: Data, s: Double, w: Double, n: Double, e: Double) throws -> Data {
        let json = try JSONDecoder().decode(OverpassJSON.self, from: jsonData)
        return try encode(json: json, s: s, w: w, n: n, e: e)
    }

    private struct OverpassJSON: Decodable { let elements: [Element] }
    private struct Element: Decodable {
        let type: String
        let id: Int
        let lat: Double?
        let lon: Double?
        let nodes: [Int]?
        let tags: [String: String]?
    }

    // MARK: classify (mirrors build_map.py)

    private static let major: Set<String> = ["motorway", "trunk", "primary", "secondary"]
    private static let minor: Set<String> = ["tertiary", "residential", "unclassified", "living_street", "pedestrian"]
    private static let path:  Set<String> = ["cycleway", "footway", "path", "track", "steps"]

    private static func classify(_ tags: [String: String]) -> UInt8? {
        if tags["railway"] != nil { return 3 }
        let hw = tags["highway"] ?? ""
        if hw == "footway", let f = tags["footway"], f == "sidewalk" || f == "crossing" { return nil }
        let base = hw.components(separatedBy: "_link").first ?? hw
        if major.contains(base) { return 0 }
        if minor.contains(base) { return 1 }
        if path.contains(base) { return 2 }
        return nil
    }

    // MARK: encode

    private static func headerOnly(s: Double, w: Double, n: Double, e: Double) -> Int {
        let td = tileDeg
        let lat0 = (s / td).rounded(.down) * td
        let lon0 = (w / td).rounded(.down) * td
        let nx = Int(((e - lon0) / td).rounded(.up))
        let ny = Int(((n - lat0) / td).rounded(.up))
        return 36 + nx * ny * 8
    }

    private static func encode(json: OverpassJSON, s: Double, w: Double, n: Double, e: Double) throws -> Data {
        var nodes: [Int: (Double, Double)] = [:]
        var ways: [(UInt8, [Int])] = []
        nodes.reserveCapacity(json.elements.count)
        for el in json.elements {
            if el.type == "node", let la = el.lat, let lo = el.lon {
                nodes[el.id] = (la, lo)
            } else if el.type == "way", let nids = el.nodes,
                      let cls = classify(el.tags ?? [:]) {
                ways.append((cls, nids))
            }
        }

        let td = tileDeg
        let midLat = (s + n) / 2
        let kx = 111320.0 * cos(midLat * .pi / 180)
        let ky = 110540.0
        let lat0 = (s / td).rounded(.down) * td
        let lon0 = (w / td).rounded(.down) * td
        let nx = Int(((e - lon0) / td).rounded(.up))
        let ny = Int(((n - lat0) / td).rounded(.up))
        guard nx > 0, ny > 0 else { throw BuildError.empty }

        let tileWm = td * kx, tileHm = td * ky

        // tile key (tx,ty) -> polylines [(cls, [(x,y) tile-local meters])]
        var tiles: [Int: [(UInt8, [(Int16, Int16)])]] = [:]

        func tileOf(_ p: (Double, Double)) -> (Int, Int) {
            (Int((p.0 / tileWm).rounded(.down)), Int((p.1 / tileHm).rounded(.down)))
        }
        func emit(_ tx: Int, _ ty: Int, _ cls: UInt8, _ run: [(Double, Double)]) {
            guard run.count >= 2, tx >= 0, tx < nx, ty >= 0, ty < ny else { return }
            let ox = Double(tx) * tileWm, oy = Double(ty) * tileHm
            var pts: [(Int16, Int16)] = []
            pts.reserveCapacity(run.count)
            for (x, y) in run {
                let lx = Int16(max(-32000, min(32000, Int((x - ox).rounded()))))
                let ly = Int16(max(-32000, min(32000, Int((y - oy).rounded()))))
                if let last = pts.last, last == (lx, ly) { continue }
                pts.append((lx, ly))
            }
            if pts.count >= 2 { tiles[ty * nx + tx, default: []].append((cls, pts)) }
        }

        for (cls, nids) in ways {
            let pts = nids.compactMap { nodes[$0] }
            if pts.count < 2 { continue }
            let m = rdp(pts.map { (lat, lon) in ((lon - lon0) * kx, (lat - lat0) * ky) }, simplifyM)
            guard m.count >= 2 else { continue }
            var run = [m[0]]
            var cur = tileOf(m[0])
            for p in m.dropFirst() {
                let t = tileOf(p)
                run.append(p)
                if t != cur {
                    emit(cur.0, cur.1, cls, run)
                    run = [run[run.count - 2], p]
                    cur = t
                }
            }
            emit(cur.0, cur.1, cls, run)
        }

        // Serialize
        var out = Data()
        out.append("EBM1".data(using: .ascii)!)
        out.appendF64(lat0); out.appendF64(lon0); out.appendF64(td)
        out.appendI32(Int32(nx)); out.appendI32(Int32(ny))

        // Build each tile's blob first, then the index, then concat.
        var blobs: [Int: Data] = [:]
        for (key, polys) in tiles {
            var b = Data()
            b.appendU16(UInt16(min(polys.count, 0xFFFF)))
            for (cls, pts) in polys {
                b.append(cls)
                b.appendU16(UInt16(min(pts.count, 0xFFFF)))
                for (x, y) in pts { b.appendI16(x); b.appendI16(y) }
            }
            blobs[key] = b
        }
        var off = 36 + nx * ny * 8
        var index = Data(); index.reserveCapacity(nx * ny * 8)
        var ordered: [Data] = []
        for ty in 0..<ny {
            for tx in 0..<nx {
                if let b = blobs[ty * nx + tx], !b.isEmpty {
                    index.appendU32(UInt32(off)); index.appendU32(UInt32(b.count))
                    ordered.append(b); off += b.count
                } else {
                    index.appendU32(0); index.appendU32(0)
                }
            }
        }
        out.append(index)
        for b in ordered { out.append(b) }
        return out
    }

    // Ramer–Douglas–Peucker on projected meter coords.
    private static func rdp(_ points: [(Double, Double)], _ eps: Double) -> [(Double, Double)] {
        if points.count < 3 { return points }
        var keep = [Bool](repeating: false, count: points.count)
        keep[0] = true; keep[points.count - 1] = true
        var stack = [(0, points.count - 1)]
        while let (a, b) = stack.popLast() {
            let (ax, ay) = points[a], (bx, by) = points[b]
            let dx = bx - ax, dy = by - ay
            let norm = max(hypot(dx, dy), 1e-9)
            var worst = 0.0, wi = -1
            if a + 1 < b {
                for i in (a + 1)..<b {
                    let (px, py) = points[i]
                    let d = abs(dx * (ay - py) - dy * (ax - px)) / norm
                    if d > worst { worst = d; wi = i }
                }
            }
            if worst > eps, wi >= 0 {
                keep[wi] = true
                stack.append((a, wi)); stack.append((wi, b))
            }
        }
        return zip(points, keep).filter { $0.1 }.map { $0.0 }
    }
}

private extension Data {
    mutating func appendU16(_ v: UInt16) { append(UInt8(v & 0xFF)); append(UInt8(v >> 8)) }
    mutating func appendI16(_ v: Int16) { appendU16(UInt16(bitPattern: v)) }
    mutating func appendU32(_ v: UInt32) { for s in stride(from: 0, to: 32, by: 8) { append(UInt8((v >> s) & 0xFF)) } }
    mutating func appendI32(_ v: Int32) { appendU32(UInt32(bitPattern: v)) }
    mutating func appendF64(_ v: Double) {
        var bits = v.bitPattern
        for _ in 0..<8 { append(UInt8(bits & 0xFF)); bits >>= 8 }
    }
}
