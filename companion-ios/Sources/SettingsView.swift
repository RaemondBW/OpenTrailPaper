import SwiftUI

// Edit device settings (FTP, timezone) and push them over BLE.
struct SettingsView: View {
    @EnvironmentObject var ble: BLEManager
    @AppStorage(UnitPref.key) private var useMiles = false
    @State private var confirmUpdate = false
    @State private var showSensors = false

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
                            Text("Frontlight").trackedLabel()
                            Picker("Frontlight", selection: Binding(
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

                    Text(ble.state == .connected
                         ? "Settings sync automatically with your Bike GPS, both ways."
                         : "Connect to sync settings with your Bike GPS.")
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
        }
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
                        Text("App bundle: \(BLEManager.bundledFirmwareVersion)")
                            .font(.system(size: 12)).foregroundStyle(Palette.muted)
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
                } else if ble.updateAvailable {
                    PrimaryButton(title: "Install \(BLEManager.bundledFirmwareVersion)",
                                  systemImage: "arrow.down.circle",
                                  enabled: true) { confirmUpdate = true }
                } else if !ble.deviceFirmware.isEmpty {
                    Text("Up to date.").font(.system(size: 12)).foregroundStyle(Palette.muted)
                }
            }
        }
        .alert("Install firmware \(BLEManager.bundledFirmwareVersion)?",
               isPresented: $confirmUpdate) {
            Button("Cancel", role: .cancel) {}
            Button("Install") { ble.startFirmwareUpdate() }
        } message: {
            Text("This sends the new firmware to your Bike GPS and restarts it. It takes a few minutes — keep the app open and the device close. If anything goes wrong, the device keeps running its current firmware.")
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
        case .verifying:  return "Step 2 of 2 · Verifying"
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
