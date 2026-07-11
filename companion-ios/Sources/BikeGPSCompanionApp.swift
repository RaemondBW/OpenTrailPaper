import SwiftUI

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

    // Lets UI screenshots open a specific tab via a launch argument.
    static var initialTab: Int {
        let a = ProcessInfo.processInfo.arguments
        if a.contains("-tab-route") { return 1 }
        if a.contains("-tab-rides") { return 2 }
        if a.contains("-tab-settings") { return 3 }
        return 0
    }
}
