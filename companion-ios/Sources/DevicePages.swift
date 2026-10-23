import SwiftUI
import UIKit

// The device's non-field pages — MUSIC, MAP, WORKOUT, STOPWATCH — drawn in the
// panel's own pixels (540 x 960, ui_render.h) and scaled once, the same idiom
// as DashPreview. One renderer per page, used twice: the dashboard editor's
// carousel cards (sample data, the defaults below) and the Ride tab's live
// copy of the device (the rider's data). Positions follow ui_render.cpp, which
// places text by its BASELINE; `PanelText` does the same.

/// The e-paper's white.
let panelPaper = Color(hex: 0xF7F5EF)

/// What the device's status bar shows.
struct PanelStatus: Equatable {
    var clock = "14:25"
    var satellites = 11
    var battery = 76
    var linked = true
    var sensors = "· HR · PWR"
    static let sample = PanelStatus()
}

/// The firmware's faces, at the sizes their ascenders give: Arial_L/B for
/// text, Impact_T/H for numbers. Drawn with the bundled Barlow / Barlow
/// Condensed, the app's stand-ins for Arial and Impact.
enum PanelFont {
    case arialL, arialB, impactT, impactH

    var name: String {
        switch self {
        case .arialL: return "Barlow-Medium"
        case .arialB: return "Barlow-Bold"
        case .impactT, .impactH: return "BarlowCondensed-Bold"
        }
    }
    /// Point size whose cap height matches the device face's ascender.
    var size: CGFloat {
        switch self {
        case .arialL: return 20
        case .arialB: return 27
        case .impactT: return 54
        case .impactH: return 160
        }
    }
    func ui(_ k: CGFloat) -> UIFont {
        UIFont(name: name, size: size * k) ?? .systemFont(ofSize: size * k, weight: .bold)
    }
    func font(_ k: CGFloat) -> Font { .custom(name, fixedSize: size * k) }
}

/// Text at a device-pixel baseline, aligned like epd text: left from `x`,
/// right to `x`, or centred on `x`.
struct PanelText: View {
    enum Align { case left, right, center }
    let text: String
    let font: PanelFont
    let x: CGFloat
    let baseline: CGFloat
    var align: Align = .left
    var color: Color = .black
    /// Truncate (shrink) to this width, as the firmware's truncToWidth does;
    /// only for `.center`.
    var maxWidth: CGFloat? = nil
    let k: CGFloat

    var body: some View {
        let top = baseline * k - font.ui(k).ascender
        let t = Text(text).font(font.font(k)).foregroundStyle(color).lineLimit(1).fixedSize()
        if let maxWidth, align == .center {
            Text(text).font(font.font(k)).foregroundStyle(color)
                .lineLimit(1).minimumScaleFactor(0.6).truncationMode(.tail)
                .frame(width: maxWidth * k)
                .offset(x: (x - maxWidth / 2) * k, y: top)
        } else {
            aligned(t, top: top)
        }
    }

    @ViewBuilder private func aligned(_ t: some View, top: CGFloat) -> some View {
        switch align {
        case .left:
            t.offset(x: x * k, y: top)
        case .right:
            t.frame(width: x * k, alignment: .trailing).offset(y: top)
        case .center:
            t.frame(width: 2 * x * k, alignment: .center).offset(y: top)
        }
    }
}

/// A device screen: the status bar and its rule on top of `content`, which
/// draws in panel pixels with scale `k`.
struct DevicePanel<Content: View>: View {
    var status: PanelStatus = .sample
    var title: String? = nil
    @ViewBuilder var content: (CGFloat) -> Content

    var body: some View {
        GeometryReader { geo in
            let k = geo.size.width / 540
            ZStack(alignment: .topLeading) {
                content(k)
                DeviceStatusBar(clock: status.clock, satellites: status.satellites,
                                battery: status.battery, sensors: status.sensors,
                                title: title, phoneLinked: status.linked, k: k)
                    .frame(width: 540 * k, height: 61 * k)
                Rectangle().fill(.black).frame(width: 540 * k, height: 3 * k).offset(y: 61 * k)
            }
            .frame(width: geo.size.width, height: 960 * k, alignment: .topLeading)
            .clipped()
        }
        .aspectRatio(540 / 960, contentMode: .fit)
        .background(panelPaper)
        .foregroundStyle(.black)
    }
}

