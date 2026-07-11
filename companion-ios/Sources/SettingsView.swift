import SwiftUI

// Edit device settings (FTP, timezone) and push them over BLE.
struct SettingsView: View {
    @EnvironmentObject var ble: BLEManager

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(spacing: 16) {
                    Card {
                        Stepper(value: $ble.ftpWatts, in: 50...500, step: 5) {
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
                            Stepper("Offset", value: $ble.tzMinutes,
                                    in: -12*60...14*60, step: 30)
                                .labelsHidden()
                        }
                    }

                    PrimaryButton(title: "Send to device",
                                  systemImage: "arrow.down.circle",
                                  enabled: ble.state == .connected) {
                        ble.pushSettings()
                    }

                    Text("Changes are saved on the device and persist across reboots.")
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
}
