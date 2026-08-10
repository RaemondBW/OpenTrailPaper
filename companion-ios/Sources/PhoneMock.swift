import SwiftUI

// The phone, running this app — for the tutorial screen that explains what the
// APP is for.
//
// That screen used to show the head unit, which quietly said the wrong thing:
// planning a route, baking tiles and reading rides back are all things you do
// here, on the phone in your hand, and illustrating them with the bike computer
// put the work on the wrong device. So the three jobs are now demonstrated on a
// phone, and the head unit stays on the welcome screen where it belongs.
//
// Drawn rather than screenshotted, for the same reason SketchIcon is drawn: a
// screenshot goes stale the first time a colour or a corner radius changes,
// while these are built from the same Palette and TypeScale as the real
// screens, so they age with them. It also lets them move — the route draws
// itself in, the tiles land one by one, and today's ride is still counting.
enum AppFace: Equatable {
    case route      // plan a route and send it over
    case maps       // bake offline tiles onto the card
    case rides      // read recorded rides back
}

struct PhoneMock: View {
    var face: AppFace = .route
    /// Screen width. The body adds its bezel around this.
    var width: CGFloat = 118
    var live: Bool = true

    /// iPhone points → this mock's points, so the screens below can be written
    /// at the sizes they really are.
    private var u: CGFloat { width / 393 }
    private var screenH: CGFloat { width * 852 / 393 }
    private var bezel: CGFloat { width * 0.042 }

    @State private var start = Date()
    @State private var faceSince = Date()
    /// The screen actually on the glass, which lags `face` by the length of the
    /// swap below.
    @State private var shown: AppFace?
    @State private var blank = false

    var body: some View {
        TimelineView(.animation(minimumInterval: live ? 1.0 / 20 : nil, paused: !live)) { ctx in
            let t = live ? ctx.date.timeIntervalSince(start) : 0
            // Anything that plays out once is timed from when its screen came
            // up, not from when the tutorial opened — otherwise arriving here
            // late shows a progress bar that finished before it was seen.
            let ft = live ? ctx.date.timeIntervalSince(faceSince) : 4
            screen(sim: RideSim(t: t), ft: ft)
                .opacity(blank ? 0 : 1)
        }
        // Screens are swapped through blank rather than cross-faded. Dissolving
        // one dense screen into another leaves two sets of headings and numbers
        // legible on top of each other for the length of the animation, which
        // reads as a rendering fault rather than as navigation.
        .task(id: face) {
            guard shown != face else { return }
            if shown != nil {
                withAnimation(.easeIn(duration: 0.12)) { blank = true }
                try? await Task.sleep(for: .milliseconds(130))
            }
            shown = face
            faceSince = Date()
            withAnimation(.easeOut(duration: 0.16)) { blank = false }
        }
    }

    private func screen(sim: RideSim, ft: TimeInterval) -> some View {
        Group {
            switch shown ?? face {
            case .route: RouteScreen(u: u, ft: ft)
            case .maps:  MapsScreen(u: u, ft: ft)
            case .rides: RidesScreen(u: u, sim: sim)
            }
        }
        .frame(width: width, height: screenH)
        .background(Palette.paper)
        .clipShape(RoundedRectangle(cornerRadius: width * 0.115, style: .continuous))
        .overlay(alignment: .top) { island }
        .padding(bezel)
        .background(
            RoundedRectangle(cornerRadius: width * 0.155, style: .continuous)
                .fill(Color(hex: 0x1D1D1F))
                .shadow(color: .black.opacity(0.18), radius: width * 0.05,
                        x: 0, y: width * 0.02))
    }

    private var island: some View {
        Capsule()
            .fill(Color(hex: 0x1D1D1F))
            .frame(width: width * 0.29, height: width * 0.075)
            .padding(.top, width * 0.028)
    }
}

// MARK: - shared furniture

/// The nav bar every screen in the app has.
private struct PhoneTitle: View {
    let text: String
    let u: CGFloat

