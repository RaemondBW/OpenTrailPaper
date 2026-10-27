import Darwin
import Foundation
import XCTest
import UIKit

// Helpers for the firmware-sim UI tests (FirmwareSimTests, sim/IOS.md). The
// Simulator shares the Mac's network, so the test talks TCP straight to the
// sim's control channel, and the app reaches the sim's BLE link (simble)
// through RangeProxy, which stands in for the radio between them.

enum SimEnv {
    private static func port(_ key: String, _ fallback: UInt16) -> UInt16 {
        ProcessInfo.processInfo.environment[key].flatMap(UInt16.init) ?? fallback
    }
    /// The sim's simble port (`otp-sim --ble-port`).
    static var blePort: UInt16 { port("OTP_SIM_BLE_PORT", 7920) }
    /// The sim's control port (`otp-sim --ctl-port`).
    static var ctlPort: UInt16 { port("OTP_SIM_CTL_PORT", 7921) }
    /// Where the app connects: RangeProxy, in front of simble.
    static var radioPort: UInt16 { port("OTP_SIM_RADIO_PORT", 7922) }
    /// The stand-in tile CDN (sim/cdn/tile_cdn.py), started by run_ios_tests.sh.
    static var cdnPort: UInt16 { port("OTP_CDN_PORT", 7923) }
}

// MARK: - sockets

enum Sock {
    static func connect(_ port: UInt16, timeout: TimeInterval = 5) -> Int32 {
        let fd = socket(AF_INET, SOCK_STREAM, 0)
        guard fd >= 0 else { return -1 }
        var one: Int32 = 1
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, socklen_t(MemoryLayout<Int32>.size))
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, socklen_t(MemoryLayout<Int32>.size))
        var addr = sockaddr_in()
        addr.sin_family = sa_family_t(AF_INET)
        addr.sin_port = port.bigEndian
        addr.sin_addr.s_addr = inet_addr("127.0.0.1")
        let r = withUnsafePointer(to: &addr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                Darwin.connect(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
        if r != 0 { close(fd); return -1 }
        setTimeout(fd, timeout)
        return fd
    }

    static func setTimeout(_ fd: Int32, _ secs: TimeInterval) {
        var tv = timeval(tv_sec: Int(secs), tv_usec: Int32((secs - floor(secs)) * 1e6))
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, socklen_t(MemoryLayout<timeval>.size))
    }

    static func sendAll(_ fd: Int32, _ data: Data) -> Bool {
        data.withUnsafeBytes { raw -> Bool in
            var off = 0
            while off < raw.count {
                let n = send(fd, raw.baseAddress! + off, raw.count - off, 0)
                if n <= 0 { return false }
                off += n
            }
            return true
        }
    }
}

// MARK: - Bluetooth off / on

/// The app's SimRadio switches Bluetooth off and on when told by a Darwin
/// notification (SimBLE.bluetoothOffNote / bluetoothOnNote, Sources/SimRadio.swift),
/// as the Settings switch would: CoreBluetooth's state change, and no
/// didDisconnect for the links it takes down.
enum SimBluetooth {
    static func set(on: Bool) {
        let name = on ? "com.raemond.opentrailpaper.sim.bluetooth-on"
                      : "com.raemond.opentrailpaper.sim.bluetooth-off"
        CFNotificationCenterPostNotification(CFNotificationCenterGetDarwinNotifyCenter(),
                                             CFNotificationName(name as CFString), nil, nil, true)
    }
}

// MARK: - the sim's control channel

/// JSON-lines commands to `otp-sim`'s control port (sim/PROTOCOL.md).
enum SimControl {
    struct Failure: Error, CustomStringConvertible { let description: String }

