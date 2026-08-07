import SwiftUI

// Tutorial icon art, drawn rather than placed: a single pen lays the outline
// down live, the panel's screentone develops behind it, and the whole badge
// flashes once as it settles — which is exactly the sequence the head unit goes
// through on every full refresh. It makes the first screens of the app feel like
// the device they're introducing.
//
// Drawn in a Canvas off a clock rather than with SwiftUI animation modifiers
// because the pen head has to sit at the END of the line being drawn, and that
// point only exists per-frame: `body` is not re-evaluated on animation frames,
// so an interpolated `progress` would never reach it.

/// What to draw. Each case is a set of strokes in a 100×100 space, listed in
/// pen order — the badge ring first, then the glyph.
enum SketchGlyph: Equatable {
    case location, waves, check

    func strokes(in side: CGFloat) -> Path {
        let k = side / 100
        func pt(_ x: CGFloat, _ y: CGFloat) -> CGPoint { CGPoint(x: x * k, y: y * k) }

        var path = Path()
        // The ring, clockwise from the top. First, so the badge draws itself
        // before whatever goes inside it.
        arc(&path, center: pt(50, 50), radius: 46 * k, from: -90, to: 270)

        switch self {
        case .location:
            // The location arrow, one closed outline.
            path.move(to: pt(75, 26))
            path.addLine(to: pt(26, 48))
            path.addLine(to: pt(47, 55))
            path.addLine(to: pt(54, 75))
            path.addLine(to: pt(75, 26))

        case .waves:
            // A transmitter: the dot, then each pair of waves spreading outward.
            path.addEllipse(in: CGRect(x: 44 * k, y: 44 * k, width: 12 * k, height: 12 * k))
            for r in [20.0, 32.0] as [CGFloat] {
                arc(&path, center: pt(50, 50), radius: r * k, from: 140, to: 220)
                arc(&path, center: pt(50, 50), radius: r * k, from: -40, to: 40)
            }

        case .check:
            path.move(to: pt(30, 52))
            path.addLine(to: pt(44, 67))
            path.addLine(to: pt(71, 33))
        }
        return path
    }

    /// `addArc` joins to the current point, which would trail a stray line in
    /// from wherever the pen was. Always start the arc's own subpath.
    private func arc(_ path: inout Path, center: CGPoint, radius: CGFloat,
                     from a: Double, to b: Double) {
        let start = CGPoint(x: center.x + radius * cos(a * .pi / 180),
                            y: center.y + radius * sin(a * .pi / 180))
        path.move(to: start)
        path.addArc(center: center, radius: radius,
                    startAngle: .degrees(a), endAngle: .degrees(b), clockwise: false)
    }
}

struct SketchIcon: View {
    let glyph: SketchGlyph
    var tint: Color = Palette.accent
    var side: CGFloat = 132
    /// True while this is the page on screen. Flipping it replays the drawing,
    /// so swiping back and forth re-draws instead of showing a stale result.
    var active: Bool = true

    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @State private var runStart: Date?
    @State private var runToken = 0

    private let drawTime = 1.15
    private let flashTime = 0.24
    private let developTime = 0.5
    private var total: Double { drawTime + flashTime + developTime }

    var body: some View {
        Group {
            if let runStart, !reduceMotion {
                TimelineView(.animation) { timeline in
                    Canvas { ctx, size in
                        render(&ctx, size: size,
                               elapsed: timeline.date.timeIntervalSince(runStart))
                    }
                }
            } else {
                // Settled state — also what Reduce Motion gets, with no drawing.
                Canvas { ctx, size in render(&ctx, size: size, elapsed: total) }
            }
        }
        .frame(width: side, height: side)
        .accessibilityHidden(true)
        .onAppear { if active { replay() } }
        .onChange(of: active) { _, on in if on { replay() } }
        // The connect page swaps its glyph the moment the device shows up;
        // drawing the new one is a better "it worked" than a swap would be.
        .onChange(of: glyph) { _, _ in if active { replay() } }
    }

    private func replay() {
        guard !reduceMotion else { return }
        runToken += 1
        let token = runToken
        runStart = Date()
        // Stop the timeline once it's done — otherwise it keeps ticking at 60 Hz
        // behind a finished drawing for as long as the tutorial is open.
        Task {
            try? await Task.sleep(for: .seconds(total + 0.15))
            if runToken == token { runStart = nil }
        }
    }

    private func render(_ ctx: inout GraphicsContext, size: CGSize, elapsed: Double) {
        let s = min(size.width, size.height)
        guard s > 0 else { return }
        let k = s / 100

        let pen = clamp(elapsed / drawTime)
        let after = elapsed - drawTime
        // One up-and-back pulse, like the panel's settle flash.
        let flash = (after >= 0 && after < flashTime)
            ? sin(.pi * after / flashTime) : 0
        let develop = clamp((after - flashTime) / developTime)

        let disc = Path(ellipseIn: CGRect(x: 2 * k, y: 2 * k,
                                          width: s - 4 * k, height: s - 4 * k))
        ctx.fill(disc, with: .color(Palette.surface))

        // The screentone the map uses, in miniature: a dot screen rather than a
        // flat tint, developing after the ink is down.
        if develop > 0 {
            var dots = Path()
            let pitch = 5 * k, r = 1.15 * k
            var y = pitch
            while y < s {
                var x = pitch
                while x < s {
                    dots.addEllipse(in: CGRect(x: x - r, y: y - r, width: r * 2, height: r * 2))
                    x += pitch
                }
                y += pitch
            }
            var layer = ctx
            layer.clip(to: disc)
            layer.opacity = develop
            layer.fill(dots, with: .color(tint.opacity(0.5)))
        }

        let full = glyph.strokes(in: s)
        let drawn = full.trimmedPath(from: 0, to: pen)
        let style = StrokeStyle(lineWidth: 3.4 * k, lineCap: .round, lineJoin: .round)
        ctx.stroke(drawn, with: .color(tint), style: style)

        // The pen itself, riding the end of the line.
        if pen > 0, pen < 1, let head = drawn.currentPoint {
            let r = 2.6 * k
            ctx.fill(Path(ellipseIn: CGRect(x: head.x - r, y: head.y - r,
                                            width: r * 2, height: r * 2)),
                     with: .color(tint))
        }

        // The settle flash: the badge inverts for an instant and clears.
        if flash > 0 {
            var layer = ctx
            layer.opacity = flash * 0.9
            layer.fill(disc, with: .color(Palette.ink))
            layer.stroke(full, with: .color(Palette.paper), style: style)
        }
    }

    private func clamp(_ v: Double) -> CGFloat { CGFloat(min(max(v, 0), 1)) }
}
