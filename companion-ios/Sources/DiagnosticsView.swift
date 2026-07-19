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
        let L = layout(s)
        ScrollView {
            VStack(alignment: .leading, spacing: 16) {
                if s.count >= 2 {
                    batterySummary(s)
                    Card {
                        VStack(alignment: .leading, spacing: 8) {
                            Text("Battery %").trackedLabel()
                            batteryChart(L, value: \.percent, color: Palette.accent,
                                         yDomain: 0...100).frame(height: 220)
                        }
                    }
                    Card {
                        VStack(alignment: .leading, spacing: 8) {
                            Text("Current draw (mA)").trackedLabel()
                            Text("Negative = discharging · shaded = unit off (time compressed)")
                                .font(.system(size: 11)).foregroundStyle(Palette.muted)
                            batteryChart(L, value: \.currentMa, color: Palette.good,
                                         yDomain: nil).frame(height: 180)
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

    // A point on the compressed timeline: `x` is the plotted position (off
    // periods collapsed), `session` groups contiguous awake runs so the line
    // breaks across an off period instead of drawing fake drain through it.
    private struct PlotPoint: Identifiable {
        let id: UUID
        let x: Double
        let percent: Double
        let currentMa: Double
        let session: Int
    }

    // Lay the samples out on a compressed x-axis: the device logs every ~2 min
    // while awake, so a gap bigger than that means it was off — collapse those
    // gaps to a thin fixed width so the active runtime fills the chart instead of
    // being squeezed by hours of off time. Returns the points, the x positions +
    // clock labels for each session start, and the collapsed off bands to shade.
    private func layout(_ s: [BatterySample])
        -> (pts: [PlotPoint], marks: [(x: Double, label: String)], offBands: [(Double, Double)]) {
        guard !s.isEmpty else { return ([], [], []) }
        let offThresh = 8.0 / 60.0    // gap > 8 min => unit was off
        let gapWidth  = 4.0 / 60.0    // draw each off period as a 4-min-wide break
        var pts: [PlotPoint] = []
        var marks: [(Double, String)] = [(0, String(s[0].timeLabel.prefix(5)))]
        var offBands: [(Double, Double)] = []
        var x = 0.0, session = 0
        for (i, sample) in s.enumerated() {
            if i > 0 {
                let gap = sample.hours - s[i - 1].hours
                if gap > offThresh {
                    offBands.append((x, x + gapWidth))
                    x += gapWidth
                    session += 1
                    marks.append((x, String(sample.timeLabel.prefix(5))))
                } else {
                    x += gap
                }
            }
            pts.append(PlotPoint(id: sample.id, x: x, percent: sample.percent,
                                 currentMa: sample.currentMa, session: session))
        }
        return (pts, marks, offBands)
    }

    @ViewBuilder
    private func batteryChart(_ L: (pts: [PlotPoint], marks: [(x: Double, label: String)],
                                    offBands: [(Double, Double)]),
                              value: KeyPath<PlotPoint, Double>,
                              color: Color, yDomain: ClosedRange<Double>?) -> some View {
        Chart {
            ForEach(L.offBands, id: \.0) { band in
                RectangleMark(xStart: .value("s", band.0), xEnd: .value("e", band.1))
                    .foregroundStyle(Palette.muted.opacity(0.12))
            }
            ForEach(L.pts) { p in
                LineMark(x: .value("t", p.x), y: .value("v", p[keyPath: value]),
                         series: .value("session", p.session))
                    .foregroundStyle(color)
                    .interpolationMethod(.monotone)
            }
        }
        .chartXAxis {
            AxisMarks(values: L.marks.map(\.x)) { v in
                AxisGridLine()
                AxisValueLabel {
                    if let d = v.as(Double.self),
                       let m = L.marks.first(where: { abs($0.x - d) < 1e-6 }) {
                        Text(m.label).font(.system(size: 9))
                    }
                }
            }
        }
        .modifier(YScale(domain: yDomain))
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

// Apply a fixed y-domain only when one is given; otherwise let the chart
// auto-scale (the current-draw chart has no natural fixed range).
private struct YScale: ViewModifier {
    let domain: ClosedRange<Double>?
    func body(content: Content) -> some View {
        if let domain { content.chartYScale(domain: domain) } else { content }
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
