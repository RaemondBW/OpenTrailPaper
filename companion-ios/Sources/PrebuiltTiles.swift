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

    /// One group's index.json: its cells and the fragments (meta.json
    /// "fragments" keys) they were built in, for the data date.
    struct Index { var cells: [String: Entry]; var regions: [String] }

    /// Group indexes, shared by every download and the Maps screen's version
    /// check for an hour (index.json's own max-age). A group answered with 404
    /// (or unusable JSON) is cached as nil: "not built".
    actor IndexCache {
        static let shared = IndexCache()
        static let ttl: TimeInterval = 3600
        private var cache: [String: (at: Date, index: Index?)] = [:]

        /// Outer nil: not cached (or expired). Inner nil: cached as "not built".
        func cached(_ url: URL) -> Index?? {
            guard let c = cache[url.absoluteString],
                  Date().timeIntervalSince(c.at) < Self.ttl else { return nil }
            return .some(c.index)
        }
        func store(_ url: URL, _ index: Index?) { cache[url.absoluteString] = (Date(), index) }
        func clear() { cache = [:] }
    }

    /// Index lookups for one download, shared by the map and POI passes so
    /// each group's index.json is fetched once. Once the host proves
    /// unreachable (DNS, refused, offline, timeout), every later lookup
    /// answers "not prebuilt" immediately — the domain not being live yet
    /// must not cost a timeout per group.
    actor Lookup {
        let base: URL?
        private var indexes: [String: Task<Index?, Never>] = [:]
        private(set) var dead = false
        private let cache: IndexCache

        init(base: URL? = PrebuiltTiles.configuredBase, cache: IndexCache = .shared) {
            self.base = base; self.cache = cache
        }

        /// Entries for the groups of `ids` (fetched concurrently). A group
        /// with no usable index is simply absent.
        func entries(for ids: [String]) async -> [String: Entry] {
            var out: [String: Entry] = [:]
            for (g, idx) in await groupIndexes(for: ids) {
                for id in ids where PrebuiltTiles.group(id) == g {
                    if let e = idx.cells[id] { out[id] = e }
                }
            }
            return out
        }

        /// The usable indexes of the groups of `ids`.
        func groupIndexes(for ids: [String]) async -> [String: Index] {
            guard let base, !dead else { return [:] }
            let groups = Set(ids.map(PrebuiltTiles.group))
            for g in groups where indexes[g] == nil {
                indexes[g] = Task { await self.fetchIndex(base: base, group: g) }
            }
            var out: [String: Index] = [:]
            for g in groups {
                if let m = await indexes[g]?.value { out[g] = m }
            }
            return out
        }

        func markDead() { dead = true }

        private func fetchIndex(base: URL, group g: String) async -> Index? {
            if dead { return nil }
            guard let url = URL(string: "\(g)/index.json", relativeTo: base) else { return nil }
            if let hit = await cache.cached(url) { return hit }
            let r = await PrebuiltTiles.get(url, kind: "cdn-index", timeout: PrebuiltTiles.indexTimeout)
            if r.unreachable { dead = true }
            let idx = (r.status == 200 ? r.data : nil).flatMap(PrebuiltTiles.parseIndexFile)
            // Only a definite answer is kept: the index, or 404 (group not
            // built). No answer or a server error is asked again next time.
            if r.status == 200 || r.status == 404 { await cache.store(url, idx) }
            return idx
        }
    }

    static func parseIndex(_ data: Data) -> [String: Entry]? { parseIndexFile(data)?.cells }

    static func parseIndexFile(_ data: Data) -> Index? {
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
        return Index(cells: out, regions: (root["regions"] as? [String]) ?? [])
    }

    /// meta.json -> fragment name -> OSM snapshot time (fragments whose
    /// timestamp is unknown are left out).
    static func parseMeta(_ data: Data) -> [String: Date] {
        guard let root = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let frags = root["fragments"] as? [String: Any] else { return [:] }
        let iso = ISO8601DateFormatter()
        var out: [String: Date] = [:]
        for (name, v) in frags {
            if let f = v as? [String: Any], let s = f["osm"] as? String, let d = iso.date(from: s) {
                out[name] = d
            }
        }
        return out
    }

    /// The OSM snapshot behind `indexes`: the oldest timestamp (meta.json)
    /// among the fragments their cells came from; nil when none is known.
    static func dataDate(_ indexes: [Index], meta: [String: Date]) -> Date? {
        indexes.flatMap(\.regions).compactMap { meta[$0] }.min()
    }

    // MARK: versions (docs/prebuilt-tiles.md#versions)
    //
    // What a device holds is recorded per hex as a version string:
    //   "cdn:<hash>"    the CDN file with that index hash (ebmHash / poiHash)
    //   "phone:<secs>"  built on the phone from Overpass at that Unix time
    // and compared with the group index: the same hash is current, anything
    // else is an update. Without an index entry (CDN unreachable, hex not
    // pre-built, or built empty) the answer is `unknown` and the caller falls
    // back to its age / bike-route heuristic.

    enum VersionState: Equatable { case current, update, unknown }

    static func cdnRecord(_ hash: String) -> String { "cdn:" + hash }
    static func phoneRecord(_ at: Date = Date()) -> String { "phone:\(Int(at.timeIntervalSince1970))" }
    /// When a "phone:" record was built (nil for CDN or unknown records).
    static func phoneBuiltAt(_ record: String?) -> Date? {
        guard let r = record, r.hasPrefix("phone:"), let t = Double(r.dropFirst(6)) else { return nil }
        return Date(timeIntervalSince1970: t)
    }
    /// The hash a device's tile is compared with: nil when the hex is not
    /// pre-built or was built empty (there is no file to send).
    static func tileHash(_ e: Entry?) -> String? {
        guard let e, e.ebmSize > 0 else { return nil }
        return e.ebmHash
    }
    /// The .poi hash ("" for a cell with no POIs, whose empty file is
    /// synthesised on the phone and recorded as "cdn:").
    static func poiHash(_ e: Entry?) -> String? { e?.poiHash }

    static func state(record: String?, cdnHash: String?) -> VersionState {
        guard let h = cdnHash else { return .unknown }
        return record == cdnRecord(h) ? .current : .update
    }

    /// Of `ids`, the ones to fetch and send: every hex whose device record is
    /// not the CDN's current hash (changed, phone-built, unknown, or a hex the
    /// CDN does not have and that is rebuilt as before).
    static func needsSend(_ ids: [String], records: [String: String],
                          hashes: [String: String]) -> [String] {
        ids.filter { state(record: records[$0], cdnHash: hashes[$0]) != .current }
    }

    /// `unreachable`: no HTTP answer that says anything about the CDN (DNS,
    /// refused, offline, TLS, timeout). `hostDown`: the host itself is gone
    /// (DNS, refused, offline) — unlike a timeout, which one slow tile on a
    /// weak link can hit without the rest being any slower.
    struct Response { var status: Int; var data: Data?; var unreachable: Bool; var hostDown = false }

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
            let hostDown = [URLError.cannotFindHost, .cannotConnectToHost, .dnsLookupFailed,
                            .notConnectedToInternet].contains(code)
            return Response(status: -1, data: nil, unreachable: unreachable, hostDown: hostDown)
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
                    // Only a host that is gone switches the CDN off mid-download;
                    // a single timed-out tile just falls back on its own.
                    if r.hostDown { await lookup.markDead() }
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
