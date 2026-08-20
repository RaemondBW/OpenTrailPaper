import SwiftUI

// Rearrange the head unit's dashboard from the phone.
//
// Lives on the Ride tab rather than in Settings: this is about the numbers you
// are looking at right now, and the live status is what makes "should Distance
// be bigger?" a question you can actually answer. Settings is for device
// housekeeping, which this is not.
//
// The preview is the point. A list of field names cannot tell you that four
// mediums leave everything small, or that a lone `half` field silently spans
// the row — so the editor draws what the panel will draw, using the same
// packing rules and height weights as ui_render.cpp.
struct DashboardEditorView: View {
    @EnvironmentObject var ble: BLEManager
    @Environment(\.dismiss) private var dismiss

    @State private var config = DashConfig(pages: [])
    @State private var pageIx = 0
    @State private var showAdd = false
    @State private var showReorder = false
    @State private var scrolled: Int?
    @State private var dirty = false

    var body: some View {
        NavigationStack {
            Group {
                if ble.dashConfig == nil {
                    ContentUnavailableView(
                        "Connect to edit",
                        systemImage: "rectangle.3.group",
                        description: Text("The layout is read from the device, so it can't be edited until the head unit is connected."))
                } else {
                    editor
                }
            }
            .navigationTitle("Dashboard")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Done") { dismiss() }
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Send") {
                        ble.sendDashConfig(config)
                        dirty = false
                    }
                    // Enabled whenever this layout is not what the device is
                    // holding — a comparison, not a flag someone has to
                    // remember to set.
                    //
                    // The flag version cleared itself the moment Send was
                    // tapped, so if the write never landed the button was dead
                    // and the only way to retry was to close the sheet and edit
                    // something. Against the device's own copy it stays live
                    // until the device reports the layout back, and it goes
                    // quiet by itself once it matches — including when the
                    // rider edits their way back to what is already on there.
                    .disabled(config.pages.isEmpty || config == ble.dashConfig)
                }
            }
            .onAppear { if let c = ble.dashConfig { config = c; pageIx = 0 } }
            // The device is the source of truth: if it corrects or rejects what
            // we sent, adopt what it actually holds rather than keeping a local
            // fiction on screen. Skipped while the rider has unsent edits, so a
            // stray notify can't wipe work in progress.
            .onChange(of: ble.dashConfig) {
                if !dirty, let c = ble.dashConfig {
                    config = c
                    if pageIx >= config.pages.count { pageIx = 0; scrolled = 0 }
                }
            }
            .sheet(isPresented: $showAdd) {
                FieldPicker { id in
                    mutateAt(pageIx) { $0.items.append(DashItem(field: id, size: .medium, half: true)) }
                }
            }
        }
    }

    private var editor: some View {
        List {
            // The carousel: every page as a peeking preview card, the add card
            // at the far end. The selected card's editor follows below.
            Section {
                carousel
                    .listRowInsets(EdgeInsets())
                    .listRowBackground(Color.clear)
            } footer: {
                if config.pages.count > 1 {
                    Text("Swipe the carousel · hold a page to reorder")
                }
            }

            if config.pages.indices.contains(pageIx), config.pages[pageIx].isMusic {
                Section {
                    VStack(alignment: .leading, spacing: 8) {
                        Label("Music controls", systemImage: "music.note")
                        Text("Shows what's playing on the phone — any app — with play/skip and volume, via the phone's own media service (pair when asked). Album art appears for Apple Music.")
                            .font(TypeScale.body).foregroundStyle(Palette.muted)
                    }
                    .padding(.vertical, 4)
                }
            } else if config.pages.indices.contains(pageIx) {
                Section {
                    ForEach(config.pages[pageIx].layout.items) { item in
                        DashItemRow(item: bindingFor(item, in: pageIx)) { dirty = true }
                    }
                    .onMove { from, to in
                        mutateAt(pageIx) { $0.items.move(fromOffsets: from, toOffset: to) }
                    }
                    .onDelete { idx in
                        mutateAt(pageIx) { $0.items.remove(atOffsets: idx) }
                    }
                    if config.pages[pageIx].layout.items.count < DashLayout.maxItems {
                        Button {
                            showAdd = true
                        } label: {
                            Label("Add a field", systemImage: "plus.circle.fill")
                        }
                    }
                } header: {
                    Text("Fields")
                } footer: {
                    VStack(alignment: .leading, spacing: 6) {
                        Text("Drag to reorder, swipe to remove. “Half width” pairs a field with the next half-width field; on its own it spans the row.")
                        Text("Fields that need a sensor — power, heart rate, cadence — are hidden on the device until it connects, and the rest expand to fill the space. Route left hides when no route is loaded.")
                    }
                }

                Section {
                    Button("Reset to the default layout") {
                        config = .deviceDefault
                        pageIx = 0
                        scrolled = 0
                        dirty = true
                    }
                }
            }
        }
        .environment(\.editMode, .constant(.active))
        .sheet(isPresented: $showReorder) { reorderSheet }
    }

    private var carousel: some View {
        ScrollView(.horizontal, showsIndicators: false) {
            LazyHStack(spacing: 16) {
                ForEach(config.pages.indices, id: \.self) { i in
                    carouselCard(i).id(i)
                }
                if config.pages.count < DashConfig.maxPages {
                    addCard.id(config.pages.count)
                }
            }
            .scrollTargetLayout()
            .padding(.vertical, 14)
        }
        .contentMargins(.horizontal, 56, for: .scrollContent)
        .scrollTargetBehavior(.viewAligned)
        .scrollPosition(id: $scrolled)
        .frame(height: 250)
        .onAppear { scrolled = pageIx }
        // The centred card IS the selection (clamped off the add card, which
        // is browsable but has nothing to edit below).
        .onChange(of: scrolled) {
            if let i = scrolled, i < config.pages.count { pageIx = i }
        }
    }

    private func carouselCard(_ i: Int) -> some View {
        ZStack(alignment: .topTrailing) {
            Group {
                if config.pages[i].isMusic {
                    MusicPreview()
                } else {
                    DashPreview(layout: config.pages[i].layout)
                }
            }
            .frame(width: 124, height: 206)
            .clipShape(RoundedRectangle(cornerRadius: 8))
            .overlay(
                RoundedRectangle(cornerRadius: 8)
                    .stroke(pageIx == i ? Color.accentColor : Palette.hairline,
                            lineWidth: pageIx == i ? 2.5 : 1))
            if config.pages.count > 1 {
                Button {
                    withAnimation {
                        config.pages.remove(at: i)
                        if pageIx >= config.pages.count { pageIx = config.pages.count - 1 }
                        scrolled = pageIx
                    }
                    dirty = true
                } label: {
                    Image(systemName: "minus.circle.fill")
                        .font(.system(size: 22))
                        .symbolRenderingMode(.palette)
                        .foregroundStyle(.white, .red)
                        .background(Circle().fill(.white).padding(2))
                }
                .buttonStyle(.plain)
                .offset(x: 9, y: -9)
                .accessibilityLabel("Remove this page")
            }
        }
        .onTapGesture {
            pageIx = i
            withAnimation { scrolled = i }
        }
        .onLongPressGesture {
            if config.pages.count > 1 { showReorder = true }
        }
    }

    // The far-right card: a new page, data or music.
    private var addCard: some View {
        Menu {
            Button {
                withAnimation {
                    config.pages.append(.fields)
                    pageIx = config.pages.count - 1
                    scrolled = pageIx
                }
                dirty = true
            } label: {
                Label("Data page", systemImage: "rectangle.3.group")
            }
            if !config.hasMusicPage {
                Button {
                    withAnimation {
                        config.pages.append(.music)
                        pageIx = config.pages.count - 1
                        scrolled = pageIx
                    }
                    dirty = true
                } label: {
                    Label("Music controls", systemImage: "music.note")
                }
            }
        } label: {
            VStack(spacing: 10) {
                Image(systemName: "plus.circle")
                    .font(.system(size: 30, weight: .light))
                Text("Add a page").font(.system(size: 13))
            }
            .foregroundStyle(Palette.muted)
            .frame(width: 124, height: 206)
            .background(
                RoundedRectangle(cornerRadius: 8)
                    .strokeBorder(Palette.hairline,
                                  style: StrokeStyle(lineWidth: 1.5, dash: [6, 5])))
        }
    }

    private var reorderSheet: some View {
        NavigationStack {
            List {
                ForEach(config.pages.indices, id: \.self) { i in
                    if config.pages[i].isMusic {
                        Label("Music controls", systemImage: "music.note")
                    } else {
                        Label("Data · \(config.pages[i].layout.items.count) fields",
                              systemImage: "rectangle.3.group")
                    }
                }
                .onMove { from, to in
                    config.pages.move(fromOffsets: from, toOffset: to)
                    dirty = true
                }
            }
            .environment(\.editMode, .constant(.active))
            .navigationTitle("Page order")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") { showReorder = false }
                }
            }
        }
        .presentationDetents([.medium])
    }

    private func mutateAt(_ i: Int, _ f: (inout DashLayout) -> Void) {
        guard config.pages.indices.contains(i), !config.pages[i].isMusic else { return }
        f(&config.pages[i].layout)
        dirty = true
    }

    /// A binding into page `i`'s copy of `item` — ForEach($...) can't reach
    /// through the pages array.
    private func bindingFor(_ item: DashItem, in i: Int) -> Binding<DashItem> {
        Binding(
            get: {
                config.pages.indices.contains(i)
                    ? config.pages[i].layout.items.first(where: { $0.id == item.id }) ?? item
                    : item
            },
            set: { new in
                mutateAt(i) { l in
                    if let ix = l.items.firstIndex(where: { $0.id == item.id }) {
                        l.items[ix] = new
                    }
                }
            })
    }
}

