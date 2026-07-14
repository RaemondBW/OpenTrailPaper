import SwiftUI

// Scan for, pair, and forget the head unit's cycling sensors from the phone.
// The device does the actual BLE central work; this drives it over the sensors
// characteristic and shows live connection state.
struct SensorsView: View {
    @EnvironmentObject var ble: BLEManager
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(spacing: 14) {
                    HStack {
                        Text("Sensors").font(TypeScale.screenTitle).foregroundStyle(Palette.ink)
                        Spacer()
                        if ble.scanningSensors { ProgressView().padding(.trailing, 2) }
                    }
                    .padding(.top, 8)

                    if ble.state != .connected {
                        Card {
                            Text("Connect to your Bike GPS to manage sensors.")
                                .font(BarlowFont.text(15)).foregroundStyle(Palette.muted)
                        }
                    } else {
                        let paired = ble.sensors.filter { $0.paired }
                        let available = ble.sensors.filter { !$0.paired }

                        if !paired.isEmpty {
                            sectionLabel("My sensors")
                            ForEach(paired) { row($0) }
                        }

                        sectionLabel(ble.scanningSensors ? "Scanning for sensors…" : "Available")
                        if available.isEmpty {
                            Text(ble.scanningSensors
                                 ? "Wake your sensor (spin the cranks / touch the strap)."
                                 : "Tap Scan to search for nearby sensors.")
                                .font(BarlowFont.text(13)).foregroundStyle(Palette.muted)
                                .frame(maxWidth: .infinity, alignment: .leading)
                        } else {
                            ForEach(available) { row($0) }
                        }
                    }
                }
                .padding(16)
            }
            .background(Palette.paper.ignoresSafeArea())
            .navigationBarHidden(true)
            .safeAreaInset(edge: .bottom) {
                if ble.state == .connected {
                    HStack(spacing: 12) {
                        Button(ble.scanningSensors ? "Stop" : "Scan") {
                            ble.scanningSensors ? ble.stopSensorScan() : ble.startSensorScan()
                        }
                        .font(BarlowFont.condensed(20, .semibold))
                        .frame(maxWidth: .infinity).padding(.vertical, 15)
                        .background(Palette.surface).foregroundStyle(Palette.accent)
                        .clipShape(RoundedRectangle(cornerRadius: 26, style: .continuous))
                        .overlay(RoundedRectangle(cornerRadius: 26, style: .continuous)
                            .strokeBorder(Palette.hairline, lineWidth: 1))

                        Button("Done") { dismiss() }
                            .font(BarlowFont.condensed(20, .semibold))
                            .frame(maxWidth: .infinity).padding(.vertical, 15)
                            .background(Palette.accent).foregroundStyle(.white)
                            .clipShape(RoundedRectangle(cornerRadius: 26, style: .continuous))
                    }
                    .padding(16).background(Palette.paper)
                }
            }
            .onAppear {
                ble.refreshSensors()    // show paired sensors immediately…
                ble.startSensorScan()   // …and scan for new ones
            }
            .onDisappear { ble.stopSensorScan() }
        }
    }

    private func sectionLabel(_ t: String) -> some View {
        Text(t).trackedLabel()
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(.top, 4)
    }

    private func row(_ s: BikeSensor) -> some View {
        Card {
            HStack(spacing: 12) {
                ZStack {
                    Circle().fill(s.connected ? Palette.accentWash : Palette.paper)
                        .frame(width: 38, height: 38)
                    Image(systemName: icon(s.kindsMask))
                        .font(.system(size: 16, weight: .semibold))
                        .foregroundStyle(s.connected ? Palette.accent : Palette.muted)
                }
                VStack(alignment: .leading, spacing: 2) {
                    Text(s.name).font(BarlowFont.text(16, .semibold)).foregroundStyle(Palette.ink)
                        .lineLimit(1)
                    Text(statusText(s)).font(BarlowFont.text(13)).foregroundStyle(statusColor(s))
                }
                Spacer()
                if s.paired {
                    Button("Forget") { ble.forgetSensor(s.addr) }
                        .font(BarlowFont.text(14, .semibold)).foregroundStyle(Palette.muted)
                } else {
                    Button("Connect") { ble.pairSensor(s.addr) }
                        .font(BarlowFont.text(14, .semibold)).foregroundStyle(Palette.accent)
                }
            }
        }
    }

    private func statusText(_ s: BikeSensor) -> String {
        if s.connected { return "Connected · \(s.kindsText)" }
        if s.paired { return "Paired · \(s.rssi != 0 ? "in range" : "not connected")" }
        return "\(s.kindsText) · \(s.rssi) dBm"
    }
    private func statusColor(_ s: BikeSensor) -> Color {
        s.connected ? Palette.good : Palette.muted
    }
    private func icon(_ mask: UInt8) -> String {
        if mask & 1 != 0 { return "heart.fill" }
        if mask & 2 != 0 { return "bolt.fill" }
        if mask & 4 != 0 { return "arrow.triangle.2.circlepath" }
        return "dot.radiowaves.left.and.right"
    }
}
