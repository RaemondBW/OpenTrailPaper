import SwiftUI
import MapKit
import CoreLocation
import Combine

// Draw a box on the map; the app covers it with H3 hexagon tiles (~5.6 km
// across), skips the ones already on the device, fetches OSM for the rest, and
// streams each new tile to the device one at a time. The device stores every
// tile by its H3 id and renders them straight off the SD card, so map coverage
// is limited by the card, not memory — and re-selecting an overlapping area
// only ever sends the genuinely new tiles.
struct MapsView: View {
    @EnvironmentObject var ble: BLEManager
    @Environment(\.dismiss) private var dismiss

    // Res-6 H3 tiles are ~5.6 km across; start wide enough to show at least
    // 3–4 of them across the screen (~0.25° ≈ 22 km) so a whole area is easy to
    // box in one drag. Following the user would snap to MapKit's default
    // street-level zoom (far too tight for tile selection), so we center on the
    // user ourselves once a fix arrives (see `locator`), keeping this fixed
    // span. Falls back to a fixed region until then.
    @State private var camera: MapCameraCommand? =
        MapCameraCommand(target: .region(MKCoordinateRegion(
            center: .init(latitude: 37.7764, longitude: -122.4346),
            span: MKCoordinateSpan(latitudeDelta: 0.25, longitudeDelta: 0.25))))
    @StateObject private var locator = MapLocator()
    @StateObject private var projection = MapProjection()
    @ObservedObject private var store = EInkTileStore.shared
    @State private var visibleRegion: MKCoordinateRegion?
    @State private var einkAreas: [EInkArea] = []
    @State private var outlineHexes: [OutlineHex] = []
    @State private var didCenter = false
    @State private var dragStart: CGPoint?
    @State private var dragEnd: CGPoint?
    @State private var box: (s: Double, w: Double, n: Double, e: Double)?
    @State private var tiles: [MapTile] = []       // covering tiles for the current box
    @State private var building = false
    @State private var status: String?
    @State private var drawMode = false
    @State private var excluded: Set<String> = []   // hexes the user tapped to skip
    // Long-pressed hex: its id, whether the device already has it, and where to
    // put the callout. Useful when a specific hex misbehaves — an ocean tile
    // that will not fill, a hex missing roads — since the id is what names the
    // file on the card (/maps/tiles/<first 6>/<rest>.ebm) and what the tile-list
    // sync talks in.
    @State private var inspected: (id: String, onDevice: Bool)?
    @State private var converted: Set<String> = []  // hexes downloaded + built this run
    @State private var downloadTotal = 0            // hexes targeted this run
    @State private var downloadTask: Task<Void, Never>?

    // Hexes in the drawn box that aren't on the device yet, in whichever state
    // the download has reached. Areas already downloaded are NOT in here — they
    // draw as e-ink underneath, which says "you have this" far better than a
    // selection tint would.
    private var selectionHexes: [SelectionHex] {
        tiles.filter { !ble.deviceTileIds.contains($0.id) }.map { t in
            SelectionHex(id: t.id, hexagon: t.hexagon,
                         kind: converted.contains(t.id) ? .done
                             : excluded.contains(t.id) ? .excluded : .pending)
        }
    }

    // Tiles that will actually be sent: not already on the device and not
    // tapped-out by the user.
    private var newTiles: [MapTile] {
        tiles.filter { !ble.deviceTileIds.contains($0.id) && !excluded.contains($0.id) }
    }
    private var onDeviceCount: Int { tiles.filter { ble.deviceTileIds.contains($0.id) }.count }

    // Toggle whether a tapped hex is included in the download.
    private func toggleHex(at coord: CLLocationCoordinate2D) {
        guard let id = H3Tiles.id(at: coord),
              tiles.contains(where: { $0.id == id }),
              !ble.deviceTileIds.contains(id) else { return }   // can't skip what's on-device
        if excluded.contains(id) { excluded.remove(id) } else { excluded.insert(id) }
    }

