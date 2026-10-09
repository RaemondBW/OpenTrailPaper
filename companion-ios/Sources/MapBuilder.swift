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
//
// Cycling layer (investigations/osm-pois-bike-routes.md; reference
// implementation docs/mapgen.js, which this must match byte for byte — see
// tools/map_test/run_crossport.sh):
//   * a sub-tile blob may end with exactly polylineCount bytes of way flags
//     (bits 0-1 bike-route network level, 0x04 cycleway, 0x08 bike lane);
//   * POIs are NOT in the .ebm: buildPoi() writes a separate <h3>.poi
//     ('EPOI' v1, 40-byte header, 6-byte records), from its own Overpass query.

enum MapBuilder {
    // Public Overpass instances (verified reachable) — the main one 504s under
    // load, so we rotate through these on failure. Don't add a mirror without
    // checking it actually responds; a hung endpoint just wastes the timeout.
    // private.coffee added 2026-10 (fast, current data); mail.ru stays as the
    // last resort — it answers, but took 20+ s for a query the others serve
    // in 2. kumi.systems was returning 500s and is left out.
    static let overpassEndpoints = [
        "https://overpass-api.de/api/interpreter",
        "https://overpass.private.coffee/api/interpreter",
        "https://maps.mail.ru/osm/tools/overpass/api/interpreter",
    ]

    /// Map batches fetched at once. Overpass's usage policy allows a few
    /// concurrent requests per client (overpass-api.de: 4 slots per IP); two
    /// keeps us well inside that while elevation, encoding and the BLE link
    /// overlap with the next fetch. Each batch starts on a different mirror.
    static let concurrentBatches = 2
    /// Hexes whose elevation grid is fetched at once (4 calls each). Open-Meteo
    /// weights a 100-point call as 10 and rate-limits per minute (the timing
    /// run hit 429 after ~20 calls in a burst), so calls also go through
    /// `ElevationGate`; this only overlaps the waiting.
    static let concurrentElevation = 2
    static let tileDeg = 0.02
    static let simplifyM = 3.0

    /// Sent on every Overpass request. overpass-api.de answers 406 to generic
    /// agents (curl, a bare library default), which silently pushed every
    /// fetch onto the slower mirror.
    static let userAgent: String = {
        let v = Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "dev"
        return "OpenTrailPaper/\(v) (iOS)"
    }()

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

    // `{B}` is the bbox. Bike routes: route=bicycle relations are resolved to
    // member way ids ON THE SERVER and come back as three derived `bikeroute`
    // elements (one per network level, ways = "id;id;…") — relation bodies
    // list every member of a route that may cross a continent (1.1 MB for
    // central SF against ~30 KB). `way(r.bk)` pulls in route members the
    // highway filter misses so a route has no holes. Same as mapgen.js QUERY
    // minus its POI clauses, which the apps fetch separately (poiQuery).
    private static let query = """
    [out:json][timeout:90];
    rel["route"="bicycle"]({B})->.bk;
    (
      way["highway"~"^(motorway|trunk|primary|secondary|tertiary|residential|unclassified|living_street|pedestrian|cycleway|footway|path|track|steps)"]({B});
      way["natural"="water"]({B});
      way["natural"="coastline"]({B});
      way["leisure"="park"]({B});
      way["landuse"~"^(grass|forest|meadow|recreation_ground|cemetery|village_green)$"]({B});
      way["natural"~"^(wood|scrub|grassland|heath)$"]({B});
      way(r.bk)({B});
    );
    out body;
    >;
    out skel qt;
    rel.bk["network"~"^(icn|ncn)$"]->.r3;
    way(r.r3)({B})->.w3;
    make bikeroute level=3, ways=w3.set(id());
    out;
    rel.bk["network"="rcn"]->.r2;
    way(r.r2)({B})->.w2;
    make bikeroute level=2, ways=w2.set(id());
    out;
    (rel.bk; - rel.bk["network"~"^(icn|ncn|rcn)$"];)->.r1;
    way(r.r1)({B})->.w1;
    make bikeroute level=1, ways=w1.set(id());
    out;
    """

