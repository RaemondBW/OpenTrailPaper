import Foundation

// The iOS side of live segments: Strava's starred list and detail parsing,
// the polyline cut, the name folding, and the device blob — checked against
// the same bytes as src/segment_match.h's parser (kFixtureHex in
// segment_test.cpp). Run by run_swift_test.sh.
@main struct SegmentsTest {
    static let fixtureHex =
        "4f54534701000200cb04fb711f010000f2020000dd3e0000390003000b436166" +
        "c3a920636c696d6258b93900401c45ffbcb939000e1c45ff52ba3900361c45ff" +
        "2a000000000000000000000088130000fdff020004466c6174a0ad3900803b45" +
        "ff62af3900803b45ff"

    static func main() {
        // The shared fixture, byte for byte.
        let blob = StravaSegments.encode([
            DeviceSegment(id: 1_234_567_890_123, name: "Café climb", prSeconds: 754, distanceMeters: 1609.3,
                          averageGrade: 5.7,
                          points: [LatLon(lat: 37.83, lon: -122.48), LatLon(lat: 37.831, lon: -122.4805),
                                   LatLon(lat: 37.8325, lon: -122.4801)]),
            DeviceSegment(id: 42, name: "Flat", prSeconds: 0, distanceMeters: 500, averageGrade: -0.3,
                          points: [LatLon(lat: 37.8, lon: -122.4), LatLon(lat: 37.8045, lon: -122.4)]),
        ])
        let hex = blob.map { String(format: "%02x", $0) }.joined()
        precondition(hex == fixtureHex, "fixture bytes: \(hex)")

        // Names: cut on a character, never inside one.
        let long = StravaSegments.encode([DeviceSegment(id: 1, name: String(repeating: "é", count: 30), prSeconds: 0,
                                                        distanceMeters: 1, averageGrade: 0,
                                                        points: [LatLon(lat: 0, lon: 0), LatLon(lat: 0, lon: 0.001)])])
        precondition(long[8 + 20] == 46)   // 23 two-byte characters
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

        // The cut: a straight line collapses to its ends; a long wiggly one
        // fits maxPoints and keeps both ends.
        let line = (0...100).map { LatLon(lat: 37 + Double($0) * 1e-4, lon: -122) }
        precondition(StravaSegments.simplify(line).count == 2)
        let wiggle = (0..<2000).map { LatLon(lat: 37 + Double($0) * 1e-5, lon: -122 + 0.01 * sin(Double($0) / 20)) }
        let thin = StravaSegments.simplify(wiggle)
        precondition(thin.count <= StravaSegments.maxPoints && thin.count > 2)
        precondition(thin.first == wiggle.first && thin.last == wiggle.last)

        // Assembly: rides with a polyline, the list's order, the detail's PR
        // when the list had none, capped.
        let segs = StravaSegments.deviceSegments(page.segments,
                                                 polylines: [229_781: "_p~iF~ps|U_ulLnnqC_mqNvxq`@", 77: "_p~iF~ps|U_ulLnnqC",
                                                             78: "_p~iF~ps|U_ulLnnqC"],
                                                 prs: [78: 5])
        precondition(segs.map(\.id) == [229_781, 78])   // 1234 has no polyline, 77 is a run
        precondition(segs[0].prSeconds == 524 && segs[1].prSeconds == 99)
        precondition(segs[0].distanceMeters == 2684.82 && segs[0].points.count == 3)
        print("Swift segment tests passed")
    }
}
