import CoreBluetooth
import Foundation

// The device's live state stream: a Nordic-UART-shaped service of its own,
// for the Ride tab. Protocol (v1) in src/ble_server.cpp, "Live state stream".
//
// Phone -> device on RX: [0x01][ver] START / keepalive (every 5 s while the
// Ride tab is on screen and the app is in the foreground), [0x02] STOP,
// [0x10][op] a stopwatch button (1 START/PAUSE, 2 RESET, 3 LAP; only when
// HELLO's caps say the device takes it).
// Device -> phone on TX: [ver][seq] + records [type][len][body], little-endian.
// Unknown record types are skipped and bodies longer than this app knows are
// read from the front, so newer firmware stays readable.
enum LiveUUID {
    static let service = CBUUID(string: "B1C51000-9E0F-4B7A-9C6D-1F2E3A4B5C6D")
    static let rx      = CBUUID(string: "B1C51001-9E0F-4B7A-9C6D-1F2E3A4B5C6D")
    static let tx      = CBUUID(string: "B1C51002-9E0F-4B7A-9C6D-1F2E3A4B5C6D")
    /// The highest protocol version this app parses (sent in START).
    static let protocolVersion: UInt8 = 1
}

/// Everything the device's pages draw, as of the last frame.
struct LiveFrame: Equatable {
    // RIDE (0x01)
    var gpsFix = false
    var recording = false
    var paused = false
    var routeActive = false
    var charging = false
    var hrLive = false
    var powerLive = false
    var cadenceLive = false
    var wheelSpeed = false
    var timeValid = false
    var battery = 0
    var sats = 0
    var speedKmh = 0.0
    var power: Int?
    var power3s: Int?
    var heartRate: Int?
    var cadence: Int?
    var distanceM = 0.0
    var rideTimeS = 0
    var movingTimeS = 0
    var climbM = 0.0
    var gradePct: Double?
    var altitudeM: Double?
    var altitudeBaro = false
    var routeLeftKm = 0.0
    /// Distance over moving time, as the device's AVG SPEED computes it.
    var avgSpeedKmh: Double {
        movingTimeS > 0 ? (distanceM / 1000) / (Double(movingTimeS) / 3600) : 0
    }

    struct Stopwatch: Equatable {
        var running = false
        var showPower = false
        var showCadence = false
        var elapsedMs: UInt32 = 0
        var laps = 0
        var lapMs: UInt32 = 0
        var lastSplitMs: UInt32?
        var lapAvgSpeedKmh: Double?
        var lapAvgPowerW: Int?
        var lapAvgCadence: Int?
    }
    var stopwatch: Stopwatch?

    /// The stopwatch page's lap table (STOPWATCH_LAPS 0x07): its columns and
    /// the newest completed laps, newest first. nil on firmware without the
    /// record (HELLO caps b2). Kept across frames rather than cleared by
    /// `startFrame`: it is sent with every frame (n = 0 with no laps), and a
    /// frame spread over several notifies must not blank the table between them.
    struct StopwatchLaps: Equatable {
        struct Lap: Equatable {
            var number = 0
            var splitMs: UInt32 = 0
            var avgSpeedKmh: Double?
            var avgCadence: Int?
            var avgPowerW: Int?
        }
        var showCadence = false
        var showPower = false
        var laps: [Lap] = []
    }
    var stopwatchLaps: StopwatchLaps?

    struct View: Equatable {
        /// Index into the dashboard config; nil while the device is off its
        /// pages (menu, settings, ride summary).
        var page: Int?
        var pageCount = 0
        /// 0 a dashboard page, 1 the map, 2 something else.
        var screen = 2
    }
    var view: View?

    struct Position: Equatable {
        var lat = 0.0, lon = 0.0
        var courseDeg: Double?
        var current = false
    }
    var position: Position?

    struct Radar: Equatable {
        var live = false
        var distancesM: [Int] = []
    }
    var radarConnected = false
    var radar: Radar?

