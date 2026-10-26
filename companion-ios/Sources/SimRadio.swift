#if DEBUG
import CoreBluetooth
import Foundation
import Network
import UIKit

// DEBUG only: the firmware simulator (sim/, `otp-sim`) as the app's radio, so
// BLEManager's real connection state machine and LinkClient run against the
// real firmware in the iOS Simulator. Launch with `-sim-ble 127.0.0.1:7920`
// (or env OTP_SIM_BLE); `-sim-forget-bond` is iOS Settings' "Forget This
// Device". Wire format: sim/PROTOCOL.md (simble). See sim/IOS.md.
//
// What it emulates of iOS, so BLEManager sees what it would on a phone:
//  - a bond store (UserDefaults): the phone's identity address, whether it
//    holds keys for the device, and the peripheral identifier iOS assigned;
//  - connect() is a pending connect with no timeout: while the simulator is
//    down (connection refused) it retries quietly and reports nothing;
//  - with a bond, a connect completes only once the link is encrypted (iOS
//    re-encrypts before didConnect); keys the device no longer holds fail it
//    with CBError.peerRemovedPairingInformation; without a bond, didConnect
//    comes at once and pairing follows (the passkey is auto-confirmed);
//  - a connect the device never answers fails with CBError.connectionFailed;
//  - a drop the device announced (DISCONNECTED) is CBError.peripheralDisconnected,
//    or connectionTimeout for reason 0x08; a link that vanishes without a
//    word (socket closed) is a supervision timeout, CBError.connectionTimeout;
//  - cancelPeer ends an active or pending connect with didDisconnect(nil);
//  - in the background the app keeps running (a background task), as
//    bluetooth-central wakes it for CoreBluetooth's events.
// Only the link service is reported; the legacy characteristics are
// CBCharacteristics, which only CoreBluetooth can make.

enum SimBLE {
    // simble frame types
    static let pConnect: UInt8 = 0x01, pDiscover: UInt8 = 0x02, pWrite: UInt8 = 0x04
    static let pSubscribe: UInt8 = 0x05, pSecure: UInt8 = 0x06, pDisconnect: UInt8 = 0x07
    static let pAdv: UInt8 = 0x0A, pPeerReadRsp: UInt8 = 0x10, pPeerServices: UInt8 = 0x12
    static let dConnected: UInt8 = 0x81, dDiscovered: UInt8 = 0x82, dWriteRsp: UInt8 = 0x84
    static let dSubscribeRsp: UInt8 = 0x85, dNotify: UInt8 = 0x86, dDisconnected: UInt8 = 0x87
    static let dServiceChanged: UInt8 = 0x89, dSecurity: UInt8 = 0x8A, dAdv: UInt8 = 0x8B
    static let dPeerRead: UInt8 = 0x90

    static let gapService = CBUUID(string: "1800")
    static let gapName = CBUUID(string: "2A00")

    /// 16 bytes in string order.
    static func bytes(_ u: CBUUID) -> Data {
        let d = u.data
        if d.count == 16 { return d }
        // 16/32-bit: 0000xxxx-0000-1000-8000-00805f9b34fb
        var full = Data([0, 0, 0, 0, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB])
        full.replaceSubrange((4 - d.count)..<4, with: d)
        return full
    }

    static func uuid(_ d: Data) -> CBUUID { CBUUID(data: Data(d)) }

    static func frame(_ type: UInt8, _ body: Data = Data()) -> Data {
        var f = Data()
        let n = UInt16(body.count + 1)
        f.append(UInt8(n & 0xFF)); f.append(UInt8(n >> 8)); f.append(type)
        f.append(body)
        return f
    }

    /// Splits complete frames off the front of `buf`.
    static func frames(_ buf: inout Data) -> [(UInt8, Data)] {
        var out: [(UInt8, Data)] = []
        while buf.count >= 3 {
            let s = buf.startIndex
            let n = Int(buf[s]) | Int(buf[s + 1]) << 8
            guard n > 0 else { buf.removeFirst(2); continue }
            guard buf.count >= 2 + n else { break }
            out.append((buf[s + 2], Data(buf[(s + 3)..<(s + 2 + n)])))
            buf.removeFirst(2 + n)
        }
        return out
    }

    static func tcp() -> NWParameters {
        let t = NWProtocolTCP.Options()
        t.noDelay = true
        t.connectionTimeout = 2
        return NWParameters(tls: nil, tcp: t)
    }
}