    var body: some View {
        NavigationStack {
            ZStack(alignment: .top) {
                EInkMapView(
                    areas: einkAreas,
                    outlines: outlineHexes,
                    selection: selectionHexes,
                    camera: camera,
                    showsUserLocation: ble.locationPermission.isGranted,
                    showsTrackingButton: true,
                    projection: projection,
                    // Tap a hex (once an area is drawn) to skip/keep it.
                    onTap: { c in
                        guard box != nil, !drawMode else { return }
                        toggleHex(at: c)
                    },
                    // Long-press any hex to read its id. Deliberately NOT gated
                    // on an area being drawn — inspecting a hex already on the
                    // device is the more useful case of the two.
                    onLongPress: { c in
                        guard !drawMode, let id = H3Tiles.id(at: c) else { return }
                        inspected = (id, ble.deviceTileIds.contains(id))
                        UIImpactFeedbackGenerator(style: .medium).impactOccurred()
                    },
                    onRegionChange: { r in
                        visibleRegion = r
                        refreshEInk()
                    })
                .ignoresSafeArea(edges: .top)

                // In draw mode a transparent layer captures the drag so the
                // map doesn't pan while you draw a box.
                if drawMode {
                    Rectangle().fill(Color.black.opacity(0.001))
                        .contentShape(Rectangle())
                        .gesture(DragGesture(minimumDistance: 4)
                            .onChanged { g in
                                if dragStart == nil { dragStart = g.startLocation }
                                dragEnd = g.location
                            }
                            .onEnded { _ in
                                if let a = dragStart, let b = dragEnd,
                                   let c1 = projection.coordinate(at: a),
                                   let c2 = projection.coordinate(at: b) {
                                    let bx = (min(c1.latitude, c2.latitude), min(c1.longitude, c2.longitude),
                                              max(c1.latitude, c2.latitude), max(c1.longitude, c2.longitude))
                                    box = bx
                                    tiles = H3Tiles.coveringTiles(south: bx.0, west: bx.1, north: bx.2, east: bx.3)
                                    excluded = []
                                    converted = []
                                    status = nil
                                }
                                dragStart = nil; dragEnd = nil
                                drawMode = false
                            })
                        .ignoresSafeArea(edges: .top)
                }

                if let a = dragStart, let b = dragEnd {
                    Rectangle().fill(Palette.accent.opacity(0.18))
                        .overlay(Rectangle().stroke(Palette.accent, lineWidth: 2))
                        .frame(width: abs(b.x - a.x), height: abs(b.y - a.y))
                        .position(x: (a.x + b.x) / 2, y: (a.y + b.y) / 2)
                        .allowsHitTesting(false)
                }

                header
            }
            .navigationBarHidden(true)
            .sheet(item: Binding(
                get: { inspected.map { TileInspection(id: $0.id, onDevice: $0.onDevice) } },
                set: { if $0 == nil { inspected = nil } }
            )) { info in
                TileInspectorSheet(info: info)
            }
            .safeAreaInset(edge: .bottom) { bottomBar }
            .onAppear {
                ble.refreshDeviceMaps(); ble.refreshDeviceTiles(); locator.start()
                store.refresh()
                fitDownloadedHexes()
            }
            // Redraw when a tile finishes decoding, or when the device's own
            // tile list arrives and flips areas to "synced". Both are also the
            // moment the coverage we want to frame becomes known.
            .onChange(of: store.version) { refreshEInk(); fitDownloadedHexes() }
            .onChange(of: ble.deviceTileIds) { refreshEInk(); fitDownloadedHexes() }
            // Center on the user's first fix, once, at our fixed tile-friendly
            // span. Only before any interaction so it never yanks the map away
            // from a box the user is drawing — and only as the FALLBACK for
            // someone with no coverage yet, since `fitDownloadedHexes` claims
            // `didCenter` the moment it has hexes to frame.
            .onReceive(locator.$coordinate) { coord in
                guard !didCenter, box == nil, !drawMode, let coord else { return }
                didCenter = true
                camera = MapCameraCommand(target: .region(MKCoordinateRegion(center: coord,
                    span: MKCoordinateSpan(latitudeDelta: 0.25, longitudeDelta: 0.25))))
            }
        }
    }

