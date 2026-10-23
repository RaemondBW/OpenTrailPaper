#if DEBUG
import Foundation

/// DEBUG only, launch argument `-link-selftest`: runs LinkClient against a
/// simulated device in-process (no Bluetooth) and logs one "[link] selftest"
/// line per check and a summary. Covers the Hello handshake, request ids,
/// multi-page lists, the two-queue writer with flow control, uploads (window,
/// acks, CRC), downloads (acks, resume, whole-file CRC) and Result errors.
@MainActor
final class LinkSelfTest: LinkTransport {
    private static var running: LinkSelfTest?

    static func runIfAsked() {
        guard ProcessInfo.processInfo.arguments.contains("-link-selftest") else { return }
        let t = LinkSelfTest()
        running = t
        t.start()
    }

    let client = LinkClient()
    private var passed = 0, failed = 0
    private var stepNumber = 0
    private var steps: [(String, (@escaping () -> Void) -> Void)] = []

    // MARK: transport (phone -> device)

    /// MTU 185, so fragments and Chunks are realistically small.
    let maxWrite = 182
    private var budget = 3
    var canWrite: Bool { budget > 0 }

    func write(_ fragment: Data, withResponse: Bool) {
        if !withResponse {
            budget -= 1
            if budget == 0 {
                // CoreBluetooth's queue drains, then peripheralIsReady.
                DispatchQueue.main.async { [weak self] in
                    self?.budget = 3
                    self?.client.pump()
                }
            }
        }
        let out = devRx.feed(fragment)
        if let p = out.problem { check(false, "device framing: \(p)") }
        guard let bytes = out.message else { return }
        do {
            let m = try LinkPhoneMsg(serializedBytes: bytes)
            DispatchQueue.main.async { [weak self] in self?.device(m) }
        } catch {
            check(false, "device decode: \(error)")
        }
    }

    func subscribe() {
        DispatchQueue.main.async { [weak self] in
            self?.client.notifyStateChanged(isNotifying: true, error: nil)
        }
    }

    // MARK: simulated device

    private var devRx = LinkReassembler()
    private var devCounter: UInt8 = 0
    private var hellos = 0
    private var upload: (xfer: UInt32, size: Int, crc: UInt32, data: Data, acked: Int)?
    private var uploadEndAt: Int?
    private var controlDuringUpload = false
    private var download: (xfer: UInt32, file: Data, sent: Int, acked: Int, window: Int, req: UInt32)?
    private var acksSeen = 0
    private var corruptCrc = false
    private let file = Data((0..<30_000).map { UInt8(truncatingIfNeeded: $0 &* 7 &+ 3) })

    private func send(_ req: UInt32, _ body: LinkDeviceMsg.OneOf_Body) {
        var m = LinkDeviceMsg()
        m.req = req
        m.body = body
        guard let d: Data = try? m.serializedBytes() else { return }
        // Small fragments so device messages arrive in several pieces.
        for f in LinkFrame.fragments(d, payloadMax: 40, counter: &devCounter) { client.receive(f) }
    }

    private func result(_ req: UInt32, _ code: LinkResultCode) {
        var r = LinkResult()
        r.code = code
        send(req, .result(r))
    }

