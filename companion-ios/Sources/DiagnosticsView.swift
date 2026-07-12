import SwiftUI
import Charts

// Shows a downloaded device diagnostics log: a battery drain chart parsed from
// the "battery:" lines, and the raw log text. Shareable.
struct DiagnosticsView: View {
    let url: URL
    @Environment(\.dismiss) private var dismiss
    @State private var tab = 0

    private var text: String { (try? String(contentsOf: url, encoding: .utf8)) ?? "" }
    private var samples: [BatterySample] { BatterySample.parse(text) }

    var body: some View {
        NavigationStack {
            VStack(spacing: 0) {
                Picker("", selection: $tab) {
                    Text("Battery").tag(0)
                    Text("Log").tag(1)
                }
                .pickerStyle(.segmented)
                .padding(16)

                if tab == 0 { batteryTab } else { logTab }
            }
            .background(Palette.paper)
            .navigationTitle("Diagnostics")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Done") { dismiss() }
                }
                ToolbarItem(placement: .primaryAction) {
                    ShareLink(item: url) { Image(systemName: "square.and.arrow.up") }
                }
            }
        }
    }

    // MARK: battery chart

    @ViewBuilder private var batteryTab: some View {
        let s = samples
        ScrollView {
            VStack(alignment: .leading, spacing: 16) {
                if s.count >= 2 {
                    batterySummary(s)
                    Card {
                        VStack(alignment: .leading, spacing: 8) {
                            Text("Battery %").trackedLabel()
                            Chart(s) { p in
                                LineMark(x: .value("Hours", p.hours),
                                         y: .value("Battery", p.percent))
                                    .foregroundStyle(Palette.accent)
                                    .interpolationMethod(.monotone)
                            }
                            .chartYScale(domain: 0...100)
                            .chartXAxisLabel("hours")
                            .frame(height: 220)
                        }
                    }
                    Card {
                        VStack(alignment: .leading, spacing: 8) {
                            Text("Current draw (mA)").trackedLabel()
                            Text("Negative = discharging")
                                .font(.system(size: 11)).foregroundStyle(Palette.muted)
                            Chart(s) { p in
                                LineMark(x: .value("Hours", p.hours),
                                         y: .value("mA", p.currentMa))
                                    .foregroundStyle(Palette.good)
                                    .interpolationMethod(.monotone)
                            }
                            .chartXAxisLabel("hours")
                            .frame(height: 180)
                        }
                    }
                } else {
                    Card {
                        Text("No battery data yet. Let the device run for a while (it logs every 2 min while awake), then download the log again.")
                            .font(TypeScale.body).foregroundStyle(Palette.muted)
                    }
                }
            }
            .padding(16)
        }
    }

    @ViewBuilder private func batterySummary(_ s: [BatterySample]) -> some View {
        let first = s.first!, last = s.last!
        let drop = first.percent - last.percent
        let hours = max(0.001, last.hours - first.hours)
        let rate = drop / hours                       // %/hr
        let avgDraw = s.map(\.currentMa).reduce(0, +) / Double(s.count)
        let runtime = rate > 0.01 ? last.percent / rate : 0   // hrs to empty
        Card {
            HStack(spacing: 16) {
                stat(String(format: "%.0f%%", last.percent), "Now")
                stat(String(format: "%.1f %%/hr", rate), "Drain")
                stat(String(format: "%.0f mA", avgDraw), "Avg draw")
                if runtime > 0 {
                    stat(String(format: "%.1f h", runtime), "Est. left")
                }
            }
        }
    }

    private func stat(_ value: String, _ label: String) -> some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(value).font(.system(size: 16, weight: .bold)).foregroundStyle(Palette.ink)
            Text(label).font(.system(size: 11)).foregroundStyle(Palette.muted)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    // MARK: raw log

    @ViewBuilder private var logTab: some View {
        ScrollView([.vertical, .horizontal]) {
            Text(text.isEmpty ? "(empty log)" : text)
                .font(.system(size: 11, design: .monospaced))
                .foregroundStyle(Palette.ink)
                .textSelection(.enabled)
                .padding(16)
                .frame(maxWidth: .infinity, alignment: .leading)
        }
    }
}

// One parsed "battery:" log entry.
struct BatterySample: Identifiable {
    let id = UUID()
    let hours: Double       // elapsed hours from the first sample
    let percent: Double
    let currentMa: Double
    let timeLabel: String

    // Parses lines like: "[14:32:10] battery: 87% 3912mV -142mA 1740/2000mAh discharging"
    static func parse(_ text: String) -> [BatterySample] {
        var out: [BatterySample] = []
        var base: Double? = nil
        var dayOffset = 0.0
        var lastSod: Double? = nil
        for lineSub in text.split(separator: "\n") {
            let line = String(lineSub)
            guard let bat = line.range(of: "battery:") else { continue }
            guard let lb = line.firstIndex(of: "["), let rb = line.firstIndex(of: "]"),
                  lb < rb else { continue }
            let ts = String(line[line.index(after: lb)..<rb])   // "HH:MM:SS" or "+Ns"
            let parts = ts.split(separator: ":")
            guard parts.count == 3, let h = Double(parts[0]),
                  let m = Double(parts[1]), let sec = Double(parts[2]) else { continue }
            var sod = h * 3600 + m * 60 + sec
            if let last = lastSod, sod < last - 1 { dayOffset += 86400 }  // past midnight
            lastSod = sod
            sod += dayOffset
            if base == nil { base = sod }

            let rest = String(line[bat.upperBound...])
            guard let pct = numberBefore("%", in: rest) else { continue }
            let ma = numberBefore("mA ", in: rest) ?? 0
            out.append(BatterySample(hours: (sod - (base ?? sod)) / 3600,
                                     percent: pct, currentMa: ma, timeLabel: ts))
        }
        return out
    }

    // The signed/decimal number immediately preceding `unit` in `s`.
    private static func numberBefore(_ unit: String, in s: String) -> Double? {
        guard let r = s.range(of: unit) else { return nil }
        var idx = r.lowerBound
        var chars: [Character] = []
        while idx > s.startIndex {
            let prev = s.index(before: idx)
            let c = s[prev]
            if c.isNumber || c == "." || c == "-" { chars.insert(c, at: 0); idx = prev }
            else { break }
        }
        return Double(String(chars))
    }
}
