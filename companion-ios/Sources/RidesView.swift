import SwiftUI
import CoreLocation
import MapKit

// Lists recorded rides on the device and downloads a selected .fit file
// over BLE, then offers the system share sheet (Strava, Files, AirDrop…).
struct RidesView: View {
    @EnvironmentObject var ble: BLEManager
    @StateObject private var previews = RidePreviewStore()
    @State private var detail: RideDetailData?
    @State private var failedURL: URL?          // parsed badly → offer to share it

    // Connected: the device's ride list. Offline: whatever is cached locally.
    private var displayedRides: [RideFile] {
        ble.state == .connected ? ble.rides : BLEManager.cachedRides()
    }

    // Show a cached ride immediately; otherwise pull it over BLE.
    private func open(_ ride: RideFile) {
        let url = BLEManager.cachedURL(for: ride.name)
        if let data = try? Data(contentsOf: url) {
            if let preview = FITDecoder.decode(data) {
                detail = RideDetailData(url: url, preview: preview)
            } else {
                failedURL = url
            }
        } else {
            ble.downloadRide(ride.name)
        }
    }

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(spacing: 16) {
                    ConnectionBanner()

                    let rides = displayedRides
                    if ble.state == .connected && ble.loadingRides {
                        ProgressView("Loading rides…").padding(.top, 30)
                    } else if rides.isEmpty {
                        Card {
                            VStack(alignment: .leading, spacing: 6) {
                                Text(ble.state == .connected ? "No rides found"
                                                             : "No cached rides")
                                    .font(TypeScale.body).foregroundStyle(Palette.ink)
                                Text(ble.state == .connected
                                     ? "Rides are saved to the SD card. If you're mid-ride, stop it first."
                                     : "Connect to your Bike GPS to download rides. Downloaded rides stay here for offline viewing.")
                                    .font(.system(size: 13)).foregroundStyle(Palette.muted)
                            }
                        }
                    } else {
                        if ble.state != .connected {
                            Text("Showing downloaded rides · connect to fetch more")
                                .font(.system(size: 12)).foregroundStyle(Palette.muted)
                                .frame(maxWidth: .infinity, alignment: .leading)
                        }
                        ForEach(rides) { ride in
                            RideRow(ride: ride,
                                    cached: BLEManager.isCached(ride.name),
                                    preview: previews.preview(for: ride.name),
                                    downloading: ble.downloadingName == ride.name,
                                    progress: ble.downloadProgress,
                                    onTap: { open(ride) },
                                    onDelete: {
                                        ble.deleteRide(ride.name)
                                        previews.forget(ride.name)
                                    })
                                .onAppear { previews.ensureLoaded(ride.name) }
                        }
                        Text("Tap to preview & share · long-press to delete")
                            .font(.system(size: 12)).foregroundStyle(Palette.muted)
                    }
                }
                .padding(16)
            }
            .background(Palette.paper)
            .navigationTitle("Rides")
            .toolbar {
                Button {
                    ble.refreshRides()
                } label: { Image(systemName: "arrow.clockwise") }
                .disabled(ble.state != .connected)
            }
            .onAppear {
                if ProcessInfo.processInfo.arguments.contains("-demo-ride"),
                   let url = Bundle.main.url(forResource: "demo", withExtension: "fit"),
                   let data = try? Data(contentsOf: url),
                   let preview = FITDecoder.decode(data) {
                    detail = RideDetailData(url: url, preview: preview)
                } else if ble.rides.isEmpty {
                    ble.refreshRides()
                }
            }
            .onChange(of: ble.state) { _, s in
                if s == .connected { ble.refreshRides() }   // fetch device list on connect
            }
            .onChange(of: ble.downloadedFileURL) { _, url in
                guard let url, let data = try? Data(contentsOf: url) else { return }
                if let preview = FITDecoder.decode(data) {
                    detail = RideDetailData(url: url, preview: preview)
                    previews.store(preview, for: url.lastPathComponent)  // fill the row thumbnail
                } else {
                    failedURL = url
                }
            }
            .sheet(item: $detail) { d in
                RideDetailView(fileURL: d.url, preview: d.preview)
            }
            .sheet(item: $failedURL) { url in
                RideParseErrorView(url: url)
            }
        }
    }
}

