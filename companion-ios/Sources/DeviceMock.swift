import SwiftUI

// A working head unit, drawn on the phone.
//
// The tutorial used to introduce the device with two flat product shots. A
// photograph of a bike computer is indistinguishable from a photograph of a
// brick: nothing about it says the numbers are live, that the map moves under
// you, or that the panel is yours to rearrange. So the welcome screen now runs
// the device instead of picturing it — same body, same panel geometry, same
// fields, ticking off RideSim.
//
// The dashboard face is the editor's DashPreview with a ride plugged into it,
// not a second drawing of the same thing. There is exactly one renderer of the
// device's panel in this app and this is a client of it, which is what stops the
// tutorial's idea of the device drifting from the one the rider configures.
enum DeviceFace: Equatable {
    case dashboard      // the numbers
    case map            // riding a route

    /// The screen the OTHER device in a pair should show, so a hero always
    /// presents two different faces rather than the same one twice.
    var counterpart: DeviceFace { self == .dashboard ? .map : .dashboard }
}

/// The head unit: body, screen, status bar, and whichever face is showing.
/// `width` is the SCREEN width; the body sizes itself around it in the same
/// proportions as the product renders.
struct DeviceMock: View {
    var face: DeviceFace = .dashboard
    var width: CGFloat = 200

    /// Frozen at a fixed instant unless told to run. A still device is the right
    /// thing behind a live one — two panels both counting draws the eye to the
    /// wrong place, and the back of the pair is mostly hidden anyway.
    var live: Bool = true

    // Device geometry (ui_render.h), so everything below can be written in the
    // panel's own pixels and scaled once.
    private let panelW: CGFloat = 540, panelH: CGFloat = 960
    private let statusH: CGFloat = 64

    private var k: CGFloat { width / panelW }
    private var screenH: CGFloat { panelH * k }

    // Body proportions, measured off the renders: the case is a little wider
    // than the glass, with a tall chin for the button.
    private var bodyW: CGFloat { width * 1.115 }
    private var bodyH: CGFloat { screenH + width * 0.466 }

    var body: some View {
        // One clock for the whole device. The panel's numbers step once a
        // second because that is how often the e-paper redraws them — a value
        // smoothly interpolating would be the one part of this that the real
        // device cannot do. The map is the deliberate exception: it pans on
        // every frame, because a map stepping 1 Hz reads as a bug rather than
        // as ink.
        TimelineView(.animation(minimumInterval: live ? 1.0 / 12 : nil, paused: !live)) { ctx in
            let t = live ? ctx.date.timeIntervalSince(start) : 0
            screen(smooth: RideSim(t: t), stepped: RideSim(t: t.rounded(.down)))
                .frame(width: width, height: screenH)
                .background(Palette.paper)
                .overlay(Rectangle().strokeBorder(.black, lineWidth: 2 * k))
                .padding(.top, width * 0.221)
                .frame(width: bodyW, height: bodyH, alignment: .top)
                .background(deviceBody)
        }
    }

    @State private var start = Date()

    @ViewBuilder
    private func screen(smooth: RideSim, stepped: RideSim) -> some View {
        VStack(spacing: 0) {
            StatusBar(sim: stepped, k: k)
                .frame(height: statusH * k)
            Rectangle().fill(.black).frame(height: 2 * k)

            switch face {
            case .dashboard:
                DashPreview(layout: .tutorial, live: stepped)
            case .map:
                MapFace(smooth: smooth, stepped: stepped, k: k)
            }
        }
        .clipped()
    }

    /// The case: warm plastic, a soft edge, the sleep button on the left rail
    /// and the round confirm button under the glass.
    private var deviceBody: some View {
        ZStack {
            RoundedRectangle(cornerRadius: bodyW * 0.092, style: .continuous)
                .fill(Color(hex: 0xF4F1E8))
                .overlay(
                    RoundedRectangle(cornerRadius: bodyW * 0.092, style: .continuous)
                        .strokeBorder(Color(hex: 0xD8D3C4), lineWidth: 1))
                .shadow(color: .black.opacity(0.16), radius: bodyW * 0.05,
                        x: 0, y: bodyW * 0.02)

            RoundedRectangle(cornerRadius: bodyW * 0.02)
                .fill(Color(hex: 0xE6E1D2))
                .frame(width: bodyW * 0.022, height: bodyH * 0.075)
                .offset(x: -bodyW * 0.502, y: bodyH * 0.02)

            Circle()
                .strokeBorder(Color(hex: 0xC9C3B2), lineWidth: bodyW * 0.008)
                .frame(width: bodyW * 0.157)
                .overlay(Circle().fill(Color(hex: 0xB8B2A0)).frame(width: bodyW * 0.028))
                .offset(y: bodyH * 0.433)
        }
    }
}

