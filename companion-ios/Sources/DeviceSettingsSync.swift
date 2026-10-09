import Foundation

// Offline device settings: the cache, the rider's offline edits, and the merge
// that runs when the head unit comes back.
//
// Pure Foundation on purpose — no CoreBluetooth, no SwiftUI — so the merge
// rules can be compiled and checked on their own (tools/check-offline-settings.sh
// builds this file with a small test driver; the Android twin is
// DeviceSettingsSync.kt with JUnit tests mirroring the same cases).
//
// The rules, in one place:
//
//  * The cache holds the device's LAST REPORTED values, per device. It is only
//    ever written from what the device said (a read, a notify, or a write the
//    device acknowledged) — never from app defaults.
//  * An edit made in the app is a PENDING edit: a value plus when it was made.
//    The screen shows pending ?? cached ?? a display default.
//  * On reconnect the device's current values are read first, then merged:
//    a field the rider edited in the app takes the app's value; every other
//    field takes the device's (the rider may have changed backlight with the
//    side button, or units in the device menu, in the meantime).
//  * Both sides changed the same field: the device keeps no per-field change
//    time or counter, so "most recent wins" can't be decided — the app's edit
//    wins, and the merge reports that it replaced a device-side change.
//  * The payload is all-fields, so the push is built from the MERGED values,
//    and only as long as the device itself reported (older firmware takes
//    fewer bytes). Pending edits for fields the device doesn't have are
//    dropped rather than retried forever.
//  * Pending flags clear only once the write is acknowledged, and only for
//    fields still holding the value that was pushed (an edit made while the
//    write was in flight stays pending and goes next).

/// One field of the settings characteristic (src/ble_server.cpp SettingsCb).
/// Layout, little-endian: int16 ftpW, int16 tzMin, u8 useMiles, u8 backlight,
/// u8 clock24h, u8 usbDrive.
enum SettingField: String, CaseIterable, Codable {
    case ftp, tz, useMiles, backlight, clock24h, usbDrive

    /// Shortest payload that carries this field — the firmware applies a
    /// field only when the write is at least this long.
    var minLength: Int {
        switch self {
        case .ftp, .tz: return 4
        case .useMiles, .backlight: return 6
        case .clock24h: return 7
        case .usbDrive: return 8
        }
    }

    /// Rider-facing name, for the "replaced the device's change" note.
    var label: String {
        switch self {
        case .ftp: return "FTP"
        case .tz: return "Timezone"
        case .useMiles: return "Units"
        case .backlight: return "Backlight"
        case .clock24h: return "Clock"
        case .usbDrive: return "USB drive"
        }
    }

    static let maxLength = 8
}

/// The settings the device reported, and how many bytes it reported them in.
struct DeviceSettingsValues: Codable, Equatable {
    var values: [String: Int] = [:]   // SettingField.rawValue -> value
    /// Payload length the device sent: 4 (FTP + tz only), 6, 7 or 8.
    var length: Int = 0

    subscript(_ f: SettingField) -> Int? {
        get { values[f.rawValue] }
        set { values[f.rawValue] = newValue }
    }

    func supports(_ f: SettingField) -> Bool { length >= f.minLength }

    /// Parse a settings read/notify. nil when it is too short to be one.
    static func decode(_ d: Data) -> DeviceSettingsValues? {
        let b = [UInt8](d)
        guard b.count >= 4 else { return nil }
        var s = DeviceSettingsValues()
        s.length = min(b.count, SettingField.maxLength)
        s[.ftp] = Int(Int16(bitPattern: UInt16(b[0]) | UInt16(b[1]) << 8))
        s[.tz] = Int(Int16(bitPattern: UInt16(b[2]) | UInt16(b[3]) << 8))
        if b.count >= 6 {
            s[.useMiles] = b[4] != 0 ? 1 : 0
            s[.backlight] = Int(b[5])
        }
        if b.count >= 7 { s[.clock24h] = b[6] != 0 ? 1 : 0 }
        if b.count >= 8 { s[.usbDrive] = b[7] != 0 ? 1 : 0 }
        return s
    }

