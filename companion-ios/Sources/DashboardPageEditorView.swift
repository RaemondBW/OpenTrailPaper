import SwiftUI

// Edit the panel on the panel.
//
// One data page, laid out by touching the picture of it: tap a cell to select,
// drag to move, drop on the left or right half of a full-width cell to pair the
// two, tap the size badge to cycle, drag the radar's grab bar for its height.
// The picture is DashPreview's — the firmware's packer in device pixels — so
// what the rider arranges is what the device will draw, and the chrome this
// screen adds (selection outline, badge, drop targets) is the only thing in
// vermilion; everything inside the panel stays ink on paper.
//
// Pushed from DashboardEditorView when a data-page card is tapped; the
// carousel, its reorder and the other page kinds stay there. Send is in this
// toolbar too so a rider can push without backing out.
//
// Port of Dashboard Editor.dc.html (2a) in the design project.
struct DashboardPageEditorView: View {
    let pageTitle: String
    @Binding var layout: DashLayout
    let canSend: Bool
    let onSend: () -> Void

    @State private var sel: UUID?
    @State private var tray: TrayMode?
    @State private var toast: String?

    private enum TrayMode: Identifiable {
        case add, change
        var id: Self { self }
    }

    private var preview: DashPreview { DashPreview(layout: layout) }

