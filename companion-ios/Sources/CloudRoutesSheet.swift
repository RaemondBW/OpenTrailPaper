import SwiftUI

/// The rider's saved routes on Strava or RideWithGPS. Picking one downloads its
/// GPX and parses it with GPXImporter; the Route screen then previews and sends
/// it like any other route (GPXExporter -> BLEManager.uploadRoute).
struct CloudRoutesSheet: View {
    let provider: SyncProvider
    let onPick: (ImportedRoute) -> Void
    @ObservedObject private var sync = SyncAccounts.shared
    @Environment(\.dismiss) private var dismiss
    @AppStorage(UnitPref.key) private var useMiles = false

    @State private var routes: [CloudRoute] = []
    @State private var nextPage: Int? = 1
    @State private var loading = false
    @State private var loadedOnce = false
    @State private var error: String?
    @State private var importing: String?

    var body: some View {
        NavigationStack {
            List {
                ForEach(routes) { r in
                    Button { pick(r) } label: {
                        HStack {
                            VStack(alignment: .leading, spacing: 2) {
                                Text(r.name).font(TypeScale.body).foregroundStyle(Palette.ink).lineLimit(1)
                                if let s = summary(r) {
                                    Text(s).font(.system(size: 12)).foregroundStyle(Palette.muted)
                                }
                            }
                            Spacer()
                            if importing == r.id { ProgressView() }
                        }
                    }
                    .disabled(importing != nil)
                }
                Section { footer }
            }
            .listStyle(.plain)
            .navigationTitle("From \(provider.title)")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") { dismiss() }
                }
            }
            .task { await loadMore() }
        }
    }

    @ViewBuilder private var footer: some View {
        VStack(spacing: 10) {
            if loading {
                ProgressView()
            } else if nextPage != nil && loadedOnce && error == nil {
                // The footer is only built once it scrolls into view, so this
                // loads the next page when the rider reaches the bottom (and
                // again straight away if a short page leaves it on screen).
                ProgressView().task(id: nextPage) { await loadMore() }
            } else if nextPage != nil && loadedOnce {
                Button("Try again") { Task { await loadMore() } }
                    .font(.system(size: 14, weight: .semibold)).foregroundStyle(Palette.accent)
            } else if loadedOnce && routes.isEmpty && error == nil {
                Text("No saved routes on \(provider.title) yet.")
                    .font(TypeScale.body).foregroundStyle(Palette.muted)
            }
            if let error {
                Text(error).font(.system(size: 13)).foregroundStyle(Palette.accentDark)
                    .multilineTextAlignment(.center)
            }
            if provider == .strava && !CloudRoutes.stravaSeesPrivateRoutes(sync.tokens[.strava]?.scope) {
                Text("Only your public Strava routes are listed. To include private ones, disconnect and connect Strava again in Settings > Accounts.")
                    .font(.system(size: 12)).foregroundStyle(Palette.faint)
                    .multilineTextAlignment(.center)
            }
        }
        .frame(maxWidth: .infinity)
        .padding(.vertical, 8)
        .listRowSeparator(.hidden)
    }

    private func loadMore() async {
        guard let page = nextPage, !loading else { return }
        loading = true
        error = nil
        defer { loading = false; loadedOnce = true }
        do {
            let got = try await sync.routes(provider, page: page)
            let have = Set(routes.map(\.id))
            routes += got.routes.filter { !have.contains($0.id) }
            nextPage = got.nextPage
        } catch {
            self.error = error.localizedDescription
        }
    }

    private func pick(_ r: CloudRoute) {
        importing = r.id
        error = nil
        Task {
            defer { importing = nil }
            do {
                let data = try await sync.routeGPX(provider, route: r)
                switch GPXImporter.parse(data, fallbackName: r.name) {
                case .success(let route):
                    onPick(route)
                    dismiss()
                case .failure(.noPoints): error = "That route has no points in it."
                case .failure(.notGPX): error = "That route didn't come back as GPX."
                }
            } catch {
                self.error = error.localizedDescription
            }
        }
    }

    /// "52.3 km · 812 m up", from whatever the provider reported.
    private func summary(_ r: CloudRoute) -> String? {
        var parts: [String] = []
        if let d = r.distanceMeters {
            parts.append(String(format: "%.1f %@", Units.distance(d / 1000, miles: useMiles), Units.distLabel(useMiles)))
        }
        if let e = r.elevationGainMeters {
            parts.append(String(format: "%.0f %@ up", Units.elevation(e, miles: useMiles), Units.elevLabel(useMiles)))
        }
        return parts.isEmpty ? nil : parts.joined(separator: " · ")
    }
}
