import SwiftUI
import UIKit

@main
struct BikeGPSCompanionApp: App {
    @StateObject private var ble = BLEManager()

    var body: some Scene {
        WindowGroup {
            RootView()
                .environmentObject(ble)
                .tint(Palette.accent)
        }
    }
}

struct RootView: View {
    @EnvironmentObject var ble: BLEManager
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
}
