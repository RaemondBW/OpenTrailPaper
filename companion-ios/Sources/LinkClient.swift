import CoreBluetooth
import Foundation
import SwiftProtobuf

// The phone link: one protobuf channel over BLE (docs/ble-link.md,
// proto/link.proto, generated Link.pb.swift). This file is the session and
// transport: Hello handshake with the legacy fallback, request ids with
// completion handlers and timeouts, the two-queue writer, and the upload /
// download helpers. BLEManager decides what to send and applies what comes
// back; LinkFraming.swift holds the byte framing and CRC-32.
//
// Everything here runs on the main actor (the central's queue is .main).
// State is per connection: `reset()` on disconnect drops pending requests,
// timers and transfers WITHOUT calling their handlers — BLEManager's
// didDisconnect already winds every feature's UI down.

enum LinkUUID {
    static let service = CBUUID(string: "B1C51000-9E0F-4B7A-9C6D-1F2E3A4B5C6D")
    static let rx      = CBUUID(string: "B1C51001-9E0F-4B7A-9C6D-1F2E3A4B5C6D")
    static let tx      = CBUUID(string: "B1C51002-9E0F-4B7A-9C6D-1F2E3A4B5C6D")
}

/// Where the link's fragments go: the RX characteristic (CBLinkTransport),
/// or the DEBUG self-test's simulated device.
@MainActor
protocol LinkTransport: AnyObject {
    /// Bytes one write without response carries (MTU - 3).
    var maxWrite: Int { get }
    /// A write without response can go now; else wait for `pump()`.
    var canWrite: Bool { get }
    func write(_ fragment: Data, withResponse: Bool)
    /// Turn TX notifications on.
    func subscribe()
}

@MainActor
final class CBLinkTransport: LinkTransport {
    private weak var peripheral: CBPeripheral?
    private let rx: CBCharacteristic
    private let tx: CBCharacteristic

    init(_ p: CBPeripheral, rx: CBCharacteristic, tx: CBCharacteristic) {
        peripheral = p; self.rx = rx; self.tx = tx
    }
    var maxWrite: Int { peripheral?.maximumWriteValueLength(for: .withoutResponse) ?? 20 }
    var canWrite: Bool { peripheral?.canSendWriteWithoutResponse ?? false }
    func write(_ fragment: Data, withResponse: Bool) {
        peripheral?.writeValue(fragment, for: rx, type: withResponse ? .withResponse : .withoutResponse)
    }
    func subscribe() { peripheral?.setNotifyValue(true, for: tx) }
}

enum LinkError: Error, CustomStringConvertible {
    /// The device answered with a non-OK code.
    case result(LinkResultCode, String)
    case timeout
    case notReady
    case crcMismatch
    case badData(String)

    var code: LinkResultCode? {
        if case .result(let c, _) = self { return c }
        return nil
    }

    var description: String {
        switch self {
        case .result(let c, let d): return d.isEmpty ? LinkError.text(c) : "\(LinkError.text(c)): \(d)"
        case .timeout: return "no answer"
        case .notReady: return "not connected"
        case .crcMismatch: return "checksum mismatch"
        case .badData(let s): return s
        }
    }

    static func text(_ c: LinkResultCode) -> String {
        switch c {
        case .resultOk: return "ok"
        case .resultUnsupported: return "not supported"
        case .resultBadRequest: return "bad request"
        case .resultBusy: return "busy"
        case .resultRecording: return "a ride is recording"
        case .resultNoSd: return "no SD card"
        case .resultNotFound: return "not found"
        case .resultNoMemory: return "out of memory"
        case .resultIoError: return "SD error"
        case .resultParseError: return "rejected"
        case .resultCrcMismatch: return "checksum mismatch"
        case .resultTooLarge: return "too large"
        case .resultRefused: return "refused"
        case .resultTimeout: return "timed out"
        case .resultCancelled: return "cancelled"
        case .UNRECOGNIZED(let n): return "error \(n)"
        }
    }
}

/// What a request's handler is given: an answer carrying its `req`, or the
/// end of waiting for one.
enum LinkReply {
    case message(LinkDeviceMsg)
    case failed(LinkError)

