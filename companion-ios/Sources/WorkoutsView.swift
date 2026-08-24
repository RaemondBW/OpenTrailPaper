import SwiftUI
import UniformTypeIdentifiers

// Structured workouts: what's loaded on the head unit, the session's live
// state, and the ways to get a workout onto the device — import an .erg/.mrc
// export, or build/edit one here block by block. Everything speaks
// CHR_WORKOUT; the device stores files in /workouts on its card.

// Workouts saved on the phone (Documents/workouts). The phone is the
// library; the device holds copies. Everything created or imported lands
// here FIRST, so nothing depends on the BLE link being up.
enum LocalWorkouts {
    static var dir: URL {
        let d = FileManager.default.urls(for: .documentDirectory,
                                         in: .userDomainMask)[0]
            .appendingPathComponent("workouts", isDirectory: true)
        try? FileManager.default.createDirectory(at: d,
                                                 withIntermediateDirectories: true)
        return d
    }
    static func list() -> [String] {
        ((try? FileManager.default.contentsOfDirectory(atPath: dir.path)) ?? [])
            .filter { $0.lowercased().hasSuffix(".mrc") || $0.lowercased().hasSuffix(".erg") }
            .sorted()
    }
    static func read(_ name: String) -> String? {
        try? String(contentsOf: dir.appendingPathComponent(name), encoding: .utf8)
    }
    static func save(_ name: String, _ text: String) {
        try? text.write(to: dir.appendingPathComponent(name),
                        atomically: true, encoding: .utf8)
    }
    static func delete(_ name: String) {
        try? FileManager.default.removeItem(at: dir.appendingPathComponent(name))
    }
}

struct WorkoutsView: View {
    @EnvironmentObject var ble: BLEManager
    @Environment(\.dismiss) private var dismiss
    @State private var showImporter = false
    @State private var showBuilder = false
    @State private var editSeed: WorkoutBuilderView.Seed?
    @State private var localWorkouts: [String] = []
    @State private var libraryExpanded = true