/// An outlined device rectangle (epd_draw_rect, `t` pixels thick).
private func panelRect(_ x: CGFloat, _ y: CGFloat, _ w: CGFloat, _ h: CGFloat,
                       k: CGFloat, line t: CGFloat = 3, fill: Color = .clear) -> some View {
    Rectangle().fill(fill)
        .overlay(Rectangle().strokeBorder(.black, lineWidth: max(1, t * k)))
        .frame(width: w * k, height: h * k)
        .offset(x: x * k, y: y * k)
}

private func panelFill(_ x: CGFloat, _ y: CGFloat, _ w: CGFloat, _ h: CGFloat,
                       k: CGFloat, _ color: Color = .black) -> some View {
    Rectangle().fill(color).frame(width: w * k, height: h * k).offset(x: x * k, y: y * k)
}

// MARK: - Sample text shared by the cards

func previewPanelLabel(_ id: String) -> String {
    id == "power3s" ? "POWER · 3S" : (DashField.named(id)?.label ?? id).uppercased()
}

func previewSample(_ id: String) -> String {
    switch id {
    case "speed": return "32.4"
    case "power3s", "power": return "247"
    case "hr": return "156"
    case "cadence": return "88"
    case "distance": return "54.8"
    case "ridetime", "movingtime": return "1:47"
    case "climb": return "918"
    case "grade": return "4.2"
    case "altitude": return "112"
    case "battery": return "76"
    case "sats": return "11"
    case "clock": return "14:25"
    case "routeleft": return "12.4"
    default: return "--"
    }
}

// MARK: - MAP

/// The device's MAP page: the map band, its zoom stack, and the configured
/// 3-field data strip. `map` is what fills the band — the Ride tab passes the
/// live map; the editor card keeps a drawn street grid.
struct MapPagePreview: View {
    var fields: [String] = ["speed", "distance", "ridetime"]
    var status: PanelStatus = .sample
    var value: (String) -> String = previewSample
    var map: AnyView? = nil

    var body: some View {
        DevicePanel(status: status) { k in
            ZStack(alignment: .topLeading) {
                Group {
                    if let map {
                        map.frame(width: 540 * k, height: (810 - 64) * k)
                            .offset(y: 64 * k)
                    } else {
                        sampleGrid(k)
                            .mask(Rectangle().frame(width: 540 * k, height: (810 - 64) * k)
                                .offset(y: 64 * k))
                    }
                }
                // Zoom stack, over the map like the device's.
                ForEach(0..<2, id: \.self) { v in
                    ZStack {
                        Rectangle().fill(panelPaper)
                        Rectangle().strokeBorder(.black, lineWidth: max(1, 3 * k))
                        Image(systemName: v == 0 ? "plus" : "minus")
                            .font(.system(size: 34 * k, weight: .bold))
                    }
                    .frame(width: 76 * k, height: 76 * k)
                    .offset(x: 462 * k, y: (560 + CGFloat(v) * 80) * k)
                }

                // Data strip: the CONFIGURED fields, drawn like dash cells.
                panelFill(0, 810, 540, 150, k: k, panelPaper)
                panelFill(0, 810, 540, 3, k: k)
                ForEach(0..<3, id: \.self) { c in
                    let id = c < fields.count ? fields[c] : "speed"
                    ZStack(alignment: .topLeading) {
                        Rectangle().strokeBorder(.black, lineWidth: max(1, 2 * k))
                        Text(previewPanelLabel(id))
                            .font(.custom("Barlow-SemiBold", fixedSize: 15 * k))
                            .kerning(2 * k)
                            .lineLimit(1).minimumScaleFactor(0.5)
                            .frame(width: 130 * k, alignment: .leading)
                            .offset(x: 14 * k, y: 12 * k)
                        Text(value(id))
                            .font(.custom("BarlowCondensed-Bold", fixedSize: 52 * k))
                            .lineLimit(1).minimumScaleFactor(0.5)
                            .frame(width: 156 * k, height: 123 * k)
                    }
                    .frame(width: 156 * k, height: 123 * k, alignment: .topLeading)
                    .offset(x: (24 + CGFloat(c) * 168) * k, y: 825 * k)
                }
            }
        }
    }

