// Host test of the iOS app's CDN tile source (companion-ios/Sources/
// PrebuiltTiles.swift), compiled with the app's MapBuilder.swift / H3Tiles.swift
// by run.sh against a builder output tree served over HTTP.
//
//   cdn_ios check  <base> <tree> <tamperedBase>   correctness checks (exit 1 on failure)
//   cdn_ios time   <base> <lat> <lon> <count>     CDN vs Overpass for the same hexes
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
default:
    fail("unknown mode \(a[1])")
}
