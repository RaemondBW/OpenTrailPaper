// Host test of the iOS app's CDN tile source (companion-ios/Sources/
// PrebuiltTiles.swift), compiled with the app's MapBuilder.swift / H3Tiles.swift
// by run.sh against a builder output tree served over HTTP.
//
//   cdn_ios check  <base> <tree> <tamperedBase>   correctness checks (exit 1 on failure)
//   cdn_ios time   <base> <lat> <lon> <count>     CDN vs Overpass for the same hexes
//   cdn_ios vtree  <dir>                          write the two small trees `versions` serves
//   cdn_ios versions <baseA> <baseB> <workdir>    hash records: skip same, re-send changed,
//                                                 phone-built = update, CDN down = heuristic
import Foundation

func fail(_ m: String) -> Never { print("FAIL: \(m)"); exit(1) }
var failures = 0
func check(_ ok: Bool, _ m: String) { print("\(ok ? "ok  " : "FAIL") \(m)"); if !ok { failures += 1 } }

let a = CommandLine.arguments
guard a.count >= 2 else { fail("usage") }

func fixtureIds(_ tree: URL) -> [String: PrebuiltTiles.Entry] {
    var all: [String: PrebuiltTiles.Entry] = [:]
    let v1 = tree.appendingPathComponent("v1")
    for g in (try? FileManager.default.contentsOfDirectory(atPath: v1.path)) ?? [] where g.count == 6 {
        let d = try! Data(contentsOf: v1.appendingPathComponent("\(g)/index.json"))
        for (k, v) in PrebuiltTiles.parseIndex(d)! { all[k] = v }
    }
    return all
}

/// Hexes in the fixture's groups but not in any index, and hexes whose group
/// has no index at all (a box over Massachusetts, north of the fixture).
func outsideHexes(_ known: [String: PrebuiltTiles.Entry]) -> (inGroup: [MapTile], noGroup: [MapTile]) {
    let groups = Set(known.keys.map(PrebuiltTiles.group))
    var inGroup: [MapTile] = [], noGroup: [MapTile] = []
    for t in H3Tiles.coveringTiles(south: 41.9, west: -72.2, north: 42.6, east: -71.0) where known[t.id] == nil {
        if groups.contains(PrebuiltTiles.group(t.id)) { inGroup.append(t) } else { noGroup.append(t) }
    }
    return (inGroup, noGroup)
}

/// Four real hexes around Providence: three in one group (the `vtree`
/// index) and one more of that group that the index leaves out.
func versionHexes() -> [MapTile] {
    let all = H3Tiles.coveringTiles(south: 41.80, west: -71.45, north: 41.86, east: -71.38).sorted { $0.id < $1.id }
    let g = PrebuiltTiles.group(all[0].id)
    let same = all.filter { PrebuiltTiles.group($0.id) == g }
    guard same.count >= 4 else { fail("need 4 hexes in one group, got \(same.count)") }
    return Array(same.prefix(4))
}

func blob(_ magic: String, _ n: Int, _ seed: Int) -> Data {
    Data((0..<n).map { i in i < 4 ? Array(magic.utf8)[i] : UInt8((i * 31 + seed) & 0xff) })
}

func run(_ body: @escaping () async -> Void) {
    let sem = DispatchSemaphore(value: 0)
    Task { await body(); sem.signal() }
    sem.wait()
}