    /// Ask the store what to draw for the region now on screen.
    private func refreshEInk() {
        guard let r = visibleRegion else { return }
        let content = store.visibleContent(in: r, synced: ble.deviceTileIds)
        einkAreas = content.areas
        outlineHexes = content.outlines
    }

    /// Frame ALL the coverage — everything the phone holds plus everything the
    /// device holds — the first time this screen has something to frame.
    ///
    /// Managing downloaded areas starts with seeing them, and the old opening
    /// shot (a fixed ~22 km box on the user) showed one screenful of a
    /// collection that can span a country: the rest was off-map with nothing to
    /// say it existed. Runs once — `didCenter` is shared with the locate-the-
    /// user fallback, so whichever gets there first wins and neither one ever
    /// moves the camera under a box being drawn.
    private func fitDownloadedHexes() {
        guard !didCenter, box == nil, !drawMode else { return }
        let ids = store.ids.union(ble.deviceTileIds)
        guard !ids.isEmpty else { return }

        var rect = MKMapRect.null
        for id in ids {
            guard let t = H3Tiles.tile(id: id) else { continue }
            for c in t.hexagon {
                let p = MKMapPoint(c)
                rect = rect.union(MKMapRect(origin: p, size: MKMapSize(width: 0, height: 0)))
            }
        }
        guard !rect.isNull, rect.size.width > 0, rect.size.height > 0 else { return }
        didCenter = true
        // Less padding than a route gets: the hexes ARE the subject here, and
        // the bottom card already eats the lower edge of the map.
        camera = MapCameraCommand(target: .rect(rect.paddedForDisplay(fraction: 0.12)))
    }

    private var header: some View {
        HStack {
            Text("Maps").font(BarlowFont.condensed(22, .bold)).foregroundStyle(Palette.ink)
                .padding(.horizontal, 16).padding(.vertical, 9)
                .background(Palette.surface).clipShape(Capsule())
                .overlay(Capsule().strokeBorder(Palette.hairline, lineWidth: 1))
            Spacer()
            Button(drawMode ? "Cancel" : "Select area") {
                box = nil; tiles = []; excluded = []; converted = []; dragStart = nil; dragEnd = nil
                drawMode.toggle()
            }
            .font(BarlowFont.condensed(18, .semibold))
            .foregroundStyle(drawMode ? Palette.accentInk : Palette.accent)
            .padding(.horizontal, 16).padding(.vertical, 9)
            .background(drawMode ? Palette.accent : Palette.surface).clipShape(Capsule())
            .overlay(Capsule().strokeBorder(Palette.hairline, lineWidth: 1))
            Button("Done") { dismiss() }
                .font(BarlowFont.condensed(18, .semibold)).foregroundStyle(Palette.accent)
                .padding(.horizontal, 16).padding(.vertical, 9)
                .background(Palette.surface).clipShape(Capsule())
                .overlay(Capsule().strokeBorder(Palette.hairline, lineWidth: 1))
        }
        .padding(.horizontal, 16).padding(.top, 8)
        .shadow(color: .black.opacity(0.08), radius: 6, y: 2)
    }

    // A floating "modal" card at the bottom. Progress states show a spinner +
    // live hex counts; idle/selection states show the controls.
    @ViewBuilder private var bottomBar: some View {
        Group {
            if building || ble.tilesUploading {
                streamCard
            } else if let b = box {
                selectionCard(b)
            } else {
                hintCard
            }
        }
        .padding(.horizontal, 16)
        .padding(.bottom, 6)
    }