/// A DeviceMock that changes screens the way the device does: e-paper cannot
/// cross-fade, it drives every pixel to black and back, so switching faces here
/// flashes once and the new screen is simply THERE afterwards. A dissolve would
/// be the prettier lie; this is the thing a rider will recognise the first time
/// they press the button on the real unit.
struct EInkDevice: View {
    let face: DeviceFace
    var width: CGFloat = 200
    var live: Bool = true

    @State private var shown: DeviceFace
    @State private var flash = false

    init(face: DeviceFace, width: CGFloat = 200, live: Bool = true) {
        self.face = face
        self.width = width
        self.live = live
        _shown = State(initialValue: face)
    }

    var body: some View {
        DeviceMock(face: shown, width: width, live: live)
            .overlay {
                // Only the glass flashes, not the case — the panel is the part
                // that refreshes.
                Rectangle()
                    .fill(.black)
                    .frame(width: width, height: width / 540 * 960)
                    .opacity(flash ? 1 : 0)
                    // The glass sits a hair above the body's centre — the chin
                    // under the screen is taller than the brow above it.
                    .offset(y: -width * 0.012)
                    .allowsHitTesting(false)
            }
            .task(id: face) {
                guard face != shown else { return }
                withAnimation(.easeIn(duration: 0.09)) { flash = true }
                try? await Task.sleep(for: .milliseconds(150))
                shown = face
                withAnimation(.easeOut(duration: 0.20)) { flash = false }
            }
    }
}

// The panel's top strip: clock, phone link, satellites, which sensors are
// paired, and the fuel gauge — the same order and the same abbreviations the
// device prints, because this is the one row a rider learns to read at a
// glance and reordering it here would teach the wrong glance.
private struct StatusBar: View {
    let sim: RideSim
    let k: CGFloat

    var body: some View {
        HStack(spacing: 9 * k) {
            Text(RideSim.hm(sim.clock))
                .font(.custom("BarlowCondensed-Bold", size: 40 * k))
            phone
            satellites
            Text("· HR · PWR")
                .font(.custom("BarlowCondensed-Bold", size: 36 * k))
            Spacer(minLength: 0)
            Text("\(sim.battery)%")
                .font(.custom("BarlowCondensed-Bold", size: 36 * k))
            battery
        }
        .foregroundStyle(.black)
        .lineLimit(1)
        .padding(.horizontal, 18 * k)
    }

    private var phone: some View {
        RoundedRectangle(cornerRadius: 3 * k)
            .strokeBorder(.black, lineWidth: 3 * k)
            .frame(width: 15 * k, height: 26 * k)
    }

    /// One dot per satellite, capped at four — the device shows lock quality,
    /// not an exact count, and four dots is what "plenty" looks like.
    private var satellites: some View {
        HStack(spacing: 4 * k) {
            ForEach(0..<4, id: \.self) { i in
                // Empty dots stay as outlines. Dropping them entirely would let
                // the row change width every time the sky opened up, and a
                // status bar that reflows is one you stop trusting to be in the
                // same place.
                Circle()
                    .strokeBorder(.black, lineWidth: 1.5 * k)
                    .background(Circle().fill(
                        i < min(4, max(0, sim.satellites - 8)) ? .black : .clear))
                    .frame(width: 9 * k, height: 9 * k)
            }
        }
    }

    private var battery: some View {
        HStack(spacing: 1.5 * k) {
            ZStack(alignment: .leading) {
                RoundedRectangle(cornerRadius: 2 * k)
                    .strokeBorder(.black, lineWidth: 3 * k)
                RoundedRectangle(cornerRadius: 1 * k)
                    .fill(.black)
                    .padding(3.5 * k)
                    .frame(width: 44 * k * CGFloat(sim.battery) / 100, alignment: .leading)
            }
            .frame(width: 44 * k, height: 22 * k)
            RoundedRectangle(cornerRadius: 1 * k)
                .fill(.black)
                .frame(width: 4 * k, height: 10 * k)
        }
    }
}

