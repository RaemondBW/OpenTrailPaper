import Foundation
import CoreBluetooth
import CoreLocation
import Combine
import CryptoKit
import UIKit

// GATT UUIDs — must match src/ble_server.cpp on the device.
enum BikeUUID {
    static let service  = CBUUID(string: "B1C50000-9E0F-4B7A-9C6D-1F2E3A4B5C6D")
    static let settings = CBUUID(string: "B1C50001-9E0F-4B7A-9C6D-1F2E3A4B5C6D")
    static let status   = CBUUID(string: "B1C50002-9E0F-4B7A-9C6D-1F2E3A4B5C6D")
    static let route    = CBUUID(string: "B1C50003-9E0F-4B7A-9C6D-1F2E3A4B5C6D")
    static let rides    = CBUUID(string: "B1C50004-9E0F-4B7A-9C6D-1F2E3A4B5C6D")
    static let ota      = CBUUID(string: "B1C50005-9E0F-4B7A-9C6D-1F2E3A4B5C6D")
    static let sensors  = CBUUID(string: "B1C50006-9E0F-4B7A-9C6D-1F2E3A4B5C6D")
    static let map      = CBUUID(string: "B1C50007-9E0F-4B7A-9C6D-1F2E3A4B5C6D")
    static let agnss    = CBUUID(string: "B1C50008-9E0F-4B7A-9C6D-1F2E3A4B5C6D")
    static let dash     = CBUUID(string: "B1C50009-9E0F-4B7A-9C6D-1F2E3A4B5C6D")
    static let mesh     = CBUUID(string: "B1C5000A-9E0F-4B7A-9C6D-1F2E3A4B5C6D")
    static let media    = CBUUID(string: "B1C5000B-9E0F-4B7A-9C6D-1F2E3A4B5C6D")
    static let workout  = CBUUID(string: "B1C5000C-9E0F-4B7A-9C6D-1F2E3A4B5C6D")
}

// A cycling sensor known to the head unit (HR / power / cadence).
struct BikeSensor: Identifiable, Equatable {
    let addr: String
    var name: String
    var kindsMask: UInt8
    var connected: Bool
    var paired: Bool
    var rssi: Int
    var id: String { addr }

    // Bits match the firmware's ble_sensors::Kind. Cadence (4) and Speed (16)
    // share one BLE service: a device the head unit has not connected to yet
    // carries both ("speed & cadence" — it can't tell which until it reads the
    // sensor's feature list); once known, a speed sensor shows as Speed, a
    // cadence sensor as Cadence, and a combo as both.
    var kindsText: String {
        var parts: [String] = []
        if kindsMask & 1 != 0 { parts.append("Heart rate") }
        if kindsMask & 2 != 0 { parts.append("Power") }
        if kindsMask & 4 != 0 && kindsMask & 16 != 0 {
            parts.append("Speed & cadence")
        } else {
            if kindsMask & 4 != 0 { parts.append("Cadence") }
            if kindsMask & 16 != 0 { parts.append("Speed") }
        }
        if kindsMask & 8 != 0 { parts.append("Radar") }
        return parts.isEmpty ? "Sensor" : parts.joined(separator: " + ")
    }
}

// A vector map already stored on the device (its coverage bounds).
struct DeviceMap: Identifiable, Equatable {
    let id = UUID()
    let south, west, north, east: Double
    let builtin: Bool
    var corners: [CLLocationCoordinate2D] {
        [.init(latitude: south, longitude: west), .init(latitude: south, longitude: east),
         .init(latitude: north, longitude: east), .init(latitude: north, longitude: west)]
    }
}

// A recorded ride file on the device.
struct RideFile: Identifiable, Hashable, Codable {
    let name: String
    let size: Int
    var id: String { name }
}

/// The device's ride list as last seen, saved per device (keyed like the
/// settings cache) so the Rides tab can show it at launch and while
/// disconnected, before the device is asked again.
struct RideListingStore {
    private static func key(_ id: String) -> String { "rideListing.\(id)" }

    let defaults: UserDefaults

    func load(_ id: String) -> [RideFile] {
        guard let d = defaults.data(forKey: Self.key(id)),
              let list = try? JSONDecoder().decode([RideFile].self, from: d) else { return [] }
        return list
    }

    func save(_ list: [RideFile], for id: String) {
        if let d = try? JSONEncoder().encode(list) { defaults.set(d, forKey: Self.key(id)) }
    }
}

// A per-day diagnostics log file on the device (/logs/YYYYMMDD.log).
struct LogFile: Identifiable, Hashable {
    let name: String
    let size: Int
    var id: String { name }
}

// Live status pushed by the device once a second.
struct DeviceStatus {
    var gpsFix = false
    var recording = false
    var hasRoute = false
    var battery = 0
    var sats = 0
    var heartRate: Int? = nil
    var power: Int? = nil
    var speedKmh = 0.0
    var remainingKm = 0.0
}

// MARK: - Meshtastic mesh

/// One message in the mesh conversation, sent or received.
///
/// `id` is the Meshtastic packet id, which is what makes this identifiable at
/// all: the device streams its whole history whenever something changes, so the
/// list is rebuilt from scratch each time and rows have to survive that.
struct MeshMessage: Identifiable, Equatable {
    enum Status: UInt8 { case pending = 0, sent = 1, acked = 2, failed = 3 }

    let id: UInt32
    let from: UInt32
    let to: UInt32
    let outgoing: Bool
    /// Which channel it belongs to. 0 is the primary (public) channel; anything
    /// else is a private one, and this is what keeps those threads apart.
    let channel: UInt8
    let status: Status
    let text: String
    /// When it happened, worked out from the age the device reported. The device
    /// often has no wall clock (no GPS fix yet, no RTC set), so the phone's own
    /// clock minus that age is the only date either end can agree on.
    let date: Date
    let rssi: Int
    let snr: Int
    let hops: Int

    var isBroadcast: Bool { to == MeshState.broadcastAddr }
}

/// A neighbour the device has heard on the mesh.
struct MeshNode: Identifiable, Equatable {
    let num: UInt32
    let shortName: String
    let longName: String
    let lastHeard: Date
    let rssi: Int
    let snr: Int
    let hops: Int
    /// Where it last said it was. nil is the common case and means "has not sent a
    /// position", which is different from being at 0,0 — nodes broadcast position
    /// on their own schedule, and many never do.
    let position: MeshNodePosition?

    var id: UInt32 { num }
    /// "!a4c1380c" — how Meshtastic writes a node number everywhere.
    var nodeId: String { "!" + String(format: "%08x", num) }
    var displayName: String {
        !longName.isEmpty ? longName : !shortName.isEmpty ? shortName : nodeId
    }
    /// The node's short name, or the last four hex digits of its id until its
    /// NodeInfo arrives — which is also what Meshtastic itself defaults a short
    /// name to, so the fallback usually matches what the owner would see.
    var shortLabel: String {
        shortName.isEmpty ? String(format: "%04x", num & 0xFFFF) : shortName
    }
    /// "direct", "2 hops", or nil when the sender's firmware does not say.
    var hopsText: String? { meshHopsText(hops) }
    /// What a map pin carries: "ALEX · 2 hops".
    var mapLabel: String { hopsText.map { "\(shortLabel) · \($0)" } ?? shortLabel }
}

/// Meshtastic carries at most 7 hops; the device reports anything above that
/// (0xFF) for a packet whose sender's firmware predates hop_start, where the
/// distance is unknown rather than zero.
func meshHopsKnown(_ hops: Int) -> Bool { (0...7).contains(hops) }

func meshHopsText(_ hops: Int) -> String? {
    guard meshHopsKnown(hops) else { return nil }
    switch hops {
    case 0: return "direct"
    case 1: return "1 hop"
    default: return "\(hops) hops"
    }
}

/// The device's mesh configuration, as it reports it.
struct MeshState: Equatable {
    static let broadcastAddr: UInt32 = 0xFFFF_FFFF

    var enabled = false
    var radioOk = false
    /// No explicit channel name is set, so `channel` is the modem preset's name —
    /// what a stock Meshtastic node does, and what keeps the two in step.
    var channelFollowsPreset = true
    var nodeNum: UInt32 = 0
    var frequencyHz: UInt32 = 0
    var channel = ""
    var channelKey: UInt8 = 1
    var longName = ""
    var shortName = ""
    var nodeCount = 0
    var unread = 0
    /// Index into `BLEManager.meshPresets`.
    var presetIndex: UInt8 = 0
    /// Radio configuration, sent by firmware that lets the phone choose it. All
    /// nil on older firmware, which compiled the region in — the sheet hides the
    /// controls rather than offering a change the device would ignore.
    var regionIndex: UInt8? = nil
    /// The power actually in use — never the "maximum" sentinel the setter takes.
    var txPowerDbm: Int8? = nil
    /// 0 = the slot comes from the channel name, as on a stock node.
    var slotOverride: UInt8? = nil
    var slotCount: UInt8? = nil
    /// 1-based, like Meshtastic's channel_num.
    var activeSlot: UInt8? = nil

    var nodeId: String { "!" + String(format: "%08x", nodeNum) }
    var frequencyMHz: Double { Double(frequencyHz) / 1_000_000 }
    /// Nothing has been heard from the device yet.
    var isUnknown: Bool { nodeNum == 0 }
    var hasRadioConfig: Bool { regionIndex != nil }
}

/// A regulatory region the device can run in: the band, and the power the law
/// there allows. Streamed from the device like the presets, for the same reason —
/// the firmware owns the table, and a copy here would drift. The numbers are
/// Meshtastic's, so a region here means the same frequencies as on a stock node.
struct MeshRegion: Identifiable, Equatable {
    let index: UInt8
    let name: String
    let startHz: UInt32
    let endHz: UInt32
    let spacingHz: UInt32
    let powerLimitDbm: Int8

    var id: UInt8 { index }
    /// "902–928 MHz". Sub-MHz edges (EU_868 is 869.4–869.65) keep their decimals.
    var band: String {
        func mhz(_ hz: UInt32) -> String {
            let v = Double(hz) / 1_000_000
            return v == v.rounded() ? String(format: "%.0f", v) : String(format: "%g", v)
        }
        return "\(mhz(startHz))–\(mhz(endHz)) MHz"
    }
    /// The SX1262 tops out at 22 dBm, so that is what binds in most regions.
    var maxTxDbm: Int8 { min(powerLimitDbm, 22) }
}

/// A position a node broadcast over the mesh.
struct MeshNodePosition: Equatable {
    let latitude: Double
    let longitude: Double
    let altitudeM: Int
    let satsInView: Int
    /// How much of the coordinate the sender chose to publish; 32 is full
    /// precision. Meshtastic lets a node blur its position on purpose, so a
    /// coarse fix must not be drawn as though it were exact.
    let precisionBits: Int
    let received: Date

    var coordinate: CLLocationCoordinate2D {
        CLLocationCoordinate2D(latitude: latitude, longitude: longitude)
    }
    var isImprecise: Bool { precisionBits < 32 }
    /// Rough radius the sender's blurring leaves, for describing the uncertainty.
    /// Each bit dropped from a coordinate doubles the box it could be in.
    var uncertaintyM: Double? {
        guard isImprecise else { return nil }
        // 2^(32 - bits) steps of ~360/2^32 degrees, taken at the equator.
        let steps = pow(2.0, Double(32 - precisionBits))
        return steps * (40_075_000 / pow(2.0, 32))
    }
    var shortText: String {
        String(format: "%.5f, %.5f", latitude, longitude)
    }
}

/// A modem preset the device supports: how fast it talks, as opposed to the
/// channel, which is where. Streamed from the device rather than hard-coded here,
/// because the firmware owns which modems exist (`mesh::kPresets`) and a copy of
/// that table on the phone is a copy that drifts.
struct MeshPreset: Identifiable, Equatable {
    let index: UInt8
    let name: String
    let sf: Int
    let bandwidthKhz: Double
    let codingRate: Int

    var id: UInt8 { index }
    var detail: String {
        String(format: "SF%d · %.0f kHz", sf, bandwidthKhz)
    }
}

/// Packet counters, for the diagnostics view.
struct MeshStats: Equatable {
    var rx = 0, rxDropped = 0, rxOtherChannel = 0, rxDuplicate = 0
    var tx = 0, txFailed = 0, acksRx = 0
}

/// How a system permission stands right now.
///
/// Kept as four cases rather than a Bool because each one calls for different
/// UI: `notDetermined` means we still owe the user the system prompt,
/// `denied` can only be undone in Settings, and `unavailable` (restricted by
/// parental controls or an MDM profile) can't be undone at all — sending
/// someone to Settings for that is a dead end.
enum PermissionState: Equatable {
    case notDetermined, granted, denied, unavailable

    var isGranted: Bool { self == .granted }
    /// Whether the Settings app can actually change this.
    var fixableInSettings: Bool { self == .denied }
}

@MainActor
final class BLEManager: NSObject, ObservableObject {
    enum ConnState: Equatable { case idle, scanning, connecting, connected, poweredOff }

    @Published var state: ConnState = .idle
    @Published var status = DeviceStatus()
    // What the Settings screen shows: the rider's pending edit, else the
    // device's last reported value (cached per device, so it survives the
    // link and a relaunch), else these display-only defaults — which are
    // never sent (see DeviceSettingsSync.swift).
    @Published var ftpWatts = 250
    @Published var tzMinutes = TimeZone.current.secondsFromGMT() / 60
    @Published var backlight = 2        // 0 off .. 3 bright (mirrors device)
    @Published var clock24h = true      // device status-bar clock format
    @Published var usbDrive = true      // expose device SD as a USB drive

    // MARK: offline settings
    private let settingsStore = DeviceSettingsStore(defaults: .standard)
    /// The device the cache below belongs to (see setCurrentDevice).
    private(set) var settingsDeviceID = DeviceSettingsStore.unpairedKey
    /// Last-known device values, the rider's unsynced edits, and the layout.
    @Published private(set) var settingsCache = DeviceSettingsCache()
    /// Anything edited in the app the device hasn't acknowledged yet.
    var hasPendingSync: Bool { settingsCache.hasPending }
    /// One-line note after a sync replaced a change made on the device.
    @Published var settingsSyncNote: String?
    /// The rider's offline layout, while it waits for the device.
    @Published private(set) var pendingDashConfig: DashConfig?
    /// Offline layout edit vs. a layout that also changed on the device.
    @Published var dashConflict: DashConflict?
    struct DashConflict: Identifiable {
        let id = UUID()
        let phone: DashConfig
        let device: DashConfig
        let editedAt: Date
    }
    // Per-link sync state: nothing is pushed until the device's own values
    // have been read on THIS connection.
    private var settingsSeenThisLink = false
    private var settingsWriteInFlight: [String: Int]?
    private var settingsWriteFailedThisLink = false
    private var dashSeenThisLink = false
    private var dashWritesOutstanding = 0
    private var dashWriteError = false
    private var dashWriteText: String?
    private var dashWriteFailedThisLink = false
    /// Screenshot demo: disconnected, with cached values and a pending edit.
    private let isDemoOfflineSettings =
        ProcessInfo.processInfo.arguments.contains("-demo-offline-settings")
    // Speed-sensor wheel circumference, mm. nil until the device reports one —
    // firmware older than the speed-sensor support doesn't, and then the app
    // neither shows the setting nor sends it.
    @Published var wheelCircMm: Int? = nil
    @Published var lastUploadProgress: Double? = nil   // 0...1 while sending
    @Published var routeSent = false                   // last route's writes were queued
    @Published var routeReceived = false               // device confirmed it got the route
    @Published var lastMessage: String? = nil

    // Permission state, surfaced so onboarding can reflect what's been granted.
    @Published var locationAuthorized = false
    @Published var bluetoothReady = false          // central powered on & allowed

    // The precise state of each permission, so the UI can tell apart the three
    // situations that need three different answers: not asked yet (we must ask),
    // refused (only Settings can undo it), and restricted (nothing the user can
    // do). `bluetoothPoweredOn` is deliberately separate — a switched-off radio
    // is not a refused permission and must not be reported as one.
    @Published var locationPermission: PermissionState = .notDetermined
    @Published var bluetoothPermission: PermissionState = .notDetermined
    @Published var bluetoothPoweredOn = false

    // Ride download
    /// The device's ride list as last seen — kept across disconnects and
    /// relaunches (saved per device, see RideListingStore) so the Rides tab
    /// never blanks while the device is re-checked. Replaced only when a new
    /// list completes.
    @Published var rides: [RideFile] = []
    @Published var loadingRides = false
    /// The ride list has completed (or been refused) on this link, so opening
    /// the Rides tab again doesn't re-ask the device.
    private(set) var ridesCheckedThisLink = false
    private let rideListingStore = RideListingStore(defaults: .standard)
    @Published var downloadingName: String? = nil
    @Published var downloadProgress: Double = 0
    @Published var downloadedFileURL: URL? = nil       // set when a ride is ready

    // Firmware / OTA
    // Explicit phases so the UI can show exactly what's happening.
    enum OTAPhase: Equatable { case idle, sending, saving, installing, verifying, done, failed }
    @Published var deviceFirmware: String = ""      // running version on the device
    @Published var otaInProgress = false
    @Published var otaPhase: OTAPhase = .idle
    @Published var otaProgress: Double = 0
    @Published var otaMessage: String? = nil
    private var otaWatchdog: Task<Void, Never>?
    @Published var logFileURL: URL? = nil           // device diagnostics log, ready to share
    @Published var deviceLogs: [LogFile] = []        // per-day log files on the device
    @Published var loadingLogs = false
    private var logsBuilding: [LogFile] = []
    // Device ride-list entries collect here and replace `rides` only when the
    // list completes, so a refresh doesn't blank the Rides tab mid-fetch.
    private var ridesBuilding: [RideFile] = []
    // Firmware now comes from GitHub Releases at runtime (FirmwareRelease), not
    // from a copy baked into the app. See that file for why the bundle went.

    // Saved routes on the device
    @Published var deviceRoutes: [String] = []
    @Published var loadingRoutes = false

    // Cycling sensors known to the head unit (managed from the app)
    @Published var sensors: [BikeSensor] = []
    @Published var scanningSensors = false
    private var sensorsChar: CBCharacteristic?
    private var sensorsBuilding: [BikeSensor] = []

    // Vector-map upload (phone -> device)
    @Published var mapUploading = false
    @Published var mapProgress: Double = 0
    @Published var mapMessage: String? = nil
    @Published var deviceMaps: [DeviceMap] = []     // coverage already on the device
    private var deviceMapsBuilding: [DeviceMap] = []
    private var mapChar: CBCharacteristic?
    private var mapData = Data()
    private var mapOffset = 0
    private var mapChunk = 180
    private var mapEndSent = false

    // H3 tile streaming (many small tiles sent one at a time over CHR_MAP).
    @Published var tilesUploading = false
    @Published var tilesTotal = 0
    @Published var tilesDone = 0
    @Published var tileMessage: String? = nil
    @Published var deviceTileIds: Set<String> = []   // H3 ids already on the SD
    private var tileIdsBuilding: [String] = []
    // Cycling layer (investigations/osm-pois-bike-routes.md). Firmware that has
    // it answers the 0x08 POI listing; older firmware never does, so this stays
    // false for the connection and nothing cycling-specific is sent to it.
    @Published var deviceSupportsPois = false

