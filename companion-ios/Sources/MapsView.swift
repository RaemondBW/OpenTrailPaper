import SwiftUI
import MapKit
import CoreLocation

// A downloaded map region, persisted so the app can show what's already on the
// device and avoid re-downloading.
struct MapRegion: Identifiable, Codable, Equatable {
    var id = UUID()
    var name: String
    var south, west, north, east: Double
    var sizeKB: Int
    var date: Date

    var center: CLLocationCoordinate2D {
        .init(latitude: (south + north) / 2, longitude: (west + east) / 2)
    }
    var corners: [CLLocationCoordinate2D] {
        [.init(latitude: south, longitude: west), .init(latitude: south, longitude: east),
         .init(latitude: north, longitude: east), .init(latitude: north, longitude: west)]
    }
}

@MainActor
final class RegionStore: ObservableObject {
    @Published private(set) var regions: [MapRegion] = []
    private let key = "downloadedMapRegions"

    init() { load() }
    private func load() {
        if let d = UserDefaults.standard.data(forKey: key),
           let r = try? JSONDecoder().decode([MapRegion].self, from: d) { regions = r }
    }
    private func save() {
        if let d = try? JSONEncoder().encode(regions) { UserDefaults.standard.set(d, forKey: key) }
    }
    func add(_ r: MapRegion) { regions.removeAll { $0.name == r.name }; regions.append(r); save() }
    func remove(_ r: MapRegion) { regions.removeAll { $0.id == r.id }; save() }
}

// Draw a box on the map, download OSM vectors for it, and stream them to the
// device. Already-downloaded regions are shown as filled rectangles.
struct MapsView: View {
    @EnvironmentObject var ble: BLEManager
    @StateObject private var store = RegionStore()
    @Environment(\.dismiss) private var dismiss

    @State private var cam: MapCameraPosition = .userLocation(fallback: .region(
        MKCoordinateRegion(center: .init(latitude: 37.7764, longitude: -122.4346),
                           span: MKCoordinateSpan(latitudeDelta: 0.15, longitudeDelta: 0.15))))
    @State private var dragStart: CGPoint?
    @State private var dragEnd: CGPoint?
    @State private var box: (s: Double, w: Double, n: Double, e: Double)?
    @State private var building = false
    @State private var status: String?
    @State private var drawMode = false

    var body: some View {
        NavigationStack {
            MapReader { proxy in
                ZStack(alignment: .top) {
                    Map(position: $cam) {
                        UserAnnotation()
                        // Areas already on the device (queried from it) — blue.
                        ForEach(ble.deviceMaps) { m in
                            MapPolygon(coordinates: m.corners)
                                .foregroundStyle(Color.blue.opacity(0.12))
                                .stroke(Color.blue.opacity(0.6), lineWidth: 1.5)
                        }
                        // Areas this app has downloaded — green.
                        ForEach(store.regions) { r in
                            MapPolygon(coordinates: r.corners)
                                .foregroundStyle(Palette.good.opacity(0.18))
                                .stroke(Palette.good, lineWidth: 2)
                        }
                        if let b = box {
                            MapPolygon(coordinates: boxCorners(b))
                                .foregroundStyle(Palette.accent.opacity(0.20))
                                .stroke(Palette.accent, lineWidth: 2)
                        }
                    }
                    .mapControls { MapUserLocationButton() }
                    .ignoresSafeArea(edges: .top)

                    // In draw mode, a transparent layer above the map captures the
                    // drag so the map doesn't pan — otherwise the map's own pan
                    // gesture wins and you can never draw a box.
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
                                        box = (min(c1.latitude, c2.latitude), min(c1.longitude, c2.longitude),
                                               max(c1.latitude, c2.latitude), max(c1.longitude, c2.longitude))
                                        status = nil
                                    }
                                    dragStart = nil; dragEnd = nil
                                    drawMode = false
                                })
                            .ignoresSafeArea(edges: .top)
                    }

                    // Live selection rectangle (view space) while dragging.
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
            .onAppear { ble.refreshDeviceMaps() }
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
                box = nil; dragStart = nil; dragEnd = nil
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

