import XCTest

/// The Rides tab and the link's edges, through the real app against the real
/// firmware (sim/IOS.md): the device's ride list in full, a ride downloaded
/// and read as the device summarised it, deleted on the device from the app,
/// recorded while the app was closed, a download cut by a link drop and
/// fetched again, and a storm of drops. sim/run_ios_tests.sh runs this class
/// against a factory-fresh otp-sim of its own (test15 pairs the phone), with
/// FIT files the firmware's own FitWriter wrote (`otp-sim card-tool fit`) in
/// OTP_RIDE_FIXTURES, which the tests put on the device's card.
final class FirmwareSimAppFlowTests: XCTestCase {
    private var app: XCUIApplication!

    override func setUpWithError() throws {
        continueAfterFailure = false
        do {
            _ = try SimControl.status()
        } catch {
            throw XCTSkip("firmware sim not running on control port \(SimEnv.ctlPort): \(error)")
        }
        try RangeProxy.shared.start()
        RangeProxy.shared.mode = .inRange
        app = XCUIApplication()
        app.launchArguments = ["-didOnboard", "YES", "-sim-ble", "127.0.0.1:\(SimEnv.radioPort)"]
    }

    override func tearDown() {
        RangeProxy.shared.mode = .inRange
        _ = try? SimControl.call(["cmd": "link_pace", "interval_ms": 0, "packets": 0, "window": 32])
        _ = try? SimControl.call(["cmd": "speed", "factor": 1])
        app?.terminate()
    }

    // MARK: fixtures

    /// One of the FIT files run_ios_tests.sh had card-tool write, by role:
    /// "summary" (the newest: an hour at 27 km/h, 145 bpm, 210 W), "long"
    /// (four hours, ~350 KB), and "history-NN" (40 short rides, a day apart).
    private struct Fixture {
        let role: String
        let name: String
        let start: TimeInterval
        let seconds: Int
        let kmh: Double
        let hr: Int
        let watts: Int
        var url: URL? { SimEnv.rideFixtures?.appendingPathComponent("rides/\(name)") }
    }

    /// The fixtures' manifest (ride-fixtures/manifest.txt: "role name start seconds kmh hr watts").
    private func fixtures() throws -> [String: Fixture] {
        guard let dir = SimEnv.rideFixtures,
              let text = try? String(contentsOf: dir.appendingPathComponent("manifest.txt"), encoding: .utf8) else {
            throw XCTSkip("no ride fixtures (OTP_RIDE_FIXTURES; run sim/run_ios_tests.sh)")
        }
        var out: [String: Fixture] = [:]
        for line in text.split(separator: "\n") {
            let f = line.split(separator: " ").map(String.init)
            guard f.count == 7, let start = Double(f[2]), let secs = Int(f[3]), let kmh = Double(f[4]),
                  let hr = Int(f[5]), let w = Int(f[6]) else { continue }
            out[f[0]] = Fixture(role: f[0], name: f[1], start: start, seconds: secs, kmh: kmh, hr: hr, watts: w)
        }
        return out
    }

    private var cardRides: URL? { SimEnv.deviceDir?.appendingPathComponent("sd/rides") }

    /// Puts fixture rides on the device's card (the device lists its card
    /// afresh on every request).
    private func putOnCard(_ rides: [Fixture]) throws {
        guard let dir = cardRides else { throw XCTSkip("OTP_SIM_DIR not set") }
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        for r in rides {
            guard let src = r.url else { continue }
            let dst = dir.appendingPathComponent(r.name)
            if !FileManager.default.fileExists(atPath: dst.path) { try FileManager.default.copyItem(at: src, to: dst) }
        }
    }

    private func onCard() -> [String] {
        guard let dir = cardRides else { return [] }
        return ((try? FileManager.default.contentsOfDirectory(atPath: dir.path)) ?? []).filter { $0.hasSuffix(".fit") }.sorted()
    }

    // MARK: the app

    private var connectedLabel: XCUIElement {
        app.descendants(matching: .any).matching(NSPredicate(format: "label BEGINSWITH 'CONNECTED'")).firstMatch
    }

    private var refusalAlert: XCUIElement {
        app.alerts.matching(NSPredicate(format: "label CONTAINS[c] %@", "isn't accepting this iPhone")).firstMatch
    }

