import CoreLocation
import Foundation

// The iOS route import: GPXImporter (Strava's export_gpx, the sync service's
// RideWithGPS GPX) and the CloudRoutes list parsers. Run by run_route_test.sh.
@main struct ImportTest {
    static func main() {
        // Strava export: the athlete's name in <metadata><author> must not win.
        let strava = """
        <?xml version="1.0" encoding="UTF-8"?>
        <gpx creator="StravaGPX" version="1.1" xmlns="http://www.topografix.com/GPX/1/1">
         <metadata><name>Hills</name><author><name>Ada L</name></author></metadata>
         <wpt lat="1.0" lon="2.0"><name>cafe</name></wpt>
         <trk><name>Hills</name><type>cycling</type><trkseg>
          <trkpt lat="37.7" lon="-122.4"><ele>10</ele></trkpt>
          <trkpt lat="37.71" lon="-122.41"><ele>12</ele></trkpt>
         </trkseg></trk></gpx>
        """
        guard case .success(let r) = GPXImporter.parse(Data(strava.utf8), fallbackName: "x") else { fatalError("strava gpx") }
        precondition(r.name == "Hills" && r.mode == "Cycling" && r.coordinates.count == 2)
        precondition(r.coordinates[0].latitude == 37.7)   // the waypoint is not in the track
        precondition(abs(r.distanceMeters - 1417) < 20)

        // Route points only, no name: falls back.
        let rte = #"<gpx><rte><rtept lat="1" lon="1"/><rtept lat="1.001" lon="1"/></rte></gpx>"#
        guard case .success(let r2) = GPXImporter.parse(Data(rte.utf8), fallbackName: "Fallback") else { fatalError("rte") }
        precondition(r2.name == "Fallback" && r2.mode == "" && r2.coordinates.count == 2)

        precondition(GPXImporter.parse(Data("{\"error\":1}".utf8), fallbackName: "x") == .failure(.notGPX))
        precondition(GPXImporter.parse(Data(#"<gpx><trk><trkseg><trkpt lat="1" lon="1"/></trkseg></trk></gpx>"#.utf8),
                                       fallbackName: "x") == .failure(.noPoints))

        // Decimation keeps the device's cap and the last point.
        let many = (0..<10_001).map { CLLocationCoordinate2D(latitude: Double($0) / 1e4, longitude: 0) }
        let thin = GPXImporter.decimate(many, cap: GPXImporter.maxPoints)
        precondition(thin.count <= GPXImporter.maxPoints + 1 && thin.last?.latitude == many.last?.latitude)
        precondition(GPXImporter.decimate(Array(many.prefix(5)), cap: 10).count == 5)

        // Strava list: id_str over the numeric id; a full page offers the next.
        let list = """
        [{"id": 3344556677889900112, "id_str": "3344556677889900112", "name": "Hills", "distance": 52345.6, "elevation_gain": 812},
         {"id": 17, "name": " ", "distance": null}, {"name": "no id"}]
        """
        let p1 = CloudRoutes.parseStrava(Data(list.utf8), page: 1)
        precondition(p1.routes == [CloudRoute(id: "3344556677889900112", name: "Hills", distanceMeters: 52345.6, elevationGainMeters: 812),
                                   CloudRoute(id: "17", name: "Strava route", distanceMeters: nil, elevationGainMeters: nil)])
        precondition(p1.nextPage == nil)
        let full = "[" + (1...CloudRoutes.stravaPageSize).map { "{\"id\": \($0)}" }.joined(separator: ",") + "]"
        precondition(CloudRoutes.parseStrava(Data(full.utf8), page: 2).nextPage == 3)
        precondition(CloudRoutes.parseStrava(Data("<html>".utf8), page: 1).routes.isEmpty)

        // RideWithGPS through the sync service.
        let rw = CloudRoutes.parseRwgps(Data(#"{"routes":[{"id":11,"name":"Loop","distance":42000.5,"elevation_gain":610}],"next_page":2}"#.utf8))
        precondition(rw.routes == [CloudRoute(id: "11", name: "Loop", distanceMeters: 42000.5, elevationGainMeters: 610)] && rw.nextPage == 2)
        precondition(CloudRoutes.parseRwgps(Data(#"{"routes":[],"next_page":null}"#.utf8)).nextPage == nil)

        // Google's documented polyline example, and the list's map.summary_polyline.
        let poly = #"_p~iF~ps|U_ulLnnqC_mqNvxq`@"#
        let pts = CloudRoutes.decodePolyline(poly)!
        let want = [(38.5, -120.2), (40.7, -120.95), (43.252, -126.453)]
        precondition(pts.count == 3 && zip(pts, want).allSatisfy { abs($0.lat - $1.0) < 1e-9 && abs($0.lon - $1.1) < 1e-9 })
        precondition(CloudRoutes.decodePolyline("") == [] && CloudRoutes.decodePolyline("_p~iF") == nil)
        let mapped = CloudRoutes.parseStrava(Data(#"[{"id": 5, "map": {"summary_polyline": "\#(poly)"}}, {"id": 6, "map": {"summary_polyline": ""}}]"#.utf8), page: 1)
        precondition(mapped.routes[0].shape == pts && mapped.routes[1].shape == nil)

        precondition(CloudRoutes.stravaSeesPrivateRoutes("read,activity:write,read_all"))
        precondition(!CloudRoutes.stravaSeesPrivateRoutes("read,activity:write"))
        precondition(!CloudRoutes.stravaSeesPrivateRoutes(nil))
        print("import_test: ok")
    }
}