    /// The write payload, exactly `length` bytes long. nil if a field inside
    /// that length is unknown — never fill a hole with a made-up value.
    func encode() -> Data? {
        guard length >= 4 else { return nil }
        var out = [UInt8]()
        func i16(_ v: Int) {
            let u = UInt16(bitPattern: Int16(clamping: v))
            out.append(UInt8(u & 0xFF)); out.append(UInt8(u >> 8))
        }
        guard let ftp = self[.ftp], let tz = self[.tz] else { return nil }
        i16(ftp); i16(tz)
        if length >= 6 {
            guard let m = self[.useMiles], let bl = self[.backlight] else { return nil }
            out.append(m != 0 ? 1 : 0)
            out.append(UInt8(clamping: bl))
        }
        if length >= 7 {
            guard let c = self[.clock24h] else { return nil }
            out.append(c != 0 ? 1 : 0)
        }
        if length >= 8 {
            guard let u = self[.usbDrive] else { return nil }
            out.append(u != 0 ? 1 : 0)
        }
        return Data(out)
    }
}

/// A change the rider made in the app that the device has not acknowledged.
struct PendingEdit: Codable, Equatable {
    var value: Int
    var editedAt: Date
}

struct SettingsMergeResult: Equatable {
    /// What the device holds once the push lands (or already holds).
    var merged: DeviceSettingsValues
    /// The write to send; nil when nothing needs pushing.
    var payload: Data?
    /// Fields carried by the push because the rider edited them, with the
    /// value pushed — what an acknowledgement clears.
    var pushed: [String: Int]
    /// Pending edits the device can't take (older firmware): drop them.
    var dropped: [SettingField]
    /// Fields the device ALSO changed since the cache last saw it, which the
    /// app's edit is replacing (no device-side timestamp to compare).
    var replacedDeviceChange: [SettingField]
}

enum SettingsMerge {
    /// Merge the device's current values with the rider's pending edits.
    ///
    /// - device: what the device reports right now.
    /// - lastKnown: the cache from before this report (nil if never seen),
    ///   used only to tell whether the device side changed a field too.
    /// - pending: the rider's unacknowledged edits.
    static func merge(device: DeviceSettingsValues,
                      lastKnown: DeviceSettingsValues?,
                      pending: [String: PendingEdit]) -> SettingsMergeResult {
        var merged = device
        var pushed: [String: Int] = [:]
        var dropped: [SettingField] = []
        var replaced: [SettingField] = []
        for f in SettingField.allCases {
            guard let edit = pending[f.rawValue] else { continue }
            guard device.supports(f) else { dropped.append(f); continue }
            if let before = lastKnown?[f], let now = device[f], before != now, now != edit.value {
                replaced.append(f)
            }
            merged[f] = edit.value
            if device[f] != edit.value { pushed[f.rawValue] = edit.value }
        }
        // Pending edits equal to what the device already holds need no write;
        // the caller drops them with clearSatisfied.
        let payload = pushed.isEmpty ? nil : merged.encode()
        return SettingsMergeResult(merged: merged, payload: payload, pushed: pushed,
                                   dropped: dropped, replacedDeviceChange: replaced)
    }

    /// Pending edits the device already matches: no write needed, clear them.
    static func clearSatisfied(pending: [String: PendingEdit],
                               device: DeviceSettingsValues) -> [String: PendingEdit] {
        pending.filter { key, edit in
            guard let f = SettingField(rawValue: key) else { return false }
            return device[f] != edit.value
        }
    }

    /// After an acknowledged write: clear each pushed field whose pending
    /// value is still the one that was sent. A field edited again while the
    /// write was in flight keeps its newer pending value.
    static func clearAcknowledged(pending: [String: PendingEdit],
                                  pushed: [String: Int]) -> [String: PendingEdit] {
        pending.filter { key, edit in pushed[key] != edit.value }
    }

    /// The device's values with the acknowledged push applied — only the
    /// pushed fields, so a device-side notify that arrived during the write
    /// isn't rolled back for the fields the rider didn't touch.
    static func applyAcknowledged(device: DeviceSettingsValues,
                                  pushed: [String: Int]) -> DeviceSettingsValues {
        var d = device
        for (k, v) in pushed { d.values[k] = v }
        return d
    }

