import SwiftUI
import MapKit

// Preview of a downloaded ride: the track on a map plus summary stats,
// with a Share button for the original .fit file.
struct RideDetailView: View {
    let fileURL: URL
    let preview: RidePreview
    @Environment(\.dismiss) private var dismiss
    @AppStorage(UnitPref.key) private var useMiles = false
    @State private var showShare = false
    @State private var restHeight: CGFloat = 0
    @ObservedObject private var names = RideNames.shared

    private var polyline: MKPolyline {
        MKPolyline(coordinates: preview.coordinates, count: preview.coordinates.count)
    }

    var body: some View {
        NavigationStack {
            // Fits on one screen: the map takes whatever height the stats and
            // upload grid leave, and the page only scrolls once even a short
            // map doesn't fit (a long list of connected accounts).
            GeometryReader { geo in
                ScrollView {
                    VStack(spacing: 16) {
                        Map {
                            MapPolyline(polyline).stroke(Palette.accent, lineWidth: 4)
                            if let first = preview.coordinates.first {
                                Marker("Start", coordinate: first).tint(Palette.good)
                            }
                            if let last = preview.coordinates.last {
                                Marker("End", coordinate: last).tint(Palette.accent)
                            }
                        }
                        .frame(height: max(Self.minMapHeight, geo.size.height - restHeight - 16 - 32))
                        .clipShape(RoundedRectangle(cornerRadius: 18))
                        .overlay(RoundedRectangle(cornerRadius: 18)
                            .strokeBorder(Palette.hairline, lineWidth: 1))

                        VStack(spacing: 16) {
                            statsCard
                            // Straight to a connected account (Settings > Accounts);
                            // the share sheet below stays for everything else.
                            SyncUploadButtons(fileURL: fileURL, defaultName: dateTitle)
                            PrimaryButton(title: "Share .fit file",
                                          systemImage: "square.and.arrow.up") {
                                showShare = true
                            }
                        }
                        .onGeometryChange(for: CGFloat.self, of: { $0.size.height }) { restHeight = $0 }
                    }
                    .padding(16)
                }
                .scrollBounceBehavior(.basedOnSize)
            }
            .background(Palette.paper)
            .navigationTitle(names.name(for: fileURL.lastPathComponent, default: dateTitle))
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Done") { dismiss() }
                }
            }
            .sheet(isPresented: $showShare) { ShareSheet(items: [fileURL]) }
        }
    }

    private static let minMapHeight: CGFloat = 180

    /// Mirrors the device's ride-complete screen in one card: distance,
    /// moving time and DEM ascent, then avg speed / avg + normalized power /
    /// avg HR, read straight from the FIT session.
    private var statsCard: some View {
        Card(padding: 14) {
            VStack(spacing: 12) {
                statRow([
                    ("Distance", String(format: "%.1f", Units.distance(preview.distanceKm, miles: useMiles)),
                     Units.distLabel(useMiles), 40),
                    ("Moving Time", movingText, "", 26),
                    ("Ascent", String(format: "%.0f", Units.elevation(preview.ascentM, miles: useMiles)),
                     Units.elevLabel(useMiles), 26),
                ])
                Rectangle().fill(Palette.hairline).frame(height: 1)
                statRow([
                    ("Avg Speed", String(format: "%.1f", Units.speed(preview.avgSpeedKmh, miles: useMiles)),
                     Units.speedLabel(useMiles), 22),
                    ("Avg Power", preview.avgPower.map(String.init) ?? "—", "W", 22),
                    ("Norm Power", preview.normPower.map(String.init) ?? "—", "W", 22),
                    ("Avg HR", preview.avgHeartRate.map(String.init) ?? "—", "bpm", 22),
                ])
            }
        }
    }

    /// Labels on one line and values on the next, so both line up across
    /// columns even when one value is set larger.
    private func statRow(_ cells: [(label: String, value: String, unit: String, size: CGFloat)]) -> some View {
        VStack(spacing: 2) {
            HStack(spacing: 8) {
                ForEach(cells, id: \.label) { c in
                    Text(c.label).font(BarlowFont.condensed(11, .medium)).tracking(0.8).textCase(.uppercase)
                        .foregroundStyle(Palette.muted).lineLimit(1).minimumScaleFactor(0.8)
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
            }
            HStack(alignment: .lastTextBaseline, spacing: 8) {
                ForEach(cells, id: \.label) { c in
                    HStack(alignment: .firstTextBaseline, spacing: 3) {
                        Text(c.value).font(c.size > 30 ? TypeScale.hero(c.size) : TypeScale.value(c.size))
                            .foregroundStyle(Palette.ink).lineLimit(1).minimumScaleFactor(0.6)
                        if !c.unit.isEmpty {
                            Text(c.unit).font(BarlowFont.text(11)).foregroundStyle(Palette.muted)
                        }
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
                }
            }
        }
    }

    private var movingText: String {
        let s = Int(preview.movingTime)
        return String(format: "%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60)
    }
    private var dateTitle: String {
        guard let d = preview.start else { return "Ride" }
        let f = DateFormatter()
        f.dateFormat = "MMM d, HH:mm"
        return f.string(from: d)
    }
}