    var body: some View {
        Text(text)
            .font(BarlowFont.condensed(38 * u, .bold))
            .foregroundStyle(Palette.ink)
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(.horizontal, 20 * u)
            .padding(.top, 56 * u)
            .padding(.bottom, 8 * u)
    }
}

/// The tab bar, so the screens read as places in an app rather than as posters.
private struct PhoneTabs: View {
    let active: Int
    let u: CGFloat

    // The app's own four, in its own order — BikeGPSCompanionApp's TabView.
    // Maps isn't here because it isn't a tab: it is a sheet off Settings.
    private let tabs = [("speedometer", "Ride"), ("map", "Route"),
                        ("list.bullet.rectangle", "Rides"), ("slider.horizontal.3", "Settings")]

    var body: some View {
        HStack(spacing: 0) {
            ForEach(Array(tabs.enumerated()), id: \.offset) { i, tab in
                VStack(spacing: 2 * u) {
                    Image(systemName: tab.0)
                        .font(.system(size: 16 * u, weight: .semibold))
                    Text(tab.1)
                        .font(BarlowFont.text(8 * u, .medium))
                        .lineLimit(1)
                        .minimumScaleFactor(0.7)
                }
                .foregroundStyle(i == active ? Palette.accent : Palette.faint)
                .frame(maxWidth: .infinity)
            }
        }
        .padding(.top, 10 * u)
        .padding(.bottom, 20 * u)
        .background(Palette.surface)
        .overlay(alignment: .top) { Rectangle().fill(Palette.hairline).frame(height: 1) }
    }
}


/// The map under everything, in the colours the app's map actually uses: warm
/// land, white roads by class, green for parks, grey-blue for water. Enough of a
/// place for a route to run through and for hexes to be picked off.
private struct MiniMap: View {
    let u: CGFloat
    /// 0…1 of the planned route drawn in; nil for no route.
    var route: Double? = nil
    /// 0…1 of the picked hexes already on the device; nil when nothing is being
    /// sent. Hexes, not squares — the app picks H3 cells, and a grid of squares
    /// was the tell that this mock had been drawn from memory.
    var hexes: Double? = nil
    /// A ride card's thumbnail: bigger blocks, one recorded track, no chrome.
    var thumbnail: Bool = false

    var body: some View {
        Canvas { ctx, size in
            base(&ctx, size: size)
            if let route { drawRoute(&ctx, size: size, progress: route) }
            if thumbnail { drawTrack(&ctx, size: size) }
            if let hexes { drawHexes(&ctx, size: size, progress: hexes) }
        }
    }

    // MARK: the place

    private var block: CGFloat { (thumbnail ? 78 : 58) * u }

    private func base(_ ctx: inout GraphicsContext, size: CGSize) {
        // Land a clear step darker than the roads: at thumbnail size the two
        // were within a few percent of each other and the map read as blank
        // paper with a stripe on it.
        ctx.fill(Path(CGRect(origin: .zero, size: size)), with: .color(Color(hex: 0xDED8CB)))

        // Water along one corner and a park or two, so the map has landmarks
        // rather than being an endless even grid.
        var bay = Path()
        bay.move(to: .init(x: size.width, y: size.height * 0.16))
        bay.addCurve(to: .init(x: size.width * 0.58, y: 0),
                     control1: .init(x: size.width * 0.88, y: size.height * 0.06),
                     control2: .init(x: size.width * 0.74, y: 0))
        bay.addLine(to: .init(x: size.width, y: 0))
        bay.closeSubpath()
        ctx.fill(bay, with: .color(Color(hex: 0xA8C7DA)))

        for r in [CGRect(x: block * 0.3, y: size.height - block * 1.9,
                         width: block * 1.4, height: block * 0.9),
                  CGRect(x: size.width - block * 1.6, y: size.height * 0.44,
                         width: block * 1.2, height: block * 0.8)] {
            ctx.fill(Path(roundedRect: r, cornerRadius: 3 * u), with: .color(Color(hex: 0xBCD69F)))
        }

        var minor = Path(), major = Path()
        var i = 0
        var x = block
        while x < size.width {
            // Every third street is an arterial, which is what keeps the grid
            // from reading as graph paper.
            if i % 3 == 0 { major.move(to: .init(x: x, y: 0)); major.addLine(to: .init(x: x, y: size.height)) }
            else { minor.move(to: .init(x: x, y: 0)); minor.addLine(to: .init(x: x, y: size.height)) }
            x += block; i += 1
        }
        i = 0
        var y = block
        while y < size.height {
            if i % 3 == 1 { major.move(to: .init(x: 0, y: y)); major.addLine(to: .init(x: size.width, y: y)) }
            else { minor.move(to: .init(x: 0, y: y)); minor.addLine(to: .init(x: size.width, y: y)) }
            y += block; i += 1
        }
        ctx.stroke(minor, with: .color(.white), lineWidth: 4 * u)
        ctx.stroke(major, with: .color(.white), lineWidth: 7.5 * u)
    }

