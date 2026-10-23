import SwiftUI
import MapKit
import MediaPlayer

// The Ride tab is the head unit's live dashboard on the phone: the same pages,
// laid out the way the device packs them (DashLayout), filled with what the
// device streams over BLE. Fields the link doesn't carry, and everything while
// disconnected, read "--" — the last known layout stays on screen.
struct RideView: View {
    @EnvironmentObject var ble: BLEManager
    @AppStorage(UnitPref.key) private var useMiles = false
    @State private var showDashEditor = false
    @State private var showWorkouts = false
    @State private var pageIx = 0

    private var connected: Bool { ble.state == .connected }
    /// The rider's layout (unsynced edit, else the device's, cached across
    /// disconnects); the device default only before it has ever been read.
    private var config: DashConfig { ble.dashTarget ?? .deviceDefault }

    var body: some View {
        NavigationStack {
            VStack(spacing: 0) {
                header
                // One screen, never a scroll: the device page keeps the
                // panel's own 9:16 and is sized to fit whatever height the
                // header, warnings and page strip leave, centred across.
                VStack(spacing: 10) {
                    warnings
                    pageSelector
                    page
                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                    if !connected {
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
            .sheet(isPresented: $showDashEditor) { DashboardEditorView() }
            .sheet(isPresented: $showWorkouts) { WorkoutsView() }
            .onAppear {
                ble.refreshSensors()
                // The session may have changed on the device while this tab
                // was away; ask rather than trust the last notify.
                if connected { ble.requestWorkoutStatus() }
                if ble.workoutStatus.loaded { showWorkoutPage() }
                // Screenshot hook: `-ridePage N` opens page N.
                if let v = UserDefaults.standard.string(forKey: "ridePage"), let n = Int(v) { pageIx = n }
                // Screenshot hook, alongside -tab-*/-demo-* in
                // BikeGPSCompanionApp: opens the layout editor straight away so
                // the dashboard preview can be captured and compared against
                // the panel render from tools/preview.
                if ProcessInfo.processInfo.arguments.contains("-demo-dash") {
                    showDashEditor = true
                }
            }
            .onChange(of: ble.state) { _, st in if st == .connected { ble.refreshSensors() } }
            .onChange(of: config.pages.count) { _, n in if pageIx >= n { pageIx = 0 } }
            // A workout loaded (here or on the device): go to its page.
            .onChange(of: ble.workoutStatus.loaded) { _, loaded in
                if loaded { showWorkoutPage() }
            }
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
                Button { showWorkouts = true } label: {
                    BlockButtonLabel(title: "Workouts", filled: false)
                }
                .buttonStyle(.plain)
                .accessibilityHint("Load or build a structured workout")
                Button { showDashEditor = true } label: {
                    BlockButtonLabel(title: "Edit", systemImage: "pencil")
                }
                .buttonStyle(.plain)
                .accessibilityLabel("Edit dashboard")
            }
            .fixedSize()
        }
    }

    /// "CONNECTED · 76% · GPS 9 SATS", or the connection state.
    private var statusLine: String {
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

    private var pageSelector: some View {
        let pages = config.pages
        return HStack(spacing: 0) {
            ForEach(Array(pages.enumerated()), id: \.element.id) { i, p in
                let on = i == min(pageIx, pages.count - 1)
                Button { pageIx = i } label: {
                    Text(pageName(i, in: pages))
                        .font(BarlowFont.text(12, .bold)).tracking(2)
                        .lineLimit(1).minimumScaleFactor(0.6)
                        .frame(maxWidth: .infinity, minHeight: 34)
                        .foregroundStyle(on ? Palette.paper : Palette.ink)
                        .background(on ? Palette.ink : Palette.paper)
                        .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .overlay(alignment: .leading) {
                    if i > 0 { Rectangle().fill(Palette.ink).frame(width: Rule.border) }
                }
                .accessibilityLabel("Dashboard page \(pageName(i, in: pages).capitalized)")
                .accessibilityAddTraits(on ? [.isSelected, .isButton] : .isButton)
            }
        }
        .einkBorder()
    }

    private func showWorkoutPage() {
        if let i = config.pages.firstIndex(where: { $0.isWorkout }) { pageIx = i }
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
        }
    }

    /// The selected page as the device draws it, live: a scaled copy of the
    /// panel (540 x 960) fitted into the space the header leaves.
    private var page: some View {
        let pages = config.pages
        let p = pages.isEmpty ? nil : pages[min(pageIx, pages.count - 1)]
        return TimelineView(.everyMinute) { ctx in
            devicePage(p, now: ctx.date)
                .frame(maxWidth: .infinity)
                .aspectRatio(540 / 960, contentMode: .fit)
                .overlay(Rectangle().strokeBorder(Palette.ink, lineWidth: Rule.border))
        }
        .accessibilityElement(children: .contain)
        .accessibilityLabel(pages.isEmpty ? "Device page"
                            : "Device page \(pageName(min(pageIx, pages.count - 1), in: pages).capitalized)")
    }

    @ViewBuilder private func devicePage(_ p: DashConfig.Page?, now: Date) -> some View {
        let status = panelStatus(now)
        switch p?.kind ?? .fields {
        case .fields:
            DevicePanel(status: status) { k in
                DashPreview(layout: p?.layout ?? .deviceDefault,
                            liveText: { liveText($0, now: now) },
                            liveWatts: { f in
                                guard connected, f == "power3s" || f == "power",
                                      let w = ble.status.power else { return nil }
                                return Double(w)
                            },
                            ftp: Double(ble.ftpWatts))
                    .frame(width: 540 * k)
                    .offset(y: 64 * k)
            }
        case .map:
            LiveMapPage(fields: config.mapFields, status: status,
                        value: { liveText($0, now: now) },
                        route: connected && ble.status.hasRoute ? ble.sentRouteCoords : [],
                        showsUser: ble.locationPermission.isGranted)
        case .music:
            LiveMusicPage(art: ble.albumArt, status: status)
        case .workout:
            WorkoutPagePreview(status: status, model: workoutModel,
                               actions: connected ? workoutActions : nil)
        case .stopwatch:
            // The stopwatch lives on the device alone: nothing of it crosses
            // the link either way, so the page shows its layout with no time
            // and its buttons greyed.
            StopwatchPagePreview(status: status, time: "--:--", state: "--", running: false,
                                 note: "The stopwatch isn't sent over Bluetooth",
                                 controlsEnabled: false)
        }
    }

    /// The status bar as the device prints it, from what the link reports.
    private func panelStatus(_ now: Date) -> PanelStatus {
        let s = ble.status
        var sensors = ""
        if connected && s.heartRate != nil { sensors += "· HR " }
        if connected && s.power != nil { sensors += "· PWR" }
        return PanelStatus(clock: clockText(now),
                           satellites: connected ? s.sats : 0,
                           battery: connected ? s.battery : 0,
                           linked: connected,
                           sensors: sensors)
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
    private func values(_ field: String) -> DashValue {
        let s = ble.status
        let live = connected
        func num(_ v: Int?) -> String { live ? v.map(String.init) ?? "--" : "--" }
        switch field {
        case "speed", "avgspeed":
            let v = field == "speed" && live
                ? String(format: "%.1f", Units.speed(s.speedKmh, miles: useMiles)) : "--"
            return DashValue(text: v, unit: Units.speedLabel(useMiles).uppercased())
        case "power3s", "power":
            let w = live ? s.power : nil
            return DashValue(text: num(w), unit: "W", zone: .some(w.map { Self.powerZone($0, ftp: ble.ftpWatts) } ?? 0))
        case "hr": return DashValue(text: num(s.heartRate), unit: "BPM")
        case "cadence": return DashValue(text: "--", unit: "RPM")
        case "distance": return DashValue(text: "--", unit: Units.distLabel(useMiles).uppercased())
        case "ridetime", "movingtime": return DashValue(text: "--", unit: "")
        case "climb", "altitude": return DashValue(text: "--", unit: Units.elevLabel(useMiles).uppercased())
        case "grade": return DashValue(text: "--", unit: "%")
        case "battery": return DashValue(text: live ? "\(s.battery)" : "--", unit: "%")
        case "sats": return DashValue(text: live ? "\(s.sats)" : "--", unit: "")
        case "clock": return DashValue(text: "", unit: "", isClock: true)
        case "routeleft":
            let v = live && s.hasRoute ? String(format: "%.1f", Units.distance(s.remainingKm, miles: useMiles)) : "--"
            return DashValue(text: v, unit: Units.distLabel(useMiles).uppercased())
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
    @State private var line: MKPolyline?
    @State private var camera: MapCameraCommand?
    @State private var region: MKCoordinateRegion?

    var body: some View {
        MapPagePreview(fields: fields, status: status, value: value,
                       map: AnyView(EInkMapView(route: line, camera: camera,
                                                showsUserLocation: showsUser,
                                                onRegionChange: { region = $0 })
                                        .allowsHitTesting(false)),
                       onZoom: zoom)
            .onAppear { update() }
            .onChange(of: route.count) { update() }
    }

    private func zoom(_ dir: Int) {
        guard var r = region else { return }
        let f = dir > 0 ? 0.5 : 2.0
        r.span = MKCoordinateSpan(latitudeDelta: min(max(r.span.latitudeDelta * f, 0.002), 60),
                                  longitudeDelta: min(max(r.span.longitudeDelta * f, 0.002), 60))
        camera = MapCameraCommand(target: .region(r))
    }

    private func update() {
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
        // Firmware that relays remote commands over AMS drives any player;
        // older firmware leaves the app only the Music app's own player.
        let remote = ble.mediaRemote
        MusicPreview(status: status, track: track,
                     art: track != nil && art.cover?.key == track?.key ? image : nil,
                     playing: musicApp ? player.playbackState == .playing : track != nil,
                     progress: nil,
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
                        : "Update the device's firmware to control other players from here")
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