switch a[1] {
case "check":
    let base = PrebuiltTiles.baseURL(a[2])!, tree = URL(fileURLWithPath: a[3])
    let tampered = PrebuiltTiles.baseURL(a[4])!
    let known = fixtureIds(tree)
    let (inGroup, noGroup) = outsideHexes(known)
    guard !inGroup.isEmpty, !noGroup.isEmpty else { fail("no outside hexes to test with") }
    let fixture = known.keys.sorted().map { H3Tiles.tile(id: $0)! }
    run {
        // (a) every indexed hex from the CDN, byte-identical; (b) the rest fall back.
        var t0 = Date()
        let lookup = PrebuiltTiles.Lookup(base: base)
        let want = fixture + [inGroup[0], noGroup[0]]
        let r = await PrebuiltTiles.fetchTiles(want, lookup: lookup)
        let dt = Date().timeIntervalSince(t0)
        var same = 0
        for (id, d) in r.tiles {
            let f = tree.appendingPathComponent("v1/\(PrebuiltTiles.group(id))/\(id).ebm")
            if (try? Data(contentsOf: f)) == d { same += 1 }
        }
        let nonEmpty = known.values.filter { $0.ebmSize > 0 }.count
        check(r.tiles.count == nonEmpty && same == nonEmpty,
              "tiles: \(r.tiles.count) from CDN, \(same) byte-identical to the tree (\(nonEmpty) indexed) in \(String(format: "%.2f", dt)) s")
        check(r.empty.count == known.values.filter { $0.ebmSize == 0 }.count, "built-empty hexes: \(r.empty.count)")
        check(Set(r.missing.map(\.id)) == [inGroup[0].id, noGroup[0].id],
              "fallback: hex absent from its group's index (\(inGroup[0].id)) and hex with no group index (\(noGroup[0].id)) -> Overpass")
        // POIs: indexed files byte-identical, poiSize 0 synthesised like sendPois.
        let p = await PrebuiltTiles.fetchPois(want, lookup: lookup)
        var poiSame = 0
        for (id, d) in p.files {
            let e = known[id]!
            let ref: Data
            if e.poiSize > 0 {
                ref = try! Data(contentsOf: tree.appendingPathComponent("v1/\(PrebuiltTiles.group(id))/\(id).poi"))
            } else {
                let t = H3Tiles.tile(id: id)!
                ref = MapBuilder.buildPoi([], south: t.south, west: t.west, north: t.north, east: t.east,
                                          cell: id, contains: { _, _ in true })
            }
            if ref == d { poiSame += 1 }
        }
        check(p.files.count == known.count && poiSame == known.count,
              "pois: \(p.files.count) served (\(known.values.filter { $0.poiSize > 0 }.count) downloaded, rest synthesised empty), \(poiSame) identical")
        check(Set(p.missing.map(\.id)) == [inGroup[0].id, noGroup[0].id], "poi fallback for the 2 unbuilt hexes")

        // (c) tampered tree: a truncated tile and one with a wrong magic.
        let lookupT = PrebuiltTiles.Lookup(base: tampered)
        let bad = (try? String(contentsOf: tree.appendingPathComponent("tampered.txt"), encoding: .utf8))?
            .split(separator: "\n").map(String.init) ?? []
        let rt = await PrebuiltTiles.fetchTiles(fixture, lookup: lookupT)
        check(!bad.isEmpty && Set(rt.missing.map(\.id)) == Set(bad),
              "tampered: rejected \(rt.missing.map(\.id).sorted()) (expected \(bad.sorted())), \(rt.tiles.count) accepted")

        // (d) unreachable bases fall back fast, and stop asking after the first failure.
        for u in ["http://127.0.0.1:9/v1/", "https://tiles.invalid/v1/"] {
            t0 = Date()
            let l = PrebuiltTiles.Lookup(base: PrebuiltTiles.baseURL(u))
            let ru = await PrebuiltTiles.fetchTiles(fixture, lookup: l)
            let rp = await PrebuiltTiles.fetchPois(fixture, lookup: l)
            let s = Date().timeIntervalSince(t0)
            let dead = await l.dead
            check(ru.missing.count == fixture.count && rp.missing.count == fixture.count && dead && s < 10,
                  "unreachable \(u): all \(fixture.count) hexes -> Overpass in \(String(format: "%.2f", s)) s (lookup marked dead: \(dead))")
        }
        // Disabled (empty base) never touches the network.
        check(PrebuiltTiles.baseURL("") == nil, "empty OTPTileBaseURL disables the CDN")
        print(await DownloadStats.shared.summary(hexes: fixture.count))
    }
    if failures > 0 { print("\(failures) check(s) failed"); exit(1) }
    print("all CDN checks passed")

case "time":
    // The same hexes through both paths, as MapsView.download runs them.
    let base = PrebuiltTiles.baseURL(a[2])!
    let lat = Double(a[3])!, lon = Double(a[4])!, count = Int(a[5])!
    let d = 0.12
    let tiles = Array(H3Tiles.coveringTiles(south: lat - d, west: lon - d, north: lat + d, east: lon + d)
        .sorted { hypot($0.center.latitude - lat, $0.center.longitude - lon) < hypot($1.center.latitude - lat, $1.center.longitude - lon) }
        .prefix(count))
    print("hexes: \(tiles.map(\.id).joined(separator: ","))")
    run {
        await DownloadStats.shared.reset()
        var t0 = Date()
        let r = await PrebuiltTiles.fetchTiles(tiles, lookup: PrebuiltTiles.Lookup(base: base))
        let cdnS = Date().timeIntervalSince(t0)
        print(String(format: "CDN: %d tiles (%d empty, %d missing), %.2f MB in %.2f s",
                     r.tiles.count, r.empty.count, r.missing.count,
                     Double(r.tiles.reduce(0) { $0 + $1.data.count }) / 1_048_576, cdnS))
        print(await DownloadStats.shared.summary(hexes: tiles.count))

        // Overpass path: one padded coastline fetch + 0.08° batches, 2 at a
        // time, each with elevation — MapsView.download / MapBuilder.buildBatch.
        await DownloadStats.shared.reset()
        t0 = Date()
        var s = 90.0, w = 180.0, n = -90.0, e = -180.0
        for t in tiles { s = min(s, t.south); w = min(w, t.west); n = max(n, t.north); e = max(e, t.east) }
        let pad = 0.003 + 0.35
        let seaRings = Task<[[(Double, Double)]], Never> {
            let cj = try? await MapBuilder.fetchCoastline(south: s - pad, west: w - pad, north: n + pad, east: e + pad)
            let chains = cj.flatMap { try? MapBuilder.extractCoastlineChains(regionJSON: $0) } ?? []
            return MapBuilder.regionSeaPolygons(chains, south: s - pad, west: w - pad, north: n + pad, east: e + pad)
        }
        let batches = Dictionary(grouping: tiles) { t -> String in
            "\(Int((t.center.latitude / 0.08).rounded(.down)))_\(Int((t.center.longitude / 0.08).rounded(.down)))"
        }.map(\.value)
        var built: [(id: String, data: Data)] = [], errors: [String] = []
        await withTaskGroup(of: Result<[(id: String, data: Data)], Error>.self) { g in
            var next = 0
            func add() {
                guard next < batches.count else { return }
                let i = next, b = batches[i]; next += 1
                g.addTask {
                    do { return .success(try await MapBuilder.buildBatch(b, startMirror: i, seaRings: seaRings)) }
                    catch { return .failure(error) }
                }
            }
            for _ in 0..<MapBuilder.concurrentBatches { add() }
            for await res in g {
                switch res {
                case .success(let x): built += x
                case .failure(let err): errors.append(err.localizedDescription)
                }
                add()
            }
        }
        let opS = Date().timeIntervalSince(t0)
        print(String(format: "Overpass: %d of %d tiles built from %d batches in %.1f s; errors: %@",
                     built.count, tiles.count, batches.count, opS, errors.isEmpty ? "none" : errors.joined(separator: " | ")))
        print(await DownloadStats.shared.summary(hexes: tiles.count))
        // Same bytes? (Differs where live Overpass data is newer than the extract,
        // or where Open-Meteo returned no elevation.)
        let cdn = Dictionary(uniqueKeysWithValues: r.tiles.map { ($0.id, $0.data) })
        let same = built.filter { cdn[$0.id] == $0.data }.count
        print("byte-identical CDN vs phone-built: \(same) of \(built.count)")
    }
case "vtree":
    // Two published states of one group: B changes the first hex's tile.
    let dir = URL(fileURLWithPath: a[2])
    let v = versionHexes()
    let g = PrebuiltTiles.group(v[0].id)
    for (name, firstHash, seed) in [("a", "aaaa1111aaaa1111", 1), ("b", "aaaa9999aaaa9999", 9)] {
        let d = dir.appendingPathComponent("\(name)/v1/\(g)")
        try! FileManager.default.createDirectory(at: d, withIntermediateDirectories: true)
        let t0 = blob("EBM2", 1000, seed), t1 = blob("EBM2", 700, 3), t2 = blob("EBM2", 600, 4)
        let p0 = blob("EPOI", 52, 2)
        try! t0.write(to: d.appendingPathComponent("\(v[0].id).ebm"))
        try! p0.write(to: d.appendingPathComponent("\(v[0].id).poi"))
        try! t1.write(to: d.appendingPathComponent("\(v[1].id).ebm"))
        try! t2.write(to: d.appendingPathComponent("\(v[2].id).ebm"))
        let index = """
            {"v":1,"cells":{"\(v[0].id)":[1000,"\(firstHash)",52,"pppp1111pppp1111"],\
            "\(v[1].id)":[700,"bbbb2222bbbb2222",0,""],"\(v[2].id)":[600,"cccc3333cccc3333",0,""]},\
            "regions":["rhode-island","rhode-island.border"]}
            """
        try! Data(index.utf8).write(to: d.appendingPathComponent("index.json"))
        let meta = """
            {"version":"v1","fragments":{"rhode-island":{"region":"rhode-island","phase":"interior",\
            "osm":"2026-10-05T20:21:02Z"},"rhode-island.border":{"region":"rhode-island","phase":"border",\
            "osm":"2026-10-06T20:21:02Z"}}}
            """
        try! Data(meta.utf8).write(to: dir.appendingPathComponent("\(name)/v1/meta.json"))
    }
    print("wrote \(dir.path)/a and /b for group \(g)")

case "versions":
    let baseA = PrebuiltTiles.baseURL(a[2])!, baseB = PrebuiltTiles.baseURL(a[3])!
    let work = URL(fileURLWithPath: a[4])
    let v = versionHexes()
    let ids = v.map(\.id)
    // h0 sent from the CDN as A's hash; h1 sent from an older CDN build; h2
    // built on the phone; h3 is not in the index (rebuilt from Overpass).
    let phone = PrebuiltTiles.phoneRecord(Date(timeIntervalSince1970: 1_790_000_000))
    var records: [String: String] = [
        ids[0]: PrebuiltTiles.cdnRecord("aaaa1111aaaa1111"),
        ids[1]: PrebuiltTiles.cdnRecord("0ld0ld0ld0ld0ld0"),
        ids[2]: phone,
        ids[3]: phone,
    ]
    run {
        let la = PrebuiltTiles.Lookup(base: baseA)
        let ea = await la.entries(for: ids)
        let ha = ea.compactMapValues { PrebuiltTiles.tileHash($0) }
        let states = ids.map { PrebuiltTiles.state(record: records[$0], cdnHash: ha[$0]) }
        check(states == [.current, .update, .update, .unknown],
              "states vs index A: same hash current, changed hash update, phone-built update, not pre-built unknown -> \(states)")
        let need = PrebuiltTiles.needsSend(ids, records: records, hashes: ha)
        check(need == [ids[1], ids[2], ids[3]], "Redownload sends only the changed, phone-built and unknown hexes (skips \(ids[0]))")
        check(PrebuiltTiles.phoneBuiltAt(phone) == Date(timeIntervalSince1970: 1_790_000_000), "phone record keeps its build time")
        // Fetch just those: the CDN serves the two it has; the unknown one falls back.
        let r = await PrebuiltTiles.fetchTiles(need.compactMap { H3Tiles.tile(id: $0) }, lookup: la)
        check(Set(r.tiles.map(\.id)) == [ids[1], ids[2]] && r.missing.map(\.id) == [ids[3]],
              "fetch: \(r.tiles.count) from the CDN, \(r.missing.count) to Overpass, \(ids[0]) never requested")
        let served = (try? String(contentsOf: work.appendingPathComponent("http1.log"), encoding: .utf8)) ?? ""
        check(served.contains("\(ids[1]).ebm") && !served.contains("\(ids[0]).ebm"),
              "server log: the current hex's tile was never requested")
        // The device saved them: record their hashes.
        for t in r.tiles { records[t.id] = PrebuiltTiles.cdnRecord(ha[t.id]!) }
        check(PrebuiltTiles.needsSend(ids, records: records, hashes: ha) == [ids[3]],
              "after sending, only the not-pre-built hex is left for a Redownload")
        // POIs by their own hash: "" for a cell without POIs (synthesised).
        let pa = ea.compactMapValues { PrebuiltTiles.poiHash($0) }
        let poiRecords = [ids[0]: PrebuiltTiles.cdnRecord("pppp1111pppp1111"), ids[1]: PrebuiltTiles.cdnRecord(""),
                          ids[2]: phone]
        check(PrebuiltTiles.needsSend(Array(ids.prefix(3)), records: poiRecords, hashes: pa) == [ids[2]],
              "POIs: same .poi hash skipped (incl. the empty one), phone-built re-sent")

        // Next week's index (B) changes h0: it is now the only update.
        let eb = await PrebuiltTiles.Lookup(base: baseB, cache: PrebuiltTiles.IndexCache()).entries(for: ids)
        let hb = eb.compactMapValues { PrebuiltTiles.tileHash($0) }
        check(PrebuiltTiles.needsSend(Array(ids.prefix(3)), records: records, hashes: hb) == [ids[0]],
              "index B: the changed hash is re-sent, the unchanged two are skipped")

        // CDN unreachable: nothing is known, every hex is `unknown` (the age /
        // bike-route heuristic decides) and a Redownload rebuilds as before.
        let ld = PrebuiltTiles.Lookup(base: PrebuiltTiles.baseURL("http://127.0.0.1:9/v1/"))
        let ed = await ld.entries(for: ids)
        let hd = ed.compactMapValues { PrebuiltTiles.tileHash($0) }
        check(ed.isEmpty && ids.allSatisfy { PrebuiltTiles.state(record: records[$0], cdnHash: hd[$0]) == .unknown }
              && PrebuiltTiles.needsSend(ids, records: records, hashes: hd) == ids,
              "CDN unreachable: all \(ids.count) unknown -> heuristic; Redownload keeps all")

        // Index cache: a second lookup within the hour does not refetch.
        let before = await DownloadStats.shared.summary(hexes: 0)
        _ = await PrebuiltTiles.Lookup(base: baseA).entries(for: ids)
        let after = await DownloadStats.shared.summary(hexes: 0)
        check(before == after, "index.json cached across lookups (no new request)")

        // Data date: the oldest fragment of the group, from meta.json.
        let idx = await PrebuiltTiles.Lookup(base: baseA).groupIndexes(for: ids)
        let metaData = try! Data(contentsOf: URL(string: "meta.json", relativeTo: baseA)!)
        let date = PrebuiltTiles.dataDate(Array(idx.values), meta: PrebuiltTiles.parseMeta(metaData))
        check(date == ISO8601DateFormatter().date(from: "2026-10-05T20:21:02Z"),
              "data date from meta.json: \(date.map { "\($0)" } ?? "nil")")

        // The phone's tile cache, keyed by hash.
        let cacheDir = work.appendingPathComponent("tilecache")
        try? FileManager.default.removeItem(at: cacheDir)
        let cache = TileCache(dir: cacheDir)
        let d0 = blob("EBM2", 1000, 1), d2 = blob("EBM2", 600, 4)
        await cache.store([(ids[0], d0)], versions: [ids[0]: PrebuiltTiles.cdnRecord("aaaa1111aaaa1111")])
        await cache.store([(ids[2], d2)])                                   // phone-built
        let c1 = await cache.partition([ids[0], ids[2]], cdnHashes: ha)
        check(c1.cached.map(\.id) == [ids[0]] && c1.missing == [ids[2]]
              && c1.versions[ids[0]] == PrebuiltTiles.cdnRecord("aaaa1111aaaa1111"),
              "cache: copy with the current hash reused, phone-built copy of a pre-built hex refetched")
        let c2 = await cache.partition([ids[0]], cdnHashes: hb)
        check(c2.cached.isEmpty, "cache: copy with last week's hash not reused")
        let c3 = await cache.partition([ids[0], ids[2]])
        check(c3.cached.count == 2 && c3.versions[ids[2]]?.hasPrefix("phone:") == true,
              "cache: without an index entry (CDN down), any recent copy is reused")
        let c4 = await cache.partition([ids[0], ids[2]], cdnHashes: [ids[0]: ha[ids[0]]!], allowAged: false)
        check(c4.cached.map(\.id) == [ids[0]], "cache on a Redownload: only exact CDN copies")

        // Per-device records survive a restart and stay apart.
        let vf = work.appendingPathComponent("versions.json")
        try? FileManager.default.removeItem(at: vf)
        let dv = DeviceTileVersions(url: vf)
        dv.setTile("dev-A", ids[0], records[ids[0]]!)
        dv.setTile("dev-A", ids[2], phone)
        dv.setPoi("dev-A", ids[0], poiRecords[ids[0]]!)
        dv.setTile("dev-B", ids[0], phone)
        dv.pruneTiles("dev-A", keeping: [ids[0]])
        dv.save()
        let dv2 = DeviceTileVersions(url: vf)
        check(dv2.store == dv.store && dv2.tile("dev-A", ids[0]) == records[ids[0]]
              && dv2.tile("dev-B", ids[0]) == phone && dv2.tile("dev-A", ids[2]) == nil
              && dv2.poi("dev-A", ids[0]) == poiRecords[ids[0]] && dv2.tile(nil, ids[0]) == nil,
              "device records: persisted per device, pruned to the device's list")
    }
    if failures > 0 { print("\(failures) check(s) failed"); exit(1) }
    print("all version checks passed")

default:
    fail("unknown mode \(a[1])")
}