    var body: some View {
        NavigationStack {
            List {
                sessionSection
                librarySection
                addSection
            }
            .scrollContentBackground(.hidden)
            .background(Palette.paper.ignoresSafeArea())
            .navigationTitle("Workouts")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Done") { dismiss() }
                }
            }
            .onAppear {
                ble.refreshWorkouts()
                localWorkouts = LocalWorkouts.list()
            }
            .onChange(of: showBuilder) { _, open in
                if !open { localWorkouts = LocalWorkouts.list() }
            }
            .onChange(of: editSeed == nil) { _, _ in
                localWorkouts = LocalWorkouts.list()
            }
            .fileImporter(isPresented: $showImporter,
                          allowedContentTypes: [.data, .plainText],
                          allowsMultipleSelection: false) { result in
                importFiles(result)
            }
            .sheet(isPresented: $showBuilder) { WorkoutBuilderView() }
            .sheet(item: $editSeed) { seed in WorkoutBuilderView(seed: seed) }
            .onChange(of: ble.fetchedWorkout) { _, fetched in
                guard let f = fetched else { return }
                ble.fetchedWorkout = nil
                if let seed = WorkoutBuilderView.Seed(file: f.name, text: f.text,
                                                     ftp: ble.ftpWatts) {
                    editSeed = seed
                } else {
                    ble.workoutMessage = "Couldn't parse \(f.name)"
                }
            }
            .overlay(alignment: .bottom) {
                if let msg = ble.workoutMessage {
                    Text(msg)
                        .font(TypeScale.body)
                        .padding(.horizontal, 14).padding(.vertical, 8)
                        .background(.thinMaterial, in: Capsule())
                        .padding(.bottom, 12)
                        .task {
                            try? await Task.sleep(nanoseconds: 2_500_000_000)
                            ble.workoutMessage = nil
                        }
                }
            }
        }
    }

    // MARK: live session

    /// With nothing live the page IS the picker, so this section only
    /// exists while a workout is loaded.
    @ViewBuilder
    private var sessionSection: some View {
        let s = ble.workoutStatus
        if s.loaded {
            Section {
                VStack(alignment: .leading, spacing: 8) {
                    HStack {
                        Text(s.name).font(BarlowFont.text(17, .semibold))
                        Spacer()
                        Text(s.done ? "DONE"
                             : s.running ? "RUNNING"
                             : s.paused ? "PAUSED" : "READY")
                            .font(BarlowFont.text(12, .bold))
                            .foregroundStyle(s.running ? .green : Palette.muted)
                    }
                    HStack {
                        Text("Block \(s.blockIndex + 1) of \(s.blockCount)")
                        Spacer()
                        Text("Target \(s.targetW) W")
                    }
                    .font(TypeScale.body).foregroundStyle(Palette.muted)
                    ProgressView(value: Double(s.elapsedSec),
                                 total: Double(max(1, s.totalSec)))
                    HStack {
                        Text(clock(s.elapsedSec))
                        Spacer()
                        Text("-" + clock(s.totalSec &- min(s.elapsedSec, s.totalSec)))
                    }
                    .font(BarlowFont.text(12, .medium).monospacedDigit())
                    .foregroundStyle(Palette.muted)
                }
                .padding(.vertical, 2)

                Toggle(isOn: Binding(
                    get: { s.pauseEachBlock },
                    set: { ble.setWorkoutPauseEachBlock($0) })) {
                    VStack(alignment: .leading, spacing: 2) {
                        Text("Pause after every block")
                            .font(BarlowFont.text(16, .medium))
                        Text("Holds at each boundary until you resume")
                            .font(BarlowFont.text(13)).foregroundStyle(Palette.muted)
                    }
                }
                .disabled(ble.state != .connected)

                HStack(spacing: 10) {
                    if s.running {
                        controlButton("pause.fill", "Pause", prominent: true) { ble.workoutPause() }
                    } else if s.paused {
                        controlButton("play.fill", "Resume", prominent: true) { ble.workoutResume() }
                    } else {
                        controlButton("play.fill", "Start", prominent: true) { ble.workoutStart() }
                    }
                    controlButton("forward.end.fill", "Skip") { ble.workoutSkip() }
                        .disabled(!s.running && !s.paused)
                        .opacity(s.running || s.paused ? 1 : 0.4)
                    controlButton("stop.fill", "Stop") { ble.workoutUnload() }
                }
                .padding(.vertical, 2)
            } header: {
                Text("SESSION").font(TypeScale.label).foregroundStyle(Palette.muted)
            }
        }
    }

    // The app's own button language, not the system blue: the one "go"
    // action is filled accent, the rest are outlined ink.
    private func controlButton(_ icon: String, _ label: String,
                               prominent: Bool = false,
                               action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Label(label, systemImage: icon)
                .font(BarlowFont.text(15, .semibold))
                .foregroundStyle(prominent ? Palette.accentInk : Palette.ink)
                .frame(maxWidth: .infinity)
                .frame(height: 40)
                .background(prominent ? Palette.accent : .clear,
                            in: RoundedRectangle(cornerRadius: 10))
                .overlay {
                    if !prominent {
                        RoundedRectangle(cornerRadius: 10)
                            .strokeBorder(Palette.hairline, lineWidth: 1.5)
                    }
                }
        }
        .buttonStyle(.plain)
    }

    // MARK: the library — phone and device as one list

    /// One row per workout name, wherever it lives. The phone copy is the
    /// editable master; the device copy is what the head unit can ride.
    private struct Item: Identifiable {
        let name: String
        let onPhone: Bool
        let onDevice: Bool
        var id: String { name }
    }

    private var items: [Item] {
        let phone = Set(localWorkouts)
        let device = Set(ble.deviceWorkouts)
        return phone.union(device).sorted().map {
            Item(name: $0, onPhone: phone.contains($0), onDevice: device.contains($0))
        }
    }

    private var librarySection: some View {
        Section {
            DisclosureGroup(isExpanded: $libraryExpanded) {
                if items.isEmpty {
                    Text("Workouts you create or import appear here.")
                        .font(TypeScale.body).foregroundStyle(Palette.muted)
                }
                ForEach(items) { item in
                    libraryRow(item)
                }
            } label: {
                HStack {
                    Text(pickerMode ? "Pick a workout" : "Workouts")
                        .font(BarlowFont.text(17, .semibold))
                    Spacer()
                    Text("\(items.count)")
                        .font(BarlowFont.text(13, .medium))
                        .foregroundStyle(Palette.muted)
                }
            }
        } footer: {
            Text(pickerMode
                 ? "Tap a workout to load it on the device. Swipe for edit and delete."
                 : "Tap to edit. The arrow sends it to the device and loads it; phone copies are the editable masters.")
                .font(BarlowFont.text(13))
        }
    }

    private var pickerMode: Bool { !ble.workoutStatus.loaded }

    private func libraryRow(_ item: Item) -> some View {
        HStack(spacing: 10) {
            // With no live workout the page is a PICKER: tapping a row loads
            // it on the device. With a session live, tapping edits — the
            // arrow (or swipe) still loads/sends either way.
            Button {
                if pickerMode { sendOrLoad(item) } else { edit(item) }
            } label: {
                HStack(spacing: 10) {
                    Image(systemName: "figure.outdoor.cycle")
                        .foregroundStyle(Palette.muted)
                    VStack(alignment: .leading, spacing: 2) {
                        Text(item.name)
                            .font(BarlowFont.text(16, .semibold))
                            .foregroundStyle(.primary)
                        HStack(spacing: 6) {
                            if item.onPhone {
                                Label("Phone", systemImage: "iphone")
                            }
                            if item.onDevice {
                                Label("Device", systemImage: "antenna.radiowaves.left.and.right")
                            }
                            if isLoaded(item.name) {
                                let s = ble.workoutStatus
                                Text(s.done ? "DONE"
                                     : s.running ? "RUNNING"
                                     : s.paused ? "PAUSED" : "LOADED")
                                    .fontWeight(.bold)
                                    .foregroundStyle(s.running ? .green : Palette.accent)
                            }
                        }
                        .font(BarlowFont.text(12, .medium))
                        .foregroundStyle(Palette.muted)
                        .labelStyle(.titleAndIcon)
                    }
                }
            }
            .buttonStyle(.plain)
            Spacer()
            // Send/load: onto the device and loaded, ready to ride.
            Button {
                sendOrLoad(item)
            } label: {
                Image(systemName: isLoaded(item.name)
                      ? "checkmark.circle.fill" : "arrow.up.circle")
                    .font(.system(size: 22))
                    .foregroundStyle(isLoaded(item.name) ? .green : Palette.accent)
            }
            .buttonStyle(.plain)
            .disabled(ble.state != .connected)
        }
        .swipeActions {
            Button(role: .destructive) {
                if item.onPhone {
                    LocalWorkouts.delete(item.name)
                    localWorkouts = LocalWorkouts.list()
                }
                if item.onDevice { ble.deleteWorkout(item.name) }
            } label: { Label("Delete", systemImage: "trash") }
            Button { edit(item) } label: { Label("Edit", systemImage: "pencil") }
                .tint(.blue)
        }
    }

    private func edit(_ item: Item) {
        if item.onPhone, let text = LocalWorkouts.read(item.name) {
            if let seed = WorkoutBuilderView.Seed(file: item.name, text: text,
                                                 ftp: ble.ftpWatts) {
                editSeed = seed
            } else {
                ble.workoutMessage = "Couldn't parse \(item.name)"
            }
        } else {
            ble.fetchWorkout(item.name)   // editor opens when the file arrives
        }
    }

    private func sendOrLoad(_ item: Item) {
        if item.onPhone, let text = LocalWorkouts.read(item.name) {
            ble.uploadWorkout(name: item.name, text: text)   // saves + loads
            ble.workoutMessage = "Sending \(item.name)…"
        } else if item.onDevice {
            ble.loadWorkout(item.name)
        }
    }

    // The device page shows the name uppercased without its extension;
    // match on that so the checkmark lands on the right row.
    private func isLoaded(_ file: String) -> Bool {
        let stem = file.split(separator: ".").dropLast().joined(separator: ".")
        let base = stem.isEmpty ? file : stem
        return ble.workoutStatus.loaded &&
               ble.workoutStatus.name.caseInsensitiveCompare(base) == .orderedSame
    }

    private var addSection: some View {
        Section {
            Button {
                showBuilder = true
            } label: {
                Label {
                    Text("Create a workout")
                        .font(BarlowFont.text(16, .medium))
                        .foregroundStyle(Palette.ink)
                } icon: {
                    Image(systemName: "plus.circle")
                        .foregroundStyle(Palette.accent)
                }
            }
            .buttonStyle(.plain)
            Button {
                showImporter = true
            } label: {
                Label {
                    Text("Import .erg / .mrc file")
                        .font(BarlowFont.text(16, .medium))
                        .foregroundStyle(Palette.ink)
                } icon: {
                    Image(systemName: "square.and.arrow.down")
                        .foregroundStyle(Palette.accent)
                }
            }
            .buttonStyle(.plain)
        } header: {
            Text("ADD A WORKOUT").font(TypeScale.label).foregroundStyle(Palette.muted)
        }
    }

    private func importFiles(_ result: Result<[URL], Error>) {
        guard case .success(let urls) = result, let url = urls.first else { return }
        let secured = url.startAccessingSecurityScopedResource()
        defer { if secured { url.stopAccessingSecurityScopedResource() } }
        guard let text = try? String(contentsOf: url, encoding: .utf8) else {
            ble.workoutMessage = "Couldn't read that file"
            return
        }
        let ext = url.pathExtension.lowercased()
        guard ext == "erg" || ext == "mrc" || text.contains("[COURSE DATA]") else {
            ble.workoutMessage = "Not an .erg/.mrc workout"
            return
        }
        LocalWorkouts.save(url.lastPathComponent, text)
        localWorkouts = LocalWorkouts.list()
        ble.uploadWorkout(name: url.lastPathComponent, text: text)
        ble.workoutMessage = "Sending \(url.lastPathComponent)…"
    }

    private func clock(_ sec: UInt32) -> String {
        String(format: "%d:%02d", sec / 60, sec % 60)
    }
}

