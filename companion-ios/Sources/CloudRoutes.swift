import Foundation

// The saved-route lists of Strava and RideWithGPS, as the route picker shows
// them. Parsing only (the requests are SyncAccounts.routes / routeGPX), and
// Foundation only, so tools/route_test can compile it on the Mac.
// Mirrors data/CloudRoutes.kt in the Android app.

/// One route in a provider's list. `id` is a string: Strava's route ids pass 2^53.
struct CloudRoute: Identifiable, Hashable {
    let id: String
    let name: String
    let distanceMeters: Double?
    let elevationGainMeters: Double?
    /// The route's outline for the list thumbnail; nil when the provider sends none.
    var shape: [LatLon]? = nil
}

/// A plain coordinate, so this file stays Foundation-only.
struct LatLon: Hashable {
    let lat: Double
    let lon: Double
}

/// One page of a list; `nextPage` is nil when that was the last.
struct CloudRoutePage {
    let routes: [CloudRoute]
    let nextPage: Int?
}

enum CloudRoutes {
    /// Strava's own default; a full page is the only sign there may be another.
    static let stravaPageSize = 30

    /// GET /api/v3/athletes/{id}/routes?page=&per_page=: a bare array.
    static func parseStrava(_ data: Data, page: Int) -> CloudRoutePage {
        guard let arr = (try? JSONSerialization.jsonObject(with: data)) as? [Any] else {
            return CloudRoutePage(routes: [], nextPage: nil)
        }
        let routes = arr.compactMap { e -> CloudRoute? in
            guard let o = e as? [String: Any],
                  // id_str first: the numeric id is past what a double holds exactly.
                  let id = str(o["id_str"]) ?? str(o["id"]) else { return nil }
            // The list's map.summary_polyline: a coarse outline, enough for a thumbnail.
            let shape = ((o["map"] as? [String: Any])?["summary_polyline"] as? String)
                .flatMap(decodePolyline).flatMap { $0.count >= 2 ? $0 : nil }
            return CloudRoute(id: id, name: str(o["name"]) ?? "Strava route",
                              distanceMeters: num(o["distance"]), elevationGainMeters: num(o["elevation_gain"]),
                              shape: shape)
        }
        return CloudRoutePage(routes: routes, nextPage: arr.count >= stravaPageSize ? page + 1 : nil)
    }

    /// The sync service's GET /v1/rwgps/routes: {routes: [...], next_page}.
    static func parseRwgps(_ data: Data) -> CloudRoutePage {
        guard let o = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any] else {
            return CloudRoutePage(routes: [], nextPage: nil)
        }
        let routes = (o["routes"] as? [Any] ?? []).compactMap { e -> CloudRoute? in
            guard let r = e as? [String: Any], let id = str(r["id"]) else { return nil }
            return CloudRoute(id: id, name: str(r["name"]) ?? "RideWithGPS route",
                              distanceMeters: num(r["distance"]), elevationGainMeters: num(r["elevation_gain"]))
        }
        return CloudRoutePage(routes: routes, nextPage: (o["next_page"] as? NSNumber)?.intValue)
    }

    /// Strava reports what the rider granted; anything without read_all lists public routes only.
    static func stravaSeesPrivateRoutes(_ scope: String?) -> Bool {
        scope?.split(separator: ",").contains { $0.trimmingCharacters(in: .whitespaces) == "read_all" } == true
    }

    /// Google's encoded polyline format (1e-5 degrees, zigzag varints in
    /// 5-bit chunks offset by 63). Nil on anything malformed.
    static func decodePolyline(_ s: String) -> [LatLon]? {
        let bytes = Array(s.utf8)
        var i = 0
        func next() -> Int? {
            var result = 0, shift = 0
            while i < bytes.count && shift <= 30 {
                let b = Int(bytes[i]) - 63
                i += 1
                guard (0...63).contains(b) else { return nil }
                result |= (b & 0x1F) << shift
                shift += 5
                if b < 0x20 { return result & 1 != 0 ? ~(result >> 1) : result >> 1 }
            }
            return nil
        }
        var out: [LatLon] = []
        var lat = 0, lon = 0
        while i < bytes.count {
            guard let dLat = next(), let dLon = next() else { return nil }
            lat += dLat
            lon += dLon
            out.append(LatLon(lat: Double(lat) / 1e5, lon: Double(lon) / 1e5))
        }
        return out
    }

    private static func str(_ v: Any?) -> String? {
        let s: String?
        if let v = v as? String { s = v }
        else if let n = v as? NSNumber, CFGetTypeID(n) != CFBooleanGetTypeID() { s = n.stringValue }
        else { s = nil }
        return s.map { $0.trimmingCharacters(in: .whitespacesAndNewlines) }.flatMap { $0.isEmpty ? nil : $0 }
    }

    private static func num(_ v: Any?) -> Double? {
        guard let n = v as? NSNumber, CFGetTypeID(n) != CFBooleanGetTypeID() else { return nil }
        return n.doubleValue
    }
}
