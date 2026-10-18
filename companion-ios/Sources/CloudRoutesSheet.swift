import SwiftUI

/// "Import a route": the connected accounts with saved routes, each opening
/// that account's list in the same sheet. Picking a route downloads its GPX and
/// parses it with GPXImporter; the Route screen then previews and sends it like
/// any other route (GPXExporter -> BLEManager.uploadRoute).
struct CloudRoutesSheet: View {
    let onPick: (ImportedRoute) -> Void
    @ObservedObject private var sync = SyncAccounts.shared
    @Environment(\.dismiss) private var dismiss
    @State private var path: [SyncProvider] = []
    @State private var detent: PresentationDetent = .medium

    var body: some View {
        NavigationStack(path: $path) {
            sources
                .navigationDestination(for: SyncProvider.self) { p in
                    CloudRoutesList(provider: p, onPick: { onPick($0); dismiss() }, close: { dismiss() })
                }
        }
        // The picker is short; a list wants the whole screen.
        .presentationDetents([.medium, .large], selection: $detent)
        .presentationDragIndicator(.visible)
        .presentationBackground(Palette.paper)
        .onChange(of: path) { detent = path.isEmpty ? .medium : .large }
    }

    private var sources: some View {
        let connected = SyncAccounts.routeProviders.filter { sync.isConnected($0) }
        return ScrollView {
            VStack(alignment: .leading, spacing: 14) {
                HStack {
                    Text("Import a route").font(BarlowFont.condensed(30, .bold)).foregroundStyle(Palette.ink)
                    Spacer()
                    RoundButton(systemName: "xmark", tint: Palette.muted, label: "Close") { dismiss() }
                }
                Text("Pick one of your saved routes. It goes to the device like any route you plan here.")
                    .font(BarlowFont.text(15)).foregroundStyle(Palette.muted)
                    .padding(.top, -6)
                Card(padding: 0) {
                    VStack(spacing: 0) {
                        ForEach(connected) { p in
                            NavigationLink(value: p) { providerRow(p) }.buttonStyle(.plain)
                            if p != connected.last {
                                Rectangle().fill(Palette.hairline).frame(height: 1).padding(.leading, 74)
                            }
                        }
                    }
                }
                // Guidance only: nothing in the app can open Settings › Accounts
                // from here (the tab and its scroll position are RootView's own).
                if connected.count < SyncAccounts.routeProviders.count {
                    Card(padding: 0) {
                        HStack(spacing: 14) {
                            RoundedRectangle(cornerRadius: 12, style: .continuous)
                                .strokeBorder(Palette.faint, style: StrokeStyle(lineWidth: 1, dash: [3, 3]))
                                .frame(width: 44, height: 44)
                                .overlay(Image(systemName: "plus").font(.system(size: 16, weight: .semibold))
                                    .foregroundStyle(Palette.muted))
                            VStack(alignment: .leading, spacing: 2) {
                                Text("Connect another account").font(BarlowFont.text(15, .semibold))
                                    .foregroundStyle(Palette.ink)
                                Text("Settings › Accounts").font(BarlowFont.text(13)).foregroundStyle(Palette.muted)
                            }
                        }
                        .padding(.vertical, 14).padding(.horizontal, 16)
                    }
                }
            }
            .padding(.horizontal, 16).padding(.top, 20).padding(.bottom, 24)
        }
        .background(Palette.paper.ignoresSafeArea())
        .toolbar(.hidden, for: .navigationBar)
    }

