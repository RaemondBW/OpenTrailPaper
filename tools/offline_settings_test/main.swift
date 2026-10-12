import Foundation

// Checks for companion-ios/Sources/DeviceSettingsSync.swift — the iOS app has
// no test target, so run.sh compiles that file together with this driver.
// The cases mirror companion-android's DeviceSettingsSyncTest.kt one for one.

var failures = 0
func check(_ cond: Bool, _ what: String, line: Int = #line) {
    if !cond { failures += 1; print("FAIL (line \(line)): \(what)") }
}

let t0 = Date(timeIntervalSince1970: 1_700_000_000)
func edit(_ v: Int) -> PendingEdit { PendingEdit(value: v, editedAt: t0) }
func device(_ bytes: [UInt8]) -> DeviceSettingsValues { DeviceSettingsValues.decode(Data(bytes))! }

// ftp 250 (0x00FA), tz -420 (0xFE5C), km, backlight 2, 24h, usb on
let full: [UInt8] = [0xFA, 0x00, 0x5C, 0xFE, 0, 2, 1, 1]

// decodeEncodeRoundTrip
do {
    let d = device(full)
    check(d.length == 8, "length 8")
    check(d[.ftp] == 250 && d[.tz] == -420, "ftp/tz")
    check(d[.useMiles] == 0 && d[.backlight] == 2 && d[.clock24h] == 1 && d[.usbDrive] == 1, "bytes")
    check(d.encode() == Data(full), "round trip")
    check(DeviceSettingsValues.decode(Data([1, 2, 3])) == nil, "short payload rejected")
}

// noPendingNoWrite
do {
    let r = SettingsMerge.merge(device: device(full), lastKnown: device(full), pending: [:])
    check(r.payload == nil && r.pushed.isEmpty, "nothing to push")
}

// offlineEditWinsUntouchedFieldsTakeDevice
do {
    // Cached: km, backlight 2. Offline: rider sets backlight 0. Meanwhile on
    // the device: units -> miles. Push = miles (device) + backlight 0 (app).
    var onDevice = full; onDevice[4] = 1
    let r = SettingsMerge.merge(device: device(onDevice), lastKnown: device(full),
                                pending: ["backlight": edit(0)])
    check(r.pushed == ["backlight": 0], "only backlight pushed")
    check(r.payload == Data([0xFA, 0x00, 0x5C, 0xFE, 1, 0, 1, 1]), "merged payload")
    check(r.replacedDeviceChange.isEmpty, "no conflict")
}

// sameFieldChangedBothSidesAppWins
do {
    var onDevice = full; onDevice[5] = 3          // side button: bright
    let r = SettingsMerge.merge(device: device(onDevice), lastKnown: device(full),
                                pending: ["backlight": edit(1)])
    check(r.merged[.backlight] == 1, "app value wins")
    check(r.replacedDeviceChange == [.backlight], "reported as replacing the device's change")
}

// pendingEqualToDeviceNeedsNoWrite
do {
    let pending = ["ftp": edit(250)]
    let r = SettingsMerge.merge(device: device(full), lastKnown: nil, pending: pending)
    check(r.payload == nil, "no write")
    check(SettingsMerge.clearSatisfied(pending: pending, device: device(full)).isEmpty, "cleared")
}

// olderFirmwareShortPayload
do {
    let six = Array(full.prefix(6))
    let r = SettingsMerge.merge(device: device(six), lastKnown: nil,
                                pending: ["ftp": edit(300), "clock24h": edit(0)])
    check(r.payload?.count == 6, "6-byte write for 6-byte firmware")
    check(r.dropped == [.clock24h], "clock edit dropped")
    check(r.pushed == ["ftp": 300], "ftp pushed")
}

// ackClearsOnlyUnchangedEdits
do {
    let pending = ["ftp": edit(300), "tz": edit(60)]
    // ftp was re-edited to 310 while the write carrying 300 was in flight.
    var now = pending; now["ftp"] = edit(310)
    let left = SettingsMerge.clearAcknowledged(pending: now, pushed: ["ftp": 300, "tz": 60])
    check(left == ["ftp": edit(310)], "newer edit survives the ack")
}

// ackAppliesOnlyPushedFields
do {
    var notified = full; notified[4] = 1     // device-side units change arrived mid-write
    let d = SettingsMerge.applyAcknowledged(device: device(notified), pushed: ["ftp": 300])
    check(d[.ftp] == 300 && d[.useMiles] == 1, "pushed applied, notify kept")
}

// encodeRefusesHoles
do {
    var d = DeviceSettingsValues(); d.length = 8; d[.ftp] = 1; d[.tz] = 0
    check(d.encode() == nil, "unknown fields are never invented")
}

// effectiveValue
do {
    check(SettingsMerge.effective(.ftp, device: nil, pending: [:]) == nil, "nothing known")
    check(SettingsMerge.effective(.ftp, device: device(full), pending: [:]) == 250, "device")
    check(SettingsMerge.effective(.ftp, device: device(full), pending: ["ftp": edit(9)]) == 9, "pending")
}

// dashResolve
do {
    check(DashSync.resolve(device: "A", pending: nil) == .adoptDevice, "adopt")
    let p = PendingDash(text: "B", baseText: "A", editedAt: t0)
    check(DashSync.resolve(device: "B", pending: p) == .alreadyInSync, "in sync")
    check(DashSync.resolve(device: "A", pending: p) == .pushPhone, "push")
    check(DashSync.resolve(device: "C", pending: p) == .conflict, "conflict")
    check(DashSync.resolve(device: "A", pending: PendingDash(text: "B", baseText: nil, editedAt: t0)) == .conflict,
          "unknown base asks")
}

// dashSavingKeepsFirstBase
do {
    let first = DashSync.saving("B", deviceText: "A", over: nil, at: t0)
    check(first?.baseText == "A", "base = device")
    let second = DashSync.saving("C", deviceText: "A", over: first, at: t0)
    check(second?.baseText == "A" && second?.text == "C", "base kept")
    check(DashSync.saving("A", deviceText: "A", over: second, at: t0) == nil, "back to device cancels")
}

// storeAdoptsUnpairedEdits
do {
    let ud = UserDefaults(suiteName: "offline-settings-test-\(UUID().uuidString)")!
    let store = DeviceSettingsStore(defaults: ud)
    check(store.currentDevice == DeviceSettingsStore.unpairedKey, "starts unpaired")
    var c = DeviceSettingsCache(); c.pending["ftp"] = edit(280)
    store.save(c, for: DeviceSettingsStore.unpairedKey)
    let adopted = store.setCurrent("dev-1")
    check(adopted.pending["ftp"] == edit(280), "first device takes the unpaired edits")
    check(store.currentDevice == "dev-1", "current set")
    check(store.load(DeviceSettingsStore.unpairedKey).pending.isEmpty, "unpaired cleared")
    var c2 = adopted; c2.device = device(full)
    store.save(c2, for: "dev-1")
    _ = store.setCurrent("dev-2")
    check(store.load("dev-2").pending.isEmpty, "second device does not inherit dev-1's edits")
    check(store.setCurrent("dev-1") == c2, "per-device cache round-trips")
}

if failures == 0 { print("offline settings: all checks passed") } else { print("\(failures) failure(s)"); exit(1) }
