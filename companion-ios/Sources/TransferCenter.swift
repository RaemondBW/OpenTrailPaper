import ActivityKit
import Foundation
import UIKit

// Every long transfer in the app reports here — ride and log downloads, the
// firmware update, map tiles, route sends, uploads to Strava and friends — and
// this is the only place that knows about Live Activities and background time.
//
// Call sites say three things: begin(id), update(id, …) as often as they like,
// finish(id). Everything else happens here:
//
//  - A Live Activity (Lock Screen + Dynamic Island) for whichever transfer is
//    newest, with a count of the others. It is started only once a transfer
//    has run for a moment — a two-second route send should not flash one up —
//    or immediately when the app is about to leave the screen, since an
//    activity can only be STARTED while the app is in the foreground.
//  - Updates are coalesced to one per second (ActivityKit throttles anything
//    faster, and the system budgets how often a Lock Screen redraws). Phase
//    changes still go out promptly. The ETA is sent as a date, so the
//    countdown ticks on the Lock Screen without any updates at all.
//  - The activity ends with the outcome on it and lingers briefly.
//  - While anything is running the app holds a background task, so a timer a
//    transfer depends on (route pacing, watchdogs, an HTTP request) is not
//    frozen the instant the phone locks. BLE work itself keeps going through
//    the `bluetooth-central` background mode.
@MainActor
final class TransferCenter: ObservableObject {
    static let shared = TransferCenter()

    struct Transfer: Identifiable, Equatable {
        let id: String
        var kind: TransferKind
        var title: String
        var detail: String
        var completed: Int64 = 0
        var total: Int64 = 0
        var unit: TransferUnit = .bytes
        /// No measurable size right now (rebooting, processing, building).
        var indeterminate = false
        /// More of the same queued behind this one (e.g. rides).
        var waiting = 0
        let startedAt: Date
        var phase: TransferActivityAttributes.ContentState.Phase = .running

        // Throughput, for the ETA: a smoothed rate from successive updates.
        fileprivate var lastSample: (date: Date, completed: Int64)?
        fileprivate var rate: Double = 0    // units per second

        var fraction: Double? {
            guard !indeterminate, total > 0 else { return nil }
            return min(1, max(0, Double(completed) / Double(total)))
        }

        var eta: Date? {
            guard phase == .running, let fraction, fraction < 1, rate > 0,
                  Date().timeIntervalSince(startedAt) > 2 else { return nil }
            return Date().addingTimeInterval(Double(total - completed) / rate)
        }

        static func == (a: Transfer, b: Transfer) -> Bool {
            a.id == b.id && a.kind == b.kind && a.title == b.title && a.detail == b.detail
                && a.completed == b.completed && a.total == b.total && a.unit == b.unit
                && a.indeterminate == b.indeterminate && a.waiting == b.waiting
                && a.phase == b.phase
        }
    }

    /// Running transfers, oldest first.
    @Published private(set) var active: [Transfer] = []

    /// The last transfer to end, for the activity's closing state.
    private var lastEnded: Transfer?

    private var activity: Activity<TransferActivityAttributes>?
    private var lastPush = Date.distantPast
    private var lastPushed: TransferActivityAttributes.ContentState?
    private var pushTask: Task<Void, Never>?
    private var startTask: Task<Void, Never>?
    private var settleTask: Task<Void, Never>?
    private var backgroundTask: UIBackgroundTaskIdentifier = .invalid

    /// An activity is worth starting once a transfer has lasted this long.
    private let startDelay: TimeInterval = 1.5
    /// ActivityKit coalesces anything faster.
    private let minPushInterval: TimeInterval = 1.0
    /// How long the finished/failed state stays on the Lock Screen.
    private let lingerAfterEnd: TimeInterval = 8

    private init() {
        // Activities left over from a previous run (the app was killed mid-
        // transfer) describe transfers that no longer exist.
        for a in Activity<TransferActivityAttributes>.activities {
            Task { await a.end(nil, dismissalPolicy: .immediate) }
        }
        // The last moment an activity may be started from this process is on
        // the way out of the foreground. If a short transfer hasn't reached
        // startDelay yet, start it now rather than leaving the rider with
        // nothing on the Lock Screen.
        NotificationCenter.default.addObserver(
            forName: UIApplication.willResignActiveNotification, object: nil, queue: .main
        ) { _ in
            MainActor.assumeIsolated { TransferCenter.shared.startActivityIfNeeded() }
        }
    }

    // MARK: - API for transfer code

