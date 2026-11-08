import XCTest
@testable import OpenTrailPaper

// The version the app compares with the device's. A version release is tagged
// with it; the nightly's tag never moves, so CI titles it with the version
// instead (.github/workflows/build.yml, "Publish nightly").
final class FirmwareReleaseTests: XCTestCase {
    func testAVersionReleaseIsItsTag() {
        XCTAssertEqual(FirmwareRelease.version(tag: "v1.23", name: "Firmware v1.23"), "v1.23")
    }

    func testTheNightlyIsItsTitlesVersion() {
        XCTAssertEqual(FirmwareRelease.version(tag: "nightly", name: "Firmware v1.23-nightly.81dbf91"),
                       "v1.23-nightly.81dbf91")
    }

    func testANightlyTitledAnyOtherWayIsTheWholeTitle() {
        XCTAssertEqual(FirmwareRelease.version(tag: "nightly", name: "v1.24-nightly.abc1234"),
                       "v1.24-nightly.abc1234")
    }
}