    // Cycling POIs on their own, so they can be refreshed without rebuilding
    // map tiles. `out tags center`: nodes come with lat/lon, outlines with the
    // server's centre, and no geometry is downloaded (~180 KB for a 15 km box
    // over San Francisco against ~30 MB for the map query).
    private static let poiQuery = """
    [out:json][timeout:60];
    (
      node["amenity"~"^(drinking_water|toilets|bicycle_repair_station)$"]({B});
      node["man_made"="water_tap"]["drinking_water"="yes"]({B});
      node["amenity"="fountain"]["drinking_water"="yes"]({B});
      nwr["shop"="bicycle"]({B});
      way["amenity"~"^(toilets|bicycle_repair_station)$"]({B});
    );
    out tags center;
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
    /// Coastline ways alone, over a deliberately generous bbox.
    ///
    /// Sea fill needs the COAST, and a selection out in open water does not
    /// contain any: the batch bbox for a set of ocean-only hexes has no
    /// coastline in it at all, so no rings could be assembled and those tiles
    /// came out blank. Fetching coastline on its own lets the box be padded far
    /// past the tiles without dragging in every road for that wider area.
    static func fetchCoastline(south s: Double, west w: Double,
                               north n: Double, east e: Double) async throws -> Data {
        let q = "[out:json][timeout:60];"
              + "way[\"natural\"=\"coastline\"](\(s),\(w),\(n),\(e));"
              + "(._;>;);out body;"
        return try await overpassPost(q, startMirror: 1, kind: "coast")
    }

    static func fetchOSM(south s: Double, west w: Double, north n: Double, east e: Double,
                         startMirror: Int = 0,
                         onProgress: (@Sendable (String) -> Void)? = nil) async throws -> Data {
        let bbox = [s, w, n, e].map { String($0) }.joined(separator: ",")
        let q = query.replacingOccurrences(of: "{B}", with: bbox)
        return try await overpassPost(q, startMirror: startMirror, kind: "map", onProgress: onProgress)
    }

    /// Raw Overpass JSON of the cycling POIs in a box (see poiQuery).
    static func fetchPOIs(south s: Double, west w: Double, north n: Double, east e: Double,
                          onProgress: (@Sendable (String) -> Void)? = nil) async throws -> Data {
        let bbox = [s, w, n, e].map { String($0) }.joined(separator: ",")
        let q = poiQuery.replacingOccurrences(of: "{B}", with: bbox)
        return try await overpassPost(q, startMirror: 1, kind: "poi", onProgress: onProgress)
    }

    /// POST an Overpass QL query, walking the mirror list twice so a transient
    /// failure on one server is retried elsewhere. Shared by every fetch —
    /// this retry logic used to live inside fetchOSM, so any new query either
    /// duplicated it or went without.
    private static func overpassPost(_ q: String, startMirror: Int = 0, kind: String = "map",
                                     onProgress: (@Sendable (String) -> Void)? = nil
    ) async throws -> Data {
        let body = ("data=" + (q.addingPercentEncoding(withAllowedCharacters: .alphanumerics) ?? q))
            .data(using: .utf8)

        var lastStatus = 0
        var lastError: Error? = nil
        let total = overpassEndpoints.count * 2
        // Two passes over the mirror list (so a transient failure gets retried).
        for attempt in 0..<total {
            try Task.checkCancellation()
            let urlStr = overpassEndpoints[(startMirror + attempt) % overpassEndpoints.count]
            guard let url = URL(string: urlStr) else { continue }
            let host = url.host ?? urlStr
            onProgress?("server \(host) (try \(attempt + 1)/\(total))")
            var req = URLRequest(url: url)
            req.httpMethod = "POST"
            req.httpBody = body
            req.setValue(userAgent, forHTTPHeaderField: "User-Agent")
            req.timeoutInterval = 45          // fail a hung mirror fast, move on
            let started = Date()
            do {
                let (data, resp) = try await URLSession.shared.data(for: req)
                guard let http = resp as? HTTPURLResponse else { continue }
                await DownloadStats.shared.request(kind, host: host, status: http.statusCode,
                    seconds: Date().timeIntervalSince(started), bytes: data.count)
                if http.statusCode == 200 { return data }
                lastStatus = http.statusCode
                // 429 (rate limit) / 504 (timeout) / 5xx (overload) → back off, try next mirror.
            } catch is CancellationError {
                throw CancellationError()
            } catch {
                await DownloadStats.shared.request(kind, host: host, status: -1,
                    seconds: Date().timeIntervalSince(started), bytes: 0)
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
            // Emit EVERY tile, including ones with no roads. Water, sea rings,
            // parks and elevation are appended by the caller AFTER this, so
            // dropping a road-empty tile here threw away the only chance an
            // all-water or all-park hex had to become anything — which is why a
            // bay hex with no streets in it never saved as water. The caller
            // decides what is empty once everything has been appended.
            out.append((id: t.id, data: data))
            let done = i + 1
            Task { @MainActor in progress(done, tiles.count) }
        }
        return out
    }

    /// Fetch, encode and finish one batch of hexes: roads (+ way flags), DEM
    /// elevation, water and sea fill, parks. Returns the non-empty tiles.
    /// Runs off the main actor; the Maps screen keeps `concurrentBatches` of
    /// these in flight. `seaRings` is awaited only after the fetch, so the
    /// shared coastline download overlaps with the first batches.
    static func buildBatch(_ batch: [MapTile], startMirror: Int,
                           seaRings: Task<[[(Double, Double)]], Never>,
                           onProgress: (@Sendable (String) -> Void)? = nil
    ) async throws -> [(id: String, data: Data)] {
        var s = 90.0, w = 180.0, n = -90.0, e = -180.0
        for t in batch { s = min(s, t.south); w = min(w, t.west); n = max(n, t.north); e = max(e, t.east) }
        let pad = 0.003
        let json = try await fetchOSM(south: s - pad, west: w - pad, north: n + pad, east: e + pad,
                                      startMirror: startMirror, onProgress: onProgress)
        let t0 = Date()
        let part = try encodeTiles(regionJSON: json, tiles: batch)
        let waterWays = (try? extractWaterWays(regionJSON: json)) ?? []
        let parkWays = (try? extractParkWays(regionJSON: json)) ?? []
        await DownloadStats.shared.add("encode", Date().timeIntervalSince(t0))
        let rings = await seaRings.value
        // Elevation for a few hexes at a time; best-effort, as before.
        var grids: [String: [Int16]] = [:]
        try await withThrowingTaskGroup(of: (String, [Int16]?).self) { group in
            var it = batch.makeIterator()
            var running = 0
            while running < concurrentElevation, let t = it.next() {
                group.addTask { (t.id, try? await fetchElevationGrid(south: t.south, west: t.west,
                                                                     north: t.north, east: t.east)) }
                running += 1
            }
            for try await (id, grid) in group {
                if let grid { grids[id] = grid }
                if let t = it.next() {
                    group.addTask { (t.id, try? await fetchElevationGrid(south: t.south, west: t.west,
                                                                         north: t.north, east: t.east)) }
                }
            }
        }
        let t1 = Date()
        var out: [(id: String, data: Data)] = []
        for var p in part {
            guard let t = batch.first(where: { $0.id == p.id }) else { continue }
            if let grid = grids[t.id] {
                appendElevation(to: &p.data, south: t.south, west: t.west, north: t.north,
                                east: t.east, grid: grid, n: elevationGrid)
            }
            // WTR2 water section after any ELV1 block, then PRK2.
            appendWater(to: &p.data, waterWays: waterWays, seaRings: rings,
                        south: t.south, west: t.west, north: t.north, east: t.east)
            appendParks(to: &p.data, parkWays: parkWays,
                        south: t.south, west: t.west, north: t.north, east: t.east)
            // Decide emptiness only now — a hex can be pure water and still
            // be worth storing.
            if isEmpty(p.data, tile: t) { continue }
            out.append(p)
        }
        await DownloadStats.shared.add("encode", Date().timeIntervalSince(t1))
        return out
    }

    // Test hook: encode a raw Overpass JSON payload (bypasses the network).
    static func encodeForTest(jsonData: Data, s: Double, w: Double, n: Double, e: Double,
                              cycling: Bool = true) throws -> Data {
        let json = try JSONDecoder().decode(OverpassJSON.self, from: jsonData)
        return try encode(json: json, s: s, w: w, n: n, e: e, cycling: cycling)
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
        // The 100-point calls (4 per hex) go out together rather than one
        // after another: each is ~0.3 s of mostly waiting.
        let started = Date()
        var out = [Int16](repeating: 0, count: gridN * gridN)
        let chunks = stride(from: 0, to: lats.count, by: 100).map { ($0, min($0 + 100, lats.count)) }
        try await withThrowingTaskGroup(of: (Int, [Double?]).self) { group in
            for (idx, end) in chunks {
                let la = lats[idx..<end].map { String(format: "%.5f", $0) }.joined(separator: ",")
                let lo = lons[idx..<end].map { String(format: "%.5f", $0) }.joined(separator: ",")
                group.addTask {
                    await ElevationGate.shared.wait()
                    var comp = URLComponents(string: "https://api.open-meteo.com/v1/elevation")!
                    comp.queryItems = [URLQueryItem(name: "latitude", value: la),
                                       URLQueryItem(name: "longitude", value: lo)]
                    var req = URLRequest(url: comp.url!)
                    req.timeoutInterval = 30
                    req.setValue(userAgent, forHTTPHeaderField: "User-Agent")
                    var (data, resp) = try await URLSession.shared.data(for: req)
                    if (resp as? HTTPURLResponse)?.statusCode == 429 {
                        // Rate-limited: one retry after the minute window moves.
                        try await Task.sleep(nanoseconds: 20_000_000_000)
                        await ElevationGate.shared.wait()
                        (data, resp) = try await URLSession.shared.data(for: req)
                    }
                    guard (resp as? HTTPURLResponse)?.statusCode == 200 else {
                        throw BuildError.overpass("elevation server error")
                    }
                    return (idx, try JSONDecoder().decode(ElevResp.self, from: data).elevation)
                }
            }
            for try await (idx, elevation) in group {
                for (k, ev) in elevation.enumerated() where idx + k < out.count {
                    out[idx + k] = Int16(max(-2000, min(9000, (ev ?? 0).rounded())))
                }
            }
        }
        await DownloadStats.shared.add("elevation", Date().timeIntervalSince(started))
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
    /// True when a finished tile carries nothing but its header — no roads, no
    /// water, no parks, no elevation. Only then is it not worth storing.
    static func isEmpty(_ data: Data, tile t: MapTile) -> Bool {
        data.count <= headerOnly(s: t.south, w: t.west, n: t.north, e: t.east)
    }

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
    private struct Center: Decodable { let lat: Double; let lon: Double }
    private struct Element: Decodable {
        let type: String
        let id: Int
        let lat: Double?
        let lon: Double?
        let nodes: [Int]?
        let tags: [String: String]?
        let center: Center?     // `out center` (the POI query)
    }

    // MARK: cycling layer (mirrors docs/mapgen.js — keep in step)

    static let wayRouteMask: UInt8 = 0x03   // 0 none, 1 local, 2 regional, 3 national/intl
    static let wayCycleway: UInt8 = 0x04
    static let wayBikeLane: UInt8 = 0x08
    private static let laneValues: Set<String> = ["lane", "track", "opposite_lane", "opposite_track"]
    private static let pathLike: Set<String> = ["path", "footway", "bridleway", "track", "pedestrian"]

    /// Flags for a way from its own tags plus its best route-network level.
    static func wayFlags(_ tags: [String: String], routeLevel: UInt8) -> UInt8 {
        var f = routeLevel & wayRouteMask
        let hw = tags["highway"] ?? ""
        if hw == "cycleway" || (pathLike.contains(hw) && tags["bicycle"] == "designated") {
            f |= wayCycleway
        }
        for k in ["cycleway", "cycleway:both", "cycleway:left", "cycleway:right"] {
            if laneValues.contains(tags[k] ?? "") { f |= wayBikeLane; break }
        }
        return f
    }

    /// A route member the highway filter does not classify still has to draw.
    private static func classifyRouteMember(_ tags: [String: String]) -> UInt8? {
        let hw = tags["highway"] ?? ""
        if hw.isEmpty || hw == "proposed" || hw == "construction" || hw == "platform" { return nil }
        return hw == "bridleway" || hw == "corridor" ? 5 : 4
    }

    /// way id -> highest bike-route level, from the derived `bikeroute` elements.
    private static func routeLevels(_ json: OverpassJSON) -> [Int: UInt8] {
        var out: [Int: UInt8] = [:]
        for el in json.elements where el.type == "bikeroute" {
            guard let ways = el.tags?["ways"], !ways.isEmpty else { continue }
            let lvl = UInt8((Int(el.tags?["level"] ?? "") ?? 0) & 3)
            for part in ways.split(separator: ";", omittingEmptySubsequences: false) {
                guard let k = Int(part), k != 0 else { continue }
                if (out[k] ?? 0) < lvl { out[k] = lvl }
            }
        }
        return out
    }

    // POI types / flags — the on-file bytes (src/map_view.h MapPoiType).
    static let poiWater: UInt8 = 1, poiToilets: UInt8 = 2, poiRepair: UInt8 = 3, poiBikeShop: UInt8 = 4

    /// OSM tags -> (type, flags), or nil if this is not a POI we keep.
    static func poiOf(_ tags: [String: String]) -> (UInt8, UInt8)? {
        let a = tags["amenity"] ?? ""
        func yes(_ k: String) -> Bool { let v = tags[k]; return v == "yes" || v == "only" }
        let acc = tags["access"] ?? ""
        if acc == "private" || acc == "no" { return nil }
        var restricted = acc == "customers" || acc == "permissive_customers" ||
            tags["fee"] == "yes" || ((tags["seasonal"] ?? "").isEmpty == false && tags["seasonal"] != "no")
        var type: UInt8 = 0, f: UInt8 = 0
        if a == "drinking_water" ||
            ((tags["man_made"] == "water_tap" || a == "fountain") && tags["drinking_water"] == "yes") {
            if tags["drinking_water"] == "no" { return nil }
            type = poiWater
        } else if a == "toilets" {
            type = poiToilets
            if tags["drinking_water"] == "yes" { f |= 0x01 }
        } else if a == "bicycle_repair_station" {
            type = poiRepair
            if yes("service:bicycle:pump") { f |= 0x01 }
            if yes("service:bicycle:tools") { f |= 0x02 }
            if yes("service:bicycle:chain_tool") { f |= 0x04 }
            if yes("service:bicycle:stand") { f |= 0x08 }
        } else if tags["shop"] == "bicycle" {
            type = poiBikeShop
            if yes("service:bicycle:pump") { f |= 0x01 }
            if yes("service:bicycle:repair") || yes("service:bicycle:diy") { f |= 0x02 }
            if yes("service:bicycle:rental") { f |= 0x04 }
            if yes("service:bicycle:retail") || yes("service:bicycle:parts") { f |= 0x08 }
            if yes("service:bicycle:second_hand") { f |= 0x10 }
            if yes("service:bicycle:ebike") || yes("service:bicycle:charging") { f |= 0x20 }
            restricted = false   // a shop is "customers only" by nature
        } else {
            return nil
        }
        if restricted { f |= 0x80 }
        return (type, f)
    }

    struct Poi { let type: UInt8; let flags: UInt8; let lat: Double; let lon: Double }

    /// Every POI in an Overpass response (the POI query, or a combined one), in
    /// the order mapgen.js collectPois produces: keys n/w/r<id> in element
    /// order, outlines without a `center` placed at their vertex centroid last.
    static func collectPois(regionJSON: Data) throws -> [Poi] {
        let json = try JSONDecoder().decode(OverpassJSON.self, from: regionJSON)
        var nodes: [Int: (Double, Double)] = [:]
        var order: [String] = []
        var byKey: [String: Poi] = [:]
        func put(_ key: String, _ p: Poi) {
            if byKey[key] == nil { order.append(key) }
            byKey[key] = p
        }
        var outlines: [(String, (UInt8, UInt8), [Int])] = []
        for el in json.elements {
            if el.type == "node", let la = el.lat, let lo = el.lon {
                nodes[el.id] = (la, lo)
                if let t = el.tags, let p = poiOf(t) { put("n\(el.id)", Poi(type: p.0, flags: p.1, lat: la, lon: lo)) }
            } else if el.type == "way" || el.type == "relation", let t = el.tags, let p = poiOf(t) {
                let key = (el.type == "way" ? "w" : "r") + "\(el.id)"
                if let c = el.center {
                    put(key, Poi(type: p.0, flags: p.1, lat: c.lat, lon: c.lon))
                } else if el.type == "way", let nids = el.nodes {
                    outlines.append((key, p, nids))
                }
            }
        }
        for (key, p, nids) in outlines {
            var la = 0.0, lo = 0.0, k = 0
            let ring = nids.count > 1 && nids.first == nids.last ? Array(nids.dropLast()) : nids
            for id in ring { if let q = nodes[id] { la += q.0; lo += q.1; k += 1 } }
            if k > 0 { put(key, Poi(type: p.0, flags: p.1, lat: la / Double(k), lon: lo / Double(k))) }
        }
        return order.compactMap { byKey[$0] }
    }

    /// Python/JS-style round-half-to-even (mapgen.js pyRound), used by the .poi
    /// writer so its bytes match the website's exactly.
    private static func pyRound(_ x: Double) -> Int { Int(x.rounded(.toNearestOrEven)) }

    /// One tile's `.poi` file (format: src/map_tiles.cpp poiFileValid). Keeps
    /// the POIs `contains(lat, lon)` accepts — pass the H3 cell test so each
    /// POI lives in exactly one tile — or, without it, those in the bbox.
    /// Always returns a file: an empty one means "built, nothing here".
    static func buildPoi(_ pois: [Poi], south s: Double, west w: Double, north n: Double,
                         east e: Double, cell: String?,
                         contains: ((Double, Double) -> Bool)? = nil) -> Data {
        let lat0 = (s + n) / 2, lon0 = (w + e) / 2
        let kx = 111320.0 * cos((lat0 * Double.pi) / 180), ky = 110540.0
        let fk = Double(Float(kx)), fky = Double(Float(ky))
        var list: [(UInt8, UInt8, Int, Int)] = []
        for p in pois {
            let inside = contains.map { $0(p.lat, p.lon) }
                ?? (p.lat >= s && p.lat <= n && p.lon >= w && p.lon <= e)
            if !inside { continue }
            let x = max(-32000, min(32000, pyRound((p.lon - lon0) * fk)))
            let y = max(-32000, min(32000, pyRound((p.lat - lat0) * fky)))
            list.append((p.type, p.flags, x, y))
        }
        // Stable order (y, x, type), as mapgen.js sorts.
        list = list.enumerated().sorted { a, b in
            if a.element.3 != b.element.3 { return a.element.3 < b.element.3 }
            if a.element.2 != b.element.2 { return a.element.2 < b.element.2 }
            if a.element.0 != b.element.0 { return a.element.0 < b.element.0 }
            return a.offset < b.offset
        }.map(\.element)
        if list.count > 0xFFFF { list = Array(list.prefix(0xFFFF)) }
        var out = Data()
        out.append("EPOI".data(using: .ascii)!)
        out.append(1)          // version
        out.append(6)          // record size
        out.appendU16(UInt16(list.count))
        let c = cell.flatMap { UInt64($0, radix: 16) } ?? 0
        out.appendU32(UInt32(c & 0xFFFF_FFFF)); out.appendU32(UInt32(c >> 32))
        out.appendF64(lat0); out.appendF64(lon0)
        out.appendU32(Float(kx).bitPattern); out.appendU32(Float(ky).bitPattern)
        for (t, f, x, y) in list {
            out.append(t); out.append(f)
            out.appendI16(Int16(x)); out.appendI16(Int16(y))
        }
        return out
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

    // `cycling: false` writes the pre-cycling bytes (no way-flag trailers).
    private static func encode(json: OverpassJSON, s: Double, w: Double, n: Double, e: Double,
                               cycling: Bool = true) throws -> Data {
        var nodes: [Int: (Double, Double)] = [:]
        var ways: [(UInt8, [Int], UInt8)] = []
        nodes.reserveCapacity(json.elements.count)
        let levels = cycling ? routeLevels(json) : [:]
        var seenWays = Set<Int>()   // a way can arrive twice (highway filter + route member)
        for el in json.elements {
            if el.type == "node", let la = el.lat, let lo = el.lon {
                nodes[el.id] = (la, lo)
            } else if el.type == "way", let nids = el.nodes {
                if !seenWays.insert(el.id).inserted { continue }
                let tags = el.tags ?? [:]
                if cycling && poiOf(tags) != nil { continue }   // a POI outline, not a map feature
                let lvl = levels[el.id] ?? 0
                var cls = classify(tags)
                if cls == nil && lvl != 0 { cls = classifyRouteMember(tags) }
                if let cls { ways.append((cls, nids, cycling ? wayFlags(tags, routeLevel: lvl) : 0)) }
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

        // tile key (tx,ty) -> polylines [(cls, [(x,y) tile-local meters], flags)]
        var tiles: [Int: [(UInt8, [(Int16, Int16)], UInt8)]] = [:]

        func tileOf(_ p: (Double, Double)) -> (Int, Int) {
            (Int((p.0 / tileWm).rounded(.down)), Int((p.1 / tileHm).rounded(.down)))
        }
        func emit(_ tx: Int, _ ty: Int, _ cls: UInt8, _ run: [(Double, Double)], _ flags: UInt8) {
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
            if pts.count >= 2 { tiles[ty * nx + tx, default: []].append((cls, pts, flags)) }
        }

        for (cls, nids, flags) in ways {
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
                    emit(cur.0, cur.1, cls, run, flags)
                    run = [run[run.count - 2], p]
                    cur = t
                }
            }
            emit(cur.0, cur.1, cls, run, flags)
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
            for (cls, pts, _) in polys {
                b.append(cls)
                b.appendU16(UInt16(min(pts.count, 0xFFFF)))
                for (x, y) in pts { b.appendI16(x); b.appendI16(y) }
            }
            // Way-flag trailer: exactly one byte per polyline, only when one is set.
            if polys.contains(where: { $0.2 != 0 }) {
                for p in polys.prefix(0xFFFF) { b.append(p.2) }
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

/// Where a map download's time goes, logged at the end of each run (Console,
/// subsystem com.raemond.opentrailpaper, category maps) so a slow download on
/// a real phone can be broken down: Overpass per request, elevation, encode,
/// and the BLE drain after the last tile is built.
actor DownloadStats {
    static let shared = DownloadStats()
    private var started = Date()
    private var requests: [(kind: String, host: String, status: Int, seconds: Double, bytes: Int)] = []
    private var stages: [String: Double] = [:]

    func reset() { started = Date(); requests = []; stages = [:] }
    func request(_ kind: String, host: String, status: Int, seconds: Double, bytes: Int) {
        requests.append((kind, host, status, seconds, bytes))
    }
    func add(_ stage: String, _ seconds: Double) { stages[stage, default: 0] += seconds }

    func summary(hexes: Int) -> String {
        var lines = ["map download: \(hexes) hexes in \(String(format: "%.1f", Date().timeIntervalSince(started))) s"]
        for kind in ["map", "coast", "poi"] {
            let rs = requests.filter { $0.kind == kind }
            guard !rs.isEmpty else { continue }
            let ok = rs.filter { $0.status == 200 }
            lines.append(String(format: "  overpass %@: %d requests (%d failed), %.1f s summed, %.1f MB, hosts %@",
                                kind, rs.count, rs.count - ok.count, rs.reduce(0) { $0 + $1.seconds },
                                Double(ok.reduce(0) { $0 + $1.bytes }) / 1_048_576,
                                Set(ok.map(\.host)).sorted().joined(separator: ",")))
        }
        for (k, v) in stages.sorted(by: { $0.key < $1.key }) {
            lines.append(String(format: "  %@: %.1f s summed", k, v))
        }
        return lines.joined(separator: "\n")
    }
}

/// Spaces Open-Meteo elevation calls at least `interval` apart across the
/// whole app. Each 100-point call counts as 10 against its per-minute limit
/// (600), so 1.1 s apart keeps a long download at ~550/min instead of
/// bursting into 429s — which used to leave tiles silently without elevation.
actor ElevationGate {
    static let shared = ElevationGate()
    private let interval: TimeInterval = 1.1
    private var next = Date.distantPast
    func wait() async {
        let now = Date()
        let at = max(now, next)
        next = at.addingTimeInterval(interval)
        let delay = at.timeIntervalSince(now)
        if delay > 0 { try? await Task.sleep(nanoseconds: UInt64(delay * 1_000_000_000)) }
    }
}