    /// A Result answer, as success or error. nil for any other message.
    var result: Result<Void, LinkError>? {
        switch self {
        case .failed(let e): return .failure(e)
        case .message(let m):
            guard case .result(let r)? = m.body else { return nil }
            return r.code == .resultOk ? .success(()) : .failure(.result(r.code, r.detail))
        }
    }
}

@MainActor
final class LinkClient {
    enum Phase: Equatable { case idle, subscribing, hello, ready, fallback }

    // MARK: session state

    private(set) var phase: Phase = .idle
    var ready: Bool { phase == .ready }
    private(set) var deviceHello: LinkDeviceHello?
    private(set) var features: Set<LinkFeature> = []
    func has(_ f: LinkFeature) -> Bool { ready && features.contains(f) }

    /// DeviceHello arrived: every feature now goes over the link.
    var onReady: ((LinkDeviceHello) -> Void)?
    /// No DeviceHello after three Hellos (or no link characteristics): use
    /// the legacy characteristics for this connection.
    var onFallback: ((String) -> Void)?
    /// A message that answers no pending request: pushes (req 0) and the
    /// answers to requests sent without a handler.
    var onPush: ((LinkDeviceMsg) -> Void)?

    /// The features this app handles, listed in Hello.
    static let phoneFeatures: [LinkFeature] = [
        .settings, .wheelSize, .live, .stopwatch, .stopwatchLaps, .routes, .navigation,
        .phoneLocation, .rides, .logs, .firmwareUpdate, .sensors, .maps, .mapTiles,
        .mapTileVersions, .mapPoi, .mapLayers, .dash, .mesh, .meshRadioConfig, .mediaArt,
        .mediaArtTone, .mediaTrack, .mediaRemote, .workouts, .unpair,
    ]

    private var transport: LinkTransport?
    private var helloAttempts = 0
    private var helloTimer: Timer?

    // MARK: framing

    private var reassembler = LinkReassembler()
    private var txCounter: UInt8 = 0

    // MARK: requests

    private struct Pending {
        let handler: (LinkReply) -> Bool
        let timeout: TimeInterval?
        var timer: Timer?
    }
    private var nextReq: UInt32 = 1
    private var pending: [UInt32: Pending] = [:]

    // MARK: writer: control first, Chunk only when no control message waits

    private var control: [Data] = []
    /// Fragments of the message going out now; never interleaved.
    private var outgoing: [Data] = []
    private var pumping = false

    // MARK: transfers

    private var nextXfer: UInt32 = 1
    private var uploads: [UInt32: Upload] = [:]
    private var downloads: [UInt32: Download] = [:]
    private var watchdog: Timer?

    // MARK: - Session

    /// The link service's characteristics were discovered: subscribe to TX
    /// and say Hello once that is confirmed (or after 2 s regardless).
    func attach(peripheral p: CBPeripheral, rx: CBCharacteristic?, tx: CBCharacteristic?) {
        guard let rx, let tx else {
            if phase == .idle { fallBack("link characteristics missing") }
            return
        }
        attach(CBLinkTransport(p, rx: rx, tx: tx))
    }

    func attach(_ t: LinkTransport) {
        guard phase == .idle || phase == .subscribing else { return }
        transport = t
        phase = .subscribing
        t.subscribe()
        armHelloTimer(2) { $0.sendHello() }
    }

    /// iOS reported TX's notification state. Confirmation is proof the link
    /// is encrypted, so Hello goes now (again, with its retries reset, if the
    /// 2 s grace already sent one into a link that was still pairing).
    func notifyStateChanged(isNotifying: Bool, error: Error?) {
        guard phase == .subscribing || phase == .hello else { return }
        if let error { NSLog("[link] TX subscribe failed: %@", "\(error)") }
        guard isNotifying else { return }
        helloAttempts = 0
        sendHello()
    }

    /// A write to RX failed (only Hello is written with response).
    func writeFailed(_ error: Error) {
        NSLog("[link] RX write failed: %@", "\(error)")
    }

