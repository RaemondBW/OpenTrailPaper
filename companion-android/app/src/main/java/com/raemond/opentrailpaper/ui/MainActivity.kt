package com.raemond.opentrailpaper.ui

import android.Manifest
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.provider.Settings
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.app.ActivityCompat
import com.raemond.opentrailpaper.OpenTrailPaperApp
import com.raemond.opentrailpaper.ble.BleManager
import com.raemond.opentrailpaper.data.Prefs

/**
 * The single activity.
 *
 * It owns the two things Compose cannot: the runtime-permission dance, and the
 * window flag that keeps the screen awake through a firmware or map transfer
 * (iOS's `isIdleTimerDisabled`). Everything else is Compose.
 */
class MainActivity : ComponentActivity() {

    private val ble: BleManager get() = (application as OpenTrailPaperApp).ble

    private var keepAwakeApplied = false

    private val requestPermissions =
        registerForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { result ->
            Prefs.noteAsked(result.keys)
            refreshPermissions()
            // A grant is the moment the radio can actually come up; without this
            // the rider would have to leave the screen and come back.
            ble.startCentral()
        }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        refreshPermissions()

        setContent {
            OpenTrailPaperTheme {
                RootScreen(
                    ble = ble,
                    requestLocation = { ask(locationPermissions) },
                    requestBluetooth = { ask(bluetoothPermissions) },
                    openAppSettings = ::openAppSettings,
                )
            }
        }
    }

    override fun onResume() {
        super.onResume()
        // Coming back from system Settings is the one way a permission changes
        // without anything telling us, and it's exactly the path our own "Open
        // Settings" buttons send people down.
        refreshPermissions()
        applyKeepAwake()
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) applyKeepAwake()
    }

    /**
     * Mirrors the manager's request onto the window. Polled at the two moments
     * the flag can matter rather than observed, because a transfer that starts
     * while the screen is on only needs the flag before the screen would dim.
     */
    private fun applyKeepAwake() {
        val wanted = ble.keepScreenOn
        if (wanted == keepAwakeApplied) return
        keepAwakeApplied = wanted
        if (wanted) {
            window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        } else {
            window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        }
    }

    private val locationPermissions = arrayOf(
        Manifest.permission.ACCESS_FINE_LOCATION,
        Manifest.permission.ACCESS_COARSE_LOCATION,
    )

    private val bluetoothPermissions: Array<String>
        get() = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            arrayOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)
        } else {
            // Pre-12 there is no Bluetooth runtime permission; a scan is refused
            // without location, so that is what the Bluetooth step must ask for.
            locationPermissions
        }

    private fun ask(permissions: Array<String>) {
        Prefs.noteAsked(permissions.toList())
        requestPermissions.launch(permissions)
    }

    /**
     * Recompute what the app is allowed to do.
     *
     * "Permanently denied" is inferred, not reported: a permission we have asked
     * for, do not hold, and may no longer show a rationale for is one only system
     * Settings can grant. Everything else is still promptable.
     */
    private fun refreshPermissions() {
        val asked = Prefs.askedPermissions
        val candidates = locationPermissions.toMutableSet()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            candidates += Manifest.permission.BLUETOOTH_SCAN
            candidates += Manifest.permission.BLUETOOTH_CONNECT
        }
        val permanentlyDenied = candidates.filter { permission ->
            permission in asked &&
                ActivityCompat.checkSelfPermission(this, permission) !=
                android.content.pm.PackageManager.PERMISSION_GRANTED &&
                !ActivityCompat.shouldShowRequestPermissionRationale(this, permission)
        }.toSet()
        ble.refreshPermissions(permanentlyDenied)
    }

    /** Opens this app's page in system Settings — the only place a refused
     *  permission can be granted again. */
    private fun openAppSettings() {
        startActivity(
            Intent(
                Settings.ACTION_APPLICATION_DETAILS_SETTINGS,
                Uri.fromParts("package", packageName, null),
            ).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK),
        )
    }
}

/** Actions a screen needs from the Activity, passed down rather than looked up. */
class HostActions(
    val requestLocation: () -> Unit,
    val requestBluetooth: () -> Unit,
    val openAppSettings: () -> Unit,
)
