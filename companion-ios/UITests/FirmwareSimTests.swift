import CoreLocation
import XCTest

/// The app against the real firmware, run by the simulator (sim/IOS.md):
/// BLEManager's connection state machine and LinkClient on SimRadio instead
/// of CoreBluetooth. Needs `otp-sim` running; sim/run_ios_tests.sh starts a
/// factory-fresh one and runs these in order (test01 pairs the phone).
final class FirmwareSimTests: XCTestCase {
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

    /// Cancelled at the end of each test, so a watcher never reports into the next.
    private var watching = Operation()

    override func tearDown() {
        watching.cancel()
        RangeProxy.shared.mode = .inRange
        app?.terminate()
    }

    // MARK: helpers

    private var connectedLabel: XCUIElement {
        app.descendants(matching: .any)
            .matching(NSPredicate(format: "label BEGINSWITH 'CONNECTED'")).firstMatch
    }

    private var refusalAlert: XCUIElement {
        app.alerts.matching(NSPredicate(format: "label CONTAINS[c] %@", "isn't accepting this iPhone")).firstMatch
    }

    /// Launches the app and waits for both ends to agree it is connected.
    @discardableResult
    private func launchConnected(extra: [String] = [], file: StaticString = #filePath,
                                 line: UInt = #line) -> [String: Any] {
        app.launchArguments += extra
        app.launch()
        let st = SimControl.waitStatus(timeout: 30) { SimControl.phoneConnected($0) }
        XCTAssertNotNil(st, "device never saw the phone connect", file: file, line: line)
        XCTAssertTrue(connectedLabel.waitForExistence(timeout: 20), "app never showed CONNECTED",
                      file: file, line: line)
        return st ?? [:]
    }

    // MARK: 1. first connect pairs

    func test01_FirstConnectPairsAndShowsConnected() throws {
        let before = try SimControl.status()
        guard SimControl.bonds(before).isEmpty else {
            throw XCTSkip("needs a factory-fresh device (run sim/run_ios_tests.sh)")
        }
        // A phone with no bond: the app finds the device by scanning, pairs
        // (the sim confirms the passkey the panel shows) and connects.
        let st = launchConnected(extra: ["-sim-forget-bond"])
        let ble = st["ble"] as? [String: Any] ?? [:]
        let bonds = SimControl.bonds(st)
        XCTAssertEqual(bonds.count, 1, "device should hold exactly one bond: \(bonds)")
        let conns = ble["connections"] as? [[String: Any]] ?? []
        XCTAssertEqual(conns.count, 1)
        XCTAssertEqual(conns.first?["identity"] as? String, bonds.first, "the bond is this phone's")
        XCTAssertEqual(conns.first?["encrypted"] as? Bool, true)
        XCTAssertEqual(st["phone_paired"] as? Bool, true)
        XCTAssertFalse(refusalAlert.exists)
    }

    // MARK: 2. paging the Ride tab pages the device

    func test02_RidePagingPagesTheDevice() throws {
        launchConnected()
        // The default layout has a field page and the map.
        XCTAssertNotNil(SimControl.waitStatus(timeout: 10) { ($0["page_count"] as? Int ?? 0) >= 2 })
        XCTAssertNotNil(SimControl.waitStatus(timeout: 5) { $0["page"] as? Int == 0 },
                        "device should start on page 0")
        let pagePreview = app.descendants(matching: .any)
            .matching(NSPredicate(format: "label == 'Device page Page 1'")).firstMatch
        XCTAssertTrue(pagePreview.waitForExistence(timeout: 10))
        pagePreview.swipeLeft()
        XCTAssertNotNil(SimControl.waitStatus(timeout: 10) { $0["page"] as? Int == 1 },
                        "swiping to the map page should show it on the device")
        // And back, with the page strip.
        app.buttons["Dashboard page Page 1"].tap()
        XCTAssertNotNil(SimControl.waitStatus(timeout: 10) { $0["page"] as? Int == 0 },
                        "tapping PAGE 1 should bring the device back to page 0")
    }

    // MARK: 3. regression: reconnect after a background link loss

    /// 2026-10-26: the link timed out with the app in the background, the
    /// reconnects failed without ever reaching the device, and the app then
    /// said "OpenTrailPaper isn't accepting this iPhone" until force-quit.
    func test03_BackgroundLinkLossReconnectsOnForeground() throws {
        launchConnected()
        XCUIDevice.shared.press(.home)
        Thread.sleep(forTimeInterval: 2)
        // The rider rides off: the device is at the edge of range. The link
        // times out (supervision timeout, HCI 0x08) and every reconnect the
        // phone starts fails before it is up.
        RangeProxy.shared.mode = .connectFails
        let failedBefore = RangeProxy.shared.failedConnects
        try SimControl.call(["cmd": "ble_drop", "reason": 8])
        XCTAssertNotNil(SimControl.waitStatus(timeout: 10) { !SimControl.phoneConnected($0) })
        // Long enough for the app to fail several reconnects (it retries
        // every 2 s); before the fix three of them were taken as a refusal.
        let deadline = Date().addingTimeInterval(20)
        while RangeProxy.shared.failedConnects - failedBefore < 4 && Date() < deadline {
            Thread.sleep(forTimeInterval: 0.5)
        }
        let failed = RangeProxy.shared.failedConnects - failedBefore
        XCTAssertGreaterThanOrEqual(failed, 3, "the app should have tried (and failed) to reconnect")
        // Back in range, back to the app.
        RangeProxy.shared.mode = .inRange
        app.activate()
        XCTAssertFalse(refusalAlert.waitForExistence(timeout: 3),
                       "the app said the device isn't accepting this iPhone")
        XCTAssertNotNil(SimControl.waitStatus(timeout: 30) { SimControl.phoneConnected($0) },
                        "the app never reconnected after \(failed) failed connects")
        XCTAssertTrue(connectedLabel.waitForExistence(timeout: 15), "app never showed CONNECTED again")
        XCTAssertFalse(refusalAlert.exists, "the app said the device isn't accepting this iPhone")
    }

