import SwiftUI

// Design system for the Bike GPS companion app.
//
// The language borrows from the e-paper head unit it pairs with: warm
// off-white "paper", near-black ink, heavy condensed numerals, generous
// tracking on small labels, and a single high-energy accent for live data
// and primary actions. Restrained, high-contrast, glanceable.

// Palette sampled directly from the design mockup (warm e-ink cream, a single
// vermilion accent, warm grays, a muted green for "connected/good").
enum Palette {
    static let paper = Color(hex: 0xEDEAE1)       // app background (warm cream)
    static let surface = Color(hex: 0xFAF8F3)     // cards (slightly lighter)
    static let ink = Color(hex: 0x1A1A1A)         // primary text
    static let muted = Color(hex: 0x5C564B)       // secondary text (darker warm gray, readable)
    static let faint = Color(hex: 0x9A9488)       // placeholders / tertiary
    static let hairline = Color(hex: 0xDBD6C6)    // separators / borders
    static let accent = Color(hex: 0xF4501E)      // vermilion — actions, live
    static let accentDark = Color(hex: 0xB9541E)  // pressed / on-accent detail
    static let accentWash = Color(hex: 0xFBE9E0)  // selected chip / accent tint bg
    static let accentInk = Color.white
    static let good = Color(hex: 0x2E7D5B)        // connected / success
}

// Bundled Barlow: condensed for numerals/titles/labels (athletic, echoes the
// device's Impact numerals); regular Barlow for body copy.
enum BarlowFont {
    static func condensed(_ size: CGFloat, _ weight: Font.Weight = .semibold) -> Font {
        let name: String
        switch weight {
        case .bold, .heavy, .black: name = "BarlowCondensed-Bold"
        case .medium, .regular:     name = "BarlowCondensed-Medium"
        default:                    name = "BarlowCondensed-SemiBold"
        }
        return .custom(name, size: size)
    }
    static func text(_ size: CGFloat, _ weight: Font.Weight = .regular) -> Font {
        let name: String
        switch weight {
        case .semibold, .bold, .heavy, .black: name = "Barlow-SemiBold"
        case .medium:                          name = "Barlow-Medium"
        default:                               name = "Barlow-Regular"
        }
        return .custom(name, size: size)
    }
}

enum TypeScale {
    // Big numerals — heavy condensed, echoing the device hero numbers.
    static func hero(_ size: CGFloat = 68) -> Font { BarlowFont.condensed(size, .bold) }
    static func value(_ size: CGFloat = 30) -> Font { BarlowFont.condensed(size, .semibold) }
    static let screenTitle = BarlowFont.condensed(38, .bold)   // "Ride" / "Rides"
    static let title = BarlowFont.condensed(24, .semibold)
    static let body = BarlowFont.text(16, .regular)
    static let bodyStrong = BarlowFont.text(15, .semibold)
    static let label = BarlowFont.condensed(13, .medium)
}

extension Text {
    /// Tracked-out uppercase label, matching the device's field captions.
    func trackedLabel() -> some View {
        self.font(TypeScale.label)
            .tracking(1.2)
            .textCase(.uppercase)
            .foregroundStyle(Palette.muted)
    }
}

// A soft content card — the mockup's rounded, hairline-bordered surface.
struct Card<Content: View>: View {
    var padding: CGFloat = 16
    @ViewBuilder var content: Content
    var body: some View {
        content
            .padding(padding)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(Palette.surface)
            .clipShape(RoundedRectangle(cornerRadius: 18, style: .continuous))
            .overlay(RoundedRectangle(cornerRadius: 18, style: .continuous)
                .strokeBorder(Palette.hairline, lineWidth: 1))
    }
}

// Full-width primary action — fully rounded vermilion pill (mockup 2a/2b).
struct PrimaryButton: View {
    let title: String
    var systemImage: String? = nil
    var enabled: Bool = true
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            HStack(spacing: 8) {
                if let systemImage { Image(systemName: systemImage) }
                Text(title).font(BarlowFont.condensed(21, .semibold))
            }
            .frame(maxWidth: .infinity)
            .padding(.vertical, 16)
            .foregroundStyle(Palette.accentInk)
            .background(enabled ? Palette.accent : Palette.faint)
            .clipShape(RoundedRectangle(cornerRadius: 26, style: .continuous))
        }
        .disabled(!enabled)
    }
}

extension Color {
    init(hex: UInt32) {
        self.init(
            .sRGB,
            red: Double((hex >> 16) & 0xFF) / 255,
            green: Double((hex >> 8) & 0xFF) / 255,
            blue: Double(hex & 0xFF) / 255,
            opacity: 1)
    }
}

// Display-unit conversions. Ride data is stored/decoded in metric; these
// convert only at display time based on the user's preference. The shared
// preference key is read via @AppStorage(UnitPref.key) in each view.
enum UnitPref {
    static let key = "useMiles"
}

enum Units {
    static func distance(_ km: Double, miles: Bool) -> Double {
        miles ? km * 0.621371 : km
    }
    static func speed(_ kmh: Double, miles: Bool) -> Double {
        miles ? kmh * 0.621371 : kmh
    }
    static func elevation(_ m: Double, miles: Bool) -> Double {
        miles ? m * 3.28084 : m
    }
    static func distLabel(_ miles: Bool) -> String { miles ? "mi" : "km" }
    static func speedLabel(_ miles: Bool) -> String { miles ? "mph" : "km/h" }
    static func elevLabel(_ miles: Bool) -> String { miles ? "ft" : "m" }
}
