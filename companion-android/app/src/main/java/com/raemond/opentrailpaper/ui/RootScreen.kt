package com.raemond.opentrailpaper.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.consumeWindowInsets
import androidx.compose.foundation.layout.statusBars
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.padding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Map
import androidx.compose.material.icons.filled.Speed
import androidx.compose.material.icons.filled.Tune
import androidx.compose.material.icons.filled.WifiTethering
import androidx.compose.material.icons.automirrored.filled.ViewList
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import com.raemond.opentrailpaper.ble.BleManager
import com.raemond.opentrailpaper.data.Prefs
import com.raemond.opentrailpaper.data.RouteImport

private class Tab(val label: String, val icon: ImageVector)

private val TABS = listOf(
    Tab("Ride", Icons.Filled.Speed),
    Tab("Route", Icons.Filled.Map),
    Tab("Rides", Icons.AutoMirrored.Filled.ViewList),
    Tab("Mesh", Icons.Filled.WifiTethering),
    Tab("Settings", Icons.Filled.Tune),
)

/**
 * The five tabs, and the full-screen tutorial that covers them on first run.
 *
 * The tutorial is a cover rather than a destination so that both first launch and
 * the Settings "Show tutorial" button drive exactly the same thing — the state
 * that decides it lives here, above the tabs.
 */
@Composable
fun RootScreen(
    ble: BleManager,
    requestLocation: () -> Unit,
    requestBluetooth: () -> Unit,
    openAppSettings: () -> Unit,
) {
    var tab by rememberSaveable { mutableIntStateOf(0) }
    var showTutorial by rememberSaveable { mutableStateOf(!Prefs.onboarded) }
    val host = remember {
        HostActions(requestLocation, requestBluetooth, openAppSettings)
    }

    val overlay = rememberOverlayHostState()
    // An import can arrive from another app while any tab is up; the route it
    // produced is only meaningful on the one that draws routes.
    //
    // Keyed on `fresh` rather than on the route itself, because the route now
    // outlives the screen that draws it: keying on `pending` would re-claim the
    // tab on every activity recreation, for a route that arrived minutes ago.
    // Not rotation — this activity is locked to portrait — but a font or
    // display size change, a locale switch, or "don't keep activities" all
    // recreate it, and the rider should stay on the tab they chose.
    LaunchedEffect(RouteImport.fresh, RouteImport.error) {
        if (RouteImport.fresh || RouteImport.error != null) tab = 1
    }
    // The dashboard was edited in the app while disconnected AND changed on the
    // device meanwhile: never overwrite either silently. App-wide, since the
    // reconnect can happen on any tab.
    ble.dashConflict?.let { c ->
        androidx.compose.material3.AlertDialog(
            onDismissRequest = {},
            title = { Text("Dashboard changed in both places") },
            text = {
                val at = java.text.DateFormat
                    .getDateTimeInstance(java.text.DateFormat.MEDIUM, java.text.DateFormat.SHORT)
                    .format(java.util.Date(c.editedAt))
                Text(
                    "You edited the dashboard layout in the app while your OpenTrailPaper " +
                        "was away ($at), and the layout on the device has changed since. " +
                        "Which one should both keep?",
                )
            },
            confirmButton = {
                androidx.compose.material3.TextButton(
                    onClick = { ble.resolveDashConflict(keepPhone = true) },
                ) { Text("Keep phone version") }
            },
            dismissButton = {
                androidx.compose.material3.TextButton(
                    onClick = { ble.resolveDashConflict(keepPhone = false) },
                ) { Text("Keep device version") }
            },
            containerColor = Palette.surface,
        )
    }

    CompositionLocalProvider(LocalOverlayHost provides overlay) {
    Box(Modifier.fillMaxSize().background(Palette.paper)) {
        Scaffold(
            containerColor = Palette.paper,
            bottomBar = {
                // The device's tab bar: 3dp top rule, CAPS labels, the
                // selected tab inverted (EinkTabBar.kt, as on iOS).
                EinkTabBar(
                    tabs = TABS.map {
                        EinkTab(it.label, it.icon, if (it.label == "Mesh") ble.meshState.unread else 0)
                    },
                    selected = tab,
                    onSelect = { tab = it },
                )
            },
            // The bar pads itself for the gesture bar; the content only
            // needs the status bar kept clear.
            contentWindowInsets = WindowInsets.statusBars,
        ) { padding ->
            // The window doesn't resize for the keyboard (adjustResize under
            // edge-to-edge only reports it), so the tab's content is shrunk to
            // sit on top of it here, for every tab. consumeWindowInsets first:
            // the tab bar's height is already padded off the bottom, and the
            // keyboard covers the bar, so only the rest of it is added.
            Box(Modifier.padding(padding).consumeWindowInsets(padding).imePadding()) {
                when (tab) {
                    0 -> RideScreen(ble)
                    1 -> RouteScreen(ble)
                    2 -> RidesScreen(ble)
                    3 -> MeshScreen(ble)
                    else -> SettingsScreen(ble, host) { showTutorial = true }
                }
            }
        }

        if (showTutorial) {
            OnboardingScreen(ble = ble, host = host) {
                Prefs.onboarded = true
                showTutorial = false
                ble.startCentral()
            }
        }

        // Every full-screen cover in the app is drawn here, above the tabs —
        // see Overlay.kt for why they are not windows.
        // Above the keyboard too, so a cover's text field isn't left under it.
        Box(Modifier.fillMaxSize().imePadding()) { OverlayHost(overlay) }

        // Fixed pairing: the device turned this phone away — say why from
        // whichever tab is up.
        PairingIssueDialog(ble)
    }
    }
}