    // MARK: 4. a setting changed in the app reaches the device

    func test04_SettingChangeReachesTheDevice() throws {
        launchConnected()
        func miles(_ s: [String: Any]) -> Bool? { (s["settings"] as? [String: Any])?["miles"] as? Bool }
        let start = try XCTUnwrap(miles(try SimControl.status()), "status has no settings.miles")
        app.buttons["Settings"].firstMatch.tap()
        let target = start ? "Units, km" : "Units, mi"
        let button = app.buttons[target]
        XCTAssertTrue(button.waitForExistence(timeout: 10))
        button.tap()
        XCTAssertNotNil(SimControl.waitStatus(timeout: 10) { miles($0) == !start },
                        "the device's units never changed")
        // Put it back, which must reach the device too.
        app.buttons[start ? "Units, mi" : "Units, km"].tap()
        XCTAssertNotNil(SimControl.waitStatus(timeout: 10) { miles($0) == start })
    }

    // MARK: 5. regression: the link survives the device's Service Changed

    /// 868ace8 / 56fc2b3: the device indicates Service Changed once per boot,
    /// ~2 s after it admits the phone (so an iOS that cached an older GATT
    /// table looks again). The app must rediscover and carry on over the same
    /// session: a setting changed afterwards still reaches the device.
    func test05_LinkSurvivesServiceChanged() throws {
        // Broken, the app spins "Syncing with device…" forever and XCUITest
        // waits for it to idle; fail instead (run_ios_tests.sh enables test
        // timeouts).
        executionTimeAllowance = 180
        launchConnected()
        // A new boot, so the device owes this phone its Service Changed.
        try SimControl.call(["cmd": "reboot"])
        Thread.sleep(forTimeInterval: 1)
        let back = Date().addingTimeInterval(30)
        while (try? SimControl.status()) == nil && Date() < back { Thread.sleep(forTimeInterval: 0.3) }
        XCTAssertNotNil(SimControl.waitStatus(timeout: 30) { SimControl.phoneConnected($0) },
                        "the app never reconnected after the reboot")
        XCTAssertNoThrow(try SimControl.call(["cmd": "wait_log", "since": 0, "timeout_ms": 20_000,
                                               "pattern": "service changed indicated"], timeout: 25),
                         "the device never indicated Service Changed")
        // The app rediscovers at once; give it past LinkClient's 10 s
        // rediscovery deadline, so a session that never resumed has failed.
        Thread.sleep(forTimeInterval: 12)
        XCTAssertTrue(connectedLabel.waitForExistence(timeout: 10), "app not CONNECTED after Service Changed")

        func miles(_ s: [String: Any]) -> Bool? { (s["settings"] as? [String: Any])?["miles"] as? Bool }
        let start = try XCTUnwrap(miles(try SimControl.status()), "status has no settings.miles")
        app.buttons["Settings"].firstMatch.tap()
        let button = app.buttons[start ? "Units, km" : "Units, mi"]
        XCTAssertTrue(button.waitForExistence(timeout: 10))
        button.tap()
        XCTAssertNotNil(SimControl.waitStatus(timeout: 10) { miles($0) == !start },
                        "a setting changed after Service Changed never reached the device")
        app.buttons[start ? "Units, mi" : "Units, km"].tap()
        XCTAssertNotNil(SimControl.waitStatus(timeout: 10) { miles($0) == start })
    }

    // MARK: maps: the area the rider picks, from the CDN to the device

    /// Settings → Maps on SD card → Select area → a box dragged around the
    /// phone's position (the fixture's centre: the map centres on the rider at
    /// a fixed span). Returns the number of hexes the Download button (or
    /// `button`, e.g. "Send POIs for 2 hexes") offers.
    private func selectAreaAroundTheRider(_ area: TileCdn.Area, button: NSPredicate = downloadButton,
                                          centreOnRider: Bool = false, beforeDownload: () -> Void = {},
                                          file: StaticString = #filePath, line: UInt = #line) throws -> Int {
        app.buttons["Settings"].firstMatch.tap()
        let mapsRow = app.buttons.matching(NSPredicate(format: "label BEGINSWITH 'Maps on SD card'")).firstMatch
        XCTAssertTrue(mapsRow.waitForExistence(timeout: 10), "no Maps row in Settings", file: file, line: line)
        mapsRow.tap()
        let select = app.buttons["Select area"]
        XCTAssertTrue(select.waitForExistence(timeout: 10), "Maps screen didn't open", file: file, line: line)
        allowLocationIfAsked()
        Thread.sleep(forTimeInterval: 3)   // the map centres on the rider's first fix
        if centreOnRider {
            // With tiles on the device the screen frames their coverage
            // instead; the locate button brings the rider back to the middle.
            app.buttons["Tracking"].firstMatch.tap()
            Thread.sleep(forTimeInterval: 2)
        }
        select.tap()
        // A box ~60 pt across around the middle of the map: well inside the
        // fixture's box (±0.04°/±0.05°) at the screen's ~0.25° span.
        let from = app.coordinate(withNormalizedOffset: CGVector(dx: 0.42, dy: 0.40))
        let to = app.coordinate(withNormalizedOffset: CGVector(dx: 0.58, dy: 0.46))
        from.press(forDuration: 0.2, thenDragTo: to)
        // The screen looks the selection's group indexes up at once: a box
        // outside the fixture shows up here, before anything is downloaded.
        let groups = Set(area.cells.keys.map { "/v1/\($0.prefix(6))/index.json" })
        Thread.sleep(forTimeInterval: 1)
        let foreign = try TileCdn.paths().filter { $0.hasSuffix("/index.json") && !groups.contains($0) }
        guard foreign.isEmpty else {
            XCTFail("the selected box is outside the fixture (the map didn't centre on the rider?): \(foreign)",
                    file: file, line: line)
            throw XCTSkip("selection outside the fixture")
        }
        let download = app.buttons.matching(button).firstMatch
        if !download.waitForExistence(timeout: 10) {
            // What the screen shows instead (a screenshot needs no UI query:
            // the app may not idle for one, sim/IOS.md).
            let shot = XCTAttachment(screenshot: XCUIScreen.main.screenshot())
            shot.name = "no \(button) button"
            shot.lifetime = .keepAlways
            add(shot)
            XCTFail("no \(button) button after drawing a box", file: file, line: line)
            throw XCTSkip("no \(button) button")
        }
        // The Download button waits for the device's tile list (canUploadMap).
        let enabled = NSPredicate(format: "isEnabled == true")
        expectation(for: enabled, evaluatedWith: download)
        waitForExpectations(timeout: 15)
        let label = download.label
        let n = label.split(separator: " ").compactMap { Int($0) }.first ?? 0
        XCTAssertGreaterThan(n, 0, "Download button: \(label)", file: file, line: line)
        beforeDownload()
        download.tap()
        return n
    }

