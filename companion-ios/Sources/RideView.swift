import SwiftUI

// Live device status — connection, GPS, battery, and ride data streamed
// over BLE. Mirrors the head unit's dashboard at a glance.
struct RideView: View {
    @EnvironmentObject var ble: BLEManager

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(spacing: 16) {
                    ConnectionBanner()

                    let s = ble.status
                    HStack(spacing: 16) {
                        Stat(label: "Speed", value: String(format: "%.1f", s.speedKmh),
                             unit: "km/h", big: true)
                        Stat(label: "Battery", value: "\(s.battery)", unit: "%")
                    }
                    HStack(spacing: 16) {
                        Stat(label: "Heart Rate",
                             value: s.heartRate.map(String.init) ?? "—", unit: "bpm")
                        Stat(label: "Power",
                             value: s.power.map(String.init) ?? "—", unit: "W")
                    }

                    Card {
                        HStack {
                            Label(s.gpsFix ? "GPS lock" : "No GPS fix",
                                  systemImage: s.gpsFix ? "location.fill" : "location.slash")
                                .foregroundStyle(s.gpsFix ? Palette.good : Palette.muted)
                            Spacer()
                            Text("\(s.sats) sats").trackedLabel()
                        }
                        .font(TypeScale.body)
                    }

                    if s.hasRoute {
                        Card {
                            VStack(alignment: .leading, spacing: 6) {
                                Text("Active route").trackedLabel()
                                Text(String(format: "%.1f km remaining", s.remainingKm))
                                    .font(TypeScale.value(28))
                                    .foregroundStyle(Palette.ink)
                            }
                        }
                    }
                }
                .padding(16)
            }
            .background(Palette.paper)
            .navigationTitle("Ride")
        }
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
        case .connected: return "Connected to Bike GPS"
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
