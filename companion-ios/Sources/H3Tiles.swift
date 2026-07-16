import Foundation
import CoreLocation

// One H3 res-6 map tile: its H3 id (the device's filename), the raw cell, and
// lat/lng bbox.
struct MapTile: Identifiable, Equatable {
    let id: String            // H3 hex id, e.g. "86283082fffffff"
    let cell: UInt64          // raw H3 index (for the hexagon boundary)
    let south: Double
    let west: Double
    let north: Double
    let east: Double

    // The tile's actual hexagon outline (not its bounding box).
    var hexagon: [CLLocationCoordinate2D] { H3Tiles.hexagon(of: cell) }
    // Center of the hexagon (for placing the on-device check mark).
    var center: CLLocationCoordinate2D {
        .init(latitude: (south + north) / 2, longitude: (west + east) / 2)
    }
}

// Thin Swift front for the H3 C shim (Sources/H3/h3shim.*). Res-6 hexagons are
// ~5.6 km across; the whole scheme is deterministic and global, so the same
// ground always maps to the same tile id — that's what makes dedup work.
enum H3Tiles {
    // Every res-6 tile overlapping the drawn box, in a stable order.
    static func coveringTiles(south s: Double, west w: Double,
                              north n: Double, east e: Double) -> [MapTile] {
        var ids = [UInt64](repeating: 0, count: 4096)
        let count = Int(ids.withUnsafeMutableBufferPointer {
            h3_covering_cells(s, w, n, e, $0.baseAddress, Int32($0.count))
        })
        return (0..<count).map { i in tile(from: ids[i]) }
    }

    static func tile(from cell: UInt64) -> MapTile {
        var ts = 0.0, tw = 0.0, tn = 0.0, te = 0.0
        h3_cell_bbox(cell, &ts, &tw, &tn, &te)
        var idBuf = [CChar](repeating: 0, count: 17)
        h3_cell_id(cell, &idBuf, 17)
        return MapTile(id: String(cString: idBuf), cell: cell,
                       south: ts, west: tw, north: tn, east: te)
    }

    // The res-6 H3 id containing a coordinate (for hit-testing map taps).
    static func id(at coord: CLLocationCoordinate2D) -> String? {
        let cell = h3_cell_at(coord.latitude, coord.longitude)
        guard cell != 0 else { return nil }
        var idBuf = [CChar](repeating: 0, count: 17)
        h3_cell_id(cell, &idBuf, 17)
        return String(cString: idBuf)
    }

    // The cell's hexagon outline as map coordinates.
    static func hexagon(of cell: UInt64) -> [CLLocationCoordinate2D] {
        var buf = [Double](repeating: 0, count: 24)   // up to 12 verts × 2
        let n = Int(buf.withUnsafeMutableBufferPointer {
            h3_cell_boundary(cell, $0.baseAddress, Int32($0.count / 2))
        })
        return (0..<n).map { CLLocationCoordinate2D(latitude: buf[$0 * 2], longitude: buf[$0 * 2 + 1]) }
    }
}