    private static let downloadButton = NSPredicate(format: "label CONTAINS[c] 'download ' AND label CONTAINS[c] ' hex'")

    /// Watches the device's tiles from a thread of its own for `seconds`,
    /// failing the test the moment `bad` finds something wrong with them. A
    /// UI query can block until the app idles (the Maps screen has been seen
    /// never to, see sim/IOS.md), so the device is not left to the queries
    /// that follow the tap.
    private func watchDevice(for seconds: TimeInterval, _ bad: @escaping ([String: String]) -> String?) {
        let end = Date().addingTimeInterval(seconds)
        let stop = watching
        Thread {
            while Date() < end && !stop.isCancelled {
                if let tiles = try? SimControl.tiles(), let problem = bad(tiles), !stop.isCancelled {
                    // Also to the log: a failure recorded from here can wait
                    // behind a main thread that is stuck in a UI query.
                    print("FirmwareSimTests device watch: FAIL: \(problem)")
                    XCTFail(problem)
                    return
                }
                Thread.sleep(forTimeInterval: 0.5)
            }
        }.start()
    }

    private func mapsLaunch(_ area: TileCdn.Area) {
        // The phone is in the fixture's area: the Maps screen centres there.
        XCUIDevice.shared.location = XCUILocation(location: CLLocation(latitude: area.lat, longitude: area.lon))
        // Whatever an earlier test answered to iOS's location prompt, ask
        // again and allow (allowLocationIfAsked): without a fix the map opens
        // on its default region, far from the fixture.
        app.resetAuthorizationStatus(for: .location)
        launchConnected(extra: TileCdn.launchArguments)
        allowLocationIfAsked()
    }

    /// iOS's location prompt (the app asks when it connects and when Maps
    /// opens): allow, so the Maps screen centres on the rider.
    private func allowLocationIfAsked() {
        let springboard = XCUIApplication(bundleIdentifier: "com.apple.springboard")
        let alert = springboard.alerts.firstMatch
        guard alert.waitForExistence(timeout: 2) else { return }
        for title in ["Allow While Using App", "Allow Once", "Allow"] where alert.buttons[title].exists {
            alert.buttons[title].tap()
            return
        }
    }

    private func requireCdn() throws -> TileCdn.Area {
        guard TileCdn.reachable() else {
            throw XCTSkip("stand-in tile CDN not running on port \(SimEnv.cdnPort) (run sim/run_ios_tests.sh)")
        }
        try TileCdn.reset()
        return try TileCdn.area()
    }

    // MARK: 6. a broken tile from the CDN never reaches the device

    /// Every tile the CDN serves is truncated (a 200 with half the body). The
    /// app must not send one to the device: they fall back to Overpass (the
    /// stand-in answers 503, a busy server), and the Maps screen says the
    /// download failed.
    func test06_TruncatedTilesAreReportedAndNotSent() throws {
        executionTimeAllowance = 240
        let area = try requireCdn()
        XCTAssertTrue(try SimControl.tiles().isEmpty, "the device should start with no tiles")
        try TileCdn.fault("*.ebm", "truncate")
        mapsLaunch(area)
        _ = try selectAreaAroundTheRider(area, beforeDownload: {
            self.watchDevice(for: 90) { $0.isEmpty ? nil : "a truncated tile reached the device: \($0)" }
        })

        // Watch the device while the app works: a truncated tile arriving
        // fails the test at once (the app may never say anything then).
        let failed = app.staticTexts.matching(NSPredicate(format: "label CONTAINS[c] 'busy' OR label CONTAINS[c] 'failed'")).firstMatch
        var reported = false
        let end = Date().addingTimeInterval(60)
        repeat {
            let got = try SimControl.tiles()
            XCTAssertEqual(got, [:], "a truncated tile reached the device")
            if !got.isEmpty { return }
            if failed.exists { reported = true; break }
            Thread.sleep(forTimeInterval: 1)
        } while Date() < end
        XCTAssertTrue(reported, "the Maps screen never reported the failed download")
        // Give the queue time to drain whatever it was given.
        Thread.sleep(forTimeInterval: 3)
        XCTAssertEqual(try SimControl.tiles(), [:], "a truncated tile reached the device")
        let paths = try TileCdn.paths()
        XCTAssertTrue(paths.contains { $0.hasSuffix(".ebm") }, "the app never asked the CDN for tiles: \(paths)")
        XCTAssertTrue(paths.contains { $0.hasSuffix("/api/interpreter") },
                      "the bad tiles should fall back to (the stand-in's) Overpass: \(paths)")
    }

    // MARK: 7. a downloaded area reaches the device, and the device draws it