    private func sendHello() {
        guard phase == .subscribing || phase == .hello, let t = transport else { return }
        helloAttempts += 1
        phase = .hello
        var h = LinkHello()
        h.proto = UInt32(LinkProtoVersion.current.rawValue)
        h.platform = .ios
        h.appVersion = Self.appVersion
        h.features = Self.phoneFeatures
        var m = LinkPhoneMsg()
        m.req = takeReq()
        m.hello = h
        guard let data = encode(m) else { return }
        NSLog("[link] hello #%d (req %u)", helloAttempts, m.req)
        // With response: Hello is the only message in flight until DeviceHello,
        // and a protected write makes iOS finish pairing rather than drop it.
        for frag in LinkFrame.fragments(data, payloadMax: payloadMax(t), counter: &txCounter) {
            t.write(frag, withResponse: true)
        }
        armHelloTimer(3) { me in
            if me.helloAttempts < 3 { me.sendHello() } else { me.fallBack("no DeviceHello after 3 Hellos") }
        }
    }

    private func armHelloTimer(_ secs: TimeInterval, _ fire: @escaping (LinkClient) -> Void) {
        helloTimer?.invalidate()
        helloTimer = Timer.scheduledTimer(withTimeInterval: secs, repeats: false) { [weak self] _ in
            MainActor.assumeIsolated {
                guard let self, self.phase == .subscribing || self.phase == .hello else { return }
                fire(self)
            }
        }
    }

    private func fallBack(_ why: String) {
        helloTimer?.invalidate(); helloTimer = nil
        phase = .fallback
        NSLog("[link] fallback to legacy: %@", why)
        onFallback?(why)
    }

    private func handleHello(_ h: LinkDeviceHello) {
        guard phase != .idle else { return }
        helloTimer?.invalidate(); helloTimer = nil
        // A Hello that iOS held back while pairing can reach the device after
        // the fallback; the device then pushes on the link only, so its
        // DeviceHello wins over the fallback.
        if phase == .fallback { NSLog("[link] DeviceHello after the fallback: using the link") }
        deviceHello = h
        features = Set(h.features)
        let first = phase != .ready
        phase = .ready
        NSLog("[link] ready: fw %@, %d features, max %u, mtu %u%@", h.firmware, h.features.count,
              h.maxMessage, h.mtu, first ? "" : " (again)")
        onReady?(h)
        pump()
    }

    /// The connection ended: forget everything about it. No handler is called.
    func reset() {
        helloTimer?.invalidate(); helloTimer = nil
        watchdog?.invalidate(); watchdog = nil
        for p in pending.values { p.timer?.invalidate() }
        if !pending.isEmpty || !uploads.isEmpty || !downloads.isEmpty {
            NSLog("[link] reset: dropped %d requests, %d uploads, %d downloads",
                  pending.count, uploads.count, downloads.count)
        }
        pending = [:]
        uploads = [:]
        downloads = [:]
        control = []
        outgoing = []
        reassembler.reset()
        txCounter = 0
        nextReq = 1
        nextXfer = 1
        helloAttempts = 0
        deviceHello = nil
        features = []
        transport = nil
        phase = .idle
    }

    static var appVersion: String {
        let i = Bundle.main.infoDictionary
        let v = i?["CFBundleShortVersionString"] as? String ?? "?"
        let b = i?["CFBundleVersion"] as? String ?? "?"
        return "\(v) (\(b))"
    }

    // MARK: - Sending

    private func takeReq() -> UInt32 {
        let r = nextReq
        nextReq = nextReq == UInt32.max ? 1 : nextReq + 1
        return r
    }

    private func encode(_ m: LinkPhoneMsg) -> Data? {
        do {
            let d: Data = try m.serializedBytes()
            if let max = deviceHello?.maxMessage, max > 0, d.count > Int(max) {
                NSLog("[link] message of %d bytes over the device's %u", d.count, max)
            }
            return d
        } catch {
            NSLog("[link] encode failed: %@", "\(error)")
            return nil
        }
    }

    /// Message bytes per fragment: one write without response, less the header.
    private func payloadMax(_ t: LinkTransport) -> Int {
        max(19, t.maxWrite - 1)
    }