/// The phone's side of pairing, as iOS keeps it.
enum SimBondStore {
    private static let identityKey = "sim.ble.identity"
    private static let bondedKey = "sim.ble.bonded"
    private static let peerKey = "sim.ble.peerId"

    /// The phone's identity address (6 bytes, most significant first).
    static var identity: Data {
        if let s = UserDefaults.standard.string(forKey: identityKey), let d = Data(hex: s), d.count == 6 {
            return d
        }
        var d = Data((0..<6).map { _ in UInt8.random(in: 0...255) })
        d[0] |= 0xC0   // static random address
        UserDefaults.standard.set(d.map { String(format: "%02x", $0) }.joined(), forKey: identityKey)
        return d
    }
    static var bonded: Bool {
        get { UserDefaults.standard.bool(forKey: bondedKey) }
        set { UserDefaults.standard.set(newValue, forKey: bondedKey) }
    }
    /// The CBPeripheral.identifier iOS gave the device (stable per phone).
    static var peerId: UUID {
        if let s = UserDefaults.standard.string(forKey: peerKey), let u = UUID(uuidString: s) { return u }
        let u = UUID()
        UserDefaults.standard.set(u.uuidString, forKey: peerKey)
        return u
    }
    /// Settings › Bluetooth › Forget This Device.
    static func forget() { bonded = false }
}

private extension Data {
    init?(hex: String) {
        var d = Data()
        var i = hex.startIndex
        while i < hex.endIndex {
            guard let j = hex.index(i, offsetBy: 2, limitedBy: hex.endIndex),
                  let b = UInt8(hex[i..<j], radix: 16) else { return nil }
            d.append(b)
            i = j
        }
        self = d
    }
    var le16: UInt16 { count >= 2 ? UInt16(self[startIndex]) | UInt16(self[startIndex + 1]) << 8 : 0 }
}

@MainActor
final class SimRadio: RadioCentral {
    /// `-sim-ble host:port` or OTP_SIM_BLE; nil = use CoreBluetooth.
    static func fromLaunchArguments(events: RadioEvents) -> SimRadio? {
        let args = ProcessInfo.processInfo.arguments
        var spec = ProcessInfo.processInfo.environment["OTP_SIM_BLE"]
        if let i = args.firstIndex(of: "-sim-ble"), i + 1 < args.count { spec = args[i + 1] }
        guard let spec, !spec.isEmpty else { return nil }
        let parts = spec.split(separator: ":")
        let host = parts.count > 1 ? String(parts[0]) : "127.0.0.1"
        guard let portNum = UInt16(parts.last ?? ""), let port = NWEndpoint.Port(rawValue: portNum) else {
            NSLog("[sim-ble] bad endpoint %@", spec)
            return nil
        }
        if args.contains("-sim-forget-bond") { SimBondStore.forget() }
        NSLog("[sim-ble] radio is the firmware sim at %@:%d (bonded=%d)", host, Int(portNum),
              SimBondStore.bonded ? 1 : 0)
        return SimRadio(host: NWEndpoint.Host(host), port: port, events: events)
    }

    let host: NWEndpoint.Host
    let port: NWEndpoint.Port
    weak var events: RadioEvents?
    /// The one device behind the endpoint.
    private(set) lazy var device = SimPeer(radio: self)
    private(set) var state: CBManagerState = .poweredOn

    private var scanning = false
    private var scanDuplicates = false
    private var scanReported = false
    private var scanGen = 0
    private var probe: NWConnection?
    private var probeBuffer = Data()
    private var backgroundTask: UIBackgroundTaskIdentifier = .invalid
    private var observers: [NSObjectProtocol] = []

    init(host: NWEndpoint.Host, port: NWEndpoint.Port, events: RadioEvents) {
        self.host = host
        self.port = port
        self.events = events
        let nc = NotificationCenter.default
        observers = [
            nc.addObserver(forName: UIApplication.didEnterBackgroundNotification, object: nil,
                           queue: .main) { [weak self] _ in
                MainActor.assumeIsolated { self?.enteredBackground() }
            },
            nc.addObserver(forName: UIApplication.willEnterForegroundNotification, object: nil,
                           queue: .main) { [weak self] _ in
                MainActor.assumeIsolated { self?.endBackgroundTask() }
            },
        ]
        // CoreBluetooth reports its state after the central is made.
        DispatchQueue.main.async { [weak self] in
            self?.events?.radioStateChanged(.poweredOn)
        }
    }

