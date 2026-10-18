import Foundation
import CoreLocation

/// A route somebody planned elsewhere (Strava, RideWithGPS): geometry and a name.
struct ImportedRoute: Identifiable, Equatable {
    let id = UUID()
    let name: String
    let coordinates: [CLLocationCoordinate2D]
    /// The raw `<type>`, e.g. "cycling"; nil when the file omits it.
    let activityType: String?
    /// Where it was picked from ("Strava") and the climb that list reported;
    /// GPXImporter leaves both nil (the exported GPX has neither).
    var source: String? = nil
    var climbMeters: Double? = nil

    var distanceMeters: Double {
        zip(coordinates, coordinates.dropFirst()).reduce(0) { sum, pair in
            sum + CLLocation(latitude: pair.0.latitude, longitude: pair.0.longitude)
                .distance(from: CLLocation(latitude: pair.1.latitude, longitude: pair.1.longitude))
        }
    }

    /// Feeds the badge the summary card already draws. Blank when the file doesn't say.
    var mode: String {
        switch activityType?.lowercased() {
        case "racebike", "cycling", "bike", "mtb", "touringbicycle", "gravel_bike",
             "mountainbike", "e_bike": return "Cycling"
        default: return ""
        }
    }

    static func == (a: ImportedRoute, b: ImportedRoute) -> Bool { a.id == b.id }
}

/// Reads a GPX route into plain coordinates; the iOS twin of the Android app's
/// GpxImporter.
///
/// Deliberately not a pass-through to the device: the firmware's scanner pairs
/// any `lat="`/`lon="` it finds regardless of tag (routes.cpp), so waypoints
/// ahead of the track would be dragged into the polyline. Parsing here and
/// re-emitting through GPXExporter keeps that from happening, and drops
/// `<ele>`/`<time>` on the way.
enum GPXImporter {
    enum Failure: Error, Equatable { case notGPX, noPoints }

    /// The firmware's own cap (routes.cpp MAX_PTS). Decimating here saves BLE time.
    static let maxPoints = 4096
    /// Above this we stop collecting: a pathological file must not exhaust memory.
    private static let hardLimit = 200_000

    static func parse(_ data: Data, fallbackName: String) -> Result<ImportedRoute, Failure> {
        let handler = Handler()
        let parser = XMLParser(data: data)
        parser.shouldResolveExternalEntities = false
        parser.delegate = handler
        // A truncated file that already yielded a usable track is still a
        // route, so the parse result itself is not the verdict.
        _ = parser.parse()
        guard handler.sawGPX else { return .failure(.notGPX) }
        let points = handler.trackPoints.isEmpty ? handler.routePoints : handler.trackPoints
        guard points.count >= 2 else { return .failure(.noPoints) }
        let name = (handler.trackName ?? handler.metadataName ?? fallbackName)
            .trimmingCharacters(in: .whitespacesAndNewlines)
        let type = handler.activityType?.trimmingCharacters(in: .whitespacesAndNewlines)
        return .success(ImportedRoute(name: name.isEmpty ? fallbackName : name,
                                      coordinates: decimate(points, cap: maxPoints),
                                      activityType: type?.isEmpty == false ? type : nil))
    }

    /// Every stride-th point, last one always kept: the same shape as the
    /// firmware's own halving, done here so a long export is not thinned on arrival.
    static func decimate(_ points: [CLLocationCoordinate2D], cap: Int) -> [CLLocationCoordinate2D] {
        guard points.count > cap else { return points }
        let stride = Int((Double(points.count) / Double(cap)).rounded(.up))
        var out = Swift.stride(from: 0, to: points.count, by: stride).map { points[$0] }
        if (points.count - 1) % stride != 0 { out.append(points[points.count - 1]) }
        return out
    }

    /// Element names are matched against their parent, never on their own:
    /// `<name>` and `<type>` both appear inside `<author>` too (Strava writes the
    /// athlete in `<metadata><author><name>`).
    private final class Handler: NSObject, XMLParserDelegate {
        var trackPoints: [CLLocationCoordinate2D] = []
        var routePoints: [CLLocationCoordinate2D] = []
        var trackName: String?
        var metadataName: String?
        var activityType: String?
        var sawGPX = false

        private var stack: [String] = []
        private var text: String?
        private static let scopes: Set<String> = ["trk", "rte", "metadata"]

        private var parent: String? { stack.count >= 2 ? stack[stack.count - 2] : nil }

        private static func local(_ qName: String) -> String {
            (qName.split(separator: ":").last.map(String.init) ?? qName).lowercased()
        }

        func parser(_ parser: XMLParser, didStartElement elementName: String, namespaceURI: String?,
                    qualifiedName qName: String?, attributes: [String: String] = [:]) {
            let name = Self.local(elementName)
            stack.append(name)
            switch name {
            case "gpx": sawGPX = true
            case "trkpt": if let p = point(attributes), trackPoints.count < GPXImporter.hardLimit { trackPoints.append(p) }
            case "rtept": if let p = point(attributes), routePoints.count < GPXImporter.hardLimit { routePoints.append(p) }
            case "name", "type": if let parent, Self.scopes.contains(parent) { text = "" }
            default: break
            }
        }

        func parser(_ parser: XMLParser, foundCharacters string: String) {
            text? += string
        }

        func parser(_ parser: XMLParser, didEndElement elementName: String, namespaceURI: String?,
                    qualifiedName qName: String?) {
            let name = Self.local(elementName)
            if let value = text {
                switch (name, parent) {
                case ("name", "trk"), ("name", "rte"): if trackName == nil { trackName = value }
                case ("name", "metadata"): if metadataName == nil { metadataName = value }
                case ("type", "trk"), ("type", "rte"): if activityType == nil { activityType = value }
                default: break
                }
            }
            text = nil
            if !stack.isEmpty { stack.removeLast() }
        }

        private func point(_ a: [String: String]) -> CLLocationCoordinate2D? {
            guard let lat = a["lat"].flatMap(Double.init), let lon = a["lon"].flatMap(Double.init),
                  (-90...90).contains(lat), (-180...180).contains(lon) else { return nil }
            return CLLocationCoordinate2D(latitude: lat, longitude: lon)
        }
    }
}
