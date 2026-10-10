// Equivalence test, step 2: the iOS app's own code (MapBuilder.swift +
// H3Tiles.swift, compiled for the host) building each cell exactly as
// MapsView.download / MapBuilder.buildBatch / MapsView.sendPois do for a
// one-hex selection, from the Overpass JSON dump_overpass.mjs wrote.
//
//   apptile <dumpdir> <outdir>
// Writes <outdir>/<id>.ebm and <outdir>/<id>.poi, and checks the app's cell
// bbox (H3Tiles.tile(id:)) against tiles.txt (the builder's h3tool).
import Foundation

let args = CommandLine.arguments
guard args.count == 3 else {
    FileHandle.standardError.write("usage: apptile <dumpdir> <outdir>\n".data(using: .utf8)!)
    exit(2)
}
let dir = URL(fileURLWithPath: args[1]), out = URL(fileURLWithPath: args[2])
try FileManager.default.createDirectory(at: out, withIntermediateDirectories: true)
let lines = try String(contentsOf: dir.appendingPathComponent("tiles.txt"), encoding: .utf8)
    .split(separator: "\n").map { $0.split(separator: " ").map(String.init) }

var bboxMismatch = 0
for l in lines where l.count == 5 {
    let id = l[0]
    guard let t = H3Tiles.tile(id: id) else { print("BAD ID \(id)"); exit(1) }
    if t.south != Double(l[1])! || t.west != Double(l[2])! || t.north != Double(l[3])! || t.east != Double(l[4])! {
        bboxMismatch += 1
        print("BBOX DIFF \(id): app \(t.south) \(t.west) \(t.north) \(t.east) vs tiles.txt \(l[1...4].joined(separator: " "))")
    }
    let map = try Data(contentsOf: dir.appendingPathComponent("\(id).map.json"))
    let coast = try Data(contentsOf: dir.appendingPathComponent("\(id).coast.json"))
    let poiJSON = try Data(contentsOf: dir.appendingPathComponent("\(id).poi.json"))
    let elev = try String(contentsOf: dir.appendingPathComponent("\(id).elev.txt"), encoding: .utf8)
        .split(whereSeparator: { $0 == " " || $0 == "\n" }).map { Int16($0)! }

    // MapsView.download: one coastline fetch over union(selection) padded
    // 0.35°, rings assembled against that padded box.
    let pad = 0.003, cpad = 0.35
    let all = (s: t.south - pad, w: t.west - pad, n: t.north + pad, e: t.east + pad)
    let chains = (try? MapBuilder.extractCoastlineChains(regionJSON: coast)) ?? []
    let rings = MapBuilder.regionSeaPolygons(chains, south: all.s - cpad, west: all.w - cpad,
                                             north: all.n + cpad, east: all.e + cpad)
    // MapBuilder.buildBatch for the batch [t].
    var p = try MapBuilder.encodeTiles(regionJSON: map, tiles: [t])[0]
    let waterWays = (try? MapBuilder.extractWaterWays(regionJSON: map)) ?? []
    let parkWays = (try? MapBuilder.extractParkWays(regionJSON: map)) ?? []
    MapBuilder.appendElevation(to: &p.data, south: t.south, west: t.west, north: t.north,
                               east: t.east, grid: elev, n: MapBuilder.elevationGrid)
    MapBuilder.appendWater(to: &p.data, waterWays: waterWays, seaRings: rings,
                           south: t.south, west: t.west, north: t.north, east: t.east)
    MapBuilder.appendParks(to: &p.data, parkWays: parkWays,
                           south: t.south, west: t.west, north: t.north, east: t.east)
    if !MapBuilder.isEmpty(p.data, tile: t) {
        try p.data.write(to: out.appendingPathComponent("\(id).ebm"))
    }
    // MapsView.sendPois: POIs grouped by the H3 cell they fall in.
    let pois = try MapBuilder.collectPois(regionJSON: poiJSON)
    let mine = pois.filter { H3Tiles.id(at: .init(latitude: $0.lat, longitude: $0.lon)) == id }
    let poi = MapBuilder.buildPoi(mine, south: t.south, west: t.west, north: t.north, east: t.east,
                                  cell: id, contains: { _, _ in true })
    try poi.write(to: out.appendingPathComponent("\(id).poi"))
}
print("swift: \(lines.filter { $0.count == 5 }.count) cells, \(bboxMismatch) bbox mismatches")
