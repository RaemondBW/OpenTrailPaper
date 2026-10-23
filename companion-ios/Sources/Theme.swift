import SwiftUI
import UIKit

// Design system for the OpenTrailPaper companion app.
//
// The app looks like the e-paper head unit it pairs with: pure white paper,
// pure black ink, square corners, 2pt black rules around tiles and cards, and
// heavy condensed numerals. Emphasis is an INVERTED tile (black, white type) —
// the selected tab, primary buttons, done/connected/uploaded — rather than a
// colour. Warnings are a hatched panel, the way the device shades an area.
// The only colour left is a service's own logo (Strava's orange tile, the
// intervals.icu and Ride with GPS marks).

enum Palette {
    static let paper = Color.white                // app background
    static let surface = Color.white              // cards
    static let ink = Color.black                  // primary text, rules
    /// Secondary text. Near-black: hierarchy comes from size and weight, not
    /// from fading the ink (16:1 on white).
    static let muted = Color(hex: 0x262626)
    /// Placeholders and disabled controls only. 4.5:1 on white (WCAG AA).
    static let faint = Color(hex: 0x767676)
    /// Borders. Black like everything else; the weight is in `Rule`.
    static let hairline = Color.black
    /// Actions and live data — the ink itself now; emphasis is inversion.
    static let accent = Color.black
    static let accentDark = Color.black
    /// Behind a selected chip in screens not yet rebuilt with inverted tiles.
    static let accentWash = Color(hex: 0xEDEDED)
    static let accentInk = Color.white
    /// "Connected / success". Ink: done states are inverted, not green.
    static let good = Color.black
    /// Map hexes due an update — the one place two states must differ on the
    /// same map without a badge; a mid grey, drawn hatched by the map.
    static let update = Color(hex: 0x767676)
    /// Strava's brand orange, for its logo tile only.
    static let strava = Color(hex: 0xFC4C02)
}

/// Rule weights: tiles and cards, the line under a screen title, dividers.
enum Rule {
    static let border: CGFloat = 2
    static let title: CGFloat = 3
    static let divider: CGFloat = 2
}

// Bundled faces. Anton (heavy condensed) for numerals and screen titles,
// Barlow Condensed for compact labels, Barlow for body copy. All scale with
// Dynamic Type through `.custom(_:size:)`, which tracks the body style.
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
        case .bold, .heavy, .black: name = "Barlow-Bold"
        case .semibold:             name = "Barlow-SemiBold"
        case .medium:               name = "Barlow-Medium"
        default:                    name = "Barlow-Regular"
        }
        return .custom(name, size: size)
    }
}

/// Anton: the device's heavy condensed numerals and the screen titles.
enum DisplayFont {
    static func anton(_ size: CGFloat) -> Font { .custom("Anton-Regular", size: size) }
    /// A fixed-size variant for numbers inside fixed geometry (dashboard
    /// tiles), which scale with their box instead of with Dynamic Type.
    static func antonFixed(_ size: CGFloat) -> Font { .custom("Anton-Regular", fixedSize: size) }
}

enum TypeScale {
    // Big numerals — Anton, echoing the device hero numbers.
    static func hero(_ size: CGFloat = 68) -> Font { DisplayFont.anton(size) }
    static func value(_ size: CGFloat = 30) -> Font { DisplayFont.anton(size) }
    static let screenTitle = DisplayFont.anton(40)              // "RIDE" / "RIDES"
    static let title = BarlowFont.condensed(24, .bold)
    static let body = BarlowFont.text(16, .regular)
    static let bodyStrong = BarlowFont.text(15, .bold)
    static let label = BarlowFont.text(12, .bold)
}

extension Text {
    /// Tracked-out bold caps label, matching the device's field captions.
    func trackedLabel() -> some View {
        self.font(TypeScale.label)
            .tracking(2)
            .textCase(.uppercase)
            .foregroundStyle(Palette.ink)
    }
}

// MARK: - Surfaces

/// A content card: white, square, a 2pt black border.
struct Card<Content: View>: View {
    var padding: CGFloat = 16
    @ViewBuilder var content: Content
    var body: some View {
        content
            .padding(padding)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(Palette.surface)
            .overlay(Rectangle().strokeBorder(Palette.ink, lineWidth: Rule.border))
    }
}

extension View {
    /// A 2pt black square border, the shape of every tile.
    func einkBorder(_ width: CGFloat = Rule.border) -> some View {
        overlay(Rectangle().strokeBorder(Palette.ink, lineWidth: width))
    }
}