    /// The editor card's stand-in map: a rotated street grid and a position.
    private func sampleGrid(_ k: CGFloat) -> some View {
        ZStack(alignment: .topLeading) {
            ForEach(0..<4, id: \.self) { i in
                Rectangle().fill(.black.opacity(i == 1 ? 0.9 : 0.35))
                    .frame(width: 700 * k, height: (i == 1 ? 7 : 3) * k)
                    .rotationEffect(.degrees(18))
                    .offset(x: -80 * k, y: (150 + CGFloat(i) * 150) * k)
            }
            ForEach(0..<3, id: \.self) { i in
                Rectangle().fill(.black.opacity(0.35))
                    .frame(width: 3 * k, height: 700 * k)
                    .rotationEffect(.degrees(18))
                    .offset(x: (120 + CGFloat(i) * 160) * k, y: 80 * k)
            }
            Circle().fill(.black)
                .frame(width: 26 * k, height: 26 * k)
                .offset(x: 257 * k, y: 420 * k)
            Circle().stroke(.black, lineWidth: max(1, 2 * k))
                .frame(width: 44 * k, height: 44 * k)
                .offset(x: 248 * k, y: 411 * k)
        }
    }
}

// MARK: - MUSIC

/// The device's MUSIC page (ui_render_music): art (or a record when there is
/// none), title / artist / album, progress, transport and volume.
struct MusicPreview: View {
    var status: PanelStatus = .sample
    /// nil with `live` set = NOTHING PLAYING; the card shows a sample track.
    var track: MediaTrack? = MediaTrack(title: "Turn Your Lights Down Low",
                                        artist: "Bob Marley & The Wailers", album: "Exodus")
    /// The cover as the device draws it (pre-dithered to its five tones).
    var art: UIImage? = nil
    var playing = true
    /// Position / length, when known. The AMS feed the device gets does not
    /// reach the app, so the live page passes nil and the bar stays empty.
    var progress: (pos: Int, dur: Int)? = (154, 331)

    var body: some View {
        DevicePanel(status: status, title: "MUSIC") { k in
            ZStack(alignment: .topLeading) {
                // Album art frame: the cover, or a record when there is none.
                panelRect(108, 96, 324, 324, k: k, line: 2)
                if let art {
                    Image(uiImage: art).resizable().interpolation(.none)
                        .frame(width: 300 * k, height: 300 * k)
                        .offset(x: 120 * k, y: 108 * k)
                } else {
                    Circle().stroke(.black, lineWidth: max(1, 2 * k))
                        .frame(width: 208 * k, height: 208 * k)
                        .offset(x: (270 - 104) * k, y: (258 - 104) * k)
                    Circle().fill(.black)
                        .frame(width: 68 * k, height: 68 * k)
                        .offset(x: (270 - 34) * k, y: (258 - 34) * k)
                    Circle().fill(panelPaper)
                        .frame(width: 24 * k, height: 24 * k)
                        .offset(x: (270 - 12) * k, y: (258 - 12) * k)
                }

                // Volume, zoom-button style.
                PanelText(text: "VOL", font: .arialL, x: 500, baseline: 128, align: .center, k: k)
                ForEach(0..<2, id: \.self) { v in
                    ZStack {
                        Rectangle().strokeBorder(.black, lineWidth: max(1, 3 * k))
                        Image(systemName: v == 0 ? "plus" : "minus")
                            .font(.system(size: 34 * k, weight: .bold))
                    }
                    .frame(width: 76 * k, height: 76 * k)
                    .offset(x: 462 * k, y: (140 + CGFloat(v) * 92) * k)
                }

                if let t = track {
                    PanelText(text: (t.title.isEmpty ? "UNTITLED" : t.title).uppercased(),
                              font: .impactT, x: 270, baseline: 508, align: .center, maxWidth: 492, k: k)
                    PanelText(text: t.artist, font: .arialB, x: 270, baseline: 556, align: .center,
                              maxWidth: 492, k: k)
                    PanelText(text: t.album, font: .arialL, x: 270, baseline: 592, align: .center,
                              color: .black.opacity(0.6), maxWidth: 492, k: k)
                } else {
                    PanelText(text: "NOTHING PLAYING", font: .impactT, x: 270, baseline: 508,
                              align: .center, k: k)
                    PanelText(text: "Start playback on your phone", font: .arialB, x: 270,
                              baseline: 556, align: .center, color: .black.opacity(0.6), k: k)
                }

                // Progress + times.
                panelRect(24, 640, 492, 14, k: k, line: 1)
                if let p = progress, track != nil, p.dur > 0 {
                    panelFill(26, 642, 488 * CGFloat(min(p.pos, p.dur)) / CGFloat(p.dur), 10, k: k)
                    PanelText(text: Self.mmss(p.pos), font: .arialL, x: 24, baseline: 690,
                              color: .black.opacity(0.6), k: k)
                    PanelText(text: Self.mmss(p.dur), font: .arialL, x: 516, baseline: 690,
                              align: .right, color: .black.opacity(0.6), k: k)
                }

                // Transport.
                ForEach(0..<3, id: \.self) { b in
                    ZStack {
                        Rectangle().strokeBorder(.black, lineWidth: max(1, 2 * k))
                        Image(systemName: ["backward.end.fill", playing ? "pause.fill" : "play.fill",
                                           "forward.end.fill"][b])
                            .font(.system(size: 40 * k))
                    }
                    .frame(width: [140.0, 188.0, 140.0][b] * k, height: 120 * k)
                    .offset(x: [24.0, 176.0, 376.0][b] * k, y: 780 * k)
                }
            }
        }
    }

