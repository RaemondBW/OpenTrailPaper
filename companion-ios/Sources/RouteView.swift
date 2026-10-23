import SwiftUI
import MapKit
import CoreLocation
import UniformTypeIdentifiers

// Search a destination, build a cycling route with MapKit (or pick one saved
// on Strava / RideWithGPS), preview it, and send it to the head unit as GPX
// over BLE.
struct RouteView: View {
    @EnvironmentObject var ble: BLEManager
    @StateObject private var model = RouteModel()
    @ObservedObject private var store = EInkTileStore.shared
    @ObservedObject private var sync = SyncAccounts.shared
    @AppStorage(UnitPref.key) private var useMiles = false
    @State private var showSaved = false
    /// "Import a route": the saved routes of a connected account.
    @State private var showCloud = false
    /// "Import a route": a .gpx file from Files.
    @State private var showGPX = false
    @State private var gpxError: String?
    @State private var visibleRegion: MKCoordinateRegion?
    /// Hexes the planned route crosses that NOTHING covers — see `coverageGaps`.
    @State private var gapHexes: [OutlineHex] = []

    private var hasRoute: Bool { model.route != nil || model.imported != nil }

    var body: some View {
        NavigationStack {
            VStack(spacing: 0) {
                ScreenHeader(title: "Route") {
                    HStack(spacing: 8) {
                        SquareIconButton(systemName: "folder", label: "Routes on the device",
                                         enabled: ble.state == .connected) {
                            ble.refreshRoutes(); showSaved = true
                        }
                        importMenu {
                            Image(systemName: "plus")
                                .font(.system(size: 18, weight: .bold))
                                .foregroundStyle(Palette.ink)
                                .frame(width: 44, height: 44)
                                .einkBorder()
                                .contentShape(Rectangle())
                        }
                        .accessibilityLabel("Import a route")
                    }
                }

                ZStack(alignment: .top) {
                    // Same map component as the Maps screen, but showing only the
                    // gaps: this page is for finding a destination and building a
                    // route, and plain Apple Maps reads better for that.
                    EInkMapView(
                        // Only the GAP hexes here — the ground a planned route
                        // crosses that nothing covers. That is the question this
                        // page is for ("will I ride off my maps?"); ordinary
                        // coverage would just be noise over the route.
                        outlines: gapHexes,
                        route: model.route?.polyline ?? model.importedPolyline,
                        destination: model.destination.map {
                            MapDestination(name: model.destinationName, coordinate: $0)
                        },
                        camera: model.camera,
                        showsUserLocation: ble.locationPermission.isGranted,
                        onRegionChange: { r in
                            visibleRegion = r
                            refreshCoverage()
                        })
                    .accessibilityLabel("Map")

                    HStack(alignment: .top, spacing: 8) {
                        if hasRoute { nameOverlay } else { Spacer() }
                        if hasRoute { Spacer(minLength: 0) }
                        SquareIconButton(systemName: "location.fill", label: "Show my location") {
                            model.recenter()
                        }
                    }
                    .padding(12)

                    if !hasRoute && !model.results.isEmpty {
                        VStack {
                            Spacer()
                            ResultsList(results: model.results) { model.choose($0) }
                                .padding(12)
                        }
                    }
                }
                .frame(maxHeight: .infinity)
                .clipped()

                Rectangle().fill(Palette.ink).frame(height: Rule.title)

                VStack(spacing: 10) {
                    if hasRoute {
                        statsBox
                        sendControls
                    } else {
                        // Hidden while a route is up: a search would replace
                        // it, and the name box's clear button is the way back.
                        SearchField(text: $model.query) { model.search() }
                    }
                    importMenu {
                        BlockButtonLabel(title: importTitle, filled: false, expand: true)
                    }
                }
                .padding(.horizontal, 16).padding(.vertical, 12)
            }
            .background(Palette.paper.ignoresSafeArea())
            .navigationBarHidden(true)
            .sheet(isPresented: $showSaved) { SavedRoutesSheet() }
            .sheet(isPresented: $showCloud) {
                CloudRoutesSheet { r in
                    ble.routeSent = false; ble.routeReceived = false
                    model.importRoute(r)
                }
            }
            .fileImporter(isPresented: $showGPX,
                          allowedContentTypes: [UTType(filenameExtension: "gpx") ?? .xml, .xml]) { result in
                importGPX(result)
            }
            .alert("Couldn't import that file", isPresented: Binding(
                get: { gpxError != nil }, set: { if !$0 { gpxError = nil } })) {
                Button("OK", role: .cancel) {}
            } message: { Text(gpxError ?? "") }
            .onAppear {
                if ProcessInfo.processInfo.arguments.contains("-demo-route") { model.demoRoute() }
                else { model.requestLocation() }
                store.refresh()
            }
            .onChange(of: model.destinationName) {
                ble.routeSent = false; ble.routeReceived = false
            }
            .onChange(of: store.version) { refreshCoverage(); recomputeCoverage() }
            .onChange(of: ble.deviceTileIds) { refreshCoverage(); recomputeCoverage() }
            // The route itself is the trigger that matters: a new route can walk
            // straight off the downloaded area, and clearing one must take the
            // hexagons away again.
            .onChange(of: model.route) { recomputeCoverage() }
            .onChange(of: model.imported) { recomputeCoverage() }
        }
    }