    var body: some View {
        let placement = preview.placement
        let selected = placement.cells.first { $0.item.id == sel }
        VStack(spacing: 0) {
            // THE PANEL. The device's own shape, as large as the screen allows
            // once the inspector has the height it needs.
            ZStack(alignment: .top) {
                PanelCanvas(
                    preview: preview,
                    placement: placement,
                    selected: $sel,
                    onDrop: { id, target in layout.items = applyDrop(layout.items, placement, id, target) },
                    onCycleSize: { id in
                        setItem(id) { it in
                            let all = DashSize.allCases
                            it.size = all[(all.firstIndex(of: it.size)! + 1) % all.count]
                        }
                        sel = id
                    },
                    onRadarHeight: { id, pct in setItem(id) { $0.heightPercent = pct } })
                if let toast {
                    Text(toast)
                        .font(BarlowFont.text(14, .semibold))
                        .foregroundStyle(Palette.surface)
                        .multilineTextAlignment(.center)
                        .padding(.horizontal, 16).padding(.vertical, 12)
                        .background(Palette.ink, in: RoundedRectangle(cornerRadius: 12))
                        .padding(.top, 12)
                        .transition(.opacity)
                }
            }
            .frame(maxHeight: .infinity)
            .padding(.horizontal, 24)
            .padding(.vertical, 6)

            inspector(placement: placement, selected: selected)
                .frame(minHeight: 232)
                .padding(.horizontal, 16)
                .padding(.top, 6)
                .padding(.bottom, 16)
        }
        .background(Palette.paper)
        .navigationTitle(pageTitle)
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .confirmationAction) {
                Button {
                    onSend()
                    withAnimation { toast = "Sent — waiting for the head unit to echo" }
                } label: {
                    Text("Send")
                        .font(BarlowFont.condensed(17, .semibold))
                        .foregroundStyle(Palette.accentInk)
                        .padding(.horizontal, 14).padding(.vertical, 6)
                        .background(canSend ? Palette.accent : Palette.faint.opacity(0.55), in: Capsule())
                }
                .disabled(!canSend)
            }
        }
        // A selection that no longer exists (removed, or replaced by a reset)
        // is dropped rather than left pointing at nothing.
        .onChange(of: layout) {
            if let sel, !layout.items.contains(where: { $0.id == sel }) { self.sel = nil }
        }
        .onChange(of: toast) {
            guard toast != nil else { return }
            Task { @MainActor in
                try? await Task.sleep(for: .seconds(1.8))
                withAnimation { toast = nil }
            }
        }
        .sheet(item: $tray) { mode in
            FieldTray(
                title: mode == .change ? "Change field" : "Add a field",
                allowRadar: mode == .add && placement.cells.allSatisfy { !$0.isRadar }
            ) { id in
                if mode == .change, let sel {
                    setItem(sel) { $0.field = id }
                } else {
                    let it = DashItem(field: id, size: .medium, half: false)
                    layout.items.append(it)
                    sel = it.id
                }
            }
        }
    }

    private func setItem(_ id: UUID, _ f: (inout DashItem) -> Void) {
        guard let i = layout.items.firstIndex(where: { $0.id == id }) else { return }
        f(&layout.items[i])
    }

    // MARK: - the inspector

    @ViewBuilder
    private func inspector(placement: DashPreview.Placement, selected: DashPreview.Placed?) -> some View {
        Card(padding: 14) {
            if let selected {
                if selected.isRadar {
                    radarInspector(placement: placement, item: selected.item)
                } else {
                    cellInspector(placement: placement, cell: selected)
                }
            } else {
                emptyInspector(placement: placement)
            }
        }
    }

    private func emptyInspector(placement: DashPreview.Placement) -> some View {
        VStack(alignment: .leading, spacing: 0) {
            Text("Tap a cell to edit it. Hold and drag to move — drop on the left or right half of a full-width cell to pair them.")
                .font(BarlowFont.text(14)).foregroundStyle(Palette.muted)
            Spacer(minLength: 8)
            HStack {
                Spacer()
                Button("Reset to default") { layout = .deviceDefault; sel = nil }
                    .font(BarlowFont.text(14, .semibold)).foregroundStyle(Palette.muted)
            }
            .padding(.bottom, 8)
            let hasRadar = placement.cells.contains { $0.isRadar }
            let canAdd = layout.items.count < DashLayout.maxItems
            HStack(spacing: 8) {
                Button { tray = .add } label: {
                    Text("Add a field")
                        .font(BarlowFont.condensed(19, .semibold))
                        .foregroundStyle(Palette.accentInk)
                        .frame(maxWidth: .infinity).frame(height: 44)
                        .background(canAdd ? Palette.accent : Palette.faint, in: Capsule())
                }
                .disabled(!canAdd)
                Button {
                    guard !hasRadar else { return }
                    let it = DashItem(field: "radar", size: .medium, half: false, vertical: true)
                    layout.items.append(it)
                    sel = it.id
                } label: {
                    Text(hasRadar ? "Radar added" : "Add radar")
                        .font(BarlowFont.condensed(19, .semibold))
                        .foregroundStyle(hasRadar ? Palette.faint : Palette.ink)
                        .frame(maxWidth: .infinity).frame(height: 44)
                        .overlay(Capsule().strokeBorder(hasRadar ? Palette.hairline : Palette.ink,
                                                        lineWidth: hasRadar ? 1 : 1.5))
                }
                .disabled(hasRadar || !canAdd)
            }
            .buttonStyle(.plain)
        }
    }

    private func headerActions(_ id: UUID) -> some View {
        HStack(spacing: 14) {
            Button("Remove") { layout.items.removeAll { $0.id == id }; sel = nil }
                .foregroundStyle(Palette.accent)
            Button("Done") { sel = nil }
                .foregroundStyle(Palette.muted)
        }
        .font(BarlowFont.text(14, .semibold))
        .buttonStyle(.plain)
    }

    private func cellInspector(placement: DashPreview.Placement, cell: DashPreview.Placed) -> some View {
        let item = cell.item
        let row = placement.rows.first { $0.ids.contains(item.id) }
        let unpaired = placement.isUnpaired(cell)
        return ScrollView(showsIndicators: false) {
            VStack(alignment: .leading, spacing: 0) {
                HStack(alignment: .lastTextBaseline) {
                    Button { tray = .change } label: {
                        HStack(alignment: .lastTextBaseline, spacing: 6) {
                            Text(item.fieldLabel).font(TypeScale.title).foregroundStyle(Palette.ink)
                            Text("Change ›").font(BarlowFont.text(14, .semibold)).foregroundStyle(Palette.accent)
                        }
                    }
                    .buttonStyle(.plain)
                    Spacer()
                    headerActions(item.id)
                }
                HStack(alignment: .lastTextBaseline) {
                    Text("Size").trackedLabel()
                    Spacer()
                    if let row {
                        Text("\(row.weight) OF \(placement.totalWeight) SHARES → \(Int(row.h)) PX")
                            .font(TypeScale.label).tracking(0.6).foregroundStyle(Palette.muted)
                    }
                }
                .padding(.top, 10)
                Picker("Size", selection: binding(item.id, \.size)) {
                    ForEach(DashSize.allCases) { Text($0.label).tag($0) }
                }
                .pickerStyle(.segmented)
                .padding(.top, 4)
                HStack(alignment: .center, spacing: 10) {
                    Picker("Width", selection: binding(item.id, \.half)) {
                        Text("Full row").tag(false)
                        Text("Half").tag(true)
                    }
                    .pickerStyle(.segmented)
                    .frame(width: 170)
                    Text(!item.half ? "Drop another field on this cell’s left or right half to pair them."
                         : unpaired ? "Unpaired — it spans the row. Drag another field beside it."
                         : "Paired with its neighbour.")
                        .font(BarlowFont.text(12.5))
                        .foregroundStyle(item.half && unpaired ? Palette.accent : Palette.muted)
                }
                .padding(.top, 8)
            }
        }
    }

    private func radarInspector(placement: DashPreview.Placement, item: DashItem) -> some View {
        ScrollView(showsIndicators: false) {
            VStack(alignment: .leading, spacing: 0) {
                HStack(alignment: .lastTextBaseline) {
                    Text("Radar").font(TypeScale.title).foregroundStyle(Palette.ink)
                    Text("Varia · right column").font(BarlowFont.text(13, .medium)).foregroundStyle(Palette.muted)
                    Spacer()
                    headerActions(item.id)
                }
                HStack(alignment: .lastTextBaseline) {
                    Text("Height").trackedLabel()
                    Spacer()
                    Text("\(Int(placement.railH)) PX · \(item.bottomAligned ? "FROM BOTTOM" : "FROM TOP")")
                        .font(TypeScale.label).tracking(0.6).foregroundStyle(Palette.muted)
                }
                .padding(.top, 10)
                Picker("Height", selection: binding(item.id, \.heightPercent)) {
                    Text("Half").tag(50)
                    Text("Three-quarter").tag(75)
                    Text("Full").tag(100)
                }
                .pickerStyle(.segmented)
                .padding(.top, 4)
                HStack(alignment: .center, spacing: 10) {
                    Picker("Alignment", selection: binding(item.id, \.bottomAligned)) {
                        Text("Top").tag(false)
                        Text("Bottom").tag(true)
                    }
                    .pickerStyle(.segmented)
                    .frame(width: 150)
                    .disabled(item.heightPercent == 100)
                    Text(item.heightPercent == 100 ? "Full height fills the column either way."
                         : "Bottom lets a full-width hero sit above the tile.")
                        .font(BarlowFont.text(12.5)).foregroundStyle(Palette.muted)
                }
                .padding(.top, 8)
            }
        }
    }

    /// A binding into one item's property, by id — the item may move between
    /// rows under the rider's finger, so it is never addressed by index.
    private func binding<T>(_ id: UUID, _ path: WritableKeyPath<DashItem, T>) -> Binding<T> {
        Binding(
            get: { layout.items.first { $0.id == id }.map { $0[keyPath: path] } ?? layout.items[0][keyPath: path] },
            set: { v in setItem(id) { $0[keyPath: path] = v } })
    }
}

