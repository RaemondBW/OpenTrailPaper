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

    var kindsText: String {
        var parts: [String] = []
        if kindsMask & 1 != 0 { parts.append("Heart rate") }
        if kindsMask & 2 != 0 { parts.append("Power") }
        if kindsMask & 4 != 0 { parts.append("Cadence") }
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
struct RideFile: Identifiable, Hashable {
    let name: String
    let size: Int
    var id: String { name }
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

    var id: UInt32 { num }
    /// "!a4c1380c" — how Meshtastic writes a node number everywhere.
    var nodeId: String { "!" + String(format: "%08x", num) }
    var displayName: String { longName.isEmpty ? nodeId : longName }
}

/// The device's mesh configuration, as it reports it.
struct MeshState: Equatable {
    static let broadcastAddr: UInt32 = 0xFFFF_FFFF

    var enabled = false
    var radioOk = false
    var nodeNum: UInt32 = 0
    var frequencyHz: UInt32 = 0
    var channel = ""
    var channelKey: UInt8 = 1
    var longName = ""
    var shortName = ""
    var nodeCount = 0
    var unread = 0

    var nodeId: String { "!" + String(format: "%08x", nodeNum) }
    var frequencyMHz: Double { Double(frequencyHz) / 1_000_000 }
    /// Nothing has been heard from the device yet.
    var isUnknown: Bool { nodeNum == 0 }
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
    @Published var ftpWatts = 250
    @Published var tzMinutes = -420
    @Published var backlight = 2        // 0 off .. 3 bright (mirrors device)
    @Published var clock24h = true      // device status-bar clock format
    @Published var usbDrive = true      // expose device SD as a USB drive
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
    @Published var rides: [RideFile] = []
    @Published var loadingRides = false
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
    private var tileQueue: [(id: String, data: Data)] = []
    private var currentTileId: String? = nil        // non-nil while sending a tile
    private var tileJobFailed = false
    private var tilesMoreComing = false             // app still building tiles to enqueue

    private var central: CBCentralManager!
    private var peripheral: CBPeripheral?
    private var settingsChar: CBCharacteristic?
    private var dashChar: CBCharacteristic?
    /// The device's dashboard layout, as the text of /config/dashboard.cfg.
    /// nil until the device has been read, which is how the editor knows to
    /// show "connect to edit" rather than an invented default that would
    /// overwrite the rider's real one the moment they touched a control.
    @Published var dashLayout: DashLayout?
    /// Version we are flashing, so the success check compares against what
    /// was actually sent rather than a compile-time constant.
    private var otaTargetVersion = ""
    private var statusChar: CBCharacteristic?
    private var routeChar: CBCharacteristic?
    private var ridesChar: CBCharacteristic?
    private var otaChar: CBCharacteristic?

    // MARK: Meshtastic
    private var meshChar: CBCharacteristic?
    /// Rebuilt from the device's stream, then published in one go at the end
    /// marker — publishing per message would redraw the chat mid-list.
    private var meshBuilding: [MeshMessage] = []
    private var meshNodesBuilding: [MeshNode] = []
    @Published var meshState = MeshState()
    @Published var meshMessages: [MeshMessage] = []
    @Published var meshNodes: [MeshNode] = []
    @Published var meshStats = MeshStats()
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

    override init() {
        super.init()
        if isDemoUpdate { state = .connected; deviceFirmware = "v0.83" }
        if isDemoDash { state = .connected; dashLayout = .deviceDefault }
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
        // Returning users get Bluetooth up immediately so the device auto-
        // connects. First-run users create the central when they tap "Enable
        // Bluetooth" in onboarding, so the system prompt lands on that screen.
        if UserDefaults.standard.bool(forKey: Self.onboardedKey) {
            startCentral()
        }
    }

    static let onboardedKey = "didOnboard"

    /// Create the Bluetooth central. iOS shows the Bluetooth permission prompt
    /// the first time this runs. Idempotent; skipped in the update demo.
    private func startCentral() {
        guard central == nil, !isDemoUpdate else { return }
        central = CBCentralManager(delegate: self, queue: .main)
    }

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
        p.writeValue(payload, for: c, type: .withResponse)
        lastAidingSent = Date()
    }

    func startScan() {
        guard let central, central.state == .poweredOn else { return }
        state = .scanning
        central.scanForPeripherals(withServices: [BikeUUID.service])
    }

    func disconnect() {
        if let p = peripheral { central?.cancelPeripheralConnection(p) }
    }

    // MARK: settings

    // Auto-sync: any app-side settings edit goes to these setters, which update
    // the mirror and immediately push to the device. Values arriving FROM the
    // device (parseSettings) set the @Published fields directly, so they never
    // echo back.
    func setFtp(_ v: Int) { ftpWatts = v; pushSettings() }
    func setTz(_ v: Int) { tzMinutes = v; pushSettings() }
    func setBacklight(_ v: Int) { backlight = v; pushSettings() }
    func setUseMiles(_ v: Bool) {
        UserDefaults.standard.set(v, forKey: UnitPref.key)
        pushSettings()
    }
    func setClock24h(_ v: Bool) { clock24h = v; pushSettings() }
    func setUsbDrive(_ v: Bool) { usbDrive = v; pushSettings() }

    func pushSettings() {
        guard let c = settingsChar, let p = peripheral else { return }
        var payload = Data()
        payload.appendLE(Int16(ftpWatts))
        payload.appendLE(Int16(tzMinutes))
        payload.append(UserDefaults.standard.bool(forKey: UnitPref.key) ? 1 : 0)
        payload.append(UInt8(clamping: backlight))
        payload.append(clock24h ? 1 : 0)
        payload.append(usbDrive ? 1 : 0)
        p.writeValue(payload, for: c, type: .withResponse)
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
        rides = []
        loadingRides = true
        p.writeValue(Data([0x01]), for: c, type: .withResponse)
    }

    func deleteRide(_ name: String) {
        rides.removeAll { $0.name == name }
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
        Task { @MainActor in
            do {
                let data = try await FirmwareRelease.shared.image(for: release)
                startFirmwareUpload(data, tag: release.tag)
            } catch {
                keepAwake(false)
                otaMessage = "Download failed — check your connection"
            }
        }
    }

    /// Second half of startFirmwareUpdate, once the image is in hand.
    private func startFirmwareUpload(_ data: Data, tag: String) {
        guard let c = otaChar, let p = peripheral else {
            keepAwake(false); otaMessage = "Not connected"; return
        }
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
            p.writeValue(pkt, for: c, type: .withoutResponse)
        }
        if !otaCommitSent {                                        // all data sent
            otaCommitSent = true
            p.writeValue(Data([0x03]), for: c, type: .withResponse)   // commit
            otaPhase = .saving
            otaMessage = "Saving to the device…"
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
    // Connected sensors whose kind mask includes `bit` (1 HR, 2 power, 4 cadence).
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

    var canUploadMap: Bool { mapChar != nil && peripheral != nil }

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
    }

    // Ask the device which map areas it already has (streamed back via notify).
    func refreshDeviceMaps() {
        guard let c = mapChar, let p = peripheral else { return }
        p.writeValue(Data([0x05]), for: c, type: .withResponse)
    }

    // MARK: H3 tile streaming

    // Ask the device which H3 tile ids are already on its SD, so the caller can
    // skip re-sending them (replied via 0xD0/0xD1/0xD2 notifies).
    func refreshDeviceTiles() {
        guard let c = mapChar, let p = peripheral else { return }
        p.writeValue(Data([0x07]), for: c, type: .withResponse)
    }

    // Streaming upload: the app produces tiles batch-by-batch while download +
    // vectorization runs, and sends them in parallel. Call startTileStream()
    // once, enqueueTiles() per batch as they're built, finishTileStream() when
    // the last batch has been produced.
    func startTileStream() {
        guard mapChar != nil, peripheral != nil else { tileMessage = "Not connected"; return }
        tileQueue = []
        tilesTotal = 0
        tilesDone = 0
        tileJobFailed = false
        tilesMoreComing = true
        tilesUploading = true
        tileMessage = "Sending tiles…"
        keepAwake(true)
    }

    // Add freshly-built tiles to the send queue; starts pumping if idle. Skips
    // tiles already on the device or already queued.
    func enqueueTiles(_ newOnes: [(id: String, data: Data)]) {
        guard tilesUploading else { return }
        let queued = Set(tileQueue.map(\.id))
        let fresh = newOnes.filter {
            !deviceTileIds.contains($0.id) && !queued.contains($0.id) && $0.id != currentTileId
        }
        guard !fresh.isEmpty else { return }
        tileQueue.append(contentsOf: fresh)
        tilesTotal += fresh.count
        if currentTileId == nil { sendNextTile() }   // pump if idle
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
        tilesUploading = false
        tilesMoreComing = false
        mapUploading = false
        currentTileId = nil
        keepAwake(false)
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
        mapData = tile.data
        mapOffset = 0
        mapEndSent = false
        mapUploading = true            // reuse the CHR_MAP chunk pump
        mapChunk = max(20, p.maximumWriteValueLength(for: .withoutResponse)) - 1
        tileMessage = "Sending tile \(tilesDone + 1)/\(tilesTotal)…"

        var cmd = Data([0x06])         // begin-tile
        var size = UInt32(tile.data.count).littleEndian
        withUnsafeBytes(of: &size) { cmd.append(contentsOf: $0) }
        cmd.append(Data(tile.id.utf8))
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
                deviceTileIds.insert(id)
                tilesDone += 1
                currentTileId = nil
                sendNextTile()
            } else {
                mapUploading = false; mapProgress = 1; mapMessage = "Map installed"
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
            }
        case 0xD0: tileIdsBuilding = []                           // tile-list begin
        case 0xD1 where d.count > 1:                              // comma-separated ids
            if let s = String(data: d.subdata(in: 1..<d.count), encoding: .utf8) {
                for id in s.split(separator: ",") where !id.isEmpty {
                    tileIdsBuilding.append(String(id))
                }
            }
        case 0xD2:                                               // tile-list end
            deviceTileIds = Set(tileIdsBuilding)
            cacheDeviceTiles()
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
    }

    func requestMeshStats() {
        guard let c = meshChar, let p = peripheral else { return }
        p.writeValue(Data([0x09]), for: c, type: .withResponse)
    }

    /// Sends a message to one node, or to the whole channel when `to` is nil.
    func sendMeshText(_ text: String, to: UInt32? = nil) {
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
    func setMeshChannel(name: String, key: UInt8) {
        guard let c = meshChar, let p = peripheral else { return }
        let n = Data(String(name.prefix(14)).utf8)
        guard !n.isEmpty else { return }
        var cmd = Data([0x07, max(1, min(10, key))])
        cmd.append(n)
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
        meshState = MeshState(enabled: true, radioOk: true, nodeNum: 0xa4c1380c,
                              frequencyHz: 906_875_000, channel: "LongFast",
                              channelKey: 1, longName: "OpenTrail 380c",
                              shortName: "380c", nodeCount: 3, unread: 0)
        let now = Date()
        meshNodes = [
            MeshNode(num: 0x7b2e91aa, shortName: "ALEX", longName: "Alex",
                     lastHeard: now.addingTimeInterval(-90), rssi: -84, snr: 6, hops: 0),
            MeshNode(num: 0x3fd10c55, shortName: "SAM", longName: "Sam's T-Beam",
                     lastHeard: now.addingTimeInterval(-600), rssi: -108, snr: -4, hops: 2),
            MeshNode(num: 0x11aa4402, shortName: "GATE", longName: "Trailhead Gate",
                     lastHeard: now.addingTimeInterval(-2400), rssi: -97, snr: 1, hops: 1),
        ]
        let bc = MeshState.broadcastAddr
        meshMessages = [
            MeshMessage(id: 1, from: 0x7b2e91aa, to: bc, outgoing: false,
                        status: .sent, text: "Heading up the fire road now, should be at the saddle in 40.",
                        date: now.addingTimeInterval(-2100), rssi: -84, snr: 6, hops: 0),
            MeshMessage(id: 2, from: 0xa4c1380c, to: bc, outgoing: true,
                        status: .sent, text: "Copy. I'm still at the creek crossing, water is high.",
                        date: now.addingTimeInterval(-1800), rssi: 0, snr: 0, hops: 0),
            MeshMessage(id: 3, from: 0x3fd10c55, to: bc, outgoing: false,
                        status: .sent, text: "Same, went around on the north side. Bridge is out.",
                        date: now.addingTimeInterval(-1500), rssi: -108, snr: -4, hops: 2),
            MeshMessage(id: 4, from: 0xa4c1380c, to: 0x7b2e91aa, outgoing: true,
                        status: .acked, text: "Taking the north detour, add 20 min.",
                        date: now.addingTimeInterval(-900), rssi: 0, snr: 0, hops: 0),
            MeshMessage(id: 5, from: 0x7b2e91aa, to: bc, outgoing: false,
                        status: .sent, text: "Got it. Waiting at the saddle, no rush.",
                        date: now.addingTimeInterval(-120), rssi: -84, snr: 6, hops: 0),
        ]
        meshStats = MeshStats(rx: 214, rxDropped: 11, rxOtherChannel: 63,
                              rxDuplicate: 88, tx: 19, txFailed: 1, acksRx: 12)
    }

    private func handleMeshNotify(_ d: Data) {
        guard let op = d.first else { return }
        switch op {
        case 0x90:  // state
            guard d.count >= 13 else { return }
            var s = MeshState()
            s.enabled = d[1] & 1 != 0
            s.radioOk = d[1] & 2 != 0
            s.nodeNum = d.le32(at: 2)
            s.frequencyHz = d.le32(at: 6)
            s.channelKey = d[10]
            s.nodeCount = Int(d[11])
            s.unread = Int(d[12])
            var i = 13
            s.channel = d.lenString(at: &i)
            s.shortName = d.lenString(at: &i)
            s.longName = d.lenString(at: &i)
            meshState = s

        case 0x91:  // one message
            guard d.count >= 26 else { return }
            let textLen = Int(d[25])
            guard d.count >= 26 + textLen else { return }
            let flags = d[24]
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
            meshNodesBuilding.append(MeshNode(
                num: d.le32(at: 1),
                shortName: short,
                longName: long,
                lastHeard: Date().addingTimeInterval(-age),
                rssi: Int(Int8(bitPattern: d[10])),
                snr: Int(Int8(bitPattern: d[9])),
                hops: Int(d[11])))

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
        guard ridesChar != nil, peripheral != nil else { return }
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
            rides.append(RideFile(name: name, size: size))
        case 0x03:  // list done
            loadingRides = false
            rides.sort { $0.name > $1.name }   // newest first
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

    // MARK: route upload (chunked with a 1-byte opcode per packet)

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
        Task { @MainActor in
            for (idx, packet) in packets.enumerated() {
                p.writeValue(packet, for: c, type: .withResponse)
                lastUploadProgress = Double(idx + 1) / Double(packets.count)
                try? await Task.sleep(nanoseconds: 12_000_000)  // pace writes
            }
            lastUploadProgress = nil
            // The writes are queued; the button now shows "Sent". Firmware ≥ this
            // release notifies back (0x23/0x24) so we can upgrade that to a real
            // "Received by device" confirmation — see handleRouteNotify.
            routeSent = true
            lastMessage = "Route “\(name)” sent — \(maneuvers.count) turns"
        }
    }
}

extension BLEManager: CBCentralManagerDelegate {
    nonisolated func centralManagerDidUpdateState(_ c: CBCentralManager) {
        if isDemoUpdate || isDemoDash { return }   // demo holds a fake connected state
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
        MainActor.assumeIsolated {
            guard peripheral == nil else { return }
            peripheral = p
            p.delegate = self
            state = .connecting
            c.stopScan()
            c.connect(p)
        }
    }

    nonisolated func centralManager(_ c: CBCentralManager,
                                    didConnect p: CBPeripheral) {
        MainActor.assumeIsolated { p.discoverServices([BikeUUID.service]) }
    }

    nonisolated func centralManager(_ c: CBCentralManager,
                                    didDisconnectPeripheral p: CBPeripheral,
                                    error: Error?) {
        MainActor.assumeIsolated {
            settingsChar = nil; statusChar = nil; routeChar = nil; ridesChar = nil
            sensorsChar = nil; mapChar = nil; otaChar = nil; meshChar = nil
            // A half-received mesh stream must not be published on reconnect.
            meshBuilding = []; meshNodesBuilding = []
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
            if tilesUploading {
                tileQueue = []
                finishTileJob(message: "Interrupted — reconnect to resume")
            }
            status = DeviceStatus()
            sawStatusSinceConnect = false
            rides = []; loadingRides = false
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
            if let known = p as CBPeripheral? {
                peripheral = known
                known.delegate = self
                state = .connecting
                c.connect(known)
            } else {
                startScan()
            }
        }
    }
}

extension BLEManager: CBPeripheralDelegate {
    nonisolated func peripheral(_ p: CBPeripheral, didDiscoverServices error: Error?) {
        // Discover ALL characteristics (nil) — an explicit list previously
        // omitted the sensors + map characteristics, so those features never
        // worked. nil is also future-proof as new characteristics are added.
        for s in p.services ?? [] where s.uuid == BikeUUID.service {
            p.discoverCharacteristics(nil, for: s)
        }
    }

    nonisolated func peripheral(_ p: CBPeripheral,
                                didDiscoverCharacteristicsFor s: CBService,
                                error: Error?) {
        MainActor.assumeIsolated {
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
                case BikeUUID.mesh:
                    meshChar = ch; p.setNotifyValue(true, for: ch)
                default: break
                }
            }
            state = .connected
        }
    }

    // The version reply comes back as a notification, so only ask once the OTA
    // characteristic's notifications are actually enabled (otherwise the reply
    // is dropped and the app never learns the device firmware version).
    nonisolated func peripheral(_ p: CBPeripheral,
                                didUpdateNotificationStateFor ch: CBCharacteristic,
                                error: Error?) {
        MainActor.assumeIsolated {
            if ch.uuid == BikeUUID.ota { queryDeviceFirmware() }
            if ch.uuid == BikeUUID.sensors, let p = peripheral {
                p.writeValue(Data([0x05]), for: ch, type: .withResponse)   // one snapshot
            }
            // Same reason as the OTA version query: the device answers by
            // notification, so asking before notifications are on throws the
            // reply away and the Messages tab sits empty until it is opened.
            if ch.uuid == BikeUUID.mesh { refreshMesh() }
        }
    }

    // CoreBluetooth's send queue drained — push more firmware / map chunks.
    nonisolated func peripheralIsReady(toSendWriteWithoutResponse p: CBPeripheral) {
        MainActor.assumeIsolated {
            pumpOtaChunks()
            pumpMapChunks()
        }
    }

    nonisolated func peripheral(_ p: CBPeripheral,
                                didUpdateValueFor ch: CBCharacteristic,
                                error: Error?) {
        guard let data = ch.value else { return }
        MainActor.assumeIsolated {
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
            default: break
            }
        }
    }

    // MARK: - Dashboard layout

    private func parseDashLayout(_ d: Data) {
        guard let text = String(data: d, encoding: .utf8) else { return }
        dashLayout = DashLayout(text: text)
    }

    /// Push a layout to the device. It writes the file, applies it to the panel,
    /// and notifies back what it actually stored — so a rejected layout corrects
    /// the editor instead of leaving it out of step.
    func sendDashLayout(_ layout: DashLayout) {
        guard let ch = dashChar, let p = peripheral else { return }
        guard let data = layout.configText.data(using: .utf8) else { return }
        p.writeValue(data, for: ch, type: .withResponse)
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
    // Set the mirror fields directly — no push back.
    private func parseSettings(_ d: Data) {
        guard d.count >= 4 else { return }
        ftpWatts = Int(Int16(bitPattern: UInt16(d[0]) | (UInt16(d[1]) << 8)))
        tzMinutes = Int(Int16(bitPattern: UInt16(d[2]) | (UInt16(d[3]) << 8)))
        if d.count >= 6 {
            UserDefaults.standard.set(d[4] != 0, forKey: UnitPref.key)
            backlight = Int(d[5])
        }
        if d.count >= 7 { clock24h = d[6] != 0 }
        if d.count >= 8 { usbDrive = d[7] != 0 }
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
