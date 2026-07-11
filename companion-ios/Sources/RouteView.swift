import SwiftUI
import MapKit
import CoreLocation

// Search a destination, build a cycling route with MapKit, preview it, and
// send it to the head unit as GPX over BLE.
struct RouteView: View {
    @EnvironmentObject var ble: BLEManager
    @StateObject private var model = RouteModel()
    @State private var showSaved = false

    var body: some View {
        NavigationStack {
            ZStack(alignment: .bottom) {
                Map(position: $model.camera) {
                    UserAnnotation()
                    if let dest = model.destination {
                        Marker(model.destinationName, coordinate: dest)
                            .tint(Palette.accent)
                    }
                    if let route = model.route {
                        MapPolyline(route.polyline)
                            .stroke(Palette.accent, lineWidth: 5)
                    }
                }
                .mapControls { MapUserLocationButton() }
                .ignoresSafeArea(edges: .top)

                VStack(spacing: 12) {
                    SearchField(text: $model.query) { model.search() }

                    if !model.results.isEmpty && model.route == nil {
                        ResultsList(results: model.results) { model.choose($0) }
                    }

                    if let route = model.route {
                        RouteSummaryCard(
                            name: model.destinationName,
                            mode: model.routeMode,
                            distanceKm: route.distance / 1000,
                            minutes: Int(route.expectedTravelTime / 60),
                            progress: ble.lastUploadProgress,
                            canSend: ble.state == .connected
                        ) {
                            let coords = route.polyline.coordinates
                            let gpx = GPXExporter.make(
                                name: model.fileName, coordinates: coords)
                            // Turn cues from the route steps: the maneuver
                            // point is each step's first coordinate; skip the
                            // "proceed to…" opener with no real instruction.
                            let turns = route.steps.compactMap { step -> BLEManager.Maneuver? in
                                let text = step.instructions.trimmingCharacters(
                                    in: .whitespaces)
                                guard !text.isEmpty,
                                      let c = step.polyline.coordinates.first
                                else { return nil }
                                return BLEManager.Maneuver(
                                    lat: c.latitude, lon: c.longitude, text: text)
                            }
                            ble.uploadRoute(name: model.fileName, gpx: gpx,
                                            maneuvers: turns)
                        } clear: {
                            model.clearRoute()
                        }
                    }
                }
                .padding(16)
            }
            .navigationTitle("Route")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                Button {
                    ble.refreshRoutes(); showSaved = true
                } label: { Image(systemName: "folder") }
                .disabled(ble.state != .connected)
            }
            .sheet(isPresented: $showSaved) { SavedRoutesSheet() }
            .onAppear { model.requestLocation() }
        }
    }
}

// Saved routes on the device, with delete.
private struct SavedRoutesSheet: View {
    @EnvironmentObject var ble: BLEManager
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            Group {
                if ble.loadingRoutes {
                    ProgressView("Loading…")
                } else if ble.deviceRoutes.isEmpty {
                    ContentUnavailableView("No saved routes", systemImage: "map",
                        description: Text("Routes you send to the device appear here."))
                } else {
                    List {
                        ForEach(ble.deviceRoutes, id: \.self) { name in
                            Text(name)
                        }
                        .onDelete { idx in
                            idx.map { ble.deviceRoutes[$0] }.forEach(ble.deleteRoute)
                        }
                    }
                }
            }
            .navigationTitle("Saved Routes")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") { dismiss() }
                }
            }
        }
    }
}

// MARK: - Model

@MainActor
final class RouteModel: NSObject, ObservableObject {
    @Published var query = ""
    @Published var results: [MKMapItem] = []
    @Published var destination: CLLocationCoordinate2D?
    @Published var destinationName = ""
    @Published var route: MKRoute?
    @Published var routeMode = ""   // "Cycling" or "Walking"
    @Published var camera: MapCameraPosition = .userLocation(
        fallback: .region(MKCoordinateRegion(
            center: CLLocationCoordinate2D(latitude: 37.7764, longitude: -122.4346),
            span: MKCoordinateSpan(latitudeDelta: 0.08, longitudeDelta: 0.08))))

    private let locManager = CLLocationManager()

    var fileName: String {
        let base = destinationName.isEmpty ? "route" : destinationName
        let safe = base.lowercased()
            .replacingOccurrences(of: " ", with: "_")
            .filter { $0.isLetter || $0.isNumber || $0 == "_" }
        return String(safe.prefix(24)) + ".gpx"
    }

    func requestLocation() {
        locManager.requestWhenInUseAuthorization()
    }

    func search() {
        guard !query.isEmpty else { return }
        let request = MKLocalSearch.Request()
        request.naturalLanguageQuery = query
        if let region = currentRegion { request.region = region }
        MKLocalSearch(request: request).start { [weak self] resp, _ in
            Task { @MainActor in self?.results = resp?.mapItems ?? [] }
        }
    }

    func choose(_ item: MKMapItem) {
        destination = item.placemark.coordinate
        destinationName = item.name ?? "Destination"
        results = []
        buildRoute()
    }

    func clearRoute() {
        route = nil
        destination = nil
        destinationName = ""
    }

