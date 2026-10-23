import SwiftUI

/// Words for fixed pairing (firmware ble_server.cpp). The app can't remove the
/// iOS bond itself, so wherever the fix needs it, the copy names the exact
/// iOS Settings path.
enum PairingCopy {
    static let iosForgetSteps =
        "On this iPhone: Settings › Bluetooth › tap \u{24D8} next to \u{201C}OpenTrailPaper\u{201D} › Forget This Device."
    static let deviceUnpairSteps =
        "On the device: Menu › Paired Devices › Phone › Unpair."
    /// The device refused this phone (fixed pairing: it stays paired until
    /// unpaired on the device). Same words on every path that ends there.
    static let refusedSummary =
        "This OpenTrailPaper is paired with another phone (or forgot this one). On the device: Paired Devices › Phone › Unpair, then connect again."

    static func title(_ issue: BLEManager.PairingIssue) -> String {
        switch issue {
        case .pairedElsewhere: return "Paired with another phone"
        case .notRecognised: return "OpenTrailPaper isn't accepting this iPhone"
        case .pairingNotFinished: return "Pairing didn't finish"
        }
    }

    static func message(_ issue: BLEManager.PairingIssue) -> String {
        switch issue {
        case .pairedElsewhere:
            return "\(refusedSummary)\n\nThe app stops trying to connect until you tap Connect."
        case .notRecognised:
            return "\(refusedSummary) If you removed it in iPhone Settings › Bluetooth, that's expected.\n\nIf the device was unpaired but this iPhone still lists it: \(iosForgetSteps) Then tap Connect — the device shows a code to pair."
        case .pairingNotFinished:
            return "The pairing code wasn't entered (or didn't match). Tap Connect and type the 6-digit code shown on the OpenTrailPaper's screen."
        }
    }

    static let unpairMessage =
        "The device forgets this iPhone and opens pairing to the next phone, and the app stops connecting to it."
    static let afterUnpairMessage =
        "The device has forgotten this iPhone. One more step so they can pair again later: \(iosForgetSteps)"

    static let forgetMessage =
        "The app stops connecting to this device and looks for one to pair with.\n\nTo pair the device with a different phone, also unpair it on the device (Menu › Paired Devices › Phone › Unpair). To pair this iPhone again later, first remove the old pairing: \(iosForgetSteps)"
}

/// Only what the Settings tab shows of the BLE manager (see BLEScope). Every
/// BLE publish used to re-evaluate the whole Settings body — dozens of cards,
/// UIKit steppers and pickers — mid-scroll, which made scrolling stutter.
struct SettingsSnapshot: Equatable {
    var state: BLEManager.ConnState
    var otaPhase: BLEManager.OTAPhase
    var otaInProgress: Bool
    var otaProgress: Double
    var otaMessage: String?
    var deviceFirmware: String
    var updateAvailable: Bool
    var battery: Int
    var pairedDeviceId: UUID?
    var pairedDeviceName: String?
    var bluetoothPermission: PermissionState
    var bluetoothPoweredOn: Bool
    var locationPermission: PermissionState
    var settingsCache: DeviceSettingsCache
    var settingsSyncNote: String?
    var ftpWatts: Int, tzMinutes: Int, backlight: Int
    var clock24h: Bool, usbDrive: Bool
    var wheelCircMm: Int?
    var sensors: [BikeSensor]
    var deviceTiles: Int
    var logFileURL: URL?
    var deviceLogs: [LogFile]
    var loadingLogs: Bool
    var downloadingLog: Bool
    var downloadingName: String?
    var downloadProgress: Double
    var queuedDownloads: [String]
    var unpairNeedsIOSForget: Bool

    @MainActor init(_ b: BLEManager) {
        state = b.state; otaPhase = b.otaPhase; otaInProgress = b.otaInProgress
        otaProgress = b.otaProgress; otaMessage = b.otaMessage
        deviceFirmware = b.deviceFirmware; updateAvailable = b.updateAvailable
        battery = b.status.battery
        pairedDeviceId = b.pairedDeviceId; pairedDeviceName = b.pairedDeviceName
        bluetoothPermission = b.bluetoothPermission; bluetoothPoweredOn = b.bluetoothPoweredOn
        locationPermission = b.locationPermission
        settingsCache = b.settingsCache; settingsSyncNote = b.settingsSyncNote
        ftpWatts = b.ftpWatts; tzMinutes = b.tzMinutes; backlight = b.backlight
        clock24h = b.clock24h; usbDrive = b.usbDrive; wheelCircMm = b.wheelCircMm
        sensors = b.sensors; deviceTiles = b.deviceTileIds.count
        logFileURL = b.logFileURL; deviceLogs = b.deviceLogs; loadingLogs = b.loadingLogs
        downloadingLog = b.downloadingLog
        // Ride downloads tick progress too; only a log's matters here.
        downloadingName = b.downloadingLog ? b.downloadingName : nil
        downloadProgress = b.downloadingLog ? b.downloadProgress : 0
        queuedDownloads = b.queuedDownloads
        unpairNeedsIOSForget = b.unpairNeedsIOSForget
    }
}

/// The Settings tab: hands SettingsView the manager without observing it, so
/// the tab's redraws come from its BLEScope alone.
struct SettingsTab: View {
    @EnvironmentObject private var ble: BLEManager
    var body: some View { SettingsView(ble: ble).equatable() }
}

