import SwiftUI

// Edit device settings (FTP, timezone) and push them over BLE.
struct SettingsView: View {
    @EnvironmentObject var ble: BLEManager
    @AppStorage(UnitPref.key) private var useMiles = false
    @State private var confirmUpdate = false

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(spacing: 16) {
                    if ble.state == .connected { firmwareCard }
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
            .background(Palette.paper)
            .navigationTitle("Settings")
        }
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
                    ProgressView(value: ble.otaProgress) {
                        Text(ble.otaMessage ?? "Updating…").font(.system(size: 12))
                            .foregroundStyle(Palette.muted)
                    }
                    Text("Keep the app open and the device nearby until it finishes.")
                        .font(.system(size: 11)).foregroundStyle(Palette.muted)
                } else if ble.updateAvailable {
                    PrimaryButton(title: "Install \(BLEManager.bundledFirmwareVersion)",
                                  systemImage: "arrow.down.circle",
                                  enabled: true) { confirmUpdate = true }
                } else if let msg = ble.otaMessage {
                    Text(msg).font(.system(size: 12)).foregroundStyle(Palette.muted)
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
}