struct RideDetailData: Identifiable {
    let url: URL
    let preview: RidePreview
    var id: String { url.absoluteString }
}

private struct RideRow: View {
    let ride: RideFile
    let cached: Bool
    let preview: RidePreview?
    let downloading: Bool
    let progress: Double
    let onTap: () -> Void
    let onDelete: () -> Void
    @AppStorage(UnitPref.key) private var useMiles = false

    var body: some View {
        Button(action: onTap) {
            Card {
                VStack(spacing: 12) {
                    banner
                    HStack(alignment: .top, spacing: 12) {
                        VStack(alignment: .leading, spacing: 4) {
                            Text(prettyDate).font(TypeScale.body).foregroundStyle(Palette.ink)
                            if let p = preview {
                                stats(p)
                            } else {
                                Text(String(format: "%.0f KB · %@",
                                            Double(ride.size) / 1024, ride.name))
                                    .font(.system(size: 12)).foregroundStyle(Palette.muted)
                            }
                        }
                        Spacer()
                        trailing
                    }
                }
            }
        }
        .buttonStyle(.plain)
        .contextMenu {
            Button(role: .destructive, action: onDelete) {
                Label("Delete ride", systemImage: "trash")
            }
        }
    }

    // Large map banner: a real map snapshot with the route drawn on it once the
    // ride is decoded, otherwise a prompt to download.
    @ViewBuilder private var banner: some View {
        ZStack {
            RoundedRectangle(cornerRadius: 12).fill(Palette.paper)
            if let p = preview, p.coordinates.count > 1 {
                RouteMapThumbnail(name: ride.name, coords: p.coordinates)
            } else if downloading {
                VStack(spacing: 8) {
                    ProgressView(value: progress)
                        .frame(maxWidth: 160)
                    Text("Downloading…").font(.system(size: 12))
                        .foregroundStyle(Palette.muted)
                }
            } else {
                VStack(spacing: 8) {
                    Image(systemName: cached ? "map" : "arrow.down.circle")
                        .font(.system(size: 30)).foregroundStyle(Palette.muted)
                    Text(cached ? "Loading map…" : "Tap to download")
                        .font(.system(size: 12)).foregroundStyle(Palette.muted)
                }
            }
        }
        .frame(height: 170)
        .clipShape(RoundedRectangle(cornerRadius: 12))
        .overlay(RoundedRectangle(cornerRadius: 12)
            .strokeBorder(Palette.hairline, lineWidth: 1))
    }

    @ViewBuilder private func stats(_ p: RidePreview) -> some View {
        HStack(spacing: 14) {
            statItem(String(format: "%.1f %@",
                            Units.distance(p.distanceKm, miles: useMiles),
                            Units.distLabel(useMiles)), "Distance")
            statItem(timeStr(p.duration), "Time")
            statItem(String(format: "%.1f", Units.speed(p.avgSpeedKmh, miles: useMiles)),
                     "\(Units.speedLabel(useMiles)) avg")
            statItem(String(format: "%.0f %@",
                            Units.elevation(p.ascentM, miles: useMiles),
                            Units.elevLabel(useMiles)), "Ascent")
            if let w = p.avgPower { statItem("\(w) W", "Power") }
        }
    }

    private func statItem(_ value: String, _ label: String) -> some View {
        VStack(alignment: .leading, spacing: 1) {
            Text(value).font(.system(size: 13, weight: .semibold))
                .foregroundStyle(Palette.ink)
            Text(label).font(.system(size: 10)).foregroundStyle(Palette.muted)
        }
    }

    @ViewBuilder private var trailing: some View {
        if downloading {
            ProgressView(value: progress).frame(width: 54)
        } else if cached {
            Image(systemName: "chevron.right")
                .font(.system(size: 14, weight: .semibold))
                .foregroundStyle(Palette.muted)
        } else {
            Image(systemName: "square.and.arrow.down")
                .font(.system(size: 20, weight: .semibold))
                .foregroundStyle(Palette.accent)
        }
    }

    private func timeStr(_ s: TimeInterval) -> String {
        let m = Int(s) / 60, h = m / 60
        return h > 0 ? "\(h)h \(m % 60)m" : "\(m)m"
    }