    @discardableResult
    private func launchConnected(extra: [String] = [], file: StaticString = #filePath, line: UInt = #line) -> [String: Any] {
        app.launchArguments += extra
        app.launch()
        let st = SimControl.waitStatus(timeout: 30) { SimControl.phoneConnected($0) }
        XCTAssertNotNil(st, "device never saw the phone connect", file: file, line: line)
        XCTAssertTrue(connectedLabel.waitForExistence(timeout: 20), "app never showed CONNECTED", file: file, line: line)
        return st ?? [:]
    }

    private func openRides() {
        app.buttons["Rides"].firstMatch.tap()
    }

    /// The Rides header's count ("41 ON DEVICE"), once the device has answered.
    private func waitForDeviceCount(_ n: Int, timeout: TimeInterval = 30, file: StaticString = #filePath, line: UInt = #line) {
        let header = app.staticTexts["\(n) ON DEVICE"]
        XCTAssertTrue(header.waitForExistence(timeout: timeout),
                      "the Rides header never said \(n) ON DEVICE (shows: \(rideHeaderText()))", file: file, line: line)
    }

    private func rideHeaderText() -> String {
        app.staticTexts.matching(NSPredicate(format: "label ENDSWITH 'ON DEVICE' OR label == 'SYNCED' OR label == 'OFFLINE'"))
            .allElementsBoundByIndex.map(\.label).joined(separator: " | ")
    }

    /// A ride's row: its button, found by the date the row shows (as the app
    /// formats it from the file name, in this phone's locale and zone).
    private func row(_ name: String) -> XCUIElement {
        let base = name.replacingOccurrences(of: ".fit", with: "")
        let f = DateFormatter()
        f.dateFormat = "yyyyMMdd-HHmmss"
        let d = f.date(from: base)!
        let date = d.formatted(.dateTime.month(.abbreviated).day()) + " · " + d.formatted(.dateTime.hour().minute())
        // The featured card shows the date in capitals.
        return app.buttons.matching(NSPredicate(format: "label CONTAINS[c] %@", date)).firstMatch
    }

    /// Rows of rides that are on the device only (their tag: ON DEVICE).
    private var onDeviceRows: XCUIElementQuery {
        app.buttons.matching(NSPredicate(format: "label CONTAINS[c] 'On device'"))
    }

    /// On the Rides tab: its header button says the app is connected.
    private var ridesConnected: XCUIElement { app.buttons["Check for new rides"] }

    /// The value of one of the ride page's stat cells ("DISTANCE, 26.4, KM" → "26.4").
    private func stat(_ label: String) -> String? {
        let cell = app.descendants(matching: .any).matching(NSPredicate(format: "label BEGINSWITH %@", label + ",")).firstMatch
        guard cell.waitForExistence(timeout: 5) else { return nil }
        let parts = cell.label.components(separatedBy: ", ")
        return parts.count > 1 ? parts[1] : nil
    }

    /// The device console since `seq`, lines matching `pattern`.
    private func deviceLog(since seq: Int, matching pattern: String) throws -> [String] {
        let r = try SimControl.call(["cmd": "log", "since": seq])
        let lines = (r["lines"] as? [[Any]] ?? []).compactMap { $0.count > 1 ? $0[1] as? String : nil }
        let re = try NSRegularExpression(pattern: pattern)
        return lines.filter { re.firstMatch(in: $0, range: NSRange($0.startIndex..., in: $0)) != nil }
    }

    private func logSeq() throws -> Int { try SimControl.call(["cmd": "seq"])["seq"] as? Int ?? 0 }

    // MARK: 15. this class's phone

    /// A factory-fresh device (this class runs on its own): the app pairs.
    func test15_AFreshPhonePairsWithTheDevice() throws {
        let before = try SimControl.status()
        guard SimControl.bonds(before).isEmpty else {
            throw XCTSkip("needs a factory-fresh device (run sim/run_ios_tests.sh)")
        }
        let st = launchConnected(extra: ["-sim-forget-bond"])
        XCTAssertEqual(SimControl.bonds(st).count, 1)
        XCTAssertFalse(refusalAlert.exists)
    }

    // MARK: 16. the device's ride list, in full