    static func mmss(_ s: Int) -> String { String(format: "%d:%02d", s / 60, s % 60) }
}

extension ArtDither {
    /// 8-bit gray -> the device's five tones -> an image, so the phone shows
    /// the cover exactly as the panel draws it.
    static func panelImage(gray: [UInt8], side: Int) -> UIImage? {
        guard gray.count == side * side else { return nil }
        let tones = dither(gray, width: side, height: side)
        var px = tones.map { UInt8(lum[Int($0)]) }
        return px.withUnsafeMutableBytes { buf -> UIImage? in
            guard let ctx = CGContext(data: buf.baseAddress, width: side, height: side,
                                      bitsPerComponent: 8, bytesPerRow: side,
                                      space: CGColorSpaceCreateDeviceGray(),
                                      bitmapInfo: CGImageAlphaInfo.none.rawValue),
                  let cg = ctx.makeImage() else { return nil }
            return UIImage(cgImage: cg)
        }
    }
}

// MARK: - STOPWATCH

/// The device's STOPWATCH page (ui_render_stopwatch), no laps: the time across
/// the middle, what it is doing, and START/PAUSE · RESET · LAP.
struct StopwatchPagePreview: View {
    var status: PanelStatus = .sample
    var time = "12:34"
    var state = "RUNNING"
    var running = true
    /// A line under the state, for the Ride tab to say why it shows "--".
    var note: String? = nil

    var body: some View {
        DevicePanel(status: status, title: "STOPWATCH") { k in
            ZStack(alignment: .topLeading) {
                Text(time).font(.custom("BarlowCondensed-Bold", fixedSize: 190 * k))
                    .lineLimit(1).minimumScaleFactor(0.5)
                    .frame(width: 540 * k)
                    .offset(y: 250 * k)
                PanelText(text: state, font: .arialB, x: 270, baseline: 530, align: .center, k: k)
                if let note {
                    PanelText(text: note, font: .arialL, x: 270, baseline: 570, align: .center,
                              color: .black.opacity(0.6), k: k)
                }
                panelFill(0, 756, 540, 3, k: k)
                ForEach(0..<3, id: \.self) { i in
                    let xs: [CGFloat] = [24, 240, 384]
                    let ws: [CGFloat] = [204, 132, 132]
                    ZStack {
                        Rectangle().strokeBorder(.black, lineWidth: max(1, (i == 1 && running ? 1 : 3) * k))
                        Text([running ? "PAUSE" : "START", "RESET", "LAP"][i])
                            .font(.custom("BarlowCondensed-Bold", fixedSize: 30 * k))
                            .foregroundStyle(.black.opacity(i == 1 && running ? 0.35 : 1))
                    }
                    .frame(width: ws[i] * k, height: 156 * k)
                    .offset(x: xs[i] * k, y: 780 * k)
                }
            }
        }
    }
}

// MARK: - WORKOUT

/// One workout as the device's WORKOUT page needs it.
struct WorkoutPageModel {
    struct Block: Equatable { var seconds: Int; var watts: Int }

