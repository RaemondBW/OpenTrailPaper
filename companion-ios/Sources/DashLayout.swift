import Foundation

// Swift side of the device's dashboard layout (src/dash_layout.h).
//
// The wire format IS the config file's text — the same bytes that sit at
// /config/dashboard.cfg on the card. Keeping one representation means there is
// no third encoding to keep in step: what the rider edits here is literally
// what lands in the file, and what a hand-edited file says is exactly what this
// editor shows.
//
// The field ids and size names below MUST match dash_layout.cpp's tables. They
// are the contract; the labels are cosmetic.

struct DashField: Identifiable, Hashable {
    let id: String        // "power3s" — the config token
    let label: String     // "Power (3s)" — what the rider picks from
    let detail: String    // where the number comes from

    // Ordered as they appear in the picker. Every one maps to something the
    // device already measures — a paired sensor, the recorder, the GPS, or the
    // elevation grid baked into the map tiles.
    static let all: [DashField] = [
        .init(id: "speed",      label: "Speed",        detail: "GPS"),
        .init(id: "power3s",    label: "Power (3s)",   detail: "Power meter, 3s average"),
        .init(id: "power",      label: "Power",        detail: "Power meter, instant"),
        .init(id: "hr",         label: "Heart rate",   detail: "Heart-rate strap"),
        .init(id: "cadence",    label: "Cadence",      detail: "Cadence sensor or power meter"),
        .init(id: "distance",   label: "Distance",     detail: "This ride"),
        .init(id: "ridetime",   label: "Ride time",    detail: "Elapsed"),
        .init(id: "movingtime", label: "Moving time",  detail: "Elapsed minus stops"),
        .init(id: "climb",      label: "Climb",        detail: "From the map elevation grid"),
        .init(id: "grade",      label: "Grade",        detail: "From the map elevation grid"),
        .init(id: "altitude",   label: "Altitude",     detail: "From the map elevation grid"),
        .init(id: "battery",    label: "Battery",      detail: "Fuel gauge"),
        .init(id: "sats",       label: "Satellites",   detail: "GPS"),
        .init(id: "clock",      label: "Clock",        detail: "Time of day"),
        .init(id: "routeleft",  label: "Route left",   detail: "Distance to the end of the route"),
    ]

    static func named(_ id: String) -> DashField? { all.first { $0.id == id } }
}

enum DashSize: String, CaseIterable, Identifiable {
    case small, medium, large, hero
    var id: String { rawValue }

    var label: String {
        switch self {
        case .small:  return "Small"
        case .medium: return "Medium"
        case .large:  return "Large"
        case .hero:   return "Hero"
        }
    }

    /// Height share, matching kWeight in ui_render.cpp — the preview is only
    /// honest if it divides the panel the same way the device does.
    var weight: Int {
        switch self {
        case .small: return 2
        case .medium: return 3
        case .large: return 4
        case .hero: return 8
        }
    }
}

struct DashItem: Identifiable, Hashable {
    let id = UUID()
    var field: String
    var size: DashSize
    var half: Bool

    var fieldLabel: String { DashField.named(field)?.label ?? field }

    /// The caption the DEVICE draws above this field — kFields in
    /// dash_layout.cpp. Uppercasing the picker label matches every entry except
    /// the 3 s power average, whose panel caption uses a middot, and the layout
    /// preview has to show what the panel will show.
    var panelLabel: String {
        field == "power3s" ? "POWER · 3S" : fieldLabel.uppercased()
    }
}

struct DashLayout: Equatable {
    var items: [DashItem]

    static func == (a: DashLayout, b: DashLayout) -> Bool {
        a.configText == b.configText
    }

    /// At most 12, matching DASH_MAX_ITEMS — the device silently stops parsing
    /// past that, so the editor must not let a rider build a layout whose tail
    /// would vanish without explanation.
    static let maxItems = 12

    init(items: [DashItem]) { self.items = items }