    /// What the screen shows for a field: pending, else device, else nil
    /// (the caller picks a display-only default).
    static func effective(_ f: SettingField, device: DeviceSettingsValues?,
                          pending: [String: PendingEdit]) -> Int? {
        pending[f.rawValue]?.value ?? device?[f]
    }
}

// MARK: - Dashboard layout

/// A layout the rider saved in the app that the device hasn't taken yet.
struct PendingDash: Codable, Equatable {
    /// The edited layout, normalized config text.
    var text: String
    /// The device layout this edit started from — what the cache held when
    /// the rider first edited. The reconnect compares the device against it
    /// to tell "device unchanged" from "changed on both sides".
    var baseText: String?
    var editedAt: Date
}

enum DashSync {
    enum Action: Equatable {
        /// No offline edit: take the device's layout.
        case adoptDevice
        /// The device already holds the phone's layout: clear the pending.
        case alreadyInSync
        /// The device is unchanged since the edit started: send the phone's.
        case pushPhone
        /// Both changed: ask the rider.
        case conflict
    }

    /// Texts are normalized config text (DashConfig.configText) on both sides.
    static func resolve(device: String, pending: PendingDash?) -> Action {
        guard let p = pending else { return .adoptDevice }
        if p.text == device { return .alreadyInSync }
        if let base = p.baseText, base == device { return .pushPhone }
        return .conflict
    }

    /// Save an edit on top of whatever is pending. The base stays the one the
    /// FIRST unsynced edit started from; an edit back to the device's own
    /// layout cancels the pending one.
    static func saving(_ text: String, deviceText: String?, over pending: PendingDash?,
                       at now: Date) -> PendingDash? {
        if pending == nil, let d = deviceText, d == text { return nil }
        let base = pending?.baseText ?? deviceText
        if let b = base, b == text { return nil }
        return PendingDash(text: text, baseText: base, editedAt: now)
    }
}

// MARK: - Per-device cache

/// Everything remembered about one device's settings.
struct DeviceSettingsCache: Codable, Equatable {
    var device: DeviceSettingsValues?
    var pending: [String: PendingEdit] = [:]
    /// The device's last reported layout (normalized config text).
    var dashText: String?
    var pendingDash: PendingDash?

    var hasPending: Bool { !pending.isEmpty || pendingDash != nil }
}

/// Persistence, keyed by device identity. "Current device" is one key:
/// the last device that connected, or — once a device is paired explicitly —
/// the paired one (`setCurrent(_:)`). Edits made before any device has ever
/// connected are kept under `unpairedKey` and adopted by the first device.
struct DeviceSettingsStore {
    static let unpairedKey = "_unpaired"
    private static let currentKey = "deviceSettings.current"
    private static func key(_ id: String) -> String { "deviceSettings.\(id)" }

    let defaults: UserDefaults

    var currentDevice: String {
        defaults.string(forKey: Self.currentKey) ?? Self.unpairedKey
    }

    func load(_ id: String) -> DeviceSettingsCache {
        guard let d = defaults.data(forKey: Self.key(id)),
              let c = try? JSONDecoder().decode(DeviceSettingsCache.self, from: d) else {
            return DeviceSettingsCache()
        }
        return c
    }

    func save(_ c: DeviceSettingsCache, for id: String) {
        if let d = try? JSONEncoder().encode(c) { defaults.set(d, forKey: Self.key(id)) }
    }

    /// Make `id` the current device and return its cache. Offline edits made
    /// before any device was known move over to it — they were made for
    /// "my device", and this is the first one there is.
    func setCurrent(_ id: String) -> DeviceSettingsCache {
        let previous = currentDevice
        defaults.set(id, forKey: Self.currentKey)
        var cache = load(id)
        if previous == Self.unpairedKey, id != Self.unpairedKey {
            let orphan = load(Self.unpairedKey)
            if !orphan.pending.isEmpty || orphan.pendingDash != nil {
                for (k, v) in orphan.pending where cache.pending[k] == nil { cache.pending[k] = v }
                if cache.pendingDash == nil, var pd = orphan.pendingDash {
                    pd.baseText = nil   // never saw this device's layout: ask, don't overwrite
                    cache.pendingDash = pd
                }
                save(cache, for: id)
            }
            defaults.removeObject(forKey: Self.key(Self.unpairedKey))
        }
        return cache
    }
}
