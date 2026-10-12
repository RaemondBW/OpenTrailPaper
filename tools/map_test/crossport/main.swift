// Host driver for the cross-port check (run_crossport.sh): runs a saved
// Overpass response through the iOS app's MapBuilder exactly as the app does
// and writes, per tile, <id>.ebm (roads + way-flag trailer, the part every
// port builds the same way) and the two .poi variants:
//   <id>.poi      — POIs in the H3 cell (what the app sends)
//   <id>.bbox.poi — POIs in the tile bbox (buildPoi without a cell test)
//
//   crossport <overpass.json> <tiles.txt> <outdir>
// tiles.txt: one "<h3id> <s> <w> <n> <e>" per line.
import Foundation

let args = CommandLine.arguments
// `crossport tiles <s> <w> <n> <e>`: print the H3 res-6 tiles covering a box,
// with the bboxes the app uses (H3 C, as H3Tiles.coveringTiles), for tiles.txt.
if args.count == 6, args[1] == "tiles" {
    for t in H3Tiles.coveringTiles(south: Double(args[2])!, west: Double(args[3])!,
                                   north: Double(args[4])!, east: Double(args[5])!) {
        print("\(t.id) \(t.south) \(t.west) \(t.north) \(t.east)")
    }
    exit(0)
}
guard args.count == 4 else {
    FileHandle.standardError.write("usage: crossport <overpass.json> <tiles.txt> <outdir>\n".data(using: .utf8)!)
    exit(2)
}
let json = try Data(contentsOf: URL(fileURLWithPath: args[1]))
let tiles = try String(contentsOfFile: args[2], encoding: .utf8)
    .split(separator: "\n").map { $0.split(separator: " ").map(String.init) }
let out = URL(fileURLWithPath: args[3])
try FileManager.default.createDirectory(at: out, withIntermediateDirectories: true)

let pois = try MapBuilder.collectPois(regionJSON: json)
for t in tiles where t.count == 5 {
    let id = t[0]
    let s = Double(t[1])!, w = Double(t[2])!, n = Double(t[3])!, e = Double(t[4])!
    let ebm = try MapBuilder.encodeForTest(jsonData: json, s: s, w: w, n: n, e: e)
    try ebm.write(to: out.appendingPathComponent("\(id).ebm"))
    let cell = MapTile(id: id, cell: 0, south: s, west: w, north: n, east: e)
    let inCell = MapBuilder.buildPoi(pois, south: s, west: w, north: n, east: e, cell: id,
                                     contains: { la, lo in
        H3Tiles.id(at: .init(latitude: la, longitude: lo)) == cell.id })
    try inCell.write(to: out.appendingPathComponent("\(id).poi"))
    let inBox = MapBuilder.buildPoi(pois, south: s, west: w, north: n, east: e, cell: id)
    try inBox.write(to: out.appendingPathComponent("\(id).bbox.poi"))
}
