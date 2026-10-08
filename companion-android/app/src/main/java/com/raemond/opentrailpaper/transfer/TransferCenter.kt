package com.raemond.opentrailpaper.transfer

import android.content.Context
import android.content.Intent
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.util.Log
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.core.content.ContextCompat
import kotlin.math.max
import kotlin.math.min

/**
 * Every long transfer in the app reports here — ride and log downloads, the
 * firmware update, map tiles, route sends, uploads to Strava and friends — and
 * this is the only place that knows about the foreground service and its
 * notification. The Android twin of iOS's `TransferCenter.swift`.
 *
 * Call sites say three things: [begin], [update] as often as they like, and
 * [finish]. From any thread: everything is marshalled onto the main thread.
 *
 *  - The first transfer starts [TransferService], a foreground service typed
 *    `connectedDevice` (BLE) and/or `dataSync` (HTTP), so Android keeps the
 *    process — and the transfer — alive with the screen off or the app in the
 *    background. It has to start while the app is still in front (Android 12+
 *    refuses background starts), which is exactly when a rider taps Send.
 *  - Its ongoing notification shows the newest transfer with a progress bar,
 *    bytes and ETA. Redraws are coalesced to one a second; phase changes go out
 *    promptly. On Android 16 it is a ProgressStyle notification that asks to be
 *    promoted to a Live Update (status-bar chip, top of the shade).
 *  - When the last transfer ends the service stops. If the app is not on
 *    screen (or it failed) a short-lived "finished" / "failed" notification is
 *    left behind, so the rider learns how it went.
 */
object TransferCenter {

    enum class Kind(val label: String) {
        RIDE_DOWNLOAD("Ride download"),
        LOG_DOWNLOAD("Log download"),
        FIRMWARE("Firmware update"),
        MAP_TILES("Map tiles"),
        MAP_UPLOAD("Map upload"),
        ROUTE("Route"),
        CLOUD_UPLOAD("Upload"),
    }

    enum class Unit { BYTES, ITEMS }
    enum class Phase { RUNNING, FINISHED, FAILED }

    data class Transfer(
        val id: String,
        val kind: Kind,
        val title: String,
        val detail: String,
        val completed: Long = 0,
        val total: Long = 0,
        val unit: Unit = Unit.BYTES,
        /** No measurable size right now (rebooting, processing, building). */
        val indeterminate: Boolean = false,
        /** More of the same queued behind this one (e.g. rides). */
        val waiting: Int = 0,
        /** Uses Bluetooth to the head unit (foreground type connectedDevice). */
        val ble: Boolean = true,
        /** Uses the network (foreground type dataSync). */
        val network: Boolean = false,
        val startedAt: Long = SystemClock.elapsedRealtime(),
        val phase: Phase = Phase.RUNNING,
        /** Smoothed throughput in units per second, for the ETA. */
        val rate: Double = 0.0,
        internal val sampleAt: Long = 0,
        internal val sampleCompleted: Long = -1,
    ) {
        /** 0..1, or null while nothing is measurable. */
        val fraction: Double?
            get() = if (indeterminate || total <= 0) null
                    else min(1.0, max(0.0, completed.toDouble() / total))

        /** Seconds left, once there is enough history to guess. */
        val etaSeconds: Long?
            get() {
                val f = fraction ?: return null
                if (phase != Phase.RUNNING || f >= 1.0 || rate <= 0) return null
                if (SystemClock.elapsedRealtime() - startedAt < 2_000) return null
                return ((total - completed) / rate).toLong()
            }
    }

    private const val TAG = "TransferCenter"

    private lateinit var appContext: Context
    private val main = Handler(Looper.getMainLooper())

    /** Running transfers, oldest first. Compose-observable. */
    var active by mutableStateOf<List<Transfer>>(emptyList()); private set

    /** The last transfer to end, for the closing notification. */
    internal var lastEnded: Transfer? = null; private set

    /** Set by the visible activity: whether the UI itself is showing progress. */
    @Volatile var appVisible = false

    /**
     * Set by the visible activity: asks for POST_NOTIFICATIONS. Without it the
     * transfer still runs, but its notification is invisible.
     */
    var requestNotificationPermission: (() -> kotlin.Unit)? = null

    private var lastPush = 0L
    private var pushPending = false
    private var settlePending: Runnable? = null
    private var serviceRunning = false

    fun init(context: Context) {
        appContext = context.applicationContext
        TransferNotifications.createChannel(appContext)
    }

    // MARK: - API for transfer code

    /** Starts (or re-describes) a transfer. */
    fun begin(
        id: String,
        kind: Kind,
        title: String,
        detail: String = "",
        total: Long = 0,
        unit: Unit = Unit.BYTES,
        indeterminate: Boolean = false,
        ble: Boolean = true,
        network: Boolean = false,
    ) = onMain {
        val existing = active.firstOrNull { it.id == id }
        val t = existing?.copy(
            kind = kind, title = title, detail = detail, total = total, unit = unit,
            indeterminate = indeterminate, ble = ble, network = network,
            sampleCompleted = -1, rate = 0.0,
        ) ?: Transfer(
            id = id, kind = kind, title = title, detail = detail, total = total, unit = unit,
            indeterminate = indeterminate, ble = ble, network = network,
        )
        active = if (existing != null) active.map { if (it.id == id) t else it } else active + t
        settlePending?.let { main.removeCallbacks(it) }
        settlePending = null
        if (existing == null) requestNotificationPermission?.invoke()
        ensureService()
        schedulePush(urgent = true)
    }