    private var currentRegion: MKCoordinateRegion? {
        guard let loc = locManager.location else { return nil }
        return MKCoordinateRegion(center: loc.coordinate,
                                  latitudinalMeters: 20_000,
                                  longitudinalMeters: 20_000)
    }

    private func buildRoute() {
        guard let dest = destination, let from = locManager.location else { return }
        let src = from.coordinate
        // Prefer Apple's cycling directions. They're only available where
        // Apple has cycling coverage, so fall back to walking geometry
        // (closest to a bike-friendly path) when cycling returns nothing.
        calcRoute(from: src, to: dest, type: .cycling) { [weak self] r in
            if let r {
                self?.apply(r, mode: "Cycling")
            } else {
                self?.calcRoute(from: src, to: dest, type: .walking) { r2 in
                    if let r2 { self?.apply(r2, mode: "Walking") }
                }
            }
        }
    }

    private func calcRoute(from: CLLocationCoordinate2D,
                           to: CLLocationCoordinate2D,
                           type: MKDirectionsTransportType,
                           done: @escaping (MKRoute?) -> Void) {
        let request = MKDirections.Request()
        request.source = MKMapItem(placemark: MKPlacemark(coordinate: from))
        request.destination = MKMapItem(placemark: MKPlacemark(coordinate: to))
        request.transportType = type
        MKDirections(request: request).calculate { resp, _ in
            Task { @MainActor in done(resp?.routes.first) }
        }
    }

    @MainActor private func apply(_ r: MKRoute, mode: String) {
        route = r
        routeMode = mode
        camera = .rect(r.polyline.boundingMapRect)
    }
}

// MARK: - Subviews

private struct SearchField: View {
    @Binding var text: String
    let onSubmit: () -> Void
    var body: some View {
        HStack {
            Image(systemName: "magnifyingglass").foregroundStyle(Palette.muted)
            TextField("Search destination", text: $text)
                .submitLabel(.search)
                .onSubmit(onSubmit)
        }
        .padding(14)
        .background(Palette.surface)
        .clipShape(RoundedRectangle(cornerRadius: 14))
        .overlay(RoundedRectangle(cornerRadius: 14)
            .strokeBorder(Palette.hairline, lineWidth: 1))
    }
}

private struct ResultsList: View {
    let results: [MKMapItem]
    let onPick: (MKMapItem) -> Void
    var body: some View {
        VStack(spacing: 0) {
            ForEach(results.prefix(4), id: \.self) { item in
                Button { onPick(item) } label: {
                    VStack(alignment: .leading, spacing: 2) {
                        Text(item.name ?? "—")
                            .font(TypeScale.body).foregroundStyle(Palette.ink)
                        if let t = item.placemark.title {
                            Text(t).font(.system(size: 12)).foregroundStyle(Palette.muted)
                                .lineLimit(1)
                        }
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(.vertical, 12).padding(.horizontal, 14)
                }
                if item != results.prefix(4).last { Divider() }
            }
        }
        .background(Palette.surface)
        .clipShape(RoundedRectangle(cornerRadius: 14))
        .overlay(RoundedRectangle(cornerRadius: 14)
            .strokeBorder(Palette.hairline, lineWidth: 1))
    }
}

private struct RouteSummaryCard: View {
    let name: String
    let mode: String
    let distanceKm: Double
    let minutes: Int
    let progress: Double?
    let canSend: Bool
    let send: () -> Void
    let clear: () -> Void
    @AppStorage(UnitPref.key) private var useMiles = false

    var body: some View {
        Card {
            VStack(alignment: .leading, spacing: 14) {
                HStack {
                    VStack(alignment: .leading, spacing: 2) {
                        Text(name).font(TypeScale.title).foregroundStyle(Palette.ink)
                            .lineLimit(1)
                        HStack(spacing: 6) {
                            if !mode.isEmpty {
                                Text(mode.uppercased())
                                    .font(.system(size: 11, weight: .bold))
                                    .padding(.horizontal, 7).padding(.vertical, 3)
                                    .background(mode == "Cycling" ? Palette.good : Palette.muted)
                                    .foregroundStyle(.white)
                                    .clipShape(Capsule())
                            }
                            Text(String(format: "%.1f %@ · %d min",
                                        Units.distance(distanceKm, miles: useMiles),
                                        Units.distLabel(useMiles), minutes))
                                .font(TypeScale.body).foregroundStyle(Palette.muted)
                        }
                    }
                    Spacer()
                    Button(action: clear) {
                        Image(systemName: "xmark.circle.fill")
                            .foregroundStyle(Palette.muted)
                    }
                }
                if let progress {
                    ProgressView(value: progress) {
                        Text("Sending to device…").font(.system(size: 12))
                            .foregroundStyle(Palette.muted)
                    }
                } else {
                    PrimaryButton(title: "Send to device",
                                  systemImage: "arrow.down.circle",
                                  enabled: canSend, action: send)
                }
            }
        }
    }
}

// MARK: - Polyline coordinate extraction

extension MKPolyline {
    var coordinates: [CLLocationCoordinate2D] {
        var coords = [CLLocationCoordinate2D](
            repeating: kCLLocationCoordinate2DInvalid, count: pointCount)
        getCoordinates(&coords, range: NSRange(location: 0, length: pointCount))
        return coords
    }
}
