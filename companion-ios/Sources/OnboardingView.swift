import SwiftUI

// First-run tutorial. Introduces what the app does, then primes and triggers
// the two system permission prompts (location, Bluetooth) on their own
// explaining screens — so the OS dialog arrives with context rather than cold
// at launch. Shown once; gated by BLEManager.onboardedKey.
//
// Every screen that explains a permission must be able to ACT on it, which is
// what the permission steps below are built around: if it hasn't been asked, the
// button raises the system prompt; if it was refused, the button opens Settings
// (the only place that can be undone); either way there is always a way past the
// screen without granting anything. A screen that describes a permission and
// then can't do anything about it is the failure mode to avoid.
struct OnboardingView: View {
    @EnvironmentObject var ble: BLEManager
    var onFinish: () -> Void

    @State private var step = OnboardingView.initialStep

    /// Which screen the welcome's live head unit is showing.
    @State private var heroFace: DeviceFace = .dashboard
    /// Which of the three jobs the overview is demonstrating, and whether the
    /// rider has taken the wheel — after the first tap the demo stops cycling.
    @State private var feature = 0
    @State private var drivingOverview = false

    private let lastStep = 5

    // Screenshot support: `-onboarding-step N` opens the tutorial on a page.
    static var initialStep: Int {
        let a = ProcessInfo.processInfo.arguments
        if let i = a.firstIndex(of: "-onboarding-step"), i + 1 < a.count,
           let n = Int(a[i + 1]) { return min(max(n, 0), 5) }
        return 0
    }

    var body: some View {
        ZStack {
            Palette.paper.ignoresSafeArea()

            VStack(spacing: 0) {
                topBar

                TabView(selection: $step) {
                    welcome.tag(0)
                    overview.tag(1)
                    locationStep.tag(2)
                    bluetoothStep.tag(3)
                    connectStep.tag(4)
                    ready.tag(5)
                }
                .tabViewStyle(.page(indexDisplayMode: .never))
                .animation(.easeInOut, value: step)

                footer
            }
        }
        // Auto-advance the moment a permission is granted, so allowing feels
        // instant; denying leaves the button as "Continue" to move on.
        .onChange(of: ble.locationAuthorized) { _, granted in
            if step == 2, granted { advance() }
        }
        .onChange(of: ble.bluetoothReady) { _, ready in
            if step == 3, ready { advance() }
        }
    }

    // MARK: chrome

    private var topBar: some View {
        HStack {
            dots
            Spacer()
            if step < lastStep {
                Button("Skip") { onFinish() }
                    .font(TypeScale.bodyStrong)
                    .foregroundStyle(Palette.muted)
            }
        }
        .padding(.horizontal, 24)
        .padding(.top, 20)
        .frame(height: 44)
    }

    private var dots: some View {
        HStack(spacing: 7) {
            ForEach(0...lastStep, id: \.self) { i in
                Capsule()
                    .fill(i == step ? Palette.accent : Palette.hairline)
                    .frame(width: i == step ? 20 : 7, height: 7)
                    .animation(.easeInOut, value: step)
            }
        }
    }

    private var footer: some View {
        VStack(spacing: 12) {
            PrimaryButton(title: stepAction.title, action: primaryAction)
            // Permission steps always keep an out on the same screen as the ask,
            // so declining never traps anyone. Hidden only when the primary
            // button IS "Continue" — two identical buttons help nobody.
            if let state = permissionState, stepAction.kind != .next {
                Button(state == .notDetermined ? "Not now" : "Continue") { advance() }
                    .font(TypeScale.bodyStrong)
                    .foregroundStyle(Palette.muted)
            }
        }
        .padding(.horizontal, 24)
        .padding(.bottom, 28)
        .padding(.top, 8)
    }

    // MARK: pages

    private var welcome: some View {
        page(
            art: AnyView(deviceHero),
            title: "Welcome to OpenTrailPaper",
            body: "This is the companion for the OpenTrailPaper head unit — a sunlight-readable e-paper bike computer you build yourself. The phone does the fiddly bits; the device does the riding.",
            note: AnyView(tapHint)
        )
    }

