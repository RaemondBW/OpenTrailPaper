import SwiftUI
import MapKit
import CoreLocation

// Draw a box on the map; the app covers it with H3 hexagon tiles (~5.6 km
// across), skips the ones already on the device, fetches OSM for the rest, and
// streams each new tile to the device one at a time. The device stores every
// tile by its H3 id and renders them straight off the SD card, so map coverage
// is limited by the card, not memory — and re-selecting an overlapping area
// only ever sends the genuinely new tiles.
struct MapsView: View {
    @EnvironmentObject var ble: BLEManager
    @Environment(\.dismiss) private var dismiss

    @State private var cam: MapCameraPosition = .userLocation(fallback: .region(
        MKCoordinateRegion(center: .init(latitude: 37.7764, longitude: -122.4346),
                           span: MKCoordinateSpan(latitudeDelta: 0.15, longitudeDelta: 0.15))))
    @State private var dragStart: CGPoint?
    @State private var dragEnd: CGPoint?
    @State private var box: (s: Double, w: Double, n: Double, e: Double)?
    @State private var tiles: [MapTile] = []       // covering tiles for the current box
    @State private var building = false
    @State private var status: String?
    @State private var drawMode = false
    @State private var excluded: Set<String> = []   // hexes the user tapped to skip
    @State private var converted: Set<String> = []  // hexes downloaded + built this run
    @State private var downloadTotal = 0            // hexes targeted this run
    @State private var downloadTask: Task<Void, Never>?

    // Tiles the device already has, as drawable rectangles.
    private var deviceTiles: [MapTile] {
        ble.deviceTileIds.compactMap { id in
            let cell = h3_from_id(id)
            return cell == 0 ? nil : H3Tiles.tile(from: cell)
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
            MapReader { proxy in
                ZStack(alignment: .top) {
                    Map(position: $cam) {
                        UserAnnotation()
                        // Hexes already on the device — blue.
                        ForEach(deviceTiles) { t in
                            MapPolygon(coordinates: t.hexagon)
                                .foregroundStyle(Color.blue.opacity(0.12))
                                .stroke(Color.blue.opacity(0.5), lineWidth: 1)
                        }
                        // Covering tiles for the current selection. Live states:
                        // on device (blue, drawn above) → done; converted this
                        // run (green) → downloaded/building; tapped-out (faint);
                        // pending (accent).
                        ForEach(tiles) { t in
                            let have = ble.deviceTileIds.contains(t.id)
                            let done = converted.contains(t.id)
                            let off = excluded.contains(t.id)
                            MapPolygon(coordinates: t.hexagon)
                                .foregroundStyle(have ? Color.clear
                                                 : done ? Palette.good.opacity(0.28)
                                                 : off ? Palette.muted.opacity(0.08)
                                                       : Palette.accent.opacity(0.16))
                                .stroke(have ? Palette.muted.opacity(0.4)
                                        : done ? Palette.good
                                        : off ? Palette.muted.opacity(0.55) : Palette.accent,
                                        lineWidth: have ? 1 : 1.5)
                        }
                    }
                    .mapControls { MapUserLocationButton() }
                    .ignoresSafeArea(edges: .top)
                    // Tap a hex (once an area is drawn) to skip/keep it.
                    .simultaneousGesture(SpatialTapGesture().onEnded { e in
                        guard box != nil, !drawMode,
                              let c = proxy.convert(e.location, from: .local) else { return }
                        toggleHex(at: c)
                    })

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
                                       let c1 = proxy.convert(a, from: .local),
                                       let c2 = proxy.convert(b, from: .local) {
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
            }
            .navigationBarHidden(true)
            .safeAreaInset(edge: .bottom) { bottomBar }
            .onAppear { ble.refreshDeviceMaps(); ble.refreshDeviceTiles() }
        }
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
            if ble.tilesUploading {
                progressCard(title: "Sending to device",
                             detail: ble.tileMessage ?? "Uploading hexes…",
                             done: ble.tilesDone, total: ble.tilesTotal,
                             noun: "sent") { ble.cancelTileUpload() }
            } else if building {
                progressCard(title: "Downloading maps",
                             detail: status ?? "Fetching…",
                             done: converted.count, total: max(downloadTotal, 1),
                             noun: "ready") { cancelDownload() }
            } else if let b = box {
                selectionCard(b)
            } else {
                hintCard
            }
        }
        .padding(.horizontal, 16)
        .padding(.bottom, 6)
    }

    // Spinner + title + live "X of Y hexes" + progress bar + Cancel.
    private func progressCard(title: String, detail: String, done: Int, total: Int,
                              noun: String, cancel: @escaping () -> Void) -> some View {
        card {
            VStack(alignment: .leading, spacing: 10) {
                HStack(spacing: 12) {
                    ProgressView().tint(Palette.accent)
                    VStack(alignment: .leading, spacing: 2) {
                        Text(title).font(BarlowFont.condensed(19, .semibold)).foregroundStyle(Palette.ink)
                        Text(detail).font(BarlowFont.text(12)).foregroundStyle(Palette.muted)
                            .lineLimit(1)
                    }
                    Spacer(minLength: 6)
                    Button("Cancel", action: cancel)
                        .font(BarlowFont.text(14, .semibold)).foregroundStyle(Palette.accent)
                }
                ProgressView(value: Double(done), total: Double(max(total, 1))).tint(Palette.good)
                Text("\(done) of \(total) hexes \(noun)")
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
                              : "Tap “Select area”, then drag a box. Blue = already on the device.")
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

        downloadTask = Task {
            var built: [(id: String, data: Data)] = []
            do {
                for (i, batch) in batches.enumerated() {
                    if i > 0 { try? await Task.sleep(nanoseconds: 1_000_000_000) }  // pace the servers
                    try Task.checkCancellation()
                    let n = batches.count
                    status = "Fetching area \(i + 1)/\(n)…"
                    let u = union(batch)
                    let json = try await MapBuilder.fetchOSM(south: u.s, west: u.w, north: u.n, east: u.e) { m in
                        Task { @MainActor in status = "Fetching area \(i + 1)/\(n) — \(m)" }
                    }
                    status = "Building tiles \(i + 1)/\(n)…"
                    let part = try MapBuilder.encodeTiles(regionJSON: json, tiles: batch)
                    built.append(contentsOf: part)
                    converted.formUnion(batch.map(\.id))   // fill these hexes in live
                }
                building = false
                if built.isEmpty {
                    status = "No roads found in that area."
                    return
                }
                status = nil
                ble.uploadTiles(built)
            } catch is CancellationError {
                building = false
                status = "Canceled"
            } catch {
                building = false
                status = error.localizedDescription
            }
        }
    }

    private func cancelDownload() {
        downloadTask?.cancel()
        downloadTask = nil
        building = false
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