// Edit device settings (FTP, timezone) and push them over BLE.
struct SettingsView: View, Equatable {
    @StateObject private var scope: BLEScope<SettingsSnapshot>
    private var ble: BLEManager { scope.ble }

    init(ble: BLEManager) { _scope = StateObject(wrappedValue: BLEScope(ble, SettingsSnapshot.init)) }

    /// Same manager, same view: the parent's re-evaluations (every BLE
    /// publish) stop here.
    nonisolated static func == (a: SettingsView, b: SettingsView) -> Bool { true }
    @ObservedObject private var release = FirmwareRelease.shared
    @EnvironmentObject var appState: AppState
    @AppStorage(UnitPref.key) private var useMiles = false
    @State private var confirmUpdate = false
    @State private var notesExpanded = false
    @State private var showSensors = false
    @State private var showMaps = false
    @State private var confirmForget = false
    // The rim last chosen for the wheel size: the device keeps only the
    // circumference, which two rims can share.
    @AppStorage("wheelRimBsd") private var wheelRimBsd = WheelSize.defaultRim

    var body: some View {
        let _ = PerfMonitor.body("Settings")
        NavigationStack {
            VStack(spacing: 0) {
            // The sync state lives in the header, one line whose text changes
            // in place. As a hint at the top of the list it appeared on every
            // edit ("Syncing…") and vanished on the device's echo, shoving the
            // whole page down and back up each time a setting changed.
            ScreenHeader(title: "Settings") {
                HStack(spacing: 6) {
                    Image(systemName: ble.hasPendingSync ? "arrow.triangle.2.circlepath" : "checkmark")
                        .font(.system(size: 10, weight: .bold))
                    Text(syncLine)
                        .font(BarlowFont.text(11, .bold)).tracking(1.5)
                        .lineLimit(1).minimumScaleFactor(0.7)
                }
                .foregroundStyle(Palette.ink)
                .accessibilityElement(children: .combine)
            } trailing: { EmptyView() }
            ScrollViewReader { proxy in
            ScrollView {
                VStack(spacing: 14) {
                    if let note = ble.settingsSyncNote {
                        Text(note).font(BarlowFont.text(12)).foregroundStyle(Palette.muted)
                            .frame(maxWidth: .infinity, alignment: .leading)
                    }
                    pairedDeviceCard
                    // Keep the firmware card visible through the reboot/disconnect
                    // of an in-progress (or just-finished) update so its status
                    // doesn't vanish.
                    if ble.state == .connected || ble.otaInProgress
                        || ble.otaPhase == .failed || ble.otaPhase == .done { firmwareCard }
                    if ble.state != .connected { connectOnlyCard }
                    SyncAccountsCard()
                    deviceSection
                    if ble.state == .connected, let mm = ble.wheelCircMm {
                        wheelCard(mm)
                    }

                    AlbumArtLookupCard(art: ble.albumArt)

                    if ble.state == .connected { diagnosticsCard }

                    permissionsCard
                    tutorialCard

                    Text(ble.state == .connected
                         ? "Settings sync automatically with your OpenTrailPaper, both ways."
                         : ble.settingsCache.device == nil
                         ? "Changes you make here are sent to your OpenTrailPaper when it first connects."
                         : "Showing your OpenTrailPaper's settings from the last connection. Changes sync when it reconnects.")
                        .multilineTextAlignment(.center)
                        .font(.system(size: 13))
                        .foregroundStyle(Palette.muted)
                        .frame(maxWidth: .infinity, alignment: .center)
                        .padding(.top, 4)
                }
                .padding(16)
            }
            // Screenshot hook, alongside -tab-*/-demo-* in BikeGPSCompanionApp:
            // scrolls down to the segmented pickers, toggle and steppers so
            // they can be captured (simctl can't scroll).
            .onAppear {
                if ProcessInfo.processInfo.arguments.contains("-settings-controls") {
                    DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) {
                        proxy.scrollTo("units", anchor: .top)
                    }
                }
                // Layout-stability check: flip the clock format a few seconds
                // in, so frames before and after can be compared.
                if ProcessInfo.processInfo.arguments.contains("-settings-flip") {
                    DispatchQueue.main.asyncAfter(deadline: .now() + 4) {
                        ble.setClock24h(!ble.clock24h)
                        ble.setBacklight((ble.backlight + 1) % 4)
                    }
                }
            }
            }
            }
            .background(Palette.paper.ignoresSafeArea())
            .navigationBarHidden(true)
            .sheet(item: Binding(get: { ble.logFileURL }, set: { ble.logFileURL = $0 })) { url in
                DiagnosticsView(url: url)
            }
            .sheet(isPresented: $showSensors) { SensorsView() }
            .sheet(isPresented: $showMaps) { MapsView() }
            .onAppear {
                // Screenshot hook: open the maps screen straight away.
                if ble.isDemoMaps { showMaps = true }
            }
        }
    }

    // The device: name, firmware and battery, an inverted CONNECTED chip —
    // and, fixed pairing being what it is, the way out.
    @ViewBuilder private var pairedDeviceCard: some View {
        let connected = ble.state == .connected
        VStack(alignment: .leading, spacing: 0) {
            HStack(spacing: 12) {
                DeviceGlyph().frame(width: 40, height: 56).accessibilityHidden(true)
                VStack(alignment: .leading, spacing: 2) {
                    Text((ble.pairedDeviceName ?? "OpenTrailPaper").uppercased())
                        .font(BarlowFont.text(15, .bold)).tracking(2)
                        .lineLimit(1).minimumScaleFactor(0.7)
                    Text(deviceDetail).font(BarlowFont.text(13))
                        .fixedSize(horizontal: false, vertical: true)
                }
                .foregroundStyle(Palette.ink)
                Spacer(minLength: 4)
                Tag(text: connected ? "Connected" : ble.pairedDeviceId == nil ? "No device" : "Offline",
                    inverted: connected)
            }
            .padding(.horizontal, 14).padding(.vertical, 12)
            .accessibilityElement(children: .combine)
            GroupedDivider()
            VStack(alignment: .leading, spacing: 8) {
                if ble.pairedDeviceId != nil {
                    Text("The app only connects to this device, and the device only accepts this iPhone until it is unpaired on the device (Menu › Paired Devices › Phone).")
                        .font(BarlowFont.text(12)).foregroundStyle(Palette.muted)
                    Button(role: .destructive) { confirmForget = true } label: {
                        Text((connected ? "Unpair This Device" : "Forget This Device").uppercased())
                            .font(BarlowFont.text(12, .bold)).tracking(1.5)
                            .foregroundStyle(Palette.ink)
                            .frame(minHeight: 32)
                    }
                    .buttonStyle(.plain)
                } else {
                    Text(connected
                         ? "Pairing with this OpenTrailPaper…"
                         : "No device yet. Turn your OpenTrailPaper on nearby — the first one that isn't paired with another phone connects. Type the 6-digit code from its screen into the iPhone's pairing prompt.")
                        .font(BarlowFont.text(12)).foregroundStyle(Palette.muted)
                }
            }
            .padding(.horizontal, 14).padding(.vertical, 10)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .einkBorder()
        .confirmationDialog(ble.state == .connected ? "Unpair this OpenTrailPaper?"
                                                    : "Forget this OpenTrailPaper?",
                            isPresented: $confirmForget, titleVisibility: .visible) {
            if ble.state == .connected {
                Button("Unpair", role: .destructive) { ble.unpairDevice() }
            } else {
                Button("Forget Device", role: .destructive) { ble.forgetDevice() }
            }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text(ble.state == .connected ? PairingCopy.unpairMessage : PairingCopy.forgetMessage)
        }
        .alert("Unpaired", isPresented: Binding(get: { ble.unpairNeedsIOSForget },
                                                set: { ble.unpairNeedsIOSForget = $0 })) {
            Button("OK", role: .cancel) {}
        } message: {
            Text(PairingCopy.afterUnpairMessage)
        }
    }

    /// Device settings stay editable offline (cached per device, synced on
    /// the next connect); this says which state they are in.
    private var syncLine: String {
        let connected = ble.state == .connected
        if ble.hasPendingSync { return connected ? "SYNCING WITH DEVICE…" : "WILL SYNC WHEN CONNECTED" }
        return connected ? "IN SYNC WITH DEVICE" : "OFFLINE · CHANGES SYNC ON CONNECT"
    }

    private var deviceDetail: String {
        if ble.state == .connected {
            let fw = ble.deviceFirmware.isEmpty ? "" : "Firmware \(ble.deviceFirmware) · "
            return fw + "battery \(ble.status.battery)%"
        }
        return ble.pairedDeviceId == nil ? "Not paired" : "Not connected"
    }

    /// DEVICE: the head unit's own settings as one bordered list, binary ones
    /// as two-segment switches.
    private var deviceSection: some View {
        VStack(alignment: .leading, spacing: 6) {
            SettingsSectionHeader(title: "Device")
            GroupedRows {
                settingRow("Units") {
                    SegmentedSwitch(options: [("km", false), ("mi", true)],
                                    selection: Binding(get: { useMiles }, set: { ble.setUseMiles($0) }),
                                    accessibilityName: "Units")
                }
                .id("units")
                // Shown offline too; hidden only for firmware known to lack them.
                if ble.supportsSetting(.clock24h) {
                    GroupedDivider()
                    settingRow("Clock") {
                        SegmentedSwitch(options: [("24h", true), ("12h", false)],
                                        selection: Binding(get: { ble.clock24h }, set: { ble.setClock24h($0) }),
                                        accessibilityName: "Clock")
                    }
                }
                GroupedDivider()
                settingRow("Backlight") {
                    SegmentedSwitch(options: [("Off", 0), ("Lo", 1), ("Med", 2), ("Max", 3)],
                                    selection: Binding(get: { ble.backlight }, set: { ble.setBacklight($0) }),
                                    accessibilityName: "Backlight")
                }
                if ble.supportsSetting(.usbDrive) {
                    GroupedDivider()
                    VStack(alignment: .leading, spacing: 4) {
                        settingRow("USB drive", padded: false) {
                            SegmentedSwitch(options: [("On", true), ("Off", false)],
                                            selection: Binding(get: { ble.usbDrive }, set: { ble.setUsbDrive($0) }),
                                            accessibilityName: "USB drive")
                        }
                        Text(ble.usbDrive
                             ? "The SD mounts on a computer when plugged in. Turn off to keep the SD with the device (record/maps keep working while plugged in for power or serial)."
                             : "The SD stays with the device when plugged in. Turn on to copy maps, firmware, or logs from a computer.")
                            .font(BarlowFont.text(12)).foregroundStyle(Palette.muted)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                    .padding(.horizontal, 12).padding(.vertical, 10)
                }
                GroupedDivider()
                settingRow("FTP") {
                    HStack(spacing: 10) {
                        Text("\(ble.ftpWatts) W").font(DisplayFont.anton(22))
                        Stepper("FTP", value: Binding(get: { ble.ftpWatts }, set: { ble.setFtp($0) }),
                                in: 50...500, step: 5)
                            .labelsHidden()
                    }
                }
                GroupedDivider()
                settingRow("Timezone") {
                    HStack(spacing: 10) {
                        Text(tzLabel).font(DisplayFont.anton(22))
                        Stepper("Timezone offset", value: Binding(get: { ble.tzMinutes }, set: { ble.setTz($0) }),
                                in: -12*60...14*60, step: 30)
                            .labelsHidden()
                    }
                }
                if ble.state == .connected {
                    GroupedDivider()
                    linkRow("Sensors", sensorShort) { showSensors = true }
                }
                GroupedDivider()
                linkRow("Maps on SD card", mapsShort) { showMaps = true }
            }
        }
    }

    private func settingRow<C: View>(_ title: String, padded: Bool = true,
                                     @ViewBuilder control: () -> C) -> some View {
        HStack(spacing: 10) {
            Text(title).font(BarlowFont.text(15, .bold)).foregroundStyle(Palette.ink)
            Spacer(minLength: 8)
            control().fixedSize()
        }
        .padding(.horizontal, padded ? 12 : 0).padding(.vertical, padded ? 10 : 0)
        .frame(minHeight: padded ? 50 : 0)
    }

    private func linkRow(_ title: String, _ detail: String, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            HStack(spacing: 10) {
                Text(title).font(BarlowFont.text(15, .bold))
                Spacer(minLength: 8)
                Text(detail + " ›").font(BarlowFont.text(13, .semibold)).lineLimit(1)
                    .minimumScaleFactor(0.8)
            }
            .foregroundStyle(Palette.ink)
            .padding(.horizontal, 12).frame(minHeight: 50)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
    }

    private var sensorShort: String {
        let connected = ble.sensors.filter { $0.connected }
        if !connected.isEmpty { return connected.map(\.name).joined(separator: " · ") }
        let paired = ble.sensors.filter { $0.paired }
        return paired.isEmpty ? "Scan & pair" : "\(paired.count) saved"
    }

    private var mapsShort: String {
        let n = ble.deviceTileIds.count
        return n == 0 ? "Download" : "\(n) area\(n == 1 ? "" : "s")"
    }

    /// What needs the device's live answer and so waits for a connection:
    /// sensor scan/pairing, firmware updates and logs. (Mesh/LoRa lives on its
    /// own tab and says the same there.)
    @ViewBuilder private var connectOnlyCard: some View {
        Card {
            HStack(alignment: .top, spacing: 12) {
                Image(systemName: "antenna.radiowaves.left.and.right.slash")
                    .font(.system(size: 18, weight: .semibold)).foregroundStyle(Palette.faint)
                    .frame(width: 24)
                VStack(alignment: .leading, spacing: 3) {
                    Text("Connect to your OpenTrailPaper").font(BarlowFont.text(15, .semibold))
                        .foregroundStyle(Palette.ink)
                    Text("for sensors, firmware updates and diagnostic logs. The settings below work offline and sync when it connects.")
                        .font(.system(size: 12)).foregroundStyle(Palette.muted)
                        .fixedSize(horizontal: false, vertical: true)
                }
                Spacer(minLength: 0)
            }
        }
    }

    /// Permissions, and how to fix the ones that are missing.
    ///
    /// Once a permission has been refused the app can never prompt for it again
    /// — iOS only ever asks once — so without a route to Settings a "no" tapped
    /// during onboarding would be permanent and unexplained. Granted ones are
    /// listed too, so this reads as the whole picture rather than only a list of
    /// complaints.
    @ViewBuilder private var permissionsCard: some View {
        Card {
            VStack(alignment: .leading, spacing: 12) {
                Text("Permissions").trackedLabel()
                permissionRow(
                    symbol: "dot.radiowaves.left.and.right",
                    name: "Bluetooth",
                    state: ble.bluetoothPermission,
                    granted: ble.bluetoothPermission.isGranted && !ble.bluetoothPoweredOn
                        ? "Allowed, but Bluetooth is switched off — turn it on in Control Centre."
                        : "Allowed. This is how the app talks to your OpenTrailPaper.",
                    missing: "Without it the app can't reach your device at all — no routes, maps, settings or ride downloads.",
                    ask: { ble.enableBluetooth() })
                Divider().overlay(Palette.hairline)
                permissionRow(
                    symbol: "location.fill",
                    name: "Location",
                    state: ble.locationPermission,
                    granted: "Allowed while you're using the app.",
                    missing: "The app still works: you lose your position on the map, the GPS warm-start that helps the device lock on faster, and the backup fix when it can't see the sky.",
                    ask: { ble.requestLocationPermission() })

                if needsPermissionFix {
                    PrimaryButton(title: "Open Settings", systemImage: "arrow.up.forward.app",
                                  enabled: true) { openAppSettings() }
                    Text("Opens this app's page in Settings. Changes apply as soon as you come back.")
                        .font(.system(size: 11)).foregroundStyle(Palette.muted)
                }
            }
        }
    }

    /// True only when Settings can actually change something — a restricted
    /// permission or a switched-off radio is not fixable there, and offering the
    /// button anyway would send the user somewhere that can't help.
    private var needsPermissionFix: Bool {
        ble.bluetoothPermission.fixableInSettings || ble.locationPermission.fixableInSettings
    }

    private func permissionRow(symbol: String, name: String, state: PermissionState,
                               granted: String, missing: String,
                               ask: @escaping () -> Void) -> some View {
        HStack(alignment: .top, spacing: 12) {
            Image(systemName: symbol)
                .font(.system(size: 18, weight: .semibold))
                .foregroundStyle(state.isGranted ? Palette.good : Palette.accent)
                .frame(width: 24)
            VStack(alignment: .leading, spacing: 3) {
                HStack(spacing: 6) {
                    Text(name).font(BarlowFont.text(15, .semibold)).foregroundStyle(Palette.ink)
                    Text(statusLabel(state))
                        .font(.system(size: 10, weight: .bold))
                        .padding(.horizontal, 7).padding(.vertical, 3)
                        .background(state.isGranted ? Palette.good : Palette.accent)
                        .foregroundStyle(.white)
                        .clipShape(Rectangle())
                }
                Text(state.isGranted ? granted : missing)
                    .font(.system(size: 12)).foregroundStyle(Palette.muted)
                    .fixedSize(horizontal: false, vertical: true)
                // Never asked (skipped during the tutorial): iOS will still show
                // the real prompt, so ask here rather than sending the user to
                // Settings for something they were never offered.
                if state == .notDetermined {
                    Button(action: ask) {
                        Text("Allow \(name.lowercased())")
                            .font(.system(size: 13, weight: .semibold))
                            .foregroundStyle(Palette.accent)
                    }
                    .padding(.top, 2)
                }
            }
            Spacer(minLength: 0)
        }
    }

    private func statusLabel(_ state: PermissionState) -> String {
        switch state {
        case .granted:       return "ALLOWED"
        case .denied:        return "NOT ALLOWED"
        case .notDetermined: return "NOT ASKED"
        case .unavailable:   return "RESTRICTED"
        }
    }

    @ViewBuilder private var tutorialCard: some View {
        Button { appState.showTutorial = true } label: {
            Card {
                HStack(spacing: 12) {
                    Image(systemName: "questionmark.circle")
                        .font(.system(size: 20, weight: .semibold)).foregroundStyle(Palette.accent)
                    VStack(alignment: .leading, spacing: 2) {
                        Text("How it works").trackedLabel()
                        Text("Replay the intro tutorial")
                            .font(BarlowFont.text(15, .semibold)).foregroundStyle(Palette.ink)
                    }
                    Spacer()
                    Image(systemName: "chevron.right")
                        .font(.system(size: 14, weight: .semibold)).foregroundStyle(Palette.muted)
                }
            }
        }
        .buttonStyle(.plain)
    }

    @ViewBuilder private var diagnosticsCard: some View {
        Card {
            VStack(alignment: .leading, spacing: 12) {
                Text("Diagnostics").trackedLabel()
                Text("The device keeps a daily log (boot, GPS, BLE, OTA, errors). Grab today's, or pick a specific day.")
                    .font(.system(size: 12)).foregroundStyle(Palette.muted)
                // Logs share the one transfer channel with rides, so a tap while
                // something else is downloading waits its turn — say so, rather
                // than looking like the button did nothing.
                if !queuedLogs.isEmpty {
                    HStack(alignment: .top, spacing: 6) {
                        Image(systemName: "clock")
                        Text(queuedLogs.count == 1
                             ? "\(logDayLabel(queuedLogs[0])) queued — starts after the current download"
                             : "\(queuedLogs.count) logs queued — they start after the current download")
                            .fixedSize(horizontal: false, vertical: true)
                        Spacer(minLength: 0)
                    }
                    .font(.system(size: 12)).foregroundStyle(Palette.muted)
                }
                if ble.downloadingLog {
                    ProgressView(value: ble.downloadProgress) {
                        Text("Downloading \(logDayLabel(ble.downloadingName ?? "log"))…")
                            .font(.system(size: 12)).foregroundStyle(Palette.muted)
                    }
                } else {
                    PrimaryButton(title: "Download today's log",
                                  systemImage: "doc.text.magnifyingglass",
                                  enabled: true) { ble.downloadLog() }
                    Button { ble.requestLogList() } label: {
                        HStack(spacing: 6) {
                            Image(systemName: "calendar")
                            Text("Other days")
                            Spacer()
                            if ble.loadingLogs { ProgressView() }
                        }
                        .font(.system(size: 13, weight: .semibold)).foregroundStyle(Palette.accent)
                    }
                    ForEach(ble.deviceLogs) { log in
                        Button { ble.downloadLogFile(log.name) } label: {
                            HStack {
                                Text(logDayLabel(log.name)).font(.system(size: 13))
                                    .foregroundStyle(Palette.ink)
                                Spacer()
                                Text(sizeLabel(log.size)).font(.system(size: 11))
                                    .foregroundStyle(Palette.muted)
                                Image(systemName: "arrow.down.circle").foregroundStyle(Palette.accent)
                            }
                        }
                    }
                }
            }
        }
    }

    // Queued entries that are logs (rides are ".fit", today's log is "diag").
    private var queuedLogs: [String] {
        ble.queuedDownloads.filter { $0 == "diag" || $0.hasSuffix(".log") }
    }

    // "20260716.log" -> "Jul 16, 2026"; "pending.log" -> "Before first GPS fix".
    private func logDayLabel(_ name: String) -> String {
        let base = name.replacingOccurrences(of: ".log", with: "")
        if base == "pending" || base == "diag" { return base == "diag" ? "Today" : "Before first GPS fix" }
        guard base.count == 8, let _ = Int(base) else { return name }
        // The device names log files by the rider's LOCAL day now, so parse and
        // format in one zone (the phone's) — the label must repeat the name's
        // digits, not shift them. The old UTC parse re-interpreted the day and
        // could label 20260819.log as "Aug 18".
        let f = DateFormatter(); f.dateFormat = "yyyyMMdd"
        guard let d = f.date(from: base) else { return name }
        let out = DateFormatter(); out.dateFormat = "MMM d, yyyy"
        return out.string(from: d)
    }
    private func sizeLabel(_ bytes: Int) -> String {
        bytes >= 1024 ? "\(bytes / 1024) KB" : "\(bytes) B"
    }

    // Speed sensor wheel size, worked out from the rim and the tyre width
    // (WheelSize.circumference); the device stores the result in mm. A size no
    // rim and tyre gives (set over serial) shows as custom until one is chosen.
    private func wheelCard(_ mm: Int) -> some View {
        let decoded = WheelSize.decode(mm, preferredRim: wheelRimBsd)
        let rim = WheelSize.rim(decoded?.rim ?? wheelRimBsd)
        let tyre = decoded?.tyre ?? min(max(Int((Double(mm) / .pi - Double(rim.bsd)) / 2 + 0.5),
                                            WheelSize.minTyreMm), WheelSize.maxTyreMm)
        func set(_ bsd: Int, _ width: Int) {
            wheelRimBsd = bsd
            let w = min(max(width, WheelSize.minTyreMm), WheelSize.maxTyreMm)
            ble.setWheelCircMm(WheelSize.circumference(rim: bsd, tyreMm: w))
        }
        return Card {
            VStack(alignment: .leading, spacing: 10) {
                Text("Wheel size").trackedLabel()
                HStack {
                    Text("Rim").font(.system(size: 15)).foregroundStyle(Palette.ink)
                    Spacer()
                    Menu {
                        ForEach(WheelSize.rims, id: \.bsd) { r in
                            Button("\(r.name) · \(r.bsd) mm bead seat") { set(r.bsd, tyre) }
                        }
                    } label: {
                        Text(rim.name).font(.system(size: 15, weight: .semibold))
                    }
                }
                Stepper(value: Binding(get: { tyre }, set: { set(rim.bsd, $0) }),
                        in: WheelSize.minTyreMm...WheelSize.maxTyreMm, step: 1) {
                    VStack(alignment: .leading, spacing: 4) {
                        Text("Tyre width").trackedLabel()
                        Text(WheelSize.tyreLabel(tyre))
                            .font(TypeScale.value(30))
                            .foregroundStyle(Palette.ink)
                    }
                }
                Text(decoded == nil
                     ? "Custom size on the device: \(mm) mm. Choose a rim or width to replace it."
                     : "Wheel circumference ≈ \(mm) mm. A Bluetooth speed sensor counts wheel turns × this.")
                    .font(.system(size: 12)).foregroundStyle(Palette.muted)
            }
        }
    }

    private var tzLabel: String {
        let h = ble.tzMinutes / 60, m = abs(ble.tzMinutes % 60)
        return m == 0 ? String(format: "UTC%+d", h)
                      : String(format: "%+d:%02d", h, m)
    }

    /// Release-body markdown -> compact plain text: drop the trailing
    /// "Built from <sha>..." line CI appends, bullet the list items, unwrap
    /// the hard line breaks inside each item.
    static func whatsNew(_ body: String) -> String {
        var items: [String] = []
        for rawLine in body.split(separator: "\n", omittingEmptySubsequences: true) {
            let line = rawLine.trimmingCharacters(in: .whitespaces)
            if line.hasPrefix("Built from ") || line.isEmpty { continue }
            if line.hasPrefix("- ") {
                items.append("• " + line.dropFirst(2))
            } else if !items.isEmpty {
                items[items.count - 1] += " " + line   // wrapped continuation
            } else {
                items.append(line)
            }
        }
        return items.joined(separator: "\n")
    }

    @ViewBuilder private var firmwareCard: some View {
        Card {
            VStack(alignment: .leading, spacing: 12) {
                Text("Firmware").trackedLabel()
                HStack {
                    VStack(alignment: .leading, spacing: 2) {
                        Text("Device: \(ble.deviceFirmware.isEmpty ? "…" : ble.deviceFirmware)")
                            .font(TypeScale.body).foregroundStyle(Palette.ink)
                        // Latest comes from GitHub now, so it can be ahead of
                        // the installed app — shipping firmware no longer means
                        // shipping an app build.
                        if let r = release.latest {
                            Text("Latest: \(r.tag)")
                                .font(.system(size: 12)).foregroundStyle(Palette.muted)
                        } else if release.checking {
                            Text("Checking GitHub…")
                                .font(.system(size: 12)).foregroundStyle(Palette.muted)
                        } else {
                            Text(release.error ?? "Latest: unknown")
                                .font(.system(size: 12)).foregroundStyle(Palette.muted)
                        }
                    }
                    Spacer()
                    if ble.updateAvailable {
                        Text("UPDATE")
                            .font(.system(size: 11, weight: .bold))
                            .padding(.horizontal, 8).padding(.vertical, 4)
                            .background(Palette.accent).foregroundStyle(.white)
                            .clipShape(Rectangle())
                    }
                }

                // The release notes, straight from the version's CHANGELOG
                // section in the GitHub release body — the rider sees what
                // they're getting before tapping Install.
                if ble.updateAvailable, !ble.otaInProgress,
                   ble.otaPhase != .failed, ble.otaPhase != .done,
                   let r = release.latest {
                    let notes = Self.whatsNew(r.notes)
                    if !notes.isEmpty {
                        // Collapsed by default: the header plus a two-line
                        // taste of the top change, so the card stays compact
                        // but never says "update" without saying why. Tap
                        // anywhere on the panel to expand/collapse.
                        Button {
                            withAnimation(.easeInOut(duration: 0.2)) {
                                notesExpanded.toggle()
                            }
                        } label: {
                            VStack(alignment: .leading, spacing: 4) {
                                HStack {
                                    Text("What's new in \(r.tag)")
                                        .font(BarlowFont.text(13, .semibold))
                                        .foregroundStyle(Palette.ink)
                                    Spacer()
                                    Image(systemName: notesExpanded
                                          ? "chevron.up" : "chevron.down")
                                        .font(.system(size: 11, weight: .semibold))
                                        .foregroundStyle(Palette.faint)
                                }
                                Text(notes)
                                    .font(.system(size: 12))
                                    .foregroundStyle(Palette.muted)
                                    .lineLimit(notesExpanded ? nil : 2)
                                    .fixedSize(horizontal: false, vertical: true)
                                if !notesExpanded {
                                    Text("Show all")
                                        .font(BarlowFont.text(12, .semibold))
                                        .foregroundStyle(Palette.accent)
                                }
                            }
                            .padding(10)
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .background(Palette.paper,
                                        in: Rectangle())
                            .contentShape(Rectangle())
                        }
                        .buttonStyle(.plain)
                    }
                }

                if ble.otaInProgress {
                    otaProgressView
                } else if ble.otaPhase == .failed {
                    HStack(spacing: 8) {
                        Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(Palette.accent)
                        Text(ble.otaMessage ?? "Update failed").font(.system(size: 12))
                            .foregroundStyle(Palette.ink)
                    }
                    PrimaryButton(title: "Try again", systemImage: "arrow.clockwise",
                                  enabled: true) { ble.startFirmwareUpdate() }
                    Text("Or copy firmware.bin to the device's SD card and eject it — that path doesn't use Bluetooth.")
                        .font(.system(size: 11)).foregroundStyle(Palette.muted)
                } else if ble.otaPhase == .done {
                    HStack(spacing: 8) {
                        Image(systemName: "checkmark.circle.fill").foregroundStyle(Palette.good)
                        Text(ble.otaMessage ?? "Up to date").font(.system(size: 12))
                            .foregroundStyle(Palette.good)
                    }
                } else if let p = release.downloadProgress {
                    ProgressView(value: p) {
                        Text("Downloading firmware…").font(.system(size: 12))
                            .foregroundStyle(Palette.muted)
                    }
                } else if ble.updateAvailable, let r = release.latest {
                    PrimaryButton(title: "Install \(r.tag)",
                                  systemImage: "arrow.down.circle",
                                  enabled: true) { confirmUpdate = true }
                } else if !ble.deviceFirmware.isEmpty && release.latest != nil {
                    Text("Up to date.").font(.system(size: 12)).foregroundStyle(Palette.muted)
                } else if release.latest == nil && !release.checking {
                    Button("Check for updates") { Task { await release.check() } }
                        .font(.system(size: 13, weight: .semibold))
                        .foregroundStyle(Palette.accent)
                }
            }
        }
        .task { await release.check() }
        .alert("Install firmware \(release.latest?.tag ?? "")?",
               isPresented: $confirmUpdate) {
            Button("Cancel", role: .cancel) {}
            Button("Install") { ble.startFirmwareUpdate() }
        } message: {
            Text("The app downloads this release from GitHub, sends it to your OpenTrailPaper and restarts it. It takes a few minutes — keep the app open and the device close. If anything goes wrong, the device keeps running its current firmware.")
        }
    }

    // Clear, phase-based progress while an OTA is running.
    private var otaProgressView: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(spacing: 10) {
                if ble.otaPhase == .sending {
                    Image(systemName: "arrow.up.circle.fill").foregroundStyle(Palette.accent)
                } else {
                    ProgressView()
                }
                VStack(alignment: .leading, spacing: 2) {
                    Text(otaTitle).font(.system(size: 14, weight: .semibold))
                        .foregroundStyle(Palette.ink)
                    Text(ble.otaMessage ?? "").font(.system(size: 12))
                        .foregroundStyle(Palette.muted)
                }
                Spacer()
            }
            if ble.otaPhase == .sending {
                ProgressView(value: ble.otaProgress).tint(Palette.accent)
                Text("\(Int(ble.otaProgress * 100))% sent")
                    .font(.system(size: 11)).foregroundStyle(Palette.muted)
            }
            Text(otaHint).font(.system(size: 11)).foregroundStyle(Palette.muted)
        }
    }

    private var otaTitle: String {
        switch ble.otaPhase {
        case .sending:    return "Step 1 of 2 · Sending firmware"
        case .saving:     return "Step 1 of 2 · Saving to the device"
        case .installing: return "Step 2 of 2 · Installing"
        // The device is still flashing from SD here (its screen reads
        // "Installing"), so match that rather than saying "Verifying".
        case .verifying:  return "Step 2 of 2 · Installing"
        default:          return "Updating"
        }
    }

    private var otaHint: String {
        switch ble.otaPhase {
        case .sending, .saving:
            return "Keep the app open and the device nearby, and don't lock the phone."
        case .installing, .verifying:
            return "The device restarts to install (~30 s) and reconnects on its own. Keep it powered on and close."
        default:
            return ""
        }
    }
}