    // The head unit, running. It was a pair of product shots, which told a
    // first-time rider nothing a photo of a brick wouldn't: the whole pitch of
    // this device is numbers that hold steady in sunlight and a map that moves
    // under you, and neither survives being frozen. So the front unit is live —
    // ride time counting, power and speed working, the ground scrolling on the
    // map — and tapping it switches screens with the e-paper flash the real
    // button produces. The one behind shows the other screen, held still so the
    // eye knows which of the two it is meant to be watching.
    private var deviceHero: some View {
        ZStack {
            // Widths are the SCREEN's; the case adds about 11% around it. These
            // land the two bodies at the same sizes the product shots occupied,
            // so the composition is the one that was already signed off.
            EInkDevice(face: heroFace.counterpart, width: 110, live: false)
                .rotationEffect(.degrees(-9))
                .offset(x: -84, y: 12)
                .opacity(0.9)
            EInkDevice(face: heroFace, width: 135)
                .rotationEffect(.degrees(5))
                .offset(x: 26)
                .onTapGesture { withAnimation { heroFace = heroFace.counterpart } }
                .accessibilityAddTraits(.isButton)
                .accessibilityLabel(heroFace == .dashboard
                                    ? "Head unit showing the dashboard"
                                    : "Head unit showing the map")
                .accessibilityHint("Switches the screen")
        }
        .frame(height: 300)
    }

    private var tapHint: some View {
        Label(heroFace == .dashboard ? "Tap the device to see the map"
                                     : "Tap the device to see the numbers",
              systemImage: "hand.tap")
            .font(TypeScale.bodyStrong)
            .foregroundStyle(Palette.muted)
    }

    // What the APP is for, demonstrated on a phone rather than listed.
    //
    // Three bullet points describing a route upload, a map build and a ride
    // readback are three sentences a newcomer has no picture for. Each one owns
    // a screen of this app, so selecting it shows that screen doing that job —
    // the route drawing itself along the streets, tiles landing on the card, a
    // ride read back with today's still counting. It demonstrates itself on a
    // timer until the first tap, then hands over and stays where it's put: a
    // screen that keeps moving under someone reading it is fighting them.
    //
    // A phone, not the head unit: all three are things you do here, in your
    // hand, and showing the bike computer doing them put the work on the wrong
    // device. The head unit gets the welcome screen, which is about the device.
    private var overview: some View {
        VStack(spacing: 0) {
            Text("What you'll do here")
                .font(TypeScale.screenTitle)
                .foregroundStyle(Palette.ink)
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(.horizontal, 8)

            Spacer(minLength: 8)

            PhoneMock(face: OnboardingView.features[feature].face, width: 118)
                .accessibilityLabel("Phone showing \(OnboardingView.features[feature].title.lowercased())")

            Spacer(minLength: 12)

            // Short phones lose the two descriptions nobody is reading — the
            // selected row keeps its text, because that one is explaining what
            // the device above it is doing. Clipping the last row against the
            // footer, which is what a fixed layout did here, loses the same
            // words without admitting to it.
            ViewThatFits(in: .vertical) {
                rows(full: true)
                rows(full: false)
            }
        }
        .padding(.horizontal, 20)
        .padding(.vertical, 12)
        // Keyed so leaving the page or taking over cancels the demo rather than
        // leaving a timer running behind the rest of the tutorial.
        .task(id: "\(step)-\(drivingOverview)") {
            guard step == 1, !drivingOverview else { return }
            while !Task.isCancelled {
                try? await Task.sleep(for: .seconds(6.5))
                guard !Task.isCancelled else { return }
                withAnimation { feature = (feature + 1) % OnboardingView.features.count }
            }
        }
    }

    private func rows(full: Bool) -> some View {
        VStack(spacing: 6) {
            ForEach(Array(OnboardingView.features.enumerated()), id: \.offset) { i, f in
                featureRow(f, selected: i == feature, showBody: full || i == feature) {
                    drivingOverview = true
                    withAnimation { feature = i }
                }
            }
        }
    }

    private struct Feature {
        let symbol: String
        let title: String
        let body: String
        let face: AppFace
    }

    private static let features: [Feature] = [
        .init(symbol: "map", title: "Plan routes",
              body: "Search a destination and send the route to your device as GPX.",
              face: .route),
        .init(symbol: "square.and.arrow.down.on.square", title: "Build offline maps",
              body: "Pick an area; the app bakes map tiles onto the device's SD card.",
              face: .maps),
        .init(symbol: "list.bullet.rectangle", title: "Review rides",
              body: "Pull recorded rides off the device and see distance, power and climb.",
              face: .rides),
    ]

    private var locationStep: some View {
        page(
            art: AnyView(SketchIcon(glyph: .location, tint: tint(ble.locationPermission),
                                    active: step == 2)),
            title: "Share your location",
            body: "Used to show your position on the map, warm-start the device's GPS so it locks on fast, and act as a backup fix when the device can't see the sky. Only while you're using the app.",
            note: AnyView(statusChip(for: ble.locationPermission,
                                     granted: "Location allowed",
                                     denied: "Location is off for this app. Open Settings to allow it — the map still works without it.",
                                     unavailable: "Location is restricted on this iPhone."))
        )
    }

