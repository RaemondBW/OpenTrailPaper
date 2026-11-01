import XCTest
@testable import OpenTrailPaper

// The channel-share URL codec and the slot arithmetic behind "switch radio to
// match". The bytes are pinned against Meshtastic's own default channel URL, so a
// drift here is a QR the stock app would read differently.
final class MeshChannelURLTests: XCTestCase {
    /// Meshtastic's default: one unnamed channel with well-known key 1, LongFast,
    /// no region, hop limit 3, transmit on, slot from the name.
    let defaultURL = "https://meshtastic.org/e/#CgMSAQESBggBQANIAQ"

    func testDefaultVectorDecodes() {
        let share = MeshChannelURL.parse(defaultURL)
        XCTAssertEqual(share.channels.count, 1)
        XCTAssertEqual(share.channels.first?.name, "")
        XCTAssertEqual(share.channels.first?.psk, Data([0x01]))
        XCTAssertEqual(share.radio, MeshRadioConfig(usePreset: true, modemPreset: 0, region: 0,
                                                    hopLimit: 3, txEnabled: true, channelNum: 0))
    }

    func testDefaultVectorEncodes() {
        let radio = MeshRadioConfig(modemPreset: MeshRadioConfig.presetCodes["LongFast"]!)
        let data = MeshChannelURL.encode([(name: "", psk: Data([0x01]))], radio: radio)
        XCTAssertEqual([UInt8](data), [0x0a, 0x03, 0x12, 0x01, 0x01,
                                       0x12, 0x06, 0x08, 0x01, 0x40, 0x03, 0x48, 0x01])
        XCTAssertEqual(MeshChannelURL.url(forChannelNamed: "", psk: Data([0x01]), radio: radio),
                       defaultURL)
    }

    func testRoundTripPresetRegionSlot() {
        let key = Data((0..<32).map { UInt8($0 &* 7 &+ 3) })
        let radio = MeshRadioConfig(modemPreset: MeshRadioConfig.presetCodes["MediumFast"]!,
                                    region: MeshRadioConfig.regionCodes["US"]!,
                                    channelNum: 44)
        let url = MeshChannelURL.url(forChannelNamed: "Saturday Ride", psk: key, radio: radio)
        let share = MeshChannelURL.parse(url)
        XCTAssertEqual(share.channels.first?.name, "Saturday Ride")
        XCTAssertEqual(share.channels.first?.psk, key)
        XCTAssertEqual(share.radio, radio)
        XCTAssertEqual(share.radio?.presetName, "MediumFast")
        XCTAssertEqual(share.radio?.regionName, "US")
        XCTAssertEqual(share.radio?.summary, "MediumFast · US · slot 44")
    }

    func testUnknownFixed32FieldInLoraIsSkipped() {
        // lora_config = { use_preset 1, frequency_offset (fixed32, field 6) 1.5f,
        // modem_preset MediumFast, hop_limit 3, tx_enabled 1, channel_num 44 }.
        let lora: [UInt8] = [0x08, 0x01, 0x35, 0x00, 0x00, 0xc0, 0x3f,
                             0x10, 0x04, 0x40, 0x03, 0x48, 0x01, 0x58, 0x2c]
        var bytes: [UInt8] = [0x0a, 0x03, 0x12, 0x01, 0x01, 0x12, UInt8(lora.count)]
        bytes += lora
        let share = MeshChannelURL.decode(Data(bytes))
        XCTAssertEqual(share.channels.count, 1)
        XCTAssertEqual(share.radio?.modemPreset, 4)
        XCTAssertEqual(share.radio?.channelNum, 44)
        XCTAssertEqual(share.radio?.hopLimit, 3)
    }

    func testNoLoraConfigGivesNoRadio() {
        let url = MeshChannelURL.url(forChannelNamed: "Ride", psk: Data(repeating: 9, count: 16))
        let share = MeshChannelURL.parse(url)
        XCTAssertEqual(share.channels.first?.name, "Ride")
        XCTAssertNil(share.radio)
    }

