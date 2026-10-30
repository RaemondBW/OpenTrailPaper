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
            VStack(spacing: 0) {
                header
                // Fits on one screen: the map takes whatever height the stats
                // and upload grid leave, and the page only scrolls once even a
                // short map doesn't fit (a long list of connected accounts).
                GeometryReader { geo in
                    ScrollView {
                        VStack(spacing: 12) {
                            map
                                .frame(height: max(Self.minMapHeight, geo.size.height - restHeight - 12 - 36))
                                .einkBorder()

                            VStack(spacing: 12) {
                                statsBox
                                // Straight to a connected account (Settings > Accounts);
                                // the share sheet below stays for everything else.
                                SyncUploadButtons(fileURL: fileURL, defaultName: dateTitle)
                                SecondaryButton(title: "Share .fit file",
                                                systemImage: "square.and.arrow.up") {
                                    showShare = true
                                }
                            }
                            .onGeometryChange(for: CGFloat.self, of: { $0.size.height }) { restHeight = $0 }
                        }
                        .padding(.horizontal, 16).padding(.top, 12).padding(.bottom, 24)
                    }
                    .scrollBounceBehavior(.basedOnSize)
                }
            }
            .background(Palette.paper.ignoresSafeArea())
            .navigationBarHidden(true)
            .sheet(isPresented: $showShare) { ShareSheet(items: [fileURL]) }
        }
    }

    private static let minMapHeight: CGFloat = 180

    // Square back button, the ride's name in CAPS and its date, 3pt rule.
    private var header: some View {
        VStack(spacing: 0) {
            HStack(spacing: 12) {
                SquareIconButton(systemName: "chevron.left", label: "Back to rides") { dismiss() }
                VStack(alignment: .leading, spacing: 0) {
                    Text(names.name(for: fileURL.lastPathComponent, default: rideName).uppercased())
                        .font(DisplayFont.anton(24)).foregroundStyle(Palette.ink)
                        .lineLimit(1).minimumScaleFactor(0.6)
                        .accessibilityAddTraits(.isHeader)
                    Text(dateLine.uppercased())
                        .font(BarlowFont.text(12, .bold)).tracking(2).foregroundStyle(Palette.ink)
                }
                Spacer(minLength: 0)
            }
            .padding(.horizontal, 16).padding(.top, 12).padding(.bottom, 10)
            Rectangle().fill(Palette.ink).frame(height: Rule.title)
        }
    }

    // Muted, desaturated Apple map with the ride in ink: a white-cased black
    // line, a ring at the start and a square at the end.
    private var map: some View {
        Map {
            MapPolyline(polyline).stroke(.white, lineWidth: 10)
            MapPolyline(polyline).stroke(.black, lineWidth: 5)
            if let first = preview.coordinates.first {
                Annotation("Start", coordinate: first) {
                    Circle().fill(.white).frame(width: 14, height: 14)
                        .overlay(Circle().strokeBorder(.black, lineWidth: 3))
                }
                .annotationTitles(.hidden)
            }
            if let last = preview.coordinates.last {
                Annotation("End", coordinate: last) {
                    Rectangle().fill(.black).frame(width: 14, height: 14)
                }
                .annotationTitles(.hidden)
            }
        }
        .mapStyle(.standard(emphasis: .muted, pointsOfInterest: .excludingAll))
        .saturation(0)
        .accessibilityLabel("Map of the ride")
    }

    /// Mirrors the device's ride-complete screen in one box: distance,
    /// moving time and DEM ascent, then avg speed / avg + normalized power /
    /// avg HR, read straight from the FIT session. 2pt rules between cells.
    private var statsBox: some View {
        VStack(spacing: 0) {
            HStack(spacing: 0) {
                statCell("Distance", String(format: "%.1f", Units.distance(preview.distanceKm, miles: useMiles)),
                         Units.distLabel(useMiles), 38, small: false)
                vRule
                statCell("Moving", movingText, "", 26, small: false)
                vRule
                statCell("Ascent", String(format: "%.0f", Units.elevation(preview.ascentM, miles: useMiles)),
                         Units.elevLabel(useMiles), 26, small: false)
            }
            .fixedSize(horizontal: false, vertical: true)
            Rectangle().fill(Palette.ink).frame(height: Rule.divider)
            HStack(spacing: 0) {
                statCell("Avg spd", String(format: "%.1f", Units.speed(preview.avgSpeedKmh, miles: useMiles)), "", 22, small: true)
                vRule
                statCell("Avg pwr", preview.avgPower.map(String.init) ?? "--", "", 22, small: true)
                vRule
                statCell("NP", preview.normPower.map(String.init) ?? "--", "", 22, small: true)
                vRule
                statCell("Avg HR", preview.avgHeartRate.map(String.init) ?? "--", "", 22, small: true)
            }
            .fixedSize(horizontal: false, vertical: true)
        }
        .einkBorder()
    }

    private var vRule: some View {
        Rectangle().fill(Palette.ink).frame(width: Rule.divider)
    }

    private func statCell(_ label: String, _ value: String, _ unit: String,
                          _ size: CGFloat, small: Bool) -> some View {
        VStack(alignment: .leading, spacing: 0) {
            Text(label.uppercased())
                .font(BarlowFont.text(small ? 10 : 11, .bold)).tracking(small ? 1.5 : 2)
                .foregroundStyle(Palette.ink).lineLimit(1).minimumScaleFactor(0.7)
            Spacer(minLength: 0)
            HStack(alignment: .firstTextBaseline, spacing: 3) {
                Text(value).font(DisplayFont.anton(size)).foregroundStyle(Palette.ink)
                    .lineLimit(1).minimumScaleFactor(0.5)
                if !unit.isEmpty {
                    Text(unit.uppercased()).font(BarlowFont.text(small ? 11 : 12, .bold))
                        .foregroundStyle(Palette.ink)
                }
            }
        }
        .padding(.horizontal, small ? 10 : 12).padding(.vertical, 8)
        .frame(maxWidth: .infinity, alignment: .leading)
        .accessibilityElement(children: .combine)
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
    /// "Sunday morning ride", as the Rides list names it.
    private var rideName: String {
        guard let d = preview.start else { return "Ride" }
        let wd = d.formatted(.dateTime.weekday(.wide))
        let h = Calendar.current.component(.hour, from: d)
        let part = h < 5 ? "night" : h < 12 ? "morning" : h < 17 ? "afternoon"
                 : h < 21 ? "evening" : "night"
        return "\(wd) \(part) ride"
    }
    /// "Jul 19 · 7:15".
    private var dateLine: String {
        guard let d = preview.start else { return "" }
        return d.formatted(.dateTime.month(.abbreviated).day()) + " · "
             + d.formatted(.dateTime.hour().minute())
    }
}