    // Download + vectorize + send all run in parallel, so one card shows the
    // fetch stage AND the live send progress.
    private var streamCard: some View {
        let sent = ble.tilesDone
        let total = max(ble.tilesTotal, downloadTotal, 1)
        return card {
            VStack(alignment: .leading, spacing: 10) {
                HStack(spacing: 12) {
                    ProgressView().tint(Palette.accent)
                    VStack(alignment: .leading, spacing: 2) {
                        Text(building ? "Downloading maps" : "Sending to device")
                            .font(BarlowFont.condensed(19, .semibold)).foregroundStyle(Palette.ink)
                        Text(building ? (status ?? "Fetching…")
                                      : (ble.tileMessage ?? "Uploading hexes…"))
                            .font(BarlowFont.text(12)).foregroundStyle(Palette.muted).lineLimit(1)
                    }
                    Spacer(minLength: 6)
                    Button("Cancel") { cancelDownload() }
                        .font(BarlowFont.text(14, .semibold)).foregroundStyle(Palette.accent)
                }
                ProgressView(value: Double(sent), total: Double(total)).tint(Palette.good)
                Text("\(sent) of \(total) hexes sent"
                     + (building && converted.count > sent ? " · \(converted.count) built" : ""))
                    .font(BarlowFont.text(12, .semibold)).foregroundStyle(Palette.good)
            }
        }
    }

    private func selectionCard(_ b: (s: Double, w: Double, n: Double, e: Double)) -> some View {
        let new = newTiles.count
        let skipped = excluded.intersection(tiles.map(\.id)).count
        return card {
            VStack(alignment: .leading, spacing: 10) {
                HStack {
                    VStack(alignment: .leading, spacing: 1) {
                        Text("Selected area").trackedLabel()
                        Text(areaText(b)).font(BarlowFont.text(15, .semibold)).foregroundStyle(Palette.ink)
                    }
                    Spacer()
                    Button { box = nil; tiles = []; excluded = []; converted = [] } label: {
                        Image(systemName: "xmark.circle.fill").foregroundStyle(Palette.muted)
                    }
                }
                Text("\(new) to download · \(onDeviceCount) on device"
                     + (skipped > 0 ? " · \(skipped) skipped" : ""))
                    .font(BarlowFont.text(12)).foregroundStyle(Palette.muted)
                    .frame(maxWidth: .infinity, alignment: .leading)
                Text("Tap a hex to skip it (or add it back).")
                    .font(BarlowFont.text(11)).foregroundStyle(Palette.faint)
                    .frame(maxWidth: .infinity, alignment: .leading)
                PrimaryButton(title: !ble.canUploadMap ? "Connect device to send"
                                    : new == 0 ? "Nothing to download"
                                    : "Download \(new) hex\(new == 1 ? "" : "es")",
                              systemImage: "arrow.down.circle",
                              enabled: ble.canUploadMap && new > 0) { download() }
                if let s = status { Text(s).font(BarlowFont.text(12)).foregroundStyle(Palette.accent) }
            }
        }
    }

