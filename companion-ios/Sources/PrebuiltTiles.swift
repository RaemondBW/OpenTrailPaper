import Foundation

// Ready-made map tiles from the CDN (docs/prebuilt-tiles.md).
//
// A server job (tools/tiles/) builds every H3 res-6 cell's .ebm and .poi from
// OSM extracts with the same code paths the phone uses, and publishes them
// under one base URL (Info.plist OTPTileBaseURL, default
// https://tiles.opentrailpaper.com/v1/):
//
//   <base><g>/index.json      g = first 6 chars of the H3 id (its res-3 parent)
//       {"v":1,"cells":{"<id>":[ebmSize,"ebmHash",poiSize,"poiHash"], …}}
//   <base><g>/<id>.ebm?v=<ebmHash>
//   <base><g>/<id>.poi?v=<poiHash>
//
// Fetching a finished tile is one small GET instead of an Overpass query, an
// elevation call and an encode, so downloads try here first and only the
// hexes the CDN does not have (no index, not in the index, or a bad file) go
// to Overpass. ebmSize 0 means "built, nothing to draw": the same outcome as
// an Overpass build that drops the tile as empty, so it is not retried there.
// poiSize 0 means the cell has no POIs; the empty .poi is synthesised locally.
enum PrebuiltTiles {
    static let defaultBase = "https://tiles.opentrailpaper.com/v1/"
    /// Parallel tile/POI GETs per download.
    static let concurrency = 6
    static let indexTimeout: TimeInterval = 8
    static let fileTimeout: TimeInterval = 20

    /// Base URL from Info.plist (OTPTileBaseURL); an empty value turns the CDN
    /// off. Missing key (e.g. a host test) = the default.
    static let configuredBase: URL? = {
        let raw = (Bundle.main.object(forInfoDictionaryKey: "OTPTileBaseURL") as? String) ?? defaultBase
        return baseURL(raw)
    }()

    static func baseURL(_ raw: String) -> URL? {
        var s = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        // An unexpanded build setting ("$(…)") counts as unset.
        if s.hasPrefix("$(") { s = defaultBase }
        guard !s.isEmpty else { return nil }
        if !s.hasSuffix("/") { s += "/" }
        return URL(string: s)
    }

    static func group(_ id: String) -> String { String(id.prefix(6)) }

    struct Entry: Equatable {
        let ebmSize: Int, ebmHash: String, poiSize: Int, poiHash: String
    }

    /// Index lookups for one download, shared by the map and POI passes so
    /// each group's index.json is fetched once. Once the host proves
    /// unreachable (DNS, refused, offline, timeout), every later lookup
    /// answers "not prebuilt" immediately — the domain not being live yet
    /// must not cost a timeout per group.
    actor Lookup {
        let base: URL?
        private var indexes: [String: Task<[String: Entry]?, Never>] = [:]
        private(set) var dead = false

        init(base: URL? = PrebuiltTiles.configuredBase) { self.base = base }

        /// Entries for the groups of `ids` (fetched concurrently). A group
        /// with no usable index is simply absent.
        func entries(for ids: [String]) async -> [String: Entry] {
            guard let base, !dead else { return [:] }
            let groups = Set(ids.map(PrebuiltTiles.group))
            for g in groups where indexes[g] == nil {
                indexes[g] = Task { await self.fetchIndex(base: base, group: g) }
            }
            var out: [String: Entry] = [:]
            for g in groups {
                guard let m = await indexes[g]?.value else { continue }
                for id in ids where PrebuiltTiles.group(id) == g {
                    if let e = m[id] { out[id] = e }
                }
            }
            return out
        }

        func markDead() { dead = true }

        private func fetchIndex(base: URL, group g: String) async -> [String: Entry]? {
            if dead { return nil }
            guard let url = URL(string: "\(g)/index.json", relativeTo: base) else { return nil }
            let r = await PrebuiltTiles.get(url, kind: "cdn-index", timeout: PrebuiltTiles.indexTimeout)
            if r.unreachable { dead = true }
            guard let data = r.data, r.status == 200 else { return nil }
            return PrebuiltTiles.parseIndex(data)
        }
    }

    static func parseIndex(_ data: Data) -> [String: Entry]? {
        guard let root = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              (root["v"] as? Int) == 1,
              let cells = root["cells"] as? [String: Any] else { return nil }
        var out: [String: Entry] = [:]
        for (id, v) in cells {
            guard let a = v as? [Any], a.count >= 4,
                  let es = a[0] as? Int, let eh = a[1] as? String,
                  let ps = a[2] as? Int, let ph = a[3] as? String else { continue }
            out[id] = Entry(ebmSize: es, ebmHash: eh, poiSize: ps, poiHash: ph)
        }
        return out
    }

    struct Response { var status: Int; var data: Data?; var unreachable: Bool }