    struct Music: Equatable {
        /// A player exists on the phone (as the device knows it).
        var present = false
        /// Moving: playing, rewinding or fast-forwarding.
        var playing = false
        /// The device hears the player over AMS (so its remote commands
        /// reach any app, not just Music).
        var ams = false
        var elapsedMs: UInt32 = 0
        /// nil = unknown (a live stream, or no duration reported yet).
        var durationMs: UInt32?
        /// 1.0 normal, 0 paused, negative rewinding.
        var rate = 0.0
    }
    /// The device's player position (MUSIC 0x06); nil on firmware without it.
    var music: Music?

    /// From HELLO: the device's keepalive timeout.
    var keepaliveTimeoutS: Int?
    /// From HELLO: b0 takes stopwatch commands, b1 sends MUSIC, b2 sends
    /// STOPWATCH_LAPS. 0 on the first firmware with the stream (a 2-byte HELLO).
    var caps: UInt8 = 0
    var takesStopwatchCommands: Bool { caps & 0x01 != 0 }
    var sendsStopwatchLaps: Bool { caps & 0x04 != 0 }
    /// Firmware that holds 7 configurable pages plus the map (5 before).
    var holdsEightPages: Bool { caps & 0x08 != 0 }

    /// Apply one notify's records. Sections that are sent only while they
    /// have something to say (position, radar) are cleared by `startFrame`.
    mutating func apply(records d: Data) {
        var i = d.startIndex
        while i + 2 <= d.endIndex {
            let type = d[i], len = Int(d[i + 1])
            let start = i + 2
            guard start + len <= d.endIndex else { return }
            let b = Reader(d[start..<(start + len)])
            switch type {
            case 0x00:
                keepaliveTimeoutS = b.u8(0).map(Int.init)
                caps = b.u8(2) ?? 0
            case 0x01:
                guard len >= 32 else { break }
                let f = b.u16(0) ?? 0
                gpsFix = f & 1 << 0 != 0
                recording = f & 1 << 1 != 0
                paused = f & 1 << 2 != 0
                routeActive = f & 1 << 3 != 0
                charging = f & 1 << 4 != 0
                hrLive = f & 1 << 5 != 0
                powerLive = f & 1 << 6 != 0
                cadenceLive = f & 1 << 7 != 0
                wheelSpeed = f & 1 << 8 != 0
                let gradeValid = f & 1 << 9 != 0
                let altValid = f & 1 << 10 != 0
                altitudeBaro = f & 1 << 11 != 0
                radarConnected = f & 1 << 12 != 0
                timeValid = f & 1 << 14 != 0
                battery = Int(b.u8(2) ?? 0)
                sats = Int(b.u8(3) ?? 0)
                speedKmh = Double(b.u16(4) ?? 0) / 10
                power = b.u16(6).flatMap { $0 == 0xFFFF ? nil : Int($0) }
                power3s = b.u16(8).flatMap { $0 == 0xFFFF ? nil : Int($0) }
                heartRate = b.u8(10).flatMap { $0 == 0xFF ? nil : Int($0) }
                cadence = b.u8(11).flatMap { $0 == 0xFF ? nil : Int($0) }
                distanceM = Double(b.u32(12) ?? 0)
                rideTimeS = Int(b.u32(16) ?? 0)
                movingTimeS = Int(b.u32(20) ?? 0)
                climbM = Double(b.u16(24) ?? 0)
                gradePct = gradeValid ? Double(Int16(bitPattern: b.u16(26) ?? 0)) / 10 : nil
                altitudeM = altValid ? Double(Int16(bitPattern: b.u16(28) ?? 0)) : nil
                routeLeftKm = Double(b.u16(30) ?? 0) / 10
            case 0x02:
                guard len >= 19 else { break }
                let f = b.u8(0) ?? 0
                var s = Stopwatch()
                s.running = f & 1 != 0
                s.showPower = f & 2 != 0
                s.showCadence = f & 4 != 0
                s.elapsedMs = b.u32(1) ?? 0
                s.laps = Int(b.u8(5) ?? 0)
                s.lapMs = b.u32(6) ?? 0
                s.lastSplitMs = b.u32(10).flatMap { $0 == 0 ? nil : $0 }
                s.lapAvgSpeedKmh = b.u16(14).flatMap { $0 == 0xFFFF ? nil : Double($0) / 10 }
                s.lapAvgPowerW = b.u16(16).flatMap { $0 == 0xFFFF ? nil : Int($0) }
                s.lapAvgCadence = b.u8(18).flatMap { $0 == 0xFF ? nil : Int($0) }
                stopwatch = s
            case 0x03:
                guard len >= 3 else { break }
                let p = b.u8(0) ?? 0xFF
                view = View(page: p == 0xFF ? nil : Int(p), pageCount: Int(b.u8(1) ?? 0),
                            screen: Int(b.u8(2) ?? 2))
            case 0x04:
                guard len >= 11 else { break }
                let c = b.u16(8) ?? 0xFFFF
                position = Position(lat: Double(Int32(bitPattern: b.u32(0) ?? 0)) / 1e7,
                                    lon: Double(Int32(bitPattern: b.u32(4) ?? 0)) / 1e7,
                                    courseDeg: c == 0xFFFF ? nil : Double(c) / 10,
                                    current: (b.u8(10) ?? 0) & 1 != 0)
            case 0x05:
                guard len >= 2 else { break }
                let n = min(Int(b.u8(1) ?? 0), len - 2)
                radar = Radar(live: (b.u8(0) ?? 0) & 1 != 0,
                              distancesM: (0..<n).compactMap { b.u8(2 + $0).map(Int.init) })
            case 0x06:
                guard len >= 11 else { break }
                let f = b.u8(0) ?? 0
                let dur = b.u32(5) ?? 0
                music = Music(present: f & 1 != 0, playing: f & 2 != 0, ams: f & 4 != 0,
                              elapsedMs: b.u32(1) ?? 0, durationMs: dur == 0 ? nil : dur,
                              rate: Double(Int16(bitPattern: b.u16(9) ?? 0)) / 100)
            case 0x07:
                guard len >= 2 else { break }
                let cols = b.u8(0) ?? 1
                let n = min(Int(b.u8(1) ?? 0), (len - 2) / 10)
                var t = StopwatchLaps(showCadence: cols & 2 != 0, showPower: cols & 4 != 0)
                t.laps = (0..<n).map { r in
                    let o = 2 + 10 * r
                    return StopwatchLaps.Lap(
                        number: Int(b.u8(o) ?? 0),
                        splitMs: b.u32(o + 1) ?? 0,
                        avgSpeedKmh: b.u16(o + 5).flatMap { $0 == 0xFFFF ? nil : Double($0) / 10 },
                        avgCadence: b.u8(o + 7).flatMap { $0 == 0xFF ? nil : Int($0) },
                        avgPowerW: b.u16(o + 8).flatMap { $0 == 0xFFFF ? nil : Int($0) })
                }
                stopwatchLaps = t
            default:
                break   // a newer record: skipped by its length
            }
            i = start + len
        }
    }

