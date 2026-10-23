import SwiftUI
import UIKit

@main
struct BikeGPSCompanionApp: App {
    @StateObject private var ble = BLEManager()
    @StateObject private var appState = AppState()
    @Environment(\.scenePhase) private var scenePhase

    init() {
        SyncAccounts.configureAppCheck()
        // Up before any transfer: it ends Live Activities a killed run left
        // behind, and watches for the app leaving the foreground.
        TransferCenter.shared.runDemoIfRequested()
    }

    var body: some Scene {
        WindowGroup {
            RootView()
                .environmentObject(ble)
                .environmentObject(appState)
                .tint(Palette.ink)
                // The app is paper-and-ink by design (see Theme.swift): every
                // Palette colour is a fixed light value. Left to follow a dark
                // system appearance, the stock controls (segmented pickers,
                // steppers, toggles, List/Form rows, sheets) went dark while the
                // paper stayed cream — white-on-cream segments, invisible
                // stepper glyphs, a black editor sheet. UIUserInterfaceStyle=Light
                // in Info.plist pins every window and sheet; this keeps the
                // SwiftUI environment in step with it.
                .preferredColorScheme(.light)
                // Strava / RideWithGPS sign-in returning through the URL scheme
                // (normally the auth session catches it; this is the fallback).
                .onOpenURL { url in Task { await SyncAccounts.shared.handle(callback: url) } }
                // Fixed pairing: the device turned this phone away. Both causes
                // need the rider's hands — on the device (unpair) and/or in iOS
                // Settings (the old keys, which no app can delete) — so the
                // only honest move is to say exactly what to tap.
                .alert(ble.pairingIssue.map(PairingCopy.title) ?? "",
                       isPresented: Binding(get: { ble.pairingIssue != nil },
                                            set: { if !$0 { ble.pairingIssue = nil } }),
                       presenting: ble.pairingIssue) { issue in
                    if issue == .notRecognised && ble.pairedDeviceId != nil {
                        Button("Forget Device in App", role: .destructive) { ble.forgetDevice() }
                    }
                    Button("OK", role: .cancel) {}
                } message: { issue in
                    Text(PairingCopy.message(issue))
                }
                // The dashboard was edited in the app while disconnected AND
                // changed on the device meanwhile: never overwrite either
                // silently. App-wide, since the reconnect can happen anywhere.
                .alert("Dashboard changed in both places",
                       isPresented: Binding(get: { ble.dashConflict != nil }, set: { _ in }),
                       presenting: ble.dashConflict) { _ in
                    Button("Keep phone version") { ble.resolveDashConflict(keepPhone: true) }
                    Button("Keep device version") { ble.resolveDashConflict(keepPhone: false) }
                } message: { c in
                    Text("You edited the dashboard layout in the app while your OpenTrailPaper was away (\(c.editedAt.formatted(date: .abbreviated, time: .shortened))), and the layout on the device has changed since. Which one should both keep?")
                }
        }
        // Coming back from the Settings app is the one way a permission changes
        // without either framework telling us, and it's exactly the path our own
        // "Open Settings" buttons send people down.
        .onChange(of: scenePhase) { _, phase in
            if phase == .active { ble.refreshPermissions() }
        }
    }
}

/// Opens this app's page in the Settings app — the only place a refused
/// permission can be granted again.
@MainActor
func openAppSettings() {
    guard let url = URL(string: UIApplication.openSettingsURLString),
          UIApplication.shared.canOpenURL(url) else { return }
    UIApplication.shared.open(url)
}

// Lightweight app-wide UI state. Owns whether the first-run tutorial is
// showing, so both first launch and the Settings "Show tutorial" button can
// drive the same full-screen cover.
final class AppState: ObservableObject {
    @Published var showTutorial: Bool = RootView.shouldOnboard
}

struct RootView: View {
    @EnvironmentObject var ble: BLEManager
    @EnvironmentObject var appState: AppState
    @State private var tab = RootView.initialTab

    init() { RootView.configureAppearance() }

    var body: some View {
        // The system tab bar can't be squared, inverted or ruled, so it is
        // hidden and drawn here instead (EinkTabBar). TabView still owns the
        // tabs, so each keeps its navigation and scroll state when switched.
        TabView(selection: $tab) {
            RideView().einkTabBarInset().tag(0).toolbar(.hidden, for: .tabBar)
            RouteView().einkTabBarInset().tag(1).toolbar(.hidden, for: .tabBar)
            RidesView().einkTabBarInset().tag(2).toolbar(.hidden, for: .tabBar)
            MeshView().einkTabBarInset().tag(3).toolbar(.hidden, for: .tabBar)
            SettingsView().einkTabBarInset().tag(4).toolbar(.hidden, for: .tabBar)
        }
        // Drawn over the TabView; each tab insets its own content by the
        // bar's height (`einkTabBarInset`), since a TabView does not pass a
        // safe-area inset of its own on to its pages.
        .overlay(alignment: .bottom) {
            EinkTabBar(selection: $tab, badges: [3: ble.meshState.unread])
        }
        .toggleStyle(EinkToggleStyle())
        .fullScreenCover(isPresented: $appState.showTutorial) {
            OnboardingView {
                UserDefaults.standard.set(true, forKey: BLEManager.onboardedKey)
                appState.showTutorial = false
            }
            .environmentObject(ble)
        }
    }