// MARK: - drop targets

/// Where a dragged cell would land. Row indices are into `Placement.rows`.
private enum DropTarget {
    case before(Int), after(Int), left(Int), right(Int)

    var row: Int {
        switch self {
        case .before(let r), .after(let r), .left(let r), .right(let r): return r
        }
    }
}

/// The rule the pointer is judged by, in device pixels. A full-width row
/// offers its left or right half through its middle band and before/after at
/// its edges; a paired row only before/after; above and below the grid are the
/// ends. The row the cell came from is not a target, so a wobble does nothing.
private func targetFor(_ pl: DashPreview.Placement, _ dragged: UUID, _ px: CGFloat, _ py: CGFloat,
                       margin: CGFloat, gutter: CGFloat) -> DropTarget? {
    let rows = pl.rows
    guard let first = rows.first, let last = rows.last else { return nil }
    if py < first.y { return .before(0) }
    if py >= last.y + last.h {
        return last.ids.count == 1 && last.ids[0] == dragged ? nil : .after(rows.count - 1)
    }
    for (r, row) in rows.enumerated() {
        if py < row.y || py >= row.y + row.h + gutter { continue }
        let rel = (py - row.y) / row.h
        let single = row.ids.count == 1
        if single && row.ids[0] == dragged { return nil }
        if single && rel > 0.22 && rel < 0.78 {
            return px < margin + row.mainW / 2 ? .left(r) : .right(r)
        }
        return rel < 0.5 ? .before(r) : .after(r)
    }
    return nil
}

