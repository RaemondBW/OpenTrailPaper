import ActivityKit
import SwiftUI
import WidgetKit

// Lock Screen banner + Dynamic Island for a running transfer. All state comes
// from TransferActivityAttributes.ContentState (Shared/), pushed by the app's
// TransferCenter.

private enum Ink {
    static let accent = Color(red: 0xF4 / 255, green: 0x50 / 255, blue: 0x1E / 255)  // vermilion
    static let good = Color(red: 0x2E / 255, green: 0x7D / 255, blue: 0x5B / 255)
    static let paper = Color(red: 0xFA / 255, green: 0xF8 / 255, blue: 0xF3 / 255)
    static let ink = Color(red: 0x1A / 255, green: 0x1A / 255, blue: 0x1A / 255)
    static let muted = Color(red: 0x5C / 255, green: 0x56 / 255, blue: 0x4B / 255)
}

private extension TransferActivityAttributes.ContentState {
    var tint: Color {
        switch phase {
        case .running: return Ink.accent
        case .finished: return Ink.good
        case .failed: return .red
        }
    }

    var statusSymbol: String {
        switch phase {
        case .running: return kind.symbol
        case .finished: return "checkmark.circle.fill"
        case .failed: return "exclamationmark.triangle.fill"
        }
    }

    var othersText: String? {
        others > 0 ? "+\(others) more" : nil
    }
}

struct TransferLiveActivity: Widget {
    var body: some WidgetConfiguration {
        ActivityConfiguration(for: TransferActivityAttributes.self) { context in
            LockScreenView(state: context.state)
                .activityBackgroundTint(Ink.paper)
                .activitySystemActionForegroundColor(Ink.ink)
        } dynamicIsland: { context in
            let s = context.state
            return DynamicIsland {
                DynamicIslandExpandedRegion(.leading) {
                    Image(systemName: s.statusSymbol)
                        .font(.title2)
                        .foregroundStyle(s.tint)
                        .padding(.leading, 4)
                }
                DynamicIslandExpandedRegion(.trailing) {
                    if s.phase == .running, let f = s.fraction {
                        PercentRing(fraction: f, text: s.percentText, tint: s.tint)
                            .padding(.trailing, 4)
                    } else if s.phase == .running {
                        // Indeterminate step: a spinner in the ring's place.
                        ProgressView().progressViewStyle(.circular).tint(s.tint)
                            .frame(width: PercentRing.size, height: PercentRing.size)
                            .padding(.trailing, 4)
                    }
                }
                DynamicIslandExpandedRegion(.center) {
                    Text(s.title).font(.headline).lineLimit(1)
                }
                DynamicIslandExpandedRegion(.bottom) {
                    VStack(alignment: .leading, spacing: 6) {
                        ProgressBar(state: s)
                        DetailLine(state: s, color: .white.opacity(0.7))
                    }
                    .padding(.horizontal, 4)
                }
            } compactLeading: {
                // The app's own mark rather than a stock symbol, so the island
                // reads as OpenTrailPaper at a glance; the trailing side says
                // how far along it is (or that it finished / failed).
                IslandLogo(height: 22)
            } compactTrailing: {
                if s.phase == .running, let f = s.fraction {
                    Text("\(Int((f * 100).rounded(.down)))%")
                        .font(.caption.monospacedDigit().weight(.semibold))
                        .foregroundStyle(s.tint)
                        .frame(minWidth: 30)
                } else if s.phase == .running {
                    ProgressView().progressViewStyle(.circular).tint(s.tint)
                } else {
                    Image(systemName: s.phase == .finished ? "checkmark" : "xmark")
                        .foregroundStyle(s.tint)
                }
            } minimal: {
                if s.phase == .running, let f = s.fraction {
                    ProgressView(value: f) { IslandLogo(height: 13) }
                        .progressViewStyle(.circular)
                        .tint(s.tint)
                } else if s.phase == .running {
                    IslandLogo(height: 20)
                } else {
                    Image(systemName: s.statusSymbol).foregroundStyle(s.tint)
                }
            }
            .keylineTint(s.tint)
        }
    }
}

/// The device mark from the app icon, drawn white for the island's black
/// background (TransferWidget/Assets.xcassets, vector).
private struct IslandLogo: View {
    let height: CGFloat

    var body: some View {
        Image("IslandLogo")
            .resizable()
            .scaledToFit()
            .frame(height: height)
            .accessibilityLabel("OpenTrailPaper")
    }
}

/// The expanded island's percentage, with the progress drawn as a ring around it.
private struct PercentRing: View {
    static let size: CGFloat = 46
    let fraction: Double
    let text: String
    let tint: Color

    var body: some View {
        ZStack {
            Circle().stroke(tint.opacity(0.25), lineWidth: 4)
            Circle()
                .trim(from: 0, to: min(max(fraction, 0), 1))
                .stroke(tint, style: StrokeStyle(lineWidth: 4, lineCap: .round))
                .rotationEffect(.degrees(-90))
            Text(text)
                .font(.system(size: 13, weight: .semibold).monospacedDigit())
                .foregroundStyle(tint)
                .minimumScaleFactor(0.7)
                .lineLimit(1)
                .padding(.horizontal, 5)
        }
        .frame(width: Self.size, height: Self.size)
    }
}

private struct LockScreenView: View {
    let state: TransferActivityAttributes.ContentState

    var body: some View {
        HStack(alignment: .top, spacing: 12) {
            Image(systemName: state.statusSymbol)
                .font(.title2)
                .foregroundStyle(state.tint)
                .frame(width: 30)
            VStack(alignment: .leading, spacing: 6) {
                HStack(alignment: .firstTextBaseline) {
                    Text(state.title)
                        .font(.headline)
                        .foregroundStyle(Ink.ink)
                        .lineLimit(1)
                    Spacer(minLength: 8)
                    if state.phase == .running {
                        Text(state.percentText)
                            .font(.headline.monospacedDigit())
                            .foregroundStyle(state.tint)
                    }
                }
                ProgressBar(state: state)
                DetailLine(state: state)
            }
        }
        .padding(16)
    }
}

private struct ProgressBar: View {
    let state: TransferActivityAttributes.ContentState

    var body: some View {
        switch state.phase {
        case .running:
            if let f = state.fraction {
                ProgressView(value: f).tint(state.tint)
            } else {
                // Indeterminate step (installing, processing, building).
                ProgressView(value: 0.0).tint(state.tint).opacity(0.35)
            }
        case .finished:
            ProgressView(value: 1.0).tint(state.tint)
        case .failed:
            EmptyView()
        }
    }
}

private struct DetailLine: View {
    let state: TransferActivityAttributes.ContentState
    var color: Color = Ink.muted

    var body: some View {
        HStack(spacing: 6) {
            Text(state.detail)
                .lineLimit(1)
                .truncationMode(.middle)
            Spacer(minLength: 4)
            if state.phase == .running {
                if !state.amountText.isEmpty, state.fraction != nil {
                    Text(state.amountText).monospacedDigit()
                }
                if let eta = state.eta, eta > Date() {
                    Text("·")
                    Text(timerInterval: Date()...eta, countsDown: true)
                        .monospacedDigit()
                        .frame(maxWidth: 52)
                    Text("left")
                }
                if let more = state.othersText { Text("· \(more)") }
            }
        }
        .font(.caption)
        .foregroundStyle(color)
    }
}