    static func get(_ url: URL, kind: String, timeout: TimeInterval) async -> Response {
        var req = URLRequest(url: url)
        req.setValue(MapBuilder.userAgent, forHTTPHeaderField: "User-Agent")
        req.timeoutInterval = timeout
        let host = url.host ?? url.absoluteString
        let started = Date()
        do {
            let (data, resp) = try await URLSession.shared.data(for: req)
            let status = (resp as? HTTPURLResponse)?.statusCode ?? -1
            await DownloadStats.shared.request(kind, host: host, status: status,
                seconds: Date().timeIntervalSince(started), bytes: data.count)
            return Response(status: status, data: data, unreachable: false)
        } catch {
            await DownloadStats.shared.request(kind, host: host, status: -1,
                seconds: Date().timeIntervalSince(started), bytes: 0)
            let code = (error as? URLError)?.code
            let unreachable: Bool = {
                switch code {
                case .cannotFindHost, .cannotConnectToHost, .dnsLookupFailed,
                     .notConnectedToInternet, .timedOut, .secureConnectionFailed,
                     .serverCertificateUntrusted, .serverCertificateHasBadDate,
                     .serverCertificateNotYetValid, .serverCertificateHasUnknownRoot,
                     .networkConnectionLost, .internationalRoamingOff, .dataNotAllowed,
                     .appTransportSecurityRequiresSecureConnection:
                    return true
                default: return false
                }
            }()
            return Response(status: -1, data: nil, unreachable: unreachable)
        }
    }

    /// A downloaded file is usable only if it is exactly the indexed size and
    /// starts with the format's magic.
    static func valid(_ data: Data, size: Int, magic: String) -> Bool {
        data.count == size && data.count >= 4 && data.prefix(4) == Data(magic.utf8)
    }

    struct TileResult {
        var tiles: [(id: String, data: Data)] = []
        /// Built on the server and empty (nothing to draw) — not an Overpass job.
        var empty: [String] = []
        /// Not prebuilt, or the download failed/was invalid: build these on the phone.
        var missing: [MapTile] = []
    }

    /// .ebm blobs for `tiles` from the CDN, `concurrency` GETs at a time.
    static func fetchTiles(_ tiles: [MapTile], lookup: Lookup) async -> TileResult {
        var r = TileResult()
        guard !tiles.isEmpty, let base = lookup.base else { r.missing = tiles; return r }
        let started = Date()
        let entries = await lookup.entries(for: tiles.map(\.id))
        var want: [(MapTile, Entry)] = []
        for t in tiles {
            guard let e = entries[t.id] else { r.missing.append(t); continue }
            if e.ebmSize == 0 { r.empty.append(t.id) } else { want.append((t, e)) }
        }
        let got = await fetchFiles(want.map { ($0.0.id, $0.1.ebmSize, $0.1.ebmHash) },
                                   ext: "ebm", magic: "EBM2", base: base, lookup: lookup)
        for (t, _) in want {
            if let d = got[t.id] { r.tiles.append((t.id, d)) } else { r.missing.append(t) }
        }
        await DownloadStats.shared.add("cdn tiles", Date().timeIntervalSince(started))
        await DownloadStats.shared.source("cdn", r.tiles.count)
        await DownloadStats.shared.source("cdn empty", r.empty.count)
        return r
    }

    /// .poi files for `tiles` from the CDN; `missing` still need Overpass.
    static func fetchPois(_ tiles: [MapTile], lookup: Lookup) async
        -> (files: [(id: String, data: Data)], missing: [MapTile]) {
        guard !tiles.isEmpty, let base = lookup.base else { return ([], tiles) }
        let started = Date()
        let entries = await lookup.entries(for: tiles.map(\.id))
        var files: [(id: String, data: Data)] = [], missing: [MapTile] = []
        var want: [(MapTile, Entry)] = []
        for t in tiles {
            guard let e = entries[t.id] else { missing.append(t); continue }
            if e.poiSize == 0 {
                files.append((t.id, MapBuilder.buildPoi([], south: t.south, west: t.west,
                    north: t.north, east: t.east, cell: t.id, contains: { _, _ in true })))
            } else { want.append((t, e)) }
        }
        let got = await fetchFiles(want.map { ($0.0.id, $0.1.poiSize, $0.1.poiHash) },
                                   ext: "poi", magic: "EPOI", base: base, lookup: lookup)
        for (t, _) in want {
            if let d = got[t.id] { files.append((t.id, d)) } else { missing.append(t) }
        }
        await DownloadStats.shared.add("cdn pois", Date().timeIntervalSince(started))
        await DownloadStats.shared.source("cdn poi", files.count)
        return (files, missing)
    }

    private static func fetchFiles(_ items: [(id: String, size: Int, hash: String)], ext: String,
                                   magic: String, base: URL, lookup: Lookup) async -> [String: Data] {
        var out: [String: Data] = [:]
        await withTaskGroup(of: (String, Data?).self) { group in
            var it = items.makeIterator()
            func add() -> Bool {
                guard let item = it.next() else { return false }
                group.addTask {
                    if await lookup.dead { return (item.id, nil) }
                    let path = "\(PrebuiltTiles.group(item.id))/\(item.id).\(ext)?v=\(item.hash)"
                    guard let url = URL(string: path, relativeTo: base) else { return (item.id, nil) }
                    let r = await get(url, kind: "cdn", timeout: fileTimeout)
                    if r.unreachable { await lookup.markDead() }
                    guard r.status == 200, let d = r.data,
                          valid(d, size: item.size, magic: magic) else { return (item.id, nil) }
                    return (item.id, d)
                }
                return true
            }
            for _ in 0..<concurrency { if !add() { break } }
            for await (id, d) in group {
                if let d { out[id] = d }
                _ = add()
            }
        }
        return out
    }
}