    /// Chunk.data size that keeps a whole Chunk message in one fragment.
    private var chunkSize: Int {
        guard let t = transport else { return 180 }
        return max(16, payloadMax(t) - 16)
    }

    /// A message that needs no answer (req 0).
    func send(_ body: LinkPhoneMsg.OneOf_Body) {
        guard ready else { return }
        var m = LinkPhoneMsg()
        m.body = body
        enqueue(m)
    }

    /// A request. `reply` gets every message carrying its req (and timeouts)
    /// and returns true once it has its final answer; each answer restarts
    /// the timeout (nil = wait as long as the connection lasts). Without a
    /// handler the answers go to `onPush`. Returns the req, 0 if not sent.
    @discardableResult
    func request(_ body: LinkPhoneMsg.OneOf_Body, timeout: TimeInterval? = 10,
                 reply: ((LinkReply) -> Bool)? = nil) -> UInt32 {
        guard ready else {
            _ = reply?(.failed(.notReady))
            return 0
        }
        var m = LinkPhoneMsg()
        m.req = takeReq()
        m.body = body
        if let reply {
            pending[m.req] = Pending(handler: reply, timeout: timeout, timer: nil)
            armRequestTimer(m.req)
        }
        enqueue(m)
        return m.req
    }

    private func enqueue(_ m: LinkPhoneMsg) {
        guard let d = encode(m) else { return }
        control.append(d)
        pump()
    }

    private func armRequestTimer(_ req: UInt32) {
        guard var p = pending[req] else { return }
        p.timer?.invalidate()
        if let t = p.timeout {
            p.timer = Timer.scheduledTimer(withTimeInterval: t, repeats: false) { [weak self] _ in
                MainActor.assumeIsolated { self?.requestTimedOut(req) }
            }
        }
        pending[req] = p
    }

    private func requestTimedOut(_ req: UInt32) {
        guard let p = pending.removeValue(forKey: req) else { return }
        NSLog("[link] req %u timed out", req)
        _ = p.handler(.failed(.timeout))
    }

    /// Write as much as CoreBluetooth takes; peripheralIsReady calls again.
    /// Not re-entered: nextBulk() may queue UploadEnd, which pumps.
    func pump() {
        guard ready, !pumping, let t = transport else { return }
        pumping = true
        defer { pumping = false }
        while true {
            if outgoing.isEmpty {
                if !control.isEmpty {
                    outgoing = LinkFrame.fragments(control.removeFirst(), payloadMax: payloadMax(t),
                                                   counter: &txCounter)
                } else if let chunk = nextBulk() {
                    outgoing = LinkFrame.fragments(chunk, payloadMax: payloadMax(t), counter: &txCounter)
                } else {
                    return
                }
            }
            while !outgoing.isEmpty {
                guard t.canWrite else { return }
                t.write(outgoing.removeFirst(), withResponse: false)
            }
        }
    }

    // MARK: - Receiving

    /// One TX notification.
    func receive(_ fragment: Data) {
        let out = reassembler.feed(fragment)
        if let problem = out.problem { NSLog("[link] rx framing: %@", problem) }
        guard let bytes = out.message else { return }
        let m: LinkDeviceMsg
        do {
            m = try LinkDeviceMsg(serializedBytes: bytes)
        } catch {
            NSLog("[link] decode failed (%d bytes): %@", bytes.count, "\(error)")
            return
        }
        dispatch(m)
    }

    private func dispatch(_ m: LinkDeviceMsg) {
        if case .hello(let h)? = m.body {
            if m.req != 0 { pending.removeValue(forKey: m.req)?.timer?.invalidate() }
            handleHello(h)
            return
        }
        guard ready else { return }
        if handleTransfer(m) {
            // Its request (UploadBegin / UploadEnd / DownloadRequest) is answered.
            if m.req != 0 { pending.removeValue(forKey: m.req)?.timer?.invalidate() }
            return
        }
        if m.req != 0, let p = pending[m.req] {
            if p.handler(.message(m)) {
                pending.removeValue(forKey: m.req)?.timer?.invalidate()
            } else {
                armRequestTimer(m.req)
            }
            return
        }
        onPush?(m)
    }

    // MARK: - Uploads

