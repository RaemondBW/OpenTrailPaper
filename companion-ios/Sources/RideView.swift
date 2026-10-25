import SwiftUI
import MapKit
import MediaPlayer

// The Ride tab is the head unit's live dashboard on the phone: the same pages,
// laid out the way the device packs them (DashLayout), filled with what the
// device streams over BLE: LiveFrames on the phone link (DeviceLive), with the
// parts the page in view needs, else the 1 Hz status characteristic. Fields
// neither carries, and everything while disconnected, read "--" — the last
// known layout stays on screen.
struct RideView: View {
    @EnvironmentObject var ble: BLEManager
    /// The live stream's frames. Observed here alone: no other screen redraws
    /// for them (see DeviceLive).
    @EnvironmentObject var live: DeviceLive
    @AppStorage(UnitPref.key) private var useMiles = false
    /// Editing the layout in place, on the preview itself; `draft` is the
    /// working copy, sent (or saved for the next connect) on Done.
    @State private var editing = false
    @State private var draft = DashConfig(pages: [])
    @State private var confirmDiscard = false
    /// Why the ADD NEW page couldn't add the one picked.
    @State private var addError: String?
    @State private var showWorkouts = false
    @State private var pageIx = 0

    private var connected: Bool { ble.state == .connected }
    /// The rider's layout (unsynced edit, else the device's, cached across
    /// disconnects); the device default only before it has ever been read.
    private var config: DashConfig { editing ? draft : ble.dashTarget ?? .deviceDefault }

    var body: some View {
        let _ = PerfMonitor.body("Ride")
        NavigationStack {
            VStack(spacing: 0) {
                header
                // One screen, never a scroll: the device page keeps the
                // panel's own 9:16 and is sized to fit whatever height the
                // header, warnings and page strip leave, centred across.
                VStack(spacing: 10) {
                    if !editing { warnings }
                    pageSelector
                    if editing {
                        editArea
                            .frame(maxWidth: .infinity, maxHeight: .infinity)
                    } else {
                        page
                            .frame(maxWidth: .infinity, maxHeight: .infinity)
                    }
                    if !connected && !editing {
                        PrimaryButton(title: primaryTitle,
                                      systemImage: "antenna.radiowaves.left.and.right") {
                            ble.connectTapped()
                        }
                    }
                }
                .padding(.horizontal, 16).padding(.vertical, 12)
                .frame(maxHeight: .infinity, alignment: .top)
            }
            .background(Palette.paper.ignoresSafeArea())
            .navigationBarHidden(true)
            .confirmationDialog("Discard your layout changes?", isPresented: $confirmDiscard,
                                titleVisibility: .visible) {
                Button("Discard changes", role: .destructive) { endEditing(save: false) }
                Button("Keep editing", role: .cancel) {}
            }
            .sheet(isPresented: $showWorkouts) { WorkoutsView() }
            .onAppear {
                ble.liveViewer(true)   // the page's topics while this tab is up
                ble.liveTopicsInView(liveTopics)
                ble.refreshSensors()
                // The session may have changed on the device while this tab
                // was away; ask rather than trust the last notify.
                if connected { ble.requestWorkoutStatus() }
                // Screenshot hook: `-ridePage N` opens page N.
                if let v = UserDefaults.standard.string(forKey: "ridePage"), let n = Int(v) { pageIx = n }
                // Screenshot hook, alongside -tab-*/-demo-* in
                // BikeGPSCompanionApp: opens the layout editor straight away so
                // the dashboard preview can be captured and compared against
                // the panel render from tools/preview.
                if ProcessInfo.processInfo.arguments.contains("-demo-dash") {
                    beginEditing()
                }
            }
            .onDisappear { ble.liveViewer(false) }
            .onChange(of: ble.state) { _, st in if st == .connected { ble.refreshSensors() } }
            .onChange(of: config.pages.count) { _, n in if pageIx > n { pageIx = 0 } }
            .onChange(of: liveTopics) { _, t in ble.liveTopicsInView(t) }
            // Paging here pages the panel too (not while editing a layout).
            .onChange(of: pageIx) { _, i in if !editing && connected { ble.showDevicePage(i) } }
        }
    }

    // MARK: Header

    private var header: some View {
        ScreenHeader(title: "Ride") {
            HStack(spacing: 8) {
                Rectangle()
                    .fill(connected ? Palette.ink : Color.clear)
                    .overlay(Rectangle().strokeBorder(Palette.ink, lineWidth: 1.5))
                    .frame(width: 8, height: 8)
                Text(statusLine)
                    .font(BarlowFont.text(11, .bold)).tracking(1.5)
                    .foregroundStyle(Palette.ink)
                    .lineLimit(1).minimumScaleFactor(0.7)
            }
            .accessibilityElement(children: .combine)
        } trailing: {
            HStack(spacing: 8) {
                if editing {
                    Button {
                        if draft == ble.dashTarget { endEditing(save: false) } else { confirmDiscard = true }
                    } label: {
                        BlockButtonLabel(title: "Cancel", filled: false)
                    }
                    .buttonStyle(.plain)
                    Button { endEditing(save: true) } label: {
                        BlockButtonLabel(title: draft == ble.dashTarget ? "Done" : connected ? "Send" : "Save",
                                         systemImage: "checkmark")
                    }
                    .buttonStyle(.plain)
                    .accessibilityHint(connected ? "Sends the layout to the device"
                                       : "Keeps the layout and sends it on the next connect")
                } else {
                    Button { showWorkouts = true } label: {
                        BlockButtonLabel(title: "Workouts", filled: false)
                    }
                    .buttonStyle(.plain)
                    .accessibilityHint("Load or build a structured workout")
                    Button { beginEditing() } label: {
                        BlockButtonLabel(title: "Edit", systemImage: "pencil")
                    }
                    .buttonStyle(.plain)
                    // The layout is read from the device: until it has been,
                    // an invented default would overwrite the rider's real one.
                    .disabled(ble.dashTarget == nil)
                    .opacity(ble.dashTarget == nil ? 0.4 : 1)
                    .accessibilityLabel("Edit dashboard")
                }
            }
            .fixedSize()
        }
    }

