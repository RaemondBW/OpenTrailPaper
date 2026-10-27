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
    /// a fixed span). Returns the number of hexes the Download button offers.
    private func selectAreaAroundTheRider(_ area: TileCdn.Area, beforeDownload: () -> Void = {},
                                          file: StaticString = #filePath, line: UInt = #line) throws -> Int {
        app.buttons["Settings"].firstMatch.tap()
        let mapsRow = app.buttons.matching(NSPredicate(format: "label BEGINSWITH 'Maps on SD card'")).firstMatch
        XCTAssertTrue(mapsRow.waitForExistence(timeout: 10), "no Maps row in Settings", file: file, line: line)
        mapsRow.tap()
        let select = app.buttons["Select area"]
        XCTAssertTrue(select.waitForExistence(timeout: 10), "Maps screen didn't open", file: file, line: line)
        allowLocationIfAsked()
        Thread.sleep(forTimeInterval: 3)   // the map centres on the rider's first fix
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
        let download = app.buttons.matching(NSPredicate(format: "label CONTAINS[c] 'download ' AND label CONTAINS[c] ' hex'")).firstMatch
        XCTAssertTrue(download.waitForExistence(timeout: 10), "no Download button after drawing a box", file: file, line: line)
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
}