    var loaded = true
    var name = "Sweet Spot 3x10.erg"
    var blockIdx = 2
    var blockCount = 7
    var running = true
    var paused = false
    var done = false
    var elapsed = 23 * 60 + 48
    var total = 60 * 60
    var targetW = 230
    var ftpW = 250
    /// The rider's 3 s power, nil without a power meter.
    var power: Int? = 238
    /// The file's blocks; empty until the file has been read.
    var blocks: [Block] = [
        .init(seconds: 600, watts: 140), .init(seconds: 300, watts: 175),
        .init(seconds: 600, watts: 230), .init(seconds: 300, watts: 150),
        .init(seconds: 600, watts: 230), .init(seconds: 300, watts: 150),
        .init(seconds: 900, watts: 130),
    ]
    /// The device's /workouts, shown as its picker when nothing is loaded.
    var picker: [String] = []

    static let sample = WorkoutPageModel()

    /// The block being run, if the file's blocks line up with the status.
    var current: Block? { blocks.count == blockCount && blocks.indices.contains(blockIdx) ? blocks[blockIdx] : nil }
    var blockStart: Int { blocks.prefix(blockIdx).reduce(0) { $0 + $1.seconds } }
    /// Seconds left in the block: the session clock against the block's end.
    var blockRemaining: Int? {
        guard let c = current else { return nil }
        return max(0, min(c.seconds, blockStart + c.seconds - elapsed))
    }

    static func zone(_ watts: Int, ftp: Int) -> Int {
        guard ftp > 0 else { return 0 }
        let pct = watts * 100 / ftp
        return pct <= 55 ? 1 : pct <= 75 ? 2 : pct <= 90 ? 3 : pct <= 105 ? 4
            : pct <= 120 ? 5 : pct <= 150 ? 6 : 7
    }
    static let zoneNames = ["RECOVERY", "ENDURANCE", "TEMPO", "THRESHOLD",
                            "VO2 MAX", "ANAEROBIC", "NEUROMUSC"]
    /// workoutBlockTitle(): "Z4" and "THRESHOLD".
    func title(_ b: Block) -> (zone: String, name: String) {
        let z = Self.zone(b.watts, ftp: ftpW)
        return (z > 0 ? "Z\(z)" : "--", z > 0 ? Self.zoneNames[z - 1] : "BLOCK")
    }

    /// The blocks of an .erg (minutes, watts) or .mrc (minutes, %FTP) file,
    /// as the device reads them: one block between each pair of points, at
    /// the average of its ends — a ramp shows as its middle, like workoutBlockWatts.
    static func blocks(fromFile text: String, ftp: Int) -> [Block] {
        var percent = false, inData = false
        var pts: [(Double, Double)] = []
        for raw in text.split(separator: "\n", omittingEmptySubsequences: false) {
            let line = raw.trimmingCharacters(in: .whitespaces)
            let upper = line.uppercased()
            if !inData {
                if upper.contains("[COURSE DATA]") { inData = true }
                else if upper.contains("PERCENT") { percent = true }
            } else if upper.contains("[END COURSE DATA]") {
                break
            } else {
                let tok = line.split(whereSeparator: { $0 == " " || $0 == "\t" })
                if tok.count >= 2, let m = Double(tok[0]), let v = Double(tok[1]) { pts.append((m, v)) }
            }
        }
        guard pts.count >= 2 else { return [] }
        var out: [Block] = []
        for i in 1..<pts.count {
            let dur = (pts[i].0 - pts[i - 1].0) * 60
            guard dur >= 1 else { continue }
            let avg = (pts[i - 1].1 + pts[i].1) / 2
            let watts = percent ? avg * Double(max(ftp, 1)) / 100 : avg
            out.append(Block(seconds: Int(dur.rounded()), watts: Int(watts.rounded())))
        }
        return out
    }

    static func mmss(_ s: Int) -> String { String(format: "%d:%02d", s / 60, s % 60) }
}

/// The device's WORKOUT page: ui_render_workout when a workout is loaded, the
/// workout picker (ui_render_workout_picker) when not.
struct WorkoutPagePreview: View {
    var status: PanelStatus = .sample
    var model: WorkoutPageModel = .sample
    /// Tapping a picker row loads that workout, as a tap on the device does.
    var onPick: ((String) -> Void)? = nil

    var body: some View {
        DevicePanel(status: status) { k in
            if model.loaded { loaded(k) } else { picker(k) }
        }
    }

    // MARK: loaded