    /// Forty-one rides on the card (three FileList pages and more): the Rides
    /// tab says so in its header and has a row for every one.
    func test16_EveryRideOnTheCardIsListed() throws {
        let fx = try fixtures()
        let rides = fx.values.filter { $0.role == "summary" || $0.role.hasPrefix("history") }
        XCTAssertEqual(rides.count, 41)
        try putOnCard(rides)
        launchConnected()
        openRides()
        waitForDeviceCount(41)
        for r in rides.sorted(by: { $0.name > $1.name }).prefix(3) + rides.sorted(by: { $0.name < $1.name }).prefix(2) {
            XCTAssertTrue(row(r.name).exists, "no row for \(r.name)")
        }
        let deviceRows = onDeviceRows
        XCTAssertEqual(deviceRows.count, 41, "a row per ride, each still on the device")
    }

    // MARK: 17. a ride downloaded reads as the device summarised it

    /// The newest ride, downloaded with a tap: the ride page shows the numbers
    /// in the FIT's session — distance, moving time, ascent, average speed,
    /// power, NP and heart rate — as the device's FitWriter computed them.
    func test17_ADownloadedRideShowsTheDevicesSummary() throws {
        let fx = try fixtures()
        let s = try XCTUnwrap(fx["summary"])
        try putOnCard([s])
        launchConnected()
        openRides()
        let r = row(s.name)
        XCTAssertTrue(r.waitForExistence(timeout: 30), "no row for \(s.name)")
        r.tap()
        // The ride page opens by itself once the ride is on the phone.
        let back = app.buttons["Back to rides"]
        XCTAssertTrue(back.waitForExistence(timeout: 60), "the ride page never opened")

        // What card-tool's FitWriter was fed (sim/host/cardtool_sim.cpp).
        var dist = 0.0, hrSum = 0, wSum = 0
        for t in 0..<s.seconds {
            dist += s.kmh / 3.6 * (0.85 + 0.15 * sin(Double(t) / 37.0))
            hrSum += Int(Double(s.hr) + 8 * sin(Double(t) / 60.0))
            wSum += Int(Double(s.watts) + 30 * sin(Double(t) / 11.0))
        }
        XCTAssertEqual(stat("DISTANCE"), String(format: "%.1f", dist / 1000))
        XCTAssertEqual(stat("MOVING"), String(format: "%d:%02d:%02d", s.seconds / 3600, (s.seconds / 60) % 60, s.seconds % 60))
        XCTAssertEqual(stat("ASCENT"), "60")
        XCTAssertEqual(stat("AVG SPD"), String(format: "%.1f", dist / Double(s.seconds) * 3.6))
        XCTAssertEqual(stat("AVG PWR"), "\(wSum / s.seconds)")
        XCTAssertEqual(stat("NP"), "\(wSum / s.seconds)")
        XCTAssertEqual(stat("AVG HR"), "\(hrSum / s.seconds)")
        back.tap()
        // The row now shows the ride's numbers, not a download.
        XCTAssertTrue(app.buttons.matching(NSPredicate(format: "label CONTAINS %@", String(format: "%.1f", dist / 1000)))
                          .firstMatch.waitForExistence(timeout: 5), "the row doesn't show the downloaded ride")
        XCTAssertEqual(onDeviceRows.count, onCard().count - 1, "the ride still shows as on the device only")
    }

    // MARK: 18. deleting a ride from the app

    /// Long-press → Delete ride on an older ride: it is deleted on the
    /// device's card, and the header counts one fewer.
    func test18_ARideDeletedInTheAppIsGoneFromTheCard() throws {
        let fx = try fixtures()
        let victim = try XCTUnwrap(fx["history-05"])
        try putOnCard(fx.values.filter { $0.role == "summary" || $0.role.hasPrefix("history") })
        XCTAssertTrue(onCard().contains(victim.name))
        launchConnected()
        openRides()
        waitForDeviceCount(41)
        let r = row(victim.name)
        XCTAssertTrue(r.waitForExistence(timeout: 10))
        // On a loaded runner the context menu can take a while, or the press
        // can land while the list is still settling: press again once.
        let delete = app.buttons["Delete ride"]
        r.press(forDuration: 1.2)
        if !delete.waitForExistence(timeout: 10) { r.press(forDuration: 1.5) }
        XCTAssertTrue(delete.waitForExistence(timeout: 15), "no Delete ride in the row's menu")
        delete.tap()
        let gone = Date().addingTimeInterval(10)
        while onCard().contains(victim.name) && Date() < gone { Thread.sleep(forTimeInterval: 0.3) }
        XCTAssertFalse(onCard().contains(victim.name), "the ride is still on the device's card")
        XCTAssertEqual(onCard().count, 40)
        // A fresh check of the device agrees.
        app.buttons["Check for new rides"].tap()
        waitForDeviceCount(40)
        XCTAssertFalse(row(victim.name).exists)
    }