    /// "CONNECTED · 76% · GPS 9 SATS", or the connection state.
    private var statusLine: String {
        if editing {
            return draft == ble.dashTarget ? "EDITING LAYOUT" : "EDITING · UNSAVED CHANGES"
        }
        guard connected else { return connectLabel.uppercased() }
        let s = ble.status
        var parts = ["CONNECTED", "\(s.battery)%"]
        parts.append(s.gpsFix ? "GPS \(s.sats) SATS" : "NO GPS FIX · \(s.sats) SATS")
        if s.recording { parts.append("REC") }
        return parts.joined(separator: " · ")
    }

    // MARK: Warnings

    @ViewBuilder private var warnings: some View {
        if ble.bluetoothPermission.fixableInSettings || ble.bluetoothPermission == .unavailable {
            WarningBanner(title: "Bluetooth not allowed",
                          message: "The app can't reach your OpenTrailPaper without it.") {
                if ble.bluetoothPermission.fixableInSettings {
                    Button("SETTINGS") { openAppSettings() }
                        .font(BarlowFont.text(11, .bold)).tracking(1.5)
                        .foregroundStyle(Palette.ink)
                }
            }
        } else if ble.state == .poweredOff {
            WarningBanner(title: "Bluetooth is off",
                          message: "Turn it on in Control Centre to connect.")
        }
        if connected && ble.status.sdCard.isProblem { SDCardNotice(state: ble.status.sdCard) }
        if ble.dashTarget == nil {
            WarningBanner(title: "Default layout",
                          message: "Connect once to load your device's own dashboard pages.",
                          systemImage: "rectangle.3.group")
        } else if ble.pendingDashConfig != nil {
            WarningBanner(title: connected ? "Syncing layout" : "Layout waiting to sync",
                          message: connected ? nil : "Your edits go to the device on the next connect.",
                          systemImage: "arrow.triangle.2.circlepath")
        }
    }

    // MARK: Pages

    /// The page strip. Names keep their full size and spacing: they share the
    /// width when they fit, and the strip scrolls (keeping the selected one
    /// in view) when they don't — squeezing "STOPWATCH" to fit crushed it.
    private var pageSelector: some View {
        ScrollViewReader { proxy in
            ViewThatFits(in: .horizontal) {
                selectorCells(fill: true)
                ScrollView(.horizontal, showsIndicators: false) {
                    selectorCells(fill: false)
                }
                .scrollBounceBehavior(.basedOnSize)
            }
            .einkBorder()
            .onChange(of: pageIx) { _, i in withAnimation { proxy.scrollTo(i, anchor: .center) } }
        }
    }

    private func selectorCells(fill: Bool) -> some View {
        let pages = config.pages
        let sel = min(pageIx, pages.count)
        return HStack(spacing: 0) {
            ForEach(Array(pages.enumerated()), id: \.element.id) { i, p in
                selectorCell(on: i == sel, divider: i > 0) {
                    Text(pageName(i, in: pages))
                        .font(BarlowFont.text(12, .bold)).tracking(2)
                        .lineLimit(1)
                        .fixedSize()
                        .padding(.horizontal, 12)
                } tap: { withAnimation { pageIx = i } }
                .frame(maxWidth: fill ? .infinity : nil)
                .id(i)
                .accessibilityLabel("Dashboard page \(pageName(i, in: pages).capitalized)")
            }
            // The "add new" page that ends the swipe, as a narrow + cell.
            selectorCell(on: sel == pages.count, divider: !pages.isEmpty) {
                Image(systemName: "plus").font(.system(size: 14, weight: .bold))
            } tap: { withAnimation { pageIx = pages.count } }
            .frame(width: 44)
            .id(pages.count)
            .accessibilityLabel("Add a page")
        }
    }