// One configurable row: field name, size, and whether it shares its row.
private struct DashItemRow: View {
    @Binding var item: DashItem
    let changed: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(item.fieldLabel).font(TypeScale.bodyStrong).foregroundStyle(Palette.ink)
            HStack(spacing: 10) {
                Picker("", selection: $item.size) {
                    ForEach(DashSize.allCases) { Text($0.label).tag($0) }
                }
                .pickerStyle(.segmented)
                .onChange(of: item.size) { changed() }

                Toggle(isOn: $item.half) { Text("Half").font(.system(size: 13)) }
                    .toggleStyle(.button)
                    .onChange(of: item.half) { changed() }
            }
        }
        .padding(.vertical, 4)
    }
}

private struct FieldPicker: View {
    @Environment(\.dismiss) private var dismiss
    let pick: (String) -> Void

    var body: some View {
        NavigationStack {
            List(DashField.all) { f in
                Button {
                    pick(f.id)
                    dismiss()
                } label: {
                    VStack(alignment: .leading, spacing: 2) {
                        Text(f.label).foregroundStyle(Palette.ink)
                        Text(f.detail).font(.system(size: 12)).foregroundStyle(Palette.muted)
                    }
                }
            }
            .navigationTitle("Add a field")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") { dismiss() }
                }
            }
        }
        .presentationDetents([.medium, .large])
    }
}