    private var importTitle: String {
        "Import from Strava · RWGPS · GPX"
    }

    /// Both import buttons open the same choice: a saved route from a
    /// connected account, or a .gpx file.
    private func importMenu<L: View>(@ViewBuilder label: () -> L) -> some View {
        Menu {
            Button { showCloud = true } label: {
                Label("Strava or Ride with GPS route", systemImage: "point.topleft.down.to.point.bottomright.curvepath")
            }
            Button { showGPX = true } label: {
                Label("GPX file", systemImage: "doc")
            }
        } label: { label() }
        .buttonStyle(.plain)
    }

    private func importGPX(_ result: Result<URL, Error>) {
        guard case .success(let url) = result else { return }
        let scoped = url.startAccessingSecurityScopedResource()
        defer { if scoped { url.stopAccessingSecurityScopedResource() } }
        guard let data = try? Data(contentsOf: url) else {
            gpxError = "The file couldn't be read."; return
        }
        switch GPXImporter.parse(data, fallbackName: url.deletingPathExtension().lastPathComponent) {
        case .success(var route):
            route.source = "GPX file"
            ble.routeSent = false; ble.routeReceived = false
            model.importRoute(route)
        case .failure(.noPoints): gpxError = "That file has no route points in it."
        case .failure(.notGPX): gpxError = "That file isn't GPX."
        }
    }

    // MARK: Route on the map