private func applyDrop(_ items: [DashItem], _ pl: DashPreview.Placement, _ dragged: UUID, _ t: DropTarget) -> [DashItem] {
    guard let d = items.first(where: { $0.id == dragged }),
          pl.rows.indices.contains(t.row) else { return items }
    var rest = items.filter { $0.id != dragged }
    let row = pl.rows[t.row]
    var moved = d
    switch t {
    case .left, .right:
        guard let ti = rest.firstIndex(where: { $0.id == row.ids[0] }) else { return items }
        rest[ti].half = true
        moved.half = true
        if case .left = t { rest.insert(moved, at: ti) } else { rest.insert(moved, at: ti + 1) }
    case .before, .after:
        let anchor: UUID
        if case .before = t { anchor = row.ids.first! } else { anchor = row.ids.last! }
        let ai = rest.firstIndex { $0.id == anchor } ?? rest.count - 1
        moved.half = false
        if case .before = t { rest.insert(moved, at: max(ai, 0)) } else { rest.insert(moved, at: ai + 1) }
    }
    return rest
}

// MARK: - the canvas

private struct DragState {
    var id: UUID
    var translation: CGSize = .zero
    var target: DropTarget?
}

private struct PanelCanvas: View {
    let preview: DashPreview
    let placement: DashPreview.Placement
    @Binding var selected: UUID?
    let onDrop: (UUID, DropTarget) -> Void
    let onCycleSize: (UUID) -> Void
    let onRadarHeight: (UUID, Int) -> Void

    @State private var drag: DragState?

    var body: some View {
        GeometryReader { geo in
            let k = geo.size.width / preview.panelW
            ZStack(alignment: .topLeading) {
                // A tap on bare paper clears the selection. Cells sit above and
                // take their own taps first.
                Color(hex: 0xF7F5EF)
                    .contentShape(Rectangle())
                    .onTapGesture { selected = nil }
                ForEach(placement.cells, id: \.item.id) { p in
                    cellView(p, k: k)
                }
                if let t = drag?.target { indicator(t, k: k) }
            }
            .frame(width: geo.size.width, height: preview.bodyH * k, alignment: .topLeading)
            .clipShape(RoundedRectangle(cornerRadius: 8))
            .overlay(RoundedRectangle(cornerRadius: 8).strokeBorder(Palette.ink, lineWidth: 1))
        }
        .aspectRatio(preview.panelW / preview.bodyH, contentMode: .fit)
        .foregroundStyle(.black)
    }

