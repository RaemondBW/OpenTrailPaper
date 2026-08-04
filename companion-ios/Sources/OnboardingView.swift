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
            body: "This is the companion for the OpenTrailPaper head unit — a sunlight-readable e-paper bike computer you build yourself. The phone does the fiddly bits; the device does the riding."
        )
    }

    // A product shot of the head unit so it's obvious from the first screen
    // what this app pairs with — the map screen tucked behind the dashboard.
    private var deviceHero: some View {
        ZStack {
            Image("DeviceMap")
                .resizable().scaledToFit()
                .frame(width: 148)
                .rotationEffect(.degrees(-9))
                .offset(x: -84, y: 12)
            Image("DeviceDashboard")
                .resizable().scaledToFit()
                .frame(width: 182)
                .rotationEffect(.degrees(5))
                .offset(x: 26)
        }
        .frame(height: 300)
    }

    private var overview: some View {
        VStack(spacing: 0) {
            Spacer(minLength: 0)
            VStack(alignment: .leading, spacing: 18) {
                Text("What you'll do here")
                    .font(TypeScale.screenTitle)
                    .foregroundStyle(Palette.ink)
                    .padding(.bottom, 4)
                featureRow("map", "Plan routes", "Search a destination and send the route to your device as GPX.")
                featureRow("square.and.arrow.down.on.square", "Build offline maps", "Pick an area; the app bakes map tiles onto the device's SD card.")
                featureRow("list.bullet.rectangle", "Review rides", "Pull recorded rides off the device and see distance, power and climb.")
            }
            .padding(28)
            .frame(maxWidth: .infinity, alignment: .leading)
            Spacer(minLength: 0)
        }
        .padding(.horizontal, 20)
    }

    private var locationStep: some View {
        page(
            art: AnyView(iconBadge("location.fill", tint: tint(ble.locationPermission))),
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
            art: AnyView(iconBadge("dot.radiowaves.left.and.right", tint: tint(ble.bluetoothPermission))),
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
            iconBadge(connectSymbol, tint: connectTint)
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

    private var connectSymbol: String {
        ble.state == .connected ? "checkmark.circle.fill" : "antenna.radiowaves.left.and.right"
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
            art: AnyView(iconBadge("checkmark", tint: Palette.good)),
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

    private func featureRow(_ symbol: String, _ title: String, _ body: String) -> some View {
        HStack(alignment: .top, spacing: 16) {
            Image(systemName: symbol)
                .font(.system(size: 22, weight: .semibold))
                .foregroundStyle(Palette.accent)
                .frame(width: 30, height: 30)
            VStack(alignment: .leading, spacing: 3) {
                Text(title)
                    .font(TypeScale.title)
                    .foregroundStyle(Palette.ink)
                Text(body)
                    .font(TypeScale.body)
                    .foregroundStyle(Palette.muted)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
    }

    private func iconBadge(_ symbol: String, tint: Color) -> some View {
        ZStack {
            Circle()
                .fill(tint.opacity(0.12))
                .frame(width: 132, height: 132)
            Image(systemName: symbol)
                .font(.system(size: 56, weight: .semibold))
                .foregroundStyle(tint)
        }
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
