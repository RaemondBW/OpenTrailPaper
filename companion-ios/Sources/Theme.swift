import SwiftUI

// Design system for the Bike GPS companion app.
//
// The language borrows from the e-paper head unit it pairs with: warm
// off-white "paper", near-black ink, heavy condensed numerals, generous
// tracking on small labels, and a single high-energy accent for live data
// and primary actions. Restrained, high-contrast, glanceable.

enum Palette {
    static let paper = Color(hex: 0xF7F5F0)      // app background
    static let surface = Color.white              // cards
    static let ink = Color(hex: 0x171410)         // primary text
    static let muted = Color(hex: 0x8A8377)       // secondary text
    static let hairline = Color(hex: 0xE4E0D7)    // separators / borders
    static let accent = Color(hex: 0xFF4A22)      // vermilion — actions, live
    static let accentInk = Color.white
    static let good = Color(hex: 0x1F8A5B)         // connected / success
}

enum TypeScale {
    // Big numerals — rounded + heavy, echoing the device hero numbers.
    static func hero(_ size: CGFloat = 64) -> Font {
        .system(size: size, weight: .heavy, design: .rounded)
    }
    static func value(_ size: CGFloat = 34) -> Font {
        .system(size: size, weight: .bold, design: .rounded)
    }
    static let title = Font.system(size: 22, weight: .bold)
    static let body = Font.system(size: 16, weight: .medium)
    static let label = Font.system(size: 12, weight: .semibold)
}

extension Text {
    /// Tracked-out uppercase label, matching the device's field captions.
    func trackedLabel() -> some View {
        self.font(TypeScale.label)
            .tracking(1.6)
            .textCase(.uppercase)
            .foregroundStyle(Palette.muted)
    }
}

// A bordered content card.
struct Card<Content: View>: View {
    @ViewBuilder var content: Content
    var body: some View {
        content
            .padding(18)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(Palette.surface)
            .overlay(RoundedRectangle(cornerRadius: 18)
                .strokeBorder(Palette.hairline, lineWidth: 1))
            .clipShape(RoundedRectangle(cornerRadius: 18))
    }
}

// Full-width primary button.
struct PrimaryButton: View {
    let title: String
    var systemImage: String? = nil
    var enabled: Bool = true
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            HStack(spacing: 8) {
                if let systemImage { Image(systemName: systemImage) }
                Text(title).font(.system(size: 17, weight: .bold))
            }
            .frame(maxWidth: .infinity)
            .padding(.vertical, 16)
            .foregroundStyle(Palette.accentInk)
            .background(enabled ? Palette.accent : Palette.muted)
            .clipShape(RoundedRectangle(cornerRadius: 14))
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
