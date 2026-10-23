import SwiftUI
import CoreLocation
import MapKit
import CoreImage

// Lists recorded rides on the device and downloads a selected .fit file
// over BLE, then offers the system share sheet (Strava, Files, AirDrop…).
struct RidesView: View {
    @EnvironmentObject var ble: BLEManager
    @StateObject private var previews = RidePreviewStore()
    @State private var detail: RideDetailData?
    @State private var failedURL: URL?          // parsed badly → offer to share it

    // The device's last-known ride list (kept across disconnects and launches)
    // merged with the rides cached on this phone, newest first. Shown the same
    // way connected or not, and while the device is re-checked, so reconnecting
    // never blanks or rebuilds the list — a completed check only adds new rides
    // and drops ones the device no longer has (unless they're cached here).
    private var displayedRides: [RideFile] {
        BLEManager.mergeRides(device: ble.rides, cached: BLEManager.cachedRides())
    }

    // Show a cached ride immediately; otherwise pull it over BLE. Downloads are
    // queued by BLEManager, so tapping a second ride mid-transfer is safe.
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

    // Throw away a cached file that won't parse and fetch it again — the way
    // out for rides left corrupt on disk by an interrupted transfer.
    private func redownload(_ name: String) {
        try? FileManager.default.removeItem(at: BLEManager.cachedURL(for: name))
        previews.forget(name)
        ble.downloadRide(name)
    }

    // RIDES title, how many the device holds, and refresh / connect.
    private var header: some View {
        ScreenHeader(title: "Rides") {
            HStack(spacing: 10) {
                Text(headerStatus)
                    .font(BarlowFont.text(12, .bold)).tracking(2)
                    .foregroundStyle(Palette.ink)
                    .lineLimit(1).minimumScaleFactor(0.7)
                SquareIconButton(
                    systemName: ble.state == .connected ? "arrow.clockwise"
                                                        : "antenna.radiowaves.left.and.right",
                    label: ble.state == .connected ? "Check for new rides" : "Connect to device") {
                    if ble.state == .connected { ble.refreshRides() } else { ble.startScan() }
                }
            }
        }
    }

    private var headerStatus: String {
        guard ble.state == .connected else { return "OFFLINE" }
        return ble.rides.isEmpty ? "SYNCED" : "\(ble.rides.count) ON DEVICE"
    }

    // Shown while the device is actively recording, so the tab makes clear a ride
    // is underway (and why new rides haven't synced yet).
    private var inProgressBanner: some View {
        HStack(spacing: 12) {
            Tag(text: "● REC", inverted: true)
            VStack(alignment: .leading, spacing: 2) {
                Text("RIDE IN PROGRESS").font(BarlowFont.text(12, .bold)).tracking(2)
                Text("Recording to the SD card — it'll sync here when you stop.")
                    .font(BarlowFont.text(13))
            }
            Spacer(minLength: 0)
        }
        .foregroundStyle(Palette.ink)
        .padding(.horizontal, 12).padding(.vertical, 10)
        .einkBorder()
        .accessibilityElement(children: .combine)
    }

    // Inline status above the list while the device's ride list is being
    // fetched — the cached rides stay visible underneath it.
    private var checkingRow: some View {
        HStack(spacing: 10) {
            BlockSpinner()
            Text("CHECKING FOR NEW RIDES")
                .font(BarlowFont.text(12, .bold)).tracking(2).foregroundStyle(Palette.ink)
            Spacer()
        }
        .accessibilityElement(children: .combine)
        .accessibilityLabel("Checking for new rides")
        .transition(.opacity)
    }

