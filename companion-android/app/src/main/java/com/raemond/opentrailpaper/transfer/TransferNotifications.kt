package com.raemond.opentrailpaper.transfer

import android.Manifest
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import android.text.format.Formatter
import androidx.core.content.ContextCompat
import com.raemond.opentrailpaper.R
import com.raemond.opentrailpaper.ui.MainActivity
import kotlin.math.roundToInt

/**
 * Builds the transfer notifications: the ongoing one the foreground service
 * wears, and the short-lived outcome left behind when it ends.
 *
 * On Android 16 (API 36) the ongoing one is a [Notification.ProgressStyle] that
 * requests promotion to a Live Update — the status-bar chip and the pinned spot
 * at the top of the shade. Older releases get the classic progress bar, which
 * is the same information in the same place minus the chip.
 */
internal object TransferNotifications {

    const val CHANNEL_ID = "transfers"
    const val PROGRESS_ID = 43           // RideLocationService uses 42
    private const val OUTCOME_ID = 44
    private const val SCALE = 1000

    /** Notification.EXTRA_REQUEST_PROMOTED_ONGOING, not in the API 36 stubs. */
    private const val EXTRA_REQUEST_PROMOTED_ONGOING = "android.requestPromotedOngoing"

    fun createChannel(context: Context) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return
        val nm = context.getSystemService(NotificationManager::class.java)
        if (nm.getNotificationChannel(CHANNEL_ID) != null) return
        nm.createNotificationChannel(
            NotificationChannel(
                CHANNEL_ID,
                context.getString(R.string.transfer_channel),
                // LOW: no sound or peek on every update, but still eligible for
                // promotion (MIN is not).
                NotificationManager.IMPORTANCE_LOW,
            ).apply { description = context.getString(R.string.transfer_channel_description) },
        )
    }

    private fun openApp(context: Context): PendingIntent = PendingIntent.getActivity(
        context,
        1,
        Intent(context, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP),
        PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
    )

    private fun canPost(context: Context) =
        Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU ||
            ContextCompat.checkSelfPermission(context, Manifest.permission.POST_NOTIFICATIONS) ==
            PackageManager.PERMISSION_GRANTED

    /** "1.2 MB of 3.4 MB", "12 of 40 tiles", or null. */
    fun amountText(context: Context, t: TransferCenter.Transfer): String? {
        if (t.total <= 0 || t.indeterminate) return null
        return when (t.unit) {
            TransferCenter.Unit.ITEMS -> "${t.completed} of ${t.total} tiles"
            TransferCenter.Unit.BYTES ->
                "${Formatter.formatShortFileSize(context, t.completed)} of " +
                    Formatter.formatShortFileSize(context, t.total)
        }
    }

    private fun etaText(seconds: Long): String = when {
        seconds < 60 -> "${seconds}s left"
        seconds < 3600 -> "${seconds / 60} min left"
        else -> "${seconds / 3600} h ${(seconds % 3600) / 60} min left"
    }

    fun buildProgress(context: Context, t: TransferCenter.Transfer): Notification {
        val others = TransferCenter.othersCount(t)
        val fraction = t.fraction
        val percent = fraction?.let { (it * 100).toInt() }
        val line = listOfNotNull(
            t.detail.takeIf { it.isNotEmpty() },
            amountText(context, t),
            t.etaSeconds?.let(::etaText),
        ).joinToString(" · ")

        val b = Notification.Builder(context, CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_launcher_monochrome)
            .setContentTitle(t.title)
            .setContentText(line)
            .setContentIntent(openApp(context))
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .setShowWhen(false)
            .setCategory(Notification.CATEGORY_PROGRESS)
        if (others > 0) b.setSubText("+$others more")
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            // Show at once instead of after Android 12's ten-second grace for
            // short foreground services: a transfer is worth seeing.
            b.setForegroundServiceBehavior(Notification.FOREGROUND_SERVICE_IMMEDIATE)
        }

        if (Build.VERSION.SDK_INT >= 36) {
            // ProgressStyle's scale is the sum of its segments (100 with none),
            // so one segment of SCALE makes setProgress() per-mille like below.
            val style = Notification.ProgressStyle()
                .addProgressSegment(Notification.ProgressStyle.Segment(SCALE))
                .setStyledByProgress(true)
                .setProgressIndeterminate(fraction == null)
            if (fraction != null) style.setProgress((fraction * SCALE).roundToInt())
            b.setStyle(style)
            // The status-bar chip: as short as it gets.
            percent?.let { b.setShortCriticalText("$it%") }
            b.extras.putBoolean(EXTRA_REQUEST_PROMOTED_ONGOING, true)
        } else {
            if (fraction != null) b.setProgress(SCALE, (fraction * SCALE).roundToInt(), false)
            else b.setProgress(0, 0, true)
        }
        return b.build()
    }

    fun postProgress(context: Context, t: TransferCenter.Transfer) {
        if (!canPost(context)) return
        context.getSystemService(NotificationManager::class.java)
            .notify(PROGRESS_ID, buildProgress(context, t))
    }

    /** The "finished" / "failed" line left after the service is gone. */
    fun postOutcome(context: Context, t: TransferCenter.Transfer) {
        if (!canPost(context)) return
        val ok = t.phase == TransferCenter.Phase.FINISHED
        val b = Notification.Builder(context, CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_launcher_monochrome)
            .setContentTitle(if (ok) "${t.kind.label} finished" else "${t.kind.label} failed")
            .setContentText(t.detail)
            .setContentIntent(openApp(context))
            .setAutoCancel(true)
            .setOnlyAlertOnce(true)
            .setCategory(if (ok) Notification.CATEGORY_STATUS else Notification.CATEGORY_ERROR)
        // A success is only news for a moment; a failure waits to be read.
        if (ok && Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) b.setTimeoutAfter(15_000)
        context.getSystemService(NotificationManager::class.java).notify(OUTCOME_ID, b.build())
    }
}