    private var hintCard: some View {
        card {
            VStack(alignment: .leading, spacing: 3) {
                Text(drawMode ? "Drag a box across the area you want."
                              : "Tap “Select area”, then drag a box. Areas drawn like the device’s screen are downloaded; a green check means the device has them too.")
                    .font(BarlowFont.text(14)).foregroundStyle(drawMode ? Palette.accent : Palette.muted)
                    .frame(maxWidth: .infinity, alignment: .leading)
                if !ble.deviceTileIds.isEmpty {
                    Text("\(ble.deviceTileIds.count) hexes on the device")
                        .font(BarlowFont.text(11)).foregroundStyle(Palette.muted)
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
            }
        }
    }

    private func card<Content: View>(@ViewBuilder _ content: () -> Content) -> some View {
        content()
            .padding(16)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(Palette.surface, in: RoundedRectangle(cornerRadius: 20, style: .continuous))
            .overlay(RoundedRectangle(cornerRadius: 20, style: .continuous)
                .strokeBorder(Palette.hairline, lineWidth: 1))
            .shadow(color: .black.opacity(0.14), radius: 14, y: 5)
    }

    // MARK: geometry helpers

    private func spanKm(_ b: (s: Double, w: Double, n: Double, e: Double)) -> (Double, Double) {
        let latKm = (b.n - b.s) * 111.0
        let lonKm = (b.e - b.w) * 111.0 * cos((b.s + b.n) / 2 * .pi / 180)
        return (latKm, lonKm)
    }
    private func areaText(_ b: (s: Double, w: Double, n: Double, e: Double)) -> String {
        let (a, c) = spanKm(b)
        return String(format: "%.1f × %.1f km", c, a)
    }

    // MARK: download

    private func download() {
        let missing = newTiles
        guard !missing.isEmpty else { return }
        building = true
        converted = []
        downloadTotal = missing.count
        status = "Fetching map data…"

        // Group tiles into bounded OSM fetches (~0.08° ≈ 9 km) so each Overpass
        // query stays light — big queries 504 on the busy public servers. Each
        // batch is one call (retried across mirrors in MapBuilder.fetchOSM).
        let batches = Dictionary(grouping: missing) { t -> String in
            let clat = (t.south + t.north) / 2, clon = (t.west + t.east) / 2
            return "\(Int((clat / 0.08).rounded(.down)))_\(Int((clon / 0.08).rounded(.down)))"
        }.map { $0.value }

        ble.startTileStream()            // begin sending as tiles are produced
        downloadTask = Task {
            var anyBuilt = false
            do {
                // Anything built before goes straight out — no Overpass, no
                // elevation fetch, no re-encoding. This is what makes a retry
                // after a dropped link cheap instead of a full rebuild.
                let (cached, _) = await TileCache.shared.partition(missing.map(\.id))
                if !cached.isEmpty {
                    status = "Reusing \(cached.count) cached tile\(cached.count == 1 ? "" : "s")…"
                    converted.formUnion(cached.map(\.id))
                    anyBuilt = true
                    ble.enqueueTiles(cached)
                    store.noteDownloaded(cached.map(\.id))
                }
                // ONE coastline fetch for the whole selection, padded well past
                // it. Sea fill needs the COAST, and an ocean-only selection does
                // not contain any — the batch bbox for hexes out in open water
                // has no coastline in it, so no rings could be assembled and
                // those tiles came out blank. Padding by ~0.35 deg (~35 km)
                // reaches the shore from anywhere a rider would sensibly select.
                // Coastline-only, so widening it is cheap.
                let all = union(missing)
                let pad = 0.35
                status = "Fetching coastline…"
                let coastJSON = try? await MapBuilder.fetchCoastline(
                    south: all.s - pad, west: all.w - pad,
                    north: all.n + pad, east: all.e + pad)
                let coastChains = coastJSON.flatMap {
                    try? MapBuilder.extractCoastlineChains(regionJSON: $0)
                } ?? []

                let cachedIds = Set(cached.map(\.id))
                let batches = batches
                    .map { $0.filter { !cachedIds.contains($0.id) } }
                    .filter { !$0.isEmpty }

                for (i, batch) in batches.enumerated() {
                    if i > 0 { try? await Task.sleep(nanoseconds: 1_000_000_000) }  // pace the servers
                    try Task.checkCancellation()
                    let n = batches.count
                    status = "Fetching area \(i + 1)/\(n)…"
                    let u = union(batch)
                    // Coalesce the fetch progress text. It arrives many times a
                    // second, and every distinct value re-evaluates this view's
                    // body; the map itself is insulated now (HexMap is Equatable)
                    // but the rest of the body need not churn either.
                    let tick = StatusThrottle()
                    let json = try await MapBuilder.fetchOSM(south: u.s, west: u.w, north: u.n, east: u.e) { m in
                        let text = "Fetching area \(i + 1)/\(n) — \(m)"
                        Task { @MainActor in if tick.allow(text) { status = text } }
                    }
                    status = "Building tiles \(i + 1)/\(n)…"
                    let part = try MapBuilder.encodeTiles(regionJSON: json, tiles: batch)
                    // natural=water polygons for this region, parsed once and
                    // appended per tile as a WTR2 section (after ELV1).
                    let waterWays = (try? MapBuilder.extractWaterWays(regionJSON: json)) ?? []
                    // Coastline sea-fill: assemble the ocean/bay as sea rings ONCE
                    // for the whole fetch region (osmcoastline-style — the real
                    // topology, so a peninsula never encloses land), then clip
                    // each ring to the tile inside appendWater.
                    // Rings are assembled against the PADDED region, not this
                    // batch's bbox, so a batch sitting entirely offshore is still
                    // inside a ring and fills.
                    let seaRings = MapBuilder.regionSeaPolygons(coastChains,
                        south: all.s - pad, west: all.w - pad,
                        north: all.n + pad, east: all.e + pad)
                    // Parks / green areas for this region, appended per tile as a
                    // PRK2 section (after WTR2).
                    let parkWays = (try? MapBuilder.extractParkWays(regionJSON: json)) ?? []
                    // Bake a DEM elevation grid into each tile (best-effort) so
                    // the device has elevation without GPS altitude or the phone.
                    status = "Elevation \(i + 1)/\(n)…"
                    var withElev: [(id: String, data: Data)] = []
                    for var p in part {
                        if let t = batch.first(where: { $0.id == p.id }) {
                            if let grid = try? await MapBuilder.fetchElevationGrid(
                                    south: t.south, west: t.west, north: t.north, east: t.east) {
                                MapBuilder.appendElevation(to: &p.data, south: t.south, west: t.west,
                                    north: t.north, east: t.east, grid: grid, n: MapBuilder.elevationGrid)
                            }
                            // WTR2 water section after any ELV1 block, then PRK2.
                            MapBuilder.appendWater(to: &p.data, waterWays: waterWays,
                                seaRings: seaRings,
                                south: t.south, west: t.west, north: t.north, east: t.east)
                            MapBuilder.appendParks(to: &p.data, parkWays: parkWays,
                                south: t.south, west: t.west, north: t.north, east: t.east)
                        }
                        // Decide emptiness only now, with water/parks/sea/elevation
                        // already appended — a hex can be pure water and still be
                        // worth storing.
                        if let t = batch.first(where: { $0.id == p.id }),
                           MapBuilder.isEmpty(p.data, tile: t) { continue }
                        withElev.append(p)
                    }
                    converted.formUnion(batch.map(\.id))   // fill these hexes in live
                    if !withElev.isEmpty { anyBuilt = true }
                    // Cache BEFORE sending: if the link drops mid-transfer the
                    // expensive work survives and the retry is instant.
                    await TileCache.shared.store(withElev)
                    // Draw the new areas in the device's style straight away —
                    // the download IS what "downloaded" means on this map.
                    store.noteDownloaded(withElev.map(\.id))
                    ble.enqueueTiles(withElev)             // send in parallel with the next fetch
                }
                building = false
                ble.finishTileStream()                     // let the queue drain
                status = anyBuilt ? nil : "No roads found in that area."
            } catch is CancellationError {
                building = false
                ble.cancelTileUpload()
                status = "Canceled"
            } catch {
                building = false
                ble.finishTileStream()                     // send whatever built before the error
                status = error.localizedDescription
            }
        }
    }

    private func cancelDownload() {
        downloadTask?.cancel()
        downloadTask = nil
        building = false
        ble.cancelTileUpload()
        status = "Canceled"
    }

    // Bounding box enclosing a set of tiles, padded slightly so roads at tile
    // edges are present in the fetch.
    private func union(_ ts: [MapTile]) -> (s: Double, w: Double, n: Double, e: Double) {
        var s = 90.0, w = 180.0, n = -90.0, e = -180.0
        for t in ts { s = min(s, t.south); w = min(w, t.west); n = max(n, t.north); e = max(e, t.east) }
        let pad = 0.003
        return (s - pad, w - pad, n + pad, e + pad)
    }
}

// Publishes the user's location so the map can center itself at a fixed,
// tile-friendly zoom on the first fix. (MapKit's `.userLocation` follow mode
// gives no control over the zoom, so we position the camera ourselves.)
@MainActor final class MapLocator: NSObject, ObservableObject, CLLocationManagerDelegate {
    @Published var coordinate: CLLocationCoordinate2D?
    private let manager = CLLocationManager()

