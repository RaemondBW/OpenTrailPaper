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

    override func tearDown() {
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
}