    /// Starts (or restarts) a transfer. Calling it again for a running id just
    /// updates its description.
    func begin(_ id: String, kind: TransferKind, title: String, detail: String = "",
               total: Int64 = 0, unit: TransferUnit = .bytes, indeterminate: Bool = false) {
        if let i = active.firstIndex(where: { $0.id == id }) {
            active[i].kind = kind
            active[i].title = title
            active[i].detail = detail
            active[i].unit = unit
            active[i].total = total
            active[i].indeterminate = indeterminate
        } else {
            var t = Transfer(id: id, kind: kind, title: title, detail: detail,
                             startedAt: Date())
            t.total = total
            t.unit = unit
            t.indeterminate = indeterminate
            active.append(t)
        }
        settleTask?.cancel(); settleTask = nil
        holdBackgroundTime()
        scheduleStart()
        schedulePush(urgent: true)
    }

    /// Progress. Any argument left nil keeps its value. Unknown ids are ignored,
    /// so a late callback after finish() is harmless.
    func update(_ id: String, completed: Int64? = nil, total: Int64? = nil,
                detail: String? = nil, indeterminate: Bool? = nil, waiting: Int? = nil,
                title: String? = nil) {
        guard let i = active.firstIndex(where: { $0.id == id }) else { return }
        var t = active[i]
        let phaseChanged = (indeterminate != nil && indeterminate != t.indeterminate)
            || (title != nil && title != t.title)
        if let total, total != t.total {
            t.total = total
            t.lastSample = nil             // a new denominator, a new rate
        }
        if let completed {
            let now = Date()
            if let s = t.lastSample, completed > s.completed {
                let dt = now.timeIntervalSince(s.date)
                if dt >= 0.25 {
                    let r = Double(completed - s.completed) / dt
                    t.rate = t.rate == 0 ? r : t.rate * 0.7 + r * 0.3
                    t.lastSample = (now, completed)
                }
            } else if t.lastSample == nil || completed < (t.lastSample?.completed ?? 0) {
                t.lastSample = (now, completed)
            }
            t.completed = completed
        }
        if let detail { t.detail = detail }
        if let indeterminate {
            t.indeterminate = indeterminate
            if indeterminate { t.rate = 0; t.lastSample = nil }
        }
        if let waiting { t.waiting = waiting }
        if let title { t.title = title }
        guard t != active[i] else { return }
        active[i] = t
        schedulePush(urgent: phaseChanged)
    }

    /// Ends a transfer. A second finish (or one for an id never begun) is a
    /// no-op, so a generic failure path can call it after a specific success.
    func finish(_ id: String, success: Bool, message: String? = nil) {
        guard let i = active.firstIndex(where: { $0.id == id }) else { return }
        var t = active.remove(at: i)
        t.phase = success ? .finished : .failed
        if success { t.indeterminate = false; if t.total > 0 { t.completed = t.total } }
        if let message { t.detail = message }
        lastEnded = t
        if active.isEmpty {
            startTask?.cancel(); startTask = nil
            // Queues (rides, tiles) finish one item and begin the next in the
            // same breath. Ending the activity in between would lose it for
            // good when the app is in the background, where a new one cannot
            // be started — so only wind down once nothing has followed.
            settleTask?.cancel()
            settleTask = Task { @MainActor [weak self] in
                try? await Task.sleep(nanoseconds: 600_000_000)
                guard let self, !Task.isCancelled, self.active.isEmpty else { return }
                self.settleTask = nil
                self.endActivity()
                self.releaseBackgroundTime()
            }
        } else {
            schedulePush(urgent: true)
        }
    }

    func isActive(_ id: String) -> Bool { active.contains { $0.id == id } }

    /// Screenshot / simulator demo (`-demo-transfer`): a fake 4 MB ride
    /// download over ~40 s, so the Live Activity can be seen without a head
    /// unit. The simulator has no Bluetooth.
    func runDemoIfRequested() {
        guard ProcessInfo.processInfo.arguments.contains("-demo-transfer") else { return }
        let id = "demo", total: Int64 = 4_200_000
        begin(id, kind: .rideDownload, title: "Downloading ride",
              detail: "2026-10-03-0712.fit", total: total)
        update(id, waiting: 2)
        Task { @MainActor in
            var done: Int64 = 0
            while done < total {
                try? await Task.sleep(nanoseconds: 250_000_000)
                done = min(total, done + 26_000)
                update(id, completed: done)
            }
            finish(id, success: true, message: "2026-10-03-0712.fit downloaded")
        }
    }

    // MARK: - Live Activity

    /// The transfer the activity shows: the newest still running.
    private var headline: Transfer? { active.last }