    private func providerRow(_ p: SyncProvider) -> some View {
        HStack(spacing: 14) {
            RoundedRectangle(cornerRadius: 12, style: .continuous).fill(Palette.accentWash)
                .frame(width: 44, height: 44)
                .overlay(Image(systemName: p == .strava ? "point.topleft.down.to.point.bottomright.curvepath" : "bicycle")
                    .font(.system(size: 19, weight: .semibold)).foregroundStyle(Palette.accent))
            VStack(alignment: .leading, spacing: 2) {
                Text(p.title).font(BarlowFont.condensed(21, .semibold)).foregroundStyle(Palette.ink)
                HStack(spacing: 6) {
                    Circle().fill(Palette.good).frame(width: 7, height: 7)
                    Text("Connected · saved routes").font(BarlowFont.text(13)).foregroundStyle(Palette.muted)
                }
            }
            Spacer(minLength: 0)
            Image(systemName: "chevron.right").font(.system(size: 14, weight: .semibold))
                .foregroundStyle(Palette.muted)
        }
        .padding(16)
        .contentShape(Rectangle())
    }
}

/// One account's saved routes, a page at a time.
private struct CloudRoutesList: View {
    let provider: SyncProvider
    let onPick: (ImportedRoute) -> Void
    let close: () -> Void
    @ObservedObject private var sync = SyncAccounts.shared
    /// Pops back to the account picker.
    @Environment(\.dismiss) private var back
    @AppStorage(UnitPref.key) private var useMiles = false

    @State private var routes: [CloudRoute] = []
    @State private var nextPage: Int? = 1
    @State private var loading = false
    @State private var loadError: String?
    @State private var pickError: String?
    @State private var importing: String?

    var body: some View {
        VStack(spacing: 0) {
            header
            ScrollView {
                LazyVStack(spacing: 10) {
                    if provider == .strava && !CloudRoutes.stravaSeesPrivateRoutes(sync.tokens[.strava]?.scope) {
                        publicOnlyNote
                    }
                    ForEach(routes) { r in
                        Button { pick(r) } label: { row(r) }
                            .buttonStyle(.plain)
                            .disabled(importing != nil)
                            .opacity(importing != nil && importing != r.id ? 0.45 : 1)
                    }
                    footer
                }
                .padding(16)
            }
        }
        .background(Palette.paper.ignoresSafeArea())
        .toolbar(.hidden, for: .navigationBar)
        .safeAreaInset(edge: .bottom) {
            if let pickError {
                ErrorCard(title: "That route didn't import", message: pickError)
                    .onTapGesture { self.pickError = nil }
                    .padding(16)
            }
        }
    }