// MARK: - the map face

// The map the device draws while you ride: ink on paper, roads by class, parks
// as a diagonal hatch — the screentones of src/map_view.cpp, at the size they
// survive being shrunk to a hero.
//
// It scrolls off the ride's DISTANCE rather than a free-running animation, so
// the map and the numbers next to it are the same ride: when the speed dips,
// the ground under the arrow slows down with it.
private struct MapFace: View {
    let smooth: RideSim
    let stepped: RideSim
    let k: CGFloat

    /// Metres of ground per point, so the scale bar below is not a decoration.
    private let metresPerPoint: CGFloat = 2.6
    private let grid: CGFloat = 46          // block size, points
    private let angle: Double = -21         // the street grid is never square on

    var body: some View {
        GeometryReader { geo in
            let s = CGFloat(smooth.distance * 1000) / metresPerPoint
            ZStack {
                Canvas { ctx, size in
                    ctx.fill(Path(CGRect(origin: .zero, size: size)),
                             with: .color(Color(hex: 0xF7F5EF)))
                    drawMap(ctx, size: size, s: s)
                }
                rider(heading: heading(at: s))
                chrome(in: geo.size)
            }
            .frame(width: geo.size.width, height: geo.size.height)
        }
        .background(Color(hex: 0xF7F5EF))
        .overlay(alignment: .bottom) { strip }
    }

    // MARK: the route
    //
    // The map used to slide straight up forever, which is not what riding looks
    // like: the ground moved but the rider was never ON anything. It scrolled
    // past a road rather than along one, and no amount of speed made that read
    // as travel.
    //
    // Now there is an actual route through the grid and the rider is a point on
    // it. Everything else follows from that — the map is simply drawn with that
    // point under the arrow, so the streets slide because the rider is moving,
    // and at a junction the whole city swings round because the rider turned.

    /// The route, as legs through the block grid: five blocks up, two across,
    /// three up, two back. Each leg runs ALONG a street rather than across the
    /// middle of a block, and the pattern repeats, so an unbounded ride needs no
    /// stored polyline — just the leg you are on and how many times round.
    private var legs: [CGSize] {
        [CGSize(width: 0, height: -5 * grid), CGSize(width: 2 * grid, height: 0),
         CGSize(width: 0, height: -3 * grid), CGSize(width: -2 * grid, height: 0)]
    }
    private var lapLength: CGFloat { 12 * grid }
    private var lapShift: CGSize { CGSize(width: 0, height: -8 * grid) }

    /// Where the rider is after `s` points of road.
    private func point(at s: CGFloat) -> CGPoint {
        let lap = (s / lapLength).rounded(.down)
        var left = s - lap * lapLength
        var p = CGPoint(x: lapShift.width * lap, y: lapShift.height * lap)
        for leg in legs {
            let len = abs(leg.width) + abs(leg.height)
            if left <= len {
                let f = len == 0 ? 0 : left / len
                return CGPoint(x: p.x + leg.width * f, y: p.y + leg.height * f)
            }
            p.x += leg.width; p.y += leg.height; left -= len
        }
        return p
    }

    /// Which way the rider is pointing, measured as the chord across a few
    /// metres either side. Taking the leg's own direction would snap the arrow
    /// through ninety degrees in one frame at every corner; the chord rounds
    /// the turn over the second or so it takes to make it.
    private func heading(at s: CGFloat) -> Angle {
        let a = point(at: max(0, s - 9)), b = point(at: s + 9)
        // +90° because the arrow art points up, not along +x.
        return .radians(atan2(b.y - a.y, b.x - a.x) + .pi / 2)
    }

    // MARK: ink

