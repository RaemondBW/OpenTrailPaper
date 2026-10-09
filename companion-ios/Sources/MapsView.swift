import SwiftUI
import MapKit
import CoreLocation
import Combine

/// Height of the floating bottom card, reported up so the map can inset for it.
private struct CardHeightKey: PreferenceKey {
    static let defaultValue: CGFloat = 0
    static func reduce(value: inout CGFloat, nextValue: () -> CGFloat) {
        value = max(value, nextValue())
    }
}

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
    @State private var outlineHexes: [OutlineHex] = []
    /// The locator's fallback shot has been taken.
    @State private var didLocate = false
    /// All the coverage has been framed — the opening shot this screen wants.
    @State private var didFrameCoverage = false
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
    // Hexes the run could not produce (no OSM data returned, or nothing to
    // encode). Tracked so the run can say what is missing instead of reporting
    // a clean finish over a hole in the map.
    @State private var failedHexes: Set<String> = []
    @State private var downloadTotal = 0            // hexes targeted this run
    @State private var downloadTask: Task<Void, Never>?
    @State private var confirmRedownload = false
    @State private var showLayers = false
    /// Measured height of the floating card, so the map can inset for whatever
    /// the card currently is — the hint, a selection summary and a send progress
    /// card are very different heights, and this screen swaps between them.
    @State private var cardHeight: CGFloat = 0

    /// The band along the bottom of the map the card covers: the card and its
    /// padding, plus the home-indicator strip the full-bleed map runs under.
    private var cardInset: CGFloat { cardHeight + MapsView.homeIndicatorInset }

    private static var homeIndicatorInset: CGFloat {
        UIApplication.shared.connectedScenes
            .compactMap { $0 as? UIWindowScene }.first?
            .windows.first(where: \.isKeyWindow)?.safeAreaInsets.bottom ?? 0
    }

    // Hexes in the drawn box that aren't on the device yet, in whichever state
    // the download has reached. Areas already downloaded are NOT in here — they
    // already show as coverage hexagons, which says "you have this" better than
    // a selection tint would.
    // Every selected hex, coloured by what a download would do with it — the
    // same states the coverage uses, drawn heavier: new (accent), update
    // (ochre, hatched), current (green). Tapping still skips/keeps new hexes.
    private var selectionHexes: [SelectionHex] {
        tiles.map { t in
            let kind: SelectionHex.Kind =
                converted.contains(t.id) ? .done
                : ble.tileIsCurrent(t.id) ? (ble.tileNeedsUpdate(t.id) ? .update : .current)
                : excluded.contains(t.id) ? .excluded : .pending
            return SelectionHex(id: t.id, hexagon: t.hexagon, kind: kind)
        }
    }

    // Tiles that will actually be sent: not already on the device and not
    // tapped-out by the user. Tiles on the device are only rebuilt when the
    // rider asks (Redownload).
    private var newTiles: [MapTile] {
        tiles.filter { !ble.tileIsCurrent($0.id) && !excluded.contains($0.id) }
    }
    // Tiles whose cycling POIs go to the device: new ones, plus tiles already
    // there with no POIs or POIs older than PoiCache.maxAge. Empty for
    // firmware without POI support.
    private var poiTiles: [MapTile] {
        tiles.filter { !excluded.contains($0.id) && ble.poiNeedsSend($0.id) }
    }
    private var onDeviceTiles: [MapTile] { tiles.filter { ble.tileIsCurrent($0.id) } }
    private var updateCount: Int { onDeviceTiles.filter { ble.tileNeedsUpdate($0.id) }.count }
    private var onDeviceCount: Int { onDeviceTiles.count }
    /// Hexes above this many ask before a Redownload: each one is an Overpass
    /// fetch, an elevation fetch and a BLE transfer.
    private static let redownloadConfirmOver = 20

    // Toggle whether a tapped hex is included in the download.
    private func toggleHex(at coord: CLLocationCoordinate2D) {
        guard let id = H3Tiles.id(at: coord),
              tiles.contains(where: { $0.id == id }),
              !ble.tileIsCurrent(id) else { return }   // can't skip what's on-device
        if excluded.contains(id) { excluded.remove(id) } else { excluded.insert(id) }
    }

    var body: some View {
        NavigationStack {
            ZStack(alignment: .top) {
                EInkMapView(
                    outlines: outlineHexes,
                    selection: selectionHexes,
                    camera: camera,
                    showsUserLocation: ble.locationPermission.isGranted,
                    showsTrackingButton: true,
                    bottomInset: cardInset,
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
                        refreshCoverage()
                    })
                .ignoresSafeArea(edges: [.top, .bottom])

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
                        .ignoresSafeArea(edges: [.top, .bottom])
                }

                if let a = dragStart, let b = dragEnd {
                    Rectangle().fill(Palette.accent.opacity(0.18))
                        .overlay(Rectangle().stroke(Palette.accent, lineWidth: 2))
                        .frame(width: abs(b.x - a.x), height: abs(b.y - a.y))
                        .position(x: (a.x + b.x) / 2, y: (a.y + b.y) / 2)
                        .allowsHitTesting(false)
                }

                header

                // Key to the hex colours, once there is something to explain.
                if !outlineHexes.isEmpty || box != nil {
                    legend
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .padding(.leading, 16)
                        .padding(.top, 74)
                        .allowsHitTesting(false)
                }
            }
            .navigationBarHidden(true)
            .sheet(item: Binding(
                get: { inspected.map { TileInspection(id: $0.id, onDevice: $0.onDevice) } },
                set: { if $0 == nil { inspected = nil } }
            )) { info in
                TileInspectorSheet(info: info)
            }
            .overlay(alignment: .bottom) { bottomBar }
            .onAppear {
                ble.refreshDeviceMaps(); ble.refreshDeviceTiles(); locator.start()
                store.refresh()
                fitDownloadedHexes()
                // Screenshot hooks: -demo-maps-coverage shows a device holding a
                // mix of current / update hexes with and without POIs;
                // -demo-maps adds a selection of new + update + current hexes.
                if ble.isDemoMaps, box == nil {
                    let dev = H3Tiles.coveringTiles(south: 37.74, west: -122.52, north: 37.81, east: -122.40)
                        .map(\.id).sorted()
                    ble.demoMaps(onDevice: dev,
                                 current: dev.enumerated().filter { $0.offset % 2 == 0 }.map(\.element),
                                 withPois: dev.enumerated().filter { $0.offset % 3 != 2 }.map(\.element))
                    didFrameCoverage = false
                    fitDownloadedHexes()
                    if ProcessInfo.processInfo.arguments.contains("-demo-maps-layers") {
                        showLayers = true
                    } else if !ProcessInfo.processInfo.arguments.contains("-demo-maps-coverage") {
                        let b = (s: 37.69, w: -122.47, n: 37.77, e: -122.38)
                        box = b
                        tiles = H3Tiles.coveringTiles(south: b.s, west: b.w, north: b.n, east: b.e)
                    }
                }
            }
            .sheet(isPresented: $showLayers) {
                DeviceMapLayersSheet().environmentObject(ble)
                    .presentationDetents([.large])
            }
            .alert("Redownload \(onDeviceCount) hexes?", isPresented: $confirmRedownload) {
                Button("Cancel", role: .cancel) {}
                Button("Redownload") { download(redownload: true) }
            } message: {
                Text("They are rebuilt from fresh OpenStreetMap data and sent to the device again, with bike routes and water stops. This takes a while and uses data.")
            }
            // Redraw when a tile finishes decoding, or when the device's own
            // tile list arrives and flips areas to "synced". Both are also the
            // moment the coverage we want to frame becomes known.
            .onChange(of: store.version) { refreshCoverage(); fitDownloadedHexes() }
            // The device's tile list also arrives tile-by-tile during an upload,
            // and each change re-derives selectionHexes and rebuilds every
            // overlay. Coalesced for the same reason the store's version is.
            .onChange(of: ble.deviceTileIds) { scheduleRefresh() }
            .onChange(of: ble.devicePoiIds) { scheduleRefresh() }
            .onChange(of: ble.deviceSupportsPois) { scheduleRefresh() }
            // Center on the user's first fix, once, at our fixed tile-friendly
            // span. Only before any interaction so it never yanks the map away
            // from a box the user is drawing.
            //
            // This is the FALLBACK, and it no longer blocks the coverage shot.
            // Both used to claim one `didCenter` and whichever fired first won —
            // and the fix nearly always arrives before the tile scan does, so
            // the opening view was a fixed 25 km box on the rider and a
            // region-sized download was mostly off-screen with nothing to say it
            // existed. fitDownloadedHexes now overrides this once.
            .onReceive(locator.$coordinate) { coord in
                guard !didLocate, !didFrameCoverage, box == nil, !drawMode,
                      let coord else { return }
                didLocate = true
                camera = MapCameraCommand(target: .region(MKCoordinateRegion(center: coord,
                    span: MKCoordinateSpan(latitudeDelta: 0.25, longitudeDelta: 0.25))))
            }
        }
    }

    /// Coalesce redraws driven by the device's tile list. Without this, a large
    /// download rebuilds the whole overlay set once per tile and the page
    /// stutters badly by the time a few hundred hexes are on screen.
    @State private var refreshPending = false
    private func scheduleRefresh() {
        guard !refreshPending else { return }
        refreshPending = true
        Task { @MainActor in
            try? await Task.sleep(nanoseconds: 250_000_000)
            refreshPending = false
            refreshCoverage()
            fitDownloadedHexes()
        }
    }

    /// Ask the store which coverage hexagons the region now on screen needs.
    private func refreshCoverage() {
        guard let r = visibleRegion else { return }
        outlineHexes = store.visibleContent(in: r, synced: ble.deviceTileIds).map { o in
            guard o.synced else { return o }
            var h = o
            h.update = ble.tileNeedsUpdate(o.id)
            h.poi = ble.poiMark(o.id)
            return h
        }
    }

    /// Frame ALL the coverage — everything the phone holds plus everything the
    /// device holds — the first time this screen has something to frame.
    ///
    /// Managing downloaded areas starts with seeing them, and the old opening
    /// shot (a fixed ~22 km box on the user) showed one screenful of a
    /// collection that can span a country: the rest was off-map with nothing to
    /// say it existed. Runs once, and takes precedence over the locate-the-user
    /// fallback — the tile scan is asynchronous, so it reliably loses a race
    /// against a location fix that is often already cached. Neither one ever
    /// moves the camera under a box being drawn.
    private func fitDownloadedHexes() {
        guard !didFrameCoverage, box == nil, !drawMode else { return }
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
        didFrameCoverage = true
        // Less padding than a route gets: the hexes ARE the subject here. The
        // card's own share of the map is handled by the map's bottomInset, so
        // it must not be padded for a second time.
        camera = MapCameraCommand(target: .rect(rect.paddedForDisplay(fraction: 0.12)))
    }

    private var header: some View {
        HStack {
            Text("Maps").font(BarlowFont.condensed(22, .bold)).foregroundStyle(Palette.ink)
                .padding(.horizontal, 16).padding(.vertical, 9)
                .background(Palette.surface).clipShape(Capsule())
                .overlay(Capsule().strokeBorder(Palette.hairline, lineWidth: 1))
            Spacer()
            // The DEVICE's map layers (what the head unit draws), not this
            // screen's. Hidden for firmware that cannot switch them.
            if ble.mapLayersSupported {
                Button { showLayers = true } label: {
                    Image(systemName: "square.3.layers.3d")
                        .font(.system(size: 17, weight: .semibold))
                        .foregroundStyle(Palette.accent)
                        .frame(width: 40, height: 40)
                        .background(Palette.surface).clipShape(Circle())
                        .overlay(Circle().strokeBorder(Palette.hairline, lineWidth: 1))
                }
                .accessibilityLabel("Device map layers")
            }
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
        .background(GeometryReader { g in
            Color.clear.preference(key: CardHeightKey.self, value: g.size.height)
        })
        .onPreferenceChange(CardHeightKey.self) { cardHeight = $0 }
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
                Text("\(sent) of \(total) sent"
                     + (building && converted.count > sent ? " · \(converted.count) built" : ""))
                    .font(BarlowFont.text(12, .semibold)).foregroundStyle(Palette.good)
            }
        }
    }

    private func selectionCard(_ b: (s: Double, w: Double, n: Double, e: Double)) -> some View {
        let new = newTiles.count
        let poiOnly = poiTiles.filter { t in !newTiles.contains { $0.id == t.id } }.count
        let skipped = excluded.intersection(tiles.map(\.id)).count
        let onDevice = onDeviceCount
        let updates = updateCount
        let current = onDevice - updates
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
                // The same three states the map draws, in the same colours.
                HStack(spacing: 10) {
                    countChip(new, "new", Palette.accent)
                    countChip(updates, updates == 1 ? "update" : "updates", Palette.update)
                    countChip(current, "current", Palette.good)
                    Spacer(minLength: 0)
                }
                Text((poiOnly > 0 ? "\(poiOnly) need POIs" : "POIs up to date")
                     + (skipped > 0 ? " · \(skipped) skipped" : ""))
                    .font(BarlowFont.text(12)).foregroundStyle(Palette.muted)
                    .frame(maxWidth: .infinity, alignment: .leading)
                Text("Tap a hex to skip it (or add it back).")
                    .font(BarlowFont.text(11)).foregroundStyle(Palette.faint)
                    .frame(maxWidth: .infinity, alignment: .leading)
                if updates > 0 {
                    Text("\(updates) hex\(updates == 1 ? " has" : "es have") an update (made before bike routes & water stops, or over 90 days old) — redownload to refresh.")
                        .font(BarlowFont.text(12)).foregroundStyle(Palette.ink)
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
                PrimaryButton(title: !ble.canUploadMap ? "Connect device to send"
                                    : new > 0 ? "Download \(new) hex\(new == 1 ? "" : "es")"
                                    : poiOnly > 0 ? "Send POIs for \(poiOnly) hex\(poiOnly == 1 ? "" : "es")"
                                    : "Nothing to download",
                              systemImage: "arrow.down.circle",
                              enabled: ble.canUploadMap && (new > 0 || poiOnly > 0)) { download() }
                // Rebuild hexes the device already has from fresh map data —
                // the rider's choice, never automatic: it costs a fetch and a
                // transfer per hex. Offered on old firmware too (fresher roads),
                // where it simply carries no POIs.
                if onDevice > 0 {
                    SecondaryButton(title: "Redownload \(onDevice) hex\(onDevice == 1 ? "" : "es")",
                                    systemImage: "arrow.clockwise",
                                    enabled: ble.canUploadMap) {
                        if onDevice > Self.redownloadConfirmOver { confirmRedownload = true }
                        else { download(redownload: true) }
                    }
                }
                if let s = status { Text(s).font(BarlowFont.text(12)).foregroundStyle(Palette.accent) }
            }
        }
    }

    private func countChip(_ n: Int, _ label: String, _ color: Color) -> some View {
        HStack(spacing: 5) {
            RoundedRectangle(cornerRadius: 3).fill(color.opacity(0.35))
                .overlay(RoundedRectangle(cornerRadius: 3).stroke(color, lineWidth: 1.5))
                .frame(width: 12, height: 12)
            Text("\(n) \(label)").font(BarlowFont.text(13, .semibold)).foregroundStyle(Palette.ink)
        }
    }

    // Compact key to the hex colours and badges, top-left under the header.
    private var legend: some View {
        VStack(alignment: .leading, spacing: 4) {
            legendRow(Palette.good, hatched: false, "On device")
            legendRow(Palette.update, hatched: true, "Update available")
            if box != nil { legendRow(Palette.accent, hatched: false, "New") }
            legendRow(Palette.muted, hatched: false, "On this phone")
            if ble.deviceSupportsPois {
                HStack(spacing: 6) {
                    Image(systemName: "drop.fill").font(.system(size: 10))
                    Image(systemName: "drop").font(.system(size: 10))
                    Text("POIs on device / none or old").font(BarlowFont.text(11))
                }.foregroundStyle(Palette.ink)
            }
        }
        .padding(8)
        .background(Palette.surface.opacity(0.92), in: RoundedRectangle(cornerRadius: 10))
        .overlay(RoundedRectangle(cornerRadius: 10).strokeBorder(Palette.hairline))
    }

    private func legendRow(_ c: Color, hatched: Bool, _ text: String) -> some View {
        HStack(spacing: 6) {
            ZStack {
                RoundedRectangle(cornerRadius: 2).fill(c.opacity(0.3))
                if hatched {
                    Path { p in
                        for x in stride(from: -12.0, to: 14.0, by: 4.0) {
                            p.move(to: CGPoint(x: x, y: 12)); p.addLine(to: CGPoint(x: x + 12, y: 0))
                        }
                    }.stroke(c.opacity(0.7), lineWidth: 1)
                    .clipShape(RoundedRectangle(cornerRadius: 2))
                }
                RoundedRectangle(cornerRadius: 2).stroke(c, lineWidth: 1.5)
            }.frame(width: 12, height: 12)
            Text(text).font(BarlowFont.text(11)).foregroundStyle(Palette.ink)
        }
    }

    private var hintCard: some View {
        card {
            VStack(alignment: .leading, spacing: 3) {
                Text(drawMode ? "Drag a box across the area you want."
                              : "Tap “Select area”, then drag a box. Green hexes are on the device; ochre, hatched ones have an update. A filled drop means the device has their water stops and bike shops.")
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

    /// `redownload`: rebuild the selected hexes the device already has from
    /// fresh Overpass data (skipping the phone's caches) and re-send them with
    /// their POIs. Otherwise: new hexes, plus POIs that are missing or stale.
    private func download(redownload: Bool = false) {
        guard !building, !ble.tilesUploading else { return }   // one job at a time
        let missing = redownload ? onDeviceTiles : newTiles
        let poiWork = redownload ? (ble.deviceSupportsPois ? onDeviceTiles : []) : poiTiles
        guard !missing.isEmpty || !poiWork.isEmpty else { return }
        building = true
        converted = []
        failedHexes = []
        downloadTotal = missing.count + poiWork.count
        status = missing.isEmpty ? "Fetching cycling POIs…" : "Fetching map data…"

        // Group tiles into bounded OSM fetches (~0.08° ≈ 9 km) so each Overpass
        // query stays light — big queries 504 on the busy public servers. Each
        // batch is one call (retried across mirrors in MapBuilder.fetchOSM).
        let batches = Dictionary(grouping: missing) { t -> String in
            let clat = (t.south + t.north) / 2, clon = (t.west + t.east) / 2
            return "\(Int((clat / 0.08).rounded(.down)))_\(Int((clon / 0.08).rounded(.down)))"
        }.map { $0.value }

        ble.startTileStream(resend: redownload)   // begin sending as tiles are produced
        downloadTask = Task {
            var anyBuilt = false
            await DownloadStats.shared.reset()
            do {
                // Cycling POIs: their own small query and cache, started NOW so
                // they arrive while the map batches are still building (queued
                // behind any tiles already waiting to send). A failure here
                // never fails the map download — the map is what matters.
                var poiNote: String? = nil
                async let poisDone: Bool = {
                    guard !poiWork.isEmpty else { return true }
                    do { try await sendPois(poiWork, fresh: redownload); return true }
                    catch { return false }
                }()
              if !missing.isEmpty {
                // Anything built before goes straight out — no Overpass, no
                // elevation fetch, no re-encoding. This is what makes a retry
                // after a dropped link cheap instead of a full rebuild.
                // (Not on a Redownload: that is fresh data by definition.)
                var cached: [(id: String, data: Data)] = []
                if !redownload { cached = await TileCache.shared.partition(missing.map(\.id)).cached }
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
                // Coastline-only, so widening it is cheap. It runs ALONGSIDE the
                // first map batches; each batch waits for it only after its own
                // fetch, when it needs the sea rings.
                let all = union(missing)
                let pad = 0.35
                let seaRings = Task<[[(Double, Double)]], Never> {
                    let coastJSON = try? await MapBuilder.fetchCoastline(
                        south: all.s - pad, west: all.w - pad,
                        north: all.n + pad, east: all.e + pad)
                    let chains = coastJSON.flatMap {
                        try? MapBuilder.extractCoastlineChains(regionJSON: $0)
                    } ?? []
                    // Rings are assembled against the PADDED region, not a
                    // batch's bbox, so a batch sitting entirely offshore is
                    // still inside a ring and fills.
                    return MapBuilder.regionSeaPolygons(chains,
                        south: all.s - pad, west: all.w - pad,
                        north: all.n + pad, east: all.e + pad)
                }

                let cachedIds = Set(cached.map(\.id))
                let batches = batches
                    .map { $0.filter { !cachedIds.contains($0.id) } }
                    .filter { !$0.isEmpty }

                // Batches run `MapBuilder.concurrentBatches` at a time, each on
                // its own mirror, off the main actor; results are handled here
                // as each one lands (cache, then send — the link stays busy
                // while the next fetch is in flight).
                var done = 0
                status = "Fetching \(batches.count) area\(batches.count == 1 ? "" : "s")…"
                try await withThrowingTaskGroup(of: (Int, [(id: String, data: Data)]).self) { group in
                    var next = 0
                    while next < min(MapBuilder.concurrentBatches, batches.count) {
                        let i = next, b = batches[i]
                        group.addTask { (i, try await MapBuilder.buildBatch(b, startMirror: i, seaRings: seaRings)) }
                        next += 1
                    }
                    for try await (i, withElev) in group {
                        let batch = batches[i]
                        done += 1
                        status = "Built \(done) of \(batches.count) area\(batches.count == 1 ? "" : "s")…"
                        // Mark ONLY what was actually produced.
                        //
                        // This used to mark every id in the batch, including tiles
                        // that were never encoded or were dropped as empty — so
                        // the map filled them in as downloaded and the run
                        // reported success while nothing had been sent or stored.
                        // A rider then finds a hole in their coverage, with no
                        // clue which hex is missing, and it survives reboots
                        // because the tile genuinely is not on the card.
                        let produced = Set(withElev.map(\.id))
                        converted.formUnion(produced)
                        let missed = batch.map(\.id).filter { !produced.contains($0) }
                        if !missed.isEmpty { failedHexes.formUnion(missed) }
                        if !withElev.isEmpty { anyBuilt = true }
                        // Cache BEFORE sending: if the link drops mid-transfer the
                        // expensive work survives and the retry is instant.
                        await TileCache.shared.store(withElev)
                        // Draw the new areas straight away — the download IS what
                        // "downloaded" means on this map.
                        store.noteDownloaded(withElev.map(\.id))
                        ble.enqueueTiles(withElev)
                        if next < batches.count {
                            let k = next, b = batches[k]
                            group.addTask { (k, try await MapBuilder.buildBatch(b, startMirror: k, seaRings: seaRings)) }
                            next += 1
                        }
                    }
                }
              }
                if !(await poisDone) {
                    try Task.checkCancellation()
                    poiNote = "Cycling POIs could not be fetched — try again later."
                }
                print(await DownloadStats.shared.summary(hexes: missing.count + poiWork.count))
                building = false
                ble.finishTileStream()                     // let the queue drain
                if !failedHexes.isEmpty {
                    status = "\(failedHexes.count) hex\(failedHexes.count == 1 ? "" : "es") had no map data — tap Select area and retry them."
                } else if missing.isEmpty {
                    status = poiNote
                } else {
                    status = anyBuilt ? poiNote : "No roads found in that area."
                }
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

    /// Build (or reuse) and queue the `.poi` files for `work`. One POI query per
    /// ~0.25° group of tiles: the query is light, so groups can be far bigger
    /// than the map batches. Each POI is stored in the one H3 cell containing it.
    private func sendPois(_ work: [MapTile], fresh: Bool = false) async throws {
        var cached: [(id: String, data: Data)] = []
        var missingIds = work.map(\.id)
        if !fresh {   // a Redownload rebuilds them from fresh data instead
            (cached, missingIds) = await PoiCache.shared.partition(work.map(\.id))
        }
        ble.enqueuePois(cached)
        let need = Set(missingIds)
        let groups = Dictionary(grouping: work.filter { need.contains($0.id) }) { t -> String in
            let clat = (t.south + t.north) / 2, clon = (t.west + t.east) / 2
            return "\(Int((clat / 0.25).rounded(.down)))_\(Int((clon / 0.25).rounded(.down)))"
        }.map { $0.value }
        for (i, group) in groups.enumerated() {
            if i > 0 { try? await Task.sleep(nanoseconds: 1_000_000_000) }  // pace the servers
            try Task.checkCancellation()
            let u = union(group)
            let json = try await MapBuilder.fetchPOIs(south: u.s, west: u.w, north: u.n, east: u.e)
            let pois = try MapBuilder.collectPois(regionJSON: json)
            // Which cell each POI is in, once (not once per tile).
            var byCell: [String: [MapBuilder.Poi]] = [:]
            for p in pois {
                if let id = H3Tiles.id(at: .init(latitude: p.lat, longitude: p.lon)) {
                    byCell[id, default: []].append(p)
                }
            }
            let files = group.map { t in
                (id: t.id, data: MapBuilder.buildPoi(byCell[t.id] ?? [], south: t.south,
                    west: t.west, north: t.north, east: t.east, cell: t.id,
                    contains: { _, _ in true }))
            }
            await PoiCache.shared.store(files)
            ble.enqueuePois(files)
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

/// The switches for what the DEVICE's map draws — bike-route bands, cycleways
/// and lanes, and each POI type. Applied as soon as they change when the
/// device is connected; otherwise shown as last known and sent on reconnect.
struct DeviceMapLayersSheet: View {
    @EnvironmentObject var ble: BLEManager

    private static let rows: [(bit: UInt16, title: String, detail: String, icon: String)] = [
        (0x01, "Bike routes", "Grey bands along signed bike routes", "point.topleft.down.to.point.bottomright.curvepath"),
        (0x02, "Cycleways & bike lanes", "Dashed cycleways, dotted lane edges", "bicycle"),
        (0x04, "Drinking water", "Water drop icons", "drop.fill"),
        (0x08, "Toilets", "Restroom icons", "figure.stand.dress.line.vertical.figure"),
        (0x10, "Repair stations", "Self-service repair stands", "wrench.adjustable.fill"),
        (0x20, "Bike shops", "Bicycle shop icons", "storefront"),
    ]

    var body: some View {
        NavigationStack {
            List {
                Section {
                    ForEach(Self.rows, id: \.bit) { row in
                        Toggle(isOn: Binding(
                            get: { ble.mapLayers & row.bit != 0 },
                            set: { on in ble.setMapLayers(on ? ble.mapLayers | row.bit
                                                              : ble.mapLayers & ~row.bit) })) {
                            Label {
                                VStack(alignment: .leading, spacing: 2) {
                                    Text(row.title).font(BarlowFont.text(16, .semibold))
                                    Text(row.detail).font(BarlowFont.text(12)).foregroundStyle(Palette.muted)
                                }
                            } icon: {
                                Image(systemName: row.icon).foregroundStyle(Palette.ink)
                            }
                        }
                        .tint(Palette.accent)
                    }
                } footer: {
                    Text(ble.canUploadMap
                         ? "What the device's map shows. Layers only appear where the downloaded hexes include them."
                         : "Not connected — showing the last known setting. Changes are sent the next time the device connects.")
                }
            }
            .scrollContentBackground(.hidden)
            .background(Palette.paper)
            .navigationTitle("Device map layers")
            .navigationBarTitleDisplayMode(.inline)
        }
    }
}