    func test07_MapAreaDownloadReachesTheDevice() throws {
        executionTimeAllowance = 240
        let area = try requireCdn()
        XCTAssertTrue(try SimControl.tiles().isEmpty, "the device should start with no tiles")
        try SimControl.call(["cmd": "gps", "lat": area.lat, "lon": area.lon, "fix": true, "sats": 9])
        // Before: the device's map screen at the fix says "NO MAP HERE".
        try SimControl.call(["cmd": "console", "line": "screen map", "wait_ms": 500])
        XCTAssertNotNil(SimControl.waitStatus(timeout: 10) { $0["screen"] as? String == "map" })
        Thread.sleep(forTimeInterval: 3)
        let blank = try Panel.mapInk(FileManager.default.temporaryDirectory.appendingPathComponent("device-nomap.png"))
        XCTAssertLessThan(blank, 200, "the device draws a map before it has any tiles (\(blank) dark pixels)")
        mapsLaunch(area)
        let n = try selectAreaAroundTheRider(area, beforeDownload: {
            self.watchDevice(for: 90) { tiles in
                tiles.first { id, v in v != "cdn:" + (area.cells[id]?.ebmHash ?? "?") }
                    .map { "\($0.key) arrived on the device with version record \($0.value)" }
            }
        })

        // The device: exactly n hexes, all from the fixture, each with the
        // CDN's version record (tile and .poi).
        var onDevice: [String: String] = [:]
        let end = Date().addingTimeInterval(90)
        repeat {
            onDevice = (try? SimControl.tiles()) ?? [:]
            if onDevice.count >= n { break }
            Thread.sleep(forTimeInterval: 0.5)
        } while Date() < end
        XCTAssertEqual(onDevice.count, n, "the device holds \(onDevice.keys.sorted()), the app offered \(n)")
        for (id, version) in onDevice {
            let cell = try XCTUnwrap(area.cells[id], "\(id) is not a fixture hex: the selection fell outside it")
            XCTAssertEqual(version, "cdn:" + cell.ebmHash, "\(id)'s version record on the device")
        }
        let poiEnd = Date().addingTimeInterval(30)
        var pois: [String: String] = [:]
        repeat {
            pois = (try? SimControl.tiles(poi: true)) ?? [:]
            if Set(pois.keys).isSuperset(of: onDevice.keys) { break }
            Thread.sleep(forTimeInterval: 0.5)
        } while Date() < poiEnd
        for id in onDevice.keys {
            XCTAssertEqual(pois[id], "cdn:" + (area.cells[id]?.poiHash ?? "?"), "\(id).poi's version record on the device")
        }
        // Everything came from the stand-in's CDN; nothing went to Overpass.
        let paths = try TileCdn.paths()
        XCTAssertEqual(Set(paths.filter { $0.hasSuffix(".ebm") }),
                       Set(onDevice.keys.map { "/v1/\($0.prefix(6))/\($0).ebm" }), "tiles fetched: \(paths)")
        XCTAssertFalse(paths.contains { $0.hasSuffix("/api/interpreter") }, "Overpass was asked: \(paths)")

        // And it draws them: the map screen at the fix is no longer blank.
        try SimControl.call(["cmd": "console", "line": "screen dash", "wait_ms": 300])
        try SimControl.call(["cmd": "console", "line": "screen map", "wait_ms": 500])
        XCTAssertNotNil(SimControl.waitStatus(timeout: 10) { $0["screen"] as? String == "map" })
        Thread.sleep(forTimeInterval: 3)
        let shot = FileManager.default.temporaryDirectory.appendingPathComponent("device-map.png")
        let ink = try Panel.mapInk(shot)
        if let data = try? Data(contentsOf: shot) {
            let a = XCTAttachment(data: data, uniformTypeIdentifier: "public.png")
            a.name = "device map screen"
            a.lifetime = .keepAlways
            add(a)
        }
        XCTAssertGreaterThan(ink, 2_000, "the device's map screen is blank at the downloaded area (\(ink) dark pixels)")
    }

    // MARK: 8. a new CDN version of a hex on the device is offered and sent

    /// After test07: the CDN publishes a new version of one of the hexes the
    /// device holds. The Maps screen offers to redownload exactly that one,
    /// and the device ends up with the new version; the others keep theirs.
    func test08_NewCdnVersionIsRedownloaded() throws {
        executionTimeAllowance = 240
        let area = try requireCdn()
        let before = try SimControl.tiles()
        guard let changed = before.keys.sorted().first else {
            throw XCTSkip("needs test07's tiles on the device")
        }
        let newHash = try TileCdn.bump(changed)
        mapsLaunch(area)
        app.buttons["Settings"].firstMatch.tap()
        let mapsRow = app.buttons.matching(NSPredicate(format: "label BEGINSWITH 'Maps on SD card'")).firstMatch
        XCTAssertTrue(mapsRow.waitForExistence(timeout: 10))
        mapsRow.tap()
        let select = app.buttons["Select area"]
        XCTAssertTrue(select.waitForExistence(timeout: 10))
        allowLocationIfAsked()
        Thread.sleep(forTimeInterval: 3)
        select.tap()
        let from = app.coordinate(withNormalizedOffset: CGVector(dx: 0.42, dy: 0.40))
        let to = app.coordinate(withNormalizedOffset: CGVector(dx: 0.58, dy: 0.46))
        from.press(forDuration: 0.2, thenDragTo: to)
        let redownload = app.buttons.matching(NSPredicate(format: "label CONTAINS[c] 'redownload 1 hex'")).firstMatch
        XCTAssertTrue(redownload.waitForExistence(timeout: 15), "the Maps screen should offer to redownload the one changed hex")
        redownload.tap()
        var now: [String: String] = [:]
        let end = Date().addingTimeInterval(60)
        repeat {
            now = try SimControl.tiles()
            if now[changed] == "cdn:" + newHash { break }
            Thread.sleep(forTimeInterval: 0.5)
        } while Date() < end
        XCTAssertEqual(now[changed], "cdn:" + newHash, "the device's record for the changed hex")
        for (id, v) in before where id != changed { XCTAssertEqual(now[id], v, "\(id) should be untouched") }
        let tiles = try TileCdn.paths().filter { $0.hasSuffix(".ebm") }
        XCTAssertEqual(tiles, ["/v1/\(changed.prefix(6))/\(changed).ebm"], "only the changed tile is fetched")
    }