    /// Draws the city with the rider's own position under the centre of the
    /// screen. Takes the context by value: this leaves a rotation and a
    /// translation on it, and anything drawn afterwards on the caller's copy
    /// would inherit both.
    private func drawMap(_ base: GraphicsContext, size: CGSize, s: CGFloat) {
        var ctx = base
        let me = point(at: s)
        ctx.translateBy(x: size.width / 2, y: size.height / 2)
        ctx.rotate(by: .degrees(angle))       // the grid is never square to north
        ctx.translateBy(x: -me.x, y: -me.y)

        // Everything within the screen's half-diagonal of the rider can end up
        // on screen once it is rotated, so that is what gets drawn.
        let reach = 0.5 * sqrt(size.width * size.width + size.height * size.height) + grid
        let c0 = Int(((me.x - reach) / grid).rounded(.down)), c1 = Int(((me.x + reach) / grid).rounded(.up))
        let r0 = Int(((me.y - reach) / grid).rounded(.down)), r1 = Int(((me.y + reach) / grid).rounded(.up))

        // Blocks first, so every line lands on top of its own edge.
        for c in c0..<c1 {
            for r in r0..<r1 where tone(c, r) != nil {
                let rect = CGRect(x: CGFloat(c) * grid, y: CGFloat(r) * grid,
                                  width: grid, height: grid).insetBy(dx: 2, dy: 2)
                hatch(ctx, rect, dense: tone(c, r) == .water)
            }
        }

        var minor = Path()
        for c in c0...c1 {
            minor.move(to: .init(x: CGFloat(c) * grid, y: CGFloat(r0) * grid))
            minor.addLine(to: .init(x: CGFloat(c) * grid, y: CGFloat(r1) * grid))
        }
        for r in r0...r1 {
            minor.move(to: .init(x: CGFloat(c0) * grid, y: CGFloat(r) * grid))
            minor.addLine(to: .init(x: CGFloat(c1) * grid, y: CGFloat(r) * grid))
        }
        ctx.stroke(minor, with: .color(.black), lineWidth: 1)

        // Arterials, drawn heavier — a grid of identical lines reads as graph
        // paper, and the device's map is legible precisely because road class
        // comes through as line weight.
        var major = Path()
        for c in c0...c1 where c % 5 == 0 {
            major.move(to: .init(x: CGFloat(c) * grid, y: CGFloat(r0) * grid))
            major.addLine(to: .init(x: CGFloat(c) * grid, y: CGFloat(r1) * grid))
        }
        for r in r0...r1 where r % 4 == 0 {
            major.move(to: .init(x: CGFloat(c0) * grid, y: CGFloat(r) * grid))
            major.addLine(to: .init(x: CGFloat(c1) * grid, y: CGFloat(r) * grid))
        }
        ctx.stroke(major, with: .color(.black), lineWidth: 3.5)

        // The loaded route, at the weight the device reserves for it. Drawn a
        // lap either side of the current one so it runs off both edges of the
        // screen rather than beginning under the rider.
        let lap = Int((s / lapLength).rounded(.down))
        var route = Path()
        for n in (lap - 1)...(lap + 2) {
            var p = CGPoint(x: lapShift.width * CGFloat(n), y: lapShift.height * CGFloat(n))
            route.move(to: p)
            for leg in legs {
                p.x += leg.width; p.y += leg.height
                route.addLine(to: p)
            }
        }
        ctx.stroke(route, with: .color(.black),
                   style: StrokeStyle(lineWidth: 7, lineCap: .round, lineJoin: .round))
    }

    private enum Tone { case park, water }

    /// Which blocks are parks or water. A hash rather than a stored map: the
    /// grid scrolls forever and the pattern has to exist wherever it gets to.
    private func tone(_ col: Int, _ row: Int) -> Tone? {
        var h = UInt32(bitPattern: Int32(truncatingIfNeeded: col &* 73_856_093 ^ row &* 19_349_663))
        h ^= h >> 13; h = h &* 2_654_435_761; h ^= h >> 16
        switch h % 23 {
        case 0, 1: return .park
        case 2:    return .water
        default:   return nil
        }
    }

    /// The screentones, drawn as line work: parks hatch at 45°, water gets the
    /// tighter screen that reads as grey from arm's length.
    ///
    /// Takes a COPY of the context: a clip applies to a GraphicsContext for the
    /// rest of its life, so clipping the shared one to the first block would
    /// erase every block after it.
    private func hatch(_ ctx: GraphicsContext, _ rect: CGRect, dense: Bool) {
        var c = ctx
        c.clip(to: Path(rect))
        let spacing: CGFloat = dense ? 3 : 6
        var p = Path()
        var x = rect.minX - rect.height
        while x < rect.maxX {
            p.move(to: .init(x: x, y: rect.maxY))
            p.addLine(to: .init(x: x + rect.height, y: rect.minY))
            x += spacing
        }
        c.stroke(p, with: .color(.black), lineWidth: dense ? 1.4 : 1)
    }

