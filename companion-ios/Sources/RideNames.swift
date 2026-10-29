import Foundation

// Names the rider gave rides, keyed by the ride's file name on the device
// (which is also its cached file's name). Set when a ride is renamed before an
// upload, so the next upload (to another account) starts from the same name
// and the ride list and ride page show it. Kept on this phone only; the device
// and its files are untouched. Mirrors RideNames in the Android app.
@MainActor
final class RideNames: ObservableObject {
    static let shared = RideNames()
    private static let key = "rideNames"
    /// Comfortably inside what Strava, Intervals.icu and RideWithGPS accept.
    static let maxLength = 100

    private let defaults: UserDefaults
    @Published private(set) var names: [String: String]

    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
        names = (defaults.dictionary(forKey: Self.key) as? [String: String]) ?? [:]
    }

    /// The rider's name for this ride, or `fallback` when it has none.
    func name(for file: String, default fallback: String) -> String {
        names[file] ?? fallback
    }

    /// Saves what the rider typed and returns the name to upload under. Blank
    /// (or the default itself) means the default, and forgets any earlier rename.
    @discardableResult
    func rename(_ file: String, to input: String, default fallback: String) -> String {
        let name = Self.cleaned(input) ?? fallback
        names[file] = name == fallback ? nil : name
        defaults.set(names, forKey: Self.key)
        return name
    }

    func forget(_ file: String) {
        guard names[file] != nil else { return }
        names[file] = nil
        defaults.set(names, forKey: Self.key)
    }

    /// One line, trimmed, at most maxLength characters; nil when nothing is left.
    nonisolated static func cleaned(_ input: String) -> String? {
        let words = input.split(whereSeparator: { $0.isWhitespace || $0.isNewline })
        let line = words.joined(separator: " ")
        guard !line.isEmpty else { return nil }
        return String(line.prefix(maxLength)).trimmingCharacters(in: .whitespaces)
    }
}
