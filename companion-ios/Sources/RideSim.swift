import Foundation

// A ride in progress, for the tutorial to show.
//
// The first screens of the app are a head unit that has to look switched ON —
// numbers settling, the clock running, the ride time counting. A frozen
// screenshot says "brochure"; a panel whose ride time ticks past while you read
// the paragraph says "this is a thing that runs".
//
// It is a pure function of elapsed time rather than a timer mutating state:
// every value is a closed-form expression of `t`, so there is nothing to start,
// stop, or leak when the tutorial's pages come and go, scrubbing works, and two
// devices on screen (the dashboard and the map) always agree because they are
// evaluating the same instant rather than two drifting counters.
//
// Values are metric here, as the device measures them; DeviceMock converts at
// display time the same way every other screen does.
struct RideSim {
    /// Seconds since the tutorial opened.
    let t: TimeInterval

    /// The ride is already well underway when you meet it — 1:47:12, 54.8 km,
    /// the same instant the product shots freeze on. The live panel is that
    /// photo carrying on rather than a second, emptier ride starting at zero.
    static let elapsedAtStart: TimeInterval = 1 * 3600 + 47 * 60 + 12
    static let distanceAtStart = 54.8       // km
    static let climbAtStart = 918.0         // m
    static let clockAtStart: TimeInterval = 14 * 3600 + 25 * 60

    // MARK: the ride

    /// km/h. Two out-of-phase sines: a long roll for the shape of the road and a
    /// short one for the pedal-by-pedal noise. Nothing lines up on a period you
    /// can spot, which is what keeps it from reading as a loop.
    var speed: Double { 30 + 6 * sin(t / 11) + 2 * sin(t / 3.3) }

    /// Watts, instant. Rides ahead of speed — on a climb the power goes up
    /// before the speed does, so the phase lead is what makes the pair look
    /// like one rider instead of two unrelated needles.
    var power: Double { 232 + 62 * sin(t / 9 + 0.7) + 26 * sin(t / 2.4) }

    /// The 3 s average the device actually displays: the same signal with the
    /// fast term damped, so it moves like the smoothed number it claims to be.
    var power3s: Double { 232 + 62 * sin(t / 9 + 0.7) + 8 * sin(t / 2.4) }

    /// Heart rate lags power by about half a minute, the way a real one does.
    var heartRate: Double { 152 + 9 * sin(t / 9 - 0.9) + 2 * sin(t / 23) }

    var cadence: Double { 87 + 5 * sin(t / 5.5) + 2 * sin(t / 1.9) }

    /// Distance is the integral of `speed`, in closed form — so it only ever
    /// goes up, and it goes up at exactly the speed shown next to it. Deriving
    /// it any other way lets the two disagree, which on a bike computer is the
    /// one error a rider is guaranteed to notice.
    var distance: Double {
        let integral = 30 * t - 66 * (cos(t / 11) - 1) - 6.6 * (cos(t / 3.3) - 1)
        return RideSim.distanceAtStart + integral / 3600
    }

    /// Percent. Follows the same long roll as speed, inverted — you slow down
    /// where it ramps up.
    var grade: Double { 3.4 - 3.6 * sin(t / 11 + 1.2) }

    var altitude: Double { 112 + 46 * sin(t / 37) }

    /// Metres climbed: only the uphill counts, so it ratchets. Sampled per
    /// second rather than integrated — the closed form of max(0, sine) is not
    /// worth it for a number that moves this slowly.
    var climb: Double {
        RideSim.climbAtStart + max(0, t) * 0.31
    }

    /// Stopped time accrues while the tutorial runs but far slower than the
    /// clock, so moving time trails ride time by a widening minute or two.
    var movingTime: TimeInterval { elapsed - 221 - t * 0.02 }

    var elapsed: TimeInterval { RideSim.elapsedAtStart + t }

    var clock: TimeInterval { RideSim.clockAtStart + t }

    /// The route's remaining distance falls as the ride distance rises.
    var routeLeft: Double { max(0, 12.4 - (distance - RideSim.distanceAtStart)) }

    /// A fuel gauge that visibly moves would be a lie — this one is ten hours of
    /// runtime, so it drops a percent every few minutes and no faster.
    var battery: Int { max(0, 76 - Int(t / 420)) }

    /// Satellites wander between 10 and 12 with a good sky.
    var satellites: Int { 11 + Int(round(sin(t / 13))) }

    // MARK: what the panel prints

    /// The string the device would draw for a dashboard field id.
    ///
    /// Kept to the digit counts of DashPreview's samples: the panel picks one
    /// type size for the widest value a field can ever hold and then never
    /// resizes, so a live value that outgrew its sample would be the one thing
    /// on screen that doesn't behave like the real panel.
    func text(for field: String, miles: Bool = false) -> String {
        switch field {
        case "speed":      return String(format: "%.1f", Units.speed(speed, miles: miles))
        case "power":      return String(Int(power.rounded()))
        case "power3s":    return String(Int(power3s.rounded()))
        case "hr":         return String(Int(heartRate.rounded()))
        case "cadence":    return String(Int(cadence.rounded()))
        case "distance":   return String(format: "%.1f", Units.distance(distance, miles: miles))
        case "routeleft":  return String(format: "%.1f", Units.distance(routeLeft, miles: miles))
        case "ridetime":   return RideSim.hms(elapsed)
        case "movingtime": return RideSim.hms(movingTime)
        case "climb":      return String(Int(Units.elevation(climb, miles: miles).rounded()))
        case "altitude":   return String(Int(Units.elevation(altitude, miles: miles).rounded()))
        case "grade":      return String(format: "%.1f", grade)
        case "battery":    return String(battery)
        case "sats":       return String(satellites)
        case "clock":      return RideSim.hm(clock)
        default:           return "--"
        }
    }

    /// H:MM:SS, the device's ride-time format.
    static func hms(_ seconds: TimeInterval) -> String {
        let s = Int(max(0, seconds))
        return String(format: "%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60)
    }

    /// HH:MM on a 24-hour clock, wrapping at midnight.
    static func hm(_ seconds: TimeInterval) -> String {
        let s = Int(max(0, seconds)) % 86_400
        return String(format: "%d:%02d", s / 3600, (s / 60) % 60)
    }
}
