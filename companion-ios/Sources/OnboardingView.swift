import SwiftUI

// First-run tutorial. Introduces what the app does, then primes and triggers
// the two system permission prompts (location, Bluetooth) on their own
// explaining screens — so the OS dialog arrives with context rather than cold
// at launch. Shown once; gated by BLEManager.onboardedKey.
struct OnboardingView: View {
    @EnvironmentObject var ble: BLEManager
    var onFinish: () -> Void

    @State private var step = OnboardingView.initialStep
    @State private var askedThisStep = false

    private let lastStep = 4

    // Screenshot support: `-onboarding-step N` opens the tutorial on a page.
    static var initialStep: Int {
        let a = ProcessInfo.processInfo.arguments
        if let i = a.firstIndex(of: "-onboarding-step"), i + 1 < a.count,
           let n = Int(a[i + 1]) { return min(max(n, 0), 4) }
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
                    ready.tag(4)
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
        .onChange(of: step) { _, _ in askedThisStep = false }
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
            PrimaryButton(title: primaryTitle, action: primaryAction)
            // Permission steps: an out on the same screen as the ask.
            if step == 2 || step == 3 {
                Button(askedThisStep ? "Continue" : "Not now") {
                    advance()
                }
                .font(TypeScale.bodyStrong)
                .foregroundStyle(Palette.muted)
                .opacity(askedThisStep ? 0 : 1)   // once asked, primary says Continue
            }
        }
        .padding(.horizontal, 24)
        .padding(.bottom, 28)
        .padding(.top, 8)
    }

    // MARK: pages

    private var welcome: some View {
        page(
            art: AnyView(deviceGlyph),
            title: "Welcome to OpenTrailPaper",
            body: "The companion for your e-paper bike computer. Plan routes, bake offline maps, and pull your rides — the phone does the fiddly bits, the device does the riding."
        )
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
            art: AnyView(iconBadge("location.fill", tint: Palette.accent)),
            title: "Share your location",
            body: "Used to show your position on the map, warm-start the device's GPS so it locks on fast, and act as a backup fix when the device can't see the sky. Only while you're using the app."
        )
    }

    private var bluetoothStep: some View {
        page(
            art: AnyView(iconBadge("dot.radiowaves.left.and.right", tint: Palette.accent)),
            title: "Connect over Bluetooth",
            body: "Everything travels to and from your OpenTrailPaper over Bluetooth — routes, offline maps, settings and recorded rides. No account, no cloud."
        )
    }

    private var ready: some View {
        page(
            art: AnyView(iconBadge("checkmark", tint: Palette.good)),
            title: "You're all set",
            body: "Turn on your OpenTrailPaper and open the Ride tab — the app finds it automatically. You can change permissions any time in Settings."
        )
    }

    // MARK: pieces

    private func page(art: AnyView, title: String, body: String) -> some View {
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

    // A little cream "device" nodding to the head unit.
    private var deviceGlyph: some View {
        ZStack {
            RoundedRectangle(cornerRadius: 22, style: .continuous)
                .fill(Palette.surface)
                .overlay(RoundedRectangle(cornerRadius: 22, style: .continuous)
                    .strokeBorder(Palette.hairline, lineWidth: 1.5))
                .frame(width: 116, height: 150)
            VStack(spacing: 10) {
                Image(systemName: "bicycle")
                    .font(.system(size: 40, weight: .semibold))
                    .foregroundStyle(Palette.ink)
                Capsule().fill(Palette.accent).frame(width: 44, height: 6)
                Capsule().fill(Palette.hairline).frame(width: 60, height: 6)
            }
        }
    }

    // MARK: button logic

    private var primaryTitle: String {
        switch step {
        case 2: return askedThisStep ? "Continue" : "Allow location access"
        case 3: return askedThisStep ? "Continue" : "Enable Bluetooth"
        case lastStep: return "Start riding"
        default: return "Continue"
        }
    }

    private func primaryAction() {
        switch step {
        case 2:
            if askedThisStep { advance() }
            else { ble.requestLocationPermission(); askedThisStep = true }
        case 3:
            if askedThisStep { advance() }
            else { ble.enableBluetooth(); askedThisStep = true }
        case lastStep:
            onFinish()
        default:
            advance()
        }
    }

    private func advance() {
        withAnimation { step = min(step + 1, lastStep) }
    }
}
