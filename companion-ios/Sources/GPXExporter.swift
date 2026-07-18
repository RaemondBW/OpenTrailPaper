import Foundation
import CoreLocation

// Serializes a route polyline to GPX 1.1 <trk> the device's parser reads
// (it scans lat="…"/lon="…" attribute pairs, order lat-before-lon).
enum GPXExporter {
    static func make(name: String, coordinates: [CLLocationCoordinate2D]) -> String {
        var s = """
        <?xml version="1.0" encoding="UTF-8"?>
        <gpx version="1.1" creator="OpenCycleInk" xmlns="http://www.topografix.com/GPX/1/1">
        <trk><name>\(xmlEscape(name))</name><trkseg>
        """
        s.reserveCapacity(coordinates.count * 48)
        for c in coordinates {
            s += String(format: "<trkpt lat=\"%.6f\" lon=\"%.6f\"></trkpt>",
                        c.latitude, c.longitude)
        }
        s += "</trkseg></trk></gpx>"
        return s
    }

    private static func xmlEscape(_ s: String) -> String {
        s.replacingOccurrences(of: "&", with: "&amp;")
            .replacingOccurrences(of: "<", with: "&lt;")
            .replacingOccurrences(of: ">", with: "&gt;")
    }
}