// Speed-sensor wheel size from rim and tyre: circumference = π × (rim bead
// seat diameter + 2 × tyre width), the tyre's height taken as its width. Within
// about 0.5% of a measured roll-out for road and MTB tyres. The device only
// stores the mm; limits match the firmware's wheel_speed::kMinCircMm..kMaxCircMm.
// Twin of WheelSize in the Android app's ble/Models.kt.
enum WheelSize {
    static let defaultMm = 2105
    static let minMm = 800
    static let maxMm = 3500
    static let minTyreMm = 18
    static let maxTyreMm = 120
    static let defaultRim = 622

    /// A rim by its ISO bead seat diameter (mm).
    struct Rim { let bsd: Int; let name: String }
    static let rims: [Rim] = [
        .init(bsd: 622, name: "700c / 29\""),
        .init(bsd: 584, name: "650b / 27.5\""),
        .init(bsd: 571, name: "650c"),
        .init(bsd: 559, name: "26\""),
        .init(bsd: 507, name: "24\""),
        .init(bsd: 406, name: "20\""),
        .init(bsd: 349, name: "16\" (Brompton)"),
    ]

    static func rim(_ bsd: Int) -> Rim { rims.first { $0.bsd == bsd } ?? rims[0] }

    static func circumference(rim bsd: Int, tyreMm: Int) -> Int {
        min(max(Int((Double.pi * Double(bsd + 2 * tyreMm)).rounded()), minMm), maxMm)
    }