/// A screen's big CAPS title with the 3pt rule under it, and room for actions
/// on the right (mockups: RIDE / RIDES / ROUTE / SETTINGS).
struct ScreenHeader<Subtitle: View, Trailing: View>: View {
    let title: String
    @ViewBuilder var subtitle: Subtitle
    @ViewBuilder var trailing: Trailing

    var body: some View {
        VStack(spacing: 0) {
            VStack(alignment: .leading, spacing: 6) {
                HStack(alignment: .bottom, spacing: 8) {
                    Text(title.uppercased())
                        .font(TypeScale.screenTitle).tracking(1)
                        .foregroundStyle(Palette.ink)
                        .lineLimit(1).minimumScaleFactor(0.6)
                        .accessibilityAddTraits(.isHeader)
                    Spacer(minLength: 8)
                    trailing
                }
                // Full width under the title row, so a long status line is
                // not squeezed by the buttons beside the title.
                subtitle
            }
            .padding(.horizontal, 16).padding(.top, 8).padding(.bottom, 12)
            Rectangle().fill(Palette.ink).frame(height: Rule.title)
        }
        .background(Palette.paper)
    }
}

extension ScreenHeader where Subtitle == EmptyView {
    init(title: String, @ViewBuilder trailing: () -> Trailing) {
        self.title = title; self.subtitle = EmptyView(); self.trailing = trailing()
    }
}
extension ScreenHeader where Subtitle == EmptyView, Trailing == EmptyView {
    init(title: String) {
        self.title = title; self.subtitle = EmptyView(); self.trailing = EmptyView()
    }
}

// MARK: - Grouped lists (Settings)

/// A section's tracked CAPS header.
struct SettingsSectionHeader: View {
    let title: String
    var body: some View {
        Text(title).trackedLabel()
            .frame(maxWidth: .infinity, alignment: .leading)
            .accessibilityAddTraits(.isHeader)
    }
}

/// A bordered group of rows; put a `GroupedDivider` between rows.
struct GroupedRows<Content: View>: View {
    @ViewBuilder var content: Content
    var body: some View {
        VStack(alignment: .leading, spacing: 0) { content }
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(Palette.surface)
            .einkBorder()
    }
}

/// The 2pt rule between rows of a `GroupedRows`.
struct GroupedDivider: View {
    var body: some View {
        Rectangle().fill(Palette.ink).frame(height: Rule.divider)
            .accessibilityHidden(true)
    }
}

// MARK: - Hatching (warnings)

/// Diagonal lines, the device's way of shading an area — used for warnings
/// instead of a tint.
struct Hatch: Shape {
    var spacing: CGFloat = 6
    func path(in rect: CGRect) -> Path {
        var p = Path()
        var x = -rect.height
        while x < rect.width {
            p.move(to: CGPoint(x: rect.minX + x, y: rect.maxY))
            p.addLine(to: CGPoint(x: rect.minX + x + rect.height, y: rect.minY))
            x += spacing
        }
        return p
    }
}

struct HatchedBackground: View {
    var spacing: CGFloat = 6
    var lineWidth: CGFloat = 1.2
    var color: Color = Palette.ink.opacity(0.35)
    var body: some View {
        Hatch(spacing: spacing).stroke(color, lineWidth: lineWidth)
            .background(Palette.paper)
            .clipped()
    }
}

/// A warning: bordered, hatched, with the text on a white plate so it stays
/// black on white for reading.
struct WarningBanner<Accessory: View>: View {
    let title: String
    var message: String? = nil
    var systemImage: String = "exclamationmark.triangle.fill"
    @ViewBuilder var accessory: Accessory

