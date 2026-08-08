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

    @State private var layout = DashLayout(items: [])
    @State private var showAdd = false
    @State private var dirty = false

    var body: some View {
        NavigationStack {
            Group {
                if ble.dashLayout == nil {
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
                        ble.sendDashLayout(layout)
                        dirty = false
                    }
                    .disabled(!dirty || layout.items.isEmpty)
                }
            }
            .onAppear { if let l = ble.dashLayout { layout = l } }
            // The device is the source of truth: if it corrects or rejects what
            // we sent, adopt what it actually holds rather than keeping a local
            // fiction on screen. Skipped while the rider has unsent edits, so a
            // stray notify can't wipe work in progress.
            .onChange(of: ble.dashLayout) {
                if !dirty, let l = ble.dashLayout { layout = l }
            }
            .sheet(isPresented: $showAdd) {
                FieldPicker { id in
                    layout.items.append(DashItem(field: id, size: .medium, half: true))
                    dirty = true
                }
            }
        }
    }

    private var editor: some View {
        List {
            Section {
                DashPreview(layout: layout)
                    .frame(height: 320)
                    .frame(maxWidth: .infinity)
                    .listRowInsets(EdgeInsets(top: 12, leading: 0, bottom: 12, trailing: 0))
                    .listRowBackground(Color.clear)
            } header: {
                Text("How the panel will look")
            }

            Section {
                ForEach($layout.items) { $item in
                    DashItemRow(item: $item) { dirty = true }
                }
                .onMove { from, to in
                    layout.items.move(fromOffsets: from, toOffset: to)
                    dirty = true
                }
                .onDelete { idx in
                    layout.items.remove(atOffsets: idx)
                    dirty = true
                }
                if layout.items.count < DashLayout.maxItems {
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
                    layout = .deviceDefault
                    dirty = true
                }
            }
        }
        .environment(\.editMode, .constant(.active))
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
// It had drifted badly: centred captions, no cell borders, no margins, and a
// value size picked per cell. The device now draws bordered cells inset on a
// 24 px margin with a 12 px gutter, captions top-left at ONE size for the whole
// screen, and the value+unit centred as a pair with its size scaling by cell.
// A preview that does not match is worse than none — it is what the rider is
// deciding against.
struct DashPreview: View {
    let layout: DashLayout

    // Device geometry, in device pixels; everything below scales from these.
    private let panelW: CGFloat = 540, panelH: CGFloat = 960
    private let margin: CGFloat = 24, gutter: CGFloat = 12, pad: CGFloat = 16
    private let statusH: CGFloat = 64

    var body: some View {
        GeometryReader { geo in
            let k = geo.size.width / panelW           // one scale factor
            let rows = layout.rows
            let total = max(rows.reduce(0) { $0 + rowWeight($1) }, 1)
            let bodyH = panelH - statusH - margin
            let gaps = CGFloat(max(rows.count - 1, 0)) * gutter
            let avail = bodyH - gaps

            // One caption size for the whole screen, as the device does.
            let capSize = captionSize()

            VStack(spacing: gutter * k) {
                ForEach(Array(rows.enumerated()), id: \.offset) { _, row in
                    let h = avail * CGFloat(rowWeight(row)) / CGFloat(total)
                    HStack(spacing: gutter * k) {
                        ForEach(row) { item in
                            cell(item, k: k,
                                 w: row.count == 2 ? (panelW - 2 * margin - gutter) / 2
                                                   : panelW - 2 * margin,
                                 h: h, capSize: capSize)
                        }
                    }
                    .frame(height: h * k)
                }
            }
            .frame(width: geo.size.width, alignment: .top)
        }
        .aspectRatio(panelW / (panelH - statusH), contentMode: .fit)
    }

    private func rowWeight(_ row: [DashItem]) -> Int { row.map(\.size.weight).max() ?? 1 }

    /// Largest caption that fits every label — the device picks one size for all.
    private func captionSize() -> CGFloat {
        let longest = layout.items.map(\.fieldLabel.count).max() ?? 0
        return longest > 10 ? 9 : (longest > 7 ? 10 : 11)
    }

    private func cell(_ item: DashItem, k: CGFloat, w: CGFloat, h: CGFloat,
                      capSize: CGFloat) -> some View {
        // Value size scales with the cell, then clamps to the width — the same
        // two constraints the firmware's ladder applies.
        let byHeight = (h - pad * 2 - 22) * 0.62
        let byWidth = w / CGFloat(max(sample(item.field).count, 1)) * 1.5
        let vSize = max(min(byHeight, byWidth), 11)

        return ZStack(alignment: .topLeading) {
            Rectangle().stroke(Color.black, lineWidth: 2 * k)
            Text(item.fieldLabel.uppercased())
                .font(.system(size: capSize, weight: .bold))
                .tracking(capSize * 0.16)
                .lineLimit(1)
                .padding(.leading, pad * k)
                .padding(.top, pad * k)
            HStack(alignment: .lastTextBaseline, spacing: 2 * k) {
                Text(sample(item.field))
                    .font(.system(size: vSize * k, weight: .heavy))
                    .lineLimit(1).minimumScaleFactor(0.4)
                if let u = unit(item.field) {
                    Text(u).font(.system(size: capSize * 0.85, weight: .bold))
                }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)   // centred pair
        }
        .frame(width: w * k, height: h * k)
        .background(Color(hex: 0xF7F5EF))
        .foregroundStyle(.black)
    }

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

    // Plausible values: real digit counts drive the size the device will pick.
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