// A scaled likeness of the panel, drawn to the SAME rules ui_render.cpp uses.
//
// The rider is choosing a layout against this picture, so anywhere it differs
// from the panel it is actively misleading. It kept drifting because it was an
// impression of the device rather than a port of it — a value size guessed per
// cell from a character count, no hero treatment at all, no zone bar.
//
// So it now runs the firmware's actual algorithm, in DEVICE PIXELS, and scales
// the result once at the end:
//   - the same type ladders, with the real font metrics baked in below;
//   - one value face per (size class, cell width), the smallest any cell of
//     that class needs, which is what stops two same-sized cells disagreeing;
//   - one caption face for the whole screen;
//   - the hero on its own ladder, caption centred, with the FTP zone bar.

// One face from the firmware's ladders. Sizes are device pixels: `cap` is the
// digit height ui_render measures against, and the advances are what its
// textWidth() returns. Printed straight out of the host build's font tables —
// guessing these is how the preview drifted last time.
private struct Face {
    let cap: CGFloat
    let w8, w1, wDot, wColon: CGFloat

    func width(_ s: String) -> CGFloat {
        s.reduce(0) { acc, c in
            switch c {
            case ".": return acc + wDot
            case ":": return acc + wColon
            case "1": return acc + w1
            default:  return acc + w8
            }
        }
    }
}