    private final class Upload {
        let xfer: UInt32
        let data: Data
        let label: String
        var window = 16 * 1024
        var ready = false
        var sent = 0
        var acked = 0
        var endSent = false
        var beginReq: UInt32 = 0
        var endReq: UInt32 = 0
        var lastActivity = Date()
        var progress: ((Int) -> Void)?
        var allSent: (() -> Void)?
        var done: (Result<LinkTransferDone, LinkError>) -> Void

        init(xfer: UInt32, data: Data, label: String,
             done: @escaping (Result<LinkTransferDone, LinkError>) -> Void) {
            self.xfer = xfer; self.data = data; self.label = label; self.done = done
        }
    }

    /// Send `data` to `target` (UploadBegin → UploadReady → Chunks → UploadEnd →
    /// TransferDone). `begin` fills the target-specific fields; xfer, size and
    /// CRC are set here. `progress`: bytes written; `allSent`: UploadEnd went
    /// out (the device now checks and saves); `done`: TransferDone or the
    /// error. Returns the xfer (for `cancel`), 0 if not ready.
    @discardableResult
    func upload(_ begin: LinkUploadBegin, data: Data,
                progress: ((Int) -> Void)? = nil, allSent: (() -> Void)? = nil,
                done: @escaping (Result<LinkTransferDone, LinkError>) -> Void) -> UInt32 {
        guard ready else { done(.failure(.notReady)); return 0 }
        let xfer = takeXfer()
        var b = begin
        b.xfer = xfer
        b.size = UInt32(data.count)
        b.crc32 = CRC32.checksum(data)
        let label = "\(b.target) \(b.name)"
        let u = Upload(xfer: xfer, data: data, label: label, done: done)
        u.progress = progress
        u.allSent = allSent
        uploads[xfer] = u
        NSLog("[link] upload %u begin: %@, %d bytes", xfer, label, data.count)
        armWatchdog()
        u.beginReq = request(.uploadBegin(b), timeout: 15) { [weak self] reply in
            // UploadReady is routed by xfer (handleTransfer); here only errors.
            if case .failure(let e)? = reply.result { self?.finishUpload(xfer, .failure(e)) }
            return true
        }
        return xfer
    }

    private func takeXfer() -> UInt32 {
        let x = nextXfer
        nextXfer = nextXfer == UInt32.max ? 1 : nextXfer + 1
        return x
    }

    /// The next Chunk to write, if an upload has one inside its window; also
    /// queues UploadEnd once the last byte is out.
    private func nextBulk() -> Data? {
        for u in uploads.values.sorted(by: { $0.xfer < $1.xfer }) where u.ready && !u.endSent {
            if u.sent >= u.data.count {
                u.endSent = true
                var end = LinkUploadEnd()
                end.xfer = u.xfer
                let xfer = u.xfer
                // TransferDone may wait for the SD card (a ride recording): no timeout.
                u.endReq = request(.uploadEnd(end), timeout: nil) { [weak self] reply in
                    if case .failure(let e)? = reply.result { self?.finishUpload(xfer, .failure(e)) }
                    return true
                }
                u.allSent?()
                continue
            }
            let n = min(chunkSize, u.data.count - u.sent)
            guard u.sent + n - u.acked <= u.window else { continue }
            var c = LinkChunk()
            c.xfer = u.xfer
            c.offset = UInt32(u.sent)
            let s = u.data.startIndex + u.sent
            c.data = u.data.subdata(in: s..<(s + n))
            var m = LinkPhoneMsg()
            m.chunk = c
            u.sent += n
            u.lastActivity = Date()
            u.progress?(u.sent)
            return encode(m)
        }
        return nil
    }

    private func finishUpload(_ xfer: UInt32, _ r: Result<LinkTransferDone, LinkError>) {
        guard let u = uploads.removeValue(forKey: xfer) else { return }
        dropPending(u.beginReq)
        dropPending(u.endReq)
        switch r {
        case .success: NSLog("[link] upload %u done: %@", xfer, u.label)
        case .failure(let e): NSLog("[link] upload %u failed: %@ (%@)", xfer, u.label, e.description)
        }
        u.done(r)
    }