    @discardableResult
    static func call(_ cmd: [String: Any], timeout: TimeInterval = 10) throws -> [String: Any] {
        let fd = Sock.connect(SimEnv.ctlPort, timeout: timeout)
        guard fd >= 0 else { throw Failure(description: "control port \(SimEnv.ctlPort) not reachable") }
        defer { close(fd) }
        var line = try JSONSerialization.data(withJSONObject: cmd)
        line.append(0x0A)
        guard Sock.sendAll(fd, line) else { throw Failure(description: "control send failed") }
        var buf = Data()
        var chunk = [UInt8](repeating: 0, count: 65536)
        while !buf.contains(0x0A) {
            let n = recv(fd, &chunk, chunk.count, 0)
            if n <= 0 { throw Failure(description: "control: no answer to \(cmd["cmd"] ?? "?")") }
            buf.append(contentsOf: chunk[0..<n])
        }
        let first = buf.prefix(upTo: buf.firstIndex(of: 0x0A)!)
        guard let obj = try JSONSerialization.jsonObject(with: first) as? [String: Any] else {
            throw Failure(description: "control: bad answer")
        }
        if obj["ok"] as? Bool != true {
            throw Failure(description: "control \(cmd["cmd"] ?? "?"): \(obj["error"] ?? "failed")")
        }
        return obj
    }

    /// The device's state: screen, page, settings, phone link, bonds…
    static func status() throws -> [String: Any] {
        try call(["cmd": "status"])["status"] as? [String: Any] ?? [:]
    }

    /// Polls `status` until `pred` holds; returns the matching status, or nil
    /// on timeout.
    static func waitStatus(timeout: TimeInterval = 20,
                           _ pred: ([String: Any]) -> Bool) -> [String: Any]? {
        let end = Date().addingTimeInterval(timeout)
        repeat {
            if let s = try? status(), pred(s) { return s }
            Thread.sleep(forTimeInterval: 0.25)
        } while Date() < end
        return nil
    }

    /// The map tiles (or .poi files) on the card, id -> version record ("" for
    /// none), as the device lists them to a phone (control `tiles`).
    static func tiles(poi: Bool = false) throws -> [String: String] {
        let list = try call(["cmd": "tiles", "poi": poi])["tiles"] as? [[String: Any]] ?? []
        var out: [String: String] = [:]
        for t in list { if let id = t["id"] as? String { out[id] = t["version"] as? String ?? "" } }
        return out
    }

    static func bonds(_ s: [String: Any]) -> [String] {
        (s["ble"] as? [String: Any])?["bonds"] as? [String] ?? []
    }
    static func phoneConnected(_ s: [String: Any]) -> Bool { s["phone_connected"] as? Bool == true }
}

// MARK: - the radio between the app and the device

/// A TCP relay between the app (SimRadio) and the sim's simble port: the
/// radio path. In range it relays both ways; `connectFails` models a device
/// at the edge of range: the phone's connect request goes out but the link
/// never establishes (the socket closes before the device sees CONNECT), the
/// case CoreBluetooth reports as didFailToConnect. The device never hears of
/// those attempts.
final class RangeProxy {
    enum Mode { case inRange, connectFails }

    static let shared = RangeProxy(listen: SimEnv.radioPort, upstream: SimEnv.blePort)

    let listenPort: UInt16
    let upstreamPort: UInt16
    private let lock = NSLock()
    private var _mode = Mode.inRange
    private var _failedConnects = 0
    private var listenFd: Int32 = -1

    init(listen: UInt16, upstream: UInt16) {
        listenPort = listen
        upstreamPort = upstream
    }

    var mode: Mode {
        get { lock.withLock { _mode } }
        set { lock.withLock { _mode = newValue } }
    }
    /// Connect attempts that failed in `connectFails` mode.
    var failedConnects: Int { lock.withLock { _failedConnects } }