    // MARK: 9. Bluetooth off and on

    /// Bluetooth switched off and on with the device connected: the app must
    /// come back CONNECTED. CoreBluetooth reports .poweredOff and no
    /// didDisconnect for the link it takes down; the app kept that link's
    /// session and its peripheral, so on .poweredOn it scanned, ignored every
    /// advertisement (a peripheral was still set) and never connected again.
    func test09_BluetoothOffAndOnComesBackConnected() throws {
        launchConnected()
        SimBluetooth.set(on: false)
        defer { SimBluetooth.set(on: true) }
        XCTAssertNotNil(SimControl.waitStatus(timeout: 15) { !SimControl.phoneConnected($0) },
                        "the device should lose the phone when its Bluetooth goes off")
        let off = app.descendants(matching: .any)
            .matching(NSPredicate(format: "label CONTAINS[c] 'Bluetooth off' OR label CONTAINS[c] 'Bluetooth is off'"))
            .firstMatch
        XCTAssertTrue(off.waitForExistence(timeout: 10), "the app should say Bluetooth is off")
        SimBluetooth.set(on: true)
        XCTAssertNotNil(SimControl.waitStatus(timeout: 30) { SimControl.phoneConnected($0) },
                        "the app never reconnected after Bluetooth came back on")
        XCTAssertTrue(connectedLabel.waitForExistence(timeout: 20), "app never showed CONNECTED again")
        XCTAssertFalse(refusalAlert.exists, "the app said the device isn't accepting this iPhone")
    }

    // MARK: cycling POIs (sim/fixtures/appmaps/pois, made by sim/cdn/build_poi_fixture.py)
    //
    // The device is checked with the app closed: a failure recorded while the
    // Maps screen is up can wait forever on XCTest's snapshot of an app that
    // never idles (sim/IOS.md), hiding the message.

    /// XCTAssert, but said in the log first: a failure recorded here can sit
    /// behind XCTest's failure triage, which has been seen to hang (sim/IOS.md).
    private func expect(_ ok: Bool, _ msg: @autoclosure () -> String = "",
                        file: StaticString = #filePath, line: UInt = #line) {
        if !ok { fail(msg(), file: file, line: line) }
    }
    private func expectEqual<T: Equatable>(_ a: T, _ b: T, _ msg: @autoclosure () -> String = "",
                                           file: StaticString = #filePath, line: UInt = #line) {
        if a != b { fail("\(msg()): \(a) != \(b)", file: file, line: line) }
    }
    private func fail(_ msg: String, file: StaticString = #filePath, line: UInt = #line) {
        print("FirmwareSimTests FAIL (line \(line)): \(msg)")
        XCTFail(msg, file: file, line: line)
    }

    /// Where the fixture's water point, 150 m north of the centre, is drawn on
    /// the device's map at the centre: north up, 2 m/px, the rider at (270, 430).
    private let waterIcon = CGRect(x: 245, y: 330, width: 50, height: 50)

    /// The primary button when the device needs POIs: "Send POIs for N
    /// hexes", or "Download N hexes" when the box also takes in a hex the
    /// device lacks, which then goes too, with its POIs.
    private static let sendPoisButton = NSPredicate(
        format: "label CONTAINS[c] 'send pois for ' OR label BEGINSWITH[c] 'download '")

    /// The device's map screen, freshly drawn, as gray pixels (kept in the
    /// result bundle as `name`).
    private func deviceMap(_ name: String) throws -> (Int, Int, [UInt8]) {
        try SimControl.call(["cmd": "console", "line": "screen dash", "wait_ms": 300])
        try SimControl.call(["cmd": "console", "line": "screen map", "wait_ms": 500])
        _ = SimControl.waitStatus(timeout: 10) { $0["screen"] as? String == "map" }
        Thread.sleep(forTimeInterval: 3)
        let shot = FileManager.default.temporaryDirectory.appendingPathComponent("\(name).png")
        let px = try Panel.gray(shot)
        if let data = try? Data(contentsOf: shot) {
            let a = XCTAttachment(data: data, uniformTypeIdentifier: "public.png")
            a.name = name
            a.lifetime = .keepAlways
            add(a)
        }
        return px
    }

    /// The rider standing at the fixture's centre, facing north.
    private func fixAtTheCentre(_ area: TileCdn.Area) throws {
        try SimControl.call(["cmd": "gps", "lat": area.lat, "lon": area.lon, "fix": true, "sats": 9,
                             "course": 0, "speed_kmh": 0])
    }

    /// Waits until the device's .poi records satisfy `done`; returns them.
    private func waitForPois(_ timeout: TimeInterval = 60, _ done: ([String: String]) -> Bool) -> [String: String] {
        var pois: [String: String] = [:]
        let end = Date().addingTimeInterval(timeout)
        repeat {
            pois = (try? SimControl.tiles(poi: true)) ?? [:]
            if done(pois) { break }
            Thread.sleep(forTimeInterval: 0.5)
        } while Date() < end
        return pois
    }

    /// Lets the transfer queue drain, then closes the app (see above).
    private func settleAndClose() {
        Thread.sleep(forTimeInterval: 2)
        app.terminate()
    }