    private func loaded(_ k: CGFloat) -> some View {
        let m = model
        let tol = 8
        let diff = m.power.map { $0 - m.targetW } ?? 0
        let dir = m.power == nil ? "NO POWER" : diff < -tol ? "GO HARDER" : diff > tol ? "EASE OFF" : "IN RANGE"
        let cur = m.current
        let t = cur.map(m.title)
        let remain = m.blockRemaining
        return ZStack(alignment: .topLeading) {
            // Header: what session, where in it.
            PanelText(text: m.name, font: .arialL, x: 24, baseline: 92, k: k)
            PanelText(text: "BLOCK \(m.blockIdx + 1) OF \(m.blockCount)", font: .arialB, x: 24, baseline: 126, k: k)
            panelFill(0, 140, 540, 3, k: k)

            // The dark block: power, correction, target, the block's countdown.
            panelFill(0, 143, 540, 381, k: k)
            PanelText(text: "YOUR POWER · 3S", font: .arialL, x: 24, baseline: 188, color: .white, k: k)
            PanelText(text: dir, font: .arialL, x: 516, baseline: 188, align: .right, color: .white, k: k)
            HStack(alignment: .lastTextBaseline, spacing: 10 * k) {
                Text(m.power.map(String.init) ?? "--").font(PanelFont.impactH.font(k))
                Text("W").font(PanelFont.arialB.font(k))
            }
            .foregroundStyle(.white).fixedSize()
            .offset(x: 24 * k, y: (342 * k) - PanelFont.impactH.ui(k).ascender)
            if m.power != nil {
                PanelText(text: "\(diff < -tol || diff > tol ? abs(diff) : 0)", font: .impactH,
                          x: 462, baseline: 342, align: .right, color: .white, k: k)
                arrow(diff: diff, tol: tol, k: k)
            }
            PanelText(text: "TARGET \(max(0, m.targetW - tol))-\(m.targetW + tol) W · \(m.ftpW > 0 ? m.targetW * 100 / m.ftpW : 0)% FTP",
                      font: .arialL, x: 24, baseline: 386, color: .white, k: k)
            if let cur, let t {
                PanelText(text: "\(t.zone) \(t.name) · \(WorkoutPageModel.mmss(cur.seconds)) BLOCK",
                          font: .arialL, x: 24, baseline: 412, color: .white, k: k)
            }
            Rectangle().strokeBorder(.white, lineWidth: max(1, 2 * k))
                .frame(width: 300 * k, height: 20 * k).offset(x: 24 * k, y: 452 * k)
            if let cur, let remain, cur.seconds > 0 {
                panelFill(26, 454, 296 * CGFloat(cur.seconds - remain) / CGFloat(cur.seconds), 16, k: k, .white)
            }
            PanelText(text: m.done ? "DONE" : m.paused ? "PAUSED" : m.running ? "LEFT" : "READY",
                      font: .arialL, x: 516, baseline: 434, align: .right, color: .white, k: k)
            PanelText(text: remain.map(WorkoutPageModel.mmss) ?? "--:--", font: .impactT,
                      x: 516, baseline: 496, align: .right, color: .white, k: k)

            // Session profile: width is duration, height intensity; done
            // hatched, now solid, upcoming dotted.
            PanelText(text: "SESSION PROFILE", font: .arialL, x: 24, baseline: 562, k: k)
            profile(k)
            PanelText(text: "0:00", font: .arialL, x: 24, baseline: 726, k: k)
            PanelText(text: WorkoutPageModel.mmss(m.total), font: .arialL, x: 516, baseline: 726, align: .right, k: k)
            panelFill(0, 744, 540, 3, k: k)

            strips(k)

            // Footer: pause, and the whole session as a list.
            panelRect(24, 868, 240, 80, k: k)
            panelRect(276, 868, 240, 80, k: k)
            PanelText(text: m.running ? "PAUSE BLOCK" : "START BLOCK", font: .arialB, x: 144, baseline: 914, align: .center, k: k)
            PanelText(text: "ALL BLOCKS", font: .arialB, x: 396, baseline: 914, align: .center, k: k)
        }
    }

    @ViewBuilder private func arrow(diff: Int, tol: Int, k: CGFloat) -> some View {
        let ax: CGFloat = 494, ay: CGFloat = 300
        if diff < -tol {
            Path { p in
                p.move(to: CGPoint(x: ax * k, y: (ay - 26) * k))
                p.addLine(to: CGPoint(x: (ax - 22) * k, y: (ay + 16) * k))
                p.addLine(to: CGPoint(x: (ax + 22) * k, y: (ay + 16) * k))
            }.fill(.white)
        } else if diff > tol {
            Path { p in
                p.move(to: CGPoint(x: ax * k, y: (ay + 26) * k))
                p.addLine(to: CGPoint(x: (ax - 22) * k, y: (ay - 16) * k))
                p.addLine(to: CGPoint(x: (ax + 22) * k, y: (ay - 16) * k))
            }.fill(.white)
        } else {
            panelFill(ax - 20, ay - 14, 40, 9, k: k, .white)
            panelFill(ax - 20, ay + 7, 40, 9, k: k, .white)
        }
    }

