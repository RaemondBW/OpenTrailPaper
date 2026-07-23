import SwiftUI
import UIKit

@main
struct BikeGPSCompanionApp: App {
    @StateObject private var ble = BLEManager()
    @StateObject private var appState = AppState()

    var body: some Scene {
        WindowGroup {
            RootView()
                .environmentObject(ble)
                .environmentObject(appState)
                .tint(Palette.accent)
        }
    }
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
        TabView(selection: $tab) {
            RideView()
                .tabItem { Label("Ride", systemImage: "speedometer") }.tag(0)
            RouteView()
                .tabItem { Label("Route", systemImage: "map") }.tag(1)
            RidesView()
                .tabItem { Label("Rides", systemImage: "list.bullet.rectangle") }.tag(2)
            SettingsView()
                .tabItem { Label("Settings", systemImage: "slider.horizontal.3") }.tag(3)
        }
        .fullScreenCover(isPresented: $appState.showTutorial) {
            OnboardingView {
                UserDefaults.standard.set(true, forKey: BLEManager.onboardedKey)
                appState.showTutorial = false
            }
            .environmentObject(ble)
        }
    }

    // Cream tab bar with a vermilion selected state and Barlow labels, matching
    // the mockup. UIKit appearance because SwiftUI's TabView renders a UITabBar.
    static func configureAppearance() {
        let paper = UIColor(Palette.paper)
        let accent = UIColor(Palette.accent)
        let muted = UIColor(Palette.muted)

        let tab = UITabBarAppearance()
        tab.configureWithOpaqueBackground()
        tab.backgroundColor = paper
        tab.shadowColor = UIColor(Palette.hairline)
        let labelFont = UIFont(name: "BarlowCondensed-SemiBold", size: 11) ?? .systemFont(ofSize: 11)
        for item in [tab.stackedLayoutAppearance, tab.inlineLayoutAppearance, tab.compactInlineLayoutAppearance] {
            item.normal.iconColor = muted
            item.selected.iconColor = accent
            item.normal.titleTextAttributes = [.foregroundColor: muted, .font: labelFont]
            item.selected.titleTextAttributes = [.foregroundColor: accent, .font: labelFont]
        }
        UITabBar.appearance().standardAppearance = tab
        UITabBar.appearance().scrollEdgeAppearance = tab

        let nav = UINavigationBarAppearance()
        nav.configureWithOpaqueBackground()
        nav.backgroundColor = paper
        nav.shadowColor = .clear
        UINavigationBar.appearance().standardAppearance = nav
        UINavigationBar.appearance().scrollEdgeAppearance = nav
    }

    // Lets UI screenshots open a specific tab via a launch argument.
    static var initialTab: Int {
        let a = ProcessInfo.processInfo.arguments
        if a.contains("-tab-route") { return 1 }
        if a.contains("-tab-rides") { return 2 }
        if a.contains("-tab-settings") { return 3 }
        return 0
    }

    // Whether to show the first-run tutorial. Suppressed under the screenshot
    // demo flags (so gallery captures aren't covered by it); `-onboarding`
    // forces it on for capturing the tutorial itself.
    static var shouldOnboard: Bool {
        let a = ProcessInfo.processInfo.arguments
        if a.contains(where: { $0.hasPrefix("-onboarding") }) { return true }
        let demoFlags = ["-tab-route", "-tab-rides", "-tab-settings",
                         "-demo-route", "-demo-rides", "-demo-update"]
        if demoFlags.contains(where: a.contains) { return false }
        return !UserDefaults.standard.bool(forKey: BLEManager.onboardedKey)
    }
}
