import SwiftUI

// Live device status — connection, GPS, battery, and ride data streamed
// over BLE. Mirrors the head unit's dashboard at a glance.
struct RideView: View {
    @EnvironmentObject var ble: BLEManager
    @AppStorage(UnitPref.key) private var useMiles = false

    var body: some View {
        NavigationStack {
            ScrollView {
                let s = ble.status
                VStack(spacing: 14) {
                    header(s)
                    gpsCard(s)
                    speedCard(s)
                    HStack(spacing: 14) {
                        sensorCard("Heart Rate", s.heartRate.map { "\($0)" }, unit: "bpm",
                                   sensor: ble.connectedSensor(kind: 1))
                        sensorCard("Power", s.power.map { "\($0)" }, unit: "W",
                                   sensor: ble.connectedSensor(kind: 2))
                    }
                    if s.hasRoute { routeCard(s) }
                    Spacer(minLength: 8)
                    if ble.state != .connected {
                        PrimaryButton(title: primaryTitle,
                                      systemImage: "antenna.radiowaves.left.and.right") {
                            ble.startScan()
                        }
                    }
                }
                .padding(16)
            }
            .background(Palette.paper.ignoresSafeArea())
            .navigationBarHidden(true)
            .onAppear { ble.refreshSensors() }
            .onChange(of: ble.state) { _, st in if st == .connected { ble.refreshSensors() } }
        }
    }

    // "Ride" title + a compact device-status pill (green dot + name + battery).
    private func header(_ s: DeviceStatus) -> some View {
        HStack(alignment: .center) {
            Text("Ride").font(TypeScale.screenTitle).foregroundStyle(Palette.ink)
            Spacer()
            HStack(spacing: 7) {
                Circle().fill(ble.state == .connected ? Palette.good : Palette.faint)
                    .frame(width: 9, height: 9)
                Text(ble.state == .connected ? "OpenTrailPaper" : connectLabel)
                    .font(BarlowFont.text(14, .semibold)).foregroundStyle(Palette.ink)
                if ble.state == .connected {
                    Text("\(s.battery)%").font(BarlowFont.text(14, .semibold))
                        .foregroundStyle(Palette.muted)
                }
            }
            .padding(.horizontal, 12).padding(.vertical, 7)
            .background(Palette.surface)
            .clipShape(Capsule())
            .overlay(Capsule().strokeBorder(Palette.hairline, lineWidth: 1))
        }
        .padding(.top, 8)
    }

    private func gpsCard(_ s: DeviceStatus) -> some View {
        Card {
            HStack(spacing: 12) {
                ZStack {
                    Circle().fill(s.gpsFix ? Palette.accentWash : Palette.paper)
                        .frame(width: 38, height: 38)
                    Image(systemName: s.gpsFix ? "location.fill" : "star")
                        .font(.system(size: 16, weight: .semibold))
                        .foregroundStyle(s.gpsFix ? Palette.accent : Palette.muted)
                }
                VStack(alignment: .leading, spacing: 2) {
                    Text(s.gpsFix ? "GPS lock" : "Acquiring GPS")
                        .font(BarlowFont.text(16, .semibold)).foregroundStyle(Palette.ink)
                    Text(s.gpsFix ? "\(s.sats) satellites in view"
                                  : "\(s.sats) satellites · clear view of sky helps")
                        .font(BarlowFont.text(13)).foregroundStyle(Palette.muted)
                }
                Spacer()
                VStack(alignment: .trailing, spacing: 0) {
                    Text("\(s.sats)").font(TypeScale.value(26))
                        .foregroundStyle(s.gpsFix ? Palette.good : Palette.accent)
                    Text("SATS").trackedLabel()
                }
            }
        }
    }

    private func speedCard(_ s: DeviceStatus) -> some View {
        Card {
            VStack(alignment: .leading, spacing: 2) {
                Text("Speed").trackedLabel()
                HStack(alignment: .firstTextBaseline, spacing: 6) {
                    Text(String(format: "%.1f", Units.speed(s.speedKmh, miles: useMiles)))
                        .font(TypeScale.hero(72)).foregroundStyle(Palette.ink)
                        .lineLimit(1).minimumScaleFactor(0.6)
                    Text(Units.speedLabel(useMiles))
                        .font(BarlowFont.condensed(22, .semibold)).foregroundStyle(Palette.muted)
                }
                Divider().overlay(Palette.hairline).padding(.vertical, 6)
                HStack(spacing: 22) {
                    miniStat("Avg", "—")
                    miniStat("Max", "—")
                }
            }
        }
    }

