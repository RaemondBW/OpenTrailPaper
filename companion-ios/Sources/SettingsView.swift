import SwiftUI

// Edit device settings (FTP, timezone) and push them over BLE.
struct SettingsView: View {
    @EnvironmentObject var ble: BLEManager
    @ObservedObject private var release = FirmwareRelease.shared
    @EnvironmentObject var appState: AppState
    @AppStorage(UnitPref.key) private var useMiles = false
    @State private var confirmUpdate = false
    @State private var showSensors = false
    /// `-demo-maps` opens the Maps sheet at launch. The Maps screen is only
    /// reachable by tapping through Settings, so without this it could not be
    /// captured for the site, or driven from a simulator at all.
    @State private var showMaps = ProcessInfo.processInfo.arguments.contains("-demo-maps")

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(spacing: 14) {
                    HStack {
                        Text("Settings").font(TypeScale.screenTitle)
                            .foregroundStyle(Palette.ink)
                        Spacer()
                    }
                    .padding(.top, 8)
                    // Keep the firmware card visible through the reboot/disconnect
                    // of an in-progress (or just-finished) update so its status
                    // doesn't vanish.
                    if ble.state == .connected || ble.otaInProgress
                        || ble.otaPhase == .failed || ble.otaPhase == .done { firmwareCard }
                    if ble.state == .connected { sensorsCard }
                    // Not gated on the connection, unlike sensors: picking an
                    // area and fetching OSM works offline, and MapsView only
                    // needs the link for the upload itself. It was reachable
                    // with no connection from the Route page too, so gating it
                    // here would be a quiet regression.
                    mapsCard
                    Card {
                        VStack(alignment: .leading, spacing: 10) {
                            Text("Units").trackedLabel()
                            Picker("Units", selection: Binding(
                                get: { useMiles },
                                set: { ble.setUseMiles($0) })) {
                                Text("Metric (km)").tag(false)
                                Text("Standard (mi)").tag(true)
                            }
                            .pickerStyle(.segmented)
                        }
                    }

                    if ble.state == .connected {
                        Card {
                            VStack(alignment: .leading, spacing: 10) {
                                Text("Clock").trackedLabel()
                                Picker("Clock", selection: Binding(
                                    get: { ble.clock24h },
                                    set: { ble.setClock24h($0) })) {
                                    Text("24-hour").tag(true)
                                    Text("12-hour").tag(false)
                                }
                                .pickerStyle(.segmented)
                            }
                        }

                        Card {
                            VStack(alignment: .leading, spacing: 6) {
                                Toggle(isOn: Binding(get: { ble.usbDrive },
                                                     set: { ble.setUsbDrive($0) })) {
                                    Text("USB drive").trackedLabel()
                                }
                                Text(ble.usbDrive
                                     ? "The SD mounts on a computer when plugged in. Turn off to keep the SD with the device (record/maps keep working while plugged in for power or serial)."
                                     : "The SD stays with the device when plugged in. Turn on to copy maps, firmware, or logs from a computer.")
                                    .font(.system(size: 12)).foregroundStyle(Palette.muted)
                            }
                        }
                    }

                    Card {
                        Stepper(value: Binding(get: { ble.ftpWatts },
                                               set: { ble.setFtp($0) }),
                                in: 50...500, step: 5) {
                            VStack(alignment: .leading, spacing: 4) {
                                Text("FTP").trackedLabel()
                                Text("\(ble.ftpWatts) W")
                                    .font(TypeScale.value(30))
                                    .foregroundStyle(Palette.ink)
                            }
                        }
                    }

                    Card {
                        VStack(alignment: .leading, spacing: 10) {
                            Text("Timezone").trackedLabel()
                            Text(tzLabel)
                                .font(TypeScale.value(30))
                                .foregroundStyle(Palette.ink)
                            Stepper("Offset", value: Binding(get: { ble.tzMinutes },
                                                             set: { ble.setTz($0) }),
                                    in: -12*60...14*60, step: 30)
                                .labelsHidden()
                        }
                    }

                    Card {
                        VStack(alignment: .leading, spacing: 10) {
                            Text("Backlight").trackedLabel()
                            Picker("Backlight", selection: Binding(
                                get: { ble.backlight },
                                set: { ble.setBacklight($0) })) {
                                Text("Off").tag(0)
                                Text("Low").tag(1)
                                Text("Med").tag(2)
                                Text("Bright").tag(3)
                            }
                            .pickerStyle(.segmented)
                        }
                    }
                    .disabled(ble.state != .connected)

                    if ble.state == .connected { diagnosticsCard }

                    permissionsCard
                    tutorialCard

                    Text(ble.state == .connected
                         ? "Settings sync automatically with your OpenTrailPaper, both ways."
                         : "Connect to sync settings with your OpenTrailPaper.")
                        .font(.system(size: 13))
                        .foregroundStyle(Palette.muted)
                        .frame(maxWidth: .infinity, alignment: .center)
                        .padding(.top, 4)
                }
                .padding(16)
            }
            .background(Palette.paper.ignoresSafeArea())
            .navigationBarHidden(true)
            .sheet(item: $ble.logFileURL) { url in DiagnosticsView(url: url) }
            .sheet(isPresented: $showSensors) { SensorsView() }
            .sheet(isPresented: $showMaps) { MapsView() }
        }
    }

    @ViewBuilder private var mapsCard: some View {
        Button { showMaps = true } label: {
            Card {
                HStack(spacing: 12) {
                    Image(systemName: "map")
                        .font(.system(size: 20, weight: .semibold)).foregroundStyle(Palette.accent)
                    VStack(alignment: .leading, spacing: 2) {
                        Text("Maps").trackedLabel()
                        Text(mapsSummary)
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

    // deviceTileIds is cached in UserDefaults, so this still reads correctly
    // while disconnected rather than claiming the device holds nothing.
    private var mapsSummary: String {
        let n = ble.deviceTileIds.count
        if n == 0 { return "Download map areas to your device" }
        return "\(n) area\(n == 1 ? "" : "s") on device"
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
                        .clipShape(Capsule())
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

    @ViewBuilder private var sensorsCard: some View {
        Button { showSensors = true } label: {
            Card {
                HStack(spacing: 12) {
                    VStack(alignment: .leading, spacing: 4) {
                        Text("Sensors").trackedLabel()
                        Text(sensorSummary)
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

    private var sensorSummary: String {
        let connected = ble.sensors.filter { $0.connected }
        if !connected.isEmpty {
            return connected.map { $0.name }.joined(separator: ", ") + " connected"
        }
        let paired = ble.sensors.filter { $0.paired }
        if !paired.isEmpty {
            return "\(paired.count) saved · none connected"
        }
        return "Scan & pair heart rate, power, cadence"
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
        let f = DateFormatter(); f.dateFormat = "yyyyMMdd"; f.timeZone = TimeZone(secondsFromGMT: 0)
        guard let d = f.date(from: base) else { return name }
        let out = DateFormatter(); out.dateFormat = "MMM d, yyyy"
        return out.string(from: d)
    }
    private func sizeLabel(_ bytes: Int) -> String {
        bytes >= 1024 ? "\(bytes / 1024) KB" : "\(bytes) B"
    }

    private var tzLabel: String {
        let h = ble.tzMinutes / 60, m = abs(ble.tzMinutes % 60)
        return m == 0 ? String(format: "UTC%+d", h)
                      : String(format: "%+d:%02d", h, m)
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
                            .clipShape(Capsule())
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
