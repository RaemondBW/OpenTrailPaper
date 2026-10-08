import ActivityKit
import Foundation

// The one Live Activity the app runs: "a long transfer is going on".
//
// Compiled into BOTH the app (which starts, updates and ends the activity
// through TransferCenter) and the TransferWidget extension (which draws it on
// the Lock Screen and in the Dynamic Island). The two processes share nothing
// but this type — ActivityKit hands the encoded ContentState across — so no App
// Group is needed.

/// What is being moved. Drives the icon and the default wording.
enum TransferKind: String, Codable, Hashable, Sendable {
    case rideDownload      // .fit off the device over BLE
    case logDownload       // diagnostics log off the device over BLE
    case firmware          // GitHub download + OTA to the device
    case mapTiles          // H3 map tiles built here and streamed to the SD card
    case mapUpload         // one whole .ebm map to the device
    case route             // GPX + turn cues to the device
    case cloudUpload       // a ride to Strava / Intervals.icu / RideWithGPS

    /// SF Symbol shown in the Dynamic Island and on the Lock Screen.
    var symbol: String {
        switch self {
        case .rideDownload: return "arrow.down.circle"
        case .logDownload:  return "doc.text"
        case .firmware:     return "cpu"
        case .mapTiles:     return "map"
        case .mapUpload:    return "map"
        case .route:        return "point.topleft.down.to.point.bottomright.curvepath"
        case .cloudUpload:  return "icloud.and.arrow.up"
        }
    }
}

/// How `completed` / `total` are counted.
enum TransferUnit: String, Codable, Hashable, Sendable {
    case bytes
    case items      // e.g. map tiles
}

struct TransferActivityAttributes: ActivityAttributes {
    struct ContentState: Codable, Hashable {
        enum Phase: String, Codable, Hashable { case running, finished, failed }

        var kind: TransferKind
        var title: String
        /// One short line under the title ("Ride_0412.fit", "Installing…",
        /// or the outcome once it ends).
        var detail: String
        /// 0...1, or nil while the step has no measurable size (waiting for the
        /// device to reboot, Strava processing, building tiles).
        var fraction: Double?
        var completed: Int64
        var total: Int64
        var unit: TransferUnit
        /// When it should be done, for a live countdown that needs no updates.
        var eta: Date?
        var phase: Phase
        /// Other transfers running or waiting behind this one.
        var others: Int
    }

    /// When the first transfer of this activity began.
    var startedAt: Date
}

extension TransferActivityAttributes.ContentState {
    /// "1.2 of 3.4 MB", "12 of 40 tiles", or "" when nothing is countable.
    var amountText: String {
        guard total > 0 else { return "" }
        switch unit {
        case .items:
            return "\(completed) of \(total) \(kind == .mapTiles ? "tiles" : "items")"
        case .bytes:
            let f = ByteCountFormatter()
            f.countStyle = .file
            f.allowedUnits = total >= 1_000_000 ? [.useMB] : [.useKB]
            let done = f.string(fromByteCount: completed)
            let all = f.string(fromByteCount: total)
            return "\(done) of \(all)"
        }
    }

    var percentText: String {
        guard let fraction else { return "" }
        return "\(Int((fraction * 100).rounded(.down)))%"
    }
}