    private var header: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                RoundButton(systemName: "chevron.left", tint: Palette.ink, label: "Back") { back() }
                Spacer()
                RoundButton(systemName: "xmark", tint: Palette.muted, label: "Close", action: close)
            }
            HStack(alignment: .firstTextBaseline) {
                Text(provider.title).font(TypeScale.screenTitle).foregroundStyle(Palette.ink)
                Spacer()
                Text(countLabel).trackedLabel()
            }
        }
        .padding(.horizontal, 16).padding(.top, 20).padding(.bottom, 10)
        .overlay(alignment: .bottom) { Rectangle().fill(Palette.hairline).frame(height: 1) }
    }

    /// What's loaded so far; "+" while more pages may follow.
    private var countLabel: String {
        if !routes.isEmpty { return "\(routes.count)\(nextPage != nil ? "+" : "") saved routes" }
        return nextPage == nil && loadError == nil ? "No routes yet" : "Saved routes"
    }

    private func row(_ r: CloudRoute) -> some View {
        Card(padding: 12) {
            HStack(spacing: 14) {
                RouteThumb(shape: r.shape)
                VStack(alignment: .leading, spacing: 4) {
                    Text(r.name).font(BarlowFont.condensed(20, .semibold)).foregroundStyle(Palette.ink)
                        .lineLimit(1)
                    HStack(alignment: .firstTextBaseline, spacing: 16) {
                        if let d = r.distanceMeters {
                            stat(String(format: "%.1f", Units.distance(d / 1000, miles: useMiles)),
                                 Units.distLabel(useMiles))
                        }
                        if let e = r.elevationGainMeters {
                            stat(Int(Units.elevation(e, miles: useMiles).rounded()).formatted(),
                                 "\(Units.elevLabel(useMiles)) climb")
                        }
                    }
                }
                Spacer(minLength: 0)
                if importing == r.id {
                    ProgressView()
                } else {
                    Image(systemName: "chevron.right").font(.system(size: 14, weight: .semibold))
                        .foregroundStyle(Palette.faint)
                }
            }
            .padding(.trailing, 2)
        }
        .contentShape(Rectangle())
    }

    private func stat(_ value: String, _ unit: String) -> some View {
        HStack(alignment: .firstTextBaseline, spacing: 3) {
            Text(value).font(BarlowFont.condensed(18, .semibold)).foregroundStyle(Palette.ink)
            Text(unit).font(BarlowFont.text(12)).foregroundStyle(Palette.muted)
        }
    }

    @ViewBuilder private var footer: some View {
        if let loadError {
            ErrorCard(title: "Your routes didn't load", message: loadError) {
                Task { await loadMore() }
            }
        } else if nextPage != nil {
            // The footer is only built once it scrolls into view, so this
            // loads the first page, then the next whenever the rider reaches
            // the bottom (and again straight away if a short page leaves it
            // on screen).
            HStack(spacing: 10) {
                ProgressView().controlSize(.small).tint(Palette.accent)
                Text(routes.isEmpty ? "Loading routes" : "Loading more").trackedLabel()
            }
            .frame(maxWidth: .infinity)
            .padding(.top, 14).padding(.bottom, 40)
            .task(id: nextPage) { await loadMore() }
        } else if routes.isEmpty {
            Card(padding: 20) {
                VStack(spacing: 10) {
                    // A dashed route, matching the mockup's empty glyph.
                    ZStack(alignment: .topLeading) {
                        Path { p in
                            p.move(to: CGPoint(x: 6, y: 32))
                            p.addCurve(to: CGPoint(x: 22, y: 16), control1: CGPoint(x: 12, y: 20), control2: CGPoint(x: 16, y: 26))
                            p.addCurve(to: CGPoint(x: 34, y: 8), control1: CGPoint(x: 28, y: 6), control2: CGPoint(x: 32, y: 10))
                        }
                        .stroke(Palette.faint, style: StrokeStyle(lineWidth: 2.4, lineCap: .round, dash: [4, 5]))
                        Circle().fill(Palette.faint).frame(width: 6, height: 6).offset(x: 3, y: 29)
                    }
                    .frame(width: 40, height: 40)
                    Text("Nothing saved on \(provider.title)").font(BarlowFont.condensed(21, .semibold))
                        .foregroundStyle(Palette.ink)
                    Text("Routes you save there show up here.").font(BarlowFont.text(14))
                        .foregroundStyle(Palette.muted)
                }
                .multilineTextAlignment(.center)
                .frame(maxWidth: .infinity)
                .padding(.vertical, 4)
            }
        }
    }

    // There is no way to open Settings › Accounts from here, so no link.
    private var publicOnlyNote: some View {
        HStack(alignment: .top, spacing: 12) {
            Image(systemName: "lock").font(.system(size: 16, weight: .semibold))
                .foregroundStyle(Palette.accentDark)
            Text("Private routes are hidden. Reconnect Strava in Settings › Accounts to include them.")
                .font(BarlowFont.text(14)).foregroundStyle(Palette.ink)
        }
        .padding(.vertical, 14).padding(.horizontal, 16)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(Palette.accentWash, in: RoundedRectangle(cornerRadius: 18, style: .continuous))
    }

    private func loadMore() async {
        guard let page = nextPage, !loading else { return }
        loading = true
        loadError = nil
        defer { loading = false }
        do {
            let got = try await sync.routes(provider, page: page)
            let have = Set(routes.map(\.id))
            routes += got.routes.filter { !have.contains($0.id) }
            nextPage = got.nextPage
        } catch {
            loadError = error.localizedDescription
        }
    }

    private func pick(_ r: CloudRoute) {
        importing = r.id
        pickError = nil
        Task {
            defer { importing = nil }
            do {
                let data = try await sync.routeGPX(provider, route: r)
                switch GPXImporter.parse(data, fallbackName: r.name) {
                case .success(var route):
                    // The GPX carries neither; the summary card shows both.
                    route.source = provider.title
                    route.climbMeters = r.elevationGainMeters
                    onPick(route)
                case .failure(.noPoints): pickError = "That route has no points in it."
                case .failure(.notGPX): pickError = "That route didn't come back as GPX."
                }
            } catch {
                pickError = error.localizedDescription
            }
        }
    }
}