    private func dropPending(_ req: UInt32) {
        if req != 0 { pending.removeValue(forKey: req)?.timer?.invalidate() }
    }

    /// Stop a transfer (either direction) and forget it; its handler is not
    /// called.
    func cancel(_ xfer: UInt32) {
        if let u = uploads.removeValue(forKey: xfer) {
            dropPending(u.beginReq); dropPending(u.endReq)
        } else if let d = downloads.removeValue(forKey: xfer) {
            dropPending(d.req)
        } else {
            return
        }
        NSLog("[link] transfer %u cancelled", xfer)
        var c = LinkTransferCancel()
        c.xfer = xfer
        c.reason = .resultCancelled
        send(.transferCancel(c))
    }

    // MARK: - Downloads

    private final class Download {
        let xfer: UInt32
        let label: String
        /// Bytes the phone already had (a resumed ride), for the whole-file CRC.
        var prefix: Data
        var buf = Data()
        var begun = false
        var req: UInt32 = 0
        var size = 0
        var ackedAt = 0
        var lastActivity = Date()
        var progress: ((Int, Int) -> Void)?
        var done: (Result<Data, LinkError>, Data) -> Void
        var next: Int { prefix.count + buf.count }

        init(xfer: UInt32, label: String, prefix: Data,
             done: @escaping (Result<Data, LinkError>, Data) -> Void) {
            self.xfer = xfer; self.label = label; self.prefix = prefix; self.done = done
        }
    }

    /// Fetch a file: DownloadRequest → DownloadBegin → Chunks (acked every
    /// 2 KB) → TransferDone, then the whole file's size and CRC are checked.
    /// `resume`: the start of the file already in hand; only the rest is asked
    /// for. `progress`: (bytes of the file in hand, file size); `done` gets the
    /// whole file, or the error and the start of the file collected so far
    /// (empty when what arrived can't be trusted), to resume from.
    /// Returns the xfer, 0 if not ready.
    @discardableResult
    func download(_ source: LinkSource, name: String, resume: Data = Data(),
                  progress: ((Int, Int) -> Void)? = nil,
                  done: @escaping (Result<Data, LinkError>, Data) -> Void) -> UInt32 {
        guard ready else { done(.failure(.notReady), resume); return 0 }
        let xfer = takeXfer()
        let label = "\(source) \(name)"
        let d = Download(xfer: xfer, label: label, prefix: resume, done: done)
        d.progress = progress
        downloads[xfer] = d
        var r = LinkDownloadRequest()
        r.xfer = xfer
        r.source = source
        r.name = name
        r.offset = UInt32(resume.count)
        r.window = 8 * 1024
        NSLog("[link] download %u begin: %@ from %d", xfer, label, resume.count)
        armWatchdog()
        d.req = request(.download(r), timeout: 15) { [weak self] reply in
            // DownloadBegin is routed by xfer (handleTransfer); here only errors.
            if case .failure(let e)? = reply.result { self?.finishDownload(xfer, .failure(e), keep: true) }
            return true
        }
        return xfer
    }

    /// `keep`: what arrived is good, hand it back for a resume.
    private func finishDownload(_ xfer: UInt32, _ r: Result<Data, LinkError>, keep: Bool = false) {
        guard let d = downloads.removeValue(forKey: xfer) else { return }
        dropPending(d.req)
        switch r {
        case .success(let data): NSLog("[link] download %u done: %@, %d bytes", xfer, d.label, data.count)
        case .failure(let e): NSLog("[link] download %u failed at %d: %@ (%@)", xfer, d.next, d.label, e.description)
        }
        d.done(r, keep ? d.prefix + d.buf : Data())
    }

    private func ack(_ d: Download) {
        var a = LinkTransferAck()
        a.xfer = d.xfer
        a.received = UInt32(d.next)
        d.ackedAt = d.next
        send(.transferAck(a))
    }

    // MARK: - Transfer messages (routed by xfer)

