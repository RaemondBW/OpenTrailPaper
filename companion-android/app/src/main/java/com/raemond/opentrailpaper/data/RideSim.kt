package com.raemond.opentrailpaper.data

import java.util.Locale
import kotlin.math.cos
import kotlin.math.max
import kotlin.math.roundToInt
import kotlin.math.sin

/**
 * A ride in progress, for the tutorial to show.
 *
 * The first screens of the app are a head unit that has to look switched ON —
 * numbers settling, the clock running, the ride time counting. A frozen
 * screenshot says "brochure"; a panel whose ride time ticks past while you read
 * the paragraph says "this is a thing that runs".
 *
 * It is a pure function of elapsed time rather than a timer mutating state:
 * every value is a closed-form expression of [t], so there is nothing to start,
 * stop, or leak when the tutorial's pages come and go, scrubbing works, and two
 * devices on screen (the dashboard and the map) always agree because they are
 * evaluating the same instant rather than two drifting counters.
 *
 * Values are metric here, as the device measures them; the panel converts at
 * display time the same way every other screen does.
 */
data class RideSim(
    /** Seconds since the tutorial opened. */
    val t: Double,
) {
    /** km/h. Two out-of-phase sines: a long roll for the shape of the road and a
     *  short one for the pedal-by-pedal noise. Nothing lines up on a period you
     *  can spot, which is what keeps it from reading as a loop. */
    val speed: Double get() = 30 + 6 * sin(t / 11) + 2 * sin(t / 3.3)

    /** Watts, instant. Rides ahead of speed — on a climb the power goes up
     *  before the speed does, so the phase lead is what makes the pair look like
     *  one rider instead of two unrelated needles. */
    val power: Double get() = 232 + 62 * sin(t / 9 + 0.7) + 26 * sin(t / 2.4)

    /** The 3 s average the device actually displays: the same signal with the
     *  fast term damped, so it moves like the smoothed number it claims to be. */
    val power3s: Double get() = 232 + 62 * sin(t / 9 + 0.7) + 8 * sin(t / 2.4)

    /** Heart rate lags power by about half a minute, the way a real one does. */
    val heartRate: Double get() = 152 + 9 * sin(t / 9 - 0.9) + 2 * sin(t / 23)

    val cadence: Double get() = 87 + 5 * sin(t / 5.5) + 2 * sin(t / 1.9)

    /** Distance is the integral of [speed], in closed form — so it only ever
     *  goes up, and it goes up at exactly the speed shown next to it. Deriving
     *  it any other way lets the two disagree, which on a bike computer is the
     *  one error a rider is guaranteed to notice. */
    val distance: Double
        get() {
            val integral = 30 * t - 66 * (cos(t / 11) - 1) - 6.6 * (cos(t / 3.3) - 1)
            return DISTANCE_AT_START + integral / 3600
        }

    /** Percent. Follows the same long roll as speed, inverted — you slow down
     *  where it ramps up. */
    val grade: Double get() = 3.4 - 3.6 * sin(t / 11 + 1.2)

    val altitude: Double get() = 112 + 46 * sin(t / 37)

    /** Metres climbed: only the uphill counts, so it ratchets. */
    val climb: Double get() = CLIMB_AT_START + max(0.0, t) * 0.31

    /** Stopped time accrues while the tutorial runs but far slower than the
     *  clock, so moving time trails ride time by a widening minute or two. */
    val movingTime: Double get() = elapsed - 221 - t * 0.02

    val elapsed: Double get() = ELAPSED_AT_START + t

    val clock: Double get() = CLOCK_AT_START + t

    /** The route's remaining distance falls as the ride distance rises. */
    val routeLeft: Double get() = max(0.0, 12.4 - (distance - DISTANCE_AT_START))

    /** A fuel gauge that visibly moves would be a lie — this one is ten hours of
     *  runtime, so it drops a percent every few minutes and no faster. */
    val battery: Int get() = max(0, 76 - (t / 420).toInt())

    /** Satellites wander between 10 and 12 with a good sky. */
    val satellites: Int get() = 11 + Math.round(sin(t / 13)).toInt()

    /**
     * The string the device would draw for a dashboard field id.
     *
     * Kept to the digit counts of the panel preview's samples: the panel picks
     * one type size for the widest value a field can ever hold and then never
     * resizes, so a live value that outgrew its sample would be the one thing on
     * screen that doesn't behave like the real panel.
     */
    fun text(field: String, miles: Boolean = false): String = when (field) {
        "speed" -> fmt1(Units.speed(speed, miles))
        "power" -> power.roundToInt().toString()
        "power3s" -> power3s.roundToInt().toString()
        "hr" -> heartRate.roundToInt().toString()
        "cadence" -> cadence.roundToInt().toString()
        "distance" -> fmt1(Units.distance(distance, miles))
        "routeleft" -> fmt1(Units.distance(routeLeft, miles))
        "ridetime" -> hms(elapsed)
        "movingtime" -> hms(movingTime)
        "climb" -> Units.elevation(climb, miles).roundToInt().toString()
        "altitude" -> Units.elevation(altitude, miles).roundToInt().toString()
        "grade" -> fmt1(grade)
        "battery" -> battery.toString()
        "sats" -> satellites.toString()
        "clock" -> hm(clock)
        else -> "--"
    }

    companion object {
        /** The ride is already well underway when you meet it — 1:47:12, 54.8 km,
         *  the same instant the product shots freeze on. The live panel is that
         *  photo carrying on rather than a second, emptier ride starting at zero. */
        const val ELAPSED_AT_START = 1 * 3600.0 + 47 * 60 + 12
        const val DISTANCE_AT_START = 54.8      // km
        const val CLIMB_AT_START = 918.0        // m
        const val CLOCK_AT_START = 14 * 3600.0 + 25 * 60

        private fun fmt1(v: Double) = String.format(Locale.US, "%.1f", v)

        /** H:MM:SS, the device's ride-time format. */
        fun hms(seconds: Double): String {
            val s = max(0.0, seconds).toInt()
            return String.format(Locale.US, "%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60)
        }

        /** HH:MM on a 24-hour clock, wrapping at midnight. */
        fun hm(seconds: Double): String {
            val s = max(0.0, seconds).toInt() % 86_400
            return String.format(Locale.US, "%d:%02d", s / 3600, (s / 60) % 60)
        }
    }
}