    /// Parse the device's config text. Deliberately as forgiving as the
    /// firmware's dashParse: unknown tokens are skipped, not fatal, so a
    /// hand-edited file with one typo still opens in the editor.
    init(text: String) {
        var out: [DashItem] = []
        for rawLine in text.split(separator: "\n", omittingEmptySubsequences: false) {
            let line = rawLine.split(separator: "#", maxSplits: 1,
                                     omittingEmptySubsequences: false)[0]
            let tok = line.split(whereSeparator: { $0 == " " || $0 == "\t" || $0 == "\r" })
            guard let first = tok.first, DashField.named(String(first)) != nil else { continue }
            let size = tok.count > 1 ? DashSize(rawValue: String(tok[1])) ?? .medium : .medium
            let half = tok.dropFirst().contains { $0 == "half" }
            out.append(DashItem(field: String(first), size: size, half: half))
            if out.count >= DashLayout.maxItems { break }
        }
        items = out
    }

    /// Serialize back to the config file's text, byte-compatible with
    /// dashSerialize() so a round trip through the device changes nothing.
    var configText: String {
        var s = "# OpenTrailPaper dashboard layout\n"
        s += "# <field> <small|medium|large|hero> [half]\n"
        s += "# 'half' shares the row with the next 'half' field.\n"
        for it in items {
            let field = it.field.padding(toLength: max(10, it.field.count),
                                         withPad: " ", startingAt: 0)
            let size = it.size.rawValue.padding(toLength: max(6, it.size.rawValue.count),
                                                withPad: " ", startingAt: 0)
            s += "\(field) \(size)\(it.half ? " half" : "")\n"
        }
        return s
    }

    /// The device's built-in default, for the "reset" action.
    static var deviceDefault: DashLayout {
        DashLayout(items: [
            DashItem(field: "power3s", size: .hero,   half: false),
            DashItem(field: "hr",      size: .medium, half: true),
            DashItem(field: "cadence", size: .medium, half: true),
            DashItem(field: "ridetime", size: .medium, half: true),
            DashItem(field: "distance", size: .medium, half: true),
        ])
    }

    /// Group items into rows the way ui_render.cpp packs them: a `half` item
    /// pairs with the NEXT item only if that one is also `half`, otherwise it
    /// spans. Duplicated here so the preview shows what the panel will actually
    /// do — including the surprise that a lone `half` takes the full width.
    var rows: [[DashItem]] {
        var out: [[DashItem]] = []
        var i = 0
        while i < items.count {
            if items[i].half, i + 1 < items.count, items[i + 1].half {
                out.append([items[i], items[i + 1]])
                i += 2
            } else {
                out.append([items[i]])
                i += 1
            }
        }
        return out
    }
}

// MARK: - Pages

/// The whole config: an ordered carousel of pages the device's Home key steps
/// through. A page is either a field layout or the MUSIC page (phone media
/// controls — its content comes over BLE, so it carries no items). Mirrors
/// DashPages in src/dash_layout.h; the `page` / `page music` separator lines
/// are the wire format, and text with no separators is exactly the old
/// one-page config.
struct DashConfig: Equatable {
    struct Page: Identifiable {
        // Stable identity for SwiftUI. Index-keyed cards made a deletion reuse
        // the wrong views — the neighbour appeared duplicated and the map card
        // vanished until the next full re-render.
        let id = UUID()
        var kind: Kind
        var layout: DashLayout
        enum Kind { case fields, music, map, workout }
        var isMusic: Bool { kind == .music }
        var isMap: Bool { kind == .map }
        var isWorkout: Bool { kind == .workout }
        static var music: Page { Page(kind: .music, layout: DashLayout(items: [])) }
        static var fields: Page { Page(kind: .fields, layout: DashLayout(items: [])) }
        static var map: Page { Page(kind: .map, layout: DashLayout(items: [])) }
        static var workout: Page { Page(kind: .workout, layout: DashLayout(items: [])) }
    }

    var pages: [Page]
    /// The map screen's 3-cell data strip (`map <f> <f> <f>` in the config).
    var mapFields: [String] = ["speed", "distance", "ridetime"]

