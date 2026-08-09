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
