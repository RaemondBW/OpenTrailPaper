import os
import QuartzCore
import UIKit

/// Debug-only frame-time watch for chasing scroll jitter: with `-perf-log`
/// it counts display frames that ran long (missed their deadline) and logs a
/// summary every few seconds to the "perf" category, plus how often the
/// screens being checked re-evaluated their bodies. Read with
/// `log stream --predicate 'category == "perf"'`.
@MainActor
final class PerfMonitor: NSObject {
    static let shared = PerfMonitor()
    static let enabled = ProcessInfo.processInfo.arguments.contains("-perf-log")
    private let log = Logger(subsystem: "com.raemond.opentrailpaper", category: "perf")
    private var link: CADisplayLink?
    private var last: CFTimeInterval = 0
    private var frames = 0, hitches = 0, worst: CFTimeInterval = 0
    private var bodies: [String: Int] = [:]
    private var windowStart: CFTimeInterval = 0

    func start() {
        guard Self.enabled, link == nil else { return }
        let l = CADisplayLink(target: self, selector: #selector(tick(_:)))
        l.add(to: .main, forMode: .common)
        link = l
    }

    static func note(_ s: String) {
        shared.log.info("\(s, privacy: .public)")
    }

    /// Call from a view's body to count its evaluations.
    nonisolated static func body(_ name: String) {
        guard enabled else { return }
        MainActor.assumeIsolated { shared.bodies[name, default: 0] += 1 }
    }

    @objc private func tick(_ l: CADisplayLink) {
        let now = l.timestamp
        if last > 0 {
            let dt = now - last
            let expected = l.targetTimestamp - l.timestamp
            frames += 1
            if dt > max(expected, 1.0 / 120) * 1.5 { hitches += 1; worst = max(worst, dt) }
        }
        last = now
        if windowStart == 0 { windowStart = now }
        if now - windowStart >= 5 {
            let b = bodies.map { "\($0.key)=\($0.value)" }.sorted().joined(separator: " ")
            log.info("frames=\(self.frames) hitches=\(self.hitches) worst=\(Int(self.worst * 1000))ms bodies: \(b, privacy: .public)")
            frames = 0; hitches = 0; worst = 0; bodies = [:]; windowStart = now
        }
    }
}