    // MARK: device map layers (map characteristic 0x09 get / 0x0A set,
    // device answers [0xE0][u16 mask]). Bits: 0 bike routes, 1 cycleways &
    // lanes, 2 water, 3 toilets, 4 repair stations, 5 bike shops (src/map_view.h
    // MAP_LAYER_*). Kept per device: the last mask seen, whether that device
    // answers at all, and an edit made while disconnected (sent on the next
    // connect; last write wins).
    /// The device's map-layer mask as this app last knew it (live when connected).
    @Published var mapLayers: UInt16 = 0x3F
    /// The device (or, disconnected, the last one) has firmware with map layers.
    @Published var mapLayersSupported = false
    private var mapLayersCache: [String: Int] =
        (UserDefaults.standard.dictionary(forKey: "mapLayers") as? [String: Int]) ?? [:]
    private var mapLayersPending: [String: Int] =
        (UserDefaults.standard.dictionary(forKey: "mapLayersPending") as? [String: Int]) ?? [:]
    private var mapLayersSeen: [String: Bool] =
        (UserDefaults.standard.dictionary(forKey: "mapLayersSupported") as? [String: Bool]) ?? [:]
    private var layersDeviceKey: String? {
        peripheral?.identifier.uuidString ?? UserDefaults.standard.string(forKey: "mapLayersLastDevice")
    }
    /// Show the cached state for the current (or last) device.
    private func loadMapLayersState() {
        guard let k = layersDeviceKey else { return }
        UserDefaults.standard.set(k, forKey: "mapLayersLastDevice")
        mapLayers = UInt16(truncatingIfNeeded: mapLayersPending[k] ?? mapLayersCache[k] ?? 0x3F)
        mapLayersSupported = mapLayersSeen[k] ?? false
    }
    private func saveMapLayersState() {
        UserDefaults.standard.set(mapLayersCache, forKey: "mapLayers")
        UserDefaults.standard.set(mapLayersPending, forKey: "mapLayersPending")
        UserDefaults.standard.set(mapLayersSeen, forKey: "mapLayersSupported")
    }
    /// Ask the device for its mask (sent once the map characteristic notifies).
    func requestMapLayers() {
        guard let c = mapChar, let p = peripheral else { return }
        p.writeValue(Data([0x09]), for: c, type: .withResponse)
    }
    /// Change the device's map layers: applied now when connected, otherwise
    /// remembered and sent on the next connect.
    func setMapLayers(_ mask: UInt16) {
        mapLayers = mask
        guard let k = layersDeviceKey else { return }
        if let c = mapChar, let p = peripheral, mapLayersSupported {
            p.writeValue(Data([0x0A, UInt8(mask & 0xFF), UInt8(mask >> 8)]), for: c, type: .withResponse)
            mapLayersCache[k] = Int(mask)
            mapLayersPending[k] = nil
        } else {
            mapLayersPending[k] = Int(mask)
        }
        saveMapLayersState()
    }
    private func handleMapLayersNotify(_ mask: UInt16) {
        guard let k = layersDeviceKey else { return }
        mapLayersSupported = true
        mapLayersSeen[k] = true
        if let pending = mapLayersPending[k], UInt16(truncatingIfNeeded: pending) != mask,
           let c = mapChar, let p = peripheral {
            // An edit made while disconnected: it wins over the device's value.
            let m = UInt16(truncatingIfNeeded: pending)
            p.writeValue(Data([0x0A, UInt8(m & 0xFF), UInt8(m >> 8)]), for: c, type: .withResponse)
            mapLayers = m
        } else {
            mapLayersPending[k] = nil
            mapLayers = mask
            mapLayersCache[k] = Int(mask)
        }
        saveMapLayersState()
    }
    @Published var devicePoiIds: Set<String> = []    // H3 ids with a .poi on the SD
    private var poiIdsBuilding: [String] = []
    /// When this phone last sent each tile's .poi, for the refresh policy.
    private var poiSentAt: [String: Double] =
        (UserDefaults.standard.dictionary(forKey: "poiSentAt") as? [String: Double]) ?? [:]
    /// Map tiles this phone sent WITH the bike-route way flags. A tile on the
    /// device but not in here predates them and is re-sent the next time its
    /// area is synced (only to firmware that draws them).
    private var flaggedTileIds: Set<String> =
        Set(UserDefaults.standard.stringArray(forKey: "flaggedTileIds") ?? [])
    /// When this phone last sent each map tile, for "update available".
    private var tileSentAt: [String: Double] =
        (UserDefaults.standard.dictionary(forKey: "tileSentAt") as? [String: Double]) ?? [:]
    /// A map tile this phone sent longer ago than this is offered as an update
    /// (the phone's own tile cache expires at the same age).
    static let tileRefreshAge: TimeInterval = 60 * 60 * 24 * 90   // 90 days
    /// The version of every tile and .poi this phone sent, per device
    /// (TileVersions.swift), compared with the CDN index for "update
    /// available" and to skip unchanged hexes.
    let tileVersions = DeviceTileVersions()
    /// The device the version records belong to: the connected one, or the
    /// last one (same key as the map-layer store).
    var deviceKey: String? { layersDeviceKey }
    // The device's own version records (DeviceTileList), from the last
    // versioned list. They cover hexes whoever sent them, so they come before
    // this phone's records; empty on firmware that keeps none. Kept across a
    // disconnect, like deviceTileIds, but not across a restart (the next list
    // brings them back).
    /// The device answered the tile list with versions on this connection: it
    /// keeps them, so tiles go to it with their record.
    private var deviceKeepsVersions = false
    @Published private var deviceTileRecords: [String: String] = [:]
    @Published private var devicePoiRecords: [String: String] = [:]
    private var tileRecordsBuilding: [String: String] = [:]
    private var poiRecordsBuilding: [String: String] = [:]
    private var tileListVersioned = false
    private var poiListVersioned = false
    /// `version`: its record (PrebuiltTiles.cdnRecord); nil = built on this
    /// phone, recorded as phoneRecord() when the device saves it.
    private struct TileJob { let id: String; let data: Data; let poi: Bool; let version: String? }
    private var tileQueue: [TileJob] = []
    private var currentTileId: String? = nil        // non-nil while sending a tile
    private var currentIsPoi = false                // ...and it is a .poi file
    private var currentVersion: String? = nil       // ...and its version record
    private var resendAll = false                   // this job is a Redownload
    // BLE side of the download timing (see DownloadStats): bytes actually
    // written to the device and when the job started.
    private var tileJobStarted = Date()
    private var tileJobBytes = 0
    private var tileJobFailed = false
    private var tilesMoreComing = false             // app still building tiles to enqueue

    private var central: CBCentralManager!
    private var peripheral: CBPeripheral?
    private var settingsChar: CBCharacteristic?
    private var dashChar: CBCharacteristic?
    /// The device's dashboard layout, as the text of /config/dashboard.cfg —
    /// live while connected, the cached copy from the last connection when
    /// not. nil only if this device's layout has never been read, which is how
    /// the editor knows to show "connect to edit" rather than an invented
    /// default that would overwrite the rider's real one.
    @Published var dashConfig: DashConfig? {
        didSet { updateAlbumArt() }
    }
    /// What the editor edits: the rider's unsynced layout, else the device's.
    var dashTarget: DashConfig? { pendingDashConfig ?? dashConfig }
    /// First data page, for thumbnails (RideView's dashboard card).
    var dashLayout: DashLayout? { dashTarget?.firstFields }
    /// Version we are flashing, so the success check compares against what
    /// was actually sent rather than a compile-time constant.
    private var otaTargetVersion = ""
    private var statusChar: CBCharacteristic?
    private var routeChar: CBCharacteristic?
    private var ridesChar: CBCharacteristic?
    private var otaChar: CBCharacteristic?

    // MARK: Meshtastic
    private var meshChar: CBCharacteristic?
    private var workoutChar: CBCharacteristic?

    // MARK: live state stream (SVC_LIVE; DeviceLive.swift)
    /// The Ride tab's live frame. Its own ObservableObject so a frame a second
    /// redraws the Ride tab and nothing else (see DeviceLive).
    let live = DeviceLive()
    private var liveRxChar: CBCharacteristic?
    private var liveTxChar: CBCharacteristic?
    /// Screens on show that want the stream (the Ride tab), counted.
    private var liveViewers = 0
    /// The app is in the foreground; a backgrounded app never streams.
    private var appActive = true
    private var liveKeepalive: Timer?
    private var liveObservers: [NSObjectProtocol] = []

    // MARK: album art (CHR_MEDIA)
    /// Finds covers for the device's MUSIC page; see AlbumArt.swift.
    let albumArt = AlbumArtFeeder()
    private var mediaChar: CBCharacteristic?
    /// Does the device take art the phone already dithered ([0x13], a third
    /// of the bytes)? Asked with [0x03] once media notifications are up;
    /// firmware that predates it never answers, so this stays false and art
    /// goes as 8-bit grayscale.
    private var mediaToneArt = false
    /// The device takes [0x04] remote commands and sends them on over AMS,
    /// so the app's MUSIC buttons reach whatever app is playing.
    @Published private(set) var mediaRemote = false
    /// A media notification arrived on this link: notifications are on even
    /// if the state callback never said so. iOS keeps the link up for AMS
    /// when the app is relaunched, and the callback has been seen missing.
    private var mediaNotifySeen = false
    /// [0x03] caps went out on this link.
    private var mediaCapsAsked = false
    /// Retries of a refused media-notification subscribe on this link.
    private var mediaSubscribeRetries = 0
    /// What each acknowledged media write was, in order: the art pump starts
    /// on its begin's ack, so its write-without-response chunks can't overtake
    /// the begin.
    private enum MediaAck { case other, artBegin(Int), artEnd(Int) }
    private var mediaAcks: [MediaAck] = []
    private var artGen = 0              // bumps per transfer; stale acks ignored
    private var artPayload = Data()
    private var artOffset = 0
    private var artChunk = 180
    private var artPumping = false

    // MARK: workout state (published for WorkoutsView)
    struct WorkoutStatus: Equatable {
        var loaded = false, running = false, paused = false, done = false
        var blockIndex = 0, blockCount = 0
        var elapsedSec: UInt32 = 0, totalSec: UInt32 = 0
        var targetW: UInt16 = 0, ftpW: UInt16 = 0
        var pauseEachBlock = false
        var name = ""
    }
    @Published var workoutStatus = WorkoutStatus()
    @Published var deviceWorkouts: [String] = []
    @Published var workoutMessage: String?
    private var workoutListBuild: [String] = []
    struct FetchedWorkout: Equatable { let name: String; let text: String }
    @Published var fetchedWorkout: FetchedWorkout?
    /// The loaded workout's file, for the Ride tab's WORKOUT page (its block
    /// profile and the block strips). From the phone's library when it is
    /// there, else pulled off the device; kept apart from `fetchedWorkout`,
    /// which opens the builder.
    @Published private(set) var loadedWorkoutFile: FetchedWorkout?
    private var workoutFetchForPage = false
    /// The loaded workout's title while its file waits on the /workouts list.
    private var workoutPageAwaitingList = ""
    /// A title whose file isn't on the device (or failed to read): not asked
    /// for again until a different workout loads.
    private var workoutPageGaveUp = "" {
        didSet { loadedWorkoutFileUnavailable = !workoutPageGaveUp.isEmpty }
    }
    /// The WORKOUT page couldn't get the loaded workout's file, so it shows
    /// the session without its block profile rather than "Reading…".
    @Published private(set) var loadedWorkoutFileUnavailable = false
    /// The route last sent from this phone, drawn on the Ride tab's MAP page
    /// while the device reports one active (it can't send its own back).
    @Published var sentRouteCoords: [CLLocationCoordinate2D] = []
    private var workoutFetchBuf = Data()
    private var workoutFetchName = ""
    /// Rebuilt from the device's stream, then published in one go at the end
    /// marker — publishing per message would redraw the chat mid-list.
    private var meshBuilding: [MeshMessage] = []
    private var meshNodesBuilding: [MeshNode] = []
    @Published var meshState = MeshState()
    @Published var meshMessages: [MeshMessage] = []
    @Published var meshNodes: [MeshNode] = []
    @Published var meshStats = MeshStats()
    @Published var meshPresets: [MeshPreset] = []
    private var meshPresetsBuilding: [MeshPreset] = []
    @Published var meshRegions: [MeshRegion] = []
    private var meshRegionsBuilding: [MeshRegion] = []
    @Published var meshChannels: [MeshChannel] = []
    private var meshChannelsBuilding: [MeshChannel] = []
    /// Set when the device refuses a message (radio off, or its outbox is full),
    /// so the compose field can say so instead of silently losing the text.
    @Published var meshSendRejected = false
    /// Screenshot / design-review mode: the mesh screens are showing seeded data
    /// and should behave as if a device were attached. Set only by `-demo-mesh`.
    @Published var demoMesh = false

    // One-shot location handed to the device on connect so its GPS warm-starts
    // near the phone instead of cold-searching the whole sky.
    private let locationManager = CLLocationManager()
    private var wantsAiding = false
    /// Whether the device has told us its state since this connection came up.
    /// The first status is special: see syncLocationStreamToDeviceFix().
    private var sawStatusSinceConnect = false
    // Consecutive connections that died before ever delivering a status.
    // Three in a row is the stale-pairing signature: iOS reconnects with keys
    // the device no longer holds, encryption fails, iOS drops the link — and
    // only the RIDER can fix it (Forget This Device), so at three we say so.
    private var barrenConnects = 0
    /// The link proved itself this connection: a protected (authenticated-
    /// only) characteristic answered. Until then the UI says connecting /
    /// pairing, never connected — the firmware gives an unauthenticated link
    /// nothing, so "connected" before this would be a lie.
    private var authenticatedSinceConnect = false
    /// Link up but not yet authenticated: the UI shows "Pairing…" (iOS may be
    /// showing its code dialog right now; the code is on the device's panel).
    @Published private(set) var linkSecuring = false
    private var connectedAt: Date? = nil
    /// A protected read/write/subscribe failed for lack of authentication or
    /// encryption on this connection: the pairing was cancelled, failed, or
    /// refused.
    private var authFailedSinceConnect = false
    /// Last paired flag each device advertised (nil = never seen advertising,
    /// e.g. reached through retrievePeripherals).
    private var advPaired: [UUID: Bool] = [:]

    // MARK: fixed pairing
    //
    // An OpenTrailPaper belongs to one phone (firmware ble_server.cpp): the
    // first to pair owns it until the rider unpairs on the device, and every
    // other phone is disconnected the moment it tries to pair. The app's half:
    // remember WHICH device is ours and only ever connect to that one, skip
    // devices that advertise they're paired to someone else, and when a device
    // turns us away, say why and how to fix it.

    /// Why the device won't have us — drives the one pairing alert.
    enum PairingIssue: Identifiable, Equatable {
        /// The only OpenTrailPaper around is paired with another phone.
        case pairedElsewhere
        /// Our own device keeps dropping us before any data: it was unpaired
        /// on the device (or re-paired to another phone) and iOS still holds
        /// the old keys, which only the rider can delete.
        case notRecognised
        /// The device is open for pairing but the pairing didn't complete
        /// (code dialog cancelled, wrong code, or timed out).
        case pairingNotFinished
        var id: Self { self }
    }
    @Published var pairingIssue: PairingIssue? = nil
    /// CBPeripheral.identifier of OUR device, set once it has delivered data
    /// over an admitted (bonded) link. Stable across the phone's address
    /// rotation — the device's address is fixed, and iOS keys this to it.
    @Published private(set) var pairedDeviceId: UUID? =
        UserDefaults.standard.string(forKey: BLEManager.pairedDeviceKey).flatMap(UUID.init)
    @Published private(set) var pairedDeviceName: String? =
        UserDefaults.standard.string(forKey: BLEManager.pairedDeviceNameKey)
    static let pairedDeviceKey = "pairedDeviceId"
    static let pairedDeviceNameKey = "pairedDeviceName"
    /// Devices that turned us away this session — never auto-retried.
    private var rejectedIds: Set<UUID> = []
    /// Devices that REFUSED this phone (fixed pairing, see refuse()): no
    /// automatic connect of any kind — restore, system link, saved id, scan —
    /// until the rider taps Connect (connectTapped). Each automatic attempt
    /// would cost the rider nothing visible with current firmware, but older
    /// firmware answered with a pairing request, which is the dialog loop.
    private var refusedIds: Set<UUID> = []
    /// Refused while it advertised "paired": the one case where the device
    /// flipping to "unpaired" (the rider unpaired it on the panel) is worth an
    /// automatic try — it then pairs with a code on the panel. Refused while
    /// already unpaired means iOS holds stale keys or the code was cancelled;
    /// retrying that would just repeat it, so only a tap does.
    private var refusedWhilePaired: Set<UUID> = []
    /// Paired-to-someone devices seen while looking for our first device. If
    /// nothing unpaired shows up we try ONE of them anyway: after a reinstall
    /// the app has forgotten the device but iOS still holds the bond, and the
    /// device would let us straight in.
    private var pairedElsewhereSeen: [UUID: CBPeripheral] = [:]
    private var fallbackTask: Task<Void, Never>?
    /// The current connection is that one-off attempt.
    private var onFallbackAttempt = false
    /// forgetDevice() is tearing the link down: don't reconnect to it.
    private var forgetting = false
    private var fixStableTask: Task<Void, Never>?   // debounce stopping the stream
    @Published var lastAidingSent: Date? = nil

    private var dlBuffer = Data()
    private var dlExpected = 0
    private var dlName = ""
    private var dlNextSeq: UInt16 = 0
    @Published private(set) var downloadingLog = false

    // Ride and log transfers share ONE device-side streamer and one receive
    // buffer here, so exactly one may be in flight. Asking for a second file
    // mid-transfer used to reset the buffer under the running stream: the tail
    // of file A landed in the buffer and was then written out under file B's
    // name, so B was saved corrupt (and cached, so it never re-downloaded) and
    // A was never saved at all. Requests are queued and run strictly serially.
    private enum Transfer: Equatable {
        case ride(String)
        case log(String)        // "" = today's rolling diag.log

        var isLog: Bool { if case .log = self { return true }; return false }
        var displayName: String {
            switch self {
            case .ride(let n): return n
            case .log(let n):  return n.isEmpty ? "diag" : n
            }
        }
        var transferId: String { "ble.download.\(isLog ? "log" : "ride").\(displayName)" }
    }
    private var dlQueue: [Transfer] = []
    private var dlActive: Transfer? = nil
    /// List commands share the device's one request slot with transfers, so they
    /// wait for the queue to drain rather than clobbering it.
    private var listRefreshPending = false
    private var logListPending = false
    private var dlWatchdog: Task<Void, Never>?
    private var dlLastActivity = Date()
    /// Names waiting their turn, so a row can show "Queued" instead of looking
    /// like the tap did nothing.
    @Published private(set) var queuedDownloads: [String] = []

    // OTA transfer state
    private var otaData = Data()
    private var otaOffset = 0
    private var otaChunk = 180
    private var otaCommitSent = false

    // Screenshot demo: pose as connected to an older-firmware device so the
    // update-pending UI shows, without touching real Bluetooth.
    private let isDemoUpdate = ProcessInfo.processInfo.arguments.contains("-demo-update")

    // Screenshot demo: hold the device's default layout so the dashboard editor
    // and its preview can be captured without a head unit attached. The editor
    // reads the layout FROM the device, so without this it can only show its
    // "connect first" empty state.
    private let isDemoDash = ProcessInfo.processInfo.arguments.contains("-demo-dash")
    // Screenshot demo: a connected device mid-ride, for the Ride tab's live
    // dashboard — two field pages and the map, with numbers in the tiles.
    private let isDemoLive = ProcessInfo.processInfo.arguments.contains("-demo-live")

    // Screenshot demo: a connected device holding part of a selected area,
    // firmware with the cycling layer, tiles that predate it. See demoMaps().
    let isDemoMaps = ProcessInfo.processInfo.arguments.contains { $0.hasPrefix("-demo-maps") }

    /// `-demo-maps`: pose the device as holding `onDevice`, of which `current`
    /// were sent by this phone with the bike-route data (the rest show as
    /// updates), with fresh POIs for `withPois`. Memory only — never saved.
    func demoMaps(onDevice: [String], current: [String], withPois: [String]) {
        guard isDemoMaps else { return }
        state = .connected
        deviceSupportsPois = true
        mapLayersSupported = true
        mapLayers = 0x3F & ~0x22   // cycleways and shops off, to show both states
        deviceTileIds = Set(onDevice)
        flaggedTileIds = Set(current)
        devicePoiIds = Set(withPois)
        let now = Date().timeIntervalSince1970
        for id in current { tileSentAt[id] = now }
        for id in withPois { poiSentAt[id] = now }
    }