    // MARK: RadioCentral

    func connectPeer(_ p: any RadioPeer) { (p as? SimPeer)?.connect() }
    func cancelPeer(_ p: any RadioPeer) { (p as? SimPeer)?.cancel() }

    func retrievePeers(withIdentifiers ids: [UUID]) -> [any RadioPeer] {
        ids.contains(device.identifier) ? [device] : []
    }

    func retrieveConnectedPeers(withServices uuids: [CBUUID]) -> [any RadioPeer] {
        device.state == .connected ? [device] : []
    }

    func scan(for uuids: [CBUUID], allowDuplicates: Bool) {
        scanning = true
        scanDuplicates = allowDuplicates
        scanReported = false
        scanGen += 1
        probeAdvertisement(gen: scanGen)
    }

    func stopScan() {
        scanning = false
        scanGen += 1
        probe?.cancel(); probe = nil
    }

    // MARK: scanning: ask the device for its advertisement once a second

    private func probeAdvertisement(gen: Int) {
        guard scanning, gen == scanGen else { return }
        guard probe == nil, device.state == .disconnected else { return nextProbe(gen) }
        let c = NWConnection(host: host, port: port, using: SimBLE.tcp())
        probe = c
        probeBuffer = Data()
        c.stateUpdateHandler = { [weak self] st in
            MainActor.assumeIsolated {
                guard let self, self.probe === c else { return }
                switch st {
                case .ready:
                    c.send(content: SimBLE.frame(SimBLE.pAdv), completion: .contentProcessed { _ in })
                    self.probeReceive(c, gen)
                case .waiting, .failed: self.probeDone(c, gen)
                default: break
                }
            }
        }
        c.start(queue: .main)
    }

    private func probeReceive(_ c: NWConnection, _ gen: Int) {
        c.receive(minimumIncompleteLength: 1, maximumLength: 4096) { [weak self] data, _, complete, error in
            MainActor.assumeIsolated {
                guard let self, self.probe === c else { return }
                if let data { self.probeBuffer.append(data) }
                for (t, b) in SimBLE.frames(&self.probeBuffer) where t == SimBLE.dAdv {
                    self.probeDone(c, gen)
                    self.advertisement(b)
                    return
                }
                if complete || error != nil { return self.probeDone(c, gen) }
                self.probeReceive(c, gen)
            }
        }
    }

    private func probeDone(_ c: NWConnection, _ gen: Int) {
        guard probe === c else { return }
        c.cancel()
        probe = nil
        nextProbe(gen)
    }

    private func nextProbe(_ gen: Int) {
        DispatchQueue.main.asyncAfter(deadline: .now() + 1) { [weak self] in
            self?.probeAdvertisement(gen: gen)
        }
    }

    /// D_ADV: advertising, name, manufacturer data, service UUIDs.
    private func advertisement(_ b: Data) {
        var i = b.startIndex
        func byte() -> Int? { guard i < b.endIndex else { return nil }; defer { i += 1 }; return Int(b[i]) }
        func take(_ n: Int) -> Data? {
            guard b.endIndex - i >= n else { return nil }
            defer { i += n }
            return Data(b[i..<(i + n)])
        }
        guard let adv = byte(), let nl = byte(), let nameData = take(nl),
              let ml = byte(), let mfg = take(ml), let n = byte() else { return }
        var uuids: [CBUUID] = []
        for _ in 0..<n { if let u = take(16) { uuids.append(SimBLE.uuid(u)) } }
        guard scanning, adv != 0, uuids.contains(BikeUUID.service) else { return }
        if !scanDuplicates && scanReported { return }
        scanReported = true
        device.advertisedName = String(decoding: nameData, as: UTF8.self)
        // [0xFFFF][0x4F][flags], as BLEManager.advertisesPaired reads it.
        let paired: Bool? = mfg.count >= 4 && mfg[0] == 0xFF && mfg[1] == 0xFF && mfg[2] == 0x4F
            ? mfg[3] & 1 != 0 : nil
        events?.radioDidDiscover(device, paired: paired)
    }

    // MARK: background

    private func enteredBackground() {
        guard backgroundTask == .invalid else { return }
        backgroundTask = UIApplication.shared.beginBackgroundTask(withName: "sim-ble") { [weak self] in
            MainActor.assumeIsolated { self?.endBackgroundTask() }
        }
    }

