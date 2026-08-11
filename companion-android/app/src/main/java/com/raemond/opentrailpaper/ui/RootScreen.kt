package com.raemond.opentrailpaper.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Map
import androidx.compose.material.icons.filled.Speed
import androidx.compose.material.icons.filled.Tune
import androidx.compose.material.icons.automirrored.filled.ViewList
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.NavigationBarItemDefaults
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.unit.dp
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.sp
import com.raemond.opentrailpaper.ble.BleManager
import com.raemond.opentrailpaper.data.Prefs

private class Tab(val label: String, val icon: ImageVector)

private val TABS = listOf(
    Tab("Ride", Icons.Filled.Speed),
    Tab("Route", Icons.Filled.Map),
    Tab("Rides", Icons.AutoMirrored.Filled.ViewList),
    Tab("Settings", Icons.Filled.Tune),
)

/**
 * The four tabs, and the full-screen tutorial that covers them on first run.
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
    CompositionLocalProvider(LocalOverlayHost provides overlay) {
    Box(Modifier.fillMaxSize().background(Palette.paper)) {
        Scaffold(
            containerColor = Palette.paper,
            bottomBar = {
                // Cream tab bar with a vermilion selected state and Barlow
                // labels, matching the iOS tab bar.
                Column {
                    HorizontalDivider(color = Palette.hairline)
                    NavigationBar(containerColor = Palette.paper, tonalElevation = 0.dp) {
                        TABS.forEachIndexed { index, item ->
                            NavigationBarItem(
                                selected = tab == index,
                                onClick = { tab = index },
                                icon = { Icon(item.icon, contentDescription = item.label) },
                                label = {
                                    Text(item.label, style = condensed(11.sp, FontWeight.SemiBold))
                                },
                                colors = NavigationBarItemDefaults.colors(
                                    selectedIconColor = Palette.accent,
                                    selectedTextColor = Palette.accent,
                                    unselectedIconColor = Palette.muted,
                                    unselectedTextColor = Palette.muted,
                                    indicatorColor = Palette.accentWash,
                                ),
                            )
                        }
                    }
                }
            },
        ) { padding ->
            Box(Modifier.padding(padding)) {
                when (tab) {
                    0 -> RideScreen(ble)
                    1 -> RouteScreen(ble)
                    2 -> RidesScreen(ble)
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
        OverlayHost(overlay)
    }
    }
}