    var body: some View {
        HStack(alignment: .center, spacing: 10) {
            Image(systemName: systemImage)
                .font(.system(size: 16, weight: .bold))
                .accessibilityHidden(true)
            VStack(alignment: .leading, spacing: 2) {
                Text(title.uppercased()).font(BarlowFont.text(12, .bold)).tracking(1.5)
                if let message {
                    Text(message).font(BarlowFont.text(13, .medium))
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
            Spacer(minLength: 0)
            accessory
        }
        .foregroundStyle(Palette.ink)
        .padding(.horizontal, 10).padding(.vertical, 8)
        .background(Palette.paper)
        .padding(6)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(HatchedBackground())
        .einkBorder()
        .accessibilityElement(children: .combine)
    }
}

extension WarningBanner where Accessory == EmptyView {
    init(title: String, message: String? = nil,
         systemImage: String = "exclamationmark.triangle.fill") {
        self.title = title; self.message = message; self.systemImage = systemImage
        self.accessory = EmptyView()
    }
}

// MARK: - Chips and tags

/// A small CAPS tag: inverted (done / connected) or outlined (a state).
struct Tag: View {
    let text: String
    var inverted = false
    var body: some View {
        Text(text.uppercased())
            .font(BarlowFont.text(11, .bold)).tracking(1.5)
            .lineLimit(1)
            .padding(.horizontal, inverted ? 8 : 6).padding(.vertical, inverted ? 4 : 2)
            .foregroundStyle(inverted ? Palette.paper : Palette.ink)
            .background(inverted ? Palette.ink : Palette.paper)
            .overlay { if !inverted { Rectangle().strokeBorder(Palette.ink, lineWidth: Rule.border) } }
    }
}

// MARK: - Progress

/// Indeterminate progress as a row of square blocks filling in turn — the
/// e-ink stand-in for a spinner.
struct BlockSpinner: View {
    var count = 3
    var size: CGFloat = 8
    var color: Color = Palette.ink
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    var body: some View {
        TimelineView(.periodic(from: .now, by: 0.35)) { ctx in
            let step = reduceMotion ? count - 1
                : Int(ctx.date.timeIntervalSinceReferenceDate / 0.35) % (count + 1)
            HStack(spacing: 3) {
                ForEach(0..<count, id: \.self) { i in
                    Rectangle()
                        .fill(i < step ? color : Color.clear)
                        .overlay(Rectangle().strokeBorder(color, lineWidth: 1.5))
                        .frame(width: size, height: size)
                }
            }
        }
        .accessibilityLabel("In progress")
    }
}

/// Determinate progress as segmented blocks.
struct BlockProgressBar: View {
    let value: Double          // 0...1
    var segments = 10
    var height: CGFloat = 10
    var color: Color = Palette.ink

    var body: some View {
        let filled = Int((min(max(value, 0), 1) * Double(segments)).rounded(.down))
        HStack(spacing: 3) {
            ForEach(0..<segments, id: \.self) { i in
                Rectangle()
                    .fill(i < filled ? color : Color.clear)
                    .overlay(Rectangle().strokeBorder(color, lineWidth: 1.5))
                    .frame(height: height)
            }
        }
        .accessibilityElement()
        .accessibilityLabel("Progress")
        .accessibilityValue("\(Int(value * 100)) percent")
    }
}

/// A row of seven zone blocks: filled up to the current zone, outlined above.
struct ZoneBar: View {
    let zone: Int              // 1...7
    var count = 7
    var color: Color = Palette.paper

    var body: some View {
        HStack(spacing: 4) {
            ForEach(1...count, id: \.self) { i in
                Rectangle()
                    .fill(i <= zone ? color : Color.clear)
                    .overlay(Rectangle().strokeBorder(color, lineWidth: 2))
                    .frame(height: 10)
            }
        }
        .accessibilityElement()
        .accessibilityLabel("Zone \(zone) of \(count)")
    }
}

// MARK: - Buttons

// Full-width primary action — a solid black block, white CAPS in Anton.
struct PrimaryButton: View {
    let title: String
    var systemImage: String? = nil
    var enabled: Bool = true
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            HStack(spacing: 10) {
                if let systemImage { Image(systemName: systemImage).font(.system(size: 16, weight: .bold)) }
                Text(title.uppercased()).font(DisplayFont.anton(20)).tracking(2)
                    .lineLimit(1).minimumScaleFactor(0.7)
            }
            .frame(maxWidth: .infinity, minHeight: 52)
            .padding(.horizontal, 12)
            .foregroundStyle(Palette.accentInk)
            .background(enabled ? Palette.ink : Palette.faint)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .disabled(!enabled)
    }
}

/// Small "Will sync when connected" note for settings changed in the app that
/// the device hasn't taken yet.
struct PendingSyncHint: View {
    let text: String
    var body: some View {
        HStack(spacing: 6) {
            Image(systemName: "arrow.triangle.2.circlepath")
                .font(.system(size: 11, weight: .bold))
            Text(text.uppercased()).font(BarlowFont.text(11, .bold)).tracking(1.2)
        }
        .padding(.horizontal, 8).padding(.vertical, 5)
        .foregroundStyle(Palette.ink)
        .background(Palette.paper)
        .einkBorder()
        .frame(maxWidth: .infinity, alignment: .leading)
    }
}

// Full-width secondary action — PrimaryButton's block, outlined rather than
// filled, so the eye still lands on the primary one first.
struct SecondaryButton: View {
    let title: String
    var systemImage: String? = nil
    var enabled: Bool = true
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            HStack(spacing: 8) {
                if let systemImage { Image(systemName: systemImage).font(.system(size: 14, weight: .bold)) }
                Text(title.uppercased()).font(BarlowFont.text(13, .bold)).tracking(2)
                    .lineLimit(1).minimumScaleFactor(0.7)
            }
            .frame(maxWidth: .infinity, minHeight: 44)
            .padding(.horizontal, 10)
            .foregroundStyle(enabled ? Palette.ink : Palette.faint)
            .background(Palette.paper)
            .overlay(Rectangle().strokeBorder(enabled ? Palette.ink : Palette.faint,
                                              lineWidth: Rule.border))
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .disabled(!enabled)
    }
}