    // Filenames look like 20250710-212500.fit
    private var prettyDate: String {
        let base = ride.name.replacingOccurrences(of: ".fit", with: "")
        let parts = base.split(separator: "-")
        guard parts.count == 2, parts[0].count == 8, parts[1].count == 6 else {
            return ride.name
        }
        let d = parts[0], t = parts[1]
        let y = d.prefix(4), mo = d.dropFirst(4).prefix(2), da = d.suffix(2)
        let h = t.prefix(2), mi = t.dropFirst(2).prefix(2)
        return "\(y)-\(mo)-\(da)  \(h):\(mi)"
    }
}

// Shown when a downloaded ride can't be decoded — lets the user share the
// raw .fit so it can be inspected.
private struct RideParseErrorView: View {
    let url: URL
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        VStack(spacing: 20) {
            Image(systemName: "exclamationmark.triangle.fill")
                .font(.system(size: 44))
                .foregroundStyle(Palette.accent)
            Text("Couldn't read this ride")
                .font(TypeScale.title).foregroundStyle(Palette.ink)
            Text("The full file downloaded but the app couldn't parse it. Share the raw .fit file so it can be analyzed.")
                .font(TypeScale.body).foregroundStyle(Palette.muted)
                .multilineTextAlignment(.center)
            ShareLink(item: url) {
                Label("Share .fit file", systemImage: "square.and.arrow.up")
                    .font(.system(size: 17, weight: .semibold))
                    .frame(maxWidth: .infinity)
                    .padding(.vertical, 14)
                    .background(Palette.accent)
                    .foregroundStyle(.white)
                    .clipShape(RoundedRectangle(cornerRadius: 14))
            }
            Button("Close") { dismiss() }
                .foregroundStyle(Palette.muted)
            Spacer()
        }
        .padding(28)
        .presentationDetents([.medium])
    }
}

// Lazily decodes cached rides off the main thread and caches the previews so
// the list can show route thumbnails + stats without re-parsing on each render.
@MainActor
final class RidePreviewStore: ObservableObject {
    @Published private(set) var byName: [String: RidePreview] = [:]
    private var loading: Set<String> = []

    func preview(for name: String) -> RidePreview? { byName[name] }

    func store(_ p: RidePreview, for name: String) { byName[name] = p }

    func forget(_ name: String) { byName[name] = nil; loading.remove(name) }

    func ensureLoaded(_ name: String) {
        guard byName[name] == nil, !loading.contains(name),
              BLEManager.isCached(name) else { return }
        loading.insert(name)
        let url = BLEManager.cachedURL(for: name)
        Task.detached(priority: .utility) {
            let decoded = (try? Data(contentsOf: url)).flatMap { FITDecoder.decode($0) }
            await MainActor.run {
                if let decoded { self.byName[name] = decoded }
                self.loading.remove(name)
            }
        }
    }
}

// A real map (tiles) with the ride's route drawn on top, rendered once via
// MKMapSnapshotter and cached by ride name. Falls back to a track-shape glyph
// while the snapshot loads or if it can't be produced (e.g. offline).
struct RouteMapThumbnail: View {
    let name: String
    let coords: [CLLocationCoordinate2D]
    @State private var image: UIImage?

    private static let cache = NSCache<NSString, UIImage>()

    var body: some View {
        GeometryReader { geo in
            ZStack {
                if let image {
                    Image(uiImage: image).resizable().scaledToFill()
                } else {
                    RouteThumbnail(coords: coords).padding(10)
                }
            }
            .frame(width: geo.size.width, height: geo.size.height)
            .task(id: name) { await load(size: geo.size) }
        }
    }

    private func load(size: CGSize) async {
        guard size.width > 1, size.height > 1, coords.count > 1 else { return }
        let key = "\(name)@\(Int(size.width))x\(Int(size.height))" as NSString
        if let cached = Self.cache.object(forKey: key) { image = cached; return }
        if let img = await Self.render(coords: coords, size: size) {
            Self.cache.setObject(img, forKey: key)
            image = img
        }
    }