    // MARK: 19. a ride recorded while the app was closed; the app restarting

    /// The app is closed while the rider records a ride on the device. Opened
    /// again, it reconnects without pairing again (one bond on the device,
    /// no refusal), keeps the ride downloaded before (still on the phone),
    /// and lists the new ride on top.
    func test19_ARideRecordedWhileTheAppWasClosedIsListedOnRelaunch() throws {
        let fx = try fixtures()
        try putOnCard(fx.values.filter { $0.role == "summary" || $0.role.hasPrefix("history") })
        let before = Set(onCard())
        let bonds = SimControl.bonds(try SimControl.status())
        XCTAssertEqual(bonds.count, 1)

        // The app is closed (not running): the device loses the phone.
        app.terminate()
        XCTAssertNotNil(SimControl.waitStatus(timeout: 15) { !SimControl.phoneConnected($0) })
        try recordRideOnDevice(seconds: 60)
        let added = Set(onCard()).subtracting(before)
        XCTAssertEqual(added.count, 1, "one new ride on the card: \(added)")
        let newRide = try XCTUnwrap(added.first)

        let st = launchConnected()
        XCTAssertEqual(SimControl.bonds(st), bonds, "reconnected on the same bond, no new pairing")
        XCTAssertFalse(refusalAlert.exists)
        openRides()
        waitForDeviceCount(before.count + 1)
        XCTAssertTrue(row(newRide).waitForExistence(timeout: 10), "the new ride isn't listed")
        // The ride downloaded in test17 is still on the phone: every other
        // ride (the new one included) is on the device only.
        XCTAssertEqual(onDeviceRows.count, before.count, "the ride downloaded before the restart was forgotten")
    }

    /// The rider's ride on the device: a GPS track, BOOT, `seconds` at 6x,
    /// BOOT and SAVE on the summary (at real time).
    private func recordRideOnDevice(seconds: Int) throws {
        var track: [[Double]] = []
        for i in 0...60 { track.append([45.70, -121.52 + Double(i) * 0.0007]) }
        try SimControl.call(["cmd": "gps", "track": track, "speed_kmh": 30])
        XCTAssertNotNil(SimControl.waitStatus(timeout: 15) { ($0["ride"] as? [String: Any])?["gps_fix"] as? Bool == true })
        try SimControl.call(["cmd": "speed", "factor": 6])
        try SimControl.call(["cmd": "button", "name": "boot"])
        XCTAssertNotNil(SimControl.waitStatus(timeout: 10) { ($0["ride"] as? [String: Any])?["recording"] as? Bool == true })
        XCTAssertNotNil(SimControl.waitStatus(timeout: Double(seconds) / 3 + 20) {
            (($0["ride"] as? [String: Any])?["elapsed_s"] as? Int ?? 0) >= seconds
        })
        try SimControl.call(["cmd": "speed", "factor": 1])
        try SimControl.call(["cmd": "button", "name": "boot"])
        XCTAssertNotNil(SimControl.waitStatus(timeout: 10) { $0["screen"] as? String == "other" })
        try SimControl.call(["cmd": "tap", "x": 24 + 120, "y": 700 + 48])   // SAVE (ui_render.cpp kSaveButton)
        XCTAssertNotNil(SimControl.waitStatus(timeout: 20) { ($0["ride"] as? [String: Any])?["recording"] as? Bool == false })
    }

    // MARK: 20. a download cut by a link drop

