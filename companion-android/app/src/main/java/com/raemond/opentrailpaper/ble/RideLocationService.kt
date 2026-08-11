package com.raemond.opentrailpaper.ble

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import com.raemond.opentrailpaper.R
import com.raemond.opentrailpaper.ui.MainActivity

/**
 * Keeps the process alive, and location flowing, while the head unit is
 * recording a ride.
 *
 * This is the Android half of what iOS gets from
 * `allowsBackgroundLocationUpdates`: with the screen off Android will only keep
 * delivering fixes to a location-typed foreground service, and the phone's fixes
 * are the head unit's fallback track whenever its own receiver has nothing. It
 * runs ONLY while a ride is recording — the same gate iOS uses, and for the same
 * reason: a phone tracking in the background while nothing is happening is pure
 * battery cost.
 *
 * The service holds no state of its own; [BleManager] owns the location listener
 * and starts and stops this as the device's recording flag changes.
 */
class RideLocationService : Service() {

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val channelId = "ride-recording"
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val manager = getSystemService(NotificationManager::class.java)
            if (manager.getNotificationChannel(channelId) == null) {
                manager.createNotificationChannel(
                    NotificationChannel(
                        channelId,
                        getString(R.string.ride_service_channel),
                        NotificationManager.IMPORTANCE_LOW,
                    ),
                )
            }
        }

        val open = PendingIntent.getActivity(
            this,
            0,
            Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )

        val notification: Notification =
            Notification.Builder(this, channelId)
                .setContentTitle(getString(R.string.ride_service_title))
                .setContentText(getString(R.string.ride_service_text))
                .setSmallIcon(R.drawable.ic_launcher_monochrome)
                .setContentIntent(open)
                .setOngoing(true)
                .build()

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            startForeground(
                NOTIFICATION_ID,
                notification,
                ServiceInfo.FOREGROUND_SERVICE_TYPE_LOCATION,
            )
        } else {
            startForeground(NOTIFICATION_ID, notification)
        }
        // The ride ends when the device says it ends, not when the system feels
        // like restarting us — a redelivered start with no ride running would
        // put a "recording" notification over nothing.
        return START_NOT_STICKY
    }

    private companion object {
        const val NOTIFICATION_ID = 42
    }
}