    private func endBackgroundTask() {
        guard backgroundTask != .invalid else { return }
        UIApplication.shared.endBackgroundTask(backgroundTask)
        backgroundTask = .invalid
    }
}

/// The device, as a CBPeripheral would present it. One TCP connection to the
/// simulator is one BLE connection.
@MainActor
final class SimPeer: RadioPeer {
    let identifier = SimBondStore.peerId
    var name: String? { advertisedName ?? "OpenTrailPaper" }
    var advertisedName: String?
    private(set) var state: CBPeripheralState = .disconnected
    weak var delegate: CBPeripheralDelegate?
    private unowned let radio: SimRadio

    /// A connect is pending or up (until cancel, failure or disconnect).
    private var wanted = false
    private var conn: NWConnection?
    /// TCP is up (a refused / unreachable socket is "not in range").
    private var tcpUp = false
    /// The device answered CONNECT.
    private var linkUp = false
    /// didConnect has been reported for this connection.
    private var reported = false
    private var phoneBonded = false
    private var staleKeys = false
    private var disconnectReason: UInt16?
    private var buffer = Data()
    private(set) var attMtu = 23
    /// Writes without response handed to the socket, not yet sent.
    private var unsent = 0
    private static let sendWindow = 16
    /// Bumped per connection: timers and callbacks of an old one are ignored.
    private var gen = 0

    init(radio: SimRadio) { self.radio = radio }

    private var events: RadioEvents? { radio.events }

    // MARK: connect / cancel

    func connect() {
        guard !wanted else { return }
        wanted = true
        state = .connecting
        attempt()
    }