// kValueLadder, strictly descending — the firmware relies on that order and so
// does the "smallest face any cell needs" pass below.
private let kValueLadder: [Face] = [
    Face(cap: 158, w8: 103, w1: 73, wDot: 35, wColon: 39),   // Impact_XL
    Face(cap: 120, w8: 78,  w1: 56, wDot: 27, wColon: 30),   // Impact_C
    Face(cap: 95,  w8: 61,  w1: 44, wDot: 21, wColon: 23),   // Impact_H
    Face(cap: 81,  w8: 53,  w1: 38, wDot: 18, wColon: 20),   // Impact_B
    Face(cap: 69,  w8: 45,  w1: 32, wDot: 16, wColon: 17),   // Impact_M
    Face(cap: 58,  w8: 38,  w1: 27, wDot: 13, wColon: 14),   // Impact_A
    Face(cap: 46,  w8: 30,  w1: 21, wDot: 10, wColon: 11),   // Impact_V
    Face(cap: 30,  w8: 20,  w1: 14, wDot: 7,  wColon: 8),    // Impact_T
    Face(cap: 15,  w8: 12,  w1: 12, wDot: 6,  wColon: 7),    // Arial_B
]

/// The hero steps through its own ladder: XL, H, M, V.
private let kHeroLadder: [Face] = [kValueLadder[0], kValueLadder[2],
                                   kValueLadder[4], kValueLadder[6]]

/// kLabelLadder. `perChar` is the tracked caption's average advance, taken from
/// the width of "HEART RATE" in each face.
private struct LabelFace { let cap: CGFloat; let ascender: CGFloat; let perChar: CGFloat }
private let kLabelLadder: [LabelFace] = [
    LabelFace(cap: 28, ascender: 38, perChar: 30.1),   // ArialBold_20
    LabelFace(cap: 15, ascender: 19, perChar: 16.6),   // Arial_B
    LabelFace(cap: 11, ascender: 14, perChar: 12.4),   // Arial_L
]

/// Arial_B, the face the unit caption is set in.
private let kUnitFace = LabelFace(cap: 15, ascender: 19, perChar: 13.5)

struct DashPreview: View {
    let layout: DashLayout

    /// A ride to read the numbers off, for the tutorial's live head unit. The
    /// editor leaves it nil and keeps the fixed samples: someone choosing a
    /// layout is comparing box sizes, and numbers changing under them is noise.
    ///
    /// Only the DRAWN text comes from here. Type sizing still runs off the
    /// samples and the worst-case hints below, so a live panel picks exactly the
    /// faces the editor showed and no value can resize its own cell mid-ride —
    /// which is the device's behaviour, and the whole reason this preview sizes
    /// in two passes.
    var live: RideSim? = nil

    // Device geometry (ui_render.h). Everything is computed in these units.
    private let panelW: CGFloat = 540, panelH: CGFloat = 960
    private let margin: CGFloat = 24, gutter: CGFloat = 12, pad: CGFloat = 16
    private let statusH: CGFloat = 64, step: CGFloat = 12, halfStep: CGFloat = 6
    private let rule: CGFloat = 2
    private var contentW: CGFloat { panelW - 2 * margin }
    private var bodyH: CGFloat { panelH - statusH }

    /// Barlow's cap height is 0.72 em, so this converts a device cap height into
    /// the point size that draws the same-sized capital.
    private let capRatio: CGFloat = 0.72