    /// A new frame (new seq): the sections sent only while present go, so a
    /// radar that disconnected or a position that never came doesn't linger.
    mutating func startFrame() {
        position = nil
        radar = nil
    }

    private struct Reader {
        let d: Data
        init(_ d: Data) { self.d = d }
        func u8(_ o: Int) -> UInt8? {
            o < d.count ? d[d.startIndex + o] : nil
        }
        func u16(_ o: Int) -> UInt16? {
            guard o + 2 <= d.count else { return nil }
            let s = d.startIndex + o
            return UInt16(d[s]) | UInt16(d[s + 1]) << 8
        }
        func u32(_ o: Int) -> UInt32? {
            guard o + 4 <= d.count else { return nil }
            let s = d.startIndex + o
            return UInt32(d[s]) | UInt32(d[s + 1]) << 8 | UInt32(d[s + 2]) << 16 | UInt32(d[s + 3]) << 24
        }
    }
}

/// The live stream's latest frame, observed by the Ride tab ALONE.
///
/// Deliberately not a @Published property of BLEManager: a frame lands every
/// second (and on every stopwatch tap), and publishing it there would wake
/// every screen's BLEScope check for numbers only the Ride tab draws.
@MainActor
final class DeviceLive: ObservableObject {
    /// nil = no stream (old firmware, not started yet, or it went quiet);
    /// the Ride tab falls back to the status characteristic.
    @Published private(set) var frame: LiveFrame?
    /// When the frame arrived, for counting a running stopwatch on.
    private(set) var receivedAt = Date.distantPast
    /// When the MUSIC record last changed (every frame while playing): the
    /// moment its elapsed time was true, give or take the link's latency.
    private var musicAt = Date.distantPast