    private func cellView(_ p: DashPreview.Placed, k: CGFloat) -> some View {
        let id = p.item.id
        let isSel = id == selected
        let dragging = drag?.id == id
        let unpaired = placement.isUnpaired(p)
        let zoneBar = p.hero && (p.item.field == "power3s" || p.item.field == "power")
        return ZStack(alignment: .topLeading) {
            preview.cell(p, k: k)
            if isSel {
                Rectangle().strokeBorder(Palette.accent, lineWidth: 2.5)
            }
            if unpaired {
                Path { path in
                    path.move(to: CGPoint(x: p.w * k / 2, y: 6))
                    path.addLine(to: CGPoint(x: p.w * k / 2, y: p.h * k - 6))
                }
                .stroke(Palette.accent, style: StrokeStyle(lineWidth: 1.5, dash: [4, 4]))
                Text("½ UNPAIRED · SPANS")
                    .font(BarlowFont.condensed(10, .semibold)).tracking(0.8)
                    .foregroundStyle(Palette.accent)
                    .frame(maxWidth: .infinity, alignment: .trailing)
                    .padding(.top, 6).padding(.trailing, 8)
            }
            if !p.isRadar {
                // Size badge: tap to cycle. Its own button, so the tap neither
                // selects nor drags the cell underneath.
                Button { onCycleSize(id) } label: {
                    Text(sizeLetter(p.item.size))
                        .font(BarlowFont.condensed(12, .semibold))
                        .foregroundStyle(isSel ? Palette.accentInk : Palette.muted)
                        .frame(width: 20, height: 20)
                        .background(isSel ? Palette.accent : Palette.surface, in: Circle())
                        .overlay(Circle().strokeBorder(isSel ? Palette.accent : Palette.hairline, lineWidth: 1))
                }
                .buttonStyle(.plain)
                .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .bottomTrailing)
                .padding(.trailing, 4)
                .padding(.bottom, zoneBar ? 18 * k + 14 : 4)
            } else if isSel {
                radarRail(p, k: k)
            }
        }
        .frame(width: p.w * k, height: p.h * k)
        .contentShape(Rectangle())
        .offset(x: p.x * k, y: (p.y - preview.statusH) * k)
        .offset(dragging ? (drag?.translation ?? .zero) : .zero)
        .shadow(color: dragging ? Palette.ink.opacity(0.28) : .clear, radius: 12, y: 8)
        .opacity(dragging ? 0.94 : 1)
        .zIndex(dragging ? 20 : isSel ? 5 : 0)
        .onTapGesture { selected = id }
        .gesture(p.isRadar ? nil : DragGesture(minimumDistance: 6)
            .onChanged { v in
                selected = id
                // The finger on the device grid: the cell's origin plus where
                // in the cell it is, in device pixels.
                let px = p.x + v.location.x / k
                let py = p.y + v.location.y / k
                drag = DragState(id: id, translation: v.translation,
                                 target: targetFor(placement, id, px, py,
                                                   margin: preview.margin, gutter: preview.gutter))
            }
            .onEnded { _ in
                if let d = drag, let t = d.target { onDrop(id, t) }
                drag = nil
            })
    }

    /// The radar's grab bar: drag it to snap the tile's height to a half,
    /// three-quarters or the whole column. On the edge that moves — the bottom
    /// of a top-aligned tile.
    private func radarRail(_ p: DashPreview.Placed, k: CGFloat) -> some View {
        let bottom = p.item.bottomAligned
        return Text("↕ " + (p.item.heightPercent == 100 ? "FULL" : p.item.heightPercent == 75 ? "¾" : "HALF"))
            .font(BarlowFont.condensed(12, .semibold)).tracking(1)
            .foregroundStyle(Palette.accentInk)
            .frame(maxWidth: .infinity).frame(height: 22)
            .background(Palette.accent)
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: bottom ? .top : .bottom)
            .contentShape(Rectangle())
            .gesture(DragGesture(minimumDistance: 0)
                .onChanged { v in
                    let barY = bottom ? p.y : p.y + p.h - 22 / k
                    let py = barY + v.location.y / k
                    let top = preview.statusH + preview.margin - preview.step
                    let total = preview.panelH - preview.margin - top
                    let frac = bottom ? (preview.panelH - preview.margin - py) / total : (py - top) / total
                    let snap = frac < 0.625 ? 50 : frac < 0.875 ? 75 : 100
                    if snap != p.item.heightPercent { onRadarHeight(p.item.id, snap) }
                })
    }

    /// A bar between rows, or a dashed half of the row the cell would pair into.
    @ViewBuilder
    private func indicator(_ t: DropTarget, k: CGFloat) -> some View {
        if placement.rows.indices.contains(t.row) {
            let row = placement.rows[t.row]
            switch t {
            case .before, .after:
                let y: CGFloat = { if case .before = t { return row.y - preview.gutter / 2 } else { return row.y + row.h + preview.gutter / 2 } }()
                RoundedRectangle(cornerRadius: 2)
                    .fill(Palette.accent)
                    .frame(width: row.mainW * k, height: 4)
                    .offset(x: preview.margin * k, y: (y - preview.statusH) * k - 2)
                    .zIndex(15)
            case .left, .right:
                let halfW = ((row.mainW - preview.gutter) / 2).rounded(.down)
                let x: CGFloat = { if case .left = t { return preview.margin } else { return preview.margin + halfW + preview.gutter } }()
                Rectangle()
                    .fill(Palette.accent.opacity(0.12))
                    .overlay(Rectangle().strokeBorder(Palette.accent, style: StrokeStyle(lineWidth: 2.5, dash: [8, 6])))
                    .frame(width: halfW * k, height: row.h * k)
                    .offset(x: x * k, y: (row.y - preview.statusH) * k)
                    .zIndex(15)
            }
        }
    }
}