    private struct Placed {
        var x, y, w, h: CGFloat
        var item: DashItem
        var hero: Bool
        var value: Face
        var label: LabelFace
    }

    var body: some View {
        GeometryReader { geo in
            let k = geo.size.width / panelW
            ZStack(alignment: .topLeading) {
                ForEach(Array(place().enumerated()), id: \.offset) { _, p in
                    cell(p, k: k)
                        .frame(width: p.w * k, height: p.h * k)
                        .offset(x: p.x * k, y: (p.y - statusH) * k)
                }
            }
            .frame(width: geo.size.width, height: bodyH * k, alignment: .topLeading)
        }
        .aspectRatio(panelW / bodyH, contentMode: .fit)
        .background(Color(hex: 0xF7F5EF))
        .foregroundStyle(.black)
    }

    // MARK: layout — ui_render.cpp's packer, verbatim in device pixels

    private func place() -> [Placed] {
        let rows = layout.rows
        guard !rows.isEmpty else { return [] }
        let weights = rows.map { $0.map(\.size.weight).max() ?? 1 }
        let total = max(weights.reduce(0, +), 1)
        let gutters = CGFloat(rows.count - 1) * gutter
        let availH = (panelH - statusH - margin) - gutters
        let halfW = (contentW - gutter) / 2

        var out: [Placed] = []
        var y = statusH + margin - step
        for (r, row) in rows.enumerated() {
            let rowH = r == rows.count - 1 ? panelH - margin - y
                                           : availH * CGFloat(weights[r]) / CGFloat(total)
            for (c, item) in row.enumerated() {
                let w = row.count == 2 ? halfW : contentW
                let x = row.count == 2 ? (c == 0 ? margin : margin + halfW + gutter)
                                       : margin
                // The hero only gets hero treatment when it is alone on a row
                // with the height to carry it, exactly as the device decides.
                let hero = item.size == .hero && row.count == 1 && rowH >= 200
                out.append(Placed(x: x, y: y, w: w, h: rowH, item: item, hero: hero,
                                  value: kValueLadder[kValueLadder.count - 1],
                                  label: kLabelLadder[kLabelLadder.count - 1]))
            }
            y += rowH + gutter
        }
        return sized(out)
    }

    /// Second pass: equalise type across each size class, as the device does.
    /// A cell that sizes itself independently makes identical boxes disagree.
    private func sized(_ input: [Placed]) -> [Placed] {
        var out = input
        var classIdx: [String: Int] = [:]
        var labelIdx = 0

        for p in out where !p.hero {
            let availW = p.w - 2 * pad
            let unitW = unit(p.item.field).map { kUnitFace.perChar * CGFloat($0.count) + 6 } ?? 0
            let valH = p.h - pad * 2 - kLabelLadder[1].ascender - halfStep
            let idx = valueFaceIndex(kValueLadder, hint(p.item.field), availW, valH, unitW)
            let bucket = p.w > contentW * 3 / 4 ? "wide" : "narrow"
            let key = "\(p.item.size.rawValue)-\(bucket)"
            classIdx[key] = max(classIdx[key] ?? 0, idx)

            let li = kLabelLadder.firstIndex { $0.perChar * CGFloat(p.item.panelLabel.count) <= availW }
                ?? kLabelLadder.count - 1
            labelIdx = max(labelIdx, li)
        }

        for i in out.indices {
            out[i].label = kLabelLadder[labelIdx]
            if out[i].hero {
                out[i].value = heroFace(out[i])
            } else {
                let bucket = out[i].w > contentW * 3 / 4 ? "wide" : "narrow"
                out[i].value = kValueLadder[classIdx["\(out[i].item.size.rawValue)-\(bucket)"] ?? 0]
            }
        }
        return out
    }

    private func valueFaceIndex(_ ladder: [Face], _ text: String,
                                _ availW: CGFloat, _ availH: CGFloat,
                                _ unitW: CGFloat) -> Int {
        ladder.firstIndex { $0.cap <= availH && $0.width(text) + unitW <= availW }
            ?? ladder.count - 1
    }