    /// The .poi on the device's card is the POI set's reference file.
    private func assertCardHolds(_ set: String, _ id: String, file: StaticString = #filePath, line: UInt = #line) {
        guard let want = TileCdn.poiSetFile(set, id) else {
            return fail("the stand-in has no \(set)/\(id).poi", file: file, line: line)
        }
        guard let got = SimEnv.cardFile(id, "poi") else {
            return fail("\(id).poi is not on the device's card (OTP_SIM_DIR \(SimEnv.deviceDir?.path ?? "unset"))",
                           file: file, line: line)
        }
        expectEqual(got, want, "\(id).poi on the card is not the reference writer's (\(set))", file: file, line: line)
    }

    /// The hex the woodstock and changed POI sets differ in (the rider's, at
    /// the centre: the box the POI tests draw is around it), with its .poi
    /// hash in each. Leaves the CDN serving `serve`.
    private func editedHex(serve: String) throws -> (id: String, woodstock: String, changed: String) {
        let a = try TileCdn.pois("woodstock"), b = try TileCdn.pois("changed")
        try TileCdn.pois(serve)
        let ids = a.keys.filter { a[$0] != b[$0] }
        guard ids.count == 1, let id = ids.first else { throw XCTSkip("the POI sets should differ in one hex: \(ids)") }
        return (id, a[id] ?? "", b[id] ?? "")
    }

    /// Hexes on the device now that were not in `before`.
    private func newHexes(since before: [String: String]) throws -> [String] {
        try SimControl.tiles().keys.filter { before[$0] == nil }.sorted()
    }

    /// The CDN files a download that adds `added` to the device fetches for
    /// them: each tile, and each .poi with POIs.
    private func filesFor(_ added: [String], _ hashes: [String: String]) -> [String] {
        added.flatMap { a in
            ["/v1/\(a.prefix(6))/\(a).ebm"] + (hashes[a] == "" ? [] : ["/v1/\(a.prefix(6))/\(a).poi"])
        }
    }

    // MARK: 9. the area's POIs reach the device, which draws them

    /// After test07/08: the CDN now has POIs for the area (the woodstock
    /// set). A box around the rider; the Maps screen offers the hexes whose
    /// .poi changed, and the device ends up with each selected hex's .poi byte
    /// for byte the reference writer's, with the CDN's records, and draws the
    /// water point north of the rider where there was none.
    func test10_AreaPoisReachTheDeviceAndAreDrawn() throws {
        executionTimeAllowance = 240
        let area = try requireCdn()
        let onDevice = try SimControl.tiles()
        guard !onDevice.isEmpty else { throw XCTSkip("needs test07's tiles on the device") }
        try fixAtTheCentre(area)
        let before = try deviceMap("device-pois-before")
        let hashes = try TileCdn.pois("woodstock")
        let had = try SimControl.tiles(poi: true)

        mapsLaunch(area)
        _ = try selectAreaAroundTheRider(area, button: Self.sendPoisButton, centreOnRider: true)
        // The hexes whose .poi went out: the ones that changed or are new.
        _ = waitForPois { p in
            let now = (try? SimControl.tiles()) ?? onDevice
            return now.keys.contains { had[$0] != p[$0] && p[$0] == "cdn:" + (hashes[$0] ?? "?") }
                && now.keys.allSatisfy { p[$0] != nil }
        }
        settleAndClose()

        let pois = try SimControl.tiles(poi: true)
        let added = try newHexes(since: onDevice)
        let sent = pois.keys.filter { had[$0] != pois[$0] }.sorted()
        print("FirmwareSimTests test10: sent \(sent), added \(added), records \(pois)")
        expect(!(sent.isEmpty), "no .poi reached the device")
        for id in sent {
            expectEqual(pois[id], "cdn:" + (hashes[id] ?? "?"), "\(id).poi's version record on the device")
            assertCardHolds("woodstock", id)
        }
        let paths = try TileCdn.paths()
        expectEqual(Set(paths.filter { $0.hasSuffix(".poi") }),
                       Set(sent.filter { hashes[$0] != "" }.map { "/v1/\($0.prefix(6))/\($0).poi" }),
                       "the .poi files fetched: \(paths)")
        expectEqual(Set(paths.filter { $0.hasSuffix(".ebm") }), Set(added.map { "/v1/\($0.prefix(6))/\($0).ebm" }),
                       "a tile already on the device was fetched: \(paths)")
        expect(!(paths.contains { $0.hasSuffix("/api/interpreter") }), "Overpass was asked: \(paths)")

        let after = try deviceMap("device-pois-after")
        let d = Panel.diff(before, after, box: waterIcon)
        print("FirmwareSimTests test10: water icon diff \(d)")
        expect(d.inside > 150, "no water icon drawn north of the rider (\(d.inside) pixels changed there)")
    }

    // MARK: 10. a POI edit upstream is sent for that hex only

    /// After test10: the rider's hex's POIs change upstream (the changed set).
    /// Only that .poi is fetched and sent (and a hex new to the device, if
    /// the box takes one in); the device's other .poi files are untouched.
    func test11_PoiChangeUpstreamIsSentForThatHexOnly() throws {
        executionTimeAllowance = 240
        let area = try requireCdn()
        let (id, woodstock, _) = try editedHex(serve: "changed")
        let hashes = try TileCdn.pois("changed")
        guard try SimControl.tiles(poi: true)[id] == "cdn:" + woodstock else {
            throw XCTSkip("needs test10's POIs for \(id) on the device")
        }
        let before = try SimControl.tiles(poi: true)
        let tilesBefore = try SimControl.tiles()
        mapsLaunch(area)
        _ = try selectAreaAroundTheRider(area, button: Self.sendPoisButton, centreOnRider: true)
        _ = waitForPois { $0[id] == "cdn:" + (hashes[id] ?? "?") }
        settleAndClose()

        let pois = try SimControl.tiles(poi: true)
        let added = try newHexes(since: tilesBefore)
        print("FirmwareSimTests test11: \(id) -> \(pois[id] ?? "none"), added \(added)")
        expectEqual(pois[id], "cdn:" + (hashes[id] ?? "?"), "the device's record for the changed hex")
        for (other, v) in before where other != id { expectEqual(pois[other], v, "\(other).poi should be untouched") }
        assertCardHolds("changed", id)
        let paths = try TileCdn.paths()
        expectEqual(Set(paths.filter { $0.hasSuffix(".poi") || $0.hasSuffix(".ebm") }),
                       Set(["/v1/\(id.prefix(6))/\(id).poi"] + filesFor(added, hashes)),
                       "only the changed .poi (and a hex new to the device) is fetched")
    }

