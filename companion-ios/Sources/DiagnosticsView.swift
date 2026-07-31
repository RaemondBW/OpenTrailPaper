import SwiftUI
import Charts

// Shows a downloaded device diagnostics log: a battery drain chart parsed from
// the "battery:" lines, and the raw log text. Shareable.
struct DiagnosticsView: View {
    let url: URL
    @Environment(\.dismiss) private var dismiss
    @State private var tab = 0
    // One per chart: chartXSelection writes the tapped x here, and the two
    // charts are inspected independently.
    @State private var selPercent: Double?
    @State private var selDraw: Double?

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
                            Text("Tap the chart to read a value · shaded = unit off (time compressed)")
                                .font(.system(size: 11)).foregroundStyle(Palette.muted)
                            batteryChart(L, value: \.percent, color: Palette.accent,
                                         yDomain: 0...100, selection: $selPercent,
                                         format: { String(format: "%.0f%%", $0) })
                                .frame(height: 220)
                        }
                    }
                    Card {
                        VStack(alignment: .leading, spacing: 8) {
                            Text("Current draw (mA)").trackedLabel()
                            Text("Higher = drawing more · tap to read a value")
                                .font(.system(size: 11)).foregroundStyle(Palette.muted)
                            batteryChart(L, value: \.drawMa, color: Palette.good,
                                         yDomain: nil, selection: $selDraw,
                                         format: { String(format: "%.0f mA", $0) })
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

    // A point on the compressed timeline: `x` is the plotted position (off
    // periods collapsed), `session` groups contiguous awake runs so the line
    // breaks across an off period instead of drawing fake drain through it.
    private struct PlotPoint: Identifiable {
        let id: UUID
        let x: Double
        let percent: Double
        let currentMa: Double
        let session: Int
        let timeLabel: String

        // The gauge reports current signed from the battery's point of view, so
        // discharging is negative (-174mA) and it never goes positive in
        // practice. Plotting that put the whole "current draw" trace below zero,
        // which reads as though the device were producing power. Flip it so draw
        // is a positive number; a genuine charge current would show as negative.
        var drawMa: Double { -currentMa }
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
                                 currentMa: sample.currentMa, session: session,
                                 timeLabel: String(sample.timeLabel.prefix(5))))
        }
        return (pts, marks, offBands)
    }

    @ViewBuilder
    private func batteryChart(_ L: (pts: [PlotPoint], marks: [(x: Double, label: String)],
                                    offBands: [(Double, Double)]),
                              value: KeyPath<PlotPoint, Double>,
                              color: Color, yDomain: ClosedRange<Double>?,
                              selection: Binding<Double?>,
                              format: @escaping (Double) -> String) -> some View {
        // The x axis is the COMPRESSED position, not elapsed time, so snap to the
        // nearest plotted point rather than interpolating — otherwise a tap
        // inside a collapsed off-band would report a value that was never sampled.
        let hit = selection.wrappedValue.flatMap { x in
            L.pts.min(by: { abs($0.x - x) < abs($1.x - x) })
        }
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
            if let hit {
                RuleMark(x: .value("t", hit.x))
                    .foregroundStyle(Palette.muted.opacity(0.35))
                    .lineStyle(StrokeStyle(lineWidth: 1, dash: [3, 3]))
                PointMark(x: .value("t", hit.x), y: .value("v", hit[keyPath: value]))
                    .foregroundStyle(color)
                    .symbolSize(70)
                    .annotation(position: .top, spacing: 6,
                                overflowResolution: .init(x: .fit(to: .chart), y: .disabled)) {
                        VStack(alignment: .leading, spacing: 1) {
                            Text(format(hit[keyPath: value]))
                                .font(.system(size: 13, weight: .bold))
                                .foregroundStyle(Palette.ink)
                            Text(hit.timeLabel)
                                .font(.system(size: 10)).foregroundStyle(Palette.muted)
                        }
                        .padding(.horizontal, 8).padding(.vertical, 5)
                        .background(Palette.surface)
                        .clipShape(RoundedRectangle(cornerRadius: 7))
                        .overlay(RoundedRectangle(cornerRadius: 7)
                            .strokeBorder(Palette.hairline, lineWidth: 1))
                        .shadow(color: .black.opacity(0.10), radius: 4, y: 2)
                    }
            }
        }
        .chartXSelection(value: selection)
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

    // Drain measured over DISCHARGING time only.
    //
    // This used to be (first% - last%) / total elapsed, which any mid-log charge
    // destroys: charge back up and the net drop collapses while the clock keeps
    // running, so the rate reads far too low and "Est. left" far too high. On a
    // real log that went 99% -> 47%, charged to 100%, then ran down to 80% over
    // 9.5 h, it reported 2.0 %/hr and 40 h left; the true figures are 10.8 %/hr
    // and 7.4 h.
    //
    // So accumulate drop and time only across adjacent pairs that are genuinely
    // discharging: skip any pair where the percentage ROSE (charging), and any
    // pair separated by more than the off-threshold (the unit was asleep, and
    // that wall-clock time is not runtime). Same threshold the x-axis layout
    // uses, so the summary and the shaded bands agree about what "off" means.
    private func drainStats(_ s: [BatterySample]) -> (rate: Double, avgDraw: Double)? {
        let offThresh = 8.0 / 60.0
        var drop = 0.0, hours = 0.0
        for (a, b) in zip(s, s.dropFirst()) {
            let gap = b.hours - a.hours
            if gap > offThresh { continue }        // unit was off
            if b.percent > a.percent { continue }  // charging
            drop += a.percent - b.percent
            hours += gap
        }
        // Average only samples actually drawing, and as a positive number to
        // match the flipped chart.
        let draws = s.map(\.currentMa).filter { $0 < 0 }.map { -$0 }
        let avg = draws.isEmpty ? 0 : draws.reduce(0, +) / Double(draws.count)
        guard hours > 0.01, drop > 0 else { return nil }
        return (drop / hours, avg)
    }

    @ViewBuilder private func batterySummary(_ s: [BatterySample]) -> some View {
        let last = s.last!
        let stats = drainStats(s)
        Card {
            HStack(spacing: 16) {
                stat(String(format: "%.0f%%", last.percent), "Now")
                if let stats {
                    stat(String(format: "%.1f %%/hr", stats.rate), "Drain")
                    stat(String(format: "%.0f mA", stats.avgDraw), "Avg draw")
                    stat(String(format: "%.1f h", last.percent / stats.rate), "Est. left")
                } else {
                    // Charging throughout, or too little discharging time to say.
                    stat("—", "Drain")
                    stat("—", "Est. left")
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