    private func heroFace(_ p: Placed) -> Face {
        let innerW = p.w - 2 * pad
        let unitW = unit(p.item.field).map { kUnitFace.perChar * CGFloat($0.count) + 10 } ?? 0
        let barH = isPower(p.item.field) ? 18 + step : 0
        let top = pad + kLabelLadder[1].ascender + halfStep
        let bot = p.h - pad - barH
        return kHeroLadder.first {
            $0.width(sample(p.item.field)) + unitW <= innerW && $0.cap <= bot - top
        } ?? kHeroLadder[kHeroLadder.count - 1]
    }

    // MARK: drawing

    @ViewBuilder
    private func cell(_ p: Placed, k: CGFloat) -> some View {
        ZStack(alignment: .topLeading) {
            Rectangle()
                .strokeBorder(Color.black, lineWidth: rule * k)

            if p.hero {
                VStack(spacing: 0) {
                    caption(p, k: k)
                        .frame(maxWidth: .infinity)            // hero caption centres
                        .padding(.top, pad * k)
                    value(p, k: k)
                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                    if isPower(p.item.field) {
                        zoneBar(k: k, width: p.w - 2 * pad, filled: zone(p.item.field))
                            .padding(.bottom, pad * k)
                    }
                }
                .padding(.horizontal, pad * k)
            } else {
                caption(p, k: k)
                    .padding(.leading, pad * k)                // grid caption top-left
                    .padding(.top, pad * k)
                value(p, k: k)
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                    .padding(.top, (pad + p.label.ascender) * k)
            }
        }
    }

    private func caption(_ p: Placed, k: CGFloat) -> some View {
        Text(p.item.panelLabel)
            .font(.custom("Barlow-SemiBold", size: p.label.cap / capRatio * k))
            .tracking(p.label.cap * 0.18 * k)
            .lineLimit(1)
    }

    /// Value and unit are ONE object, centred as a pair — centring the number
    /// alone pushes it off-axis by half the unit's width.
    private func value(_ p: Placed, k: CGFloat) -> some View {
        HStack(alignment: .firstTextBaseline, spacing: 5 * k) {
            Text(live?.text(for: p.item.field) ?? sample(p.item.field))
                .font(.custom("BarlowCondensed-Bold", size: p.value.cap / capRatio * k))
                .lineLimit(1)
                .minimumScaleFactor(0.5)
            if let u = unit(p.item.field) {
                Text(u)
                    .font(.custom("Barlow-SemiBold", size: kUnitFace.cap / capRatio * k))
                    .lineLimit(1)
            }
        }
    }

    /// Seven FTP zone segments, filled to the current zone.
    private func zoneBar(k: CGFloat, width: CGFloat, filled: Int) -> some View {
        let segW = (width - 6 * halfStep) / 7
        return HStack(spacing: halfStep * k) {
            ForEach(0..<7, id: \.self) { i in
                Rectangle()
                    .strokeBorder(Color.black, lineWidth: 1 * k)
                    .background(i < filled ? Color.black : Color.clear)
                    .frame(width: segW * k, height: 18 * k)
            }
        }
    }

    /// Which zone the power field is in, against the device's default 250 W FTP
    /// — Coggan's boundaries, the ones ui_render.cpp fills the bar by. The fixed
    /// sample of 247 W sits at 99% of FTP, which is zone 4, so a static preview
    /// looks exactly as it always did.
    private func zone(_ field: String) -> Int {
        let watts = live.map { field == "power" ? $0.power : $0.power3s } ?? 247
        let pct = watts / 250 * 100
        switch pct {
        case ..<55:   return 1
        case ..<75:   return 2
        case ..<90:   return 3
        case ..<105:  return 4
        case ..<120:  return 5
        case ..<150:  return 6
        default:      return 7
        }
    }

    // MARK: field data

    private func isPower(_ f: String) -> Bool { f == "power3s" || f == "power" }

