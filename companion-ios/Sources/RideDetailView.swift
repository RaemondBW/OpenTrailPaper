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

    private var polyline: MKPolyline {
        MKPolyline(coordinates: preview.coordinates, count: preview.coordinates.count)
    }

    var body: some View {
        NavigationStack {
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
                    .frame(height: 300)
                    .clipShape(RoundedRectangle(cornerRadius: 18))
                    .overlay(RoundedRectangle(cornerRadius: 18)
                        .strokeBorder(Palette.hairline, lineWidth: 1))

                    HStack(spacing: 16) {
                        Stat(label: "Distance",
                             value: String(format: "%.1f",
                                            Units.distance(preview.distanceKm, miles: useMiles)),
                             unit: Units.distLabel(useMiles), big: true)
                        Stat(label: "Duration", value: durationText, unit: "")
                    }
                    HStack(spacing: 16) {
                        Stat(label: "Avg Speed",
                             value: String(format: "%.1f",
                                            Units.speed(preview.avgSpeedKmh, miles: useMiles)),
                             unit: Units.speedLabel(useMiles))
                        Stat(label: "Max Speed",
                             value: String(format: "%.1f",
                                            Units.speed(preview.maxSpeedKmh, miles: useMiles)),
                             unit: Units.speedLabel(useMiles))
                    }
                    HStack(spacing: 16) {
                        Stat(label: "Avg Power",
                             value: preview.avgPower.map(String.init) ?? "—", unit: "W")
                        Stat(label: "Avg HR",
                             value: preview.avgHeartRate.map(String.init) ?? "—",
                             unit: "bpm")
                    }
                    HStack(spacing: 16) {
                        Stat(label: "Ascent",
                             value: String(format: "%.0f",
                                            Units.elevation(preview.ascentM, miles: useMiles)),
                             unit: Units.elevLabel(useMiles))
                        Stat(label: "Points", value: "\(preview.points.count)", unit: "")
                    }

                    PrimaryButton(title: "Share .fit file",
                                  systemImage: "square.and.arrow.up") {
                        showShare = true
                    }
                }
                .padding(16)
            }
            .background(Palette.paper)
            .navigationTitle(dateTitle)
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Done") { dismiss() }
                }
            }
            .sheet(isPresented: $showShare) { ShareSheet(items: [fileURL]) }
        }
    }

    private var durationText: String {
        let s = Int(preview.duration)
        return String(format: "%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60)
    }
    private var dateTitle: String {
        guard let d = preview.start else { return "Ride" }
        let f = DateFormatter()
        f.dateFormat = "MMM d, HH:mm"
        return f.string(from: d)
    }
}