    func start() throws {
        guard listenFd < 0 else { return }
        let fd = socket(AF_INET, SOCK_STREAM, 0)
        var one: Int32 = 1
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, socklen_t(MemoryLayout<Int32>.size))
        var addr = sockaddr_in()
        addr.sin_family = sa_family_t(AF_INET)
        addr.sin_port = listenPort.bigEndian
        addr.sin_addr.s_addr = inet_addr("127.0.0.1")
        let r = withUnsafePointer(to: &addr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                bind(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
        guard r == 0, listen(fd, 8) == 0 else {
            close(fd)
            throw SimControl.Failure(description: "RangeProxy: can't listen on \(listenPort)")
        }
        listenFd = fd
        let t = Thread { [weak self] in self?.acceptLoop(fd) }
        t.name = "RangeProxy"
        t.start()
    }

    private func acceptLoop(_ lfd: Int32) {
        while true {
            let fd = accept(lfd, nil, nil)
            if fd < 0 { if errno == EINTR { continue }; return }
            var one: Int32 = 1
            setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, socklen_t(MemoryLayout<Int32>.size))
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, socklen_t(MemoryLayout<Int32>.size))
            Thread { [weak self] in self?.serve(fd) }.start()
        }
    }

    private func serve(_ phone: Int32) {
        if mode == .connectFails {
            // Take the connect request, then lose the link before it is up.
            Sock.setTimeout(phone, 1)
            var b = [UInt8](repeating: 0, count: 4096)
            let n = recv(phone, &b, b.count, 0)
            // Count connects (simble CONNECT), not scans (ADV requests).
            if n >= 3 && b[2] == 0x01 { lock.withLock { _failedConnects += 1 } }
            close(phone)
            return
        }
        let device = Sock.connect(upstreamPort)
        guard device >= 0 else { close(phone); return }   // sim down: the link drops
        Sock.setTimeout(device, 0)
        Sock.setTimeout(phone, 0)
        let group = DispatchGroup()
        for (from, to) in [(phone, device), (device, phone)] {
            group.enter()
            Thread {
                var b = [UInt8](repeating: 0, count: 65536)
                while true {
                    let n = recv(from, &b, b.count, 0)
                    if n <= 0 || !Sock.sendAll(to, Data(b[0..<n])) { break }
                }
                // Either side gone: the whole link is.
                shutdown(from, SHUT_RDWR)
                shutdown(to, SHUT_RDWR)
                group.leave()
            }.start()
        }
        group.wait()
        close(phone)
        close(device)
    }
}

// MARK: - the stand-in tile CDN

/// The stand-in for tiles.opentrailpaper.com and Overpass that
/// run_ios_tests.sh starts (sim/cdn/tile_cdn.py), serving a fixture of real
/// tiles (sim/fixtures/appmaps), and its /_ctl/ test controls.
enum TileCdn {
    struct Failure: Error, CustomStringConvertible { let description: String }

    static var base: String { "http://127.0.0.1:\(SimEnv.cdnPort)/" }
    /// What the app is launched with to use it (SimEndpoints, DEBUG only).
    static var launchArguments: [String] {
        ["-sim-tiles", base + "v1/", "-sim-overpass", base + "api/interpreter"]
    }

    /// One fixture cell: its id and its index entry.
    struct Cell { let id: String; let ebmSize: Int; let ebmHash: String; let poiSize: Int; let poiHash: String }
    struct Area { let lat: Double; let lon: Double; let cells: [String: Cell] }

    @discardableResult
    static func request(_ method: String, _ path: String, timeout: TimeInterval = 5) throws -> (Int, Data) {
        guard let url = URL(string: base + path) else { throw Failure(description: "bad path \(path)") }
        var req = URLRequest(url: url)
        req.httpMethod = method
        req.timeoutInterval = timeout
        let done = DispatchSemaphore(value: 0)
        var out: (Int, Data)?
        var err: Error?
        URLSession.shared.dataTask(with: req) { data, resp, e in
            if let e { err = e } else { out = ((resp as? HTTPURLResponse)?.statusCode ?? -1, data ?? Data()) }
            done.signal()
        }.resume()
        _ = done.wait(timeout: .now() + timeout + 1)
        if let err { throw Failure(description: "tile CDN \(path): \(err)") }
        guard let out else { throw Failure(description: "tile CDN \(path): no answer") }
        return out
    }