    // MARK: what's drawn on it

    /// The planned route, following the streets and turning at junctions — a
    /// line that ignores the roads under it is what makes a map look fake.
    private func drawRoute(_ ctx: inout GraphicsContext, size: CGSize, progress: Double) {
        let pts: [CGPoint] = [
            .init(x: block * 0.6, y: size.height - block * 0.7),
            .init(x: block * 0.6, y: size.height - block * 2.6),
            .init(x: block * 2.0, y: size.height - block * 2.6),
            .init(x: block * 2.0, y: size.height - block * 4.0),
            .init(x: block * 3.4, y: size.height - block * 4.0),
            .init(x: block * 3.4, y: size.height - block * 5.2),
        ]
        var lens: [CGFloat] = []
        for i in 1..<pts.count {
            lens.append(abs(pts[i].x - pts[i - 1].x) + abs(pts[i].y - pts[i - 1].y))
        }
        var left = lens.reduce(0, +) * progress

        var path = Path()
        path.move(to: pts[0])
        var end = pts[0]
        for i in 1..<pts.count {
            let len = lens[i - 1]
            if left >= len { path.addLine(to: pts[i]); end = pts[i]; left -= len }
            else {
                let f = len == 0 ? 0 : left / len
                end = .init(x: pts[i - 1].x + (pts[i].x - pts[i - 1].x) * f,
                            y: pts[i - 1].y + (pts[i].y - pts[i - 1].y) * f)
                path.addLine(to: end)
                break
            }
        }
        ctx.stroke(path, with: .color(Palette.accent),
                   style: StrokeStyle(lineWidth: 5 * u, lineCap: .round, lineJoin: .round))
        dot(&ctx, at: pts[0], r: 4.5 * u, fill: Palette.good)      // where you set off
        if progress > 0.98 { dot(&ctx, at: end, r: 5.5 * u, fill: Palette.accent) }
    }

    /// A recorded ride, for the list's thumbnails: a track across the city with
    /// the green start dot the real cards show.
    private func drawTrack(_ ctx: inout GraphicsContext, size: CGSize) {
        var p = Path()
        let a = CGPoint(x: size.width * 0.22, y: size.height * 0.82)
        p.move(to: a)
        p.addCurve(to: .init(x: size.width * 0.82, y: size.height * 0.18),
                   control1: .init(x: size.width * 0.42, y: size.height * 0.60),
                   control2: .init(x: size.width * 0.56, y: size.height * 0.46))
        ctx.stroke(p, with: .color(Palette.accent),
                   style: StrokeStyle(lineWidth: 4.5 * u, lineCap: .round))
        dot(&ctx, at: a, r: 4 * u, fill: Palette.good)
    }