    /// True when `m` belonged to a running transfer.
    private func handleTransfer(_ m: LinkDeviceMsg) -> Bool {
        switch m.body {
        case .uploadReady(let r)?:
            guard let u = uploads[r.xfer] else { return false }
            u.ready = true
            if r.window > 0 { u.window = Int(r.window) }
            u.lastActivity = Date()
            pump()
            return true
        case .transferAck(let a)?:
            if let u = uploads[a.xfer] {
                u.acked = max(u.acked, Int(a.received))
                u.lastActivity = Date()
                pump()
                return true
            }
            return downloads[a.xfer] != nil
        case .downloadBegin(let b)?:
            guard let d = downloads[b.xfer] else { return false }
            if Int(b.offset) != d.prefix.count {
                guard b.offset == 0 else {
                    failDownload(d, .badData("device resumed at \(b.offset), asked \(d.prefix.count)"))
                    return true
                }
                d.prefix = Data()   // the device starts over
            }
            dropPending(d.req)
            d.begun = true
            d.size = Int(b.size)
            d.ackedAt = d.next
            d.lastActivity = Date()
            d.progress?(d.next, d.size)
            return true
        case .chunk(let c)?:
            guard let d = downloads[c.xfer] else { return false }
            guard d.begun, Int(c.offset) == d.next else {
                failDownload(d, .badData("chunk at \(c.offset), expected \(d.next)"))
                return true
            }
            d.buf.append(c.data)
            d.lastActivity = Date()
            if d.next - d.ackedAt >= 2048 { ack(d) }
            d.progress?(d.next, d.size)
            return true
        case .transferDone(let t)?:
            if uploads[t.xfer] != nil {
                finishUpload(t.xfer, t.code == .resultOk ? .success(t) : .failure(.result(t.code, t.detail)))
                return true
            }
            guard let d = downloads[t.xfer] else { return false }
            guard t.code == .resultOk else {
                finishDownload(t.xfer, .failure(.result(t.code, t.detail)), keep: true)
                return true
            }
            var file = d.prefix
            file.append(d.buf)
            if t.size != 0 && Int(t.size) != file.count {
                finishDownload(t.xfer, .failure(.badData("got \(file.count) of \(t.size) bytes")))
            } else if CRC32.checksum(file) != t.crc32 {
                finishDownload(t.xfer, .failure(.crcMismatch))
            } else {
                finishDownload(t.xfer, .success(file))
            }
            return true
        case .transferCancel(let c)?:
            if uploads[c.xfer] != nil {
                finishUpload(c.xfer, .failure(.result(c.reason, "cancelled by the device")))
                return true
            }
            if downloads[c.xfer] != nil {
                finishDownload(c.xfer, .failure(.result(c.reason, "cancelled by the device")), keep: true)
                return true
            }
            return false
        default:
            return false
        }
    }

    /// A download gone wrong on this side: tell the device and give up.
    private func failDownload(_ d: Download, _ e: LinkError) {
        var c = LinkTransferCancel()
        c.xfer = d.xfer
        c.reason = e.code ?? .resultBadRequest
        send(.transferCancel(c))
        finishDownload(d.xfer, .failure(e), keep: true)
    }

    /// Stalled transfers: an upload with nothing sent or acked for 20 s
    /// (window full, or never got UploadReady), a download with no data for
    /// 20 s (the device gives up after 10 s without an ack, so by then it is
    /// gone). An upload waiting for TransferDone waits: the device may be
    /// waiting for the SD card.
    private func armWatchdog() {
        guard watchdog == nil else { return }
        watchdog = Timer.scheduledTimer(withTimeInterval: 2, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated { self?.checkStalls() }
        }
    }

    private func checkStalls() {
        let now = Date()
        for u in Array(uploads.values) where !u.endSent && now.timeIntervalSince(u.lastActivity) > 20 {
            var c = LinkTransferCancel()
            c.xfer = u.xfer
            c.reason = .resultTimeout
            send(.transferCancel(c))
            finishUpload(u.xfer, .failure(.timeout))
        }
        for d in Array(downloads.values) where now.timeIntervalSince(d.lastActivity) > 20 {
            failDownload(d, .timeout)
        }
        if uploads.isEmpty && downloads.isEmpty {
            watchdog?.invalidate(); watchdog = nil
        }
    }
}