    private func miniStat(_ label: String, _ value: String) -> some View {
        HStack(spacing: 6) {
            Text(label).trackedLabel()
            Text(value).font(BarlowFont.condensed(17, .semibold)).foregroundStyle(Palette.ink)
        }
    }

    private func sensorCard(_ label: String, _ value: String?, unit: String,
                            sensor: BikeSensor?) -> some View {
        // Subtitle shows the connected sensor's name, else live/searching state.
        let subtitle: String
        let subtitleColor: Color
        if let sensor {
            subtitle = "\(sensor.name) · connected"; subtitleColor = Palette.good
        } else if value != nil {
            subtitle = "Live"; subtitleColor = Palette.good
        } else {
            subtitle = ble.state == .connected ? "Searching…" : "Not connected"
            subtitleColor = Palette.accent
        }
        return Card {
            VStack(alignment: .leading, spacing: 4) {
                Text(label).trackedLabel()
                HStack(alignment: .firstTextBaseline, spacing: 4) {
                    Text(value ?? "—").font(TypeScale.value(30)).foregroundStyle(Palette.ink)
                    Text(unit).font(BarlowFont.condensed(14, .medium)).foregroundStyle(Palette.muted)
                }
                Text(subtitle).font(BarlowFont.text(12, .medium))
                    .foregroundStyle(subtitleColor).lineLimit(1)
            }
        }
    }

    private func routeCard(_ s: DeviceStatus) -> some View {
        Card {
            VStack(alignment: .leading, spacing: 4) {
                Text("Active route").trackedLabel()
                HStack(alignment: .firstTextBaseline, spacing: 6) {
                    Text(String(format: "%.1f", Units.distance(s.remainingKm, miles: useMiles)))
                        .font(TypeScale.value(30)).foregroundStyle(Palette.ink)
                    Text("\(Units.distLabel(useMiles)) remaining")
                        .font(BarlowFont.text(14, .medium)).foregroundStyle(Palette.muted)
                }
            }
        }
    }

    private var connectLabel: String {
        switch ble.state {
        case .scanning: return "Searching…"
        case .connecting: return "Connecting…"
        case .poweredOff: return "Bluetooth off"
        default: return "Not connected"
        }
    }
    private var primaryTitle: String {
        ble.state == .scanning || ble.state == .connecting ? "Searching…" : "Connect to OpenTrailPaper"
    }
}

struct ConnectionBanner: View {
    @EnvironmentObject var ble: BLEManager

    var body: some View {
        Card {
            HStack(spacing: 12) {
                Circle()
                    .fill(dotColor)
                    .frame(width: 12, height: 12)
                VStack(alignment: .leading, spacing: 2) {
                    Text(title).font(TypeScale.body).foregroundStyle(Palette.ink)
                    if let msg = ble.lastMessage {
                        Text(msg).font(.system(size: 12)).foregroundStyle(Palette.muted)
                    }
                }
                Spacer()
                if ble.state != .connected {
                    Button("Scan") { ble.startScan() }
                        .font(.system(size: 14, weight: .semibold))
                }
            }
        }
    }

    private var dotColor: Color {
        switch ble.state {
        case .connected: return Palette.good
        case .scanning, .connecting: return Palette.accent
        default: return Palette.muted
        }
    }
    private var title: String {
        switch ble.state {
        case .connected: return "Connected to OpenTrailPaper"
        case .scanning: return "Searching…"
        case .connecting: return "Connecting…"
        case .poweredOff: return "Bluetooth is off"
        case .idle: return "Not connected"
        }
    }
}

struct Stat: View {
    let label: String
    let value: String
    var unit: String = ""
    var big: Bool = false

    var body: some View {
        Card {
            VStack(alignment: .leading, spacing: 4) {
                Text(label).trackedLabel()
                HStack(alignment: .firstTextBaseline, spacing: 4) {
                    Text(value)
                        .font(big ? TypeScale.hero(52) : TypeScale.value())
                        .foregroundStyle(Palette.ink)
                        .minimumScaleFactor(0.5)
                        .lineLimit(1)
                    Text(unit).font(TypeScale.label).foregroundStyle(Palette.muted)
                }
            }
        }
    }
}