/// A compact CAPS button: black (primary) or outlined, 44pt tall.
struct BlockButtonLabel: View {
    let title: String
    var systemImage: String? = nil
    var filled = true
    /// Fill the available width (a full-width secondary action).
    var expand = false
    var body: some View {
        HStack(spacing: 8) {
            if let systemImage { Image(systemName: systemImage).font(.system(size: 14, weight: .bold)) }
            Text(title.uppercased()).font(BarlowFont.text(13, .bold)).tracking(2).lineLimit(1)
                .minimumScaleFactor(0.7)
        }
        .padding(.horizontal, 14)
        .frame(maxWidth: expand ? .infinity : nil, minHeight: 44)
        .foregroundStyle(filled ? Palette.paper : Palette.ink)
        .background(filled ? Palette.ink : Palette.paper)
        .overlay { if !filled { Rectangle().strokeBorder(Palette.ink, lineWidth: Rule.border) } }
        .contentShape(Rectangle())
    }
}

/// A 44pt square icon button with a 2pt border (back, import, recenter).
struct SquareIconButton: View {
    let systemName: String
    let label: String
    var enabled = true
    var filled = false
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            Image(systemName: systemName)
                .font(.system(size: 17, weight: .bold))
                .foregroundStyle(filled ? Palette.paper : enabled ? Palette.ink : Palette.faint)
                .frame(width: 44, height: 44)
                .background(filled ? Palette.ink : Palette.paper)
                .overlay(Rectangle().strokeBorder(enabled ? Palette.ink : Palette.faint,
                                                  lineWidth: Rule.border))
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .disabled(!enabled)
        .accessibilityLabel(label)
    }
}

// MARK: - Two-segment switch

/// A binary setting as two square segments, the chosen one inverted
/// (KM | MI, ON | OFF).
struct SegmentedSwitch<Value: Hashable>: View {
    let options: [(label: String, value: Value)]
    @Binding var selection: Value
    var accessibilityName: String = ""

