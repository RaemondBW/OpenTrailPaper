import Darwin
import Foundation
import XCTest

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