    private func selectorCell<L: View>(on: Bool, divider: Bool, @ViewBuilder label: () -> L,
                                       tap: @escaping () -> Void) -> some View {
        Button(action: tap) {
            label()
                .frame(maxWidth: .infinity, minHeight: 34)
                .foregroundStyle(on ? Palette.paper : Palette.ink)
                .background(on ? Palette.ink : Palette.paper)
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .overlay(alignment: .leading) {
            if divider { Rectangle().fill(Palette.ink).frame(width: Rule.border) }
        }
        .accessibilityAddTraits(on ? [.isSelected, .isButton] : .isButton)
    }

    /// "PAGE 1" counts field pages only, as the device's page dots and the
    /// dashboard editor do; the others are named for what they show.
    private func pageName(_ i: Int, in pages: [DashConfig.Page]) -> String {
        switch pages[i].kind {
        case .fields:
            let n = pages.prefix(i + 1).filter { $0.kind == .fields }.count
            return "PAGE \(n)"
        case .map: return "MAP"
        case .music: return "MUSIC"
        case .workout: return "WORKOUT"
        case .stopwatch: return "STOPWATCH"
        case .segment: return "SEGMENT"
        }
    }

    /// The pages as the device draws them, live: scaled copies of the panel
    /// (540 x 960) fitted into the space the header leaves, swiped between
    /// like the device's own page cycle, then one last "add new" page.
    private var page: some View {
        let pages = config.pages
        return TabView(selection: Binding(get: { min(pageIx, pages.count) },
                                          set: { pageIx = $0 })) {
            ForEach(Array(pages.enumerated()), id: \.element.id) { i, p in
                TimelineView(.everyMinute) { ctx in
                    panelFrame { devicePage(p, now: ctx.date) }
                }
                .tag(i)
                .accessibilityElement(children: .contain)
                .accessibilityLabel("Device page \(pageName(i, in: pages).capitalized)")
            }
            panelFrame { addNewPage }.tag(pages.count)
        }
        .tabViewStyle(.page(indexDisplayMode: .never))
    }

    private func panelFrame<C: View>(@ViewBuilder _ content: () -> C) -> some View {
        content()
            .frame(maxWidth: .infinity)
            .aspectRatio(540 / 960, contentMode: .fit)
            .overlay(Rectangle().strokeBorder(Palette.ink, lineWidth: Rule.border))
            .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    /// The last page: add a page to the device's cycle, which then syncs
    /// (now, or on the next connect) like an edit made in the editor.
    @ViewBuilder private var addNewPage: some View {
        let c = config
        let full = c.contentPages >= DashConfig.devicePages
        VStack(spacing: 14) {
            Image(systemName: "plus")
                .font(.system(size: 44, weight: .bold))
            Text("ADD NEW")
                .font(BarlowFont.text(22, .bold)).tracking(3)
            Group {
                if ble.dashTarget == nil {
                    Text("Connect once to load your device's pages, then add more here.")
                } else if full {
                    Text(DashConfig.devicePages < DashConfig.maxPages
                         ? "This firmware holds \(DashConfig.devicePages) pages besides the map. Update the device's firmware for up to \(DashConfig.maxPages), or remove one in Edit."
                         : "The device holds \(DashConfig.maxPages) pages besides the map. Remove one in Edit to add another.")

                } else {
                    Menu {
                        Button { addPage(.newData) } label: { Label("Data page", systemImage: "rectangle.3.group") }
                        if !c.hasMusicPage {
                            Button { addPage(.music) } label: { Label("Music controls", systemImage: "music.note") }
                        }
                        if !c.hasWorkoutPage {
                            Button { addPage(.workout) } label: { Label("Workout", systemImage: "figure.outdoor.cycle") }
                        }
                        if !c.hasStopwatchPage {
                            Button { addPage(.stopwatch) } label: { Label("Stopwatch", systemImage: "stopwatch") }
                        }
                        if !c.hasSegmentPage {
                            Button { addPage(.segment) } label: { Label("Segment", systemImage: "flag.checkered") }
                        }
                    } label: {
                        BlockButtonLabel(title: "Choose a page", systemImage: "plus")
                    }
                    if let addError {
                        Text(addError).padding(.top, 4)
                    }
                }
            }
            .font(BarlowFont.text(13))
            .multilineTextAlignment(.center)
            .padding(.horizontal, 24)
        }
        .foregroundStyle(Palette.ink)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(Palette.paper)
        .overlay(Rectangle().inset(by: 14)
            .strokeBorder(Palette.ink, style: StrokeStyle(lineWidth: 2, dash: [8, 6])))
        .accessibilityElement(children: .contain)
        .accessibilityLabel("Add a new page")
    }

    private func addPage(_ p: DashConfig.Page) {
        var next = editing ? draft : (ble.dashTarget ?? draft)
        next.pages.append(p)
        // The device hands the layout back in one GATT read (512 bytes at
        // most); a bigger one would come back cut short.
        guard next.configText.utf8.count <= DashConfig.maxTextBytes else {
            addError = "Your pages are full of fields — the layout is at the size the device can send back. Remove a few fields or a page first."
            return
        }
        addError = nil
        if editing {
            draft.pages.append(p)
            withAnimation { pageIx = draft.pages.count - 1 }
            return
        }
        guard var c = ble.dashTarget else { return }
        c.pages.append(p)
        ble.saveDashConfig(c)
        withAnimation { pageIx = c.pages.count - 1 }
    }

    // MARK: Editing in place

    private func beginEditing() {
        guard let c = ble.dashTarget else { return }
        draft = c
        withAnimation(.easeOut(duration: 0.15)) { editing = true }
    }

    private func endEditing(save: Bool) {
        if save, draft != ble.dashTarget, !draft.pages.isEmpty { ble.saveDashConfig(draft) }
        withAnimation(.easeOut(duration: 0.15)) { editing = false }
        if pageIx > config.pages.count { pageIx = 0 }
    }

    /// The selected page, editable where it stands. No swipe while editing:
    /// the cells take the finger for dragging, so the strip above changes
    /// page instead.
    /// The settings box under the page while editing. One height for every
    /// page — tall enough for a selected cell's inspector — so the panel
    /// above is the same size on all of them.
    private static let editSlotHeight: CGFloat = 200

    @ViewBuilder private var editArea: some View {
        let pages = draft.pages
        let i = min(pageIx, pages.count)
        VStack(spacing: 10) {
            if i == pages.count {
                // The add page has nothing to move or remove; the row is held
                // so the panel lines up with the others.
                pageActions(max(0, pages.count - 1)).hidden()
                panelFrame { addNewPage }
                FixedCard(height: Self.editSlotHeight) {
                    Text("Choose a page type on the panel to add it to the device's cycle. It goes to the device with the rest of your changes when you tap \(connected ? "Send" : "Save").")
                        .font(BarlowFont.text(13)).foregroundStyle(Palette.muted)
                }
                .padding(.top, 0)
            } else {
                pageActions(i)
                switch pages[i].kind {
                case .fields:
                    TimelineView(.everyMinute) { ctx in
                        PageLayoutEditor(layout: $draft.pages[i].layout, status: panelStatus(ctx.date),
                                         inspectorHeight: Self.editSlotHeight)
                    }
                    // A fresh editor (and selection) per page.
                    .id(pages[i].id)
                default:
                    TimelineView(.everyMinute) { ctx in
                        panelFrame { devicePage(pages[i], now: ctx.date) }
                    }
                    FixedCard(height: Self.editSlotHeight) { pageSettings(pages[i]) }
                }
            }
        }
    }

    /// Move the page through the Home-key cycle, or take it out.
    private func pageActions(_ i: Int) -> some View {
        let pages = draft.pages
        let removable = !pages[i].isMap && draft.contentPages > 1
        return HStack(spacing: 8) {
            Button { movePage(i, by: -1) } label: {
                BlockButtonLabel(title: "Move", systemImage: "chevron.left", filled: false, expand: true)
            }
            .disabled(i == 0).opacity(i == 0 ? 0.4 : 1)
            .accessibilityLabel("Move page earlier")
            Button { movePage(i, by: 1) } label: {
                BlockButtonLabel(title: "Move", systemImage: "chevron.right", filled: false, expand: true)
            }
            .disabled(i == pages.count - 1).opacity(i == pages.count - 1 ? 0.4 : 1)
            .accessibilityLabel("Move page later")
            Button {
                withAnimation {
                    draft.pages.remove(at: i)
                    pageIx = min(i, draft.pages.count - 1)
                }
            } label: {
                BlockButtonLabel(title: "Remove", systemImage: "trash", filled: false, expand: true)
            }
            .disabled(!removable).opacity(removable ? 1 : 0.4)
            .accessibilityHint(pages[i].isMap ? "The map page can't be removed" : "")
        }
        .buttonStyle(.plain)
    }

    private func movePage(_ i: Int, by d: Int) {
        let j = i + d
        guard draft.pages.indices.contains(j) else { return }
        withAnimation {
            draft.pages.swapAt(i, j)
            pageIx = j
        }
    }

    /// `page segment` / `page segment manual` for the page being edited.
    private var segmentAutoShow: Binding<Bool> {
        Binding(get: { draft.pages.indices.contains(pageIx) ? draft.pages[pageIx].autoShow : true },
                set: { v in
                    guard draft.pages.indices.contains(pageIx) else { return }
                    draft.pages[pageIx].autoShow = v
                })
    }

    /// What there is to set on a page that isn't laid out by hand.
    @ViewBuilder private func pageSettings(_ p: DashConfig.Page) -> some View {
        switch p.kind {
        case .map:
            Group {
                VStack(alignment: .leading, spacing: 8) {
                    Text("Data strip").trackedLabel()
                    HStack(spacing: 8) {
                        ForEach(0..<3, id: \.self) { slot in
                            Menu {
                                Picker(["Left cell", "Middle cell", "Right cell"][slot],
                                       selection: $draft.mapFields[slot]) {
                                    ForEach(DashField.all.filter { $0.id != "radar" }) { f in
                                        Text(f.label).tag(f.id)
                                    }
                                }
                            } label: {
                                BlockButtonLabel(title: DashField.named(draft.mapFields[slot])?.label ?? "--",
                                                 filled: false, expand: true)
                            }
                            .accessibilityLabel(["Left cell", "Middle cell", "Right cell"][slot])
                        }
                    }
                    Text("The three cells under the map. While navigating, Route left takes the right cell unless one already shows it.")
                        .font(BarlowFont.text(12.5)).foregroundStyle(Palette.muted)
                }
            }
        case .music:
            Group { AlbumArtControls(art: ble.albumArt) }
        case .workout:
            Group {
                Text("Load a workout with Workouts above, or drop .erg/.mrc files in /workouts on the SD card.")
                    .font(BarlowFont.text(13)).foregroundStyle(Palette.muted)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
        case .stopwatch:
            Group {
                Text("A clock of its own, separate from the ride timer, with START/PAUSE, RESET and LAP on the panel. Needs device firmware v1.23 or newer.")
                    .font(BarlowFont.text(13)).foregroundStyle(Palette.muted)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
        case .segment:
            Group {
                VStack(alignment: .leading, spacing: 10) {
                    Toggle(isOn: segmentAutoShow) {
                        Text("Switch to this page when a segment starts")
                            .font(BarlowFont.text(14, .semibold)).foregroundStyle(Palette.ink)
                    }
                    Text("Live Strava segments: sync your starred ones in Settings › Accounts › Strava. Efforts are timed on any page. Needs device firmware v1.23 or newer.")
                        .font(BarlowFont.text(12.5)).foregroundStyle(Palette.muted)
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
            }
        case .fields:
            EmptyView()
        }
    }

    @ViewBuilder private func devicePage(_ p: DashConfig.Page?, now: Date) -> some View {
        let status = panelStatus(now)
        switch p?.kind ?? .fields {
        case .fields:
            DevicePanel(status: status) { k in
                DashPreview(layout: p?.layout ?? .deviceDefault,
                            liveText: { liveText($0, now: now) },
                            liveWatts: liveWatts,
                            ftp: Double(ble.ftpWatts))
                    .frame(width: 540 * k)
                    .offset(y: 64 * k)
            }
        case .map:
            LiveMapPage(fields: config.mapFields, status: status,
                        value: { liveText($0, now: now) },
                        route: connected && ble.status.hasRoute ? ble.sentRouteCoords : [],
                        showsUser: ble.locationPermission.isGranted,
                        rider: frame?.position)
        case .music:
            LiveMusicPage(art: ble.albumArt, status: status)
        case .workout:
            WorkoutPagePreview(status: status, model: workoutModel,
                               actions: connected ? workoutActions : nil)
        case .stopwatch:
            stopwatchPage(status)
        case .segment:
            // The device doesn't stream its segment state: the page's tiles
            // as it draws them, every value "--".
            SegmentPagePreview(status: status, model: .placeholder,
                               note: ble.deviceSegmentCount.map {
                                   "\($0) starred segment\($0 == 1 ? "" : "s") on the device"
                               } ?? "Live segment times show on the device")
        }
    }

    /// What the device should stream for the page in view, besides RIDE
    /// (always on): the stopwatch, the player, the map's position and view,
    /// and radar where a tile shows it.
    private var liveTopics: Set<LinkLiveTopic> {
        let pages = config.pages
        guard pageIx < pages.count else { return [] }
        let p = pages[pageIx]
        switch p.kind {
        case .stopwatch: return [.stopwatch]
        case .music: return [.music]
        case .map: return config.mapFields.contains("radar") ? [.position, .view, .radar] : [.position, .view]
        case .fields: return p.layout.items.contains { $0.field == "radar" } ? [.radar] : []
        case .workout, .segment: return []
        }
    }

    /// The live stream's latest frame while connected; nil on firmware
    /// without it, before the first frame, or once it goes quiet.
    private var frame: LiveFrame? { connected ? live.frame : nil }

    /// The status bar as the device prints it, from what the link reports:
    /// the device's own labels and liveness from the live stream, else a
    /// guess from the status characteristic's readings.
    private func panelStatus(_ now: Date) -> PanelStatus {
        let s = ble.status
        var labels: [String] = []
        if let f = frame {
            if f.hrLive { labels.append("HR") }
            if f.powerLive { labels.append("PWR") }
            if f.wheelSpeed { labels.append("SPD") }
            if f.radarConnected && !f.paused { labels.append(f.radar?.live == true ? "RDR" : "RDR?") }
            if f.paused { labels.append("PAUSED") }
        } else if connected {
            if s.heartRate != nil { labels.append("HR") }
            if s.power != nil { labels.append("PWR") }
        }
        return PanelStatus(clock: clockText(now),
                           satellites: frame?.sats ?? (connected ? s.sats : 0),
                           battery: frame?.battery ?? (connected ? s.battery : 0),
                           linked: connected,
                           sensors: labels.map { "· \($0)" }.joined(separator: " "))
    }

    /// Watts the power fields' zone bar fills by; nil = no reading.
    private func liveWatts(_ field: String) -> Double? {
        guard connected, field == "power3s" || field == "power" else { return nil }
        if let f = frame { return (field == "power3s" ? f.power3s : f.power).map(Double.init) }
        return ble.status.power.map(Double.init)
    }

    /// The STOPWATCH page from the live stream: the device's clock, counted
    /// on between frames while it runs, its state and, once a lap is done,
    /// the device's lap layout (total, lap in progress, the completed laps
    /// from STOPWATCH_LAPS; the lap times count on with the total). START/PAUSE,
    /// RESET and LAP drive the device's own clock ([0x10] on the live
    /// service), enabled by its rules; the next frame redraws the page. On
    /// firmware that streams but takes no commands they stay greyed, and
    /// without the stream (older firmware) the page shows its layout with no
    /// time.
    @ViewBuilder private func stopwatchPage(_ status: PanelStatus) -> some View {
        if let f = frame, let sw = f.stopwatch {
            let controls = f.takesStopwatchCommands
            TimelineView(.periodic(from: .now, by: sw.running ? 0.25 : 3600)) { ctx in
                let ms = live.stopwatchElapsedMs(at: ctx.date) ?? sw.elapsedMs
                let lapMs = sw.lapMs &+ (ms &- sw.elapsedMs)
                StopwatchPagePreview(
                    status: status, time: StopwatchPagePreview.timeText(ms),
                    state: sw.running ? "RUNNING" : ms == 0 ? "READY" : "PAUSED",
                    running: sw.running,
                    lapPage: sw.laps == 0 ? nil : StopwatchLapPage(
                        totalMs: ms, running: sw.running, lapNumber: sw.laps + 1, lapMs: lapMs,
                        lapSpeedKmh: sw.lapAvgSpeedKmh, lapCadence: sw.lapAvgCadence,
                        lapPowerW: sw.lapAvgPowerW,
                        showCadence: f.stopwatchLaps?.showCadence ?? sw.showCadence,
                        showPower: f.stopwatchLaps?.showPower ?? sw.showPower,
                        useMiles: useMiles, rows: f.stopwatchLaps?.laps),
                    controlsEnabled: controls,
                    canReset: !sw.running && sw.elapsedMs > 0,
                    canLap: sw.running && sw.laps < 99,
                    onControl: { ble.stopwatchCommand(UInt8($0 + 1)) },
                    controlsHint: "Update the device's firmware to use these from the app")
            }
        } else {
            StopwatchPagePreview(status: status, time: "--:--", state: "--", running: false,
                                 note: connected && !ble.liveSupported
                                    ? "Update the device's firmware to see its stopwatch here"
                                    : nil,
                                 controlsEnabled: false,
                                 controlsHint: connected ? "Waiting for the device"
                                                         : "Connect the device first")
        }
    }

    /// Ride and moving time as the device prints them (formatHms).
    static func hmsText(_ secs: Int) -> String {
        String(format: "%d:%02d:%02d", secs / 3600, secs / 60 % 60, secs % 60)
    }

    private func clockText(_ now: Date) -> String {
        let f = DateFormatter()
        f.dateFormat = ble.clock24h ? "H:mm" : "h:mm"
        return f.string(from: now)
    }

    /// A field's text on the panel: live where the link carries it, else "--".
    private func liveText(_ field: String, now: Date) -> String {
        let v = values(field)
        return v.isClock ? clockText(now) : v.text
    }

    /// The WORKOUT page's taps, as the workout characteristic's commands —
    /// the same ones the device's own taps run (ui_dashboard.cpp).
    private var workoutActions: WorkoutPageActions {
        WorkoutPageActions(
            toggle: {
                let w = ble.workoutStatus
                let started = w.running || w.paused || w.done || w.elapsedSec > 0
                if !started { ble.workoutStart() }         // 0x20
                else if w.running { ble.workoutPause() }   // 0x21
                else { ble.workoutResume() }               // 0x22
            },
            jump: { ble.workoutJump(block: $0) },          // 0x25 [idx]
            unload: { ble.workoutUnload() },               // 0x27
            load: { ble.loadWorkout($0) },                 // 0x11 name
            refresh: { ble.refreshWorkouts() })            // 0x06 + 0x30
    }

    /// The WORKOUT page's model: the device's session status (0xC0), the
    /// loaded file's blocks, live power, and the device's /workouts for the
    /// picker when nothing is loaded.
    private var workoutModel: WorkoutPageModel {
        let w = ble.workoutStatus
        let ftp = w.ftpW > 0 ? Int(w.ftpW) : ble.ftpWatts
        var m = WorkoutPageModel()
        m.loaded = connected && w.loaded
        m.name = w.name
        m.blockIdx = w.blockIndex
        m.blockCount = w.blockCount
        m.running = w.running
        m.paused = w.paused
        m.done = w.done
        m.elapsed = Int(w.elapsedSec)
        m.total = Int(w.totalSec)
        m.targetW = Int(w.targetW)
        m.ftpW = ftp
        m.power = connected ? ble.status.power : nil
        if let f = ble.loadedWorkoutFile, f.name == w.name {
            m.blocks = WorkoutPageModel.blocks(fromFile: f.text, ftp: ftp)
        } else {
            m.blocks = []
        }
        m.picker = ble.deviceWorkouts
        m.fileUnavailable = ble.loadedWorkoutFileUnavailable
        return m
    }

    // MARK: Live values

    /// What a field shows right now: the value and its unit, "--" when the
    /// link doesn't carry it (or there is no link), and the power zone.
    /// Every field comes from the live stream while it runs; without it
    /// (older firmware, or before its first frame) the status characteristic
    /// covers speed, power, HR, battery, sats and route left.
    private func values(_ field: String) -> DashValue {
        let s = ble.status
        let live = connected
        let f = frame
        func num(_ v: Int?) -> String { live ? v.map(String.init) ?? "--" : "--" }
        func one(_ v: Double?) -> String { v.map { String(format: "%.1f", $0) } ?? "--" }
        func whole(_ v: Double?) -> String { v.map { String(format: "%.0f", $0) } ?? "--" }
        let speedUnit = Units.speedLabel(useMiles).uppercased()
        let distUnit = Units.distLabel(useMiles).uppercased()
        let elevUnit = Units.elevLabel(useMiles).uppercased()
        switch field {
        case "speed":
            // A frame's absent speed is "no reading"; the status's is 0.
            let kmh = f.map(\.speedKmh) ?? s.speedKmh
            let v = live ? kmh.map { Units.speed($0, miles: useMiles) } : nil
            return DashValue(text: one(v), unit: speedUnit)
        case "avgspeed":
            return DashValue(text: one(f.map { Units.speed($0.avgSpeedKmh, miles: useMiles) }), unit: speedUnit)
        case "power3s", "power":
            let w = live ? (f.map { field == "power3s" ? $0.power3s : $0.power } ?? s.power) : nil
            return DashValue(text: num(w), unit: "W", zone: .some(w.map { Self.powerZone($0, ftp: ble.ftpWatts) } ?? 0))
        case "hr": return DashValue(text: num(f.map(\.heartRate) ?? s.heartRate), unit: "BPM")
        case "cadence": return DashValue(text: num(f?.cadence), unit: "RPM")
        case "distance":
            return DashValue(text: one(f.map { Units.distance($0.distanceM / 1000, miles: useMiles) }), unit: distUnit)
        case "ridetime": return DashValue(text: f.map { Self.hmsText($0.rideTimeS) } ?? "--", unit: "")
        case "movingtime": return DashValue(text: f.map { Self.hmsText($0.movingTimeS) } ?? "--", unit: "")
        case "climb":
            return DashValue(text: whole(f.map { Units.elevation($0.climbM, miles: useMiles) }), unit: elevUnit)
        case "altitude":
            return DashValue(text: whole(f?.altitudeM.map { Units.elevation($0, miles: useMiles) }), unit: elevUnit)
        case "grade": return DashValue(text: one(f?.gradePct), unit: "%")
        case "battery": return DashValue(text: live ? "\(f?.battery ?? s.battery)" : "--", unit: "%")
        case "sats": return DashValue(text: live ? "\(f?.sats ?? s.sats)" : "--", unit: "")
        case "clock": return DashValue(text: "", unit: "", isClock: true)
        case "routeleft":
            let on = f?.routeActive ?? s.hasRoute
            let km = f?.routeLeftKm ?? s.remainingKm
            let v = live && on ? String(format: "%.1f", Units.distance(km, miles: useMiles)) : "--"
            return DashValue(text: v, unit: distUnit)
        default: return DashValue(text: "--", unit: "")
        }
    }

    /// Coggan's seven power zones as % of FTP: <55, <75, <90, <105, <120, <150, above.
    static func powerZone(_ watts: Int, ftp: Int) -> Int {
        guard ftp > 0 else { return 0 }
        let pct = Double(watts) / Double(ftp) * 100
        let bounds: [Double] = [55, 75, 90, 105, 120, 150]
        return (bounds.firstIndex { pct < $0 } ?? 6) + 1
    }

    private var connectLabel: String {
        switch ble.state {
        case .scanning: return "Searching…"
        case .connecting: return ble.linkSecuring ? "Pairing…" : "Connecting…"
        case .poweredOff: return "Bluetooth off"
        default: return "Not connected"
        }
    }
    private var primaryTitle: String {
        ble.state == .connecting && ble.linkSecuring ? "Pairing…"
            : ble.state == .scanning || ble.state == .connecting ? "Searching…" : "Connect to OpenTrailPaper"
    }
}

/// One field's live reading for a dashboard tile.
struct DashValue {
    var text: String
    var unit: String
    /// Power zone 1–7 (0 = no reading yet); nil for fields without zones.
    var zone: Int? = nil
    /// Time of day, drawn from the phone's clock (same wall clock as the device).
    var isClock = false
}

struct ConnectionBanner: View {
    @EnvironmentObject var ble: BLEManager

    var body: some View {
        Card {
            HStack(spacing: 12) {
                Circle()
                    .fill(dotColor)
                    .frame(width: 12, height: 12)
                VStack(alignment: .leading, spacing: 2) {
                    Text(title).font(TypeScale.body).foregroundStyle(Palette.ink)
                    if let msg = ble.lastMessage {
                        Text(msg).font(.system(size: 12)).foregroundStyle(Palette.muted)
                    }
                }
                Spacer()
                if ble.state != .connected {
                    Button("Scan") { ble.connectTapped() }
                        .font(.system(size: 14, weight: .semibold))
                }
            }
        }
    }

    private var dotColor: Color {
        switch ble.state {
        case .connected: return Palette.good
        case .scanning, .connecting: return Palette.accent
        default: return Palette.muted
        }
    }
    private var title: String {
        switch ble.state {
        case .connected: return "Connected to OpenTrailPaper"
        case .scanning: return "Searching…"
        case .connecting: return ble.linkSecuring ? "Pairing…" : "Connecting…"
        case .poweredOff: return "Bluetooth is off"
        case .idle: return "Not connected"
        }
    }
}

/// The MAP page: the app's e-ink-styled map (muted, desaturated) around the
/// phone's position — the device does not send its own — with the route this
/// phone last sent while the device reports one active, the page's data
/// strip, and the zoom stack. Zoom is phone-only: there is no BLE command
/// for the device's map zoom.
private struct LiveMapPage: View {
    let fields: [String]
    let status: PanelStatus
    let value: (String) -> String
    let route: [CLLocationCoordinate2D]
    let showsUser: Bool
    /// Where the DEVICE is, from the live stream: the map centres on it and
    /// draws the device's own rider marker and compass. Without it (older
    /// firmware, no fix yet) the phone's own location stands in.
    var rider: LiveFrame.Position? = nil
    @State private var line: MKPolyline?
    @State private var camera: MapCameraCommand?
    @State private var region: MKCoordinateRegion?

    var body: some View {
        MapPagePreview(fields: fields, status: status, value: value,
                       map: AnyView(mapArea),
                       onZoom: zoom)
            .onAppear { update() }
            .onChange(of: route.count) { update() }
            .onChange(of: riderKey) { follow() }
    }

    /// The map body (panel y 64...810) with the device's chrome over it:
    /// the circled rider arrow at the middle, pointing along the course, and
    /// the compass at its corner. The phone's map is north-up, so the
    /// compass needle points straight up.
    private var mapArea: some View {
        GeometryReader { geo in
            let k = geo.size.width / 540
            ZStack(alignment: .topLeading) {
                EInkMapView(route: line, camera: camera,
                            showsUserLocation: showsUser && rider == nil,
                            onRegionChange: { region = $0 })
                    .allowsHitTesting(false)
                if rider != nil {
                    RiderMark(headingDeg: rider?.courseDeg, k: k)
                        .position(x: geo.size.width / 2, y: geo.size.height / 2)
                }
                CompassMark(k: k)
                    .position(x: (540 - 46) * k, y: 48 * k)
            }
        }
    }

    /// Changes when the device's reported position does, rounded so float
    /// noise doesn't move the camera.
    private var riderKey: String {
        guard let r = rider else { return "" }
        return String(format: "%.5f,%.5f", r.lat, r.lon)
    }

    private func follow() {
        guard let r = rider else { update(); return }
        let c = CLLocationCoordinate2D(latitude: r.lat, longitude: r.lon)
        if region == nil {
            camera = MapCameraCommand(target: .region(MKCoordinateRegion(
                center: c, latitudinalMeters: 1200, longitudinalMeters: 1200)))
        } else {
            camera = MapCameraCommand(target: .center(c))
        }
    }

    private func zoom(_ dir: Int) {
        guard var r = region else { return }
        let f = dir > 0 ? 0.5 : 2.0
        r.span = MKCoordinateSpan(latitudeDelta: min(max(r.span.latitudeDelta * f, 0.002), 60),
                                  longitudeDelta: min(max(r.span.longitudeDelta * f, 0.002), 60))
        camera = MapCameraCommand(target: .region(r))
    }

    private func update() {
        if rider != nil {
            if route.count > 1 {
                var c = route
                line = MKPolyline(coordinates: &c, count: c.count)
            } else {
                line = nil
            }
            follow()
            return
        }
        if route.count > 1 {
            var c = route
            let l = MKPolyline(coordinates: &c, count: c.count)
            line = l
            camera = MapCameraCommand(target: showsUser ? .followUser
                                      : .rect(l.boundingMapRect.paddedForDisplay()))
        } else {
            line = nil
            camera = MapCameraCommand(target: .followUser)
        }
    }
}

/// The MUSIC page, live: the track the device reported over AMS and the cover
/// the app sent it, dithered to the panel's tones.
private struct LiveMusicPage: View {
    @EnvironmentObject var ble: BLEManager
    /// The live stream: the device's player position (MUSIC record).
    @EnvironmentObject var live: DeviceLive
    @ObservedObject var art: AlbumArtFeeder
    let status: PanelStatus
    @State private var image: UIImage?
    /// Bumped after a transport tap so play/pause redraws from the player.
    @State private var tick = 0

    var body: some View {
        let track = art.nowPlaying
        let player = MPMusicPlayerController.systemMusicPlayer
        // The device sends its transport over AMS, which reaches any player;
        // an app can only drive the Music app's own player, so the buttons
        // are live while the Music app is the one playing.
        let musicApp = player.nowPlayingItem != nil
        // What the device knows of the player (AMS, via the live stream):
        // play state and position for ANY app, which the phone can't see.
        let music = ble.state == .connected ? live.frame?.music : nil
        // Firmware that relays remote commands over AMS drives any player;
        // older firmware leaves the app only the Music app's own player. A
        // device whose AMS link is down (pairing declined) can't relay them
        // either, so the Music app's player takes over when it is the one.
        let remote = ble.mediaRemote && (music?.ams ?? true || !musicApp)
        let playing = music.map { $0.present && $0.playing }
            ?? (musicApp ? player.playbackState == .playing : track != nil)
        // Counted on between frames while playing; a paused bar holds still.
        TimelineView(.periodic(from: .now, by: music?.playing == true ? 0.5 : 3600)) { ctx in
            let progress: (pos: Int, dur: Int)? = {
                guard let m = music, let dur = m.durationMs,
                      let pos = live.musicPositionMs(at: ctx.date) else { return nil }
                return (Int(pos / 1000), Int((dur + 500) / 1000))
            }()
            MusicPreview(status: status, track: track,
                         art: track != nil && art.cover?.key == track?.key ? image : nil,
                         playing: playing,
                         progress: progress,
                         onTransport: { b in
                             if remote {
                                 ble.mediaCommand(b == 0 ? 3 : b == 1 ? 1 : 2)
                             } else {
                                 switch b {
                                 case 0: player.skipToPreviousItem()
                                 case 1: player.playbackState == .playing ? player.pause() : player.play()
                                 default: player.skipToNextItem()
                                 }
                             }
                             tick += 1
                         },
                         transportEnabled: remote || musicApp,
                         transportHint: remote || musicApp ? nil
                            : "Update the device's firmware to control other players from here",
                         // Volume only goes through the device (AMS): iOS has
                         // no API for an app to set the system volume.
                         onVolume: remote ? { v in ble.mediaCommand(v == 0 ? 4 : 5) } : nil,
                         volumeHint: ble.mediaRemote
                            ? "The device's music link (AMS) is off: pair it in Bluetooth settings"
                            : "Update the device's firmware, or use the phone's volume buttons")
        }
            .id(tick)
            .task(id: art.cover?.key) {
                guard let c = art.cover else { image = nil; return }
                let side = AlbumArtFeeder.side
                image = await Task.detached(priority: .utility) {
                    ArtDither.panelImage(gray: c.gray, side: side)
                }.value
            }
    }
}

private extension DashConfig.Page {
    /// A new data page starts from the device's default fields, not empty,
    /// so it is a usable page the moment it lands; Edit reworks it.
    static var newData: DashConfig.Page { DashConfig.Page(kind: .fields, layout: .deviceDefault) }
}

/// Where the MUSIC page's cover comes from (the editor's album-art settings).
private struct AlbumArtControls: View {
    @ObservedObject var art: AlbumArtFeeder

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Album art").trackedLabel()
            switch art.libraryAccess {
            case .granted:
                Text("Covers from the Music app are on")
                    .font(BarlowFont.text(13)).foregroundStyle(Palette.muted)
            case .notDetermined:
                Button("Allow covers from the Music app") { art.requestLibraryAccess() }
                    .font(BarlowFont.text(14, .semibold))
            case .denied:
                Button("Music app covers: allow in Settings") { openAppSettings() }
                    .font(BarlowFont.text(14, .semibold))
            case .unavailable:
                Text("Music app covers are blocked on this phone.")
                    .font(BarlowFont.text(13)).foregroundStyle(Palette.muted)
            }
            Toggle("Look up album art online", isOn: $art.onlineLookup)
                .font(BarlowFont.text(14, .semibold))
            Text("For Spotify and other players, finds the cover in Apple's iTunes catalogue — this sends the playing song's title, artist and album to Apple.")
                .font(BarlowFont.text(12)).foregroundStyle(Palette.muted)
        }
        .foregroundStyle(Palette.ink)
    }
}

/// The device's rider marker (map_view.cpp drawRider): a white disc with a
/// 3px ring and a split arrowhead along the course. No course yet: the
/// arrow points up, as the device's does at heading 0.
private struct RiderMark: View {
    let headingDeg: Double?
    let k: CGFloat

    var body: some View {
        Canvas { ctx, size in
            let c = CGPoint(x: size.width / 2, y: size.height / 2)
            ctx.fill(Path(ellipseIn: CGRect(x: c.x - 30 * k, y: c.y - 30 * k, width: 60 * k, height: 60 * k)),
                     with: .color(.white))
            ctx.stroke(Path(ellipseIn: CGRect(x: c.x - 29 * k, y: c.y - 29 * k, width: 58 * k, height: 58 * k)),
                       with: .color(.black), lineWidth: max(1, 3 * k))
            let rad = ((headingDeg ?? 0) - 90) * .pi / 180
            let cs = cos(rad), sn = sin(rad)
            func rot(_ lx: CGFloat, _ ly: CGFloat) -> CGPoint {
                CGPoint(x: c.x + (lx * cs - ly * sn) * k, y: c.y + (lx * sn + ly * cs) * k)
            }
            var arrow = Path()
            arrow.move(to: rot(24, 0))
            arrow.addLine(to: rot(-15, -13))
            arrow.addLine(to: rot(-8, 0))
            arrow.addLine(to: rot(-15, 13))
            arrow.closeSubpath()
            ctx.fill(arrow, with: .color(.black))
        }
        .frame(width: 64 * k, height: 64 * k)
        .accessibilityLabel("Device position")
    }
}

/// The device's compass (map_view.cpp drawCompass): ringed disc, needle to
/// north — up, on the phone's north-up map.
private struct CompassMark: View {
    let k: CGFloat

    var body: some View {
        Canvas { ctx, size in
            let c = CGPoint(x: size.width / 2, y: size.height / 2)
            ctx.fill(Path(ellipseIn: CGRect(x: c.x - 28 * k, y: c.y - 28 * k, width: 56 * k, height: 56 * k)),
                     with: .color(.white))
            ctx.stroke(Path(ellipseIn: CGRect(x: c.x - 27 * k, y: c.y - 27 * k, width: 54 * k, height: 54 * k)),
                       with: .color(.black), lineWidth: max(1, 3 * k))
            var needle = Path()
            needle.move(to: CGPoint(x: c.x, y: c.y - 19 * k))
            needle.addLine(to: CGPoint(x: c.x + 7 * k, y: c.y))
            needle.addLine(to: CGPoint(x: c.x - 7 * k, y: c.y))
            needle.closeSubpath()
            ctx.fill(needle, with: .color(.black))
        }
        .frame(width: 60 * k, height: 60 * k)
        .accessibilityLabel("North is up")
    }
}