    // MARK: 11. the rider's layer switch hides the device's water icons

    /// The Maps screen's "Device map layers" sheet: Drinking water off
    /// reaches the device (settings.map_layers), whose map loses the water
    /// icon north of the rider and nothing else; on again, it is back.
    func test12_WaterLayerSwitchHidesTheIcon() throws {
        executionTimeAllowance = 180
        let area = try requireCdn()
        guard (try SimControl.tiles(poi: true)).values.contains(where: { $0 != "cdn:" && !$0.isEmpty }) else {
            throw XCTSkip("needs test10's POIs on the device")
        }
        func layers(_ s: [String: Any]?) -> Int? { (s?["settings"] as? [String: Any])?["map_layers"] as? Int }
        expectEqual(layers(try? SimControl.status()), 0x3F, "all of the device's map layers should start on")
        try fixAtTheCentre(area)
        let on = try deviceMap("device-water-on")

        mapsLaunch(area)
        app.buttons["Settings"].firstMatch.tap()
        let mapsRow = app.buttons.matching(NSPredicate(format: "label BEGINSWITH 'Maps on SD card'")).firstMatch
        expect(mapsRow.waitForExistence(timeout: 10))
        mapsRow.tap()
        let layersButton = app.buttons["Device map layers"]
        expect(layersButton.waitForExistence(timeout: 15), "no Device map layers button on the Maps screen")
        layersButton.tap()
        let water = app.switches.matching(NSPredicate(format: "label BEGINSWITH 'Drinking water'")).firstMatch
        expect(water.waitForExistence(timeout: 10), "no Drinking water switch")
        // The app's toggle style is an ON | OFF pair at the row's trailing
        // edge: tap the half wanted.
        func flip(to mask: Int) -> Bool {
            for _ in 0..<3 {
                water.coordinate(withNormalizedOffset: CGVector(dx: mask & 0x04 != 0 ? 0.80 : 0.93, dy: 0.5)).tap()
                if SimControl.waitStatus(timeout: 5, { layers($0) == mask }) != nil { return true }
                print("FirmwareSimTests test12: device layers \(String(describing: layers(try? SimControl.status()))), want \(mask), switch \(String(describing: water.value))")
                let shot = XCTAttachment(screenshot: XCUIScreen.main.screenshot())
                shot.name = "layers want \(mask)"
                shot.lifetime = .keepAlways
                add(shot)
            }
            return false
        }
        let wentOff = flip(to: 0x3B)
        let off = try deviceMap("device-water-off")
        let cameBack = flip(to: 0x3F)
        let back = try deviceMap("device-water-back")
        app.terminate()

        expect(wentOff, "the device's layers never lost drinking water")
        expect(cameBack, "the device's layers never got drinking water back")
        let d = Panel.diff(on, off, box: waterIcon)
        let d2 = Panel.diff(on, back, box: waterIcon)
        print("FirmwareSimTests test12: off \(d), back \(d2)")
        expect(d.inside > 150, "the water icon is still drawn (\(d.inside) pixels changed)")
        expect(d.outside < 20, "more than the water icon changed (\(d.outside) pixels elsewhere)")
        expect(d2.inside + d2.outside < 20, "the map did not come back as it was (\(d2))")
    }

    // MARK: 12. Overpass timing out is not "no POIs here"

    /// The CDN cannot serve the rider's hex's .poi (500), so the app asks
    /// Overpass, which answers as it does when a query runs out of time: a
    /// 200 with no elements and a "runtime error" remark. That must not reach
    /// the device as an empty .poi; the Maps screen says the POIs could not be
    /// fetched and the device keeps what it had.
    func test13_OverpassTimeoutNeverReachesTheDeviceAsNoPois() throws {
        executionTimeAllowance = 240
        let area = try requireCdn()
        let (id, _, changed) = try editedHex(serve: "woodstock")
        guard try SimControl.tiles(poi: true)[id] == "cdn:" + changed else {
            throw XCTSkip("needs test11's POIs for \(id) on the device")
        }
        let before = try SimControl.tiles(poi: true)
        let card = SimEnv.cardFile(id, "poi")
        try TileCdn.fault("*.poi", "status:500")
        try TileCdn.fault("/api/interpreter", "remark")
        mapsLaunch(area)
        _ = try selectAreaAroundTheRider(area, button: Self.sendPoisButton, centreOnRider: true)
        let note = app.staticTexts.matching(NSPredicate(format: "label CONTAINS[c] 'could not be fetched'")).firstMatch
        let said = note.waitForExistence(timeout: 90)
        settleAndClose()

        let after = try SimControl.tiles(poi: true)
        print("FirmwareSimTests test13: said \(said), \(id): \(before[id] ?? "none") -> \(after[id] ?? "none")")
        expect(said, "the Maps screen never said the POIs could not be fetched")
        for (other, v) in before { expectEqual(after[other], v, "\(other).poi's record on the device changed") }
        expectEqual(SimEnv.cardFile(id, "poi"), card, "\(id).poi on the card changed")
        let queries = try TileCdn.overpassQueries()
        expect(queries.contains("poi"), "Overpass's POI query was never asked: \(queries)")
    }

    // MARK: 13. POIs built on the phone from Overpass