    private func content(for t: Transfer) -> TransferActivityAttributes.ContentState {
        let others = active.filter { $0.id != t.id }.count + active.reduce(0) { $0 + $1.waiting }
        return .init(kind: t.kind, title: t.title, detail: t.detail, fraction: t.fraction,
                     completed: t.completed, total: t.total, unit: t.unit, eta: t.eta,
                     phase: t.phase, others: t.phase == .running ? others : 0)
    }

    private var canStartActivity: Bool {
        ActivityAuthorizationInfo().areActivitiesEnabled
            && UIApplication.shared.applicationState != .background
    }

    private func scheduleStart() {
        guard activity == nil, startTask == nil else { return }
        startTask = Task { @MainActor [weak self] in
            try? await Task.sleep(nanoseconds: UInt64((self?.startDelay ?? 1.5) * 1e9))
            guard let self, !Task.isCancelled else { return }
            self.startTask = nil
            self.startActivityIfNeeded()
        }
    }

    fileprivate func startActivityIfNeeded() {
        guard activity == nil, let t = headline, canStartActivity else { return }
        let state = content(for: t)
        do {
            activity = try Activity.request(
                attributes: TransferActivityAttributes(startedAt: active.first?.startedAt ?? Date()),
                content: .init(state: state, staleDate: nil),
                pushType: nil)
            lastPushed = state
            lastPush = Date()
        } catch {
            // Live Activities off for this app, or the system's limit reached.
            // The transfer itself is unaffected.
        }
    }

    private func schedulePush(urgent: Bool) {
        guard activity != nil else { return }
        let wait = max(0, minPushInterval - Date().timeIntervalSince(lastPush))
        if wait == 0 || (urgent && wait < 0.3) {
            pushTask?.cancel(); pushTask = nil
            push()
            return
        }
        guard pushTask == nil else { return }       // one pending push carries the latest
        pushTask = Task { @MainActor [weak self] in
            try? await Task.sleep(nanoseconds: UInt64(wait * 1e9))
            guard let self, !Task.isCancelled else { return }
            self.pushTask = nil
            self.push()
        }
    }

    private func push() {
        guard let activity, let t = headline else { return }
        var state = content(for: t)
        // Re-deriving the ETA every second jitters the countdown; keep the one
        // already on screen unless it has drifted by more than a few seconds.
        if let old = lastPushed?.eta, let new = state.eta, abs(old.timeIntervalSince(new)) < 5 {
            state.eta = old
        }
        guard state != lastPushed else { return }
        lastPushed = state
        lastPush = Date()
        Task { await activity.update(.init(state: state, staleDate: nil)) }
    }

    private func endActivity() {
        pushTask?.cancel(); pushTask = nil
        guard let activity else { return }
        self.activity = nil
        lastPushed = nil
        guard let t = lastEnded else {
            Task { await activity.end(nil, dismissalPolicy: .immediate) }
            return
        }
        let final = content(for: t)
        let policy: ActivityUIDismissalPolicy = .after(Date().addingTimeInterval(lingerAfterEnd))
        Task { await activity.end(.init(state: final, staleDate: nil), dismissalPolicy: policy) }
    }

    // MARK: - Background time

    private func holdBackgroundTime() {
        guard backgroundTask == .invalid else { return }
        backgroundTask = UIApplication.shared.beginBackgroundTask(withName: "Transfer") {
            // Out of time. BLE transfers carry on regardless (bluetooth-central
            // wakes the app for every packet); only the timers pause.
            MainActor.assumeIsolated { TransferCenter.shared.releaseBackgroundTime() }
        }
    }

    fileprivate func releaseBackgroundTime() {
        guard backgroundTask != .invalid else { return }
        UIApplication.shared.endBackgroundTask(backgroundTask)
        backgroundTask = .invalid
    }
}

// MARK: - HTTP progress

/// Feeds a URLSession task's byte counts into TransferCenter. Pass it as the
/// `delegate:` of the async URLSession calls; it watches the task's Progress.
final class TransferProgressDelegate: NSObject, URLSessionTaskDelegate, @unchecked Sendable {
    private let id: String
    private let upload: Bool
    private var observation: NSKeyValueObservation?

    init(id: String, upload: Bool) {
        self.id = id
        self.upload = upload
    }

    func urlSession(_ session: URLSession, didCreateTask task: URLSessionTask) {
        let id = self.id, upload = self.upload
        observation = task.progress.observe(\.fractionCompleted) { _, _ in
            let done = upload ? task.countOfBytesSent : task.countOfBytesReceived
            let total = upload ? task.countOfBytesExpectedToSend : task.countOfBytesExpectedToReceive
            Task { @MainActor in
                TransferCenter.shared.update(id, completed: done,
                                             total: total > 0 ? total : nil)
            }
        }
    }

    deinit { observation?.invalidate() }
}