    /// The hex picker: the area you tapped out, filling in as the app streams
    /// each cell to the card. Cells already on the device go green, the ones
    /// still queued stay orange, and everything outside the selection is the
    /// grid you could tap next.
    ///
    /// The selection is every cell within two rings of the middle, which is
    /// exactly 19 — the number the panel underneath claims. Picking the cells
    /// geometrically instead gave a blob of a hundred under a label reading
    /// "19", which is the sort of detail that makes a mock look drawn from
    /// memory.
    private func drawHexes(_ ctx: inout GraphicsContext, size: CGSize, progress: Double) {
        let r = 26 * u
        let centre = CGPoint(x: size.width / 2, y: size.height * 0.5)

        func place(_ q: Int, _ s: Int) -> CGPoint {
            CGPoint(x: centre.x + r * 1.5 * CGFloat(q),
                    y: centre.y + r * sqrt(3) * (CGFloat(s) + CGFloat(q) / 2))
        }
        func ring(_ q: Int, _ s: Int) -> Int {
            (abs(q) + abs(q + s) + abs(s)) / 2
        }

        let reach = Int((max(size.width, size.height) / r).rounded(.up)) + 1
        // Rings 0…2 are the selection, in the order the app sends them: the
        // middle first, then outward.
        let picked = (-2...2).flatMap { q in (-2...2).map { (q, $0) } }
            .filter { ring($0.0, $0.1) <= 2 }
            .sorted { ring($0.0, $0.1) < ring($1.0, $1.1) }
        let sent = Int((Double(picked.count) * progress).rounded())

        for q in -reach...reach {
            for sIdx in -reach...reach {
                let c = place(q, sIdx)
                guard c.x > -r, c.x < size.width + r, c.y > -r, c.y < size.height + r else { continue }
                let hex = hexPath(at: c, r: r)
                guard let idx = picked.firstIndex(where: { $0 == (q, sIdx) }) else {
                    ctx.stroke(hex, with: .color(.white.opacity(0.65)), lineWidth: 1)
                    continue
                }
                let done = idx < sent
                ctx.fill(hex, with: .color((done ? Palette.good : Palette.accent).opacity(0.3)))
                ctx.stroke(hex, with: .color(done ? Palette.good : Palette.accent), lineWidth: 1.6)
            }
        }
    }

    private func hexPath(at c: CGPoint, r: CGFloat) -> Path {
        var p = Path()
        for i in 0..<6 {
            let a = CGFloat(i) * .pi / 3
            let pt = CGPoint(x: c.x + r * cos(a), y: c.y + r * sin(a))
            if i == 0 { p.move(to: pt) } else { p.addLine(to: pt) }
        }
        p.closeSubpath()
        return p
    }

    private func dot(_ ctx: inout GraphicsContext, at p: CGPoint, r: CGFloat, fill: Color) {
        let rect = CGRect(x: p.x - r, y: p.y - r, width: r * 2, height: r * 2)
        ctx.fill(Path(ellipseIn: rect), with: .color(fill))
        ctx.stroke(Path(ellipseIn: rect), with: .color(.white), lineWidth: 1.6 * u)
    }
}

// MARK: - the three screens

private struct RouteScreen: View {
    let u: CGFloat
    let ft: TimeInterval

    /// Draws in over a couple of seconds, then holds — long enough to watch it
    /// happen, short enough that it has happened by the time you look up.
    private var progress: Double { min(1, max(0, ft - 0.3) / 2.2) }