    @ViewBuilder private var bottomBar: some View {
        VStack(spacing: 10) {
            if ble.mapUploading {
                ProgressView(value: ble.mapProgress) {
                    Text(ble.mapMessage ?? "Sending…").font(BarlowFont.text(13)).foregroundStyle(Palette.muted)
                }
            } else if building {
                HStack(spacing: 10) {
                    ProgressView()
                    Text(status ?? "Working…").font(BarlowFont.text(14)).foregroundStyle(Palette.muted)
                }.frame(maxWidth: .infinity, alignment: .leading)
            } else if let b = box {
                HStack {
                    VStack(alignment: .leading, spacing: 1) {
                        Text("Selected area").trackedLabel()
                        Text(areaText(b)).font(BarlowFont.text(15, .semibold)).foregroundStyle(Palette.ink)
                    }
                    Spacer()
                    Button { box = nil } label: {
                        Image(systemName: "xmark.circle.fill").foregroundStyle(Palette.muted)
                    }
                }
                PrimaryButton(title: ble.canUploadMap ? "Download to device" : "Connect device to send",
                              systemImage: "arrow.down.circle",
                              enabled: ble.canUploadMap && !tooBig(b)) { download(b) }
                if tooBig(b) {
                    Text("That area is large — zoom in and draw a smaller box (≤ ~25 km across) for a faster download.")
                        .font(BarlowFont.text(12)).foregroundStyle(Palette.accent)
                }
            } else {
                Text(status ?? (drawMode
                    ? "Drag a box across the area you want."
                    : "Tap “Select area”, then drag a box on the map. Blue = already on device, green = downloaded here."))
                    .font(BarlowFont.text(14)).foregroundStyle(drawMode ? Palette.accent : Palette.muted)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }

            if !store.regions.isEmpty {
                Divider().overlay(Palette.hairline)
                ForEach(store.regions) { r in
                    HStack {
                        Image(systemName: "checkmark.circle.fill").foregroundStyle(Palette.good)
                        VStack(alignment: .leading, spacing: 0) {
                            Text(r.name).font(BarlowFont.text(14, .semibold)).foregroundStyle(Palette.ink)
                            Text("\(r.sizeKB) KB · on device").font(BarlowFont.text(11)).foregroundStyle(Palette.muted)
                        }
                        Spacer()
                        Button { store.remove(r) } label: {
                            Image(systemName: "trash").foregroundStyle(Palette.muted)
                        }
                    }
                }
            }
        }
        .padding(16)
        .background(Palette.paper)
    }

    private func boxCorners(_ b: (s: Double, w: Double, n: Double, e: Double)) -> [CLLocationCoordinate2D] {
        [.init(latitude: b.s, longitude: b.w), .init(latitude: b.s, longitude: b.e),
         .init(latitude: b.n, longitude: b.e), .init(latitude: b.n, longitude: b.w)]
    }
    private func spanKm(_ b: (s: Double, w: Double, n: Double, e: Double)) -> (Double, Double) {
        let latKm = (b.n - b.s) * 111.0
        let lonKm = (b.e - b.w) * 111.0 * cos((b.s + b.n) / 2 * .pi / 180)
        return (latKm, lonKm)
    }
    private func areaText(_ b: (s: Double, w: Double, n: Double, e: Double)) -> String {
        let (a, c) = spanKm(b)
        return String(format: "%.1f × %.1f km", c, a)
    }
    private func tooBig(_ b: (s: Double, w: Double, n: Double, e: Double)) -> Bool {
        let (a, c) = spanKm(b); return a > 25 || c > 25
    }

    private func download(_ b: (s: Double, w: Double, n: Double, e: Double)) {
        building = true
        status = "Downloading map data…"
        let name = "map-\(Int(b.s * 100))_\(Int(b.w * 100)).ebm"
        Task {
            do {
                let ebm = try await MapBuilder.build(south: b.s, west: b.w, north: b.n, east: b.e) { p in
                    status = p.stage
                }
                building = false
                ble.uploadMap(ebm, name: name)
                // Persist once the device confirms it saved.
                let region = MapRegion(name: name, south: b.s, west: b.w, north: b.n,
                                       east: b.e, sizeKB: ebm.count / 1024, date: Date())
                waitForInstall(region)
            } catch {
                building = false
                status = error.localizedDescription
            }
        }
    }

    // The device notifies "Map installed" via mapMessage; record the region then.
    private func waitForInstall(_ r: MapRegion) {
        Task {
            for _ in 0..<600 {   // up to ~10 min
                if !ble.mapUploading {
                    if ble.mapMessage == "Map installed" { store.add(r); box = nil }
                    return
                }
                try? await Task.sleep(nanoseconds: 1_000_000_000)
            }
        }
    }
}