    // Week roll-up from whatever previews are decoded (fills in as rows load).
    private func summary(_ rides: [RideFile]) -> some View {
        let recent = rides.filter { daysAgo($0.name) <= 7 }
        let loaded = recent.compactMap { previews.preview(for: $0.name) }
        let dist = loaded.reduce(0.0) { $0 + $1.distanceKm }
        let time = loaded.reduce(0.0) { $0 + $1.duration }
        return HStack(spacing: 0) {
            summaryCell("This week", "\(recent.count)", recent.count == 1 ? "ride" : "rides")
            Rectangle().fill(Palette.ink).frame(width: Rule.divider)
            summaryCell("Distance", String(format: "%.0f", Units.distance(dist, miles: useMiles)),
                        Units.distLabel(useMiles))
            Rectangle().fill(Palette.ink).frame(width: Rule.divider)
            summaryCell("Time", hoursMinutes(time), "hours")
        }
        .fixedSize(horizontal: false, vertical: true)
        .einkBorder()
    }

    private func summaryCell(_ label: String, _ value: String, _ unit: String) -> some View {
        VStack(alignment: .leading, spacing: 0) {
            Text(label).trackedLabel().lineLimit(1).minimumScaleFactor(0.7)
            Text(value).font(DisplayFont.anton(34)).foregroundStyle(Palette.ink)
                .lineLimit(1).minimumScaleFactor(0.6)
            Text(unit).font(BarlowFont.text(12)).foregroundStyle(Palette.ink)
        }
        .padding(.horizontal, 12).padding(.vertical, 10)
        .frame(maxWidth: .infinity, alignment: .leading)
        .accessibilityElement(children: .combine)
    }

    private func hoursMinutes(_ s: TimeInterval) -> String {
        let m = Int(s) / 60
        return String(format: "%d:%02d", m / 60, m % 60)
    }

    @AppStorage(UnitPref.key) private var useMiles = false
    /// Watched so a ride's tag turns "✓ STRAVA" as soon as its upload lands.
    @AppStorage(UploadLog.key) private var uploadsRaw = ""

    private func daysAgo(_ name: String) -> Int {
        guard let d = Self.rideDate(name) else { return 9999 }
        return Int(Date().timeIntervalSince(d) / 86400)
    }
    private func timeStrShort(_ s: TimeInterval) -> String {
        let m = Int(s) / 60, h = m / 60
        return h > 0 ? "\(h)h \(m % 60)m" : "\(m)m"
    }

    // Filenames look like 20260712-150300.fit — the device names them in the
    // RIDER'S wall-clock time (its timezone setting), so the name already says
    // what a human wants to read. Parse and display in the phone's own zone so
    // no conversion happens and the shown time equals the name, verbatim.
    // (Rides recorded before the local-naming firmware carry UTC names and
    // will display those UTC digits as-is — a one-time historical quirk.)
    static func rideDate(_ name: String) -> Date? {
        let base = name.replacingOccurrences(of: ".fit", with: "")
        let f = DateFormatter()
        f.dateFormat = "yyyyMMdd-HHmmss"
        return f.date(from: base)
    }

