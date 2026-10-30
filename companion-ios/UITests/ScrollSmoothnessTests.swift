import XCTest

/// Scroll smoothness of the tab screens while a device is connected and
/// streaming status (-demo-live ticks it at 1 Hz, like the head unit).
/// Reports XCTest's scroll hitch metrics; compare runs, the simulator's
/// absolute numbers are not the phone's.
final class ScrollSmoothnessTests: XCTestCase {
    private func scroll(tab: String) {
        let app = XCUIApplication()
        app.launchArguments = [tab, "-demo-live", "-perf-log"]
        app.launch()
        let options = XCTMeasureOptions()
        options.iterationCount = 3
        measure(metrics: [XCTOSSignpostMetric.scrollDraggingMetric,
                          XCTOSSignpostMetric.scrollDecelerationMetric],
                options: options) {
            app.swipeUp(velocity: .slow)
            app.swipeUp(velocity: .fast)
            app.swipeDown(velocity: .slow)
            app.swipeDown(velocity: .fast)
        }
    }

    func testSettingsScroll() { scroll(tab: "-tab-settings") }
    func testRidesScroll() { scroll(tab: "-tab-rides") }
}