    /// A long ride (four hours) downloads over a link that has turned slow;
    /// a third of the way in, the link drops. The row doesn't stay stuck at
    /// its percentage; the app reconnects, and the next tap downloads the
    /// ride again and the ride page opens. Starting over from the beginning
    /// is fine (iOS doesn't keep the partial file); only one download runs.
    func test20_ADownloadCutByALinkDropCanBeDownloadedAgain() throws {
        executionTimeAllowance = 300
        let fx = try fixtures()
        let long = try XCTUnwrap(fx["long"])
        try putOnCard([long])
        launchConnected()
        openRides()
        let r = row(long.name)
        XCTAssertTrue(r.waitForExistence(timeout: 30))
        try SimControl.call(["cmd": "link_pace", "interval_ms": 60, "packets": 2, "window": 4])
        r.tap()
        // The row's tag counts up: wait for a third.
        let third = Date().addingTimeInterval(120)
        var pct = 0
        let downloading = app.buttons.matching(NSPredicate(format: "label MATCHES '.*[0-9]+%.*'")).firstMatch
        repeat {
            Thread.sleep(forTimeInterval: 0.5)
            guard downloading.exists else { continue }
            let label = downloading.label
            if let m = label.range(of: #"(\d+)%"#, options: .regularExpression) {
                pct = Int(label[m].dropLast()) ?? 0
            }
        } while pct < 33 && Date() < third
        XCTAssertGreaterThanOrEqual(pct, 33, "the download never got a third of the way")
        try SimControl.call(["cmd": "ble_drop", "reason": 8])
        try SimControl.call(["cmd": "link_pace", "interval_ms": 0, "packets": 0, "window": 32])
        XCTAssertNotNil(SimControl.waitStatus(timeout: 30) { SimControl.phoneConnected($0) }, "the app never reconnected")
        XCTAssertTrue(ridesConnected.waitForExistence(timeout: 20))
        let unstuck = Date().addingTimeInterval(20)
        while downloading.exists && Date() < unstuck { Thread.sleep(forTimeInterval: 0.5) }
        XCTAssertFalse(downloading.exists, "the row is stuck downloading: \(downloading.exists ? downloading.label : "")")
        XCTAssertTrue(row(long.name).waitForExistence(timeout: 5), "the cut ride's row is gone")
        XCTAssertTrue(row(long.name).label.localizedCaseInsensitiveContains("On device"),
                      "the cut ride doesn't show as on the device: \(row(long.name).label)")

        let seq = try logSeq()
        row(long.name).tap()
        XCTAssertTrue(app.buttons["Back to rides"].waitForExistence(timeout: 120), "the ride never opened")
        let begins = try deviceLog(since: seq, matching: "link: download \\d+ begin: \(NSRegularExpression.escapedPattern(for: long.name))")
        XCTAssertEqual(begins.count, 1, "\(begins)")
        XCTAssertEqual(stat("MOVING"), "4:00:00")
        app.buttons["Back to rides"].tap()
    }

    // MARK: 21. a storm of link drops

    /// The rider at the edge of range: the link drops a dozen times, some in
    /// the middle of the reconnect. When it calms down the app is connected
    /// over one link, never told the rider the device refused it, and the
    /// Rides tab's check of the device still works.
    func test21_AStormOfLinkDropsEndsConnectedOnOneLink() throws {
        executionTimeAllowance = 240
        let fx = try fixtures()
        try putOnCard(fx.values.filter { $0.role == "summary" || $0.role.hasPrefix("history") })
        let n = onCard().count
        launchConnected()
        openRides()
        waitForDeviceCount(n)
        var rng = SystemRandomNumberGenerator()
        for _ in 0..<12 {
            Thread.sleep(forTimeInterval: Double.random(in: 0.1...1.3, using: &rng))
            if let st = try? SimControl.status(), SimControl.phoneConnected(st) {
                _ = try? SimControl.call(["cmd": "ble_drop", "reason": 8])
            }
            XCTAssertFalse(refusalAlert.exists, "the app said the device isn't accepting this iPhone")
        }
        let st = SimControl.waitStatus(timeout: 40) { SimControl.phoneConnected($0) }
        XCTAssertNotNil(st, "the app never came back after the storm")
        XCTAssertTrue(ridesConnected.waitForExistence(timeout: 20), "the Rides tab doesn't show the app connected")
        Thread.sleep(forTimeInterval: 3)
        let now = try SimControl.status()
        let conns = (now["ble"] as? [String: Any])?["connections"] as? [[String: Any]] ?? []
        XCTAssertEqual(conns.count, 1, "one link at the device: \(conns)")
        XCTAssertEqual(SimControl.bonds(now).count, 1)
        XCTAssertFalse(refusalAlert.exists, "the app said the device isn't accepting this iPhone")
        app.buttons["Check for new rides"].tap()
        waitForDeviceCount(n)
    }
}

extension SimEnv {
    /// The FIT files run_ios_tests.sh had `otp-sim card-tool fit` write for
    /// FirmwareSimAppFlowTests (rides/ and manifest.txt), or nil.
    static var rideFixtures: URL? {
        ProcessInfo.processInfo.environment["OTP_RIDE_FIXTURES"].flatMap { $0.isEmpty ? nil : URL(fileURLWithPath: $0) }
    }
}