    private func unit(_ f: String) -> String? {
        switch f {
        case "speed": return "KM/H"
        case "power3s", "power": return "W"
        case "hr": return "BPM"
        case "cadence": return "RPM"
        case "distance", "routeleft": return "KM"
        case "climb", "altitude": return "M"
        case "grade", "battery": return "%"
        default: return nil
        }
    }

    /// dashSizingHint(): the WIDEST string a field can produce. The device sizes
    /// type for the worst case so the number never resizes mid-ride, and the
    /// preview has to size for the same string or it shows the wrong face.
    private func hint(_ f: String) -> String {
        switch f {
        case "speed", "grade": return "88.8"
        case "distance", "routeleft": return "888.8"
        case "ridetime", "movingtime": return "88:88:88"
        case "climb", "altitude": return "8888"
        case "sats": return "88"
        case "clock": return "88:88"
        default: return "888"
        }
    }

    /// Plausible live values, so the preview reads like a ride in progress.
    private func sample(_ field: String) -> String {
        switch field {
        case "speed": return "32.4"
        case "power3s", "power": return "247"
        case "hr": return "156"
        case "cadence": return "88"
        case "distance": return "54.8"
        case "ridetime", "movingtime": return "1:47:12"
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
}


// Thumbnail of the device's MUSIC page, same idiom as DashPreview: device
// geometry scaled by width, stylized shapes rather than live content.
struct MusicPreview: View {
    var body: some View {
        GeometryReader { geo in
            let k = geo.size.width / 540
            ZStack(alignment: .topLeading) {
                // Status band rule
                Rectangle().fill(.black)
                    .frame(width: 540 * k, height: 3 * k)
                    .offset(y: 61 * k)
                // Album art frame + vinyl
                Rectangle().stroke(.black, lineWidth: max(1, 2 * k))
                    .frame(width: 324 * k, height: 324 * k)
                    .offset(x: 108 * k, y: 96 * k)
                Circle().stroke(.black, lineWidth: max(1, 2 * k))
                    .frame(width: 208 * k, height: 208 * k)
                    .offset(x: (270 - 104) * k, y: (258 - 104) * k)
                Circle().fill(.black)
                    .frame(width: 68 * k, height: 68 * k)
                    .offset(x: (270 - 34) * k, y: (258 - 34) * k)
                // Volume stepper, right edge
                ForEach(0..<2, id: \.self) { v in
                    Rectangle().stroke(.black, lineWidth: max(1, 2 * k))
                        .frame(width: 76 * k, height: 76 * k)
                        .offset(x: 462 * k, y: (140 + CGFloat(v) * 92) * k)
                }
                // Title / artist bars
                Capsule().fill(.black)
                    .frame(width: 320 * k, height: 26 * k)
                    .offset(x: 110 * k, y: 478 * k)
                Capsule().fill(.black.opacity(0.55))
                    .frame(width: 200 * k, height: 16 * k)
                    .offset(x: 170 * k, y: 534 * k)
                // Progress bar
                Rectangle().stroke(.black, lineWidth: max(1, 2 * k))
                    .frame(width: 492 * k, height: 14 * k)
                    .offset(x: 24 * k, y: 640 * k)
                Rectangle().fill(.black)
                    .frame(width: 220 * k, height: 10 * k)
                    .offset(x: 26 * k, y: 642 * k)
                // Transport row
                ForEach(0..<3, id: \.self) { b in
                    Rectangle().stroke(.black, lineWidth: max(1, 2 * k))
                        .frame(width: [140.0, 188.0, 140.0][b] * k, height: 120 * k)
                        .offset(x: [24.0, 176.0, 376.0][b] * k, y: 780 * k)
                }
                Image(systemName: "playpause.fill")
                    .font(.system(size: 34 * k))
                    .offset(x: 252 * k, y: 822 * k)
            }
            .frame(width: geo.size.width, height: 960 * k, alignment: .topLeading)
        }
        .aspectRatio(540 / 960, contentMode: .fit)
        .background(Color(hex: 0xF7F5EF))
        .foregroundStyle(.black)
    }
}