// MARK: - Builder

// The workout editor, taking its structure from the Claude Design "Workout
// Editor" frame — collapsed rows showing only what defines a block, ONE row
// open at a time with the zone strip and destructive action beneath it, a
// profile chart that is a control (tap a bar to open its block), and orange
// reserved for commit and the open block — dressed in the app's native
// rounded-card iOS style rather than the frame's squared chrome.
struct WorkoutBuilderView: View {
    @EnvironmentObject var ble: BLEManager
    @Environment(\.dismiss) private var dismiss

    static let zones: [(name: String, pct: Int)] = [
        ("Recovery", 50), ("Endurance", 65), ("Tempo", 80), ("Threshold", 100),
        ("VO2 max", 115), ("Anaerobic", 135), ("Sprint", 170),
    ]
    static func zone(forPct pct: Int) -> Int {
        pct <= 55 ? 1 : pct <= 75 ? 2 : pct <= 90 ? 3 : pct <= 105 ? 4
            : pct <= 120 ? 5 : pct <= 150 ? 6 : 7
    }

    struct Block: Identifiable {
        let id = UUID()
        var minutes: Double
        var zone: Int          // 1...7
        var pct: Int { WorkoutBuilderView.zones[zone - 1].pct }
    }

    /// A workout pulled off the device or the phone library, parsed into the
    /// builder's blocks. ERG (watts) files convert to %FTP with the device's
    /// FTP; ramps flatten to their average — the builder speaks flat blocks.
    struct Seed: Identifiable {
        let id = UUID()
        let file: String        // original filename — Send overwrites it
        let name: String
        let blocks: [Block]