    /// -demo-live: a connected device mid-ride, with every kind of page — a
    /// field page, a loaded workout (or, with -demo-live-picker, the picker),
    /// music with a cover, the stopwatch and the map with a route.
    private func seedLiveDemo() {
        state = .connected
        dashConfig = DashConfig(text: """
            power3s hero
            speed medium half
            hr medium half
            distance medium half
            ridetime medium half
            cadence medium half
            climb medium half
            page workout
            page music
            page stopwatch
            page map
            """)
        status = DeviceStatus(gpsFix: true, recording: true, hasRoute: true, battery: 76,
                              sats: 11, heartRate: 156, power: 238, speedKmh: 24.6,
                              remainingKm: 12.4)
        deviceWorkouts = ["over_unders_4x8.erg", "recovery_spin.mrc", "sweet_spot_3x10.erg",
                          "vo2_5x4.erg"]
        if !ProcessInfo.processInfo.arguments.contains("-demo-live-picker") {
            let name = "sweet_spot_3x10.erg"
            loadedWorkoutFile = FetchedWorkout(name: name, text: """
                [COURSE HEADER]
                MINUTES WATTS
                [END COURSE HEADER]
                [COURSE DATA]
                0 120
                10 160
                10 230
                20 230
                20 150
                25 150
                25 230
                35 230
                35 150
                40 150
                40 230
                50 230
                50 130
                60 130
                [END COURSE DATA]
                """)
            var w = WorkoutStatus()
            w.loaded = true; w.running = true; w.name = name
            w.blockIndex = 3; w.blockCount = 7
            w.elapsedSec = 28 * 60 + 12; w.totalSec = 60 * 60
            w.targetW = 230; w.ftpW = 250
            workoutStatus = w
        }
        sentRouteCoords = [
            .init(latitude: 37.7694, longitude: -122.4862), .init(latitude: 37.7712, longitude: -122.4700),
            .init(latitude: 37.7735, longitude: -122.4540), .init(latitude: 37.7760, longitude: -122.4380),
            .init(latitude: 37.7810, longitude: -122.4250), .init(latitude: 37.7905, longitude: -122.4200),
            .init(latitude: 37.8010, longitude: -122.4190), .init(latitude: 37.8080, longitude: -122.4177),
        ]
        // A cover: soft rings, so the dither has tones to work with.
        let side = AlbumArtFeeder.side
        var gray = [UInt8](repeating: 0, count: side * side)
        for y in 0..<side {
            for x in 0..<side {
                let dx = Double(x - side / 2), dy = Double(y - side / 2)
                let r = (dx * dx + dy * dy).squareRoot()
                gray[y * side + x] = UInt8(127 + 127 * cos(r / 14) * (1 - min(1, r / 230)))
            }
        }
        albumArt.showDemo(MediaTrack(title: "Turn Your Lights Down Low",
                                     artist: "Bob Marley & The Wailers", album: "Exodus"),
                          gray: gray)
        // The live stream's frame: the tiles the status characteristic
        // doesn't carry, and a running stopwatch on its third lap.
        var lf = LiveFrame()
        lf.gpsFix = true; lf.recording = true; lf.routeActive = true
        lf.hrLive = true; lf.powerLive = true; lf.cadenceLive = true; lf.timeValid = true
        lf.battery = 76; lf.sats = 11; lf.speedKmh = 24.6
        lf.power = 238; lf.power3s = 241; lf.heartRate = 156; lf.cadence = 88
        lf.distanceM = 31_420; lf.rideTimeS = 1 * 3600 + 24 * 60 + 37; lf.movingTimeS = 1 * 3600 + 18 * 60 + 5
        lf.climbM = 612; lf.gradePct = 4.8; lf.altitudeM = 287; lf.routeLeftKm = 12.4
        var sw = LiveFrame.Stopwatch()
        sw.running = true; sw.showPower = true; sw.showCadence = true
        sw.elapsedMs = 14 * 60_000 + 32_000; sw.laps = 2
        sw.lapMs = 3 * 60_000 + 11_000; sw.lastSplitMs = 5 * 60_000 + 47_300
        sw.lapAvgSpeedKmh = 26.1; sw.lapAvgPowerW = 252; sw.lapAvgCadence = 90
        lf.stopwatch = sw
        lf.view = LiveFrame.View(page: 0, pageCount: 5, screen: 0)
        live.showDemo(lf)
        // Tick like a connected head unit: a status notify every second, so
        // screens are exercised under the same republish load as on a ride.
        var t = 0.0
        demoTicker = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated {
                guard let self else { return }
                t += 1
                var s = self.status
                s.power = 238 + Int(30 * sin(t / 4))
                s.heartRate = 156 + Int(4 * sin(t / 9))
                s.speedKmh = 24.6 + 2 * sin(t / 6)
                self.status = s
                lf.power = s.power; lf.power3s = s.power.map { $0 + 3 }
                lf.heartRate = s.heartRate; lf.speedKmh = s.speedKmh
                lf.cadence = 88 + Int(3 * sin(t / 5))
                lf.distanceM += s.speedKmh / 3.6
                lf.rideTimeS += 1; lf.movingTimeS += 1
                lf.stopwatch?.elapsedMs += 1000; lf.stopwatch?.lapMs += 1000
                self.live.showDemo(lf)
            }
        }
    }
    private var demoTicker: Timer?

    override init() {
        super.init()
        loadSettingsCache()
        if isDemoOfflineSettings { seedOfflineSettingsDemo() }
        if isDemoUpdate { state = .connected; deviceFirmware = "v0.83" }
        if isDemoDash { state = .connected; dashConfig = .deviceDefault }
        if isDemoLive { seedLiveDemo() }
        locationManager.delegate = self
        locationManager.desiredAccuracy = kCLLocationAccuracyHundredMeters
        let a = locationManager.authorizationStatus
        locationAuthorized = (a == .authorizedWhenInUse || a == .authorizedAlways)
        locationPermission = Self.state(of: a)
        // Readable WITHOUT creating a central, which is the point: it tells us
        // whether Bluetooth was already refused before we consider prompting.
        bluetoothPermission = Self.state(of: CBCentralManager.authorization)
        // Show last-known on-device tiles immediately; a refresh confirms them.
        if let saved = UserDefaults.standard.stringArray(forKey: Self.tileCacheKey) {
            deviceTileIds = Set(saved)
        }
        loadMapLayersState()   // last device's map layers, for the Maps screen while disconnected
        // Returning users get Bluetooth up immediately so the device auto-
        // connects. First-run users create the central when they tap "Enable
        // Bluetooth" in onboarding, so the system prompt lands on that screen.
        if UserDefaults.standard.bool(forKey: Self.onboardedKey) {
            startCentral()
        }
        albumArt.send = { [weak self] gray, w, h in
            self?.sendMediaArt(gray, width: w, height: h) ?? false
        }
        // The live stream is for a rider looking at the app: it stops when
        // the app goes to the background and resumes when it comes back.
        let nc = NotificationCenter.default
        liveObservers = [
            nc.addObserver(forName: UIApplication.didEnterBackgroundNotification,
                           object: nil, queue: .main) { [weak self] _ in
                MainActor.assumeIsolated {
                    self?.appActive = false
                    self?.updateLiveStream()
                }
            },
            nc.addObserver(forName: UIApplication.didBecomeActiveNotification,
                           object: nil, queue: .main) { [weak self] _ in
                MainActor.assumeIsolated {
                    self?.appActive = true
                    self?.updateLiveStream()
                }
            },
        ]
    }

    static let onboardedKey = "didOnboard"

    // MARK: - Live state stream (SVC_LIVE)
    //
    // Runs while a screen that draws it is up (liveViewers, i.e. the Ride
    // tab), the app is in the foreground and the link is admitted: subscribe
    // to TX, write START, repeat START every 5 s (the device stops on its
    // own after 15 s without one). Anything else: STOP and unsubscribe.
    // Firmware without the service never gets here; the Ride tab then reads
    // the status characteristic as before.

    /// The Ride tab on screen (true) or gone (false).
    func liveViewer(_ on: Bool) {
        liveViewers = max(0, liveViewers + (on ? 1 : -1))
        updateLiveStream()
    }

    /// This device has the live service (known once discovery has run).
    var liveSupported: Bool { liveTxChar != nil && liveRxChar != nil }

    private var liveWanted: Bool {
        liveViewers > 0 && appActive && state == .connected && liveSupported
    }

    private func updateLiveStream() {
        guard let p = peripheral, let tx = liveTxChar else {
            liveKeepalive?.invalidate(); liveKeepalive = nil
            return
        }
        if liveWanted {
            if !tx.isNotifying { p.setNotifyValue(true, for: tx) }
            else { sendLiveStart() }
            if liveKeepalive == nil {
                liveKeepalive = Timer.scheduledTimer(withTimeInterval: 5, repeats: true) { [weak self] _ in
                    MainActor.assumeIsolated { self?.sendLiveStart() }
                }
            }
        } else {
            liveKeepalive?.invalidate(); liveKeepalive = nil
            if tx.isNotifying {
                if let rx = liveRxChar { p.writeValue(Data([0x02]), for: rx, type: .withResponse) }
                p.setNotifyValue(false, for: tx)
            }
            live.clear()
        }
    }

    /// START / keepalive. Only once TX is notifying: a frame sent before that
    /// is lost, and the device answers START with one at once.
    private func sendLiveStart() {
        guard liveWanted, let p = peripheral, let rx = liveRxChar,
              liveTxChar?.isNotifying == true else { return }
        p.writeValue(Data([0x01, LiveUUID.protocolVersion]), for: rx, type: .withResponse)
    }

    /// Create the Bluetooth central. iOS shows the Bluetooth permission prompt
    /// the first time this runs. Idempotent; skipped in the update demo.
    private func startCentral() {
        guard central == nil, !isDemoUpdate else { return }
        // A restore identifier lets iOS relaunch the app in the background for
        // the head unit's BLE events after it was terminated for memory while
        // a connection was pending or up — e.g. the device rebooting at the end
        // of a firmware update while the phone sat in a pocket.
        central = CBCentralManager(delegate: self, queue: .main, options: [
            CBCentralManagerOptionRestoreIdentifierKey: Self.restoreIdentifier,
        ])
    }

    static let restoreIdentifier = "com.raemond.opentrailpaper.central"
    /// Set when iOS handed us a peripheral on relaunch, so the first scan
    /// reconnects it instead of searching for a device we already hold.
    private var restoredPeripheral = false

    /// Onboarding: bring Bluetooth up and trigger its permission prompt.
    func enableBluetooth() { startCentral() }

    /// Onboarding: trigger the when-in-use location prompt.
    func requestLocationPermission() {
        if locationManager.authorizationStatus == .notDetermined {
            locationManager.requestWhenInUseAuthorization()
        }
    }

    /// Re-read both permissions. Called when the app comes back to the
    /// foreground, because the user may have just changed them in Settings and
    /// neither framework reports that while we're backgrounded.
    func refreshPermissions() {
        let a = locationManager.authorizationStatus
        locationAuthorized = (a == .authorizedWhenInUse || a == .authorizedAlways)
        locationPermission = Self.state(of: a)
        bluetoothPermission = Self.state(of: CBCentralManager.authorization)
        // Permission granted while we were away: bring the radio up now, so the
        // device connects without the user having to hunt for a button.
        if bluetoothPermission.isGranted { startCentral() }
    }

    private static func state(of a: CLAuthorizationStatus) -> PermissionState {
        switch a {
        case .notDetermined:                       return .notDetermined
        case .authorizedWhenInUse, .authorizedAlways: return .granted
        case .denied:                              return .denied
        case .restricted:                          return .unavailable
        @unknown default:                          return .notDetermined
        }
    }

    private static func state(of a: CBManagerAuthorization) -> PermissionState {
        switch a {
        case .notDetermined:  return .notDetermined
        case .allowedAlways:  return .granted
        case .denied:         return .denied
        case .restricted:     return .unavailable
        @unknown default:     return .notDetermined
        }
    }

    private static let tileCacheKey = "deviceTileIds"
    private func cacheDeviceTiles() {
        UserDefaults.standard.set(Array(deviceTileIds), forKey: Self.tileCacheKey)
    }

    // MARK: GPS aiding + live phone position

    // Stream the phone's location to the device: warm-starts its GPS (AGNSS
    // seed), and serves as a fallback position + altitude source when the
    // device's own GPS has no fix. Started on connect, stopped on disconnect.

    /// Begin streaming the phone's location to the device.
    func startLocationStream() {
        guard routeChar != nil, peripheral != nil else { return }
        wantsAiding = true
        let auth = locationManager.authorizationStatus
        if auth == .notDetermined {
            locationManager.requestWhenInUseAuthorization()
        } else if auth == .authorizedWhenInUse || auth == .authorizedAlways {
            beginLocationUpdates()
        } else {
            wantsAiding = false   // denied — nothing we can do
        }
    }

    private func beginLocationUpdates() {
        wantsAiding = true
        // Background location (the blue status bar; keeps the phone's GPS running
        // while the app is backgrounded) is only justified while a ride is being
        // recorded — then the pocketed phone stays a seed/backup for the device.
        // When not recording we only run location in the FOREGROUND to seed the
        // device; iOS suspends it once the app backgrounds, so there's no battery
        // drain or blue bar while idle. See updateBackgroundLocationMode().
        locationManager.allowsBackgroundLocationUpdates = status.recording
        locationManager.pausesLocationUpdatesAutomatically = !status.recording
        applyAidingAccuracy()
        locationManager.startUpdatingLocation()
        locationManager.requestLocation()   // nudge an immediate first fix
    }

    /// Seeding the receiver needs a rough position; being the RECORDED position
    /// while the head unit has no fix needs a real one. The device throws away
    /// any phone fix it is told is worse than 50 m, which is most of what
    /// `kCLLocationAccuracyHundredMeters` produces, so a recording ride asks for
    /// the good one. Only while recording: full accuracy in the background is
    /// what actually costs the phone's battery.
    private func applyAidingAccuracy() {
        locationManager.desiredAccuracy = status.recording
            ? kCLLocationAccuracyBest : kCLLocationAccuracyHundredMeters
    }

    // Tie background location to the ride state: only a recording ride warrants
    // the phone tracking in the background. Called as the recording flag changes.
    private func updateBackgroundLocationMode() {
        guard wantsAiding else { return }
        locationManager.allowsBackgroundLocationUpdates = status.recording
        locationManager.pausesLocationUpdatesAutomatically = !status.recording
        applyAidingAccuracy()
    }

    func stopLocationStream() {
        wantsAiding = false
        locationManager.stopUpdatingLocation()
        locationManager.allowsBackgroundLocationUpdates = false
    }

    private func transmitAiding(_ loc: CLLocation) {
        guard let c = routeChar, let p = peripheral else { return }
        // Throttle so we don't flood BLE with position updates. 3 s is plenty to
        // keep the receiver seeded, but while a ride is recording these fixes are
        // the head unit's fallback TRACK whenever its own receiver has nothing —
        // and a track sampled every 3 s cuts corners. 1 s while recording.
        let minGap: TimeInterval = status.recording ? 1 : 3
        if let last = lastAidingSent, Date().timeIntervalSince(last) < minGap { return }
        var payload = Data([0x08])
        payload.appendLE(Int32((loc.coordinate.latitude * 1e7).rounded()))
        payload.appendLE(Int32((loc.coordinate.longitude * 1e7).rounded()))
        payload.appendLE(Int32(Date().timeIntervalSince1970))     // current UTC
        // Extended fields (device parses when present): altitude + accuracy.
        payload.appendLE(Int16(max(-2000, min(9000, loc.altitude.rounded()))))
        let acc = loc.horizontalAccuracy > 0 ? loc.horizontalAccuracy : 200
        payload.appendLE(Int16(max(1, min(9999, acc.rounded()))))
        // Speed (cm/s) and course (0.01 deg) — the device shows them while
        // this is its position source. CoreLocation reports -1 for either when
        // it has no valid value (course while stationary, both on a Wi-Fi/cell
        // fix); send 0xFFFF then and the device derives or holds instead.
        // Firmware before this ignores the trailing bytes. A cached fix (the
        // first one delivered can be minutes old) says nothing about how we're
        // moving now.
        let current = -loc.timestamp.timeIntervalSinceNow < 10
        let speedOk = current && loc.speed >= 0 && loc.speedAccuracy >= 0
        payload.appendLE(speedOk ? UInt16(min(65534, (loc.speed * 100).rounded())) : UInt16.max)
        let courseOk = current && loc.course >= 0 && loc.courseAccuracy >= 0
        payload.appendLE(courseOk
            ? UInt16(min(35999, (loc.course.truncatingRemainder(dividingBy: 360) * 100).rounded()))
            : UInt16.max)
        p.writeValue(payload, for: c, type: .withResponse)
        lastAidingSent = Date()
    }

    func startScan() {
        guard let central, central.state == .poweredOn else { return }
        // Since the device bonds (for AMS), iOS holds its BLE link open at the
        // SYSTEM level even when this app has no session — so the device is
        // connected, not advertising, and a scan will sit there forever while
        // music info visibly syncs. Adopt the system's connection instead:
        // connect() on an already-connected peripheral completes immediately
        // and hands this app its own session over the same link.
        if restoredPeripheral, let p = peripheral {
            restoredPeripheral = false
            guard !refusedIds.contains(p.identifier) else {
                peripheral = nil
                watchRefused()
                return
            }
            p.delegate = self
            state = .connecting
            central.connect(p)      // completes at once if iOS kept the link up
            return
        }
        // A system-held link means iOS holds a bond with that device — with no
        // device remembered (fresh install) that makes it ours; with one, only
        // ours counts.
        if peripheral == nil,
           let p = central.retrieveConnectedPeripherals(withServices: [BikeUUID.service])
               .first(where: { !refusedIds.contains($0.identifier) && (pairedDeviceId == nil
                   ? !rejectedIds.contains($0.identifier) : $0.identifier == pairedDeviceId) }) {
            peripheral = p
            p.delegate = self
            state = .connecting
            central.connect(p)
            return
        }
        // Our device, known but not connected: a pending connect has no
        // timeout and fires the moment it is in range, whatever private
        // address the PHONE is on — no scan needed.
        if peripheral == nil, let saved = pairedDeviceId, !refusedIds.contains(saved),
           let p = central.retrievePeripherals(withIdentifiers: [saved]).first {
            peripheral = p
            p.delegate = self
            state = .connecting
            central.connect(p)
            return
        }
        // Our device refused us: don't hunt for it, just listen for it to
        // advertise "unpaired" (see watchRefused).
        if peripheral == nil, let saved = pairedDeviceId, refusedIds.contains(saved) {
            watchRefused()
            return
        }
        state = .scanning
        // Duplicates while hunting for a first device: a device that was
        // paired-elsewhere when first seen must be noticed again the moment
        // its rider unpairs it (its advertisement flips).
        central.scanForPeripherals(withServices: [BikeUUID.service], options: [
            CBCentralManagerScanOptionAllowDuplicatesKey: pairedDeviceId == nil || !refusedIds.isEmpty,
        ])
        if pairedDeviceId == nil { armPairedFallback() }
    }

    /// The rider's Connect / Scan button: the explicit retry that lifts a
    /// refusal. Never called automatically.
    func connectTapped() {
        refusedIds = []
        refusedWhilePaired = []
        barrenConnects = 0
        pairingIssue = nil
        if peripheral == nil { central?.stopScan() }
        startScan()
    }

    /// Refused by our device: listen (no connect) for its advertisement, so a
    /// rider who unpairs it on the panel gets reconnected without a tap. The
    /// state reads "not connected", not "searching" — nothing is being tried.
    private func watchRefused() {
        guard let central, central.state == .poweredOn else { return }
        state = .idle
        central.scanForPeripherals(withServices: [BikeUUID.service], options: [
            CBCentralManagerScanOptionAllowDuplicatesKey: true,
        ])
    }

    /// The device turned this phone away (fixed pairing, firmware
    /// ble_server.cpp): it is paired with another phone, or with this one
    /// before iOS forgot it, and stays that way until the rider unpairs it on
    /// the device. Stop every automatic reconnect and say what to do.
    private func refuse(_ p: CBPeripheral, iosKeysStale: Bool) {
        let id = p.identifier
        refusedIds.insert(id)
        let wasPaired = advPaired[id]
        if wasPaired == true { refusedWhilePaired.insert(id) }
        if wasPaired == false && !iosKeysStale {
            pairingIssue = .pairingNotFinished
        } else if pairedDeviceId == id || iosKeysStale {
            pairingIssue = .notRecognised
        } else {
            pairingIssue = .pairedElsewhere
            rejectedIds.insert(id)
        }
        barrenConnects = 0
    }

    func disconnect() {
        if let p = peripheral { central?.cancelPeripheralConnection(p) }
    }

    /// Manufacturer data [0xFFFF][0x4F][flags]: bit0 = paired to a phone.
    /// nil for firmware from before fixed pairing (no flag at all).
    nonisolated static func advertisesPaired(_ adv: [String: Any]) -> Bool? {
        guard let d = adv[CBAdvertisementDataManufacturerDataKey] as? Data,
              d.count >= 4, d[d.startIndex] == 0xFF, d[d.startIndex + 1] == 0xFF,
              d[d.startIndex + 2] == 0x4F else { return nil }
        return d[d.startIndex + 3] & 0x01 != 0
    }

    /// Looking for a first device and only paired-to-someone ones answer: after
    /// a few seconds, try one of them once (see pairedElsewhereSeen).
    private func armPairedFallback() {
        fallbackTask?.cancel()
        fallbackTask = Task { @MainActor [weak self] in
            var found: (UUID, CBPeripheral)? = nil
            while found == nil {
                try? await Task.sleep(nanoseconds: 6_000_000_000)
                guard let self, !Task.isCancelled, self.state == .scanning,
                      self.peripheral == nil, self.pairedDeviceId == nil else { return }
                if let hit = self.pairedElsewhereSeen.first(where: {
                    !self.rejectedIds.contains($0.key) }) { found = (hit.key, hit.value) }
            }
            guard let self, let (id, p) = found else { return }
            self.rejectedIds.insert(id)   // one try per session, success or not
            self.onFallbackAttempt = true
            self.peripheral = p
            p.delegate = self
            self.state = .connecting
            self.central.stopScan()
            self.central.connect(p)
        }
    }

    /// Remember the device we're connected to as OURS. Called once it has
    /// delivered data — proof the device admitted this phone's bond.
    private func rememberPairedDevice() {
        guard let p = peripheral else { return }
        onFallbackAttempt = false
        pairingIssue = nil
        rejectedIds.remove(p.identifier)
        refusedIds.remove(p.identifier)
        guard pairedDeviceId != p.identifier else { return }
        pairedDeviceId = p.identifier
        pairedDeviceName = p.name
        UserDefaults.standard.set(p.identifier.uuidString, forKey: Self.pairedDeviceKey)
        UserDefaults.standard.set(p.name, forKey: Self.pairedDeviceNameKey)
    }

    /// Set after Unpair succeeds: the device dropped its bond, so iOS's copy
    /// is now stale and only the rider can delete it (Settings › Bluetooth).
    @Published var unpairNeedsIOSForget = false
    private var unpairWritePending = false

    /// Settings › Unpair: while connected, tell the device to drop this
    /// iPhone ([0xFE 0x55] on settings — the same as Paired Devices › Phone ›
    /// Unpair on the panel), then forget it here. Not connected: forget here
    /// only; the device keeps the pairing until it is unpaired on the device.
    func unpairDevice() {
        guard state == .connected, let c = settingsChar, let p = peripheral else {
            forgetDevice()
            return
        }
        unpairWritePending = true
        p.writeValue(Data([0xFE, 0x55]), for: c, type: .withResponse)
        // The device drops the link right after it acts; don't wait forever
        // for the acknowledgment if it went down first.
        DispatchQueue.main.asyncAfter(deadline: .now() + 3) { [weak self] in
            guard let self, self.unpairWritePending else { return }
            self.unpairDone(ok: true)
        }
    }

    private func unpairDone(ok: Bool) {
        unpairWritePending = false
        forgetDevice()
        if ok { unpairNeedsIOSForget = true }
    }

    /// Settings › Forget this device: stop connecting to it and look for a
    /// device to pair with again. The app can't delete the iOS bond — the
    /// rider does that in iOS Settings (the UI says so).
    func forgetDevice() {
        // Not re-adopted this session (the fallback / system-link paths would
        // otherwise walk straight back into it while iOS still holds the bond).
        let forgotten = pairedDeviceId ?? peripheral?.identifier
        pairedDeviceId = nil
        pairedDeviceName = nil
        UserDefaults.standard.removeObject(forKey: Self.pairedDeviceKey)
        UserDefaults.standard.removeObject(forKey: Self.pairedDeviceNameKey)
        rides = []   // not ours any more; its saved list returns if it's re-paired
        pairingIssue = nil
        barrenConnects = 0
        rejectedIds = forgotten.map { [$0] } ?? []
        pairedElsewhereSeen = [:]
        onFallbackAttempt = false
        if let p = peripheral, p.state == .connected {
            forgetting = true
            central?.cancelPeripheralConnection(p)   // didDisconnect rescans
        } else {
            // Pending connect (device out of range): drop it here. A late
            // didDisconnect for it finds peripheral == nil and won't revive it.
            if let p = peripheral { central?.cancelPeripheralConnection(p) }
            peripheral = nil
            central?.stopScan()
            startScan()
        }
    }

    // MARK: settings

    // Every app-side edit is recorded as a pending edit for the current device
    // (cached, timestamped), shown at once, and pushed when — and only when —
    // this link has read the device's own values: immediately while
    // connected, on the next connect otherwise. The merge, the payload and the
    // conflict rule live in DeviceSettingsSync.swift. Values arriving FROM the
    // device (parseSettings) update the cache and never echo back.
    func setFtp(_ v: Int) { ftpWatts = v; editSetting(.ftp, v) }
    func setTz(_ v: Int) { tzMinutes = v; editSetting(.tz, v) }
    func setBacklight(_ v: Int) { backlight = v; editSetting(.backlight, v) }
    func setUseMiles(_ v: Bool) {
        UserDefaults.standard.set(v, forKey: UnitPref.key)
        editSetting(.useMiles, v ? 1 : 0)
    }
    func setClock24h(_ v: Bool) { clock24h = v; editSetting(.clock24h, v ? 1 : 0) }
    func setUsbDrive(_ v: Bool) { usbDrive = v; editSetting(.usbDrive, v ? 1 : 0) }
    func setWheelCircMm(_ v: Int) {
        let mm = min(max(v, WheelSize.minMm), WheelSize.maxMm)
        wheelCircMm = mm
        editSetting(.wheelCircMm, mm)
    }

    /// Whether the device takes this field. Unknown (never connected) counts
    /// as yes, so a first-run rider still sees every control.
    func supportsSetting(_ f: SettingField) -> Bool {
        settingsCache.device?.supports(f) ?? true
    }

    /// Which device the settings cache is for. Called with the peripheral's
    /// identifier on every connect; a fixed-pairing flow calls it with the
    /// paired device's identifier at launch, so offline edits are keyed to
    /// that device before it is ever in range. Edits made before any device
    /// was known move to the first one (DeviceSettingsStore.setCurrent).
    func setCurrentDevice(_ id: String) {
        guard id != settingsDeviceID, !isDemoOfflineSettings else { return }
        settingsDeviceID = id
        rides = rideListingStore.load(id)   // that device's last-known ride list
        settingsCache = settingsStore.setCurrent(id)
        dashConfig = settingsCache.dashText.map { DashConfig(text: $0) }
        dashConflict = nil
        settingsSyncNote = nil
        applyEffectiveSettings()
    }

    private func loadSettingsCache() {
        settingsDeviceID = settingsStore.currentDevice
        rides = rideListingStore.load(settingsDeviceID)   // last-known ride list
        settingsCache = settingsStore.load(settingsDeviceID)
        if let t = settingsCache.dashText { dashConfig = DashConfig(text: t) }
        applyEffectiveSettings()
    }

    private func saveSettingsCache() {
        guard !isDemoOfflineSettings else { return }
        settingsStore.save(settingsCache, for: settingsDeviceID)
    }

    /// Publish pending ?? device for every field; leave the display default
    /// where nothing is known.
    private func applyEffectiveSettings() {
        let c = settingsCache
        func v(_ f: SettingField) -> Int? {
            SettingsMerge.effective(f, device: c.device, pending: c.pending)
        }
        if let x = v(.ftp), x != ftpWatts { ftpWatts = x }
        if let x = v(.tz), x != tzMinutes { tzMinutes = x }
        if let x = v(.backlight), x != backlight { backlight = x }
        if let x = v(.clock24h), (x != 0) != clock24h { clock24h = x != 0 }
        if let x = v(.usbDrive), (x != 0) != usbDrive { usbDrive = x != 0 }
        // Absent = firmware without speed-sensor support: hide the setting.
        let wheel = c.device?.supports(.wheelCircMm) == true ? v(.wheelCircMm) : nil
        if wheel != wheelCircMm { wheelCircMm = wheel }
        // Units are also the app's own display preference.
        if let x = v(.useMiles),
           UserDefaults.standard.bool(forKey: UnitPref.key) != (x != 0) {
            UserDefaults.standard.set(x != 0, forKey: UnitPref.key)
        }
        let pd = c.pendingDash.map { DashConfig(text: $0.text) }
        if pd != pendingDashConfig { pendingDashConfig = pd }
    }

    private func editSetting(_ f: SettingField, _ v: Int) {
        // Always recorded, even when it equals the cached device value: a
        // write carrying an older edit may be in flight. syncSettings drops
        // it once nothing is in flight and the device already matches.
        settingsCache.pending[f.rawValue] = PendingEdit(value: v, editedAt: Date())
        settingsWriteFailedThisLink = false
        settingsSyncNote = nil
        saveSettingsCache()
        syncSettings()
    }

    /// Push pending edits, merged onto the device's values, as one write.
    private func syncSettings() {
        guard settingsSeenThisLink, settingsWriteInFlight == nil, !settingsWriteFailedThisLink,
              let dev = settingsCache.device, let c = settingsChar, let p = peripheral
        else { return }
        let r = SettingsMerge.merge(device: dev, lastKnown: nil, pending: settingsCache.pending)
        for f in r.dropped { settingsCache.pending[f.rawValue] = nil }
        settingsCache.pending = SettingsMerge.clearSatisfied(pending: settingsCache.pending,
                                                             device: dev)
        saveSettingsCache()
        applyEffectiveSettings()
        guard let payload = r.payload else { return }
        settingsWriteInFlight = r.pushed
        p.writeValue(payload, for: c, type: .withResponse)
    }

    /// The device acknowledged (or refused) the settings write.
    private func settingsWriteAcked(ok: Bool) {
        guard let pushed = settingsWriteInFlight else { return }
        settingsWriteInFlight = nil
        guard ok, let dev = settingsCache.device else {
            // Keep the pending edits; the next connect (or the next edit) retries.
            settingsWriteFailedThisLink = true
            return
        }
        settingsCache.device = SettingsMerge.applyAcknowledged(device: dev, pushed: pushed)
        settingsCache.pending = SettingsMerge.clearAcknowledged(pending: settingsCache.pending,
                                                                pushed: pushed)
        saveSettingsCache()
        applyEffectiveSettings()
        syncSettings()   // anything edited while the write was in flight
    }

    private func resetSyncLinkState() {
        settingsSeenThisLink = false
        settingsWriteInFlight = nil
        settingsWriteFailedThisLink = false
        dashSeenThisLink = false
        dashWritesOutstanding = 0
        dashWriteError = false
        dashWriteText = nil
        dashWriteFailedThisLink = false
    }

    /// -demo-offline-settings: a device seen before, disconnected now, with
    /// backlight and FTP changed in the app since. Not persisted.
    private func seedOfflineSettingsDemo() {
        var c = DeviceSettingsCache()
        c.device = DeviceSettingsValues.decode(Data([0xFA, 0x00, 0x5C, 0xFE, 0, 2, 1, 1]))
        c.pending = ["backlight": PendingEdit(value: 0, editedAt: Date()),
                     "ftp": PendingEdit(value: 265, editedAt: Date())]
        c.dashText = DashConfig.deviceDefault.configText
        settingsCache = c
        dashConfig = DashConfig.deviceDefault
        applyEffectiveSettings()
    }

    // MARK: rides (device -> phone)

    func refreshRides() {
        guard let c = ridesChar, let p = peripheral else { return }
        // The device holds ONE pending request: a list command written while a
        // transfer is in flight is overwritten by (or overwrites) the next
        // queued download, and whichever loses is silently dropped — leaving the
        // list spinning forever or the download waiting on its watchdog. Wait
        // for the line instead.
        guard dlActive == nil, dlQueue.isEmpty else { listRefreshPending = true; return }
        ridesBuilding = []
        loadingRides = true
        p.writeValue(Data([0x01]), for: c, type: .withResponse)
    }

    func deleteRide(_ name: String) {
        rides.removeAll { $0.name == name }
        ridesBuilding.removeAll { $0.name == name }
        rideListingStore.save(rides, for: settingsDeviceID)
        // Drop it from the download queue too, so a deleted ride doesn't get
        // fetched (and re-cached) moments later.
        dlQueue.removeAll { $0 == .ride(name) }
        publishQueue()
        try? FileManager.default.removeItem(at: BLEManager.cachedURL(for: name))
        // Offline: just drop the local cache. Connected: also delete on device.
        guard let c = ridesChar, let p = peripheral else { return }
        var cmd = Data([0x03]); cmd.append(Data(name.utf8))
        p.writeValue(cmd, for: c, type: .withResponse)
    }

    // Pull /diag.log off the device (reuses the reliable ride-transfer path).
    func downloadLog() {
        guard ridesChar != nil, peripheral != nil else {
            lastMessage = "Not connected"; return
        }
        enqueueTransfer(.log(""))
    }

    // List the per-day log files on the device (reply parsed via 0x30/0x31).
    func requestLogList() {
        guard let c = ridesChar, let p = peripheral else { return }
        guard dlActive == nil, dlQueue.isEmpty else { logListPending = true; return }
        logsBuilding = []
        loadingLogs = true
        p.writeValue(Data([0x06]), for: c, type: .withResponse)
    }

    // Download one specific day's log file.
    func downloadLogFile(_ name: String) {
        guard ridesChar != nil, peripheral != nil else { lastMessage = "Not connected"; return }
        enqueueTransfer(.log(name))
    }

    // MARK: firmware / OTA

    /// True when the device is running something other than the newest release.
    /// Still a string compare, but now against ONE source of truth rather than a
    /// hand-maintained constant that could disagree with the bytes beside it.
    var updateAvailable: Bool {
        guard let tag = FirmwareRelease.shared.latest?.tag else { return false }
        return !deviceFirmware.isEmpty && deviceFirmware != tag
    }

    func queryDeviceFirmware() {
        guard let c = otaChar, let p = peripheral else { return }
        p.writeValue(Data([0x05]), for: c, type: .withResponse)
    }

    // Stream the app-bundled firmware.bin into the device's spare OTA slot.
    // Keep the screen awake so an OTA/map transfer isn't interrupted when the
    // phone would otherwise auto-lock. Backgrounding still works because the app
    // declares the `bluetooth-central` background mode (Info.plist), so
    // CoreBluetooth keeps delivering peripheralIsReady and the pump continues.
    private func keepAwake(_ on: Bool) {
        UIApplication.shared.isIdleTimerDisabled = on
    }

    func startFirmwareUpdate() {
        guard let c = otaChar, let p = peripheral else {
            otaMessage = "Not connected"; return
        }
        guard let release = FirmwareRelease.shared.latest else {
            otaMessage = "No release found — check for updates first"; return
        }
        keepAwake(true)
        otaMessage = "Downloading \(release.tag)…"
        TransferCenter.shared.begin(Self.otaTransferId, kind: .firmware,
            title: "Firmware \(release.tag)", detail: "Downloading from GitHub…",
            total: Int64(release.size))
        Task { @MainActor in
            do {
                let data = try await FirmwareRelease.shared.image(for: release,
                                                                  transferId: Self.otaTransferId)
                startFirmwareUpload(data, tag: release.tag)
            } catch {
                keepAwake(false)
                otaMessage = "Download failed — check your connection"
                TransferCenter.shared.finish(Self.otaTransferId, success: false,
                                             message: "Download failed")
            }
        }
    }

    /// Second half of startFirmwareUpdate, once the image is in hand.
    static let otaTransferId = "ble.ota"

    private func startFirmwareUpload(_ data: Data, tag: String) {
        guard let c = otaChar, let p = peripheral else {
            keepAwake(false); otaMessage = "Not connected"
            TransferCenter.shared.finish(Self.otaTransferId, success: false, message: "Not connected")
            return
        }
        TransferCenter.shared.begin(Self.otaTransferId, kind: .firmware,
            title: "Firmware \(tag)", detail: "Sending to the device…",
            total: Int64(data.count))
        otaTargetVersion = tag
        otaData = data
        otaOffset = 0
        otaCommitSent = false
        otaInProgress = true
        otaPhase = .sending
        otaProgress = 0
        otaMessage = "Preparing…"
        otaWatchdog?.cancel(); otaWatchdog = nil
        otaChunk = max(20, p.maximumWriteValueLength(for: .withoutResponse)) - 1

        // begin: [0x01][u32 size][32-char md5 hex]
        var cmd = Data([0x01])
        var size = UInt32(data.count).littleEndian
        withUnsafeBytes(of: &size) { cmd.append(contentsOf: $0) }
        let md5 = Insecure.MD5.hash(data: data).map { String(format: "%02x", $0) }.joined()
        cmd.append(Data(md5.utf8))
        p.writeValue(cmd, for: c, type: .withResponse)
    }

    // Push firmware chunks as fast as CoreBluetooth allows using
    // write-without-response (many packets per connection interval). Pauses
    // when the send queue is full and resumes from peripheralIsReady.
    private func pumpOtaChunks() {
        guard otaInProgress, let c = otaChar, let p = peripheral else { return }
        while otaOffset < otaData.count {
            guard p.canSendWriteWithoutResponse else { return }   // resume later
            let end = min(otaOffset + otaChunk, otaData.count)
            var pkt = Data([0x02])
            pkt.append(otaData.subdata(in: otaOffset..<end))
            otaOffset = end
            otaProgress = Double(otaOffset) / Double(otaData.count)
            TransferCenter.shared.update(Self.otaTransferId, completed: Int64(otaOffset))
            p.writeValue(pkt, for: c, type: .withoutResponse)
        }
        if !otaCommitSent {                                        // all data sent
            otaCommitSent = true
            p.writeValue(Data([0x03]), for: c, type: .withResponse)   // commit
            otaPhase = .saving
            otaMessage = "Saving to the device…"
            TransferCenter.shared.update(Self.otaTransferId, detail: "Saving to the device…",
                                         indeterminate: true)
        }
    }

    // Fail the update if the device doesn't reach the new version in time.
    private func armOtaWatchdog(seconds: UInt64, failMessage: String) {
        otaWatchdog?.cancel()
        otaWatchdog = Task { @MainActor in
            try? await Task.sleep(nanoseconds: seconds * 1_000_000_000)
            guard !Task.isCancelled, otaInProgress else { return }
            otaFinish(.failed, failMessage)
        }
    }
    private static let installWatchdogMsg =
        "Device didn't come back after installing. Check it's powered on and nearby, or use the SD-card method."

    private func otaFinish(_ phase: OTAPhase, _ msg: String) {
        TransferCenter.shared.finish(Self.otaTransferId, success: phase == .done,
                                     message: phase == .done ? "Updated to \(deviceFirmware)" : msg)
        otaWatchdog?.cancel(); otaWatchdog = nil
        otaInProgress = false
        otaPhase = phase
        otaMessage = msg
        keepAwake(false)
    }

    private func handleOtaNotify(_ d: Data) {
        guard let op = d.first else { return }
        switch op {
        case 0xA3:                                   // running version
            deviceFirmware = String(decoding: d[1...], as: UTF8.self)
            if !otaTargetVersion.isEmpty && deviceFirmware == otaTargetVersion {
                // Now on the new version — success (clears any earlier transient
                // failure state too, e.g. a reconnect seen mid-flash).
                if otaInProgress {
                    otaProgress = 1
                    otaFinish(.done, "Updated to \(deviceFirmware) 🎉")
                } else if otaPhase == .failed {
                    otaWatchdog?.cancel(); otaPhase = .idle; otaMessage = nil
                }
            } else if otaInProgress && otaCommitSent {
                // Reconnected but STILL on the old version. The device flashes
                // from SD and reboots ONCE MORE, so don't fail yet — give it a
                // short grace for that second reboot to land on the new version.
                otaPhase = .verifying
                otaMessage = "Installing — flashing from the SD card…"
                TransferCenter.shared.update(Self.otaTransferId,
                                             detail: "Installing — flashing from the SD card…",
                                             indeterminate: true)
                armOtaWatchdog(seconds: 60,
                    failMessage: "Device restarted but is still on \(deviceFirmware). The install didn't take — try again, or use the SD-card method.")
            }
        case 0xA0: otaPhase = .sending; otaMessage = "Sending firmware…"; pumpOtaChunks()
        case 0xA1:                                   // received + saved to SD
            // The device reboots and flashes from SD now; wait for the reconnect
            // version-check to confirm. Arm the watchdog in case it never returns.
            otaProgress = 1
            otaPhase = .installing
            otaMessage = "Installing — the device is restarting…"
            TransferCenter.shared.update(Self.otaTransferId,
                                         detail: "Installing — the device is restarting…",
                                         indeterminate: true)
            keepAwake(false)
            armOtaWatchdog(seconds: 150, failMessage: BLEManager.installWatchdogMsg)
        case 0xA2: otaFinish(.failed, "Update canceled.")
        case 0xAF:                                   // error
            let code = d.count > 1 ? Int(d[1]) : -1
            otaFinish(.failed,
                "Transfer failed (error \(code)). The device is unchanged — tap to try again.")
        default: break
        }
    }

    func refreshRoutes() {
        guard let c = routeChar, let p = peripheral else { return }
        deviceRoutes = []
        loadingRoutes = true
        p.writeValue(Data([0x06]), for: c, type: .withResponse)
    }

    func deleteRoute(_ name: String) {
        guard let c = routeChar, let p = peripheral else { return }
        deviceRoutes.removeAll { $0 == name }
        var cmd = Data([0x07]); cmd.append(Data(name.utf8))
        p.writeValue(cmd, for: c, type: .withResponse)
    }

    // MARK: sensor management (device's HR / power / cadence sensors)

    func startSensorScan() {
        guard let c = sensorsChar, let p = peripheral else { return }
        scanningSensors = true
        p.writeValue(Data([0x01]), for: c, type: .withResponse)
    }
    // One snapshot of the device's sensors + status, without starting a scan.
    func refreshSensors() {
        guard let c = sensorsChar, let p = peripheral else { return }
        p.writeValue(Data([0x05]), for: c, type: .withResponse)
    }
    // Connected sensors whose kind mask includes `bit` (1 HR, 2 power, 4 cadence,
    // 8 radar, 16 speed).
    func connectedSensor(kind bit: UInt8) -> BikeSensor? {
        sensors.first { $0.connected && $0.kindsMask & bit != 0 }
    }
    func stopSensorScan() {
        guard let c = sensorsChar, let p = peripheral else { return }
        scanningSensors = false
        p.writeValue(Data([0x02]), for: c, type: .withResponse)
    }
    func pairSensor(_ addr: String) {
        guard let c = sensorsChar, let p = peripheral else { return }
        var cmd = Data([0x03]); cmd.append(Data(addr.utf8))
        p.writeValue(cmd, for: c, type: .withResponse)
    }
    func forgetSensor(_ addr: String) {
        guard let c = sensorsChar, let p = peripheral else { return }
        var cmd = Data([0x04]); cmd.append(Data(addr.utf8))
        p.writeValue(cmd, for: c, type: .withResponse)
    }

    // MARK: vector-map upload (phone -> device SD, mirrors the OTA transfer)

    var canUploadMap: Bool { (mapChar != nil && peripheral != nil) || isDemoMaps }

    func uploadMap(_ ebm: Data, name: String) {
        guard let c = mapChar, let p = peripheral else {
            mapMessage = "Not connected"; return
        }
        mapData = ebm
        mapOffset = 0
        mapEndSent = false
        mapUploading = true
        mapProgress = 0
        mapMessage = "Sending map…"
        keepAwake(true)
        mapChunk = max(20, p.maximumWriteValueLength(for: .withoutResponse)) - 1
        TransferCenter.shared.begin(Self.mapTransferId, kind: .mapUpload, title: "Sending map",
                                    detail: name, total: Int64(ebm.count))

        var cmd = Data([0x01])
        var size = UInt32(ebm.count).littleEndian
        withUnsafeBytes(of: &size) { cmd.append(contentsOf: $0) }
        cmd.append(Data(name.utf8))
        p.writeValue(cmd, for: c, type: .withResponse)
    }

    func cancelMapUpload() {
        guard let c = mapChar, let p = peripheral else { return }
        p.writeValue(Data([0x04]), for: c, type: .withResponse)
        mapUploading = false; keepAwake(false)
        mapMessage = "Canceled"
        TransferCenter.shared.finish(Self.mapTransferId, success: false, message: "Canceled")
    }

    // Ask the device which map areas it already has (streamed back via notify).
    func refreshDeviceMaps() {
        guard let c = mapChar, let p = peripheral else { return }
        p.writeValue(Data([0x05]), for: c, type: .withResponse)
    }

    // MARK: H3 tile streaming

    // Ask the device which H3 tile ids are already on its SD, so the caller can
    // skip re-sending them (replied via 0xD0/0xD1/0xD2 notifies). Asks for each
    // file's version too; firmware that keeps none ignores that (DeviceTileList).
    func refreshDeviceTiles() {
        guard let c = mapChar, let p = peripheral else { return }
        p.writeValue(DeviceTileList.request(0x07), for: c, type: .withResponse)
        p.writeValue(DeviceTileList.request(0x08), for: c, type: .withResponse)   // .poi files (new firmware only)
    }

    /// On the device. A tile there is never re-sent on its own — even one
    /// built before the bike-route flags; the rider chooses that (Redownload).
    func tileIsCurrent(_ id: String) -> Bool { deviceTileIds.contains(id) }

    /// On the device, on firmware that draws the cycling layer, but not sent by
    /// this phone with the bike-route flags — so it most likely predates them.
    /// Only informs the "redownload to add them" hint; a tile from the website
    /// ZIP or another phone may already have them. A tile the device has a
    /// version record for was sent by an app that sends them, so after them.
    func tilePredatesCycling(_ id: String) -> Bool {
        deviceSupportsPois && deviceTileIds.contains(id) && !flaggedTileIds.contains(id)
            && deviceTileRecords[id] == nil
    }

    private func tileRecord(_ id: String) -> String? {
        DeviceTileList.record(device: deviceTileRecords[id], phone: tileVersions.tile(deviceKey, id))
    }
    private func poiRecord(_ id: String) -> String? {
        DeviceTileList.record(device: devicePoiRecords[id], phone: tileVersions.poi(deviceKey, id))
    }

    /// The device's tile against the CDN index: the same hash is current; a
    /// different hash, a phone-built tile or one with no record (on the device
    /// or this phone) is an update; `.unknown` when the CDN index has no entry
    /// (unreachable, not pre-built, built empty).
    func tileVersionState(_ id: String) -> PrebuiltTiles.VersionState {
        PrebuiltTiles.state(record: tileRecord(id),
                            cdnHash: PrebuiltTiles.tileHash(CdnVersions.shared.entry(id)))
    }

    /// The device's .poi against the CDN index (same rules as tiles).
    func poiVersionState(_ id: String) -> PrebuiltTiles.VersionState {
        PrebuiltTiles.state(record: poiRecord(id),
                            cdnHash: PrebuiltTiles.poiHash(CdnVersions.shared.entry(id)))
    }

    /// Version records for the current device, for the download's filters:
    /// the device's own where it has one, else what this phone sent.
    var tileRecords: [String: String] {
        tileVersions.tiles(deviceKey).merging(deviceTileRecords) { _, device in device }
    }
    var poiRecords: [String: String] {
        tileVersions.pois(deviceKey).merging(devicePoiRecords) { _, device in device }
    }

    /// On the device, and Redownload would bring something new. With a CDN
    /// index entry, exactly when the hash differs from what this phone sent
    /// (tileVersionState). Without one, the heuristic: it predates the
    /// bike-route data, or this phone sent it more than tileRefreshAge ago.
    /// Drawn ochre/hatched on the Maps screen and counted as "updates".
    func tileNeedsUpdate(_ id: String) -> Bool {
        guard deviceTileIds.contains(id) else { return false }
        switch tileVersionState(id) {
        case .current: return false
        case .update: return true
        case .unknown: return tileNeedsUpdateByAge(id)
        }
    }

    /// The pre-CDN heuristic behind tileNeedsUpdate.
    func tileNeedsUpdateByAge(_ id: String) -> Bool {
        guard deviceTileIds.contains(id) else { return false }
        if tilePredatesCycling(id) { return true }
        // When this phone sent it, else when the device says it was built.
        guard let at = tileSentAt[id]
                ?? PrebuiltTiles.phoneBuiltAt(deviceTileRecords[id])?.timeIntervalSince1970
        else { return false }
        return Date().timeIntervalSince1970 - at > Self.tileRefreshAge
    }

    /// The POI drop drawn on an on-device hex: none on firmware without the POI
    /// layer, filled when the device has fresh POIs, hollow otherwise.
    func poiMark(_ id: String) -> HexPoiMark {
        guard deviceSupportsPois, deviceTileIds.contains(id) else { return .none }
        return poiNeedsSend(id) ? .missing : .present
    }

    /// The tile's POIs should go to the device: it can draw them, and it has
    /// none for this tile, or the CDN has a different .poi than the one sent
    /// (poiVersionState). Without a CDN entry: this phone sent them more than
    /// PoiCache.maxAge ago.
    func poiNeedsSend(_ id: String) -> Bool {
        guard deviceSupportsPois else { return false }
        guard devicePoiIds.contains(id) else { return true }
        switch poiVersionState(id) {
        case .current: return false
        case .update: return true
        case .unknown:
            guard let at = poiSentAt[id] else { return false }   // someone else's: leave it
            return Date().timeIntervalSince1970 - at > PoiCache.maxAge
        }
    }

    // Streaming upload: the app produces tiles batch-by-batch while download +
    // vectorization runs, and sends them in parallel. Call startTileStream()
    // once, enqueueTiles() per batch as they're built, finishTileStream() when
    // the last batch has been produced.
    /// `resend`: a Redownload — tiles and POIs go out even though the device
    /// already has them (POIs still only to firmware that draws them).
    func startTileStream(resend: Bool = false) {
        guard mapChar != nil, peripheral != nil else { tileMessage = "Not connected"; return }
        resendAll = resend
        tileJobStarted = Date()
        tileJobBytes = 0
        tileQueue = []
        tilesTotal = 0
        tilesDone = 0
        tileJobFailed = false
        tilesMoreComing = true
        tilesUploading = true
        tileMessage = "Sending tiles…"
        keepAwake(true)
        TransferCenter.shared.begin(Self.tilesTransferId, kind: .mapTiles, title: "Map tiles",
                                    detail: "Building tiles…", unit: .items, indeterminate: true)
    }

    static let mapTransferId = "ble.map"
    static let tilesTransferId = "ble.tiles"

    /// Mirror the tile job into TransferCenter.
    private func reportTiles() {
        let idle = currentTileId == nil && tileQueue.isEmpty && tilesMoreComing
        TransferCenter.shared.update(Self.tilesTransferId,
            completed: Int64(tilesDone), total: Int64(tilesTotal),
            detail: idle ? "Building tiles…" : "Sending tile \(min(tilesDone + 1, tilesTotal)) of \(tilesTotal)",
            indeterminate: tilesTotal == 0)
    }

    // Add freshly-built tiles to the send queue; starts pumping if idle. Skips
    // tiles already on the device or already queued. `versions`: each tile's
    // record (PrebuiltTiles.cdnRecord); a tile without one is phone-built.
    func enqueueTiles(_ newOnes: [(id: String, data: Data)], versions: [String: String] = [:]) {
        enqueue(newOnes.filter { resendAll || !tileIsCurrent($0.id) }, poi: false, versions: versions)
    }

    /// Queue tiles' `.poi` files (sent through the same tile transfer — the
    /// firmware tells them apart by their magic). Skipped for firmware without
    /// POI support; which POIs need sending (missing, changed hash, old) is
    /// the caller's choice (MapsView, from poiNeedsSend and the CDN index).
    func enqueuePois(_ newOnes: [(id: String, data: Data)], versions: [String: String] = [:]) {
        enqueue(newOnes.filter { _ in deviceSupportsPois }, poi: true, versions: versions)
    }

    private func enqueue(_ newOnes: [(id: String, data: Data)], poi: Bool, versions: [String: String]) {
        guard tilesUploading else { return }
        let queued = Set(tileQueue.filter { $0.poi == poi }.map(\.id))
        let fresh = newOnes.filter {
            !queued.contains($0.id) && !($0.id == currentTileId && currentIsPoi == poi)
        }
        guard !fresh.isEmpty else { return }
        tileQueue.append(contentsOf: fresh.map {
            TileJob(id: $0.id, data: $0.data, poi: poi, version: versions[$0.id])
        })
        tilesTotal += fresh.count
        if currentTileId == nil { sendNextTile() }   // pump if idle
        reportTiles()
    }

    // No more batches coming; let the queue drain and finish.
    func finishTileStream() {
        tilesMoreComing = false
        if currentTileId == nil { sendNextTile() }   // trigger drain-finish if idle
    }

    func cancelTileUpload() {
        tileQueue = []
        tilesMoreComing = false
        if let c = mapChar, let p = peripheral, currentTileId != nil {
            p.writeValue(Data([0x04]), for: c, type: .withResponse)
        }
        finishTileJob(message: "Canceled")
    }

    private func finishTileJob(message: String?) {
        if tilesUploading {
            let secs = Date().timeIntervalSince(tileJobStarted)
            print(String(format: "tile job: %d files, %d KB, %.1f s from first build to last ack (%.1f KB/s overall)",
                         tilesDone, tileJobBytes / 1024, secs, Double(tileJobBytes) / 1024 / max(secs, 0.1)))
            let ok = message == "Tiles installed"
            TransferCenter.shared.finish(Self.tilesTransferId, success: ok,
                message: ok ? "\(tilesDone) tile\(tilesDone == 1 ? "" : "s") on the device"
                            : (message ?? "Stopped"))
        }
        tilesUploading = false
        tilesMoreComing = false
        mapUploading = false
        currentTileId = nil
        keepAwake(false)
        tileVersions.save()
        if let message { tileMessage = message }
    }

    private func sendNextTile() {
        guard tilesUploading, let c = mapChar, let p = peripheral else { return }
        guard let tile = tileQueue.first else {   // queue drained
            if tilesMoreComing { return }         // more tiles still being built — idle, wait
            finishTileJob(message: tileJobFailed ? "Some tiles failed" : "Tiles installed")
            refreshDeviceTiles()
            return
        }
        tileQueue.removeFirst()
        currentTileId = tile.id
        currentIsPoi = tile.poi
        // Fixed now, so the device and this phone record the same thing.
        currentVersion = tile.version ?? PrebuiltTiles.phoneRecord()
        mapData = tile.data
        mapOffset = 0
        mapEndSent = false
        mapUploading = true            // reuse the CHR_MAP chunk pump
        mapChunk = max(20, p.maximumWriteValueLength(for: .withoutResponse)) - 1
        tileMessage = "Sending tile \(tilesDone + 1)/\(tilesTotal)…"
        reportTiles()

        var cmd = Data([0x06])         // begin-tile
        var size = UInt32(tile.data.count).littleEndian
        withUnsafeBytes(of: &size) { cmd.append(contentsOf: $0) }
        // A .poi goes under "<id>.poi"; saveTile strips the extension and
        // stores it by its magic either way, the name just reads well in logs.
        // Firmware that keeps versions gets the record after a NUL.
        cmd.append(DeviceTileList.beginName(tile.poi ? "\(tile.id).poi" : tile.id,
                                            record: deviceKeepsVersions ? currentVersion : nil))
        p.writeValue(cmd, for: c, type: .withResponse)
    }

    private func pumpMapChunks() {
        guard mapUploading, let c = mapChar, let p = peripheral else { return }
        while mapOffset < mapData.count {
            guard p.canSendWriteWithoutResponse else { return }   // resume from ready
            let end = min(mapOffset + mapChunk, mapData.count)
            var pkt = Data([0x02])
            pkt.append(mapData.subdata(in: mapOffset..<end))
            mapOffset = end
            mapProgress = Double(mapOffset) / Double(max(mapData.count, 1))
            if currentTileId == nil {
                TransferCenter.shared.update(Self.mapTransferId, completed: Int64(mapOffset))
            }
            p.writeValue(pkt, for: c, type: .withoutResponse)
        }
        if !mapEndSent {
            mapEndSent = true
            p.writeValue(Data([0x03]), for: c, type: .withResponse)   // end -> save
            mapMessage = "Saving to device…"
        }
    }

    private func handleMapNotify(_ d: Data) {
        guard let op = d.first else { return }
        switch op {
        case 0xB0: pumpMapChunks()                                // device ready (map or tile)
        case 0xB1:                                                // saved + active
            if let id = currentTileId {                           // a tile finished
                tileJobBytes += mapData.count
                let version = currentVersion ?? PrebuiltTiles.phoneRecord()
                // Kept by the device too: mirror it, as its next list would.
                if deviceKeepsVersions {
                    if currentIsPoi { devicePoiRecords[id] = version } else { deviceTileRecords[id] = version }
                }
                if currentIsPoi {
                    devicePoiIds.insert(id)
                    tileVersions.setPoi(deviceKey, id, version)
                    poiSentAt[id] = Date().timeIntervalSince1970
                    UserDefaults.standard.set(poiSentAt, forKey: "poiSentAt")
                } else {
                    tileVersions.setTile(deviceKey, id, version)
                    deviceTileIds.insert(id)
                    flaggedTileIds.insert(id)
                    UserDefaults.standard.set(Array(flaggedTileIds), forKey: "flaggedTileIds")
                    tileSentAt[id] = Date().timeIntervalSince1970
                    UserDefaults.standard.set(tileSentAt, forKey: "tileSentAt")
                }
                tilesDone += 1
                currentTileId = nil
                sendNextTile()
                reportTiles()
            } else {
                mapUploading = false; mapProgress = 1; mapMessage = "Map installed"
                TransferCenter.shared.finish(Self.mapTransferId, success: true, message: "Map installed")
                keepAwake(false); refreshDeviceMaps()
            }
        case 0xBF:
            let code = d.count > 1 ? Int(d[1]) : -1
            if currentTileId != nil {                             // skip the failed tile, keep going
                tileJobFailed = true
                currentTileId = nil
                sendNextTile()
            } else {
                mapUploading = false; keepAwake(false)
                mapMessage = "Map upload failed (\(code))"
                TransferCenter.shared.finish(Self.mapTransferId, success: false,
                                             message: "Map upload failed (\(code))")
            }
        case 0xD0:                                               // tile-list begin
            tileIdsBuilding = []
            tileRecordsBuilding = [:]
            tileListVersioned = DeviceTileList.beginHasVersions(d)
        case 0xD1 where d.count > 1:                              // comma-separated ids
            if let s = String(data: d.subdata(in: 1..<d.count), encoding: .utf8) {   // ("<id>=<record>" if versioned)
                DeviceTileList.parseEntries(s, ids: &tileIdsBuilding, records: &tileRecordsBuilding)
            }
        case 0xD2:                                               // tile-list end
            deviceKeepsVersions = tileListVersioned
            deviceTileRecords = tileRecordsBuilding
            deviceTileIds = Set(tileIdsBuilding)
            tileVersions.pruneTiles(deviceKey, keeping: deviceTileIds)
            cacheDeviceTiles()
        case 0xD3:                                               // poi-list begin
            poiIdsBuilding = []
            poiRecordsBuilding = [:]
            poiListVersioned = DeviceTileList.beginHasVersions(d)
        case 0xD4 where d.count > 1:                              // comma-separated ids
            if let s = String(data: d.subdata(in: 1..<d.count), encoding: .utf8) {
                DeviceTileList.parseEntries(s, ids: &poiIdsBuilding, records: &poiRecordsBuilding)
            }
        case 0xD5:                                               // poi-list end
            devicePoiRecords = poiListVersioned ? poiRecordsBuilding : [:]
            devicePoiIds = Set(poiIdsBuilding)
            tileVersions.prunePois(deviceKey, keeping: devicePoiIds)
            deviceSupportsPois = true
        case 0xE0 where d.count >= 3:                            // map-layer mask
            handleMapLayersNotify(UInt16(d[1]) | (UInt16(d[2]) << 8))
        case 0xC0: deviceMapsBuilding = []                        // map-list begin
        case 0xC1 where d.count >= 34:                            // entry: 4×f64 + flag
            func f64(_ i: Int) -> Double {
                var bits: UInt64 = 0
                for k in 0..<8 { bits |= UInt64(d[1 + i * 8 + k]) << (8 * k) }
                return Double(bitPattern: bits)
            }
            deviceMapsBuilding.append(DeviceMap(south: f64(0), west: f64(1),
                north: f64(2), east: f64(3), builtin: d[33] != 0))
        case 0xC2: deviceMaps = deviceMapsBuilding                // map-list end
        default: break                                            // 0xB4 progress: device-side
        }
    }

    private func handleSensorsNotify(_ d: Data) {
        guard let op = d.first else { return }
        switch op {
        case 0x10:                      // list begin
            sensorsBuilding = []
        case 0x11 where d.count >= 5:   // entry
            let mask = d[1]
            let flags = d[2]
            let rssi = Int(Int8(bitPattern: d[3]))
            let nameLen = Int(d[4])
            guard d.count >= 5 + nameLen else { return }
            let name = String(data: d.subdata(in: 5..<(5 + nameLen)), encoding: .utf8) ?? ""
            let addr = String(data: d.subdata(in: (5 + nameLen)..<d.count), encoding: .utf8) ?? ""
            guard !addr.isEmpty else { return }
            sensorsBuilding.append(BikeSensor(
                addr: addr, name: name.isEmpty ? addr : name, kindsMask: mask,
                connected: flags & 1 != 0, paired: flags & 2 != 0, rssi: rssi))
        case 0x12:                      // list end — publish
            sensors = sensorsBuilding.sorted {
                if $0.connected != $1.connected { return $0.connected }
                if $0.paired != $1.paired { return $0.paired }
                return $0.rssi > $1.rssi
            }
        default: break
        }
    }

    // MARK: - Meshtastic mesh
    //
    // The device is the node; this is its keyboard and screen. Protocol (both
    // directions) is documented in src/ble_server.cpp next to the opcodes.

    /// Ask for everything the Messages screen shows. Called when it appears and
    /// whenever the device says something changed.
    func refreshMesh() {
        guard let c = meshChar, let p = peripheral else { return }
        p.writeValue(Data([0x01]), for: c, type: .withResponse)   // state
        p.writeValue(Data([0x03]), for: c, type: .withResponse)   // history
        p.writeValue(Data([0x04]), for: c, type: .withResponse)   // nodes
        p.writeValue(Data([0x0c]), for: c, type: .withResponse)   // channels
        if meshPresets.isEmpty {
            // Fixed for the life of the firmware, so once is enough.
            p.writeValue(Data([0x0a]), for: c, type: .withResponse)
        }
        if meshRegions.isEmpty {
            // Likewise. Older firmware never answers, and the sheet then shows no
            // region control — which is right, since it has none to change.
            p.writeValue(Data([0x10]), for: c, type: .withResponse)
        }
    }

    /// Adds or replaces a private channel on the device. Slot 0 is the primary and
    /// is refused by the firmware — it decides the frequency.
    func setMeshPrivateChannel(index: UInt8, name: String, psk: Data) {
        guard let c = meshChar, let p = peripheral else { return }
        let n = Data(String(name.prefix(15)).utf8)
        var cmd = Data([0x0d, index, UInt8(n.count)])
        cmd.append(n)
        cmd.append(UInt8(psk.count))
        cmd.append(psk)
        p.writeValue(cmd, for: c, type: .withResponse)
    }

    /// Turns position sharing on or off for one channel. The device broadcasts on
    /// a distance and interval trigger, not continuously — see docs/meshtastic.md.
    func setMeshShareLocation(index: UInt8, on: Bool) {
        guard let c = meshChar, let p = peripheral else { return }
        p.writeValue(Data([0x0f, index, on ? 1 : 0]), for: c, type: .withResponse)
    }

    func forgetMeshChannel(index: UInt8) {
        guard let c = meshChar, let p = peripheral, index != 0 else { return }
        p.writeValue(Data([0x0e, index]), for: c, type: .withResponse)
    }

    /// The lowest free private slot, or nil when the device is full.
    var firstFreeMeshChannel: UInt8? {
        let used = Set(meshChannels.map(\.index))
        for i in UInt8(1)..<UInt8(8) where !used.contains(i) { return i }
        return nil
    }

    /// Switches the modem. Note this can move the frequency as well as the speed:
    /// bandwidth is part of a preset, and the channel's frequency slot is
    /// bandwidth-wide.
    func setMeshPreset(_ index: UInt8) {
        guard let c = meshChar, let p = peripheral else { return }
        p.writeValue(Data([0x0b, index]), for: c, type: .withResponse)
    }

    func requestMeshStats() {
        guard let c = meshChar, let p = peripheral else { return }
        p.writeValue(Data([0x09]), for: c, type: .withResponse)
    }

    /// Moves the radio to another regulatory band. The device retunes, drops the
    /// messages and neighbours it learned on the old band, and pushes fresh state.
    func setMeshRegion(_ index: UInt8) {
        guard let c = meshChar, let p = peripheral else { return }
        p.writeValue(Data([0x11, index]), for: c, type: .withResponse)
    }

    /// Transmit power in dBm; 0 asks for the most the region and radio allow.
    func setMeshTxPower(_ dbm: Int8) {
        guard let c = meshChar, let p = peripheral else { return }
        p.writeValue(Data([0x12, UInt8(bitPattern: dbm)]), for: c, type: .withResponse)
    }

    /// Pins the frequency slot (1-based); 0 lets the channel name choose again.
    func setMeshFreqSlot(_ slot: UInt8) {
        guard let c = meshChar, let p = peripheral else { return }
        p.writeValue(Data([0x13, slot]), for: c, type: .withResponse)
    }

    /// Sends a message to one node, or to the whole channel when `to` is nil.
    func sendMeshText(_ text: String, to: UInt32? = nil, channel: UInt8 = 0) {
        guard let c = meshChar, let p = peripheral else { return }
        let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return }
        // 200 bytes is the device's own cap. Truncated on a character boundary so
        // a multi-byte glyph is never cut in half into invalid UTF-8.
        var body = Data(trimmed.utf8)
        if body.count > 200 {
            var cut = trimmed
            while Data(cut.utf8).count > 200 { cut.removeLast() }
            body = Data(cut.utf8)
        }
        meshSendRejected = false
        var cmd = Data([0x02])
        cmd.appendLE(to ?? MeshState.broadcastAddr)
        cmd.append(channel)
        cmd.append(body)
        p.writeValue(cmd, for: c, type: .withResponse)
    }

    func setMeshEnabled(_ on: Bool) {
        guard let c = meshChar, let p = peripheral else { return }
        p.writeValue(Data([0x06, on ? 1 : 0]), for: c, type: .withResponse)
    }

    /// Renames this node on the mesh. `short` is what other apps show on a map
    /// pin, so it is kept to four characters as Meshtastic does.
    func setMeshNames(long: String, short: String) {
        guard let c = meshChar, let p = peripheral else { return }
        let l = Data(String(long.prefix(38)).utf8)
        let s = Data(String(short.prefix(4)).utf8)
        var cmd = Data([0x05, UInt8(l.count)])
        cmd.append(l)
        cmd.append(s)
        p.writeValue(cmd, for: c, type: .withResponse)
    }

    /// Moves the device to another channel. Note this retunes the radio — in
    /// Meshtastic the channel name decides the frequency slot as well as the key,
    /// so a device on "LongFast" and one on "MyTrail" cannot hear each other at all.
    /// An EMPTY name means "follow the modem preset" — the interoperable default,
    /// not a no-op: a stock node leaves its primary channel unnamed and derives the
    /// frequency from the preset, so pinning a name is what breaks interop.
    func setMeshChannel(name: String, key: UInt8) {
        guard let c = meshChar, let p = peripheral else { return }
        var cmd = Data([0x07, max(1, min(10, key))])
        cmd.append(Data(String(name.prefix(14)).utf8))
        p.writeValue(cmd, for: c, type: .withResponse)
    }

    func markMeshRead() {
        guard let c = meshChar, let p = peripheral else { return }
        p.writeValue(Data([0x08]), for: c, type: .withResponse)
    }

    /// Fills the mesh screens with a plausible conversation so the layout can be
    /// reviewed and screenshotted without a device and a second radio. Reached
    /// only via the `-demo-mesh` launch argument, like the other `-demo-*` flags.
    func seedDemoMesh() {
        // A flag of its own rather than forcing `state = .connected`: the central
        // manager's own callbacks own that field and overwrite it moments later
        // (on a simulator, straight to poweredOff), which left the demo showing
        // the not-connected screen over perfectly good seeded data.
        demoMesh = true
        // -demo-mesh-off shows the state a device comes up in now that the radio
        // defaults to off, which is the first thing a new rider sees.
        let off = ProcessInfo.processInfo.arguments.contains("-demo-mesh-off")
        meshState = MeshState(enabled: !off, radioOk: true,
                              channelFollowsPreset: true, nodeNum: 0xa4c1380c,
                              frequencyHz: 913_125_000, channel: "MediumFast",
                              channelKey: 1, longName: "OpenTrail 380c",
                              shortName: "380c", nodeCount: 3, unread: 0,
                              presetIndex: 2)
        let now = Date()
        meshNodes = [
            MeshNode(num: 0x7b2e91aa, shortName: "ALEX", longName: "Alex",
                     lastHeard: now.addingTimeInterval(-90), rssi: -84, snr: 6, hops: 0,
                     position: MeshNodePosition(latitude: 37.7955, longitude: -122.4380,
                                                altitudeM: 112, satsInView: 9,
                                                precisionBits: 32,
                                                received: now.addingTimeInterval(-120))),
            MeshNode(num: 0x3fd10c55, shortName: "SAM", longName: "Sam's T-Beam",
                     lastHeard: now.addingTimeInterval(-600), rssi: -108, snr: -4, hops: 2,
                     position: MeshNodePosition(latitude: 37.7699, longitude: -122.4103,
                                                altitudeM: 34, satsInView: 7,
                                                precisionBits: 13,
                                                received: now.addingTimeInterval(-900))),
            // No position: the common case, and the one the list has to say
            // something honest about.
            MeshNode(num: 0x11aa4402, shortName: "GATE", longName: "Trailhead Gate",
                     lastHeard: now.addingTimeInterval(-2400), rssi: -97, snr: 1, hops: 1,
                     position: nil),
        ]
        let bc = MeshState.broadcastAddr
        meshMessages = [
            MeshMessage(id: 1, from: 0x7b2e91aa, to: bc, outgoing: false, channel: 0,
                        status: .sent, text: "Heading up the fire road now, should be at the saddle in 40.",
                        date: now.addingTimeInterval(-2100), rssi: -84, snr: 6, hops: 0),
            MeshMessage(id: 2, from: 0xa4c1380c, to: bc, outgoing: true, channel: 0,
                        status: .sent, text: "Copy. I'm still at the creek crossing, water is high.",
                        date: now.addingTimeInterval(-1800), rssi: 0, snr: 0, hops: 0),
            MeshMessage(id: 3, from: 0x3fd10c55, to: bc, outgoing: false, channel: 1,
                        status: .sent, text: "Same, went around on the north side. Bridge is out.",
                        date: now.addingTimeInterval(-1500), rssi: -108, snr: -4, hops: 2),
            MeshMessage(id: 4, from: 0xa4c1380c, to: 0x7b2e91aa, outgoing: true, channel: 1,
                        status: .acked, text: "Taking the north detour, add 20 min.",
                        date: now.addingTimeInterval(-900), rssi: 0, snr: 0, hops: 0),
            MeshMessage(id: 5, from: 0x7b2e91aa, to: bc, outgoing: false, channel: 0,
                        status: .sent, text: "Got it. Waiting at the saddle, no rush.",
                        date: now.addingTimeInterval(-120), rssi: -84, snr: 6, hops: 0),
        ]
        meshStats = MeshStats(rx: 214, rxDropped: 11, rxOtherChannel: 63,
                              rxDuplicate: 88, tx: 19, txFailed: 1, acksRx: 12)
        meshChannels = [
            MeshChannel(index: 0, name: "MediumFast", hash: 0x1f, psk: Data([0x01]),
                        sharesLocation: false),
            MeshChannel(index: 1, name: "Saturday Ride", hash: 0x8c,
                        psk: Data((0..<32).map { UInt8($0 &* 7 &+ 3) }),
                        sharesLocation: true),
        ]
        meshPresets = [
            MeshPreset(index: 0, name: "LongFast", sf: 11, bandwidthKhz: 250, codingRate: 5),
            MeshPreset(index: 1, name: "MediumSlow", sf: 10, bandwidthKhz: 250, codingRate: 5),
            MeshPreset(index: 2, name: "MediumFast", sf: 9, bandwidthKhz: 250, codingRate: 5),
            MeshPreset(index: 3, name: "ShortSlow", sf: 8, bandwidthKhz: 250, codingRate: 5),
            MeshPreset(index: 4, name: "ShortFast", sf: 7, bandwidthKhz: 250, codingRate: 5),
            MeshPreset(index: 5, name: "ShortTurbo", sf: 7, bandwidthKhz: 500, codingRate: 5),
        ]
    }

    /// Straight-line distance from the phone to a coordinate, or nil when we have
    /// no location to measure from. Reads CoreLocation's cached fix rather than
    /// starting a stream: the mesh screens are not worth turning the GPS on for,
    /// and the device's own position is in the same pocket anyway.
    func distanceMeters(to coord: CLLocationCoordinate2D) -> Double? {
        guard locationAuthorized, let here = locationManager.location else { return nil }
        let there = CLLocation(latitude: coord.latitude, longitude: coord.longitude)
        return here.distance(from: there)
    }

    private func handleMeshNotify(_ d: Data) {
        guard let op = d.first else { return }
        switch op {
        case 0x90:  // state
            guard d.count >= 13 else { return }
            var s = MeshState()
            s.enabled = d[1] & 1 != 0
            s.radioOk = d[1] & 2 != 0
            s.channelFollowsPreset = d[1] & 4 != 0
            s.nodeNum = d.le32(at: 2)
            s.frequencyHz = d.le32(at: 6)
            s.channelKey = d[10]
            s.nodeCount = Int(d[11])
            s.unread = Int(d[12])
            var i = 13
            s.channel = d.lenString(at: &i)
            s.shortName = d.lenString(at: &i)
            s.longName = d.lenString(at: &i)
            if i < d.count { s.presetIndex = d[i] }
            // Radio config trails the preset; five bytes, all or nothing.
            if i + 5 < d.count {
                s.regionIndex = d[i + 1]
                s.txPowerDbm = Int8(bitPattern: d[i + 2])
                s.slotOverride = d[i + 3]
                s.slotCount = d[i + 4]
                s.activeSlot = d[i + 5]
            }
            meshState = s

        case 0x91:  // one message
            guard d.count >= 26 else { return }
            let textLen = Int(d[25])
            guard d.count >= 26 + textLen else { return }
            let flags = d[24]
            // The channel byte is appended after the text.
            let channel = d.count > 26 + textLen ? d[26 + textLen] : 0
            // The device sends both a UTC stamp and an age. Prefer the stamp —
            // it came from GPS and is exact — but it is 0 until the device has a
            // fix, and then the phone's clock minus the age is all there is.
            let utc = d.le32(at: 13)
            let age = TimeInterval(d.le32(at: 17)) / 1000
            let when = utc > 0 ? Date(timeIntervalSince1970: TimeInterval(utc))
                               : Date().addingTimeInterval(-age)
            meshBuilding.append(MeshMessage(
                id: d.le32(at: 1),
                from: d.le32(at: 5),
                to: d.le32(at: 9),
                outgoing: flags & 1 != 0,
                channel: channel,
                status: MeshMessage.Status(rawValue: flags >> 4) ?? .pending,
                text: String(data: d.subdata(in: 26..<(26 + textLen)),
                             encoding: .utf8) ?? "",
                date: when,
                rssi: Int(Int8(bitPattern: d[21])),
                snr: Int(Int8(bitPattern: d[22])),
                hops: Int(d[23])))

        case 0x92:  // end of history
            meshMessages = meshBuilding
            meshBuilding = []

        case 0x93:  // one node
            guard d.count >= 12 else { return }
            var i = 12
            let short = d.lenString(at: &i)
            let long = d.lenString(at: &i)
            let age = TimeInterval(d.le32(at: 5)) / 1000
            // Position is appended and optional: one flag byte, then the fields
            // only if the node has actually broadcast one.
            var pos: MeshNodePosition? = nil
            if i < d.count, d[i] != 0, d.count >= i + 1 + 16 {
                let base = i + 1
                let posAge = TimeInterval(d.le32(at: base + 12)) / 1000
                pos = MeshNodePosition(
                    latitude: Double(Int32(bitPattern: d.le32(at: base))) / 1e7,
                    longitude: Double(Int32(bitPattern: d.le32(at: base + 4))) / 1e7,
                    altitudeM: Int(Int16(bitPattern: UInt16(d[base + 8]) |
                                         (UInt16(d[base + 9]) << 8))),
                    satsInView: Int(d[base + 10]),
                    precisionBits: Int(d[base + 11]),
                    received: Date().addingTimeInterval(-posAge))
            }
            meshNodesBuilding.append(MeshNode(
                num: d.le32(at: 1),
                shortName: short,
                longName: long,
                lastHeard: Date().addingTimeInterval(-age),
                rssi: Int(Int8(bitPattern: d[10])),
                snr: Int(Int8(bitPattern: d[9])),
                hops: Int(d[11]),
                position: pos))

        case 0x94:  // end of node list — most recently heard first
            meshNodes = meshNodesBuilding.sorted { $0.lastHeard > $1.lastHeard }
            meshNodesBuilding = []

        case 0x95:  // packet counters
            guard d.count >= 29 else { return }
            meshStats = MeshStats(
                rx: Int(d.le32(at: 1)), rxDropped: Int(d.le32(at: 5)),
                rxOtherChannel: Int(d.le32(at: 9)), rxDuplicate: Int(d.le32(at: 13)),
                tx: Int(d.le32(at: 17)), txFailed: Int(d.le32(at: 21)),
                acksRx: Int(d.le32(at: 25)))

        case 0x99:  // one modem preset
            guard d.count >= 6 else { return }
            var i = 6
            let name = d.lenString(at: &i)
            let bw10 = Int(d[3]) | (Int(d[4]) << 8)
            meshPresetsBuilding.append(MeshPreset(
                index: d[1], name: name, sf: Int(d[2]),
                bandwidthKhz: Double(bw10) / 10, codingRate: Int(d[5])))

        case 0x9a:  // end of the preset list
            meshPresets = meshPresetsBuilding
            meshPresetsBuilding = []

        case 0x9d:  // one region
            guard d.count >= 15 else { return }
            var i = 15
            let name = d.lenString(at: &i)
            meshRegionsBuilding.append(MeshRegion(
                index: d[1], name: name,
                startHz: d.le32(at: 2), endHz: d.le32(at: 6), spacingHz: d.le32(at: 10),
                powerLimitDbm: Int8(bitPattern: d[14])))

        case 0x9e:  // end of the region list
            meshRegions = meshRegionsBuilding
            meshRegionsBuilding = []

        case 0x9b:  // one channel, with its key
            guard d.count >= 3 else { return }
            var i = 3
            let name = d.lenString(at: &i)
            guard i < d.count else { return }
            let pskLen = Int(d[i]); i += 1
            guard i + pskLen <= d.count else { return }
            let shares = d.count > i + pskLen ? d[i + pskLen] != 0 : false
            meshChannelsBuilding.append(MeshChannel(
                index: d[1], name: name, hash: d[2],
                psk: d.subdata(in: i..<(i + pskLen)),
                sharesLocation: shares))

        case 0x9c:  // end of the channel list
            meshChannels = meshChannelsBuilding.sorted { $0.index < $1.index }
            meshChannelsBuilding = []

        case 0x96:  // something changed — pull it
            refreshMesh()

        case 0x97:  // queued; the id its acks will quote
            meshSendRejected = false

        case 0x98:  // refused
            meshSendRejected = true

        default: break
        }
    }

    func downloadRide(_ name: String) {
        guard ridesChar != nil, peripheral != nil else {
            lastMessage = "Connect to your OpenTrailPaper to download this ride"
            return
        }
        enqueueTransfer(.ride(name))
    }

    // MARK: serialized transfer queue

    private func enqueueTransfer(_ t: Transfer) {
        // Tapping the same file twice queues it once.
        guard dlActive != t, !dlQueue.contains(t) else { return }
        dlQueue.append(t)
        startNextTransfer()
    }

    /// Sends the next queued request, but only when the line is free — the
    /// device streams one file at a time and we hold one receive buffer.
    private func startNextTransfer() {
        publishQueue()
        guard dlActive == nil else { return }
        guard !dlQueue.isEmpty else { runDeferredListRequests(); return }
        guard let c = ridesChar, let p = peripheral else {
            dlQueue = []          // link gone; nothing can be fetched
            publishQueue()
            return
        }
        let t = dlQueue.removeFirst()
        dlActive = t
        TransferCenter.shared.begin(t.transferId,
            kind: t.isLog ? .logDownload : .rideDownload,
            title: t.isLog ? "Downloading log" : "Downloading ride",
            detail: t.displayName, indeterminate: true)
        TransferCenter.shared.update(t.transferId, waiting: dlQueue.count)
        dlBuffer = Data()
        dlExpected = 0
        dlNextSeq = 0
        downloadProgress = 0
        downloadingLog = t.isLog
        downloadingName = t.displayName
        switch t {
        case .ride(let name):
            dlName = name
            downloadedFileURL = nil
            var cmd = Data([0x02])
            cmd.append(Data(name.utf8))
            p.writeValue(cmd, for: c, type: .withResponse)
        case .log(let name):
            dlName = name
            logFileURL = nil
            if name.isEmpty {
                p.writeValue(Data([0x05]), for: c, type: .withResponse)
            } else {
                var cmd = Data([0x07])
                cmd.append(Data(name.utf8))
                p.writeValue(cmd, for: c, type: .withResponse)
            }
        }
        publishQueue()
        armDownloadWatchdog()
    }

    /// The active transfer ended (either way) — release the line and run the
    /// next request. Always the single exit point, so the queue can't jam.
    private func endActiveTransfer() {
        // Every exit comes through here; a success has already been reported,
        // so this only lands for the failures (timeout, busy, short, dropped).
        if let t = dlActive {
            TransferCenter.shared.finish(t.transferId, success: false,
                                         message: lastMessage ?? "Download stopped")
        }
        dlWatchdog?.cancel()
        dlWatchdog = nil
        dlActive = nil
        downloadingName = nil
        downloadingLog = false
        dlBuffer = Data()          // don't hold a ride's bytes after it's saved
        dlExpected = 0
        startNextTransfer()
    }

    /// Abandon everything queued (device busy, disconnected) with one message
    /// rather than one failure toast per queued file.
    private func cancelAllTransfers(_ message: String?) {
        dlQueue = []
        if let message { lastMessage = message }
        endActiveTransfer()
    }

    private func publishQueue() {
        let names = dlQueue.map(\.displayName)
        if names != queuedDownloads { queuedDownloads = names }
        if let t = dlActive { TransferCenter.shared.update(t.transferId, waiting: dlQueue.count) }
    }

    /// Run whatever asked for the line while it was busy. Called once the queue
    /// is empty, so these never race a transfer for the device's request slot.
    private func runDeferredListRequests() {
        if listRefreshPending { listRefreshPending = false; refreshRides() }
        if logListPending { logListPending = false; requestLogList() }
    }

    /// A device that stops mid-stream (or never answers) must not strand the
    /// queue behind a transfer that will never finish.
    private func armDownloadWatchdog() {
        dlWatchdog?.cancel()
        dlLastActivity = Date()
        dlWatchdog = Task { @MainActor [weak self] in
            while !Task.isCancelled {
                try? await Task.sleep(nanoseconds: 2_000_000_000)
                guard !Task.isCancelled, let self, self.dlActive != nil else { return }
                if Date().timeIntervalSince(self.dlLastActivity) > 20 {
                    self.lastMessage = "\(self.dlActive?.displayName ?? "Transfer") timed out — try again"
                    self.endActiveTransfer()
                    return
                }
            }
        }
    }

    private func handleRidesNotify(_ d: Data) {
        guard let op = d.first else { return }
        switch op {
        case 0x01:  // list entry: [u32 size][name]
            guard d.count > 5 else { return }
            let size = Int(d[1]) | (Int(d[2]) << 8) | (Int(d[3]) << 16) | (Int(d[4]) << 24)
            let name = String(decoding: d[5...], as: UTF8.self)
            ridesBuilding.append(RideFile(name: name, size: size))
        case 0x03:  // list done
            loadingRides = false
            ridesCheckedThisLink = true
            let listed = ridesBuilding.sorted { $0.name > $1.name }   // newest first
            ridesBuilding = []
            // Unchanged list: leave `rides` alone so nothing re-renders.
            if listed != rides { rides = listed }
            rideListingStore.save(listed, for: settingsDeviceID)
        case 0x30:  // log-list entry: [u32 size][name]
            guard d.count > 5 else { return }
            let size = Int(d[1]) | (Int(d[2]) << 8) | (Int(d[3]) << 16) | (Int(d[4]) << 24)
            logsBuilding.append(LogFile(name: String(decoding: d[5...], as: UTF8.self), size: size))
        case 0x31:  // log-list done
            loadingLogs = false
            deviceLogs = logsBuilding.sorted { $0.name > $1.name }   // newest day first
        case 0x10:  // download start: [u32 total]
            // Ignore a stream we didn't ask for: after a timeout or a dropped
            // link the device can still be pushing an abandoned file, and those
            // bytes must not land in the next transfer's buffer.
            guard dlActive != nil, d.count > 4 else { return }
            dlLastActivity = Date()
            dlExpected = Int(d[1]) | (Int(d[2]) << 8) | (Int(d[3]) << 16) | (Int(d[4]) << 24)
            dlBuffer = Data(capacity: dlExpected)
            dlNextSeq = 0
            if let t = dlActive {
                TransferCenter.shared.update(t.transferId, completed: 0,
                                             total: Int64(dlExpected), indeterminate: false)
            }
        case 0x11:  // chunk: [u16 seq][payload]
            guard dlActive != nil, dlExpected > 0, d.count > 3 else { return }
            dlLastActivity = Date()
            let seq = UInt16(d[1]) | (UInt16(d[2]) << 8)
            // Strict in-order: only append the chunk we're expecting next.
            // Anything else (a duplicate from a resend, or a chunk that arrived
            // after a gap) is dropped; the device resends from dlNextSeq.
            if seq == dlNextSeq {
                dlBuffer.append(d[3...])
                dlNextSeq = seq &+ 1
                downloadProgress = min(1, Double(dlBuffer.count) / Double(dlExpected))
                if let t = dlActive {
                    TransferCenter.shared.update(t.transferId, completed: Int64(dlBuffer.count))
                }
            }
        case 0x14:  // window end — tell the device the next seq we need
            // Only ack a stream we're actually collecting — one we asked for AND
            // whose [0x10] header we saw. Acking anything else would answer an
            // abandoned transfer with our seq 0 and make the device resend that
            // whole file. Staying quiet lets its 5s ack timeout drop the stale
            // stream and pick up our request instead.
            guard dlActive != nil, dlExpected > 0 else { return }
            dlLastActivity = Date()
            var ack = Data([0x04])
            ack.append(UInt8(dlNextSeq & 0xFF))
            ack.append(UInt8(dlNextSeq >> 8))
            if let p = peripheral, let c = ridesChar {
                p.writeValue(ack, for: c, type: .withResponse)
            }
        case 0x12:  // done
            guard dlActive != nil else { return }
            finishDownload()
        case 0x13: break  // delete ack (already removed locally)
        case 0x1F:  // error (e.g. recording in progress)
            // Only surface a toast if the user was actively downloading; the
            // routine list-refresh fails silently mid-ride (the Rides tab shows
            // an "in progress" banner + the already-synced rides instead).
            loadingRides = false
            ridesCheckedThisLink = true
            guard dlActive != nil else { return }
            // A recording device refuses every file, so drop the whole queue
            // rather than failing each one in turn with its own toast.
            cancelAllTransfers("Device busy — stop the ride first")
        default: break
        }
    }

    private func handleRouteNotify(_ d: Data) {
        guard let op = d.first else { return }
        switch op {
        case 0x20: deviceRoutes.append(String(decoding: d[1...], as: UTF8.self))
        case 0x21: loadingRoutes = false; deviceRoutes.sort()
        case 0x22: break  // delete ack
        case 0x23:        // device received + parsed the route
            lastUploadProgress = nil
            routeSent = true
            routeReceived = true
        case 0x24:        // device got the upload but couldn't read the route
            lastUploadProgress = nil
            routeSent = false
            routeReceived = false
            lastMessage = "Device couldn’t read the route — try sending again"
        default: break
        }
    }

    // Downloaded rides are cached here so they never need re-downloading.
    static var ridesCacheDir: URL {
        let dir = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("rides", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir
    }
    static func cachedURL(for name: String) -> URL {
        ridesCacheDir.appendingPathComponent(name)
    }
    static func isCached(_ name: String) -> Bool {
        FileManager.default.fileExists(atPath: cachedURL(for: name).path)
    }
    /// What the Rides tab lists: the device's last-known ride list merged with
    /// the rides cached on this phone, one entry per name (the device's entry
    /// wins), newest first. A ride the device no longer lists stays while it's
    /// cached here. Pure, so the order is stable across refreshes.
    static func mergeRides(device: [RideFile], cached: [RideFile]) -> [RideFile] {
        var seen = Set<String>()
        var out: [RideFile] = []
        for r in device + cached where seen.insert(r.name).inserted { out.append(r) }
        return out.sorted { $0.name > $1.name }
    }

    // Rides already downloaded to this phone — available offline.
    static func cachedRides() -> [RideFile] {
        let urls = (try? FileManager.default.contentsOfDirectory(
            at: ridesCacheDir, includingPropertiesForKeys: [.fileSizeKey])) ?? []
        return urls
            .filter { $0.pathExtension.lowercased() == "fit" }
            .map { url in
                let size = (try? url.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0
                return RideFile(name: url.lastPathComponent, size: size)
            }
            .sorted { $0.name > $1.name }   // newest first
    }

    private func finishDownload() {
        // A short transfer means packets were lost, and a zero-length one means
        // no [0x10] header ever arrived. Either way the bytes in hand aren't the
        // file — never write them out, least of all under the requested name.
        if dlExpected == 0 || dlBuffer.count != dlExpected {
            lastMessage = dlExpected == 0
                ? "Transfer failed — try again"
                : "Transfer incomplete (\(dlBuffer.count)/\(dlExpected) bytes) — try again"
            endActiveTransfer()
            return
        }
        // Take the payload and the name BEFORE releasing the line — starting the
        // next queued transfer clears both.
        let payload = dlBuffer
        let wasLog = downloadingLog
        let name = wasLog ? (downloadingName ?? "diag") : dlName
        if let t = dlActive {
            TransferCenter.shared.finish(t.transferId, success: true,
                                         message: wasLog ? "Log downloaded" : "\(name) downloaded")
        }
        endActiveTransfer()

        if wasLog {                            // diagnostics log, not a ride
            var fname = name.replacingOccurrences(of: "/", with: "_")
            if !fname.hasSuffix(".log") { fname += ".log" }
            let url = FileManager.default.temporaryDirectory
                .appendingPathComponent("bikegps-\(fname)")
            do {
                try payload.write(to: url)
                logFileURL = url
            } catch {
                lastMessage = "Log save failed: \(error.localizedDescription)"
            }
            return
        }
        // Persist to the cache so it's available offline and never re-fetched.
        let url = BLEManager.cachedURL(for: name)
        do {
            try payload.write(to: url)
            downloadedFileURL = url
            lastMessage = "\(name) ready"
        } catch {
            lastMessage = "Save failed: \(error.localizedDescription)"
        }
    }

    // MARK: workouts (upload, control, live status — CHR_WORKOUT)

    private func workoutWrite(_ d: Data) {
        guard let c = workoutChar, let p = peripheral else { return }
        p.writeValue(d, for: c, type: .withResponse)
    }

    /// One snapshot of files + session state; also runs when notifications
    /// first come up so the Workouts screen opens populated.
    func refreshWorkouts() {
        workoutLog.info("refresh: char=\(self.workoutChar != nil) notifying=\(self.workoutChar?.isNotifying ?? false)")
        workoutWrite(Data([0x06]))
        workoutWrite(Data([0x30]))
    }

    /// The loaded workout's blocks for the WORKOUT page: the phone's copy if it
    /// has one, else the device's file (0x12, answered 0xD1…/0xD2).
    ///
    /// The status carries the device's title, not the file name: the device
    /// drops the extension and uppercases it ("sweet_spot.erg" reads
    /// "SWEET_SPOT"), so the file is found by matching that title against the
    /// phone's files and the device's /workouts list. If the list hasn't come
    /// yet it is asked for and the fetch resumes when it lands (0xB2).
    private func fetchLoadedWorkoutFile(_ title: String) {
        if let file = LocalWorkouts.list().first(where: { Self.workoutFile($0, isTitled: title) }),
           let text = LocalWorkouts.read(file) {
            loadedWorkoutFile = FetchedWorkout(name: title, text: text)
            return
        }
        guard workoutChar?.isNotifying == true, !workoutFetchForPage,
              workoutPageGaveUp != title else { return }
        guard let file = deviceWorkouts.first(where: { Self.workoutFile($0, isTitled: title) }) else {
            // Ask for the list once; if the title still matches nothing after
            // it, stop asking (the page shows its fallback, not a spinner).
            if workoutPageAwaitingList == title {
                workoutPageGaveUp = title
                workoutPageAwaitingList = ""
            } else {
                workoutPageAwaitingList = title
                workoutWrite(Data([0x06]))
            }
            return
        }
        workoutFetchForPage = true
        workoutFetchBuf = Data()
        workoutFetchName = title
        workoutWrite(Data([0x12]) + Data(file.utf8))
    }

    /// Whether `file` is the one the device titles `title` (its name without
    /// the extension, uppercased — workout_service's load()).
    static func workoutFile(_ file: String, isTitled title: String) -> Bool {
        let base = (file as NSString).deletingPathExtension
        return file.caseInsensitiveCompare(title) == .orderedSame
            || base.caseInsensitiveCompare(title) == .orderedSame
    }

    /// Just the session status (0x30 → 0xC0), for screens that show it.
    func requestWorkoutStatus() {
        guard workoutChar?.isNotifying == true else { return }
        workoutWrite(Data([0x30]))
    }

    /// Send an .erg/.mrc to the device; it saves to /workouts and loads it.
    func uploadWorkout(name: String, text: String) {
        guard let c = workoutChar, let p = peripheral else {
            workoutMessage = "Not connected"; return
        }
        let data = Data(text.utf8)
        let maxLen = max(20, p.maximumWriteValueLength(for: .withResponse)) - 1
        var packets: [Data] = [Data([0x01]) + Data(name.utf8).prefix(40)]
        var i = 0
        while i < data.count {
            let end = min(i + maxLen, data.count)
            packets.append(Data([0x02]) + data[i..<end])
            i = end
        }
        packets.append(Data([0x03]))
        Task { @MainActor in
            for packet in packets {
                p.writeValue(packet, for: c, type: .withResponse)
                try? await Task.sleep(nanoseconds: 12_000_000)
            }
            // The list refresh rides on the device's save ack (0xA0), so the
            // reply always includes the file this upload just created.
        }
    }

    func loadWorkout(_ name: String)   { workoutWrite(Data([0x11]) + Data(name.utf8)) }

    /// Pull a workout file off the device so the builder can edit it.
    func fetchWorkout(_ name: String) {
        workoutFetchBuf = Data()
        workoutFetchName = name
        fetchedWorkout = nil
        workoutWrite(Data([0x12]) + Data(name.utf8))
    }
    func deleteWorkout(_ name: String) {
        workoutWrite(Data([0x07]) + Data(name.utf8))
        workoutWrite(Data([0x06]))
    }
    func workoutStart()  { workoutWrite(Data([0x20])) }
    func workoutPause()  { workoutWrite(Data([0x21])) }
    func workoutResume() { workoutWrite(Data([0x22])) }
    func workoutStop()   { workoutWrite(Data([0x23])) }
    /// Stop AND unload — the device page returns to its no-workout state.
    func workoutUnload() { workoutWrite(Data([0x27])) }
    func workoutSkip()   { workoutWrite(Data([0x24])) }
    func workoutJump(block: Int) { workoutWrite(Data([0x25, UInt8(clamping: block)])) }
    func setWorkoutPauseEachBlock(_ on: Bool) {
        workoutStatus.pauseEachBlock = on   // answer locally; status confirms
        workoutWrite(Data([0x26, on ? 1 : 0]))
    }

    private func handleWorkoutNotify(_ d: Data) {
        guard let op = d.first else { return }
        let body = d.dropFirst()
        workoutLog.info("notify op=0x\(String(op, radix: 16), privacy: .public) len=\(d.count) loaded=\(op == 0xC0 && d.count > 1 ? Int(d[d.startIndex + 1]) : -1)")
        switch op {
        case 0xA0:   // ack: [ok/fail][msg]
            let ok = body.first == 1
            let msg = String(data: body.dropFirst(), encoding: .utf8) ?? ""
            // A failed page fetch ("file not found") answers here, not with
            // 0xD2 — without this the page waits on it forever. It's the
            // page's own background read, so no "Failed" message for it.
            if !ok, workoutFetchForPage {
                workoutFetchForPage = false
                workoutFetchBuf = Data()
                workoutPageGaveUp = workoutFetchName
                return
            }
            workoutMessage = ok ? "Loaded \(msg)" : "Failed: \(msg)"
            if ok { workoutWrite(Data([0x06])) }   // list now includes it
        case 0xB0: workoutListBuild = []
        case 0xB1:
            if let name = String(data: body, encoding: .utf8), !name.isEmpty {
                workoutListBuild.append(name)
            }
        case 0xB2:
            deviceWorkouts = workoutListBuild.sorted()
            if !workoutPageAwaitingList.isEmpty { fetchLoadedWorkoutFile(workoutPageAwaitingList) }
        case 0xD1: workoutFetchBuf.append(contentsOf: body)
        case 0xD2:
            if let text = String(data: workoutFetchBuf, encoding: .utf8) {
                let f = FetchedWorkout(name: workoutFetchName, text: text)
                if workoutFetchForPage { loadedWorkoutFile = f } else { fetchedWorkout = f }
            }
            workoutFetchForPage = false
            workoutFetchBuf = Data()
        case 0xC0:
            guard body.count >= 18 else { return }
            let b = Data(body)   // rebase indices at 0
            var s = WorkoutStatus()
            s.loaded = b[0] != 0
            s.running = b[1] != 0
            s.paused = b[2] != 0
            s.done = b[3] != 0
            s.blockIndex = Int(b[4])
            s.blockCount = Int(b[5])
            s.elapsedSec = b.subdata(in: 6..<10).withUnsafeBytes { $0.loadUnaligned(as: UInt32.self) }
            s.totalSec = b.subdata(in: 10..<14).withUnsafeBytes { $0.loadUnaligned(as: UInt32.self) }
            s.targetW = b.subdata(in: 14..<16).withUnsafeBytes { $0.loadUnaligned(as: UInt16.self) }
            s.ftpW = b.subdata(in: 16..<18).withUnsafeBytes { $0.loadUnaligned(as: UInt16.self) }
            if b.count > 18 { s.pauseEachBlock = b[18] != 0 }
            if b.count > 19 { s.name = String(data: b.subdata(in: 19..<b.count), encoding: .utf8) ?? "" }
            if s.loaded, !s.name.isEmpty, loadedWorkoutFile?.name != s.name {
                fetchLoadedWorkoutFile(s.name)
            }
            workoutLog.info("status loaded=\(s.loaded) running=\(s.running) block=\(s.blockIndex + 1)/\(s.blockCount) name=\(s.name, privacy: .public)")
            workoutStatus = s
        default: break
        }
    }

    // MARK: route upload (chunked with a 1-byte opcode per packet)

    static let routeTransferId = "ble.route"

    // One turn cue: where it happens + what to do.
    struct Maneuver { let lat: Double; let lon: Double; let text: String }

    func uploadRoute(name: String, gpx: String, maneuvers: [Maneuver] = []) {
        guard let c = routeChar, let p = peripheral else {
            lastMessage = "Not connected"
            return
        }
        let data = Data(gpx.utf8)
        let maxLen = max(20, p.maximumWriteValueLength(for: .withResponse)) - 1

        var packets: [Data] = []
        packets.append(Data([0x01]) + Data(name.utf8).prefix(38))   // start
        var i = 0
        while i < data.count {                                       // 0x02 data
            let end = min(i + maxLen, data.count)
            packets.append(Data([0x02]) + data[i..<end])
            i = end
        }
        packets.append(Data([0x03]))                                 // end track

        for m in maneuvers {                                         // 0x04 turns
            var pkt = Data([0x04])
            pkt.appendLE(Int32(m.lat * 1e7))
            pkt.appendLE(Int32(m.lon * 1e7))
            pkt.append(Data(m.text.utf8).prefix(maxLen - 8))
            packets.append(pkt)
        }
        packets.append(Data([0x05]))                                 // end nav

        lastUploadProgress = 0
        routeSent = false
        routeReceived = false
        let total = Int64(packets.reduce(0) { $0 + $1.count })
        TransferCenter.shared.begin(Self.routeTransferId, kind: .route, title: "Sending route",
                                    detail: name, total: total)
        Task { @MainActor in
            var sent: Int64 = 0
            for (idx, packet) in packets.enumerated() {
                p.writeValue(packet, for: c, type: .withResponse)
                lastUploadProgress = Double(idx + 1) / Double(packets.count)
                sent += Int64(packet.count)
                TransferCenter.shared.update(Self.routeTransferId, completed: sent)
                try? await Task.sleep(nanoseconds: 12_000_000)  // pace writes
            }
            lastUploadProgress = nil
            TransferCenter.shared.finish(Self.routeTransferId, success: true,
                                         message: "Route “\(name)” sent")
            // The writes are queued; the button now shows "Sent". Firmware ≥ this
            // release notifies back (0x23/0x24) so we can upgrade that to a real
            // "Received by device" confirmation — see handleRouteNotify.
            routeSent = true
            lastMessage = "Route “\(name)” sent — \(maneuvers.count) turns"
        }
    }
}

extension BLEManager: CBCentralManagerDelegate {
    /// State restoration: iOS relaunched the app for the head unit and hands
    /// back the peripheral it was holding. Called before DidUpdateState; the
    /// poweredOn scan then reconnects it (see startScan).
    nonisolated func centralManager(_ c: CBCentralManager, willRestoreState dict: [String: Any]) {
        MainActor.assumeIsolated {
            guard peripheral == nil,
                  let p = (dict[CBCentralManagerRestoredStatePeripheralsKey] as? [CBPeripheral])?.first
            else { return }
            peripheral = p
            p.delegate = self
            restoredPeripheral = true
        }
    }

    nonisolated func centralManagerDidUpdateState(_ c: CBCentralManager) {
        if isDemoUpdate || isDemoDash || isDemoLive { return }   // demo holds a fake connected state
        MainActor.assumeIsolated {
            bluetoothReady = (c.state == .poweredOn)
            bluetoothPoweredOn = (c.state == .poweredOn)
            // .unauthorized IS the refusal arriving; otherwise re-read, since
            // the answer to our prompt lands here.
            bluetoothPermission = c.state == .unauthorized
                ? .denied : Self.state(of: CBCentralManager.authorization)
            switch c.state {
            case .poweredOn: startScan()
            case .poweredOff: state = .poweredOff
            default: state = .idle
            }
        }
    }

    nonisolated func centralManager(_ c: CBCentralManager,
                                    didDiscover p: CBPeripheral,
                                    advertisementData: [String: Any],
                                    rssi: NSNumber) {
        let pairedFlag = BLEManager.advertisesPaired(advertisementData)
        MainActor.assumeIsolated {
            if let pairedFlag { advPaired[p.identifier] = pairedFlag }
            guard peripheral == nil else { return }
            if refusedIds.contains(p.identifier) {
                // Refused while paired, and now it says it's unpaired: the
                // rider unpaired it on the device. One automatic try — it
                // pairs with a code on the panel. Anything else waits for a tap.
                guard pairedFlag == false,
                      refusedWhilePaired.contains(p.identifier) else { return }
                refusedIds.remove(p.identifier)
                refusedWhilePaired.remove(p.identifier)
                pairingIssue = nil
            }
            if let saved = pairedDeviceId {
                // Fixed pairing: ours or nothing.
                guard p.identifier == saved else { return }
            } else {
                // Looking for a first device: never one that turned us away
                // this session, and not one that says it belongs to another
                // phone — unless nothing else turns up (armPairedFallback).
                if rejectedIds.contains(p.identifier) { return }
                if pairedFlag == true {
                    pairedElsewhereSeen[p.identifier] = p
                    return
                }
            }
            fallbackTask?.cancel()
            peripheral = p
            p.delegate = self
            state = .connecting
            c.stopScan()
            c.connect(p)
        }
    }

    nonisolated func centralManager(_ c: CBCentralManager,
                                    didConnect p: CBPeripheral) {
        MainActor.assumeIsolated {
            connectedAt = Date()
            authenticatedSinceConnect = false
            authFailedSinceConnect = false
            linkSecuring = true
            // Settings cache and offline edits are keyed by this identity.
            setCurrentDevice(p.identifier.uuidString)
            p.discoverServices([BikeUUID.service, LiveUUID.service])
        }
    }

    /// A connect that never came up. iOS reports stale pairing keys here too
    /// (the device was unpaired after iOS bonded): that is a refusal, and
    /// retrying would only repeat it. Anything else gets the same bounded
    /// treatment as a barren connect, a moment later.
    nonisolated func centralManager(_ c: CBCentralManager,
                                    didFailToConnect p: CBPeripheral,
                                    error: Error?) {
        MainActor.assumeIsolated {
            guard peripheral === p else { return }
            linkSecuring = false
            connectedAt = nil
            let code = (error as? CBError)?.code
            let stale = code == .peerRemovedPairingInformation || code == .encryptionTimedOut
            barrenConnects += 1
            if stale || barrenConnects >= 3 {
                refuse(p, iosKeysStale: stale)
                peripheral = nil
                startScan()
                return
            }
            state = .connecting
            DispatchQueue.main.asyncAfter(deadline: .now() + 2) { [weak self] in
                guard let self, self.peripheral === p, p.state == .disconnected else { return }
                c.connect(p)
            }
        }
    }

    nonisolated func centralManager(_ c: CBCentralManager,
                                    didDisconnectPeripheral p: CBPeripheral,
                                    error: Error?) {
        MainActor.assumeIsolated {
            settingsChar = nil; statusChar = nil; routeChar = nil; ridesChar = nil
            sensorsChar = nil; mapChar = nil; otaChar = nil; meshChar = nil
            dashChar = nil
            liveRxChar = nil; liveTxChar = nil
            live.clear()
            updateLiveStream()   // stops the keepalive
            // The workout characteristic belongs to this link too; a stale one
            // swallowed writes after a reconnect. The session state is the
            // device's, so it is asked for again rather than shown stale.
            workoutChar = nil
            workoutStatus = WorkoutStatus()
            workoutFetchForPage = false
            workoutPageAwaitingList = ""; workoutPageGaveUp = ""
            // Pending edits stay cached; the next connect reads and merges.
            resetSyncLinkState()
            mediaChar = nil; mediaToneArt = false; mediaRemote = false   // the next device may be older firmware
            mediaNotifySeen = false; mediaCapsAsked = false; mediaSubscribeRetries = 0
            mediaAcks = []; artPumping = false; artPayload = Data()
            albumArt.linkDown()
            updateAlbumArt()
            // A half-received mesh stream must not be published on reconnect.
            meshBuilding = []; meshNodesBuilding = []; meshPresetsBuilding = []
            meshChannelsBuilding = []
            stopLocationStream()   // no device to send the phone's position to
            // If we disconnect mid-update: after the data is sent + commit
            // requested, a disconnect is EXPECTED (the device reboots into the
            // new firmware) — show "rebooting" and let the reconnect confirm the
            // version. If we drop mid-download, the transfer failed.
            if otaInProgress {
                keepAwake(false)
                if otaCommitSent {
                    // Expected: the device reboots to flash. Wait for it to come
                    // back and confirm the version; the watchdog covers a no-show.
                    otaPhase = .installing
                    otaMessage = "Installing — the device is restarting…"
                    armOtaWatchdog(seconds: 150, failMessage: BLEManager.installWatchdogMsg)
                } else {
                    // Dropped mid-transfer — nothing was flashed.
                    otaFinish(.failed, "Connection dropped mid-transfer. The device is unchanged — try again.")
                }
            }
            if mapUploading { mapUploading = false; keepAwake(false)
                              mapMessage = "Upload interrupted — try again" }
            TransferCenter.shared.finish(Self.mapTransferId, success: false,
                                         message: "Connection dropped")
            TransferCenter.shared.finish(Self.routeTransferId, success: false,
                                         message: "Connection dropped")
            deviceSupportsPois = false      // re-learnt from the next 0x08 answer
            deviceKeepsVersions = false     // and this from the next versioned list
            if tilesUploading {
                tileQueue = []
                finishTileJob(message: "Interrupted — reconnect to resume")
            }
            status = DeviceStatus()
            let admitted = authenticatedSinceConnect || sawStatusSinceConnect
            // Was this the device turning us away? It drops a phone it won't
            // have before any protected data: at once when it's paired and we
            // aren't its phone, or when our pairing request arrives after iOS
            // forgot it. Signals, strongest first: iOS saying the keys are
            // gone; a protected request failing for lack of authentication;
            // a quick drop from a device that advertises "paired"; and, for a
            // device reached without seeing its advertisement, three barren
            // connects in a row (so a reboot or a range drop doesn't cry wolf).
            // OUR device advertises "paired" too — to us — so for it a single
            // quick drop or a mid-handshake auth error is not enough: a radio
            // drop mid-ride must keep reconnecting. It takes the stale-keys
            // error or three barren connects.
            var refused = false
            var iosKeysStale = false
            if !admitted {
                barrenConnects += 1
                let code = (error as? CBError)?.code
                iosKeysStale = code == .peerRemovedPairingInformation || code == .encryptionTimedOut
                let quick = connectedAt.map { -$0.timeIntervalSinceNow < 8 } ?? false
                let ours = pairedDeviceId == p.identifier
                refused = iosKeysStale ||
                    (!ours && authFailedSinceConnect) ||
                    (!ours && quick && advPaired[p.identifier] == true) ||
                    (barrenConnects >= 3 && !onFallbackAttempt)
            } else {
                barrenConnects = 0
            }
            sawStatusSinceConnect = false
            authenticatedSinceConnect = false
            authFailedSinceConnect = false
            linkSecuring = false
            connectedAt = nil
            // Keep `rides` (the last-known list): the Rides tab keeps showing
            // it while disconnected and through the re-check on reconnect.
            ridesBuilding = []; loadingRides = false; ridesCheckedThisLink = false
            // Nothing queued can proceed without a link, and a half-received
            // file must not be mistaken for a complete one on reconnect.
            if dlActive != nil || !dlQueue.isEmpty {
                cancelAllTransfers("Connection dropped mid-download — try again")
            }
            deviceRoutes = []; loadingRoutes = false
            lastUploadProgress = nil; routeSent = false; routeReceived = false
            sensors = []; scanningSensors = false

            // Hand the peripheral straight back to CoreBluetooth instead of
            // dropping it and re-scanning. connect() on a known peripheral has no
            // timeout — CB reconnects the instant the device is reachable again,
            // without a scan/discover/connect round trip. Re-scanning meant every
            // brief drop cost seconds and a visible "connecting…" flap, which is
            // what made the link look like it was cycling in and out while the
            // app was foregrounded, and what made log downloads and map sends
            // fail: they abort on disconnect and the retry raced the rescan.
            //
            // Except when the device is not (or no longer) ours to reconnect to:
            // forgetDevice() dropped it, it was dropped already (peripheral nil),
            // or it was the one-off try at a paired-elsewhere device and it
            // turned us away — then say so and go back to looking.
            if forgetting || peripheral == nil {
                forgetting = false
                peripheral = nil
                startScan()
            } else if refused {
                // No reconnect: that would only be refused again (and older
                // firmware answered with a pairing dialog — the loop).
                onFallbackAttempt = false
                refuse(p, iosKeysStale: iosKeysStale)
                peripheral = nil
                startScan()   // listens for it to unpair; never connects to it
            } else if onFallbackAttempt && !admitted {
                onFallbackAttempt = false
                pairingIssue = .pairedElsewhere
                peripheral = nil
                startScan()
            } else {
                onFallbackAttempt = false
                peripheral = p
                p.delegate = self
                state = .connecting
                c.connect(p)
            }
        }
    }
}

extension BLEManager: CBPeripheralDelegate {
    /// First protected value of this connection: the link is authenticated
    /// and the device admitted us. Only now is it "connected".
    private func markAuthenticated() {
        authenticatedSinceConnect = true
        linkSecuring = false
        barrenConnects = 0
        state = .connected
        rememberPairedDevice()
        updateLiveStream()   // the Ride tab may be waiting for the link
        // The workout list/status asked for when notifications came up can be
        // refused (the characteristic is WRITE_ENC) if that raced the link's
        // encryption, and nothing asked again: a workout loaded on the device
        // stayed invisible until the Workouts sheet was opened. Ask once the
        // link is known to be secure.
        if workoutChar?.isNotifying == true { refreshWorkouts() }
    }

    /// A protected request refused for lack of authentication/encryption:
    /// pairing was cancelled, failed, or the device won't pair with us. Acted
    /// on when the link drops (didDisconnect), not here — iOS answers these
    /// errors by pairing, and the first pairing on an open device must be
    /// allowed to finish.
    private func noteAuthError(_ error: Error?) {
        guard let error = error as? CBATTError, !authenticatedSinceConnect else { return }
        switch error.code {
        case .insufficientAuthentication, .insufficientEncryption,
             .insufficientEncryptionKeySize, .insufficientAuthorization:
            authFailedSinceConnect = true
        default: break
        }
    }

    nonisolated func peripheral(_ p: CBPeripheral, didDiscoverServices error: Error?) {
        // Discover ALL characteristics (nil) — an explicit list previously
        // omitted the sensors + map characteristics, so those features never
        // worked. nil is also future-proof as new characteristics are added.
        for s in p.services ?? [] where s.uuid == BikeUUID.service {
            p.discoverCharacteristics(nil, for: s)
        }
        // The live state stream's own service; absent on older firmware.
        for s in p.services ?? [] where s.uuid == LiveUUID.service {
            p.discoverCharacteristics([LiveUUID.rx, LiveUUID.tx], for: s)
        }
    }

    nonisolated func peripheral(_ p: CBPeripheral,
                                didDiscoverCharacteristicsFor s: CBService,
                                error: Error?) {
        MainActor.assumeIsolated {
            // Again here (idempotent): a link iOS restored on relaunch reaches
            // discovery without a didConnect.
            setCurrentDevice(p.identifier.uuidString)
            for ch in s.characteristics ?? [] {
                switch ch.uuid {
                case BikeUUID.settings:
                    settingsChar = ch
                    p.readValue(for: ch)
                    p.setNotifyValue(true, for: ch)   // live device-side edits

                case BikeUUID.dash:
                    dashChar = ch
                    p.readValue(for: ch)              // pull the device's layout
                    p.setNotifyValue(true, for: ch)

                case BikeUUID.status:
                    statusChar = ch; p.setNotifyValue(true, for: ch)
                case BikeUUID.route:
                    routeChar = ch; p.setNotifyValue(true, for: ch)
                    startLocationStream()   // warm-start + live fallback position
                case BikeUUID.rides:
                    ridesChar = ch; p.setNotifyValue(true, for: ch)
                case BikeUUID.ota:
                    otaChar = ch; p.setNotifyValue(true, for: ch)
                case BikeUUID.sensors:
                    sensorsChar = ch; p.setNotifyValue(true, for: ch)
                case BikeUUID.map:
                    mapChar = ch; p.setNotifyValue(true, for: ch)
                    loadMapLayersState()
                case BikeUUID.mesh:
                    meshChar = ch; p.setNotifyValue(true, for: ch)
                case BikeUUID.workout:
                    workoutChar = ch; p.setNotifyValue(true, for: ch)
                case BikeUUID.media:
                    artLog.info("media char found: props=0x\(String(ch.properties.rawValue, radix: 16), privacy: .public) descriptors=\(ch.descriptors?.count ?? -1)")
                    mediaChar = ch; p.setNotifyValue(true, for: ch)
                // Not subscribed here: the stream runs only while the Ride
                // tab is up (updateLiveStream).
                case LiveUUID.rx:
                    liveRxChar = ch
                case LiveUUID.tx:
                    liveTxChar = ch
                default: break
                }
            }
            if s.uuid == LiveUUID.service { updateLiveStream() }
            // Not .connected yet: that waits for the first protected read to
            // come back (markAuthenticated). Until then iOS may be pairing.
        }
    }

    // The version reply comes back as a notification, so only ask once the OTA
    // characteristic's notifications are actually enabled (otherwise the reply
    // is dropped and the app never learns the device firmware version).
    nonisolated func peripheral(_ p: CBPeripheral,
                                didUpdateNotificationStateFor ch: CBCharacteristic,
                                error: Error?) {
        MainActor.assumeIsolated {
            noteAuthError(error)
            if ch.uuid == BikeUUID.ota { queryDeviceFirmware() }
            if ch.uuid == LiveUUID.tx {
                if ch.isNotifying { sendLiveStart() } else { live.clear() }
            }
            if ch.uuid == BikeUUID.sensors, let p = peripheral {
                p.writeValue(Data([0x05]), for: ch, type: .withResponse)   // one snapshot
            }
            // Same reason as the OTA version query: the device answers by
            // notification, so asking before notifications are on throws the
            // reply away and the Messages tab sits empty until it is opened.
            if ch.uuid == BikeUUID.mesh { refreshMesh() }
            // Map layers: firmware without them never answers, which is what
            // keeps the app's switches hidden for it.
            if ch.uuid == BikeUUID.map { requestMapLayers() }
            if ch.uuid == BikeUUID.workout {
                workoutLog.info("notify state: \(ch.isNotifying) error=\(error?.localizedDescription ?? "none", privacy: .public)")
                refreshWorkouts()
            }
            // Which art format the device takes (and whether it forwards AMS
            // tracks); the answer is a notification.
            if ch.uuid == BikeUUID.media {
                artLog.info("media notify state: \(ch.isNotifying) error=\(error?.localizedDescription ?? "none", privacy: .public)")
                // Seen on a relaunch while iOS kept the link up (it does, for
                // AMS): the device intermittently refuses the subscribe with
                // "request not supported". Without it no AMS track ever
                // reaches the app, so no art. A second try goes through.
                if error != nil, !ch.isNotifying, mediaSubscribeRetries < 4,
                   let p = peripheral {
                    mediaSubscribeRetries += 1
                    DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) { [weak self] in
                        guard let self, self.mediaChar === ch, !ch.isNotifying else { return }
                        p.setNotifyValue(true, for: ch)
                    }
                }
            }
            if ch.uuid == BikeUUID.media, ch.isNotifying {
                askMediaCaps()
                updateAlbumArt()
            }
        }
    }

    // CoreBluetooth's send queue drained — push more firmware / map chunks.
    nonisolated func peripheralIsReady(toSendWriteWithoutResponse p: CBPeripheral) {
        MainActor.assumeIsolated {
            pumpOtaChunks()
            pumpMapChunks()
            pumpArtChunks()
        }
    }

    nonisolated func peripheral(_ p: CBPeripheral,
                                didWriteValueFor ch: CBCharacteristic,
                                error: Error?) {
        if let error, ch.uuid == BikeUUID.workout {
            workoutLog.error("workout write failed: \(error.localizedDescription, privacy: .public)")
        }
        if let error {
            artLog.error("write to \(ch.uuid.uuidString, privacy: .public) failed: \(error.localizedDescription, privacy: .public)")
            MainActor.assumeIsolated { noteAuthError(error) }
        }
        if ch.uuid == BikeUUID.settings {
            // Settings edits and Unpair ([0xFE 0x55]) both write here; writes
            // with response are acknowledged in order, so an edit already in
            // flight gets the first acknowledgment.
            MainActor.assumeIsolated {
                if settingsWriteInFlight != nil { settingsWriteAcked(ok: error == nil) }
                else if unpairWritePending { unpairDone(ok: error == nil) }
            }
            return
        }
        if ch.uuid == BikeUUID.dash {
            MainActor.assumeIsolated { dashWriteAcked(ok: error == nil) }
            return
        }
        guard ch.uuid == BikeUUID.media else { return }
        MainActor.assumeIsolated { mediaWriteAcked(failed: error != nil) }
    }

    nonisolated func peripheral(_ p: CBPeripheral,
                                didUpdateValueFor ch: CBCharacteristic,
                                error: Error?) {
        if let error {
            MainActor.assumeIsolated { noteAuthError(error) }
            return
        }
        guard let data = ch.value else { return }
        MainActor.assumeIsolated {
            // Every characteristic on the device needs an authenticated link,
            // so any value arriving proves one.
            if !authenticatedSinceConnect { markAuthenticated() }
            switch ch.uuid {
            case BikeUUID.status: parseStatus(data)
            case BikeUUID.settings: parseSettings(data)
            case BikeUUID.rides: handleRidesNotify(data)
            case BikeUUID.route: handleRouteNotify(data)
            case BikeUUID.ota: handleOtaNotify(data)
            case BikeUUID.sensors: handleSensorsNotify(data)
            case BikeUUID.map: handleMapNotify(data)
            case BikeUUID.dash: parseDashLayout(data)
            case BikeUUID.mesh: handleMeshNotify(data)
            case BikeUUID.workout: handleWorkoutNotify(data)
            case BikeUUID.media: handleMediaNotify(data)
            case LiveUUID.tx: live.ingest(data)
            default: break
            }
        }
    }

    // MARK: - Album art (the device's MUSIC page)
    //
    // CHR_MEDIA, phone -> device: [0x10 w h] 8-bit art begin or [0x13 w h]
    // tone-art begin, [0x11]+bytes, [0x12] end, [0x03] caps? Device -> phone:
    // [0xA1][caps] (bit0 tone art), [0xA2][title\0artist\0album\0] the track
    // AMS reports (see src/ble_server.cpp). The iPhone sends no metadata:
    // the device gets that for every player from iOS itself (AMS).

    /// Art only matters to a dashboard with a music page.
    private func updateAlbumArt() {
        let notifying = mediaChar != nil && (mediaChar?.isNotifying == true || mediaNotifySeen)
        artLog.info("art active? notifying=\(notifying) musicPage=\(self.dashConfig?.hasMusicPage == true)")
        albumArt.setActive(notifying && dashConfig?.hasMusicPage == true)
    }

    /// Which art format the device takes, and whether it forwards AMS tracks;
    /// answered by notification. Once per link.
    private func askMediaCaps() {
        guard !mediaCapsAsked, mediaChar != nil else { return }
        mediaCapsAsked = true
        writeMedia(Data([0x03]), ack: .other)
    }

    private func handleMediaNotify(_ d: Data) {
        // A notification arriving proves notifications are on, whatever the
        // state callback said (or whether it came at all).
        if !mediaNotifySeen {
            mediaNotifySeen = true
            askMediaCaps()
            updateAlbumArt()
        }
        guard d.count >= 2 else { return }
        switch d[d.startIndex] {
        case 0xA1:
            mediaToneArt = d[d.startIndex + 1] & 0x01 != 0
            mediaRemote = d[d.startIndex + 1] & 0x04 != 0
            artLog.info("device caps 0x\(String(d[d.startIndex + 1], radix: 16), privacy: .public)")
        case 0xA2: if let t = MediaTrack(packet: d) { albumArt.deviceTrackChanged(t) }
        default: break   // 0xA0 transport commands: the device uses AMS on iPhone
        }
    }

    /// Play/pause, next or previous for whatever is playing: the device sends
    /// it to iOS over AMS (media_state.h MediaCmd: 1 toggle, 2 next, 3 prev).
    func mediaCommand(_ cmd: UInt8) {
        guard mediaRemote else { return }
        writeMedia(Data([0x04, cmd]), ack: .other)
    }

    private func writeMedia(_ pkt: Data, ack: MediaAck) {
        guard let c = mediaChar, let p = peripheral else { return }
        mediaAcks.append(ack)
        p.writeValue(pkt, for: c, type: .withResponse)
    }

    /// Streamed: begin ([0x13] tone art the device unpacks, or [0x10] 8-bit
    /// grayscale for firmware that predates it), chunks without response as
    /// fast as CoreBluetooth takes them, then [0x12]. iOS has no connection-
    /// priority call; the firmware moves the link to its fast interval itself
    /// while art writes flow (noteBulk). A newer cover simply restarts: the
    /// device drops a half-received one on the next begin.
    func sendMediaArt(_ gray: [UInt8], width: Int, height: Int) -> Bool {
        guard let p = peripheral, mediaChar != nil, !otaInProgress else { return false }
        let tones = mediaToneArt
        artPayload = tones
            ? Data(ArtDither.pack(ArtDither.dither(gray, width: width, height: height)))
            : Data(gray)
        artOffset = 0
        artPumping = false
        artGen += 1
        artChunk = max(20, p.maximumWriteValueLength(for: .withoutResponse)) - 1
        var begin = Data([tones ? 0x13 : 0x10])
        begin.appendLE(UInt16(width))
        begin.appendLE(UInt16(height))
        writeMedia(begin, ack: .artBegin(artGen))
        return true
    }

    private func mediaWriteAcked(failed: Bool) {
        guard !mediaAcks.isEmpty else { return }
        switch mediaAcks.removeFirst() {
        case .artBegin(let gen) where gen == artGen:
            if failed { artPayload = Data(); return }
            artPumping = true
            pumpArtChunks()
        default: break
        }
    }

    private func pumpArtChunks() {
        guard artPumping, let c = mediaChar, let p = peripheral else { return }
        while artOffset < artPayload.count {
            guard p.canSendWriteWithoutResponse else { return }   // resume from ready
            let end = min(artOffset + artChunk, artPayload.count)
            var pkt = Data([0x11])
            pkt.append(artPayload.subdata(in: artOffset..<end))
            artOffset = end
            p.writeValue(pkt, for: c, type: .withoutResponse)
        }
        artPumping = false
        artPayload = Data()
        writeMedia(Data([0x12]), ack: .artEnd(artGen))
    }

    // MARK: - Dashboard layout

    private func parseDashLayout(_ d: Data) {
        // A 1-byte 0x01 is the device saying "changed, too big for a notify —
        // read me": a notification truncates at MTU-3 bytes, and adopting a
        // truncated multi-page config is how the editor once wiped itself.
        if d.count == 1, d[0] == 0x01 {
            if let ch = dashChar { peripheral?.readValue(for: ch) }
            return
        }
        guard let text = String(data: d, encoding: .utf8) else { return }
        let cfg = DashConfig(text: text)
        dashConfig = cfg
        dashSeenThisLink = true
        settingsCache.dashText = cfg.configText
        saveSettingsCache()
        reconcileDash()
    }

    /// The editor's Save/Send: record the layout as pending for this device
    /// and push it when the link allows. Pending until the device acks it.
    func saveDashConfig(_ config: DashConfig) {
        settingsCache.pendingDash = DashSync.saving(
            config.configText, deviceText: dashConfig?.configText,
            over: settingsCache.pendingDash, at: Date())
        dashWriteFailedThisLink = false
        saveSettingsCache()
        applyEffectiveSettings()
        reconcileDash()
    }

    /// Settle the offline layout against the device's: push it if the device
    /// is unchanged since the edit started, drop it if they already match,
    /// ask the rider if both changed.
    private func reconcileDash() {
        guard dashSeenThisLink, dashWritesOutstanding == 0, !dashWriteFailedThisLink,
              dashConflict == nil, let device = dashConfig else { return }
        let pending = settingsCache.pendingDash
        switch DashSync.resolve(device: device.configText, pending: pending) {
        case .adoptDevice:
            break
        case .alreadyInSync:
            settingsCache.pendingDash = nil
            saveSettingsCache()
            applyEffectiveSettings()
        case .pushPhone:
            if let p = pending { writeDash(p.text) }
        case .conflict:
            if let p = pending {
                dashConflict = DashConflict(phone: DashConfig(text: p.text), device: device,
                                            editedAt: p.editedAt)
            }
        }
    }

    /// The rider's answer to a layout conflict.
    func resolveDashConflict(keepPhone: Bool) {
        guard dashConflict != nil else { return }
        dashConflict = nil
        if keepPhone, var p = settingsCache.pendingDash {
            p.baseText = dashConfig?.configText   // goes over the device's current one
            settingsCache.pendingDash = p
        } else {
            settingsCache.pendingDash = nil
        }
        saveSettingsCache()
        applyEffectiveSettings()
        reconcileDash()
    }

    /// Push a layout to the device. It writes the file, applies it to the panel,
    /// and notifies back what it actually stored — so a rejected layout corrects
    /// the editor instead of leaving it out of step.
    private func writeDash(_ text: String) {
        guard let ch = dashChar, let p = peripheral else { return }
        guard let data = text.data(using: .utf8) else { return }
        // Streamed ([0x01] begin, [0x02]+bytes, [0x03] commit): a multi-page
        // config outgrows the 512-byte ceiling iOS puts on a single write.
        let chunk = max(20, p.maximumWriteValueLength(for: .withResponse)) - 1
        var packets = [Data([0x01])]
        var off = 0
        while off < data.count {
            let n = min(chunk, data.count - off)
            var pkt = Data([0x02])
            pkt.append(data.subdata(in: off..<off + n))
            packets.append(pkt)
            off += n
        }
        packets.append(Data([0x03]))
        // Every packet is with-response and acked in order; the layout counts
        // as delivered once the last ack lands with no error on the way.
        dashWritesOutstanding = packets.count
        dashWriteError = false
        dashWriteText = text
        for pkt in packets { p.writeValue(pkt, for: ch, type: .withResponse) }
    }

    private func dashWriteAcked(ok: Bool) {
        guard dashWritesOutstanding > 0 else { return }
        if !ok { dashWriteError = true }
        dashWritesOutstanding -= 1
        guard dashWritesOutstanding == 0, let text = dashWriteText else { return }
        dashWriteText = nil
        if dashWriteError {
            dashWriteFailedThisLink = true   // stays pending; the next connect retries
            return
        }
        if var p = settingsCache.pendingDash {
            if p.text == text {
                settingsCache.pendingDash = nil
            } else {
                p.baseText = text   // edited again mid-write: that goes next
                settingsCache.pendingDash = p
            }
        }
        // The device notifies what it actually stored; until then it holds this.
        if dashConfig?.configText != text { dashConfig = DashConfig(text: text) }
        settingsCache.dashText = text
        saveSettingsCache()
        applyEffectiveSettings()
        reconcileDash()
    }

    private func parseStatus(_ d: Data) {
        guard d.count >= 10 else { return }
        var s = DeviceStatus()
        let flags = d[0]
        s.gpsFix = flags & 1 != 0
        s.recording = flags & 2 != 0
        s.hasRoute = flags & 4 != 0
        s.battery = Int(d[1])
        s.sats = Int(d[2])
        s.heartRate = d[3] == 0xFF ? nil : Int(d[3])
        let pw = Int(d[4]) | (Int(d[5]) << 8)
        s.power = pw == 0 ? nil : pw
        s.speedKmh = Double(Int(d[6]) | (Int(d[7]) << 8)) / 10.0
        s.remainingKm = Double(Int(d[8]) | (Int(d[9]) << 8)) / 10.0
        status = s
        syncLocationStreamToDeviceFix()
        updateBackgroundLocationMode()   // background GPS only while recording
    }

    // Only run iOS location (and the blue background indicator) while the device
    // NEEDS it. Once the device has its own GPS fix, stop streaming; resume if it
    // loses the fix. Debounced so a brief dropout doesn't flap the stream.
    private func syncLocationStreamToDeviceFix() {
        guard state == .connected, routeChar != nil else { return }
        let firstStatus = !sawStatusSinceConnect
        sawStatusSinceConnect = true
        barrenConnects = 0   // a working link ends the stale-pairing streak
        if firstStatus { rememberPairedDevice() }   // the device admitted us: ours
        if status.gpsFix {
            // The device was ALREADY locked when we connected. We start the
            // stream the moment the route characteristic shows up, before the
            // device has told us anything about itself, so this is the first
            // chance to know it was never needed — drop it now rather than
            // running the phone's GPS through the debounce window. That window
            // exists to ride out a DROPOUT mid-ride, not to delay this.
            if firstStatus {
                fixStableTask?.cancel(); fixStableTask = nil
                if wantsAiding { stopLocationStream() }
                return
            }
            if wantsAiding && fixStableTask == nil {
                fixStableTask = Task { @MainActor in
                    try? await Task.sleep(nanoseconds: 8_000_000_000)   // fix held 8 s
                    if !Task.isCancelled, status.gpsFix { stopLocationStream() }
                    fixStableTask = nil
                }
            }
        } else {
            fixStableTask?.cancel(); fixStableTask = nil
            if !wantsAiding { startLocationStream() }   // device is cold again — help it
        }
    }

    // Values pushed from the device (on connect, or when edited on the unit).
    // They become the cache's device values — never echoed back — and then
    // any pending app edits are merged on top and pushed (syncSettings).
    private func parseSettings(_ d: Data) {
        guard let snap = DeviceSettingsValues.decode(d) else { return }
        let before = settingsCache.device
        if !settingsCache.pending.isEmpty {
            // The device keeps no change time, so an offline edit wins even
            // over a newer change made on the device; say so rather than let
            // the device's change vanish silently.
            let r = SettingsMerge.merge(device: snap, lastKnown: before,
                                        pending: settingsCache.pending)
            if !r.replacedDeviceChange.isEmpty {
                let names = r.replacedDeviceChange.map(\.label).joined(separator: ", ")
                settingsSyncNote = "Your phone's \(names) change replaced the one made on the device."
            }
        }
        settingsCache.device = snap
        settingsSeenThisLink = true
        saveSettingsCache()
        applyEffectiveSettings()
        syncSettings()
    }
}