    private var bluetoothStep: some View {
        page(
            art: AnyView(SketchIcon(glyph: .waves, tint: tint(ble.bluetoothPermission),
                                    active: step == 3)),
            title: "Connect over Bluetooth",
            body: "Everything travels to and from your OpenTrailPaper over Bluetooth — routes, offline maps, settings and recorded rides. No account, no cloud. Next, we'll link the app to your device.",
            note: AnyView(bluetoothNote)
        )
    }

    // Bluetooth has a fourth state the others don't: allowed, but the radio is
    // switched off. That is not a refused permission and must not be reported as
    // one — no button of ours can fix it, only Control Centre.
    @ViewBuilder private var bluetoothNote: some View {
        if ble.bluetoothPermission.isGranted && !ble.bluetoothPoweredOn {
            chip("Bluetooth is switched off — turn it on in Control Centre.",
                 symbol: "exclamationmark.circle.fill", tint: Palette.accent)
        } else {
            statusChip(for: ble.bluetoothPermission,
                       granted: "Bluetooth allowed",
                       denied: "Bluetooth is off for this app. Open Settings to allow it — without it the app can't reach your device at all.",
                       unavailable: "Bluetooth is restricted on this iPhone.")
        }
    }

    @ViewBuilder private func statusChip(for state: PermissionState, granted: String,
                                         denied: String, unavailable: String) -> some View {
        switch state {
        case .granted:      chip(granted, symbol: "checkmark.circle.fill", tint: Palette.good)
        case .denied:       chip(denied, symbol: "exclamationmark.circle.fill", tint: Palette.accent)
        case .unavailable:  chip(unavailable, symbol: "lock.circle.fill", tint: Palette.muted)
        case .notDetermined: EmptyView()
        }
    }

    private func chip(_ text: String, symbol: String, tint: Color) -> some View {
        HStack(alignment: .top, spacing: 8) {
            Image(systemName: symbol).foregroundStyle(tint)
            Text(text)
                .font(BarlowFont.text(13))
                .foregroundStyle(Palette.muted)
                .multilineTextAlignment(.leading)
                .fixedSize(horizontal: false, vertical: true)
        }
        .padding(.horizontal, 16).padding(.vertical, 12)
        .background(Palette.surface)
        .clipShape(RoundedRectangle(cornerRadius: 14, style: .continuous))
        .overlay(RoundedRectangle(cornerRadius: 14, style: .continuous)
            .strokeBorder(Palette.hairline, lineWidth: 1))
    }

    private func tint(_ state: PermissionState) -> Color {
        state.isGranted ? Palette.good : Palette.accent
    }

    // Reassures the user the app pairs with the head unit, and — because the
    // Bluetooth central is live by now — shows it actually finding the device.
    private var connectStep: some View {
        VStack(spacing: 0) {
            Spacer()
            SketchIcon(glyph: ble.state == .connected ? .check : .waves,
                       tint: connectTint, active: step == 4)
                .padding(.bottom, 36)
            Text("Pair with your device")
                .font(TypeScale.screenTitle)
                .foregroundStyle(Palette.ink)
                .multilineTextAlignment(.center)
                .padding(.bottom, 14)
            Text("Turn on your OpenTrailPaper and keep it nearby. The app finds it over Bluetooth automatically — no pairing codes to type. Once linked, it stays paired and reconnects on its own every ride.")
                .font(TypeScale.body)
                .foregroundStyle(Palette.muted)
                .multilineTextAlignment(.center)
                .lineSpacing(3)
                .fixedSize(horizontal: false, vertical: true)
            connectStatusChip
                .padding(.top, 26)
            Spacer()
            Spacer()
        }
        .padding(.horizontal, 34)
        .frame(maxWidth: .infinity)
    }

    private var connectTint: Color {
        ble.state == .connected ? Palette.good : Palette.accent
    }

    private var connectStatusChip: some View {
        HStack(spacing: 10) {
            if ble.state == .connected {
                Circle().fill(Palette.good).frame(width: 10, height: 10)
            } else if ble.state == .poweredOff {
                Circle().fill(Palette.faint).frame(width: 10, height: 10)
            } else {
                ProgressView().controlSize(.small).tint(Palette.accent)
            }
            Text(connectStatusText)
                .font(TypeScale.bodyStrong)
                .foregroundStyle(Palette.ink)
        }
        .padding(.horizontal, 18)
        .padding(.vertical, 12)
        .background(Palette.surface)
        .clipShape(Capsule())
        .overlay(Capsule().strokeBorder(Palette.hairline, lineWidth: 1))
    }