        init?(file: String, text: String, ftp: Int) {
            let ftpW = ftp > 0 ? ftp : 250
            var percent = false
            var inData = false
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
                    if tok.count >= 2, let m = Double(tok[0]), let v = Double(tok[1]) {
                        pts.append((m, v))
                    }
                }
            }
            guard pts.count >= 2 else { return nil }
            var out: [Block] = []
            for i in 1..<pts.count {
                let dur = pts[i].0 - pts[i - 1].0
                guard dur > 0.001 else { continue }
                let avg = (pts[i - 1].1 + pts[i].1) / 2
                let pct = percent ? avg : avg * 100 / Double(ftpW)
                out.append(Block(minutes: (dur * 2).rounded() / 2,
                                 zone: WorkoutBuilderView.zone(forPct: Int(pct.rounded()))))
            }
            guard !out.isEmpty else { return nil }
            self.file = file
            let stem = file.split(separator: ".").dropLast().joined(separator: ".")
            self.name = (stem.isEmpty ? file : stem).replacingOccurrences(of: "_", with: " ")
            self.blocks = out
        }
    }

    private let editingFile: String?

    @State private var name: String
    @State private var blocks: [Block]
    @State private var openId: UUID?
    // True while the name field is visible; when it scrolls off, the nav bar
    // title becomes the workout's name.
    @State private var nameInView = true

    private struct NameTopKey: PreferenceKey {
        static var defaultValue: CGFloat = 0
        static func reduce(value: inout CGFloat, nextValue: () -> CGFloat) {
            value = nextValue()
        }
    }

    init(seed: Seed? = nil) {
        editingFile = seed?.file
        _name = State(initialValue: seed?.name ?? "My Workout")
        let initial = seed?.blocks ?? [
            Block(minutes: 10, zone: 2),
            Block(minutes: 5, zone: 4),
            Block(minutes: 3, zone: 1),
            Block(minutes: 5, zone: 4),
            Block(minutes: 5, zone: 1),
        ]
        _blocks = State(initialValue: initial)
        _openId = State(initialValue: initial.first?.id)
    }

    private var totalMin: Double { blocks.reduce(0) { $0 + $1.minutes } }
    private var ftpW: Int { ble.ftpWatts > 0 ? ble.ftpWatts : 250 }
    private func watts(_ b: Block) -> Int { (ftpW * b.pct / 100 / 5) * 5 }
    private func fmtDur(_ m: Double) -> String {
        let sec = Int((m * 60).rounded())
        return "\(sec / 60):" + String(format: "%02d", sec % 60)
    }

    var body: some View {
        NavigationStack {
            ScrollViewReader { proxy in
                ScrollView {
                    // The chart is a pinned section header: the name scrolls
                    // away (and into the nav title), the blocks scroll under
                    // the chart, and the chart never leaves the screen — it
                    // is the map of what you're editing.
                    LazyVStack(spacing: 14, pinnedViews: [.sectionHeaders]) {
                        nameField
                            .background(GeometryReader { g in
                                Color.clear.preference(
                                    key: NameTopKey.self,
                                    value: g.frame(in: .named("wkScroll")).maxY)
                            })
                        Section {
                            blockList
                            bottomBar
                        } header: {
                            chartCard
                                .padding(.bottom, 6)
                                .background(Palette.paper)
                        }
                    }
                    .padding(16)
                }
                .coordinateSpace(name: "wkScroll")
                .onPreferenceChange(NameTopKey.self) { maxY in
                    let visible = maxY > 8
                    if visible != nameInView {
                        withAnimation(.easeInOut(duration: 0.15)) {
                            nameInView = visible
                        }
                    }
                }
                .onChange(of: openId) { _, id in
                    if let id { withAnimation { proxy.scrollTo(id, anchor: .center) } }
                }
            }
            .background(Palette.paper.ignoresSafeArea())
            .navigationTitle(nameInView
                             ? (editingFile == nil ? "New Workout" : "Edit Workout")
                             : name)
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") { dismiss() }
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Send") { send() }
                        .fontWeight(.semibold)
                        .disabled(blocks.isEmpty ||
                                  name.trimmingCharacters(in: .whitespaces).isEmpty)
                }
            }
        }
    }

    private var nameField: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text("WORKOUT NAME")
                .font(TypeScale.label).kerning(1.2).foregroundStyle(Palette.muted)
            TextField("Name", text: $name)
                .font(BarlowFont.condensed(30, .bold))
                .foregroundStyle(Palette.ink)
        }
        .padding(14)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(Palette.surface, in: RoundedRectangle(cornerRadius: 12))
    }

    // The profile is a control: tap a bar to open that block's row. Height is
    // intensity, the open block is orange, hard blocks (>=95% FTP) are ink —
    // the device page's own encoding, in the app's rounded card.
    private var chartCard: some View {
        let maxPct = max(blocks.map(\.pct).max() ?? 100, 100)
        return VStack(spacing: 8) {
            GeometryReader { geo in
                let gaps = 2.0 * Double(max(0, blocks.count - 1))
                let unit = (geo.size.width - gaps) / max(1, totalMin)
                HStack(alignment: .bottom, spacing: 2) {
                    ForEach(blocks) { b in
                        let h = 14 + 56 * Double(b.pct) / Double(maxPct)
                        UnevenRoundedRectangle(topLeadingRadius: 3,
                                               topTrailingRadius: 3)
                            .fill(b.id == openId ? Palette.accent
                                  : b.pct >= 95 ? Palette.ink
                                  : Palette.hairline)
                            .frame(width: max(4, unit * b.minutes), height: h)
                            .contentShape(Rectangle())
                            .onTapGesture { openId = b.id }
                    }
                }
                .frame(maxHeight: .infinity, alignment: .bottom)
            }
            .frame(height: 74)
            HStack {
                Text("\(blocks.count) BLOCKS · \(Int(totalMin.rounded())) MIN")
                Spacer()
                Text("FTP \(ftpW) W")
            }
            .font(TypeScale.label).kerning(1.0)
            .foregroundStyle(Palette.muted)
        }
        .padding(14)
        .background(Palette.surface, in: RoundedRectangle(cornerRadius: 12))
    }

    private var blockList: some View {
        VStack(spacing: 0) {
            ForEach($blocks) { $b in
                blockRow($b)
                    .id(b.id)
                if b.id != blocks.last?.id {
                    Divider().overlay(Palette.hairline).padding(.leading, 16)
                }
            }
        }
        .background(Palette.surface)
        .clipShape(RoundedRectangle(cornerRadius: 12))
    }

    private func blockRow(_ b: Binding<Block>) -> some View {
        let block = b.wrappedValue
        let isOpen = block.id == openId
        let zd = Self.zones[block.zone - 1]
        return VStack(spacing: 0) {
            // Collapsed face: chip / name+power / duration / caret.
            Button {
                withAnimation(.easeInOut(duration: 0.18)) {
                    openId = isOpen ? nil : block.id
                }
            } label: {
                HStack(spacing: 12) {
                    Text("Z\(block.zone)")
                        .font(BarlowFont.text(15, .semibold))
                        .frame(width: 42, height: 42)
                        .background(zd.pct >= 95 ? Palette.ink : Palette.paper,
                                    in: RoundedRectangle(cornerRadius: 9))
                        .foregroundStyle(zd.pct >= 95 ? .white : Palette.ink)
                    VStack(alignment: .leading, spacing: 2) {
                        Text(zd.name)
                            .font(BarlowFont.text(17, .semibold))
                            .foregroundStyle(Palette.ink)
                        Text("\(watts(block)) W · \(zd.pct)% FTP")
                            .font(BarlowFont.text(13, .medium))
                            .foregroundStyle(Palette.muted)
                    }
                    Spacer()
                    Text(fmtDur(block.minutes))
                        .font(BarlowFont.condensed(26, .bold))
                        .foregroundStyle(Palette.ink)
                    Image(systemName: isOpen ? "chevron.up" : "chevron.down")
                        .font(.system(size: 12, weight: .semibold))
                        .foregroundStyle(Palette.faint)
                }
                .padding(.horizontal, 16)
                .frame(height: 70)
                // The whole row is the tap target — without an explicit
                // content shape, the clear space around the Spacer ignores
                // touches and only the text/chip responds.
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)

            if isOpen {
                VStack(alignment: .leading, spacing: 12) {
                    HStack(spacing: 4) {
                        ForEach(1...7, id: \.self) { z in
                            Button {
                                b.wrappedValue.zone = z
                            } label: {
                                Text("Z\(z)")
                                    .font(BarlowFont.text(14, .semibold))
                                    .frame(maxWidth: .infinity)
                                    .frame(height: 42)
                                    .background(z == block.zone ? Palette.ink : Palette.paper,
                                                in: RoundedRectangle(cornerRadius: 9))
                                    .foregroundStyle(z == block.zone ? .white : Palette.muted)
                            }
                            .buttonStyle(.plain)
                        }
                    }
                    HStack(spacing: 10) {
                        HStack(spacing: 0) {
                            stepBtn("minus") {
                                b.wrappedValue.minutes = max(1, block.minutes - 1)
                            }
                            Text(fmtDur(block.minutes))
                                .font(BarlowFont.condensed(22, .bold))
                                .foregroundStyle(Palette.ink)
                                .frame(maxWidth: .infinity)
                            stepBtn("plus") {
                                b.wrappedValue.minutes = min(60, block.minutes + 1)
                            }
                        }
                        .background(Palette.paper, in: RoundedRectangle(cornerRadius: 10))
                        actionBtn("Copy", color: Palette.ink) {
                            if let i = blocks.firstIndex(where: { $0.id == block.id }) {
                                let dup = Block(minutes: block.minutes,
                                                zone: block.zone)
                                blocks.insert(dup, at: i + 1)
                                openId = dup.id
                            }
                        }
                        actionBtn("Delete", color: Palette.accent) {
                            withAnimation {
                                blocks.removeAll { $0.id == block.id }
                                openId = nil
                            }
                        }
                    }
                }
                .padding(.horizontal, 16).padding(.bottom, 14)
            }
        }
        .background(isOpen ? Color.white : .clear)
        .overlay(alignment: .leading) {
            if isOpen {
                UnevenRoundedRectangle(topLeadingRadius: 3, bottomLeadingRadius: 3)
                    .fill(Palette.accent)
                    .frame(width: 5)
            }
        }
    }

    private func stepBtn(_ icon: String, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Image(systemName: icon)
                .font(.system(size: 17, weight: .bold))
                .foregroundStyle(Palette.ink)
                .frame(width: 46, height: 44)
        }
        .buttonStyle(.plain)
    }

    private func actionBtn(_ label: String, color: Color,
                           action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Text(label)
                .font(BarlowFont.text(14, .semibold))
                .foregroundStyle(color)
                .frame(width: 76, height: 44)
                .overlay(RoundedRectangle(cornerRadius: 10)
                    .strokeBorder(color.opacity(0.6), lineWidth: 1.5))
        }
        .buttonStyle(.plain)
    }

    private var bottomBar: some View {
        HStack(spacing: 10) {
            bottomBtn("Add block") {
                let nb = Block(minutes: 5, zone: 4)
                blocks.append(nb)
                openId = nb.id
            }
            bottomBtn("Recovery") {
                let nb = Block(minutes: 3, zone: 1)
                blocks.append(nb)
                openId = nb.id
            }
        }
    }

    private func bottomBtn(_ label: String,
                           action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Label(label, systemImage: "plus")
                .font(BarlowFont.text(15, .semibold))
                .foregroundStyle(Palette.ink)
                .frame(maxWidth: .infinity)
                .frame(height: 48)
                .background(Palette.surface, in: RoundedRectangle(cornerRadius: 12))
        }
        .buttonStyle(.plain)
    }

    // Serialize to .mrc: [COURSE DATA] pairs of (minutes, %FTP), each block a
    // flat step — byte format the device's workoutParse() already reads.
    private func send() {
        let safe = name.trimmingCharacters(in: .whitespaces)
            .replacingOccurrences(of: " ", with: "_")
            .filter { $0.isLetter || $0.isNumber || $0 == "_" || $0 == "-" }
        // Editing keeps the original filename so Send is an overwrite, not a
        // sibling — even if the display name was reworded.
        let file = editingFile ?? ((safe.isEmpty ? "workout" : safe.lowercased()) + ".mrc")
        var s = "[COURSE HEADER]\nVERSION = 2\nUNITS = ENGLISH\n"
        s += "DESCRIPTION = \(name)\nFILE NAME = \(file)\n"
        s += "MINUTES PERCENT\n[END COURSE HEADER]\n[COURSE DATA]\n"
        var t = 0.0
        for b in blocks {
            s += String(format: "%.2f %d\n", t, b.pct)
            t += b.minutes
            s += String(format: "%.2f %d\n", t, b.pct)
        }
        s += "[END COURSE DATA]\n"
        // The phone keeps the master copy either way; the device gets it now
        // if it's connected, or from the library row later.
        LocalWorkouts.save(file, s)
        if ble.state == .connected {
            ble.uploadWorkout(name: file, text: s)
            ble.workoutMessage = "Saved on phone — sending \(file)…"
        } else {
            ble.workoutMessage = "Saved on phone — device not connected"
        }
        dismiss()
    }
}