extension BLEManager: CLLocationManagerDelegate {
    nonisolated func locationManagerDidChangeAuthorization(_ m: CLLocationManager) {
        MainActor.assumeIsolated {
            let a = m.authorizationStatus
            locationAuthorized = (a == .authorizedWhenInUse || a == .authorizedAlways)
            locationPermission = Self.state(of: a)
            if wantsAiding, a == .authorizedWhenInUse || a == .authorizedAlways {
                beginLocationUpdates()
            } else if a == .denied || a == .restricted {
                wantsAiding = false
            }
        }
    }

    nonisolated func locationManager(_ m: CLLocationManager,
                                     didUpdateLocations locs: [CLLocation]) {
        MainActor.assumeIsolated {
            guard wantsAiding, let loc = locs.last else { return }
            transmitAiding(loc)   // stream every fix (throttled inside)
        }
    }

    nonisolated func locationManager(_ m: CLLocationManager,
                                     didFailWithError error: Error) {
        MainActor.assumeIsolated { wantsAiding = false }
    }
}

private extension Data {
    mutating func appendLE(_ v: Int16) {
        let u = UInt16(bitPattern: v)
        append(UInt8(u & 0xFF)); append(UInt8(u >> 8))
    }
    mutating func appendLE(_ v: UInt16) {
        append(UInt8(v & 0xFF)); append(UInt8(v >> 8))
    }
    mutating func appendLE(_ v: Int32) {
        let u = UInt32(bitPattern: v)
        for s in stride(from: 0, to: 32, by: 8) { append(UInt8((u >> s) & 0xFF)) }
    }
    mutating func appendLE(_ v: UInt32) {
        for s in stride(from: 0, to: 32, by: 8) { append(UInt8((v >> s) & 0xFF)) }
    }

    /// Little-endian u32 at a byte offset. Returns 0 rather than trapping if the
    /// packet is short — a malformed notification must not crash the app.
    func le32(at i: Int) -> UInt32 {
        guard i + 4 <= count else { return 0 }
        return UInt32(self[i]) | (UInt32(self[i + 1]) << 8) |
               (UInt32(self[i + 2]) << 16) | (UInt32(self[i + 3]) << 24)
    }

    /// A length-prefixed UTF-8 string, advancing `i` past it. The mesh protocol
    /// prefixes every name this way so several can share one packet.
    func lenString(at i: inout Int) -> String {
        guard i < count else { return "" }
        let n = Int(self[i])
        i += 1
        guard n > 0, i + n <= count else { return "" }
        let s = String(data: subdata(in: i..<(i + n)), encoding: .utf8) ?? ""
        i += n
        return s
    }
}