    private var seq: UInt8?
    private var building = LiveFrame()
    private var staleTimer: Timer?

    /// One TX notification.
    func ingest(_ d: Data) {
        guard d.count >= 2, d[d.startIndex] >= 1 else { return }
        let s = d[d.startIndex + 1]
        if s != seq {
            if seq == nil { NSLog("[live] first frame (%d bytes)", d.count) }
            seq = s
            building.startFrame()
        }
        let music = building.music
        building.apply(records: d.dropFirst(2))
        receivedAt = Date()
        if building.music != music { musicAt = receivedAt }
        if frame != building { frame = building }
        if building.holdsEightPages { DashConfig.deviceHoldsMorePages = true }
        armStale()
    }

    /// The stream stopped (STOP, disconnect, background).
    func clear() {
        if frame != nil { NSLog("[live] frame cleared") }
        staleTimer?.invalidate(); staleTimer = nil
        seq = nil
        building = LiveFrame()
        if frame != nil { frame = nil }
    }

    /// Screenshot demo (-demo-live): a frame as if streamed.
    func showDemo(_ f: LiveFrame) {
        if f.music != building.music { musicAt = Date() }
        building = f
        receivedAt = Date()
        if frame != f { frame = f }
    }

    /// The player's position now, in ms: the device's, counted on at its
    /// rate while playing (capped at 10 s past the last frame, so a stream
    /// that stalls doesn't run the bar off on its own) and held while paused,
    /// clamped to the track. nil without a MUSIC record or a player.
    func musicPositionMs(at now: Date) -> UInt32? {
        guard let m = frame?.music, m.present else { return nil }
        var p = Double(m.elapsedMs)
        if m.playing {
            p += min(max(0, now.timeIntervalSince(musicAt)), 10) * 1000 * m.rate
        }
        if let d = m.durationMs { p = min(p, Double(d)) }
        return UInt32(max(0, p))
    }

    /// Frames come every second; a stream silent for 4 s is gone (out of
    /// range, keepalive refused), and stale numbers must not pose as live.
    private func armStale() {
        staleTimer?.invalidate()
        staleTimer = Timer.scheduledTimer(withTimeInterval: 4, repeats: false) { [weak self] _ in
            MainActor.assumeIsolated { NSLog("[live] stale: no frame for 4 s"); self?.clear() }
        }
    }

    /// The stopwatch's time now: the frame's, counted on while it runs.
    func stopwatchElapsedMs(at now: Date) -> UInt32? {
        guard let s = frame?.stopwatch else { return nil }
        guard s.running else { return s.elapsedMs }
        let extra = max(0, now.timeIntervalSince(receivedAt)) * 1000
        return s.elapsedMs &+ UInt32(min(extra, 10_000))
    }
}
