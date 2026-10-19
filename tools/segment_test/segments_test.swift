import Foundation

// The iOS side of live segments: Strava's starred list, detail and streams
// parsing, the line + elevation cut, the name folding, and the device blob —
// checked against the same bytes as src/segment_match.h's parser
// (kFixtureHexV1 / kFixtureHexV2 in segment_test.cpp). Run by run_swift_test.sh.
@main struct SegmentsTest {
    static let fixtureHexV1 =
        "4f54534701000200cb04fb711f010000f2020000dd3e0000390003000b436166" +
        "c3a920636c696d6258b93900401c45ffbcb939000e1c45ff52ba3900361c45ff" +
        "2a000000000000000000000088130000fdff020004466c6174a0ad3900803b45" +
        "ff62af3900803b45ff"
    static let fixtureHexV2 =
        "4f54534702000200cb04fb711f010000f2020000dd3e000039000300010b4361" +
        "66c3a920636c696d6258b93900401c45ff7427bcb939000e1c45ff8d2752ba39" +
        "00361c45fff0262a000000000000000000000088130000fdff02000004466c61" +
        "74a0ad3900803b45ff000062af3900803b45ff0000"

    static func main() {
        // The shared fixtures, byte for byte.
        let segs = [
            DeviceSegment(id: 1_234_567_890_123, name: "Café climb", prSeconds: 754, distanceMeters: 1609.3,
                          averageGrade: 5.7,
                          points: [LatLon(lat: 37.83, lon: -122.48), LatLon(lat: 37.831, lon: -122.4805),
                                   LatLon(lat: 37.8325, lon: -122.4801)],
                          elevations: [10.0, 12.5, -3.2]),
            DeviceSegment(id: 42, name: "Flat", prSeconds: 0, distanceMeters: 500, averageGrade: -0.3,
                          points: [LatLon(lat: 37.8, lon: -122.4), LatLon(lat: 37.8045, lon: -122.4)]),
        ]
        func hex(_ d: Data) -> String { d.map { String(format: "%02x", $0) }.joined() }
        precondition(hex(StravaSegments.encode(segs, version: 1)) == fixtureHexV1,
                     "v1 fixture bytes: \(hex(StravaSegments.encode(segs, version: 1)))")
        precondition(hex(StravaSegments.encode(segs)) == fixtureHexV2, "v2 fixture bytes: \(hex(StravaSegments.encode(segs)))")

        // Names: cut on a character, never inside one.
        let long = StravaSegments.encode([DeviceSegment(id: 1, name: String(repeating: "é", count: 30), prSeconds: 0,
                                                        distanceMeters: 1, averageGrade: 0,
                                                        points: [LatLon(lat: 0, lon: 0), LatLon(lat: 0, lon: 0.001)])])
        precondition(long[8 + 21] == 46)   // v2: the name length follows the flags byte
        precondition(StravaSegments.deviceName("  Côte de  l'Église ") == "Cote de l'Eglise")
        precondition(StravaSegments.deviceName("山") == "Segment")
        precondition(StravaSegments.deviceName(String(repeating: "a", count: 60)).count == 47)

        // Starred list: ids, PRs wherever they are, rides only, paging.
        let list = """
        [{"id": 229781, "name": "Hawk Hill", "distance": 2684.82, "average_grade": 5.7,
          "activity_type": "Ride", "athlete_pr_effort": {"pr_elapsed_time": 524, "effort_count": 3}},
         {"id": 1234, "name": "Old", "distance": 100, "average_grade": 1, "activity_type": "Ride",
          "athlete_pr_effort": {"id": 9, "elapsed_time": 61}},
         {"id": 77, "name": "Run hill", "distance": 400, "average_grade": 8, "activity_type": "Run"},
         {"id": 78, "name": "Legacy", "distance": 400, "average_grade": 2, "pr_time": 99},
         {"name": "no id"}]
        """
        let page = StravaSegments.parseStarred(Data(list.utf8), page: 1)
        precondition(page.segments.count == 4 && page.nextPage == nil)
        precondition(page.segments[0].id == 229_781 && page.segments[0].prSeconds == 524)
        precondition(page.segments[1].prSeconds == 61 && page.segments[3].prSeconds == 99)
        precondition(!page.segments[2].isRide && page.segments[3].isRide)
        let full = "[" + Array(repeating: #"{"id": 1, "name": "x"}"#, count: 200).joined(separator: ",") + "]"
        precondition(StravaSegments.parseStarred(Data(full.utf8), page: 2).nextPage == 3)
        precondition(StravaSegments.parseStarred(Data("{\"message\":\"x\"}".utf8), page: 1).segments.isEmpty)

        // Detail: the polyline, the PR from the stats.
        let detail = #"{"id": 77, "map": {"polyline": "_p~iF~ps|U_ulLnnqC_mqNvxq`@"}, "athlete_segment_stats": {"pr_elapsed_time": 300}}"#
        let d = StravaSegments.parseDetail(Data(detail.utf8))
        precondition(d.polyline == "_p~iF~ps|U_ulLnnqC_mqNvxq`@" && d.prSeconds == 300)

        // Streams, keyed by type and as an array; altitude only when it lines up.
        let keyed = #"{"latlng": {"data": [[37.1, -122.1], [37.2, -122.2], [37.3, -122.25]], "series_type": "distance"}, "altitude": {"data": [10, 20.5, 31]}, "distance": {"data": [0, 100, 200]}}"#
        let s1 = StravaSegments.parseStreams(Data(keyed.utf8))!
        precondition(s1.points.count == 3 && s1.points[1] == LatLon(lat: 37.2, lon: -122.2) && s1.altitudes == [10, 20.5, 31])
        let arr = #"[{"type": "latlng", "data": [[1, 2], [1.001, 2]]}, {"type": "altitude", "data": [5]}]"#
        let s2 = StravaSegments.parseStreams(Data(arr.utf8))!
        precondition(s2.points.count == 2 && s2.altitudes == nil)   // altitude doesn't line up
        precondition(StravaSegments.parseStreams(Data(#"{"altitude": {"data": [1, 2]}}"#.utf8)) == nil)
        precondition(StravaSegments.parseStreams(Data("[]".utf8)) == nil)

        // The cut: a straight line collapses to its ends; a long wiggly one
        // fits maxPoints and keeps both ends.
        let line = (0...100).map { LatLon(lat: 37 + Double($0) * 1e-4, lon: -122) }
        precondition(StravaSegments.simplify(line).count == 2)
        let wiggle = (0..<2000).map { LatLon(lat: 37 + Double($0) * 1e-5, lon: -122 + 0.01 * sin(Double($0) / 20)) }
        let thin = StravaSegments.simplify(wiggle)
        precondition(thin.count <= StravaSegments.maxPoints && thin.count > 2)
        precondition(thin.first == wiggle.first && thin.last == wiggle.last)
        // ...and in 3D: a straight road whose grade changes halfway keeps the
        // point where it changes, with its altitude.
        let road = (0...100).map { LatLon(lat: 37 + Double($0) * 1e-4, lon: -122) }
        let alt: [Double] = (0...100).map { (i: Int) -> Double in i <= 50 ? Double(i) * 0.2 : 10.0 + Double(i - 50) }
        let cut = StravaSegments.simplify(road, altitudes: alt)
        precondition(cut.points.count == 3 && cut.altitudes == [0, 10, 60], "3D cut: \(cut)")
        precondition(StravaSegments.simplify(road, altitudes: nil).altitudes == nil)

        // Assembly: rides with a line, the list's order, the detail's PR when
        // the list had none, capped; the elevation rides along.
        let g = StravaSegments.geometry(points: s1.points, altitudes: s1.altitudes)
        let poly = StravaSegments.geometry(points: CloudRoutes.decodePolyline("_p~iF~ps|U_ulLnnqC")!, altitudes: nil)
        let devs = StravaSegments.deviceSegments(page.segments, geometry: [229_781: g, 77: poly, 78: poly],
                                                 prs: [78: 5])
        precondition(devs.map(\.id) == [229_781, 78])   // 1234 has no line, 77 is a run
        precondition(devs[0].prSeconds == 524 && devs[1].prSeconds == 99)
        precondition(devs[0].elevations == [10, 20.5, 31] && devs[1].elevations == nil)
        // The cache round-trips.
        let back = try! JSONDecoder().decode(SegmentGeometry.self, from: JSONEncoder().encode(g))
        precondition(back == g)
        print("Swift segment tests passed")
    }
}