    private func profile(_ k: CGFloat) -> some View {
        let m = model
        let cx0: CGFloat = 24, cw: CGFloat = 492, cbase: CGFloat = 706, ctop: CGFloat = 574
        let blocks = m.blocks
        let total = max(1, blocks.reduce(0) { $0 + $1.seconds })
        let maxW = max(1, blocks.map(\.watts).max() ?? 1)
        let gaps = CGFloat(2 * max(0, blocks.count - 1))
        struct Bar: Identifiable { let x, w, h: CGFloat; let id: Int }
        var bars: [Bar] = []
        var x = cx0
        for (i, b) in blocks.enumerated() {
            var bw = (cw - gaps) * CGFloat(b.seconds) / CGFloat(total)
            if i == blocks.count - 1 { bw = cx0 + cw - x }
            bw = max(4, bw)
            let bh = 24 + 100 * CGFloat(b.watts) / CGFloat(maxW)
            bars.append(Bar(x: x, w: bw, h: bh, id: i))
            x += bw + 2
        }
        return ZStack(alignment: .topLeading) {
            if blocks.isEmpty {
                PanelText(text: "Reading the workout file…", font: .arialL, x: 270, baseline: 650,
                          align: .center, color: .black.opacity(0.6), k: k)
            }
            ForEach(bars) { bar in
                ZStack {
                    if bar.id < m.blockIdx {
                        Hatch(spacing: 6 * k).stroke(.black, lineWidth: max(0.5, 1.2 * k))
                    } else if bar.id == m.blockIdx {
                        Rectangle().fill(.black)
                    } else {
                        Rectangle().fill(.black.opacity(0.15))
                    }
                    Rectangle().strokeBorder(.black, lineWidth: max(1, (bar.id == m.blockIdx ? 2 : 1) * k))
                }
                .clipped()
                .frame(width: bar.w * k, height: bar.h * k)
                .offset(x: bar.x * k, y: (cbase - bar.h) * k)
            }
            panelFill(cx0, cbase - 3, cw, 3, k: k)
            if m.total > 0 {
                let mx = min(cx0 + cw * CGFloat(m.elapsed) / CGFloat(m.total), cx0 + cw - 3)
                panelFill(mx, ctop - 6, 3, cbase - ctop + 6, k: k)
            }
        }
    }

    /// Redo-previous and start-next strips.
    private func strips(_ k: CGFloat) -> some View {
        let m = model
        let ready = !m.running && !m.paused && !m.done && m.elapsed == 0
        return ZStack(alignment: .topLeading) {
            panelRect(24, 754, 240, 104, k: k, line: 2,
                      fill: .clear)
            if ready {
                PanelText(text: "< BACK", font: .arialL, x: 38, baseline: 780, k: k)
                PanelText(text: "ALL WORKOUTS", font: .arialB, x: 38, baseline: 812, k: k)
                PanelText(text: "PICK ANOTHER", font: .arialL, x: 38, baseline: 846, color: .black.opacity(0.6), k: k)
            } else {
                Hatch(spacing: 6 * k).stroke(.black, lineWidth: max(0.5, 1.2 * k))
                    .frame(width: 236 * k, height: 100 * k).clipped()
                    .offset(x: 26 * k, y: 756 * k)
                if m.blockIdx > 0, m.blocks.count == m.blockCount {
                    let pb = m.blocks[m.blockIdx - 1]
                    let t = m.title(pb)
                    // Bottom line first, so each chip above paints over the
                    // one below rather than under it.
                    chip("\(pb.watts) W", .impactT, 38, 848, k)
                    chip(t.name, .arialB, 38, 812, k)
                    chip("< REDO · \(t.zone)", .arialL, 38, 780, k)
                } else {
                    chip("< REDO", .arialL, 38, 780, k)
                    chip(m.blockIdx > 0 ? "PREVIOUS" : "SESSION START", .arialB, 38, 812, k)
                }
            }
            panelRect(276, 754, 240, 104, k: k, line: 2)
            if m.blockIdx + 1 < m.blockCount {
                if m.blocks.count == m.blockCount {
                    let nb = m.blocks[m.blockIdx + 1]
                    let t = m.title(nb)
                    PanelText(text: "START NEXT · \(t.zone) >", font: .arialL, x: 290, baseline: 780, k: k)
                    PanelText(text: "\(t.name) · \(WorkoutPageModel.mmss(nb.seconds))", font: .arialB,
                              x: 290, baseline: 812, k: k)
                        .frame(width: 506 * k, alignment: .leading).clipped()
                    PanelText(text: "\(nb.watts) W", font: .impactT, x: 290, baseline: 848, k: k)
                } else {
                    PanelText(text: "START NEXT >", font: .arialL, x: 290, baseline: 780, k: k)
                }
            } else {
                PanelText(text: "NEXT >", font: .arialL, x: 290, baseline: 780, k: k)
                PanelText(text: "SESSION COMPLETE", font: .arialB, x: 290, baseline: 812, k: k)
            }
        }
    }