    /// The CDN cannot serve the hex's .poi (500): the app builds it from the
    /// (stand-in's) Overpass answer to its POI query, with its own H3 and
    /// MapBuilder.buildPoi. The device must hold the reference writer's bytes
    /// for the same POIs, recorded as built on this phone.
    func test14_OverpassPoisAreBuiltOnThePhone() throws {
        executionTimeAllowance = 240
        let area = try requireCdn()
        let (id, _, changed) = try editedHex(serve: "woodstock")
        guard try SimControl.tiles(poi: true)[id] == "cdn:" + changed else {
            throw XCTSkip("needs test13's POIs for \(id) on the device (test11's, kept)")
        }
        try TileCdn.fault("*.poi", "status:500")
        mapsLaunch(area)
        _ = try selectAreaAroundTheRider(area, button: Self.sendPoisButton, centreOnRider: true)
        _ = waitForPois { $0[id]?.hasPrefix("phone:") == true }
        settleAndClose()

        let pois = try SimControl.tiles(poi: true)
        print("FirmwareSimTests test14: \(id) -> \(pois[id] ?? "none")")
        expect(pois[id]?.hasPrefix("phone:") == true, "\(id).poi's record: \(pois[id] ?? "none")")
        assertCardHolds("woodstock", id)
        let queries = try TileCdn.overpassQueries()
        expect(!queries.isEmpty && queries.allSatisfy { $0 == "poi" }, "Overpass queries: \(queries)")
    }
}

// MARK: - routes (cov/nav)

extension FirmwareSimTests {
    /// A file on the device's card (`<OTP_SIM_DIR>/sd/<path>`).
    private func cardData(_ path: String) -> Data? {
        guard let d = SimEnv.deviceDir else { return nil }
        return try? Data(contentsOf: d.appendingPathComponent("sd/" + path))
    }

    /// The `lat`/`lon` pairs of every `<trkpt>` in a GPX (enough for the
    /// files these tests make and the app writes).
    private func trackPoints(_ data: Data) -> [(Double, Double)] {
        let s = String(decoding: data, as: UTF8.self)
        let re = try! NSRegularExpression(pattern: #"<trkpt lat="([-0-9.]+)" lon="([-0-9.]+)""#)
        return re.matches(in: s, range: NSRange(s.startIndex..., in: s)).compactMap { m in
            guard let a = Range(m.range(at: 1), in: s), let b = Range(m.range(at: 2), in: s),
                  let la = Double(s[a]), let lo = Double(s[b]) else { return nil }
            return (la, lo)
        }
    }

    /// Route → Import a route → GPX file → the Files picker → On My iPhone →
    /// lost_lake_loop (sim/fixtures/routes, put there by run_ios_tests.sh) →
    /// Send to device. The device must hold the track the file has — its 400
    /// points, not the two waypoints the file lists ahead of it — under the
    /// app's file name, loaded (an imported route has no turn cues, so no
    /// NAVIGATE prompt) and as long as the file's track.
    func testN01_GpxImportedThroughFilesReachesTheDevice() throws {
        executionTimeAllowance = 240
        guard SimEnv.deviceDir != nil else { throw XCTSkip("needs OTP_SIM_DIR (run sim/run_ios_tests.sh)") }
        launchConnected()
        app.buttons["Route"].firstMatch.tap()
        let importButton = app.buttons["Import a route"].firstMatch
        XCTAssertTrue(importButton.waitForExistence(timeout: 10), "no Import a route button")
        importButton.tap()
        let gpxItem = app.buttons["GPX file"].firstMatch
        XCTAssertTrue(gpxItem.waitForExistence(timeout: 5), "no GPX file in the import menu")
        gpxItem.tap()

        // The system document picker: Browse → On My iPhone → the file.
        let file = app.descendants(matching: .any)
            .matching(NSPredicate(format: "label BEGINSWITH 'lost_lake_loop'")).firstMatch
        if !file.waitForExistence(timeout: 8) {
            let browse = app.buttons["Browse"].firstMatch
            if browse.waitForExistence(timeout: 5) { browse.tap() }
            let onPhone = app.descendants(matching: .any)
                .matching(NSPredicate(format: "label == 'On My iPhone'")).firstMatch
            XCTAssertTrue(onPhone.waitForExistence(timeout: 10), "no On My iPhone in the picker")
            onPhone.tap()
        }
        XCTAssertTrue(file.waitForExistence(timeout: 15), "lost_lake_loop.gpx not in the picker")
        file.tap()

        let send = app.buttons.matching(NSPredicate(format: "label ==[c] 'Send to device'")).firstMatch
        if !send.waitForExistence(timeout: 15) {
            print("FirmwareSimTests test40: after the picker\n\(app.debugDescription)")
            XCTFail("no Send to device after the import")
        }
        send.tap()

        let st = SimControl.waitStatus(timeout: 30) { s in
            ((s["nav"] as? [String: Any])?["route"] as? Bool) == true
        }
        let nav = try XCTUnwrap(st?["nav"] as? [String: Any], "the device never loaded the route")
        XCTAssertEqual(nav["pending"] as? Bool, false, "a NAVIGATE prompt with no cues")
        XCTAssertTrue(app.descendants(matching: .any)
            .matching(NSPredicate(format: "label CONTAINS[c] 'by device'")).firstMatch.waitForExistence(timeout: 10),
            "the app never said the device received the route")

        let name = "lost_lake_loop.gpx"
        let stored = try XCTUnwrap(cardData("routes/" + name), "no routes/\(name) on the card")
        let sent = trackPoints(stored)
        XCTAssertEqual(sent.count, 400, "the device got \(sent.count) points; the file's track has 400")
        XCTAssertEqual(sent.first?.0 ?? 0, 45.7055, accuracy: 0.001, "the line starts at a waypoint")
        // 399 legs of 15 m.
        let km = nav["remaining_km"] as? Double ?? 0
        XCTAssertEqual(km, 399 * 15 / 1000.0, accuracy: 0.05, "the device's route is \(km) km")
    }
}
