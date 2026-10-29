import Foundation
import SwiftUI

// Starred Strava segments -> the device's SEGMENT page. One action, "Sync
// starred segments", under the Strava row in Settings > Accounts: it is a
// Strava thing that only makes sense with that account connected, and like a
// map download it is done once in a while, not per ride.
//
// Each segment's line and altitude come from its streams (one request), cut
// to the device's 200 points and cached here by segment id (a segment never
// changes), so after the first sync only the starred list is fetched again —
// one or two requests, well inside Strava's rate limits. A segment whose
// streams have no line falls back to its detail's polyline, without a profile.
// Mirrors SegmentSync in the Android app (data/SegmentSync.kt).
@MainActor
final class SegmentSync: ObservableObject {
    static let shared = SegmentSync()

    @Published private(set) var busy = false
    @Published private(set) var status: String?
    @Published private(set) var failed = false
    @Published private(set) var lastSync: Date? = UserDefaults.standard.object(forKey: "segments.lastSync") as? Date

    private var cache: [UInt64: SegmentGeometry] = [:]
    private static var cacheURL: URL {
        let dir = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir.appendingPathComponent("segment-geometry.json")
    }

    init() {
        if let d = try? Data(contentsOf: Self.cacheURL),
           let m = try? JSONDecoder().decode([String: SegmentGeometry].self, from: d) {
            for (k, v) in m { if let id = UInt64(k) { cache[id] = v } }
        }
    }

    private func saveCache() {
        let m = Dictionary(uniqueKeysWithValues: cache.map { (String($0.key), $0.value) })
        if let d = try? JSONEncoder().encode(m) { try? d.write(to: Self.cacheURL, options: .atomic) }
    }

    func sync(ble: BLEManager) {
        guard !busy else { return }
        busy = true
        failed = false
        status = "Reading your starred segments…"
        Task {
            defer { busy = false }
            do {
                let starred = try await SyncAccounts.shared.starredSegments()
                let rides = starred.filter(\.isRide)
                var prs: [UInt64: Int] = [:]
                var limited = false
                let wanted = rides.prefix(StravaSegments.maxSegments)
                let missing = wanted.filter { cache[$0.id] == nil }
                for (i, s) in missing.enumerated() {
                    status = "Fetching segment \(i + 1) of \(missing.count)…"
                    do {
                        if let st = try await SyncAccounts.shared.segmentStreams(s.id) {
                            cache[s.id] = StravaSegments.geometry(points: st.points, altitudes: st.altitudes)
                        } else {
                            // No line in the streams: the detail's polyline, no profile.
                            let d = try await SyncAccounts.shared.segmentDetail(s.id)
                            if let poly = d.polyline, let pts = CloudRoutes.decodePolyline(poly), pts.count >= 2 {
                                cache[s.id] = StravaSegments.geometry(points: pts, altitudes: nil)
                            }
                            if let pr = d.prSeconds { prs[s.id] = pr }
                        }
                    } catch SyncError.http(404, _, _) {
                        continue   // gone from Strava: leave it out
                    } catch SyncError.http(429, _, _) {
                        limited = true   // send what there is; the rest next time
                        break
                    }
                }
                saveCache()
                let segs = StravaSegments.deviceSegments(Array(wanted), geometry: cache, prs: prs)
                guard ble.state == .connected else {
                    fail("Connect your OpenTrailPaper, then sync again.")
                    return
                }
                status = "Sending \(segs.count) segment\(segs.count == 1 ? "" : "s") to the device…"
                // Elevation (v2) only to firmware that said it reads it.
                let version = min(StravaSegments.blobVersion, max(1, ble.deviceSegmentVersion))
                guard await ble.uploadSegments(StravaSegments.encode(segs, version: version)) else {
                    fail(ble.segmentsSupported == false
                         ? "The device needs firmware v1.23 or newer for segments."
                         : "The device did not confirm the segments. Try again.")
                    return
                }
                lastSync = Date()
                UserDefaults.standard.set(lastSync, forKey: "segments.lastSync")
                var note = "\(segs.count) starred segment\(segs.count == 1 ? "" : "s") on the device."
                if rides.count > StravaSegments.maxSegments {
                    note += " The first \(StravaSegments.maxSegments) of your \(rides.count) are kept."
                }
                if starred.count > rides.count {
                    note += " \(starred.count - rides.count) non-ride segment\(starred.count - rides.count == 1 ? " was" : "s were") left out."
                }
                if limited {
                    note += " Strava is limiting requests, so some are missing; sync again in 15 minutes."
                }
                if version < 2 {
                    note += " Update the device firmware to see their elevation profiles."
                }
                status = note
            } catch {
                fail(error.localizedDescription)
            }
        }
    }

    private func fail(_ message: String) {
        failed = true
        status = message
    }
}

/// The Strava row's segments line in Settings > Accounts: how many are on the
/// device and the sync button. Shown only while Strava is connected.
struct StravaSegmentsRow: View {
    @EnvironmentObject var ble: BLEManager
    @ObservedObject private var sync = SegmentSync.shared
    @ObservedObject private var accounts = SyncAccounts.shared

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack(spacing: 12) {
                VStack(alignment: .leading, spacing: 2) {
                    Text("Starred segments").font(BarlowFont.text(14, .semibold)).foregroundStyle(Palette.ink)
                    Text(summary).font(.system(size: 12)).foregroundStyle(Palette.muted)
                }
                Spacer()
                if sync.busy {
                    ProgressView()
                } else {
                    Button("Sync") { sync.sync(ble: ble) }
                        .font(BarlowFont.text(14, .semibold))
                        .foregroundStyle(canSync ? Palette.accent : Palette.muted)
                        .disabled(!canSync)
                }
            }
            if let s = sync.status, sync.busy || sync.failed || ble.state == .connected {
                Text(s).font(.system(size: 12))
                    .foregroundStyle(sync.failed ? Palette.accentDark : Palette.muted)
            }
            if !CloudRoutes.stravaSeesPrivateRoutes(accounts.tokens[.strava]?.scope) {
                Text("Private segments need a fresh sign-in: disconnect and connect Strava again.")
                    .font(.system(size: 12)).foregroundStyle(Palette.muted)
            }
            Text("Powered by Strava").font(.system(size: 11, weight: .semibold))
                .foregroundStyle(Color(hex: 0xFC4C02))
        }
        .padding(.leading, 12)
        .task(id: ble.state) {
            if ble.state == .connected { ble.querySegments() }
        }
    }

    private var canSync: Bool { ble.state == .connected && ble.segmentsSupported != false }

    private var summary: String {
        guard ble.state == .connected else { return "Connect the device to sync them to its SEGMENT page." }
        if ble.segmentsSupported == false { return "Needs device firmware v1.23 or newer." }
        guard let n = ble.deviceSegmentCount else { return "None on the device yet." }
        var s = "\(n) on the device"
        if let d = sync.lastSync {
            s += " · synced \(d.formatted(.relative(presentation: .named)))"
        }
        return s
    }
}