    // MARK: chrome

    /// The rider: fixed at the centre of the screen, turning to face the way the
    /// route goes. The badge stays upright — only the arrow inside it swings —
    /// because the ring is chrome and chrome does not rotate.
    private func rider(heading: Angle) -> some View {
        Circle()
            .fill(Color(hex: 0xF7F5EF))
            .overlay(Circle().strokeBorder(.black, lineWidth: 2.5 * k))
            .frame(width: 76 * k, height: 76 * k)
            .overlay {
                Path { p in
                    p.move(to: .init(x: 20 * k, y: 4 * k))
                    p.addLine(to: .init(x: 34 * k, y: 36 * k))
                    p.addLine(to: .init(x: 20 * k, y: 27 * k))
                    p.addLine(to: .init(x: 6 * k, y: 36 * k))
                    p.closeSubpath()
                }
                .fill(.black)
                .frame(width: 40 * k, height: 40 * k)
                .rotationEffect(heading + .degrees(angle))
            }
    }

    private func chrome(in size: CGSize) -> some View {
        ZStack {
            // North arrow, top right.
            Circle()
                .fill(Color(hex: 0xF7F5EF))
                .overlay(Circle().strokeBorder(.black, lineWidth: 2.5 * k))
                .frame(width: 58 * k, height: 58 * k)
                .overlay {
                    Path { p in
                        p.move(to: .init(x: 11 * k, y: 2 * k))
                        p.addLine(to: .init(x: 21 * k, y: 20 * k))
                        p.addLine(to: .init(x: 1 * k, y: 20 * k))
                        p.closeSubpath()
                    }
                    .fill(.black)
                    .frame(width: 22 * k, height: 22 * k)
                }
                .position(x: size.width - 42 * k, y: 42 * k)

            // The scale bar, and it means it: 200 m of real ground.
            VStack(alignment: .leading, spacing: 1 * k) {
                Text("200 M")
                    .font(.custom("BarlowCondensed-Bold", size: 34 * k))
                    .foregroundStyle(.black)
                Rectangle().fill(.black)
                    .frame(width: 200 / metresPerPoint, height: 3 * k)
            }
            .position(x: 22 * k + 100 / metresPerPoint, y: size.height - 34 * k)
        }
    }

    /// The strip the map face keeps below it, so the numbers never disappear
    /// just because you are looking at where you are.
    private var strip: some View {
        HStack(spacing: 0) {
            stripCell("SPEED", stepped.text(for: "speed"))
            stripCell("DIST", stepped.text(for: "distance"))
            stripCell("TIME", RideSim.hm(stepped.elapsed))
        }
        .frame(height: 150 * k)
        .background(Color(hex: 0xF7F5EF))
        .overlay(alignment: .top) { Rectangle().fill(.black).frame(height: 2 * k) }
    }

    private func stripCell(_ label: String, _ value: String) -> some View {
        VStack(spacing: 0) {
            Text(label)
                .font(.custom("Barlow-SemiBold", size: 26 * k))
                .tracking(2 * k)
            Text(value)
                .font(.custom("BarlowCondensed-Bold", size: 76 * k))
                .lineLimit(1)
                .minimumScaleFactor(0.5)
        }
        .foregroundStyle(.black)
        .padding(.vertical, 8 * k)
        .frame(maxWidth: .infinity)
        .overlay(alignment: .trailing) {
            Rectangle().fill(.black).frame(width: 2 * k)
        }
    }

}

extension DashLayout {
    /// The layout the tutorial's device arrives with — power up top with its
    /// zone bar, then the four numbers the product shots show. It is the
    /// device's own default with speed in place of cadence, because speed is
    /// the field a newcomer looks for first and the one whose movement is
    /// obvious without knowing what a good number is.
    static var tutorial: DashLayout {
        DashLayout(items: [
            DashItem(field: "power3s",  size: .hero,   half: false),
            DashItem(field: "hr",       size: .medium, half: true),
            DashItem(field: "speed",    size: .medium, half: true),
            DashItem(field: "ridetime", size: .medium, half: true),
            DashItem(field: "distance", size: .medium, half: true),
        ])
    }
}