    /// The rim and tyre width that give exactly `mm`, trying `preferredRim`
    /// first (650b × 47 and 700c × 28 are the same wheel). nil when no
    /// combination does: a size set over serial or by an older app.
    static func decode(_ mm: Int, preferredRim: Int) -> (rim: Int, tyre: Int)? {
        for r in [rim(preferredRim)] + rims {
            let tyre = Int(((Double(mm) / .pi - Double(r.bsd)) / 2).rounded())
            if (minTyreMm...maxTyreMm).contains(tyre), circumference(rim: r.bsd, tyreMm: tyre) == mm {
                return (r.bsd, tyre)
            }
        }
        return nil
    }

    /// "28 mm", or "56 mm · 2.2\"" for tyres sized in inches.
    static func tyreLabel(_ mm: Int) -> String {
        mm >= 45 ? String(format: "%d mm · %.1f\"", mm, Double(mm) / 25.4) : "\(mm) mm"
    }
}

/// The online cover lookup, where a rider looks for it. Also on the dashboard
/// editor's music page, next to the Music app permission.
private struct AlbumArtLookupCard: View {
    @ObservedObject var art: AlbumArtFeeder

    var body: some View {
        Card {
            VStack(alignment: .leading, spacing: 10) {
                Text("Album art").trackedLabel()
                Toggle("Look up album art online", isOn: $art.onlineLookup)
                Text("For Spotify and other players, finds the cover in Apple's iTunes catalogue. This sends the playing song's title, artist and album to Apple.")
                    .font(.system(size: 13))
                    .foregroundStyle(Palette.muted)
            }
        }
    }
}

/// The head unit, drawn the way the mockup draws it: a tall frame, a screen,
/// a key.
private struct DeviceGlyph: View {
    var body: some View {
        GeometryReader { g in
            let w = g.size.width, h = g.size.height
            ZStack(alignment: .topLeading) {
                Rectangle().strokeBorder(Palette.ink, lineWidth: 3)
                Rectangle().strokeBorder(Palette.ink, lineWidth: 1.5)
                    .frame(width: w * 0.65, height: h * 0.54)
                    .offset(x: w * 0.175, y: h * 0.14)
                Rectangle().fill(Palette.ink)
                    .frame(width: w * 0.3, height: h * 0.125)
                    .offset(x: w * 0.175, y: h * 0.75)
            }
        }
    }
}