    /// White chip under text on the hatched strip, so it stays readable.
    private func chip(_ s: String, _ f: PanelFont, _ x: CGFloat, _ baseline: CGFloat, _ k: CGFloat) -> some View {
        let ui = f.ui(k)
        // The chip covers the glyphs (cap height and descender) only, not the
        // font's line box, so stacked lines don't paint over each other.
        return Text(s).font(f.font(k)).lineLimit(1).fixedSize()
            .padding(.horizontal, 3 * k)
            .background(alignment: .bottom) {
                panelPaper.frame(height: ui.capHeight + 6 * k)
                    .offset(y: -(abs(ui.descender) - 3 * k))
            }
            .offset(x: (x - 3) * k, y: baseline * k - ui.ascender)
    }

    // MARK: picker

    private func picker(_ k: CGFloat) -> some View {
        let list = model.picker
        let rowTop: CGFloat = 143, rowH: CGFloat = 88
        return ZStack(alignment: .topLeading) {
            if list.isEmpty {
                PanelText(text: "NO WORKOUTS ON THE CARD", font: .impactT, x: 270, baseline: 400, align: .center, k: k)
                PanelText(text: "Add them from the app.", font: .arialB, x: 270, baseline: 456,
                          align: .center, color: .black.opacity(0.6), k: k)
                PanelText(text: "(.erg / .mrc files in /workouts)", font: .arialB, x: 270, baseline: 492,
                          align: .center, color: .black.opacity(0.6), k: k)
            } else {
                PanelText(text: "TAP A WORKOUT TO LOAD IT", font: .arialL, x: 24, baseline: 92, k: k)
                PanelText(text: "\(list.count) WORKOUT\(list.count == 1 ? "" : "S")", font: .impactT,
                          x: 24, baseline: 134, k: k)
                panelFill(0, 140, 540, 3, k: k)
                ForEach(Array(list.prefix(8).enumerated()), id: \.offset) { r, name in
                    let top = rowTop + CGFloat(r) * rowH
                    Button { onPick?(name) } label: {
                        ZStack(alignment: .topLeading) {
                            Rectangle().fill(panelPaper)
                            PanelText(text: Self.title(name), font: .arialB, x: 24, baseline: 34, k: k)
                            PanelText(text: name, font: .arialL, x: 24, baseline: 70,
                                      color: .black.opacity(0.6), k: k)
                            panelFill(0, rowH - 1, 540, 1, k: k)
                        }
                        .frame(width: 540 * k, height: rowH * k, alignment: .topLeading)
                    }
                    .buttonStyle(.plain)
                    .disabled(onPick == nil)
                    .accessibilityLabel("Load workout \(Self.title(name))")
                    .offset(y: top * k)
                }
            }
            // Footer: REFRESH, like ui_render_pager's.
            panelFill(0, 856, 540, 3, k: k)
            panelRect(24, 872, 492, 72, k: k)
            PanelText(text: "REFRESH", font: .arialB, x: 270, baseline: 918, align: .center, k: k)
        }
    }

    /// "sweet_spot_3x10.erg" -> "sweet spot 3x10".
    static func title(_ file: String) -> String {
        let stem = file.split(separator: ".").dropLast().joined(separator: ".")
        return (stem.isEmpty ? file : stem).replacingOccurrences(of: "_", with: " ")
    }
}
