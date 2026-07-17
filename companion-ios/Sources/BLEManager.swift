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
    static let bundledFirmwareVersion = "v0.72"      // matches src/config.h

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
    private var statusChar: CBCharacteristic?
    private var routeChar: CBCharacteristic?
    private var ridesChar: CBCharacteristic?
    private var otaChar: CBCharacteristic?

    // One-shot location handed to the device on connect so its GPS warm-starts
    // near the phone instead of cold-searching the whole sky.
    private let locationManager = CLLocationManager()
    private var wantsAiding = false
    private var fixStableTask: Task<Void, Never>?   // debounce stopping the stream
    @Published var lastAidingSent: Date? = nil

    private var dlBuffer = Data()
    private var dlExpected = 0
    private var dlName = ""
    private var dlNextSeq: UInt16 = 0
    @Published private(set) var downloadingLog = false

    // OTA transfer state
    private var otaData = Data()
    private var otaOffset = 0
    private var otaChunk = 180
    private var otaCommitSent = false

    override init() {
        super.init()
        central = CBCentralManager(delegate: self, queue: .main)
        locationManager.delegate = self
        locationManager.desiredAccuracy = kCLLocationAccuracyHundredMeters
        // Show last-known on-device tiles immediately; a refresh confirms them.
        if let saved = UserDefaults.standard.stringArray(forKey: Self.tileCacheKey) {
            deviceTileIds = Set(saved)
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
        // Keep streaming while riding with the phone pocketed / app backgrounded
        // (requires the `location` background mode, which we declare).
        locationManager.allowsBackgroundLocationUpdates = true
        locationManager.pausesLocationUpdatesAutomatically = false
        locationManager.startUpdatingLocation()
        locationManager.requestLocation()   // nudge an immediate first fix
    }

    func stopLocationStream() {
        wantsAiding = false
        locationManager.stopUpdatingLocation()
        locationManager.allowsBackgroundLocationUpdates = false
    }

    private func transmitAiding(_ loc: CLLocation) {
        guard let c = routeChar, let p = peripheral else { return }
        // Throttle to ~3 s so we don't flood BLE with position updates.
        if let last = lastAidingSent, Date().timeIntervalSince(last) < 3 { return }
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
        guard central.state == .poweredOn else { return }
        state = .scanning
        central.scanForPeripherals(withServices: [BikeUUID.service])
    }

    func disconnect() {
        if let p = peripheral { central.cancelPeripheralConnection(p) }
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
        rides = []
        loadingRides = true
        p.writeValue(Data([0x01]), for: c, type: .withResponse)
    }

    func deleteRide(_ name: String) {
        rides.removeAll { $0.name == name }
        try? FileManager.default.removeItem(at: BLEManager.cachedURL(for: name))
        // Offline: just drop the local cache. Connected: also delete on device.
        guard let c = ridesChar, let p = peripheral else { return }
        var cmd = Data([0x03]); cmd.append(Data(name.utf8))
        p.writeValue(cmd, for: c, type: .withResponse)
    }

    // Pull /diag.log off the device (reuses the reliable ride-transfer path).
    func downloadLog() {
        guard let c = ridesChar, let p = peripheral else {
            lastMessage = "Not connected"; return
        }
        dlBuffer = Data()
        dlExpected = 0
        downloadingLog = true
        downloadingName = "diag"
        downloadProgress = 0
        logFileURL = nil
        p.writeValue(Data([0x05]), for: c, type: .withResponse)
    }

    // List the per-day log files on the device (reply parsed via 0x30/0x31).
    func requestLogList() {
        guard let c = ridesChar, let p = peripheral else { return }
        logsBuilding = []
        loadingLogs = true
        p.writeValue(Data([0x06]), for: c, type: .withResponse)
    }

    // Download one specific day's log file.
    func downloadLogFile(_ name: String) {
        guard let c = ridesChar, let p = peripheral else { lastMessage = "Not connected"; return }
        dlBuffer = Data()
        dlExpected = 0
        downloadingLog = true
        downloadingName = name
        downloadProgress = 0
        logFileURL = nil
        var cmd = Data([0x07]); cmd.append(Data(name.utf8))
        p.writeValue(cmd, for: c, type: .withResponse)
    }

    // MARK: firmware / OTA

    var updateAvailable: Bool {
        !deviceFirmware.isEmpty && deviceFirmware != BLEManager.bundledFirmwareVersion
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
        keepAwake(true)
        guard let url = Bundle.main.url(forResource: "firmware", withExtension: "bin"),
              let data = try? Data(contentsOf: url) else {
            otaMessage = "Bundled firmware missing"; return
        }
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
            if deviceFirmware == BLEManager.bundledFirmwareVersion {
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
                otaMessage = "Verifying the new version…"
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

    func downloadRide(_ name: String) {
        guard let c = ridesChar, let p = peripheral else { return }
        dlBuffer = Data()
        dlExpected = 0
        dlName = name
        downloadingName = name
        downloadProgress = 0
        downloadedFileURL = nil
        var cmd = Data([0x02])
        cmd.append(Data(name.utf8))
        p.writeValue(cmd, for: c, type: .withResponse)
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
            dlExpected = Int(d[1]) | (Int(d[2]) << 8) | (Int(d[3]) << 16) | (Int(d[4]) << 24)
            dlBuffer = Data(capacity: dlExpected)
            dlNextSeq = 0
        case 0x11:  // chunk: [u16 seq][payload]
            guard d.count > 3 else { return }
            let seq = UInt16(d[1]) | (UInt16(d[2]) << 8)
            // Strict in-order: only append the chunk we're expecting next.
            // Anything else (a duplicate from a resend, or a chunk that arrived
            // after a gap) is dropped; the device resends from dlNextSeq.
            if seq == dlNextSeq {
                dlBuffer.append(d[3...])
                dlNextSeq = seq &+ 1
                if dlExpected > 0 {
                    downloadProgress = min(1, Double(dlBuffer.count) / Double(dlExpected))
                }
            }
        case 0x14:  // window end — tell the device the next seq we need
            var ack = Data([0x04])
            ack.append(UInt8(dlNextSeq & 0xFF))
            ack.append(UInt8(dlNextSeq >> 8))
            if let p = peripheral, let c = ridesChar {
                p.writeValue(ack, for: c, type: .withResponse)
            }
        case 0x12:  // done
            finishDownload()
        case 0x13: break  // delete ack (already removed locally)
        case 0x1F:  // error (e.g. recording in progress)
            downloadingName = nil
            loadingRides = false
            lastMessage = "Device busy — stop the ride first"
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
        // A short transfer means packets were lost — surface it instead of
        // handing a truncated file to the parser.
        if dlExpected > 0 && dlBuffer.count != dlExpected {
            lastMessage = "Transfer incomplete (\(dlBuffer.count)/\(dlExpected) bytes) — try again"
            downloadingName = nil
            downloadingLog = false
            return
        }
        if downloadingLog {                    // diagnostics log, not a ride
            downloadingLog = false
            var fname = (downloadingName ?? "diag").replacingOccurrences(of: "/", with: "_")
            downloadingName = nil
            if !fname.hasSuffix(".log") { fname += ".log" }
            let url = FileManager.default.temporaryDirectory
                .appendingPathComponent("bikegps-\(fname)")
            do {
                try dlBuffer.write(to: url)
                logFileURL = url
            } catch {
                lastMessage = "Log save failed: \(error.localizedDescription)"
            }
            return
        }
        // Persist to the cache so it's available offline and never re-fetched.
        let url = BLEManager.cachedURL(for: dlName)
        do {
            try dlBuffer.write(to: url)
            downloadedFileURL = url
            lastMessage = "\(dlName) ready"
        } catch {
            lastMessage = "Save failed: \(error.localizedDescription)"
        }
        downloadingName = nil
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
        MainActor.assumeIsolated {
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
            peripheral = nil
            settingsChar = nil; statusChar = nil; routeChar = nil; ridesChar = nil
            sensorsChar = nil; mapChar = nil; otaChar = nil
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
            rides = []; loadingRides = false; downloadingName = nil
            deviceRoutes = []; loadingRoutes = false
            lastUploadProgress = nil; routeSent = false; routeReceived = false
            sensors = []; scanningSensors = false
            startScan()
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
            default: break
            }
        }
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
    }

    // Only run iOS location (and the blue background indicator) while the device
    // NEEDS it. Once the device has its own GPS fix, stop streaming; resume if it
    // loses the fix. Debounced so a brief dropout doesn't flap the stream.
    private func syncLocationStreamToDeviceFix() {
        guard state == .connected, routeChar != nil else { return }
        if status.gpsFix {
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
}
