package com.raemond.opentrailpaper.transfer

import android.Manifest
import android.app.Service
import android.content.Intent
import android.content.pm.PackageManager
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import android.util.Log
import androidx.core.content.ContextCompat

/**
 * Keeps the process alive while a long transfer runs, so it survives the screen
 * turning off and the app going to the background.
 *
 * Holds no state: [TransferCenter] owns the transfers, starts this when the
 * first begins and stops it when the last ends. The foreground type follows
 * what is running — `connectedDevice` for anything over Bluetooth to the head
 * unit, `dataSync` for HTTP (uploads to Strava and friends, the firmware
 * download) — and is widened in place if a new kind of transfer joins.
 */
class TransferService : Service() {

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        instance = this
    }

    override fun onDestroy() {
        if (instance === this) instance = null
        super.onDestroy()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        // startForegroundService() obliges a startForeground() within seconds,
        // even if everything finished in the meantime.
        refreshForeground()
        if (TransferCenter.active.isEmpty()) stop()
        // A transfer cannot be resumed by a restarted service with no state.
        return START_NOT_STICKY
    }

    /** (Re)enter the foreground with the types the running transfers need. */
    internal fun refreshForeground() {
        val t = TransferCenter.headline ?: TransferCenter.lastEnded ?: return
        val notification = TransferNotifications.buildProgress(this, t)
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) {
            startForeground(TransferNotifications.PROGRESS_ID, notification)
            return
        }
        var types = 0
        // connectedDevice requires a Bluetooth runtime grant on Android 14+;
        // without one there is no BLE transfer to protect anyway.
        if (TransferCenter.needsBle && hasBluetoothConnect()) {
            types = types or ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE
        }
        if (TransferCenter.needsNetwork || types == 0) {
            types = types or ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC
        }
        try {
            startForeground(TransferNotifications.PROGRESS_ID, notification, types)
        } catch (e: Exception) {
            // A type the system will not grant right now: carry on as dataSync
            // alone rather than crash the transfer.
            Log.w(TAG, "startForeground($types) refused", e)
            runCatching {
                startForeground(
                    TransferNotifications.PROGRESS_ID, notification,
                    ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC,
                )
            }
        }
    }

    private fun hasBluetoothConnect(): Boolean =
        Build.VERSION.SDK_INT < Build.VERSION_CODES.S ||
            ContextCompat.checkSelfPermission(this, Manifest.permission.BLUETOOTH_CONNECT) ==
            PackageManager.PERMISSION_GRANTED

    internal fun stop() {
        // Forget this instance now, not in onDestroy: a transfer beginning in
        // the gap must start a fresh service rather than talk to a dying one.
        if (instance === this) instance = null
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
            stopForeground(STOP_FOREGROUND_REMOVE)
        } else {
            @Suppress("DEPRECATION")
            stopForeground(true)
        }
        stopSelf()
    }

    /**
     * Android 15 caps dataSync at six hours a day. No real transfer comes close;
     * if one somehow does, step out of the foreground as the system demands.
     */
    override fun onTimeout(startId: Int, fgsType: Int) {
        Log.w(TAG, "foreground time limit reached for type $fgsType")
        stop()
    }

    internal companion object {
        private const val TAG = "TransferService"

        /** The running service, if any. Main-thread only. */
        @Volatile var instance: TransferService? = null
            private set
    }
}
