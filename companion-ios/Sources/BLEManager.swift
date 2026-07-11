import Foundation
import CoreBluetooth
import Combine
import CryptoKit

// GATT UUIDs — must match src/ble_server.cpp on the device.
enum BikeUUID {
    static let service  = CBUUID(string: "B1C50000-9E0F-4B7A-9C6D-1F2E3A4B5C6D")
    static let settings = CBUUID(string: "B1C50001-9E0F-4B7A-9C6D-1F2E3A4B5C6D")
    static let status   = CBUUID(string: "B1C50002-9E0F-4B7A-9C6D-1F2E3A4B5C6D")
    static let route    = CBUUID(string: "B1C50003-9E0F-4B7A-9C6D-1F2E3A4B5C6D")
    static let rides    = CBUUID(string: "B1C50004-9E0F-4B7A-9C6D-1F2E3A4B5C6D")
    static let ota      = CBUUID(string: "B1C50005-9E0F-4B7A-9C6D-1F2E3A4B5C6D")
}

// A recorded ride file on the device.
struct RideFile: Identifiable, Hashable {
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
    @Published var lastUploadProgress: Double? = nil   // 0...1 while sending
    @Published var lastMessage: String? = nil

    // Ride download
    @Published var rides: [RideFile] = []
    @Published var loadingRides = false
    @Published var downloadingName: String? = nil
    @Published var downloadProgress: Double = 0
    @Published var downloadedFileURL: URL? = nil       // set when a ride is ready

    // Firmware / OTA
    @Published var deviceFirmware: String = ""      // running version on the device
    @Published var otaInProgress = false
    @Published var otaProgress: Double = 0
    @Published var otaMessage: String? = nil
    static let bundledFirmwareVersion = "v0.5"      // matches src/config.h

    // Saved routes on the device
    @Published var deviceRoutes: [String] = []
    @Published var loadingRoutes = false

    private var central: CBCentralManager!
    private var peripheral: CBPeripheral?
    private var settingsChar: CBCharacteristic?
    private var statusChar: CBCharacteristic?
    private var routeChar: CBCharacteristic?
    private var ridesChar: CBCharacteristic?
    private var otaChar: CBCharacteristic?

    private var dlBuffer = Data()
    private var dlExpected = 0
    private var dlName = ""
    private var dlNextSeq: UInt16 = 0

    // OTA transfer state
    private var otaData = Data()
    private var otaOffset = 0
    private var otaChunk = 180
    private var otaCommitSent = false

    override init() {
        super.init()
        central = CBCentralManager(delegate: self, queue: .main)
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

    func pushSettings() {
        guard let c = settingsChar, let p = peripheral else { return }
        var payload = Data()
        payload.appendLE(Int16(ftpWatts))
        payload.appendLE(Int16(tzMinutes))
        payload.append(UserDefaults.standard.bool(forKey: UnitPref.key) ? 1 : 0)
        payload.append(UInt8(clamping: backlight))
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

    // MARK: firmware / OTA

    var updateAvailable: Bool {
        !deviceFirmware.isEmpty && deviceFirmware != BLEManager.bundledFirmwareVersion
    }

    func queryDeviceFirmware() {
        guard let c = otaChar, let p = peripheral else { return }
        p.writeValue(Data([0x05]), for: c, type: .withResponse)
    }

    // Stream the app-bundled firmware.bin into the device's spare OTA slot.
    func startFirmwareUpdate() {
        guard let c = otaChar, let p = peripheral else {
            otaMessage = "Not connected"; return
        }
        guard let url = Bundle.main.url(forResource: "firmware", withExtension: "bin"),
              let data = try? Data(contentsOf: url) else {
            otaMessage = "Bundled firmware missing"; return
        }
        otaData = data
        otaOffset = 0
        otaCommitSent = false
        otaInProgress = true
        otaProgress = 0
        otaMessage = "Preparing…"
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
            otaMessage = "Verifying…"
        }
    }

    private func handleOtaNotify(_ d: Data) {
        guard let op = d.first else { return }
        switch op {
        case 0xA3:                                   // running version
            deviceFirmware = String(decoding: d[1...], as: UTF8.self)
        case 0xA0: otaMessage = "Sending…"; pumpOtaChunks()   // device ready
        case 0xA1:                                   // success (device rebooting)
            otaInProgress = false; otaProgress = 1
            otaMessage = "Update complete — device rebooting"
            deviceFirmware = BLEManager.bundledFirmwareVersion
        case 0xA2: otaInProgress = false; otaMessage = "Update canceled"
        case 0xAF:                                   // error
            otaInProgress = false
            let code = d.count > 1 ? Int(d[1]) : -1
            otaMessage = "Update failed (error \(code)) — device unchanged"
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
        Task { @MainActor in
            for (idx, packet) in packets.enumerated() {
                p.writeValue(packet, for: c, type: .withResponse)
                lastUploadProgress = Double(idx + 1) / Double(packets.count)
                try? await Task.sleep(nanoseconds: 12_000_000)  // pace writes
            }
            lastUploadProgress = nil
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
            status = DeviceStatus()
            rides = []; loadingRides = false; downloadingName = nil
            deviceRoutes = []; loadingRoutes = false
            startScan()
        }
    }
}

extension BLEManager: CBPeripheralDelegate {
    nonisolated func peripheral(_ p: CBPeripheral, didDiscoverServices error: Error?) {
        for s in p.services ?? [] where s.uuid == BikeUUID.service {
            p.discoverCharacteristics(
                [BikeUUID.settings, BikeUUID.status, BikeUUID.route,
                 BikeUUID.rides, BikeUUID.ota], for: s)
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
                case BikeUUID.rides:
                    ridesChar = ch; p.setNotifyValue(true, for: ch)
                case BikeUUID.ota:
                    otaChar = ch; p.setNotifyValue(true, for: ch)
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
        }
    }

    // CoreBluetooth's send queue drained — push more firmware chunks.
    nonisolated func peripheralIsReady(toSendWriteWithoutResponse p: CBPeripheral) {
        MainActor.assumeIsolated { pumpOtaChunks() }
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