    private func device(_ m: LinkPhoneMsg) {
        // GetSettings arriving while chunks are still due: the writer put a
        // control message ahead of the bulk queue.
        if case .getSettings? = m.body, let u = upload, uploadEndAt == nil, u.data.count < u.size {
            controlDuringUpload = true
        }
        switch m.body {
        case .hello(let h)?:
            hellos += 1
            check(h.platform == .ios && h.proto == 1 && !h.features.isEmpty, "Hello fields")
            var dh = LinkDeviceHello()
            dh.proto = 1
            dh.firmware = "v9.99"
            dh.features = [.settings, .live, .rides, .firmwareUpdate]
            dh.maxMessage = 4096
            dh.dashMaxPages = 7
            dh.dashMaxText = 2048
            send(m.req, .hello(dh))
            var s = LinkSettings()
            s.ftpW = 250
            send(0, .settings(s))
        case .getSettings?:
            var s = LinkSettings()
            s.ftpW = 251
            s.wheelMm = 2105
            send(m.req, .settings(s))
        case .list(let l)? where l.kind == .listRides:
            for page in 0..<2 {
                var fl = LinkFileList()
                fl.kind = .listRides
                fl.files = (0..<(page == 0 ? 16 : 3)).map { i in
                    var e = LinkFileEntry()
                    e.name = "ride\(page * 16 + i).fit"
                    e.size = UInt32(1000 + i)
                    return e
                }
                fl.last = page == 1
                send(m.req, .fileList(fl))
            }
        case .uploadBegin(let b)?:
            upload = (b.xfer, Int(b.size), b.crc32, Data(), 0)
            uploadEndAt = nil
            var r = LinkUploadReady()
            r.xfer = b.xfer
            r.window = 4096
            send(m.req, .uploadReady(r))
        case .chunk(let c)?:
            guard var u = upload, c.xfer == u.xfer else { return }
            check(Int(c.offset) == u.data.count, "upload chunk in order")
            check(u.data.count + c.data.count - u.acked <= 4096, "upload inside the window")
            u.data.append(c.data)
            if u.data.count - u.acked >= 1024 || u.data.count == u.size {
                u.acked = u.data.count
                var a = LinkTransferAck()
                a.xfer = u.xfer
                a.received = UInt32(u.acked)
                send(0, .transferAck(a))
            }
            upload = u
        case .uploadEnd(let e)?:
            guard let u = upload, e.xfer == u.xfer else { return result(m.req, .resultBadRequest) }
            uploadEndAt = u.data.count
            var d = LinkTransferDone()
            d.xfer = u.xfer
            let ok = u.data.count == u.size && CRC32.checksum(u.data) == u.crc
            d.code = ok ? .resultOk : .resultCrcMismatch
            send(m.req, .transferDone(d))
        case .download(let r)?:
            download = (r.xfer, file, Int(r.offset), Int(r.offset), Int(r.window), m.req)
            var b = LinkDownloadBegin()
            b.xfer = r.xfer
            b.size = UInt32(file.count)
            b.offset = r.offset
            send(m.req, .downloadBegin(b))
            streamDownload()
        case .transferAck(let a)?:
            guard var d = download, a.xfer == d.xfer else { return }
            acksSeen += 1
            d.acked = Int(a.received)
            download = d
            streamDownload()
        case .transferCancel?:
            download = nil
        default:
            result(m.req, .resultUnsupported)
        }
    }

    private func streamDownload() {
        guard var d = download else { return }
        while d.sent < d.file.count && d.sent - d.acked < d.window {
            let n = min(150, d.file.count - d.sent)
            var c = LinkChunk()
            c.xfer = d.xfer
            c.offset = UInt32(d.sent)
            c.data = d.file.subdata(in: d.sent..<(d.sent + n))
            send(0, .chunk(c))
            d.sent += n
        }
        download = d
        if d.sent == d.file.count {
            var t = LinkTransferDone()
            t.xfer = d.xfer
            t.size = UInt32(d.file.count)
            t.crc32 = CRC32.checksum(d.file) ^ (corruptCrc ? 1 : 0)
            send(0, .transferDone(t))
            download = nil
        }
    }

    // MARK: checks

    private func check(_ ok: Bool, _ what: String) {
        if ok { passed += 1 } else { failed += 1; NSLog("[link] selftest FAIL: %@", what) }
    }

