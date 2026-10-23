import Combine
import CoreLocation
import SwiftUI

/// A screen's view of BLEManager that only republishes when what the screen
/// shows changes.
///
/// BLEManager is one ObservableObject that publishes many times a second
/// while a device is connected — status every second, sensors, mesh, the
/// phone's location stream, transfer progress. Observed directly, every one
/// of those re-evaluated whole tab screens (the visible tab and the one kept
/// alive beside it), including mid-scroll, which is what made scrolling
/// stutter. A screen holds one of these instead and redraws when its
/// `Snapshot` changes.
///
/// Checked on the main queue after each publish (objectWillChange fires
/// before the new value lands); that queue also runs while a scroll tracks,
/// so a change the screen does care about still shows at once.
@MainActor
final class BLEScope<Snapshot: Equatable>: ObservableObject {
    let ble: BLEManager
    private let take: @MainActor (BLEManager) -> Snapshot
    private var last: Snapshot
    private var sub: AnyCancellable?
    private var scheduled = false

    init(_ ble: BLEManager, _ take: @escaping @MainActor (BLEManager) -> Snapshot) {
        self.ble = ble
        self.take = take
        last = take(ble)
        sub = ble.objectWillChange.sink { [weak self] _ in self?.schedule() }
    }

    private func schedule() {
        guard !scheduled else { return }
        scheduled = true
        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            self.scheduled = false
            let now = self.take(self.ble)
            if now != self.last {
                if PerfMonitor.enabled {
                    let a = Mirror(reflecting: self.last).children, b = Mirror(reflecting: now).children
                    let changed = zip(a, b).filter { String(describing: $0.value) != String(describing: $1.value) }
                        .compactMap(\.0.label)
                    PerfMonitor.note("\(Snapshot.self) changed: \(changed.joined(separator: ","))")
                }
                self.last = now
                self.objectWillChange.send()
            }
        }
    }
}

/// The tab screens, each handed the manager without observing it so their
/// redraws come from their BLEScope alone (`.equatable()` stops the root's
/// re-evaluations at the tab).
struct RidesTab: View {
    @EnvironmentObject private var ble: BLEManager
    var body: some View { RidesView(ble: ble).equatable() }
}
struct RouteTab: View {
    @EnvironmentObject private var ble: BLEManager
    var body: some View { RouteView(ble: ble).equatable() }
}
struct MeshTab: View {
    @EnvironmentObject private var ble: BLEManager
    var body: some View { MeshView(ble: ble).equatable() }
}

struct RidesSnapshot: Equatable {
    var rides: [RideFile], loadingRides: Bool, checked: Bool
    var downloadingName: String?, downloadProgress: Double, queued: [String]
    var state: BLEManager.ConnState, recording: Bool, downloadedFileURL: URL?
    @MainActor init(_ b: BLEManager) {
        rides = b.rides; loadingRides = b.loadingRides; checked = b.ridesCheckedThisLink
        downloadingName = b.downloadingName; downloadProgress = b.downloadProgress
        queued = b.queuedDownloads; state = b.state; recording = b.status.recording
        downloadedFileURL = b.downloadedFileURL
    }
}

struct RouteSnapshot: Equatable {
    var deviceRoutes: [String], loadingRoutes: Bool, deviceTileIds: Set<String>
    var lastUploadProgress: Double?, routeSent: Bool, routeReceived: Bool
    var locationPermission: PermissionState, state: BLEManager.ConnState
    var sentRoute: Int
    @MainActor init(_ b: BLEManager) {
        deviceRoutes = b.deviceRoutes; loadingRoutes = b.loadingRoutes
        deviceTileIds = b.deviceTileIds; lastUploadProgress = b.lastUploadProgress
        routeSent = b.routeSent; routeReceived = b.routeReceived
        locationPermission = b.locationPermission; state = b.state
        sentRoute = b.sentRouteCoords.count
    }
}

struct MeshSnapshot: Equatable {
    var meshState: MeshState, messages: [MeshMessage], nodes: [MeshNode]
    var stats: MeshStats, presets: [MeshPreset], regions: [MeshRegion]
    var channels: [MeshChannel], sendRejected: Bool, demo: Bool
    var locationAuthorized: Bool, state: BLEManager.ConnState
    @MainActor init(_ b: BLEManager) {
        meshState = b.meshState; messages = b.meshMessages; nodes = b.meshNodes
        stats = b.meshStats; presets = b.meshPresets; regions = b.meshRegions
        channels = b.meshChannels; sendRejected = b.meshSendRejected; demo = b.demoMesh
        locationAuthorized = b.locationAuthorized; state = b.state
    }
}
