import Foundation

// The device's live state for the Ride tab: LinkLiveFrame messages on the
// phone link (docs/ble-link.md, "Live state"), as the app's own model. The
// device fills only the parts the phone subscribed to (BLEManager's
// LiveSubscribe, chosen from the page in view), and an absent optional field
// is "no reading".

/// Everything the device's pages draw, as of the last frame.
struct LiveFrame: Equatable {
    // RIDE
    var gpsFix = false
    var recording = false
    var paused = false
    var routeActive = false
    var charging = false
    var hrLive = false
    var powerLive = false
    var cadenceLive = false
    /// The device's "Show offline": a field whose sensor isn't live is greyed
    /// (true) or left off the page (false). nil = firmware that doesn't say.
    var showOffline: Bool?
    var wheelSpeed = false
    var timeValid = false
    var battery = 0
    var sats = 0
    /// nil = no speed reading.
    var speedKmh: Double?
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

    /// The stopwatch page's lap table: its columns and the newest completed
    /// laps, newest first. nil on firmware without FEATURE_STOPWATCH_LAPS.
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
    /// The device's player position; nil unless MUSIC is subscribed.
    var music: Music?

    /// The device takes StopwatchCommand (FEATURE_STOPWATCH).
    var takesStopwatchCommands = false
}

extension LiveFrame {
    /// Is there a source for this dashboard field right now? The device's
    /// dashFieldAvailable (ui_render.cpp), on what the link carries: a
    /// sensor's value is present exactly while the device counts it live.
    func fieldAvailable(_ field: String) -> Bool {
        switch field {
        case "power", "power3s": return powerLive
        case "hr": return hrLive
        case "cadence": return cadenceLive
        case "routeleft": return routeActive
        default: return true
        }
    }

    /// One link frame. `laps`: the device has FEATURE_STOPWATCH_LAPS;
    /// `stopwatchCommands`: FEATURE_STOPWATCH.
    init(link f: LinkLiveFrame, laps: Bool, stopwatchCommands: Bool) {
        self.init()
        takesStopwatchCommands = stopwatchCommands
        if f.hasRide {
            let r = f.ride
            gpsFix = r.fix
            recording = r.recording
            paused = r.autoPaused
            routeActive = r.routeActive
            charging = r.charging
            hrLive = r.hasHrBpm
            powerLive = r.hasPowerW
            cadenceLive = r.hasCadenceRpm
            wheelSpeed = r.speedFromWheel
            timeValid = r.timeValid
            battery = Int(r.batteryPct)
            sats = Int(r.sats)
            speedKmh = r.hasSpeedDkmh ? Double(r.speedDkmh) / 10 : nil
            power = r.hasPowerW ? Int(r.powerW) : nil
            power3s = r.hasPower3SW ? Int(r.power3SW) : nil
            heartRate = r.hasHrBpm ? Int(r.hrBpm) : nil
            cadence = r.hasCadenceRpm ? Int(r.cadenceRpm) : nil
            distanceM = Double(r.distanceM)
            rideTimeS = Int(r.elapsedS)
            movingTimeS = Int(r.movingS)
            climbM = Double(r.climbM)
            gradePct = r.hasGradeDpct ? Double(r.gradeDpct) / 10 : nil
            altitudeM = r.hasAltitudeM ? Double(r.altitudeM) : nil
            altitudeBaro = r.altitudeBaro
            routeLeftKm = r.hasRouteLeftM ? Double(r.routeLeftM) / 1000 : 0
            radarConnected = r.radarConnected
            showOffline = r.hasShowOffline ? r.showOffline : nil
        }
        if f.hasStopwatch {
            let w = f.stopwatch
            var s = Stopwatch()
            s.running = w.running
            s.showPower = w.showsPower
            s.showCadence = w.showsCadence
            s.elapsedMs = w.elapsedMs
            s.laps = Int(w.laps)
            s.lapMs = w.lapMs
            s.lastSplitMs = w.hasLastSplitMs ? w.lastSplitMs : nil
            s.lapAvgSpeedKmh = w.hasLapSpeedDkmh ? Double(w.lapSpeedDkmh) / 10 : nil
            s.lapAvgPowerW = w.hasLapPowerW ? Int(w.lapPowerW) : nil
            s.lapAvgCadence = w.hasLapCadenceRpm ? Int(w.lapCadenceRpm) : nil
            stopwatch = s
            if laps {
                stopwatchLaps = StopwatchLaps(
                    showCadence: w.showsCadence, showPower: w.showsPower,
                    laps: w.recent.map {
                        .init(number: Int($0.number), splitMs: $0.splitMs,
                              avgSpeedKmh: $0.hasSpeedDkmh ? Double($0.speedDkmh) / 10 : nil,
                              avgCadence: $0.hasCadenceRpm ? Int($0.cadenceRpm) : nil,
                              avgPowerW: $0.hasPowerW ? Int($0.powerW) : nil)
                    })
            }
        }
        if f.hasView {
            view = View(page: f.view.hasPage ? Int(f.view.page) : nil,
                        pageCount: Int(f.view.pageCount), screen: f.view.screen.rawValue)
        }
        if f.hasPosition {
            let p = f.position
            position = Position(lat: Double(p.latE7) / 1e7, lon: Double(p.lonE7) / 1e7,
                                courseDeg: p.hasCourseDdeg ? Double(p.courseDdeg) / 10 : nil,
                                current: p.current)
        }
        if f.hasRadar {
            radar = Radar(live: f.radar.live, distancesM: f.radar.distancesM.map(Int.init))
        }
        if f.hasMusic {
            let m = f.music
            music = Music(present: m.hasPlayer_p, playing: m.playing, ams: m.fromAms,
                          elapsedMs: m.elapsedMs, durationMs: m.durationMs == 0 ? nil : m.durationMs,
                          rate: Double(m.ratePct) / 100)
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
    /// nil = no stream (firmware without the link, not started yet, or it
    /// went quiet); the Ride tab then falls back to the status values.
    @Published private(set) var frame: LiveFrame?
    /// When the frame arrived, for counting a running stopwatch on.
    private(set) var receivedAt = Date.distantPast
    /// When the MUSIC part last changed (every frame while playing): the
    /// moment its elapsed time was true, give or take the link's latency.
    private var musicAt = Date.distantPast
    private var staleTimer: Timer?

    /// One LiveFrame from the link.
    func ingest(_ f: LinkLiveFrame, laps: Bool, stopwatchCommands: Bool) {
        let next = LiveFrame(link: f, laps: laps, stopwatchCommands: stopwatchCommands)
        receivedAt = Date()
        if next.music != frame?.music { musicAt = receivedAt }
        if frame != next { frame = next }
        armStale()
    }

    /// The stream stopped (disconnect).
    func clear() {
        staleTimer?.invalidate(); staleTimer = nil
        if frame != nil { frame = nil }
    }

    /// Screenshot demo (-demo-live): a frame as if streamed.
    func showDemo(_ f: LiveFrame) {
        if f.music != frame?.music { musicAt = Date() }
        receivedAt = Date()
        if frame != f { frame = f }
    }

    /// The player's position now, in ms: the device's, counted on at its
    /// rate while playing (capped at 10 s past the last frame, so a stream
    /// that stalls doesn't run the bar off on its own) and held while paused,
    /// clamped to the track. nil without a MUSIC part or a player.
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
    /// range), and stale numbers must not pose as live.
    private func armStale() {
        staleTimer?.invalidate()
        staleTimer = Timer.scheduledTimer(withTimeInterval: 4, repeats: false) { [weak self] _ in
            MainActor.assumeIsolated { self?.clear() }
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