    func cancel() {
        let had = wanted || conn != nil
        wanted = false
        if let c = conn {
            if tcpUp {
                c.send(content: SimBLE.frame(SimBLE.pDisconnect, Data([0x13])),
                       completion: .contentProcessed { _ in c.cancel() })
            } else {
                c.cancel()
            }
        }
        reset()
        guard had else { return }
        // CoreBluetooth answers a cancel (of an active or a pending connect)
        // with didDisconnect and no error, after the call returns.
        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            self.events?.radioDidDisconnect(self, error: nil)
        }
    }

    private func reset() {
        gen += 1
        conn = nil
        tcpUp = false; linkUp = false; reported = false
        staleKeys = false; disconnectReason = nil
        buffer = Data(); unsent = 0; attMtu = 23
        state = .disconnected
    }

    private func attempt() {
        guard wanted, conn == nil else { return }
        gen += 1
        let g = gen
        let c = NWConnection(host: radio.host, port: radio.port, using: SimBLE.tcp())
        conn = c
        tcpUp = false; linkUp = false; reported = false
        staleKeys = false; disconnectReason = nil; buffer = Data(); unsent = 0
        state = .connecting
        c.stateUpdateHandler = { [weak self] st in
            MainActor.assumeIsolated {
                guard let self, self.gen == g, self.conn === c else { return }
                switch st {
                case .ready: self.tcpReady(c, g)
                case .waiting, .failed:
                    if self.tcpUp { self.linkLost() } else { self.unreachable(c) }
                default: break
                }
            }
        }
        c.start(queue: .main)
    }

    /// Not in range: the connect stays pending, as CoreBluetooth's does.
    private func unreachable(_ c: NWConnection) {
        c.cancel()
        conn = nil
        gen += 1
        let g = gen
        DispatchQueue.main.asyncAfter(deadline: .now() + 1) { [weak self] in
            guard let self, self.gen == g else { return }
            self.attempt()
        }
    }

    private func tcpReady(_ c: NWConnection, _ g: Int) {
        tcpUp = true
        phoneBonded = SimBondStore.bonded
        // bit0 the phone holds a bond, bit1 pair when asked, bit2 confirm the
        // passkey (the code the panel shows, as the rider would).
        let flags: UInt8 = (phoneBonded ? 1 : 0) | 2 | 4
        let name = Data(UIDevice.current.name.utf8.prefix(60))
        var b = Data([1])
        b.append(contentsOf: [UInt8(247 & 0xFF), UInt8(247 >> 8)])
        b.append(flags)
        b.append(SimBondStore.identity)
        b.append(UInt8(name.count)); b.append(name)
        send(SimBLE.pConnect, b)
        receive(c, g)
        // A connect the device never answers failed to establish.
        DispatchQueue.main.asyncAfter(deadline: .now() + 5) { [weak self] in
            guard let self, self.gen == g, !self.linkUp else { return }
            NSLog("[sim-ble] no answer to CONNECT")
            self.linkLost()
        }
    }

    private func receive(_ c: NWConnection, _ g: Int) {
        c.receive(minimumIncompleteLength: 1, maximumLength: 65536) { [weak self] data, _, complete, error in
            MainActor.assumeIsolated {
                guard let self, self.gen == g else { return }
                if let data { self.buffer.append(data) }
                for (t, b) in SimBLE.frames(&self.buffer) {
                    self.frame(t, b)
                    if self.gen != g { return }   // the frame ended the link
                }
                if complete || error != nil { self.linkLost(); return }
                self.receive(c, g)
            }
        }
    }

    private func send(_ type: UInt8, _ body: Data = Data(), sent: (() -> Void)? = nil) {
        guard let c = conn, tcpUp else { return }
        c.send(content: SimBLE.frame(type, body), completion: .contentProcessed { _ in
            if let sent { MainActor.assumeIsolated { sent() } }
        })
    }

    // MARK: frames from the device

    private func frame(_ t: UInt8, _ b: Data) {
        switch t {
        case SimBLE.dConnected:
            guard b.count >= 4 else { return }
            linkUp = true
            attMtu = Int(b.dropFirst(2).le16)
            // The phone's own GATT server: the GAP name the device reads.
            send(SimBLE.pPeerServices, SimBLE.bytes(SimBLE.gapService) + SimBLE.bytes(SimBLE.gapName)
                 + Data([0x02, 0, 0, 0]))
            // Encrypt (with a bond) or pair (without), as iOS does on connect
            // to a bonded device or on "insufficient authentication".
            send(SimBLE.pSecure)
            if phoneBonded {
                let g = gen
                DispatchQueue.main.asyncAfter(deadline: .now() + 10) { [weak self] in
                    guard let self, self.gen == g, !self.reported else { return }
                    NSLog("[sim-ble] encryption timed out")
                    self.end(CBError(.encryptionTimedOut))
                }
            } else {
                didConnect()
            }
        case SimBLE.dSecurity:
            guard b.count >= 4 else { return }
            let encrypted = b[b.startIndex] != 0, bonded = b[b.startIndex + 1] != 0
            NSLog("[sim-ble] security: encrypted=%d bonded=%d", encrypted ? 1 : 0, bonded ? 1 : 0)
            if encrypted {
                if bonded { SimBondStore.bonded = true }
                if !reported { didConnect() }
            } else if phoneBonded {
                // The device lost the keys iOS still holds: iOS drops the link
                // and keeps its bond until the rider forgets the device.
                staleKeys = true
                send(SimBLE.pDisconnect, Data([0x05]))
                end(CBError(.peerRemovedPairingInformation))
            }
            // Pairing that failed leaves the link up; protected requests
            // answer "insufficient authentication" from here on.
        case SimBLE.dDisconnected:
            disconnectReason = b.le16
        case SimBLE.dDiscovered:
            var services: [(CBUUID, CBUUID)] = []
            var i = b.startIndex
            while b.endIndex - i >= 36 {
                services.append((SimBLE.uuid(b[i..<(i + 16)]), SimBLE.uuid(b[(i + 16)..<(i + 32)])))
                i += 36
            }
            let link = services.filter { $0.0 == LinkUUID.service }.map(\.1)
            let offered = link.contains(LinkUUID.rx) && link.contains(LinkUUID.tx)
            events?.radioServicesDiscovered(self, linkOffered: offered)
            if offered { events?.radioLinkFound(self, transport: SimLinkTransport(self)) }
        case SimBLE.dWriteRsp:
            guard b.count >= 17, b[b.startIndex + 16] != 0 else { return }
            events?.radioLinkWriteFailed(Self.attError(b[b.startIndex + 16]))
        case SimBLE.dSubscribeRsp:
            guard b.count >= 17 else { return }
            let st = b[b.startIndex + 16]
            events?.radioLinkNotifyState(isNotifying: st == 0, error: st == 0 ? nil : Self.attError(st))
        case SimBLE.dNotify:
            guard b.count >= 16, SimBLE.uuid(b.prefix(16)) == LinkUUID.tx else { return }
            events?.radioLinkValue(Data(b.dropFirst(16)))
        case SimBLE.dServiceChanged:
            events?.radioServicesModified(self, invalidated: [BikeUUID.service, LinkUUID.service])
        case SimBLE.dPeerRead:
            guard b.count >= 36 else { return }
            let req = Data(b.prefix(4))
            let chr = SimBLE.uuid(b[(b.startIndex + 20)..<(b.startIndex + 36)])
            if chr == SimBLE.gapName {
                send(SimBLE.pPeerReadRsp, req + Data([0]) + Data(UIDevice.current.name.utf8))
            } else {
                send(SimBLE.pPeerReadRsp, req + Data([0x0A]))   // attribute not found
            }
        default:
            break
        }
    }

    private static func attError(_ status: UInt8) -> CBATTError {
        CBATTError(CBATTError.Code(rawValue: Int(status)) ?? .unlikelyError)
    }

    private func didConnect() {
        reported = true
        state = .connected
        NSLog("[sim-ble] connected (mtu %d)", attMtu)
        events?.radioDidConnect(self)
    }

    /// The socket closed (or never answered): the link is gone.
    private func linkLost() {
        let e: CBError
        if staleKeys {
            e = CBError(.peerRemovedPairingInformation)
        } else if !reported {
            e = CBError(.connectionFailed)   // never came up
        } else if let r = disconnectReason {
            e = CBError(r == 0x08 ? .connectionTimeout : .peripheralDisconnected)
        } else {
            e = CBError(.connectionTimeout)  // vanished without a word: supervision timeout
        }
        end(e)
    }

    /// The connection is over: didDisconnect if it had come up, else
    /// didFailToConnect. Either way the pending connect is spent.
    private func end(_ error: CBError) {
        let wasConnected = reported
        NSLog("[sim-ble] link ended: %@ (reason %d, connected=%d)", "\(error.code)",
              Int(disconnectReason ?? 0), wasConnected ? 1 : 0)
        conn?.cancel()
        wanted = false
        reset()
        if wasConnected {
            events?.radioDidDisconnect(self, error: error)
        } else {
            events?.radioDidFailToConnect(self, error: error)
        }
    }

    // MARK: GATT (the link)

    func discoverServices(_ serviceUUIDs: [CBUUID]?) {
        guard state == .connected else { return }
        send(SimBLE.pDiscover)
    }

    var canSendWriteWithoutResponse: Bool { state == .connected && unsent < Self.sendWindow }

    func maximumWriteValueLength(for type: CBCharacteristicWriteType) -> Int {
        type == .withResponse ? 512 : max(20, attMtu - 3)
    }

    func writeRX(_ data: Data, withResponse: Bool) {
        guard state == .connected else { return }
        var b = SimBLE.bytes(LinkUUID.rx)
        b.append(withResponse ? 1 : 0)
        b.append(data)
        if withResponse { send(SimBLE.pWrite, b); return }
        unsent += 1
        let g = gen
        send(SimBLE.pWrite, b) { [weak self] in
            guard let self, self.gen == g else { return }
            let wasFull = self.unsent >= Self.sendWindow
            self.unsent -= 1
            if wasFull { self.events?.radioReadyToSend() }
        }
    }

    func subscribeTX() {
        guard state == .connected else { return }
        send(SimBLE.pSubscribe, SimBLE.bytes(LinkUUID.tx) + Data([1, 0]))
    }

    // The legacy characteristics: never handed out by this radio.
    func readValue(for characteristic: CBCharacteristic) {}
    func writeValue(_ data: Data, for characteristic: CBCharacteristic, type: CBCharacteristicWriteType) {}
    func setNotifyValue(_ enabled: Bool, for characteristic: CBCharacteristic) {}
}

/// The link's RX / TX on the simulator.
@MainActor
final class SimLinkTransport: LinkTransport {
    private weak var peer: SimPeer?
    init(_ p: SimPeer) { peer = p }
    var maxWrite: Int { peer?.maximumWriteValueLength(for: .withoutResponse) ?? 20 }
    var canWrite: Bool { peer?.canSendWriteWithoutResponse ?? false }
    func write(_ fragment: Data, withResponse: Bool) { peer?.writeRX(fragment, withResponse: withResponse) }
    func subscribe() { peer?.subscribeTX() }
}
#endif
