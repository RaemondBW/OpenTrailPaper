import Foundation

// Builds an .ebm vector-map blob (the format src/map_tiles.cpp reads) from
// OpenStreetMap data for a chosen bounding box, entirely on the phone. This is
// a straight port of tools/maps/build_map.py so the output is byte-compatible
// with what the device already renders.
//
// Format (little-endian):
//   'EBM2', f64 lat0, f64 lon0, f64 tileDeg, i32 nx, i32 ny,
//   index[nx*ny] of (u32 offset, u32 length)  (0,0 = empty tile),
//   tiles: u16 polylineCount, per polyline { u8 class, u16 pointCount,
//          i16 x,y per point (meters E/N of the tile SW corner) }.
//   class: 0 arterial, 1 primary, 2 secondary, 3 tertiary, 4 minor, 5 path.
//   then an optional 'ELV1' block, a 'WTR2' water section, then a 'PRK2' park
//   section: 'WTR2'/'PRK2', u16 polygonCount, per polygon { u16 pointCount,
//          i16 x,y per point (meters E/N of the grid SW origin lat0,lon0) }.

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
      way["natural"="water"](%@,%@,%@,%@);
      way["natural"="coastline"](%@,%@,%@,%@);
      way["leisure"="park"](%@,%@,%@,%@);
      way["landuse"~"^(grass|forest|meadow|recreation_ground|cemetery|village_green)$"](%@,%@,%@,%@);
      way["natural"~"^(wood|scrub|grassland|heath)$"](%@,%@,%@,%@);
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
        // One bbox group per way[...] line in `query` (6 lines → 24 args).
        let q = String(format: query, bbox[0], bbox[1], bbox[2], bbox[3],
                       bbox[0], bbox[1], bbox[2], bbox[3],
                       bbox[0], bbox[1], bbox[2], bbox[3],
                       bbox[0], bbox[1], bbox[2], bbox[3],
                       bbox[0], bbox[1], bbox[2], bbox[3],
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

    // MARK: elevation (DEM baked into the tile so the device needs no GPS
    // altitude or phone — see the ELV1 block the device reads back).

    static let elevationGrid = 20     // gw = gh; ~20 samples over a ~7 km tile ≈ 350 m

    private struct ElevResp: Decodable { let elevation: [Double?] }

    // A gw×gh grid of int16 elevations (metres) over [s,w]-[n,e], row 0 = south,
    // west→east within a row. Sampled from Open-Meteo (free, no key, 100/req).
    static func fetchElevationGrid(south s: Double, west w: Double,
                                   north n: Double, east e: Double,
                                   n gridN: Int = elevationGrid) async throws -> [Int16] {
        var lats: [Double] = [], lons: [Double] = []
        lats.reserveCapacity(gridN * gridN)
        for i in 0..<gridN {
            let lat = s + (n - s) * Double(i) / Double(gridN - 1)
            for j in 0..<gridN {
                let lon = w + (e - w) * Double(j) / Double(gridN - 1)
                lats.append(lat); lons.append(lon)
            }
        }
        var out = [Int16](repeating: 0, count: gridN * gridN)
        var idx = 0
        while idx < lats.count {
            let end = min(idx + 100, lats.count)
            let la = lats[idx..<end].map { String(format: "%.5f", $0) }.joined(separator: ",")
            let lo = lons[idx..<end].map { String(format: "%.5f", $0) }.joined(separator: ",")
            var comp = URLComponents(string: "https://api.open-meteo.com/v1/elevation")!
            comp.queryItems = [URLQueryItem(name: "latitude", value: la),
                               URLQueryItem(name: "longitude", value: lo)]
            var req = URLRequest(url: comp.url!)
            req.timeoutInterval = 30
            let (data, resp) = try await URLSession.shared.data(for: req)
            guard (resp as? HTTPURLResponse)?.statusCode == 200 else {
                throw BuildError.overpass("elevation server error")
            }
            let decoded = try JSONDecoder().decode(ElevResp.self, from: data)
            for (k, ev) in decoded.elevation.enumerated() where idx + k < out.count {
                out[idx + k] = Int16(max(-2000, min(9000, (ev ?? 0).rounded())))
            }
            idx = end
        }
        return out
    }

    // Append an ELV1 elevation block to an already-encoded tile.
    //   'ELV1', i32 gw, i32 gh, f64 s,w,n,e, i16 elev[gh*gw]  (row 0 = south)
    static func appendElevation(to data: inout Data, south s: Double, west w: Double,
                                north n: Double, east e: Double, grid: [Int16], n gridN: Int) {
        guard grid.count == gridN * gridN else { return }
        data.append("ELV1".data(using: .ascii)!)
        data.appendI32(Int32(gridN)); data.appendI32(Int32(gridN))
        data.appendF64(s); data.appendF64(w); data.appendF64(n); data.appendF64(e)
        for v in grid { data.appendI16(v) }
    }

    // MARK: water (natural=water polygons -> WTR2 section)

    // Resolve natural=water ways to lists of (lat, lon) node coords, using the
    // shared node table (parsed once). Call once per region, then pass the
    // result to appendWater for each tile.
    static func extractWaterWays(regionJSON: Data) throws -> [[(Double, Double)]] {
        let json = try JSONDecoder().decode(OverpassJSON.self, from: regionJSON)
        var nodes: [Int: (Double, Double)] = [:]
        nodes.reserveCapacity(json.elements.count)
        var wayNodeIds: [[Int]] = []
        for el in json.elements {
            if el.type == "node", let la = el.lat, let lo = el.lon {
                nodes[el.id] = (la, lo)
            } else if el.type == "way", let nids = el.nodes,
                      el.tags?["natural"] == "water" {
                wayNodeIds.append(nids)
            }
        }
        return wayNodeIds.map { nids in nids.compactMap { nodes[$0] } }
    }

    // Parks / green areas -> PRK2 section. Must agree with mapgen.js /
    // build_map.py and the fetchOSM query.
    private static let parkLanduse: Set<String> = ["grass", "forest", "meadow", "recreation_ground", "cemetery", "village_green"]
    private static let parkNatural: Set<String> = ["wood", "scrub", "grassland", "heath"]
    private static func isPark(_ tags: [String: String]) -> Bool {
        if tags["leisure"] == "park" { return true }
        if let lu = tags["landuse"], parkLanduse.contains(lu) { return true }
        if let na = tags["natural"], parkNatural.contains(na) { return true }
        return false
    }

    // Resolve park / green-area ways to lists of (lat, lon) node coords, like
    // extractWaterWays. Call once per region, then pass to appendParks per tile.
    static func extractParkWays(regionJSON: Data) throws -> [[(Double, Double)]] {
        let json = try JSONDecoder().decode(OverpassJSON.self, from: regionJSON)
        var nodes: [Int: (Double, Double)] = [:]
        nodes.reserveCapacity(json.elements.count)
        var wayNodeIds: [[Int]] = []
        for el in json.elements {
            if el.type == "node", let la = el.lat, let lo = el.lon {
                nodes[el.id] = (la, lo)
            } else if el.type == "way", let nids = el.nodes,
                      let tags = el.tags, isPark(tags) {
                wayNodeIds.append(nids)
            }
        }
        return wayNodeIds.map { nids in nids.compactMap { nodes[$0] } }
    }

    // Resolve natural=coastline ways and assemble them into maximal chains of
    // (lat, lon), preserving direction (LAND on the LEFT, SEA on the RIGHT).
    // Call once per region, then pass the result to appendWater for each tile.
    static func extractCoastlineChains(regionJSON: Data) throws -> [[(Double, Double)]] {
        let json = try JSONDecoder().decode(OverpassJSON.self, from: regionJSON)
        var nodes: [Int: (Double, Double)] = [:]
        nodes.reserveCapacity(json.elements.count)
        var coastWays: [[Int]] = []
        for el in json.elements {
            if el.type == "node", let la = el.lat, let lo = el.lon {
                nodes[el.id] = (la, lo)
            } else if el.type == "way", let nids = el.nodes,
                      el.tags?["natural"] == "coastline" {
                coastWays.append(nids)
            }
        }
        return assembleCoastline(coastWays, nodes)
    }

    // Join coastline ways (node-id lists) into maximal chains of (lat,lon).
    // Ways are joined end-to-end; a way is reversed only to make endpoints meet.
    static func assembleCoastline(_ coastWays: [[Int]],
                                  _ nodes: [Int: (Double, Double)]) -> [[(Double, Double)]] {
        var chains: [[Int]?] = coastWays.map { $0 }
        var changed = true
        while changed {
            changed = false
            for i in 0..<chains.count {
                if chains[i] == nil { continue }
                for j in 0..<chains.count {
                    if j == i || chains[j] == nil { continue }
                    let a = chains[i]!
                    let b = chains[j]!
                    // Forward joins only. Reversing a coastline way flips its
                    // direction and thus which side is water (OSM: water on the
                    // right), which inverts the sea fill. Valid coastlines chain
                    // head-to-tail, so forward joins suffice.
                    if a.last! == b.first! {
                        chains[i] = a + b.dropFirst()
                        chains[j] = nil; changed = true
                    } else if a.first! == b.last! {
                        chains[i] = b + a.dropFirst()
                        chains[j] = nil; changed = true
                    }
                    if changed { break }
                }
                if changed { break }
            }
        }
        var out: [[(Double, Double)]] = []
        for c in chains {
            guard let c else { continue }
            let pts = c.compactMap { nodes[$0] }
            if pts.count >= 2 { out.append(pts) }
        }
        return out
    }

    // Clip segment a->b (points (lat,lon)) to the rectangle. Returns the inside
    // parameter interval (t0,t1) with 0<=t0<=t1<=1, or nil if outside.
    private static func liangBarsky(_ a: (Double, Double), _ b: (Double, Double),
                                    _ s: Double, _ w: Double, _ n: Double, _ e: Double) -> (Double, Double)? {
        let ax = a.1, ay = a.0  // x=lon, y=lat
        let bx = b.1, by = b.0
        let dx = bx - ax, dy = by - ay
        let p = [-dx, dx, -dy, dy]
        let q = [ax - w, e - ax, ay - s, n - ay]
        var t0 = 0.0, t1 = 1.0
        for i in 0..<4 {
            if p[i] == 0.0 {
                if q[i] < 0.0 { return nil }
            } else {
                let t = q[i] / p[i]
                if p[i] < 0.0 {
                    if t > t1 { return nil }
                    if t > t0 { t0 = t }
                } else {
                    if t < t0 { return nil }
                    if t < t1 { t1 = t }
                }
            }
        }
        return (t0, t1)
    }

    private static func lerp(_ a: (Double, Double), _ b: (Double, Double), _ t: Double) -> (Double, Double) {
        (a.0 + t * (b.0 - a.0), a.1 + t * (b.1 - a.1))
    }

    // Split a chain into rectangle-clipped sub-chains. Each result is
    // (points, startOnBoundary, endOnBoundary).
    private static func clipChain(_ chain: [(Double, Double)],
                                  _ s: Double, _ w: Double, _ n: Double, _ e: Double)
        -> [([(Double, Double)], Bool, Bool)] {
        var subs: [([(Double, Double)], Bool, Bool)] = []
        var cur: [(Double, Double)]? = nil
        var startB = false
        if chain.count >= 2 {
            for k in 0..<(chain.count - 1) {
                let a = chain[k], b = chain[k + 1]
                guard let lb = liangBarsky(a, b, s, w, n, e) else {
                    if let c = cur { subs.append((c, startB, false)); cur = nil }
                    continue
                }
                let (t0, t1) = lb
                // Use the exact chain endpoint when unclipped, so a shared point
                // matches bit-for-bit across adjacent segments (avoids a
                // degenerate zero-length segment from float drift in lerp).
                let p0 = t0 == 0.0 ? a : lerp(a, b, t0)
                let p1 = t1 == 1.0 ? b : lerp(a, b, t1)
                if cur == nil {
                    cur = [p0]
                    startB = t0 > 0.0
                } else if cur!.last! != p0 {
                    cur!.append(p0)
                }
                if cur!.last! != p1 { cur!.append(p1) }
                if t1 < 1.0 { subs.append((cur!, startB, true)); cur = nil }
            }
        }
        if let c = cur { subs.append((c, startB, false)) }
        return subs
    }

    // Position of a boundary point along the perimeter, CCW from the SW corner.
    private static func perimPos(_ pt: (Double, Double),
                                 _ s: Double, _ w: Double, _ n: Double, _ e: Double) -> Double {
        let lat = pt.0, lon = pt.1
        let ww = e - w, hh = n - s
        let db = abs(lat - s), dr = abs(lon - e)
        let dt = abs(lat - n), dl = abs(lon - w)
        let mn = min(min(db, dr), min(dt, dl))
        if mn == db { return lon - w }
        if mn == dr { return ww + (lat - s) }
        if mn == dt { return ww + hh + (e - lon) }
        return ww + hh + ww + (n - lat)
    }

    // Positive modulo (matches Python's % for the perimeter math).
    private static func pmod(_ a: Double, _ b: Double) -> Double {
        let r = a.truncatingRemainder(dividingBy: b)
        return r < 0 ? r + b : r
    }

    // Corner points passed walking the perimeter from fromPos to toPos, CCW
    // (increasing) or CW (decreasing).
    private static func closing(_ fromPos: Double, _ toPos: Double,
                                _ s: Double, _ w: Double, _ n: Double, _ e: Double,
                                _ ccw: Bool) -> [(Double, Double)] {
        let ww = e - w, hh = n - s
        let total = 2 * ww + 2 * hh
        let corners: [(Double, Double, Double)] = [
            (s, w, 0.0),
            (s, e, ww),
            (n, e, ww + hh),
            (n, w, ww + hh + ww),
        ]
        var res: [(Double, (Double, Double))] = []
        if ccw {
            let d = pmod(toPos - fromPos, total)
            for (clat, clon, cpos) in corners {
                let cd = pmod(cpos - fromPos, total)
                if cd > 0.0 && cd < d { res.append((cd, (clat, clon))) }
            }
        } else {
            let d = pmod(fromPos - toPos, total)
            for (clat, clon, cpos) in corners {
                let cd = pmod(fromPos - cpos, total)
                if cd > 0.0 && cd < d { res.append((cd, (clat, clon))) }
            }
        }
        res.sort { $0.0 < $1.0 }
        return res.map { $0.1 }
    }

    // Assemble SEA rings for the box [s,w,n,e] the osmcoastline way: clip every
    // chain into boundary-to-boundary sub-chains, then trace rings by following
    // each coast forward (OSM: water on the right) and, at its exit, walking the
    // box boundary CLOCKWISE (interior on the right = water) to the NEXT coast's
    // entry. Uses the real topology, so a peninsula never encloses land. Returns
    // rings as [(lat,lon), ...]. Must match build_map.py / mapgen.js.
    static func regionSeaPolygons(_ chains: [[(Double, Double)]],
                                  south s: Double, west w: Double,
                                  north n: Double, east e: Double) -> [[(Double, Double)]] {
        var subs: [[(Double, Double)]] = []
        for chain in chains {
            for (pts, sb, eb) in clipChain(chain, s, w, n, e) {
                if sb && eb && pts.count >= 2 { subs.append(pts) }
            }
        }
        if subs.isEmpty { return [] }
        let ww = e - w, hh = n - s, total = 2 * ww + 2 * hh
        let entries = subs.map { perimPos($0[0], s, w, n, e) }
        let exits = subs.map { perimPos($0[$0.count - 1], s, w, n, e) }
        var used = [Bool](repeating: false, count: subs.count)
        var rings: [[(Double, Double)]] = []
        for start in 0..<subs.count {
            if used[start] { continue }
            var ring: [(Double, Double)] = []
            var i = start
            var guardN = 0
            while !used[i] && guardN < 4 * subs.count + 8 {
                guardN += 1
                used[i] = true
                ring.append(contentsOf: subs[i])            // coast A->B (water right)
                let ex = exits[i]
                var best = -1
                var bestGap = Double.infinity
                for j in 0..<subs.count {
                    var gap = pmod(ex - entries[j], total)  // CW ex -> entry
                    if gap <= 1e-12 { gap += total }        // not the same point
                    if gap < bestGap { bestGap = gap; best = j }
                }
                ring.append(contentsOf: closing(ex, entries[best], s, w, n, e, false))  // CW
                i = best
            }
            if ring.count >= 3 { rings.append(ring) }
        }
        return rings
    }

    // Sutherland–Hodgman clip of a polygon (lat,lon) to the rectangle [s,w,n,e].
    // Needed because a region sea ring can enclose a fully-ocean tile without
    // placing any vertex inside it — the tile must still fill.
    private static func clipToBox(_ poly: [(Double, Double)],
                                  _ s: Double, _ w: Double, _ n: Double, _ e: Double) -> [(Double, Double)] {
        func clip(_ pts: [(Double, Double)],
                  _ inside: ((Double, Double)) -> Bool,
                  _ isect: ((Double, Double), (Double, Double)) -> (Double, Double)) -> [(Double, Double)] {
            if pts.isEmpty { return [] }
            var res: [(Double, Double)] = []
            let m = pts.count
            for i in 0..<m {
                let cur = pts[i]
                let prev = pts[(i + m - 1) % m]
                let curIn = inside(cur), prevIn = inside(prev)
                if curIn {
                    if !prevIn { res.append(isect(prev, cur)) }
                    res.append(cur)
                } else if prevIn {
                    res.append(isect(prev, cur))
                }
            }
            return res
        }
        var p = poly
        p = clip(p, { $0.1 >= w }, { a, b in let t = (w - a.1) / (b.1 - a.1); return (a.0 + t * (b.0 - a.0), w) })
        p = clip(p, { $0.1 <= e }, { a, b in let t = (e - a.1) / (b.1 - a.1); return (a.0 + t * (b.0 - a.0), e) })
        p = clip(p, { $0.0 >= s }, { a, b in let t = (s - a.0) / (b.0 - a.0); return (s, a.1 + t * (b.1 - a.1)) })
        p = clip(p, { $0.0 <= n }, { a, b in let t = (n - a.0) / (b.0 - a.0); return (n, a.1 + t * (b.1 - a.1)) })
        return p
    }

    // Append a WTR2 water section to an already-encoded tile (after its ELV1
    // block, if any). Mirrors the whole-region encoders: points are meters E/N
    // of the tile's snapped grid origin (lat0,lon0), RDP-simplified at
    // simplifyM, i16-clamped. A polygon is included if any of its points fall
    // in [s,w,n,e]; the whole simplified ring is stored (>= 3 points, else it
    // is skipped). Always writes the "WTR2" magic + count (0 if no polygons).
    static func appendWater(to data: inout Data, waterWays: [[(Double, Double)]],
                            seaRings: [[(Double, Double)]] = [],
                            south s: Double, west w: Double, north n: Double, east e: Double) {
        let td = tileDeg
        let midLat = (s + n) / 2
        let kx = 111320.0 * cos(midLat * .pi / 180)
        let ky = 110540.0
        let lat0 = (s / td).rounded(.down) * td
        let lon0 = (w / td).rounded(.down) * td

        var polys: [[(Int16, Int16)]] = []
        for pts in waterWays {
            let inBox = pts.contains { (lat, lon) in lat >= s && lat <= n && lon >= w && lon <= e }
            if !inBox { continue }
            // Radial decimation (NOT RDP) so closed rings survive; the implicit
            // closing point (equal to the first) drops at distance 0. The device
            // closes the ring, so we never append a closing point.
            let m = decimate(pts.map { (lat, lon) in ((lon - lon0) * kx, (lat - lat0) * ky) }, simplifyM)
            guard m.count >= 3 else { continue }
            var poly: [(Int16, Int16)] = []
            poly.reserveCapacity(m.count)
            for (x, y) in m {
                let ix = Int16(max(-32000, min(32000, Int(x.rounded()))))
                let iy = Int16(max(-32000, min(32000, Int(y.rounded()))))
                poly.append((ix, iy))
            }
            polys.append(poly)
        }

        // Coastline sea-fill: clip each region-level sea ring to this tile, then
        // project/decimate/i16 like a water polygon. Clipping (not a vertex test)
        // is required so a tile fully inside the sea still fills.
        for ring in seaRings {
            let clipped = clipToBox(ring, s, w, n, e)
            if clipped.count < 3 { continue }
            let m = decimate(clipped.map { (lat, lon) in ((lon - lon0) * kx, (lat - lat0) * ky) }, simplifyM)
            if m.count < 3 { continue }
            var poly: [(Int16, Int16)] = []
            poly.reserveCapacity(m.count)
            for (x, y) in m {
                let ix = Int16(max(-32000, min(32000, Int(x.rounded()))))
                let iy = Int16(max(-32000, min(32000, Int(y.rounded()))))
                poly.append((ix, iy))
            }
            polys.append(poly)
        }

        data.append("WTR2".data(using: .ascii)!)
        data.appendU16(UInt16(min(polys.count, 0xFFFF)))
        for poly in polys {
            data.appendU16(UInt16(min(poly.count, 0xFFFF)))
            for (x, y) in poly { data.appendI16(x); data.appendI16(y) }
        }
    }

    // Append a PRK2 park section (after the WTR2 block). Same encoding as
    // appendWater's water polygons; parks are already closed rings so there is
    // no coastline assembly. Always writes the "PRK2" magic + count.
    static func appendParks(to data: inout Data, parkWays: [[(Double, Double)]],
                            south s: Double, west w: Double, north n: Double, east e: Double) {
        let td = tileDeg
        let midLat = (s + n) / 2
        let kx = 111320.0 * cos(midLat * .pi / 180)
        let ky = 110540.0
        let lat0 = (s / td).rounded(.down) * td
        let lon0 = (w / td).rounded(.down) * td

        var polys: [[(Int16, Int16)]] = []
        for pts in parkWays {
            let inBox = pts.contains { (lat, lon) in lat >= s && lat <= n && lon >= w && lon <= e }
            if !inBox { continue }
            let m = decimate(pts.map { (lat, lon) in ((lon - lon0) * kx, (lat - lat0) * ky) }, simplifyM)
            guard m.count >= 3 else { continue }
            var poly: [(Int16, Int16)] = []
            poly.reserveCapacity(m.count)
            for (x, y) in m {
                let ix = Int16(max(-32000, min(32000, Int(x.rounded()))))
                let iy = Int16(max(-32000, min(32000, Int(y.rounded()))))
                poly.append((ix, iy))
            }
            polys.append(poly)
        }

        data.append("PRK2".data(using: .ascii)!)
        data.appendU16(UInt16(min(polys.count, 0xFFFF)))
        for poly in polys {
            data.appendU16(UInt16(min(poly.count, 0xFFFF)))
            for (x, y) in poly { data.appendI16(x); data.appendI16(y) }
        }
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

    // Road tiers (device render classes). primary/secondary/tertiary are their
    // own tiers so each can be styled + shed independently per zoom; only
    // motorway/trunk (arterial) survive at the widest zooms.
    private static let arterial:  Set<String> = ["motorway", "trunk"]
    private static let primary:   Set<String> = ["primary"]
    private static let secondary: Set<String> = ["secondary"]
    private static let tertiary:  Set<String> = ["tertiary"]
    private static let minor:     Set<String> = ["residential", "unclassified", "living_street", "pedestrian"]
    private static let path:      Set<String> = ["cycleway", "footway", "path", "track", "steps"]

    // Rail/transit is dropped; natural=water is handled separately (WTR2), so
    // neither yields a road class here. Numbering must agree with build_map.py /
    // mapgen.js and the firmware MapFeatureClass enum.
    private static func classify(_ tags: [String: String]) -> UInt8? {
        let hw = tags["highway"] ?? ""
        if hw == "footway", let f = tags["footway"], f == "sidewalk" || f == "crossing" { return nil }
        let base = hw.components(separatedBy: "_link").first ?? hw
        if arterial.contains(base) { return 0 }
        if primary.contains(base) { return 1 }
        if secondary.contains(base) { return 2 }
        if tertiary.contains(base) { return 3 }
        if minor.contains(base) { return 4 }
        if path.contains(base) { return 5 }
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
        out.append("EBM2".data(using: .ascii)!)
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

    // Radial decimation for closed rings (water): keep the first point, then
    // each point only if it's > eps metres from the last kept point. Unlike
    // RDP this survives closed rings (the implicit closing point drops at
    // distance 0).
    private static func decimate(_ points: [(Double, Double)], _ eps: Double) -> [(Double, Double)] {
        guard let first = points.first else { return points }
        var kept = [first]
        var (lx, ly) = first
        for (x, y) in points.dropFirst() {
            if hypot(x - lx, y - ly) > eps {
                kept.append((x, y))
                lx = x; ly = y
            }
        }
        return kept
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