    /// The bordered name box over the map's top-left corner.
    private var nameOverlay: some View {
        HStack(alignment: .top, spacing: 8) {
            VStack(alignment: .leading, spacing: 1) {
                Text(routeName.uppercased())
                    .font(BarlowFont.text(14, .bold)).tracking(2)
                    .lineLimit(2)
                Text(routeSubtitle).font(BarlowFont.text(12, .semibold)).lineLimit(1)
            }
            Button { model.clearRoute() } label: {
                Image(systemName: "xmark").font(.system(size: 12, weight: .bold))
                    .frame(width: 32, height: 32)
                    .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .accessibilityLabel("Clear route")
        }
        .foregroundStyle(Palette.ink)
        .padding(.leading, 10).padding(.vertical, 6).padding(.trailing, 2)
        .background(Palette.paper)
        .einkBorder()
    }

    private var routeName: String {
        model.imported?.name ?? model.destinationName
    }

    private var distanceKm: Double {
        if let r = model.route { return r.distance / 1000 }
        return (model.imported?.distanceMeters ?? 0) / 1000
    }

    private var routeSubtitle: String {
        var parts: [String] = []
        if let src = model.imported?.source { parts.append("from \(src)") }
        else if !model.routeMode.isEmpty { parts.append(model.routeMode.lowercased()) }
        let mode = model.imported?.mode ?? ""
        if model.imported != nil && !mode.isEmpty { parts.append(mode.lowercased()) }
        parts.append(String(format: "%.1f %@", Units.distance(distanceKm, miles: useMiles), Units.distLabel(useMiles)))
        if let climb = model.imported?.climbMeters {
            parts.append("\(Int(Units.elevation(climb, miles: useMiles).rounded())) \(Units.elevLabel(useMiles))")
        }
        return parts.joined(separator: " · ")
    }

    // MARK: Panel under the map

    /// Distance · climb (or riding time for a planned route) · map coverage.
    private var statsBox: some View {
        HStack(spacing: 0) {
            stat("Distance", String(format: "%.1f", Units.distance(distanceKm, miles: useMiles)),
                 Units.distLabel(useMiles))
            Rectangle().fill(Palette.ink).frame(width: Rule.divider)
            if let r = model.route {
                stat("Time", "\(Int(r.expectedTravelTime / 60))", "min")
            } else {
                stat("Climb", model.imported?.climbMeters
                        .map { "\(Int(Units.elevation($0, miles: useMiles).rounded()))" } ?? "--",
                     Units.elevLabel(useMiles))
            }
            Rectangle().fill(Palette.ink).frame(width: Rule.divider)
            stat("Map tiles", tilesState, "")
        }
        .fixedSize(horizontal: false, vertical: true)
        .einkBorder()
    }

    /// READY when every hex the route crosses is downloaded; otherwise how
    /// many are missing (drawn on the map), or NONE with nothing downloaded.
    private var tilesState: String {
        if store.ids.union(ble.deviceTileIds).isEmpty { return "NONE" }
        return gapHexes.isEmpty ? "READY" : "\(gapHexes.count) GAPS"
    }

    private func stat(_ label: String, _ value: String, _ unit: String) -> some View {
        VStack(alignment: .leading, spacing: 0) {
            Text(label).trackedLabel().lineLimit(1).minimumScaleFactor(0.7)
            HStack(alignment: .firstTextBaseline, spacing: 3) {
                Text(value).font(DisplayFont.anton(26)).foregroundStyle(Palette.ink)
                    .lineLimit(1).minimumScaleFactor(0.5)
                if !unit.isEmpty {
                    Text(unit.uppercased()).font(BarlowFont.text(11, .bold)).foregroundStyle(Palette.ink)
                }
            }
        }
        .padding(.horizontal, 10).padding(.vertical, 8)
        .frame(maxWidth: .infinity, alignment: .leading)
        .accessibilityElement(children: .combine)
    }

    @ViewBuilder private var sendControls: some View {
        let canSend = ble.state == .connected
        if let progress = ble.lastUploadProgress {
            VStack(alignment: .leading, spacing: 6) {
                Text("SENDING TO DEVICE…").font(BarlowFont.text(12, .bold)).tracking(2)
                BlockProgressBar(value: progress, segments: 12)
            }
            .frame(minHeight: 52)
        } else if ble.routeSent {
            HStack(spacing: 10) {
                // "Received" once the device confirms (0x23); until then
                // (or on older firmware that never acks) show "Sent".
                Text(ble.routeReceived ? "✓ RECEIVED BY DEVICE" : "✓ SENT TO DEVICE")
                    .font(DisplayFont.anton(18)).tracking(1.5)
                    .foregroundStyle(Palette.paper)
                    .frame(maxWidth: .infinity, minHeight: 52)
                    .background(Palette.ink)
                Button { sendRoute() } label: {
                    BlockButtonLabel(title: "Again", filled: false).frame(minHeight: 52)
                }
                .buttonStyle(.plain)
                .disabled(!canSend)
                .accessibilityLabel("Send again")
            }
        } else {
            PrimaryButton(title: canSend ? "Send to device" : "Connect to send",
                          enabled: canSend) { sendRoute() }
        }
    }

    private func sendRoute() {
        if let route = model.route {
            let coords = route.polyline.coordinates
            let gpx = GPXExporter.make(name: model.fileName, coordinates: coords)
            // Turn cues from the route steps: the maneuver point is each
            // step's first coordinate; skip the "proceed to…" opener with no
            // real instruction.
            let turns = route.steps.compactMap { step -> BLEManager.Maneuver? in
                let text = step.instructions.trimmingCharacters(in: .whitespaces)
                guard !text.isEmpty, let c = step.polyline.coordinates.first else { return nil }
                return BLEManager.Maneuver(lat: c.latitude, lon: c.longitude, text: text)
            }
            ble.sentRouteCoords = coords
            ble.uploadRoute(name: model.fileName, gpx: gpx, maneuvers: turns)
        } else if let imported = model.imported {
            // Someone else's route: no time estimate and no turn cues, sent
            // through the same GPX path as one built here.
            let gpx = GPXExporter.make(name: model.fileName, coordinates: imported.coordinates)
            ble.sentRouteCoords = imported.coordinates
            ble.uploadRoute(name: model.fileName, gpx: gpx)
        }
    }

    /// Nothing to refresh here: this screen draws plain Apple Maps plus the
    /// coverage GAPS, and those come from the route rather than the viewport
    /// (`recomputeCoverage`). Kept as a no-op so the region-change plumbing
    /// stays in one shape with MapsView.
    private func refreshCoverage() {}

    /// The hexes the route crosses that neither the phone nor the device holds.
    ///
    /// Tested against the H3 cell of each route point rather than the hexes
    /// currently on screen: coverage is a property of the whole route, not of
    /// the part the camera happens to frame, so panning must not change the
    /// answer. `store.ids` is what the phone holds, `deviceTileIds` what the
    /// head unit holds — riding into either is covered, since the device draws
    /// from its own card.
    private func recomputeCoverage() {
        guard let coords = model.route?.polyline.coordinates ?? model.imported?.coordinates,
              !coords.isEmpty else {
            gapHexes = []          // no route, nothing to warn about
            return
        }
        let covered = store.ids.union(ble.deviceTileIds)
        guard !covered.isEmpty else {
            // Nothing downloaded at all. Papering the whole route in hexagons
            // would say only what the empty Maps screen already says.
            gapHexes = []
            return
        }
        var seen = Set<String>()
        gapHexes = coords.compactMap { c in
            guard let id = H3Tiles.id(at: c), !covered.contains(id),
                  seen.insert(id).inserted, let t = H3Tiles.tile(id: id)
            else { return nil }
            return OutlineHex(id: id, hexagon: t.hexagon, center: t.center,
                              synced: false, missing: true)
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
    /// A route picked from Strava / RideWithGPS, shown instead of `route`.
    @Published private(set) var imported: ImportedRoute?
    @Published private(set) var importedPolyline: MKPolyline?
    // Starts on a fixed region and switches to following the user as soon as a
    // fix exists (see requestLocation/recenter) — the map has no fallback mode
    // of its own, so the camera is always something we chose.
    @Published var camera: MapCameraCommand? = MapCameraCommand(
        target: .region(MKCoordinateRegion(
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
        // Frame the user as soon as we have them, instead of sitting on the
        // fallback region until the first tap of "recenter".
        if let loc = locManager.location {
            camera = MapCameraCommand(target: .region(MKCoordinateRegion(
                center: loc.coordinate,
                span: MKCoordinateSpan(latitudeDelta: 0.08, longitudeDelta: 0.08))))
        }
    }

    // Recenter/follow the user (replaces the default map location button, which
    // sat under the status bar on the full-bleed map).
    func recenter() {
        locManager.requestWhenInUseAuthorization()
        if let loc = locManager.location {
            camera = MapCameraCommand(target: .region(MKCoordinateRegion(center: loc.coordinate,
                span: MKCoordinateSpan(latitudeDelta: 0.05, longitudeDelta: 0.05))))
        } else {
            camera = MapCameraCommand(target: .followUser)
        }
    }

    // Screenshot demo: build a fixed SF route without needing a live location.
    func demoRoute() {
        let from = CLLocationCoordinate2D(latitude: 37.7690, longitude: -122.4830)
        let dest = CLLocationCoordinate2D(latitude: 37.8079, longitude: -122.4177)
        destination = dest
        destinationName = "Fisherman's Wharf"
        calcRoute(from: from, to: dest, type: .cycling) { [weak self] r in
            if let r { self?.apply(r, mode: "Cycling") }
            else { self?.calcRoute(from: from, to: dest, type: .walking) { r2 in
                if let r2 { self?.apply(r2, mode: "Walking") } } }
        }
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

    func importRoute(_ r: ImportedRoute) {
        route = nil
        results = []
        destination = nil
        destinationName = r.name
        imported = r
        var coords = r.coordinates
        let line = MKPolyline(coordinates: &coords, count: coords.count)
        importedPolyline = line
        camera = MapCameraCommand(target: .rect(line.boundingMapRect.paddedForDisplay()))
    }

    func clearRoute() {
        imported = nil
        importedPolyline = nil
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
        imported = nil
        importedPolyline = nil
        route = r
        routeMode = mode
        // Pad the bounding rect before framing. .rect() fits the polyline edge
        // to edge, so the route ran into the screen borders and under the
        // search field and the route summary — hard to read, and impossible to
        // see what it passes near. 25% on each axis gives it room to breathe.
        camera = MapCameraCommand(target: .rect(r.polyline.boundingMapRect.paddedForDisplay()))
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
        .font(BarlowFont.text(16, .medium))
        .padding(.horizontal, 12).frame(minHeight: 44)
        .background(Palette.surface)
        .einkBorder()
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
                if item != results.prefix(4).last { GroupedDivider() }
            }
        }
        .background(Palette.surface)
        .clipShape(Rectangle())
        .overlay(Rectangle()
            .strokeBorder(Palette.ink, lineWidth: 2))
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

extension MKMapRect {
    /// Grow the rect by a fraction of its own size on each axis, so a route
    /// framed with `.rect(...)` is not flush against the screen edges or hidden
    /// behind the search field and summary overlays.
    ///
    /// A minimum span keeps very short routes (a few hundred metres) from
    /// filling the screen at street level, where the surrounding context that
    /// makes a route legible is all off-screen.
    func paddedForDisplay(fraction: Double = 0.25,
                          minimumSpan: Double = 2_000) -> MKMapRect {
        let padX = max(width * fraction, (minimumSpan - width) / 2)
        let padY = max(height * fraction, (minimumSpan - height) / 2)
        return insetBy(dx: -max(padX, 0), dy: -max(padY, 0))
    }
}