    func testGarbledLoraKeepsChannels() {
        // lora_config's modem_preset varint has its continuation bit set and nothing after.
        let bytes: [UInt8] = [0x0a, 0x03, 0x12, 0x01, 0x01, 0x12, 0x02, 0x10, 0x80]
        let share = MeshChannelURL.decode(Data(bytes))
        XCTAssertEqual(share.channels.count, 1)
        XCTAssertNil(share.radio)
    }

    // MARK: - Slots

    // US: 902–928 MHz, spacing 0, 250 kHz presets.
    let us = MeshRegion(index: 0, name: "US", startHz: 902_000_000, endHz: 928_000_000,
                        spacingHz: 0, powerLimitDbm: 30)

    func testUSChannelCount() {
        XCTAssertEqual(MeshSlots.channelCount(startHz: us.startHz, endHz: us.endHz,
                                              spacingHz: us.spacingHz, bandwidthKhz: 250), 104)
    }

    func testSlotForPresetNames() {
        XCTAssertEqual(MeshSlots.slot(forName: "LongFast", count: 104), 19)
        XCTAssertEqual(MeshSlots.frequencyMHz(slot: 19, startHz: us.startHz,
                                              spacingHz: 0, bandwidthKhz: 250),
                       906.875, accuracy: 1e-6)
        XCTAssertEqual(MeshSlots.slot(forName: "MediumFast", count: 104), 44)
        XCTAssertEqual(MeshSlots.frequencyMHz(slot: 44, startHz: us.startHz,
                                              spacingHz: 0, bandwidthKhz: 250),
                       913.125, accuracy: 1e-6)
    }

    // MARK: - Matching a scanned code

    let presets = [
        MeshPreset(index: 0, name: "LongFast", sf: 11, bandwidthKhz: 250, codingRate: 5),
        MeshPreset(index: 2, name: "MediumFast", sf: 9, bandwidthKhz: 250, codingRate: 5),
    ]

    /// A device on LongFast, US, automatic slot — which is slot 20 (1-based).
    func usLongFast() -> MeshState {
        var s = MeshState()
        s.presetIndex = 0
        s.regionIndex = 0
        s.slotOverride = 0
        s.activeSlot = 20
        return s
    }

    func testMatchSameSettingsIsEmpty() {
        let code = MeshRadioConfig(modemPreset: 0, region: 1, channelNum: 20)
        let m = MeshRadioMatch(code, state: usLongFast(), presets: presets, regions: [us],
                               primaryName: nil)
        XCTAssertTrue(m.isEmpty)
        XCTAssertNil(m.slotOverride)
    }

    func testMatchPresetChangeLandsOnAutomaticSlot() {
        // MediumFast's own hash slot is 45, so no pin is needed.
        let code = MeshRadioConfig(modemPreset: 4, region: 1, channelNum: 45)
        let m = MeshRadioMatch(code, state: usLongFast(), presets: presets, regions: [us],
                               primaryName: nil)
        XCTAssertEqual(m.preset?.name, "MediumFast")
        XCTAssertNil(m.region)
        XCTAssertNil(m.slotOverride)
        XCTAssertEqual(m.changes, ["Speed: LongFast → MediumFast", "Slot: 20 → 45"])
    }

    func testMatchOtherSlotPinsIt() {
        let code = MeshRadioConfig(modemPreset: 4, region: 1, channelNum: 44)
        let m = MeshRadioMatch(code, state: usLongFast(), presets: presets, regions: [us],
                               primaryName: nil)
        XCTAssertEqual(m.slotOverride, 44)
    }

    func testMatchUnknownPresetIsReported() {
        let code = MeshRadioConfig(modemPreset: 1, region: 1, channelNum: 20)   // LongSlow
        let m = MeshRadioMatch(code, state: usLongFast(), presets: presets, regions: [us],
                               primaryName: nil)
        XCTAssertNil(m.preset)
        XCTAssertEqual(m.unsupported, ["This device can't use LongSlow"])
    }
}