    override init() {
        super.init()
        manager.delegate = self
    }

    func start() {
        manager.requestWhenInUseAuthorization()
        manager.requestLocation()
        if let c = manager.location?.coordinate { coordinate = c }
    }

    nonisolated func locationManager(_ m: CLLocationManager, didUpdateLocations locs: [CLLocation]) {
        guard let c = locs.last?.coordinate else { return }
        Task { @MainActor in self.coordinate = c }
    }

    nonisolated func locationManager(_ m: CLLocationManager, didFailWithError error: Error) {}
}


/// Rate-limits a status string so a chatty progress callback cannot drive one
/// SwiftUI state update per network chunk.
private final class StatusThrottle: @unchecked Sendable {
    private var last = Date.distantPast
    private var lastText = ""
    func allow(_ text: String, minInterval: TimeInterval = 0.3) -> Bool {
        if text == lastText { return false }
        let now = Date()
        guard now.timeIntervalSince(last) >= minInterval else { return false }
        last = now; lastText = text
        return true
    }
}

/// A hex the user long-pressed, for the inspector sheet.
struct TileInspection: Identifiable {
    let id: String
    let onDevice: Bool
}

/// Shows a hex's H3 id and where it lives on the card.
///
/// The id is the thing every other part of the system names a tile by: the
/// filename on the SD card, the tile-list the app and device reconcile, and what
/// a diag log prints. When one specific hex misbehaves — an ocean tile that will
/// not fill, a hex with no roads — being able to read its id off the map turns
/// "somewhere around here" into something greppable.
private struct TileInspectorSheet: View {
    let info: TileInspection
    @Environment(\.dismiss) private var dismiss