    private static func render(coords: [CLLocationCoordinate2D],
                               size: CGSize) async -> UIImage? {
        // Map rect covering the whole track, with ~18% padding.
        var rect = MKMapRect.null
        for c in coords {
            let p = MKMapPoint(c)
            rect = rect.union(MKMapRect(x: p.x, y: p.y, width: 0, height: 0))
        }
        guard rect.size.width > 0 || rect.size.height > 0 else { return nil }
        let pad = max(rect.size.width, rect.size.height, 200) * 0.18
        rect = rect.insetBy(dx: -pad, dy: -pad)

        let opts = MKMapSnapshotter.Options()
        opts.mapRect = rect
        opts.size = size
        opts.showsBuildings = false
        opts.pointOfInterestFilter = .excludingAll

        let snapshotter = MKMapSnapshotter(options: opts)
        let snapshot: MKMapSnapshotter.Snapshot? = await withCheckedContinuation { cont in
            snapshotter.start(with: .global(qos: .utility)) { snap, _ in
                cont.resume(returning: snap)
            }
        }
        guard let snapshot else { return nil }

        let accent = UIColor(Palette.accent)
        let good = UIColor(Palette.good)
        let renderer = UIGraphicsImageRenderer(size: snapshot.image.size)
        return renderer.image { rctx in
            snapshot.image.draw(at: .zero)
            let cg = rctx.cgContext
            cg.setLineWidth(3.5)
            cg.setLineJoin(.round); cg.setLineCap(.round)
            cg.setStrokeColor(accent.cgColor)
            for (i, c) in coords.enumerated() {
                let pt = snapshot.point(for: c)
                if i == 0 { cg.move(to: pt) } else { cg.addLine(to: pt) }
            }
            cg.strokePath()
            // Start marker.
            let s = snapshot.point(for: coords[0])
            cg.setFillColor(good.cgColor)
            cg.fillEllipse(in: CGRect(x: s.x - 4, y: s.y - 4, width: 8, height: 8))
        }
    }
}

// Draws the GPS track as a normalized polyline — a recognizable "shape of the
// ride" glyph, used as an instant/offline fallback for the map snapshot.
struct RouteThumbnail: View {
    let coords: [CLLocationCoordinate2D]

    var body: some View {
        Canvas { ctx, size in
            guard coords.count > 1 else { return }
            let lats = coords.map(\.latitude), lons = coords.map(\.longitude)
            let minLat = lats.min()!, maxLat = lats.max()!
            let minLon = lons.min()!, maxLon = lons.max()!
            let meanLat = (minLat + maxLat) / 2
            let cosLat = max(0.01, Foundation.cos(meanLat * .pi / 180))
            let w = (maxLon - minLon) * cosLat
            let h = maxLat - minLat
            guard w > 0 || h > 0 else { return }
            // Fit the track into the box, preserving aspect ratio, north up.
            let scale = min(w > 0 ? size.width / w : .greatestFiniteMagnitude,
                            h > 0 ? size.height / h : .greatestFiniteMagnitude)
            let offX = (size.width - w * scale) / 2
            let offY = (size.height - h * scale) / 2
            func pt(_ c: CLLocationCoordinate2D) -> CGPoint {
                let x = offX + (c.longitude - minLon) * cosLat * scale
                let y = size.height - (offY + (c.latitude - minLat) * scale)
                return CGPoint(x: x, y: y)
            }
            var path = Path()
            path.move(to: pt(coords[0]))
            for c in coords.dropFirst() { path.addLine(to: pt(c)) }
            ctx.stroke(path, with: .color(Palette.accent),
                       style: StrokeStyle(lineWidth: 2, lineCap: .round, lineJoin: .round))
            ctx.fill(Path(ellipseIn: CGRect(x: pt(coords[0]).x - 2.5,
                                            y: pt(coords[0]).y - 2.5,
                                            width: 5, height: 5)),
                     with: .color(Palette.good))
        }
    }
}

// UIKit share sheet wrapper.
struct ShareSheet: UIViewControllerRepresentable {
    let items: [Any]
    func makeUIViewController(context: Context) -> UIActivityViewController {
        UIActivityViewController(activityItems: items, applicationActivities: nil)
    }
    func updateUIViewController(_ vc: UIActivityViewController, context: Context) {}
}

extension URL: @retroactive Identifiable {
    public var id: String { absoluteString }
}