    var body: some View {
        HStack(spacing: 0) {
            ForEach(Array(options.enumerated()), id: \.offset) { i, o in
                let on = o.value == selection
                Button { selection = o.value } label: {
                    Text(o.label.uppercased())
                        .font(BarlowFont.text(12, .bold)).tracking(1.5)
                        .lineLimit(1)
                        .padding(.horizontal, 10)
                        .frame(minWidth: 44, minHeight: 30)
                        .foregroundStyle(on ? Palette.paper : Palette.ink)
                        .background(on ? Palette.ink : Palette.paper)
                        .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .overlay(alignment: .leading) {
                    if i > 0 { Rectangle().fill(Palette.ink).frame(width: Rule.border) }
                }
                .accessibilityLabel(accessibilityName.isEmpty ? o.label : "\(accessibilityName), \(o.label)")
                .accessibilityAddTraits(on ? .isSelected : [])
            }
        }
        .einkBorder()
    }
}

/// Every Toggle in the app as an ON | OFF two-segment switch. Set once on the
/// root view; the environment carries it into sheets and lists.
struct EinkToggleStyle: ToggleStyle {
    func makeBody(configuration: Configuration) -> some View {
        HStack(spacing: 12) {
            configuration.label
                .frame(maxWidth: .infinity, alignment: .leading)
                .layoutPriority(1)
            SegmentedSwitch(options: [("On", true), ("Off", false)],
                            selection: configuration.$isOn)
                .fixedSize()
                .accessibilityHidden(true)
        }
        .accessibilityElement(children: .combine)
        .accessibilityAddTraits(.isButton)
        .accessibilityValue(configuration.isOn ? "On" : "Off")
        .accessibilityAction { configuration.isOn.toggle() }
    }
}

// MARK: - UIKit chrome

enum EinkAppearance {
    /// Square, black-and-white UIKit controls the SwiftUI views sit on: nav
    /// bars, segmented pickers, switches, steppers.
    static func apply() {
        let ink = UIColor.black, paper = UIColor.white

        let nav = UINavigationBarAppearance()
        nav.configureWithOpaqueBackground()
        nav.backgroundColor = paper
        nav.shadowColor = ink
        nav.titleTextAttributes = [
            .foregroundColor: ink,
            .font: UIFont(name: "BarlowCondensed-Bold", size: 19) ?? .boldSystemFont(ofSize: 17),
            .kern: 1.5,
        ]
        nav.largeTitleTextAttributes = [
            .foregroundColor: ink,
            .font: UIFont(name: "Anton-Regular", size: 34) ?? .boldSystemFont(ofSize: 34),
            .kern: 1,
        ]
        let buttonFont = UIFont(name: "Barlow-Bold", size: 16) ?? .boldSystemFont(ofSize: 16)
        nav.buttonAppearance.normal.titleTextAttributes = [.foregroundColor: ink, .font: buttonFont]
        nav.doneButtonAppearance.normal.titleTextAttributes = [.foregroundColor: ink, .font: buttonFont]
        UINavigationBar.appearance().standardAppearance = nav
        UINavigationBar.appearance().scrollEdgeAppearance = nav
        UINavigationBar.appearance().compactAppearance = nav
        UINavigationBar.appearance().tintColor = ink

        // Segmented pickers: square segments, a 2pt black frame, the chosen
        // segment inverted. Background images are what drop the rounding.
        let seg = UISegmentedControl.appearance()
        seg.setBackgroundImage(image(fill: paper, border: ink), for: .normal, barMetrics: .default)
        seg.setBackgroundImage(image(fill: ink, border: ink), for: .selected, barMetrics: .default)
        seg.setBackgroundImage(image(fill: UIColor(white: 0.9, alpha: 1), border: ink),
                               for: .highlighted, barMetrics: .default)
        seg.setDividerImage(image(fill: ink, border: ink, size: CGSize(width: 2, height: 2)),
                            forLeftSegmentState: .normal, rightSegmentState: .normal, barMetrics: .default)
        seg.setDividerImage(image(fill: ink, border: ink, size: CGSize(width: 2, height: 2)),
                            forLeftSegmentState: .selected, rightSegmentState: .normal, barMetrics: .default)
        seg.setDividerImage(image(fill: ink, border: ink, size: CGSize(width: 2, height: 2)),
                            forLeftSegmentState: .normal, rightSegmentState: .selected, barMetrics: .default)
        let segFont = UIFont(name: "Barlow-Bold", size: 13) ?? .boldSystemFont(ofSize: 13)
        seg.setTitleTextAttributes([.foregroundColor: ink, .font: segFont], for: .normal)
        seg.setTitleTextAttributes([.foregroundColor: paper, .font: segFont], for: .selected)

        UISwitch.appearance().onTintColor = ink
        // Steppers: two square bordered cells with bold − / +.
        let stepper = UIStepper.appearance()
        stepper.tintColor = ink
        stepper.setBackgroundImage(image(fill: paper, border: ink), for: .normal)
        stepper.setBackgroundImage(image(fill: UIColor(white: 0.9, alpha: 1), border: ink), for: .highlighted)
        stepper.setBackgroundImage(image(fill: paper, border: UIColor(white: 0.46, alpha: 1)), for: .disabled)
        stepper.setDividerImage(image(fill: ink, border: ink, size: CGSize(width: 2, height: 2)),
                                forLeftSegmentState: .normal, rightSegmentState: .normal)
        let sym = UIImage.SymbolConfiguration(pointSize: 15, weight: .bold)
        stepper.setIncrementImage(UIImage(systemName: "plus", withConfiguration: sym), for: .normal)
        stepper.setDecrementImage(UIImage(systemName: "minus", withConfiguration: sym), for: .normal)
        UIProgressView.appearance().progressTintColor = ink
        UIProgressView.appearance().trackTintColor = UIColor(white: 0.88, alpha: 1)
        UIActivityIndicatorView.appearance().color = ink
        UITableView.appearance().separatorColor = ink
    }

    /// A tiny resizable square: `fill` inside a 2pt `border`.
    private static func image(fill: UIColor, border: UIColor,
                              size: CGSize = CGSize(width: 8, height: 8)) -> UIImage {
        let img = UIGraphicsImageRenderer(size: size).image { ctx in
            border.setFill(); ctx.fill(CGRect(origin: .zero, size: size))
            let inset: CGFloat = size.width > 4 ? 2 : 0
            fill.setFill(); ctx.fill(CGRect(origin: .zero, size: size).insetBy(dx: inset, dy: inset))
        }
        return img.resizableImage(withCapInsets: UIEdgeInsets(top: 3, left: 3, bottom: 3, right: 3),
                                  resizingMode: .stretch)
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