    /// Matches src/map_store.cpp: /maps/tiles/<first 6>/<rest>.ebm
    private var cardPath: String {
        guard info.id.count > 6 else { return "/maps/tiles/\(info.id).ebm" }
        let cut = info.id.index(info.id.startIndex, offsetBy: 6)
        return "/maps/tiles/\(info.id[..<cut])/\(info.id[cut...]).ebm"
    }

    var body: some View {
        NavigationStack {
            List {
                Section("H3 cell") {
                    HStack {
                        Text(info.id).font(.system(.body, design: .monospaced))
                        Spacer()
                        Button {
                            UIPasteboard.general.string = info.id
                        } label: { Image(systemName: "doc.on.doc") }
                            .buttonStyle(.borderless)
                    }
                }
                Section("On the SD card") {
                    Text(cardPath).font(.system(.footnote, design: .monospaced))
                        .foregroundStyle(.secondary)
                }
                Section("Status") {
                    Label(info.onDevice ? "On the device" : "Not on the device",
                          systemImage: info.onDevice ? "checkmark.circle.fill"
                                                     : "circle.dashed")
                        .foregroundStyle(info.onDevice ? Palette.good : Palette.muted)
                }
            }
            .navigationTitle("Tile")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") { dismiss() }
                }
            }
        }
        .presentationDetents([.medium])
    }
}