    var body: some View {
        VStack(spacing: 0) {
            PhoneTitle(text: "Route", u: u)

            HStack(spacing: 6 * u) {
                Image(systemName: "magnifyingglass")
                    .font(.system(size: 11 * u, weight: .semibold))
                    .foregroundStyle(Palette.faint)
                Text("Fisherman's Wharf")
                    .font(BarlowFont.text(12 * u, .medium))
                    .foregroundStyle(Palette.ink)
                Spacer(minLength: 0)
            }
            .padding(.horizontal, 12 * u)
            .padding(.vertical, 9 * u)
            .background(Palette.surface)
            .clipShape(RoundedRectangle(cornerRadius: 10 * u, style: .continuous))
            .overlay(RoundedRectangle(cornerRadius: 10 * u, style: .continuous)
                .strokeBorder(Palette.hairline, lineWidth: 1))
            .padding(.horizontal, 16 * u)

            MiniMap(u: u, route: progress)
                .clipShape(RoundedRectangle(cornerRadius: 14 * u, style: .continuous))
                .padding(.horizontal, 16 * u)
                .padding(.top, 10 * u)
                .frame(maxHeight: .infinity)

            VStack(spacing: 8 * u) {
                HStack(alignment: .firstTextBaseline, spacing: 6 * u) {
                    Text("12.2")
                        .font(BarlowFont.condensed(30 * u, .bold))
                        .foregroundStyle(Palette.ink)
                    Text("KM")
                        .font(BarlowFont.text(11 * u, .semibold))
                        .foregroundStyle(Palette.muted)
                    Spacer(minLength: 0)
                    Text("46 min")
                        .font(BarlowFont.text(12 * u, .medium))
                        .foregroundStyle(Palette.muted)
                }
                Text(progress < 1 ? "Building route…" : "Connect to send")
                    .font(BarlowFont.condensed(17 * u, .semibold))
                    .foregroundStyle(Palette.accentInk)
                    .frame(maxWidth: .infinity)
                    .padding(.vertical, 10 * u)
                    .background(progress < 1 ? Palette.faint : Palette.accent)
                    .clipShape(Capsule())
            }
            .padding(.horizontal, 16 * u)
            .padding(.vertical, 12 * u)

            PhoneTabs(active: 1, u: u)
        }
    }
}

// Picking an area and streaming it to the card. The map fills the screen and
// the hexes sit on top of it, because in the app the map IS the interface —
// you tap cells on the place you are looking at. Showing a bare grid of
// squares on paper, as this did, was a picture of a progress bar rather than a
// picture of the screen.
private struct MapsScreen: View {
    let u: CGFloat
    let ft: TimeInterval

    private var progress: Double { min(1, max(0, ft - 0.4) / 4.5) }
    /// Rings 0…2 of the hex grid — the same 19 cells MiniMap fills in.
    private let picked = 19

    var body: some View {
        VStack(spacing: 0) {
            // A sheet, because that is how the app opens this: presented over
            // Settings, so it gets a grabber and no tab bar.
            Capsule()
                .fill(Palette.hairline)
                .frame(width: 34 * u, height: 5 * u)
                .padding(.top, 8 * u)

            PhoneTitle(text: "Maps", u: u)

            MiniMap(u: u, hexes: progress)
                .clipShape(RoundedRectangle(cornerRadius: 14 * u, style: .continuous))
                .padding(.horizontal, 16 * u)
                .frame(maxHeight: .infinity)

            VStack(alignment: .leading, spacing: 7 * u) {
                Text("SELECTED AREA")
                    .font(BarlowFont.text(9 * u, .semibold))
                    .tracking(0.9 * u)
                    .foregroundStyle(Palette.muted)
                HStack(alignment: .firstTextBaseline, spacing: 5 * u) {
                    Text(progress < 1 ? "\(Int(Double(picked) * progress))" : "\(picked)")
                        .font(BarlowFont.condensed(26 * u, .bold))
                        .foregroundStyle(Palette.ink)
                    Text(progress < 1 ? "of \(picked) hexes sent" : "hexes on the device")
                        .font(BarlowFont.text(11 * u, .medium))
                        .foregroundStyle(Palette.muted)
                    Spacer(minLength: 0)
                }
                ZStack(alignment: .leading) {
                    Capsule().fill(Palette.hairline)
                    GeometryReader { g in
                        Capsule().fill(progress < 1 ? Palette.accent : Palette.good)
                            .frame(width: g.size.width * progress)
                    }
                }
                .frame(height: 5 * u)
            }
            .padding(.horizontal, 16 * u)
            .padding(.vertical, 12 * u)
            .padding(.bottom, 14 * u)
        }
    }
}