/// 44pt round surface button (back / close) of the import sheet's headers.
private struct RoundButton: View {
    let systemName: String
    let tint: Color
    let label: String
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            Image(systemName: systemName).font(.system(size: 15, weight: .semibold))
                .foregroundStyle(tint)
                .frame(width: 44, height: 44)
                .background(Palette.surface, in: Circle())
                .overlay(Circle().strokeBorder(Palette.hairline, lineWidth: 1))
        }
        .accessibilityLabel(label)
    }
}

/// An alert line, the message, and a retry when there is one.
private struct ErrorCard: View {
    let title: String
    let message: String
    var retry: (() -> Void)? = nil

    var body: some View {
        Card {
            VStack(alignment: .leading, spacing: 12) {
                HStack(alignment: .top, spacing: 12) {
                    Image(systemName: "exclamationmark.circle").font(.system(size: 20, weight: .semibold))
                        .foregroundStyle(Palette.accentDark)
                    VStack(alignment: .leading, spacing: 2) {
                        Text(title).font(BarlowFont.text(15, .semibold)).foregroundStyle(Palette.ink)
                        Text(message.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
                             ? "Check your connection, then try again." : message)
                            .font(BarlowFont.text(14)).foregroundStyle(Palette.muted)
                    }
                }
                if let retry { SecondaryButton(title: "Try again", action: retry) }
            }
        }
    }
}

/// The list's 58pt tile: the route's outline when the provider sent one
/// (Strava's summary polyline), else a plain route glyph (RideWithGPS).
private struct RouteThumb: View {
    let shape: [LatLon]?

    var body: some View {
        RoundedRectangle(cornerRadius: 12, style: .continuous).fill(Palette.paper)
            .overlay(RoundedRectangle(cornerRadius: 12, style: .continuous)
                .strokeBorder(Palette.hairline, lineWidth: 1))
            .frame(width: 58, height: 58)
            .overlay {
                if let shape {
                    RouteOutline(points: shape)
                        .stroke(Palette.accent, style: StrokeStyle(lineWidth: 2.4, lineCap: .round, lineJoin: .round))
                        .frame(width: 44, height: 44)
                } else {
                    Image(systemName: "point.topleft.down.to.point.bottomright.curvepath")
                        .font(.system(size: 20, weight: .semibold)).foregroundStyle(Palette.muted)
                }
            }
    }
}

/// Lat/lon fitted into the rect, centred, aspect kept. Longitude is scaled by
/// cos(latitude) so a route doesn't widen the further it is from the equator.
private struct RouteOutline: Shape {
    let points: [LatLon]

    func path(in rect: CGRect) -> Path {
        guard let first = points.first else { return Path() }
        let k = cos(first.lat * .pi / 180)
        let xs = points.map { $0.lon * k }, ys = points.map { -$0.lat }
        let minX = xs.min()!, minY = ys.min()!
        let w = max(xs.max()! - minX, 1e-9), h = max(ys.max()! - minY, 1e-9)
        let scale = min(rect.width / w, rect.height / h)
        let ox = rect.minX + (rect.width - w * scale) / 2, oy = rect.minY + (rect.height - h * scale) / 2
        var path = Path()
        path.addLines(zip(xs, ys).map { CGPoint(x: ox + ($0 - minX) * scale, y: oy + ($1 - minY) * scale) })
        return path
    }
}