    private func start() {
        NSLog("[link] selftest: start")
        var pushes: [LinkDeviceMsg] = []
        client.onPush = { pushes.append($0) }
        steps = [
            ("hello", { next in
                self.client.onReady = { h in
                    self.check(h.firmware == "v9.99" && self.client.has(.rides) && !self.client.has(.mesh),
                               "DeviceHello stored")
                    self.client.onReady = nil
                    next()
                }
                self.client.attach(self)
            }),
            ("push", { next in
                DispatchQueue.main.async {
                    self.check(pushes.contains { if case .settings? = $0.body { return true }; return false },
                               "Settings push after Hello")
                    self.check(self.hellos == 1, "one Hello (got \(self.hellos))")
                    next()
                }
            }),
            ("request", { next in
                self.client.request(.getSettings(LinkGetSettings())) { reply in
                    if case .message(let m) = reply, case .settings(let s)? = m.body {
                        self.check(s.ftpW == 251 && s.hasWheelMm && !s.hasBacklight, "Settings reply, optional fields")
                    } else {
                        self.check(false, "Settings reply")
                    }
                    next()
                    return true
                }
            }),
            ("list", { next in
                var l = LinkListRequest()
                l.kind = .listRides
                var n = 0, pages = 0
                self.client.request(.list(l)) { reply in
                    guard case .message(let m) = reply, case .fileList(let p)? = m.body else {
                        self.check(false, "list page"); next(); return true
                    }
                    n += p.files.count
                    pages += 1
                    guard p.last else { return false }
                    self.check(n == 19 && pages == 2, "two pages, 19 rides (got \(n) in \(pages))")
                    next()
                    return true
                }
            }),
            ("unsupported", { next in
                self.client.request(.agnssInfo(LinkAgnssInfoRequest())) { reply in
                    if case .failure(let e)? = reply.result {
                        self.check(e.code == .resultUnsupported, "Result UNSUPPORTED")
                    } else {
                        self.check(false, "Result for an unknown request")
                    }
                    next()
                    return true
                }
            }),
            ("upload", { next in
                let data = Data((0..<50_000).map { UInt8(truncatingIfNeeded: $0 &* 13 &+ 1) })
                var b = LinkUploadBegin()
                b.target = .firmware
                b.name = "v9.99"
                var allSent = false
                var asked = false
                self.controlDuringUpload = false
                self.client.upload(b, data: data, progress: { sent in
                    // A control message while chunks flow goes out between them.
                    guard sent > 10_000, !asked else { return }
                    asked = true
                    self.client.request(.getSettings(LinkGetSettings())) { _ in true }
                }, allSent: { allSent = true }) { r in
                    if case .failure(let e) = r { self.check(false, "upload: \(e)") }
                    self.check(allSent, "allSent before TransferDone")
                    self.check(self.upload?.data == data, "device got every byte")
                    self.check(self.uploadEndAt == data.count, "UploadEnd after the last chunk")
                    self.check(self.controlDuringUpload, "control message between chunks")
                    next()
                }
            }),
            ("download", { next in
                self.acksSeen = 0
                self.client.download(.ride, name: "a.fit") { r, _ in
                    if case .success(let d) = r { self.check(d == self.file, "download content") }
                    else { self.check(false, "download failed: \(r)") }
                    self.check(self.acksSeen >= 10, "acks every 2 KB (got \(self.acksSeen))")
                    next()
                }
            }),
            ("resume", { next in
                self.client.download(.ride, name: "a.fit", resume: self.file.prefix(10_000)) { r, _ in
                    if case .success(let d) = r { self.check(d == self.file, "resumed download, whole-file CRC") }
                    else { self.check(false, "resume failed: \(r)") }
                    next()
                }
            }),
            ("crc", { next in
                self.corruptCrc = true
                self.client.download(.log, name: "x.log") { r, _ in
                    self.corruptCrc = false
                    if case .failure(.crcMismatch) = r { self.check(true, "") }
                    else { self.check(false, "bad CRC caught (got \(r))") }
                    next()
                }
            }),
        ]
        runNext()
    }

    private func runNext() {
        guard !steps.isEmpty else {
            NSLog("[link] selftest: %d passed, %d failed", passed, failed)
            client.reset()
            Self.running = nil
            return
        }
        let (name, step) = steps.removeFirst()
        stepNumber += 1
        let n = stepNumber
        let timer = Timer.scheduledTimer(withTimeInterval: 10, repeats: false) { [weak self] _ in
            MainActor.assumeIsolated {
                guard let self, self.stepNumber == n else { return }
                self.stepNumber += 1
                self.check(false, "step \(name) timed out")
                self.runNext()
            }
        }
        step {
            guard self.stepNumber == n else { return }
            self.stepNumber += 1
            timer.invalidate()
            DispatchQueue.main.async { self.runNext() }
        }
    }
}
#endif