    var body: some View {
        NavigationStack {
            VStack(spacing: 0) {
            header
            ScrollView {
                let rides = displayedRides
                let checking = ble.state == .connected && ble.loadingRides
                VStack(spacing: 12) {
                    if ble.status.recording { inProgressBanner }
                    if checking { checkingRow }
                    if !rides.isEmpty { summary(rides) }

                    // With nothing cached yet, the checking row above stands in
                    // for the empty state until the device answers.
                    if rides.isEmpty && !checking {
                        Card {
                            VStack(alignment: .leading, spacing: 6) {
                                Text(ble.status.recording ? "No synced rides yet"
                                     : ble.state == .connected ? "No rides found"
                                                               : "No cached rides")
                                    .font(TypeScale.title).textCase(.uppercase)
                                    .foregroundStyle(Palette.ink)
                                Text(ble.status.recording
                                     ? "The current ride is being recorded to the SD card — it'll appear here once you finish and stop it."
                                     : ble.state == .connected
                                     ? "Rides are saved to the SD card and sync here automatically."
                                     : "Connect to your OpenTrailPaper to download rides. Downloaded rides stay here for offline viewing.")
                                    .font(BarlowFont.text(14)).foregroundStyle(Palette.muted)
                            }
                        }
                    } else if !rides.isEmpty {
                        ForEach(Array(rides.enumerated()), id: \.element.id) { i, ride in
                            RideRow(ride: ride,
                                    featured: i == 0,
                                    cached: BLEManager.isCached(ride.name),
                                    preview: previews.preview(for: ride.name),
                                    downloading: ble.downloadingName == ride.name,
                                    queued: ble.queuedDownloads.contains(ride.name),
                                    progress: ble.downloadProgress,
                                    uploads: UploadLog.providers(for: ride.name, raw: uploadsRaw),
                                    onTap: { open(ride) },
                                    onDelete: {
                                        ble.deleteRide(ride.name)
                                        previews.forget(ride.name)
                                    })
                                .onAppear { previews.ensureLoaded(ride.name) }
                        }
                        Text("Tap to preview & share · long-press to delete")
                            .font(BarlowFont.text(12)).foregroundStyle(Palette.muted)
                    }
                }
                .padding(16)
                .animation(.easeInOut(duration: 0.2), value: checking)
            }
            }
            .background(Palette.paper.ignoresSafeArea())
            .navigationBarHidden(true)
            .onAppear {
                let args = ProcessInfo.processInfo.arguments
                if args.contains("-demo-rides"),
                   let url = Bundle.main.url(forResource: "demo", withExtension: "fit"),
                   let data = try? Data(contentsOf: url) {
                    // Screenshot demo: seed a few recent rides into the cache so the
                    // Rides list renders with cards (all preview the bundled demo ride).
                    let names = ["20260719-071500.fit", "20260718-163000.fit", "20260716-090000.fit"]
                    let dir = BLEManager.cachedURL(for: "x").deletingLastPathComponent()
                    for f in (try? FileManager.default.contentsOfDirectory(at: dir, includingPropertiesForKeys: nil)) ?? []
                        where f.pathExtension == "fit" { try? FileManager.default.removeItem(at: f) }
                    let preview = FITDecoder.decode(data)
                    for n in names {
                        try? data.write(to: BLEManager.cachedURL(for: n))
                        if let preview { previews.store(preview, for: n) }
                    }
                } else if args.contains("-demo-ride"),
                   let url = Bundle.main.url(forResource: "demo", withExtension: "fit"),
                   let data = try? Data(contentsOf: url),
                   let preview = FITDecoder.decode(data) {
                    detail = RideDetailData(url: url, preview: preview)
                } else if ble.state == .connected, !ble.loadingRides,
                          !ble.ridesCheckedThisLink {
                    ble.refreshRides()
                }
            }
            .onChange(of: ble.state) { _, s in
                if s == .connected { ble.refreshRides() }   // fetch device list on connect
            }
            .onChange(of: ble.downloadedFileURL) { _, url in
                guard let url, let data = try? Data(contentsOf: url) else { return }
                // A queued ride that lands while the rider is looking at an
                // earlier one fills in its row rather than yanking the sheet
                // out from under them.
                let busy = detail != nil || failedURL != nil
                if let preview = FITDecoder.decode(data) {
                    previews.store(preview, for: url.lastPathComponent)  // fill the row thumbnail
                    if !busy { detail = RideDetailData(url: url, preview: preview) }
                } else if !busy {
                    failedURL = url
                }
            }
            .sheet(item: $detail) { d in
                RideDetailView(fileURL: d.url, preview: d.preview)
            }
            .sheet(item: $failedURL) { url in
                RideParseErrorView(url: url,
                                   canRetry: ble.state == .connected,
                                   onRetry: { redownload(url.lastPathComponent) })
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
    /// The newest ride: a big card with its map. The rest are compact rows.
    let featured: Bool
    let cached: Bool
    let preview: RidePreview?
    let downloading: Bool
    let queued: Bool
    let progress: Double
    let uploads: [SyncProvider]
    let onTap: () -> Void
    let onDelete: () -> Void
    @AppStorage(UnitPref.key) private var useMiles = false

    var body: some View {
        Button(action: onTap) {
            Group {
                if featured { bigCard } else { compactRow }
            }
            .background(Palette.paper)
            .einkBorder()
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityElement(children: .combine)
        .accessibilityHint(cached ? "Opens the ride" : "Downloads the ride from the device")
        .contextMenu {
            Button(role: .destructive, action: onDelete) {
                Label("Delete ride", systemImage: "trash")
            }
        }
    }

    // The latest ride: map on top, then name, date and four numbers.
    private var bigCard: some View {
        VStack(alignment: .leading, spacing: 0) {
            banner
                .frame(height: 150)
                .clipped()
                .overlay(alignment: .bottom) { Rectangle().fill(Palette.ink).frame(height: Rule.border) }
            VStack(alignment: .leading, spacing: 6) {
                HStack(alignment: .firstTextBaseline) {
                    Text(rideName).font(BarlowFont.text(16, .bold)).foregroundStyle(Palette.ink)
                        .lineLimit(1)
                    Spacer(minLength: 6)
                    Text(dateSubtitle.uppercased()).font(BarlowFont.text(12, .semibold))
                        .foregroundStyle(Palette.ink)
                }
                if let p = preview {
                    HStack(alignment: .top, spacing: 6) {
                        bigStat(String(format: "%.1f", Units.distance(p.distanceKm, miles: useMiles)),
                                Units.distLabel(useMiles))
                        bigStat(clock(p.duration), "Time")
                        bigStat(String(format: "%.1f", Units.speed(p.avgSpeedKmh, miles: useMiles)),
                                Units.speedLabel(useMiles))
                        bigStat(p.avgPower.map(String.init) ?? "--", "W avg")
                    }
                } else {
                    HStack { statusLine; Spacer(); tag }
                }
            }
            .padding(.horizontal, 12).padding(.vertical, 10)
        }
    }

    private func bigStat(_ value: String, _ label: String) -> some View {
        VStack(alignment: .leading, spacing: 0) {
            Text(value).font(DisplayFont.anton(22)).foregroundStyle(Palette.ink)
                .lineLimit(1).minimumScaleFactor(0.6)
            Text(label.uppercased()).font(BarlowFont.text(11, .bold)).tracking(1)
                .foregroundStyle(Palette.ink).lineLimit(1)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    // An older ride: track glyph, name and stats on one line, state tag.
    private var compactRow: some View {
        HStack(spacing: 12) {
            ZStack {
                Palette.paper
                if let p = preview, p.coordinates.count > 1 {
                    RouteThumbnail(coords: p.coordinates).padding(6)
                } else {
                    Image(systemName: cached ? "map" : "arrow.down")
                        .font(.system(size: 16, weight: .bold)).foregroundStyle(Palette.ink)
                }
            }
            .frame(width: 64, height: 48)
            .einkBorder()
            .accessibilityHidden(true)
            VStack(alignment: .leading, spacing: 2) {
                Text(rideName).font(BarlowFont.text(15, .bold)).foregroundStyle(Palette.ink)
                    .lineLimit(1).minimumScaleFactor(0.8)
                statusLine
            }
            Spacer(minLength: 4)
            tag
        }
        .padding(.horizontal, 12).padding(.vertical, 10)
    }

    @ViewBuilder private var statusLine: some View {
        if let p = preview {
            Text(String(format: "%.1f %@ · %@ · %.1f %@",
                        Units.distance(p.distanceKm, miles: useMiles), Units.distLabel(useMiles),
                        clock(p.duration),
                        Units.speed(p.avgSpeedKmh, miles: useMiles), Units.speedLabel(useMiles)))
                .font(BarlowFont.text(13)).foregroundStyle(Palette.ink).lineLimit(1)
                .minimumScaleFactor(0.8)
        } else if downloading {
            BlockProgressBar(value: progress, segments: 8, height: 8).frame(maxWidth: 120)
        } else {
            Text(dateSubtitle + String(format: " · %.0f KB", Double(ride.size) / 1024))
                .font(BarlowFont.text(13)).foregroundStyle(Palette.ink).lineLimit(1)
        }
    }

    // ✓ STRAVA once uploaded; else where the ride is, or the transfer state.
    @ViewBuilder private var tag: some View {
        if downloading {
            Tag(text: "\(Int(progress * 100))%")
        } else if queued {
            Tag(text: "Queued")
        } else if let first = uploads.first {
            Tag(text: "✓ " + first.shortTitle + (uploads.count > 1 ? " +\(uploads.count - 1)" : ""),
                inverted: true)
        } else if cached {
            Tag(text: "On phone")
        } else {
            Tag(text: "On device")
        }
    }

    // Map banner for the big card: a real map snapshot with the route drawn on
    // it once the ride is decoded, otherwise a prompt to download.
    @ViewBuilder private var banner: some View {
        ZStack {
            Palette.paper
            if let p = preview, p.coordinates.count > 1 {
                RouteMapThumbnail(name: ride.name, coords: p.coordinates)
            } else if downloading {
                VStack(spacing: 8) {
                    BlockProgressBar(value: progress).frame(maxWidth: 180)
                    Text("DOWNLOADING…").font(BarlowFont.text(12, .bold)).tracking(2)
                }
            } else if queued {
                VStack(spacing: 8) {
                    BlockSpinner()
                    Text("QUEUED — STARTS AFTER THE CURRENT DOWNLOAD")
                        .font(BarlowFont.text(11, .bold)).tracking(1.5)
                        .multilineTextAlignment(.center)
                        .padding(.horizontal, 16)
                }
            } else {
                VStack(spacing: 8) {
                    Image(systemName: cached ? "map" : "arrow.down.square")
                        .font(.system(size: 28, weight: .bold))
                    Text(cached ? "LOADING MAP…" : "TAP TO DOWNLOAD")
                        .font(BarlowFont.text(12, .bold)).tracking(2)
                }
                .padding(14)
                .background(Palette.paper)
                .frame(maxWidth: .infinity, maxHeight: .infinity)
                .background(HatchedBackground())
            }
        }
        .foregroundStyle(Palette.ink)
    }

    private func clock(_ s: TimeInterval) -> String {
        let m = Int(s) / 60
        return String(format: "%d:%02d", m / 60, m % 60)
    }

    // Human name like "Saturday afternoon ride" from the file's timestamp.
    private var rideName: String {
        guard let d = RidesView.rideDate(ride.name) else { return ride.name }
        let wd = d.formatted(.dateTime.weekday(.wide))
        let h = Calendar.current.component(.hour, from: d)
        let part = h < 5 ? "night" : h < 12 ? "morning" : h < 17 ? "afternoon"
                 : h < 21 ? "evening" : "night"
        return "\(wd) \(part) ride"
    }
    private var dateSubtitle: String {
        guard let d = RidesView.rideDate(ride.name) else { return "" }
        return d.formatted(.dateTime.month(.abbreviated).day()) + " · "
             + d.formatted(.dateTime.hour().minute())
    }
}

// Shown when a downloaded ride can't be decoded — lets the user share the
// raw .fit so it can be inspected.
private struct RideParseErrorView: View {
    let url: URL
    let canRetry: Bool
    let onRetry: () -> Void
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        VStack(spacing: 20) {
            Image(systemName: "exclamationmark.triangle.fill")
                .font(.system(size: 40, weight: .bold))
                .foregroundStyle(Palette.ink)
                .frame(width: 72, height: 72)
                .background(HatchedBackground())
                .einkBorder()
            Text("Couldn't read this ride")
                .font(TypeScale.title).textCase(.uppercase).foregroundStyle(Palette.ink)
            Text(canRetry
                 ? "The app couldn't parse this file. Download it again from the device, or share the raw .fit so it can be analyzed."
                 : "The full file downloaded but the app couldn't parse it. Share the raw .fit file so it can be analyzed.")
                .font(TypeScale.body).foregroundStyle(Palette.muted)
                .multilineTextAlignment(.center)
            if canRetry {
                PrimaryButton(title: "Download again", systemImage: "arrow.clockwise") {
                    dismiss()
                    onRetry()
                }
            }
            ShareLink(item: url) {
                BlockButtonLabel(title: "Share .fit file", systemImage: "square.and.arrow.up",
                                 filled: !canRetry)
                    .frame(maxWidth: .infinity)
                    .background(canRetry ? Palette.paper : Palette.ink)
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
    private static func key(_ name: String, _ size: CGSize) -> NSString {
        "\(name)@\(Int(size.width))x\(Int(size.height))" as NSString
    }

    var body: some View {
        GeometryReader { geo in
            ZStack {
                // Read the cache synchronously too, so a row that SwiftUI
                // rebuilds shows its map at once instead of the glyph first.
                if let image = image ?? Self.cache.object(forKey: Self.key(name, geo.size)) {
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
        let key = Self.key(name, size)
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
        // Muted, then fully desaturated below: the map sits in the app's
        // black-and-white look with the ride drawn on it in ink.
        opts.preferredConfiguration = MKStandardMapConfiguration(emphasisStyle: .muted)

        let snapshotter = MKMapSnapshotter(options: opts)
        let snapshot: MKMapSnapshotter.Snapshot? = await withCheckedContinuation { cont in
            snapshotter.start(with: .global(qos: .utility)) { snap, _ in
                cont.resume(returning: snap)
            }
        }
        guard let snapshot else { return nil }

        let base = grayscale(snapshot.image) ?? snapshot.image
        let renderer = UIGraphicsImageRenderer(size: snapshot.image.size)
        return renderer.image { rctx in
            base.draw(in: CGRect(origin: .zero, size: snapshot.image.size))
            let cg = rctx.cgContext
            cg.setLineJoin(.round); cg.setLineCap(.round)
            let path = CGMutablePath()
            for (i, c) in coords.enumerated() {
                let pt = snapshot.point(for: c)
                if i == 0 { path.move(to: pt) } else { path.addLine(to: pt) }
            }
            // White casing under the black line, as the device draws routes.
            cg.addPath(path); cg.setLineWidth(7); cg.setStrokeColor(UIColor.white.cgColor); cg.strokePath()
            cg.addPath(path); cg.setLineWidth(3.5); cg.setStrokeColor(UIColor.black.cgColor); cg.strokePath()
            // Start: a white ring. End: a black square.
            let s = snapshot.point(for: coords[0])
            let ring = CGRect(x: s.x - 5, y: s.y - 5, width: 10, height: 10)
            cg.setFillColor(UIColor.white.cgColor); cg.fillEllipse(in: ring)
            cg.setStrokeColor(UIColor.black.cgColor); cg.setLineWidth(2.5); cg.strokeEllipse(in: ring)
            let e = snapshot.point(for: coords[coords.count - 1])
            cg.setFillColor(UIColor.black.cgColor)
            cg.fill(CGRect(x: e.x - 5, y: e.y - 5, width: 10, height: 10))
        }
    }

    /// The snapshot without colour, contrast nudged up so roads stay legible.
    private static func grayscale(_ image: UIImage) -> UIImage? {
        guard let input = CIImage(image: image),
              let filter = CIFilter(name: "CIColorControls") else { return nil }
        filter.setValue(input, forKey: kCIInputImageKey)
        filter.setValue(0.0, forKey: kCIInputSaturationKey)
        filter.setValue(1.15, forKey: kCIInputContrastKey)
        filter.setValue(0.04, forKey: kCIInputBrightnessKey)
        guard let out = filter.outputImage,
              let cg = CIContext().createCGImage(out, from: input.extent) else { return nil }
        return UIImage(cgImage: cg, scale: image.scale, orientation: image.imageOrientation)
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
            ctx.stroke(path, with: .color(Palette.ink),
                       style: StrokeStyle(lineWidth: 3, lineCap: .round, lineJoin: .round))
            ctx.fill(Path(ellipseIn: CGRect(x: pt(coords[0]).x - 2.5,
                                            y: pt(coords[0]).y - 2.5,
                                            width: 5, height: 5)),
                     with: .color(Palette.ink))
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