    /// Configurable (non-map) pages the device accepts; the map page rides
    /// along on top of these (DASH_MAX_PAGES = 5 on the device).
    static let maxPages = 4

    var contentPages: Int { pages.filter { !$0.isMap }.count }

    static func == (a: DashConfig, b: DashConfig) -> Bool {
        a.configText == b.configText
    }

    init(pages: [Page]) { self.pages = pages }

    /// Parse the device's text: split into per-page chunks on `page` lines,
    /// reusing DashLayout's forgiving field parsing for each chunk.
    init(text: String) {
        var out: [Page] = []
        var chunk = ""
        var kind = Page.Kind.fields
        var sawMap = false
        func commit() {
            let layout = kind == .fields ? DashLayout(text: chunk) : DashLayout(items: [])
            var keep = kind != .fields || !layout.items.isEmpty
            if kind == .map {
                if sawMap { keep = false } else { sawMap = true }
            }
            if keep { out.append(Page(kind: kind, layout: layout)) }
            chunk = ""
            kind = .fields
        }
        var strip = ["speed", "distance", "ridetime"]
        for rawLine in text.split(separator: "\n", omittingEmptySubsequences: false) {
            let line = rawLine.split(separator: "#", maxSplits: 1,
                                     omittingEmptySubsequences: false)[0]
            let tok = line.split(whereSeparator: { $0 == " " || $0 == "\t" || $0 == "\r" })
            if tok.first == "page" {
                commit()
                if tok.count > 1, tok[1] == "music" { kind = .music }
                else if tok.count > 1, tok[1] == "map" { kind = .map }
                else if tok.count > 1, tok[1] == "workout" { kind = .workout }
            } else if tok.first == "map" {
                for i in 0..<3 where tok.count > i + 1 {
                    if DashField.named(String(tok[i + 1])) != nil {
                        strip[i] = String(tok[i + 1])
                    }
                }
            } else {
                chunk += rawLine + "\n"
            }
        }
        commit()
        // A pre-map-page config: the map belongs at the end, where the old
        // fixed cycle put it.
        if !sawMap { out.append(.map) }
        pages = Array(out.prefix(DashConfig.maxPages + 1))
        if !pages.contains(where: { $0.isMap }) { pages.append(.map) }
        mapFields = strip
    }

    /// Serialize, byte-compatible with dashSerializePages(): the first field
    /// page carries no `page` line, so a one-page config round-trips to the
    /// pre-pages format.
    var configText: String {
        // Byte-identical with dash_layout.cpp's kHeader — `==` between the
        // app's config and the device's echo is a string comparison.
        var s = "# OpenTrailPaper dashboard layout\n"
        s += "# <field> <small|medium|large|hero> [half]; 'page' or 'page music' starts a new page\n"
        s += "map \(mapFields[0]) \(mapFields[1]) \(mapFields[2])\n"
        for (i, page) in pages.enumerated() {
            if page.isMusic {
                s += "page music\n"
            } else if page.isMap {
                s += "page map\n"
            } else if page.isWorkout {
                s += "page workout\n"
            } else if i > 0 {
                s += "page\n"
            }
            if page.kind == .fields {
                for it in page.layout.items {
                    let field = it.field.padding(toLength: max(10, it.field.count),
                                                 withPad: " ", startingAt: 0)
                    let size = it.size.rawValue.padding(toLength: max(6, it.size.rawValue.count),
                                                        withPad: " ", startingAt: 0)
                    s += "\(field) \(size)\(it.half ? " half" : "")\n"
                }
            }
        }
        return s
    }

    /// The first data page — what thumbnails show.
    var firstFields: DashLayout? {
        pages.first(where: { $0.kind == .fields })?.layout
    }

    var hasMusicPage: Bool { pages.contains { $0.isMusic } }
    var hasWorkoutPage: Bool { pages.contains { $0.isWorkout } }

    static var deviceDefault: DashConfig {
        DashConfig(pages: [Page(kind: .fields, layout: .deviceDefault), .map])
    }
}