    // Square black-and-white UIKit chrome (nav bars, segmented pickers…).
    static func configureAppearance() {
        EinkAppearance.apply()
        // Kept for anything that still shows a system tab bar.
        let tab = UITabBarAppearance()
        tab.configureWithOpaqueBackground()
        tab.backgroundColor = .white
        tab.shadowColor = .black
        UITabBar.appearance().standardAppearance = tab
        UITabBar.appearance().scrollEdgeAppearance = tab
    }

    // Lets UI screenshots open a specific tab via a launch argument.
    static var initialTab: Int {
        let a = ProcessInfo.processInfo.arguments
        if a.contains("-tab-route") { return 1 }
        if a.contains("-tab-rides") { return 2 }
        if a.contains("-tab-mesh") || a.contains(where: { $0.hasPrefix("-demo-mesh") }) {
            return 3
        }
        if a.contains("-tab-settings") || a.contains(where: { $0.hasPrefix("-demo-maps") }) { return 4 }
        return 0
    }

    // Whether to show the first-run tutorial. Suppressed under the screenshot
    // demo flags (so gallery captures aren't covered by it); `-onboarding`
    // forces it on for capturing the tutorial itself.
    static var shouldOnboard: Bool {
        let a = ProcessInfo.processInfo.arguments
        if a.contains(where: { $0.hasPrefix("-onboarding") }) { return true }
        let demoFlags = ["-tab-route", "-tab-rides", "-tab-mesh", "-tab-settings",
                         "-demo-route", "-demo-rides", "-demo-update", "-demo-dash", "-demo-live",
                         "-demo-mesh", "-demo-mesh-settings", "-demo-mesh-map",
                         "-demo-mesh-nodes", "-demo-mesh-channels",
                         "-demo-mesh-share", "-demo-mesh-off", "-demo-transfer",
                         "-demo-maps", "-demo-maps-coverage", "-demo-maps-layers"]
        if demoFlags.contains(where: a.contains) { return false }
        return !UserDefaults.standard.bool(forKey: BLEManager.onboardedKey)
    }
}

/// The app's tab bar, drawn like the device's own chrome: a 3pt rule on top,
/// square cells, CAPS labels, and the selected tab as a solid black cell.
struct EinkTabBar: View {
    static let height: CGFloat = Rule.title + 56
    @Binding var selection: Int
    var badges: [Int: Int] = [:]

    private static let items: [(title: String, icon: String)] = [
        ("Ride", "speedometer"),
        ("Route", "map"),
        ("Rides", "list.bullet.rectangle"),
        ("Mesh", "dot.radiowaves.left.and.right"),
        ("Settings", "slider.horizontal.3"),
    ]

    var body: some View {
        // The bar runs down under the home indicator to the glass, and its
        // icons and labels sit centred in all of it — not in the top 56pt
        // with an empty band below.
        GeometryReader { geo in
            VStack(spacing: 0) {
                Rectangle().fill(Palette.ink).frame(height: Rule.title)
                HStack(spacing: 0) {
                    ForEach(Array(Self.items.enumerated()), id: \.offset) { i, item in
                        cell(i, item.title, item.icon)
                    }
                }
                .frame(height: 56 + geo.safeAreaInsets.bottom)
            }
            .background(Palette.paper)
            .ignoresSafeArea(edges: .bottom)
        }
        .frame(height: Self.height)
    }

    private func cell(_ i: Int, _ title: String, _ icon: String) -> some View {
        let on = selection == i
        let badge = badges[i] ?? 0
        return Button { selection = i } label: {
            VStack(spacing: 4) {
                Image(systemName: icon)
                    .font(.system(size: 19, weight: .bold))
                    .frame(height: 22)
                    .overlay(alignment: .topTrailing) {
                        if badge > 0 {
                            Text(badge > 99 ? "99+" : "\(badge)")
                                .font(BarlowFont.text(10, .bold))
                                .padding(.horizontal, 4).padding(.vertical, 1)
                                .foregroundStyle(on ? Palette.ink : Palette.paper)
                                .background(on ? Palette.paper : Palette.ink)
                                .overlay(Rectangle().strokeBorder(on ? Palette.ink : Palette.paper, lineWidth: 1))
                                .offset(x: 12, y: -6)
                        }
                    }
                Text(title.uppercased())
                    .font(.custom("Barlow-Bold", fixedSize: 11)).tracking(1.5)
                    .lineLimit(1).minimumScaleFactor(0.7)
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .foregroundStyle(on ? Palette.paper : Palette.ink)
            .background(on ? Palette.ink : Palette.paper)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityLabel(badge > 0 ? "\(title), \(badge) unread" : title)
        .accessibilityAddTraits(on ? [.isSelected, .isButton] : .isButton)
        .accessibilityHint("Tab \(i + 1) of \(Self.items.count)")
    }
}

extension View {
    /// Keeps a tab's content clear of the overlaid EinkTabBar.
    func einkTabBarInset() -> some View {
        padding(.bottom, EinkTabBar.height)
    }
}