    private static func json(_ method: String, _ path: String) throws -> Any {
        let (code, data) = try request(method, path)
        guard code == 200 else { throw Failure(description: "tile CDN \(path): HTTP \(code)") }
        return try JSONSerialization.jsonObject(with: data)
    }

    static func reachable() -> Bool { (try? request("GET", "_ctl/log", timeout: 2))?.0 == 200 }

    /// Every request so far (method, path, status, fault).
    static func log() throws -> [[String: Any]] { try json("GET", "_ctl/log") as? [[String: Any]] ?? [] }
    static func paths() throws -> [String] { try log().compactMap { $0["path"] as? String } }
    static func reset() throws { _ = try json("POST", "_ctl/reset") }
    /// Misbehave on paths matching `match` (fnmatch): "truncate", "status:<code>", "close".
    static func fault(_ match: String, _ kind: String) throws {
        _ = try json("POST", "_ctl/fault?match=\(match)&kind=\(kind)")
    }

    /// Publishes a new version of `<id>.<ext>`; returns its new index hash.
    static func bump(_ id: String, ext: String = "ebm") throws -> String {
        let r = try json("POST", "_ctl/bump?id=\(id)&ext=\(ext)") as? [String: Any]
        guard let h = r?["hash"] as? String else { throw Failure(description: "bump \(id): \(String(describing: r))") }
        return h
    }

    /// The fixture's area: its centre and its cells with their CDN index entries.
    static func area() throws -> Area {
        guard let a = try json("GET", "_ctl/area") as? [String: Any],
              let c = a["center"] as? [Double], c.count == 2,
              let cells = a["cells"] as? [[String: Any]] else { throw Failure(description: "bad area.json") }
        var out: [String: Cell] = [:]
        for cell in cells {
            guard let id = cell["id"] as? String else { continue }
            let (code, data) = try request("GET", "_ctl/file?path=/v1/\(id.prefix(6))/index.json")
            guard code == 200, let idx = try JSONSerialization.jsonObject(with: data) as? [String: Any],
                  let e = (idx["cells"] as? [String: Any])?[id] as? [Any], e.count == 4 else {
                throw Failure(description: "no index entry for \(id)")
            }
            out[id] = Cell(id: id, ebmSize: e[0] as? Int ?? -1, ebmHash: e[1] as? String ?? "",
                           poiSize: e[2] as? Int ?? -1, poiHash: e[3] as? String ?? "")
        }
        return Area(lat: c[0], lon: c[1], cells: out)
    }
}

// MARK: - the device's panel

enum Panel {
    /// Dark pixels on the device's map screen away from its fixed furniture
    /// (status bar, compass, zoom buttons, the "NO MAP HERE" box and the
    /// position marker, the scale, the data fields): roads, water, parks.
    /// Writes the screenshot to `file`.
    static func mapInk(_ file: URL) throws -> Int {
        try SimControl.call(["cmd": "screenshot", "path": file.path])
        guard let img = UIImage(contentsOfFile: file.path)?.cgImage else {
            throw SimControl.Failure(description: "no screenshot at \(file.path)")
        }
        let w = img.width, h = img.height
        var px = [UInt8](repeating: 255, count: w * h)
        let ctx = CGContext(data: &px, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w,
                            space: CGColorSpaceCreateDeviceGray(), bitmapInfo: CGImageAlphaInfo.none.rawValue)
        ctx?.draw(img, in: CGRect(x: 0, y: 0, width: w, height: h))
        var dark = 0
        for y in 70..<min(745, h) where !(395...505).contains(y) {
            for x in 0..<min(455, w) where !(y < 150 && x > 440) {
                if px[y * w + x] < 100 { dark += 1 }
            }
        }
        return dark
    }
}