// The rides list, as it actually is: a week's totals, then a card per ride with
// the map of where you went. The map is the whole point of the card — it is how
// you recognise a ride before you have read a single number off it.
private struct RidesScreen: View {
    let u: CGFloat
    let sim: RideSim

    var body: some View {
        VStack(spacing: 0) {
            PhoneTitle(text: "Rides", u: u)

            VStack(spacing: 10 * u) {
                HStack(spacing: 0) {
                    week("3", "rides this week")
                    week("14.4", "km distance")
                    week("29m", "riding time")
                }
                .padding(.vertical, 10 * u)
                .padding(.horizontal, 12 * u)
                .background(Palette.surface)
                .clipShape(RoundedRectangle(cornerRadius: 12 * u, style: .continuous))
                .overlay(RoundedRectangle(cornerRadius: 12 * u, style: .continuous)
                    .strokeBorder(Palette.hairline, lineWidth: 1))

                rideCard(title: "Sunday night ride", when: "Jul 19 · 12:15 AM",
                         distance: "4.8", time: "9m", speed: "5.1", power: "219")
                rideCard(title: "Saturday morning ride", when: "Jul 18 · 9:30 AM",
                         distance: "12.6", time: "34m", speed: "22.2", power: "204")
                rideCard(title: "Thursday commute", when: "Jul 16 · 8:02 AM",
                         distance: "9.1", time: "26m", speed: "21.0", power: "188")
                rideCard(title: "Tuesday intervals", when: "Jul 14 · 6:40 PM",
                         distance: "31.7", time: "1:04", speed: "29.7", power: "263")
            }
            .padding(.horizontal, 16 * u)
            // The list runs on past the bottom of the screen, the way a list
            // with more in it than fits does.
            .frame(maxHeight: .infinity, alignment: .top)
            .clipped()

            PhoneTabs(active: 2, u: u)
        }
    }

    private func week(_ value: String, _ label: String) -> some View {
        VStack(alignment: .leading, spacing: 0) {
            Text(value)
                .font(BarlowFont.condensed(21 * u, .bold))
                .foregroundStyle(Palette.ink)
            Text(label)
                .font(BarlowFont.text(8 * u))
                .foregroundStyle(Palette.muted)
                .lineLimit(1)
                .minimumScaleFactor(0.7)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    private func rideCard(title: String, when: String, distance: String,
                          time: String, speed: String, power: String) -> some View {
        VStack(alignment: .leading, spacing: 7 * u) {
            MiniMap(u: u, thumbnail: true)
                .frame(height: 88 * u)
                .clipShape(RoundedRectangle(cornerRadius: 8 * u, style: .continuous))

            HStack(spacing: 5 * u) {
                Text(title)
                    .font(BarlowFont.text(11 * u, .semibold))
                    .foregroundStyle(Palette.ink)
                    .lineLimit(1)
                Text(when)
                    .font(BarlowFont.text(8 * u))
                    .foregroundStyle(Palette.muted)
                    .lineLimit(1)
                Spacer(minLength: 0)
            }

            HStack(spacing: 0) {
                stat(distance, "Distance")
                stat(time, "Time")
                stat(speed, "km/h")
                stat(power, "Power")
            }
        }
        .padding(9 * u)
        .background(Palette.surface)
        .clipShape(RoundedRectangle(cornerRadius: 12 * u, style: .continuous))
        .overlay(RoundedRectangle(cornerRadius: 12 * u, style: .continuous)
            .strokeBorder(Palette.hairline, lineWidth: 1))
    }

    private func stat(_ value: String, _ label: String) -> some View {
        VStack(alignment: .leading, spacing: 0) {
            Text(value)
                .font(BarlowFont.condensed(17 * u, .bold))
                .foregroundStyle(Palette.ink)
                .lineLimit(1)
            Text(label)
                .font(BarlowFont.text(7.5 * u))
                .foregroundStyle(Palette.muted)
                .lineLimit(1)
                .minimumScaleFactor(0.7)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }
}