    /** Progress. A null argument keeps its value; an unknown id is ignored. */
    fun update(
        id: String,
        completed: Long? = null,
        total: Long? = null,
        detail: String? = null,
        indeterminate: Boolean? = null,
        waiting: Int? = null,
        title: String? = null,
    ) = onMain {
        val i = active.indexOfFirst { it.id == id }
        if (i < 0) return@onMain
        val old = active[i]
        var t = old
        if (total != null && total != t.total) t = t.copy(total = total, sampleCompleted = -1)
        if (completed != null) {
            val now = SystemClock.elapsedRealtime()
            t = when {
                t.sampleCompleted < 0 || completed < t.sampleCompleted ->
                    t.copy(sampleAt = now, sampleCompleted = completed)
                completed > t.sampleCompleted && now - t.sampleAt >= 250 -> {
                    val r = (completed - t.sampleCompleted) * 1000.0 / (now - t.sampleAt)
                    t.copy(
                        rate = if (t.rate == 0.0) r else t.rate * 0.7 + r * 0.3,
                        sampleAt = now, sampleCompleted = completed,
                    )
                }
                else -> t
            }.copy(completed = completed)
        }
        if (detail != null) t = t.copy(detail = detail)
        if (indeterminate != null) {
            t = t.copy(indeterminate = indeterminate)
            if (indeterminate) t = t.copy(rate = 0.0, sampleCompleted = -1)
        }
        if (waiting != null) t = t.copy(waiting = waiting)
        if (title != null) t = t.copy(title = title)
        if (t == old) return@onMain
        val urgent = t.indeterminate != old.indeterminate || t.title != old.title
        active = active.toMutableList().also { it[i] = t }
        schedulePush(urgent)
    }

    /** Ends a transfer. A repeat (or an id never begun) is a no-op. */
    fun finish(id: String, success: Boolean, message: String? = null) = onMain {
        val t = active.firstOrNull { it.id == id } ?: return@onMain
        active = active.filter { it.id != id }
        lastEnded = t.copy(
            phase = if (success) Phase.FINISHED else Phase.FAILED,
            detail = message ?: t.detail,
            indeterminate = if (success) false else t.indeterminate,
            completed = if (success && t.total > 0) t.total else t.completed,
        )
        if (active.isEmpty()) {
            // Queues (rides, tiles) finish one item and begin the next in the
            // same breath. Stopping the service in between would lose it: a
            // foreground service cannot be restarted from the background.
            val r = Runnable {
                settlePending = null
                if (active.isEmpty()) stopService()
            }
            settlePending = r
            main.postDelayed(r, 600)
        } else {
            schedulePush(urgent = true)
        }
    }

    fun isActive(id: String) = active.any { it.id == id }

    /**
     * Emulator demo (iOS: -demo-transfer): a fake 4 MB ride download over
     * ~40 s, so the notification can be seen without a head unit.
     *   adb shell am start ... --ez demo-transfer true
     */
    fun runDemo() {
        val id = "demo"
        val total = 4_200_000L
        begin(id, Kind.RIDE_DOWNLOAD, "Downloading ride", detail = "2026-10-03-0712.fit", total = total)
        update(id, waiting = 2)
        var done = 0L
        val tick = object : Runnable {
            override fun run() {
                done = min(total, done + 26_000)
                update(id, completed = done)
                if (done < total) main.postDelayed(this, 250)
                else finish(id, success = true, message = "2026-10-03-0712.fit downloaded")
            }
        }
        main.postDelayed(tick, 250)
    }

    // MARK: - service + notification

    /** The transfer the notification shows: the newest still running. */
    internal val headline: Transfer? get() = active.lastOrNull()

    /** Others running, plus anything queued behind them. */
    internal fun othersCount(t: Transfer): Int =
        active.count { it.id != t.id } + active.sumOf { it.waiting }

    internal val needsBle: Boolean get() = active.any { it.ble }
    internal val needsNetwork: Boolean get() = active.any { it.network }

    private fun ensureService() {
        val running = TransferService.instance
        if (running != null) {
            running.refreshForeground()       // the foreground types may have grown
            return
        }
        if (serviceRunning) return
        serviceRunning = true
        try {
            ContextCompat.startForegroundService(
                appContext, Intent(appContext, TransferService::class.java),
            )
        } catch (e: Exception) {
            // ForegroundServiceStartNotAllowedException: the app is already in
            // the background. The transfer still runs for as long as the
            // process does; it just has no notification or protection.
            serviceRunning = false
            Log.w(TAG, "foreground service refused", e)
        }
    }

    private fun stopService() {
        serviceRunning = false
        val ended = lastEnded
        // Never stopService() from outside: a service started with
        // startForegroundService() that is stopped before it reaches
        // startForeground() crashes the app. Not yet up? It stops itself in
        // onStartCommand when it finds nothing running.
        TransferService.instance?.stop()
        if (ended != null && (!appVisible || ended.phase == Phase.FAILED)) {
            TransferNotifications.postOutcome(appContext, ended)
        }
    }

    private fun schedulePush(urgent: Boolean) {
        val since = SystemClock.elapsedRealtime() - lastPush
        val wait = 1_000 - since
        if (wait <= 0 || (urgent && wait < 300)) {
            push()
            return
        }
        if (pushPending) return
        pushPending = true
        main.postDelayed({ pushPending = false; push() }, wait)
    }

    private fun push() {
        val t = headline ?: return
        lastPush = SystemClock.elapsedRealtime()
        TransferService.instance?.let { TransferNotifications.postProgress(appContext, t) }
    }

    private inline fun onMain(crossinline block: () -> kotlin.Unit) {
        if (Looper.myLooper() == Looper.getMainLooper()) block() else main.post { block() }
    }
}