private func sizeLetter(_ s: DashSize) -> String {
    switch s {
    case .small: return "S"
    case .medium: return "M"
    case .large: return "L"
    case .hero: return "H"
    }
}

// MARK: - the field tray

/// A sheet of every field, two to a row.
private struct FieldTray: View {
    @Environment(\.dismiss) private var dismiss
    let title: String
    let allowRadar: Bool
    let pick: (String) -> Void

    private let columns = [GridItem(.flexible(), spacing: 8), GridItem(.flexible(), spacing: 8)]

    var body: some View {
        VStack(spacing: 0) {
            HStack {
                Text(title).font(TypeScale.title).foregroundStyle(Palette.ink)
                Spacer()
                Button("Cancel") { dismiss() }
                    .font(BarlowFont.text(16, .semibold)).foregroundStyle(Palette.accent)
            }
            .padding(.horizontal, 20)
            .frame(height: 48)
            .padding(.top, 12)
            ScrollView {
                LazyVGrid(columns: columns, spacing: 8) {
                    ForEach(DashField.all.filter { $0.id != "radar" || allowRadar }) { f in
                        Button {
                            pick(f.id)
                            dismiss()
                        } label: {
                            VStack(alignment: .leading, spacing: 2) {
                                Text(f.label).font(BarlowFont.text(15, .semibold)).foregroundStyle(Palette.ink)
                                Text(f.detail).font(BarlowFont.text(12)).foregroundStyle(Palette.muted)
                                    .multilineTextAlignment(.leading)
                            }
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .padding(.horizontal, 12).padding(.vertical, 9)
                            .background(Palette.surface, in: RoundedRectangle(cornerRadius: 12))
                            .overlay(RoundedRectangle(cornerRadius: 12).strokeBorder(Palette.hairline, lineWidth: 1))
                        }
                        .buttonStyle(.plain)
                    }
                }
                .padding(.horizontal, 16)
                .padding(.vertical, 6)
            }
        }
        .background(Palette.paper)
        .presentationDetents([.medium, .large])
        .presentationDragIndicator(.visible)
    }
}