    private var connectStatusText: String {
        switch ble.state {
        case .connected:  return "Connected to your device"
        case .connecting: return "Connecting…"
        case .poweredOff: return "Turn on Bluetooth to connect"
        default:          return "Looking for your device…"
        }
    }

    private var ready: some View {
        page(
            art: AnyView(SketchIcon(glyph: .check, tint: Palette.good, active: step == lastStep)),
            title: "You're all set",
            body: "Your device connects on its own whenever it's on and nearby — you'll see it on the Ride tab. Plan a route or build a map any time, and it syncs over. Anything you skipped is listed under Permissions in Settings."
        )
    }

    // MARK: pieces

    private func page(art: AnyView, title: String, body: String,
                      note: AnyView? = nil) -> some View {
        VStack(spacing: 0) {
            Spacer()
            art
                .padding(.bottom, 36)
            Text(title)
                .font(TypeScale.screenTitle)
                .foregroundStyle(Palette.ink)
                .multilineTextAlignment(.center)
                .padding(.bottom, 14)
            Text(body)
                .font(TypeScale.body)
                .foregroundStyle(Palette.muted)
                .multilineTextAlignment(.center)
                .lineSpacing(3)
                .fixedSize(horizontal: false, vertical: true)
            if let note { note.padding(.top, 22) }
            Spacer()
            Spacer()
        }
        .padding(.horizontal, 34)
        .frame(maxWidth: .infinity)
    }

    /// One of the three, as a button. The unselected rows keep their full text
    /// rather than collapsing: this is still the screen that says what the app
    /// does, and a list that hides two thirds of itself to look tidy has stopped
    /// doing that job.
    private func featureRow(_ f: Feature, selected: Bool, showBody: Bool,
                            tap: @escaping () -> Void) -> some View {
        Button(action: tap) {
            HStack(alignment: .top, spacing: 14) {
                Image(systemName: f.symbol)
                    .font(.system(size: 20, weight: .semibold))
                    .foregroundStyle(selected ? Palette.accent : Palette.faint)
                    .frame(width: 28, height: 26)
                VStack(alignment: .leading, spacing: 2) {
                    Text(f.title)
                        .font(TypeScale.title)
                        .foregroundStyle(Palette.ink)
                    if showBody {
                        Text(f.body)
                            .font(BarlowFont.text(13.5))
                            .foregroundStyle(Palette.muted)
                            .fixedSize(horizontal: false, vertical: true)
                            .multilineTextAlignment(.leading)
                    }
                }
                Spacer(minLength: 0)
            }
            .padding(.horizontal, 12)
            .padding(.vertical, 10)
            .background(selected ? Palette.accentWash : .clear)
            .clipShape(RoundedRectangle(cornerRadius: 14, style: .continuous))
            .overlay(RoundedRectangle(cornerRadius: 14, style: .continuous)
                .strokeBorder(selected ? Palette.accent.opacity(0.35) : .clear, lineWidth: 1))
        }
        .buttonStyle(.plain)
        .accessibilityAddTraits(selected ? [.isButton, .isSelected] : .isButton)
    }


    // MARK: button logic

    /// The permission this step is about, if any.
    private var permissionState: PermissionState? {
        switch step {
        case 2: return ble.locationPermission
        case 3: return ble.bluetoothPermission
        default: return nil
        }
    }

    private struct StepAction {
        enum Kind { case ask, openSettings, next, finish }
        let title: String
        let kind: Kind
    }

    /// What the primary button says and does. The permission steps are driven
    /// entirely by the current state, so the button can never be an ask that
    /// raises no prompt (iOS only prompts once) or a dead end after a refusal.
    private var stepAction: StepAction {
        if step == lastStep { return .init(title: "Start riding", kind: .finish) }
        guard let state = permissionState else { return .init(title: "Continue", kind: .next) }
        switch state {
        case .notDetermined:
            return .init(title: step == 2 ? "Allow location access" : "Enable Bluetooth",
                         kind: .ask)
        case .denied:
            return .init(title: "Open Settings", kind: .openSettings)
        case .granted, .unavailable:
            return .init(title: "Continue", kind: .next)
        }
    }

    private func primaryAction() {
        switch stepAction.kind {
        case .ask:
            if step == 2 { ble.requestLocationPermission() } else { ble.enableBluetooth() }
        case .openSettings:
            openAppSettings()
        case .next:
            advance()
        case .finish:
            onFinish()
        }
    }

    private func advance() {
        withAnimation { step = min(step + 1, lastStep) }
    }
}
