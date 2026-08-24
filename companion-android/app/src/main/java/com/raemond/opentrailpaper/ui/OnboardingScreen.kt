package com.raemond.opentrailpaper.ui

import androidx.activity.compose.BackHandler
import androidx.compose.animation.core.animateDpAsState
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.navigationBars
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBars
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.pager.HorizontalPager
import androidx.compose.foundation.pager.rememberPagerState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.CheckCircle
import androidx.compose.material.icons.filled.Error
import androidx.compose.material.icons.filled.Lock
import androidx.compose.material.icons.filled.Map
import androidx.compose.material.icons.filled.SaveAlt
import androidx.compose.material.icons.filled.TouchApp
import androidx.compose.material.icons.automirrored.filled.ViewList
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Icon
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.rotate
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.raemond.opentrailpaper.ble.BleManager
import com.raemond.opentrailpaper.ble.PermissionState
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch

/**
 * First-run tutorial. Introduces what the app does, then primes and triggers the
 * two system permission prompts (location, Bluetooth) on their own explaining
 * screens — so the OS dialog arrives with context rather than cold at launch.
 * Shown once; gated by the onboarded flag in Prefs.
 *
 * Every screen that explains a permission must be able to ACT on it, which is what
 * the permission steps below are built around: if it hasn't been asked, the button
 * raises the system prompt; if it was refused, the button opens Settings (the only
 * place that can be undone); either way there is always a way past the screen
 * without granting anything. A screen that describes a permission and then can't
 * do anything about it is the failure mode to avoid.
 */
@Composable
fun OnboardingScreen(ble: BleManager, host: HostActions, onFinish: () -> Unit) {
    val lastStep = 5
    val pager = rememberPagerState(pageCount = { lastStep + 1 })
    val scope = rememberCoroutineScope()
    val step = pager.currentPage

    /** Which screen the welcome's live head unit is showing. */
    var heroFace by remember { mutableStateOf(DeviceFace.DASHBOARD) }
    /** Which of the three jobs the overview is demonstrating, and whether the
     *  rider has taken the wheel — after the first tap the demo stops cycling. */
    var feature by remember { mutableIntStateOf(0) }
    var drivingOverview by remember { mutableStateOf(false) }

    fun advance() {
        scope.launch { pager.animateScrollToPage(minOf(step + 1, lastStep)) }
    }

    // Auto-advance the moment a permission is granted, so allowing feels instant;
    // denying leaves the button as "Continue" to move on.
    LaunchedEffect(ble.locationAuthorized) {
        if (step == 2 && ble.locationAuthorized) advance()
    }
    LaunchedEffect(ble.bluetoothReady) {
        if (step == 3 && ble.bluetoothReady) advance()
    }

    // Demonstrates itself on a timer until the first tap, then hands over and
    // stays where it's put: a screen that keeps moving under someone reading it is
    // fighting them.
    LaunchedEffect(step, drivingOverview) {
        if (step != 1 || drivingOverview) return@LaunchedEffect
        while (true) {
            delay(6_500)
            feature = (feature + 1) % FEATURES.size
        }
    }

    Overlay {
        // Swallows back and taps: the tutorial is a cover, and the only ways out
        // of it are Skip and Start riding.
        BackHandler {}
        Surface(
            Modifier.fillMaxSize().pointerInput(Unit) {},
            color = Palette.paper,
        ) {
            Column(
                Modifier
                    .fillMaxSize()
                    .windowInsetsPadding(WindowInsets.statusBars)
                    .windowInsetsPadding(WindowInsets.navigationBars),
            ) {
                TopBar(step, lastStep, onSkip = onFinish)

                HorizontalPager(
                    state = pager,
                    modifier = Modifier.fillMaxWidth().weight(1f),
                ) { page ->
                    when (page) {
                        0 -> Welcome(heroFace) { heroFace = heroFace.counterpart }
                        1 -> Overview(feature) { drivingOverview = true; feature = it }
                        2 -> PermissionPage(
                            glyph = SketchGlyph.LOCATION,
                            active = step == 2,
                            state = ble.locationPermission,
                            title = "Share your location",
                            body = "Used to show your position on the map, warm-start the " +
                                "device's GPS so it locks on fast, and act as a backup fix " +
                                "when the device can't see the sky. Only while you're using " +
                                "the app.",
                            granted = "Location allowed",
                            denied = "Location is off for this app. Open Settings to allow it " +
                                "— the map still works without it.",
                            unavailable = "Location is restricted on this phone.",
                        )

                        3 -> PermissionPage(
                            glyph = SketchGlyph.WAVES,
                            active = step == 3,
                            state = ble.bluetoothPermission,
                            title = "Connect over Bluetooth",
                            body = "Everything travels to and from your OpenTrailPaper over " +
                                "Bluetooth — routes, offline maps, settings and recorded " +
                                "rides. No account, no cloud. Next, we'll link the app to " +
                                "your device.",
                            granted = "Bluetooth allowed",
                            denied = "Bluetooth is off for this app. Open Settings to allow " +
                                "it — without it the app can't reach your device at all.",
                            unavailable = "Bluetooth is restricted on this phone.",
                            // Bluetooth has a fourth state the others don't:
                            // allowed, but the radio is switched off. That is not
                            // a refused permission and must not be reported as
                            // one — no button of ours can fix it, only Quick
                            // Settings.
                            override = if (ble.bluetoothPermission.isGranted &&
                                !ble.bluetoothPoweredOn
                            ) {
                                "Bluetooth is switched off — turn it on in Quick Settings."
                            } else {
                                null
                            },
                        )

                        4 -> ConnectPage(ble, active = step == 4)
                        else -> Page(
                            art = {
                                SketchIcon(
                                    SketchGlyph.CHECK,
                                    tint = Palette.good,
                                    active = step == lastStep,
                                )
                            },
                            title = "You're all set",
                            body = "Your device connects on its own whenever it's on and " +
                                "nearby — you'll see it on the Ride tab. Plan a route or " +
                                "build a map any time, and it syncs over. Anything you " +
                                "skipped is listed under Permissions in Settings.",
                        )
                    }
                }

                Footer(
                    step = step,
                    lastStep = lastStep,
                    ble = ble,
                    host = host,
                    onAdvance = { advance() },
                    onFinish = onFinish,
                )
            }
        }
    }
}

// MARK: chrome

@Composable
private fun TopBar(step: Int, lastStep: Int, onSkip: () -> Unit) {
    Row(
        Modifier
            .fillMaxWidth()
            .height(44.dp)
            .padding(horizontal = 24.dp, vertical = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Row(horizontalArrangement = Arrangement.spacedBy(7.dp)) {
            for (i in 0..lastStep) {
                val width by animateDpAsState(if (i == step) 20.dp else 7.dp, label = "dot")
                Box(
                    Modifier
                        .size(width, 7.dp)
                        .background(
                            if (i == step) Palette.accent else Palette.hairline,
                            RoundedCornerShape(50),
                        ),
                )
            }
        }
        Spacer(Modifier.weight(1f))
        if (step < lastStep) {
            TextButton(onClick = onSkip) {
                Text("Skip", style = TypeScale.bodyStrong, color = Palette.muted)
            }
        }
    }
}

@Composable
private fun Footer(
    step: Int,
    lastStep: Int,
    ble: BleManager,
    host: HostActions,
    onAdvance: () -> Unit,
    onFinish: () -> Unit,
) {
    val permission: PermissionState? = when (step) {
        2 -> ble.locationPermission
        3 -> ble.bluetoothPermission
        else -> null
    }

    // What the primary button says and does. The permission steps are driven
    // entirely by the current state, so the button can never be an ask that
    // raises no prompt or a dead end after a refusal.
    val (title, action) = when {
        step == lastStep -> "Start riding" to onFinish
        permission == null -> "Continue" to onAdvance
        permission == PermissionState.NOT_DETERMINED ->
            (if (step == 2) "Allow location access" else "Enable Bluetooth") to
                (if (step == 2) host.requestLocation else host.requestBluetooth)

        permission == PermissionState.DENIED -> "Open Settings" to host.openAppSettings
        else -> "Continue" to onAdvance
    }

    Column(
        Modifier.padding(horizontal = 24.dp).padding(top = 8.dp, bottom = 28.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        PrimaryButton(title) { action() }
        // Permission steps always keep an out on the same screen as the ask, so
        // declining never traps anyone. Hidden only when the primary button IS
        // "Continue" — two identical buttons help nobody.
        if (permission != null && title != "Continue") {
            TextButton(onClick = onAdvance) {
                Text(
                    if (permission == PermissionState.NOT_DETERMINED) "Not now" else "Continue",
                    style = TypeScale.bodyStrong,
                    color = Palette.muted,
                )
            }
        }
    }
}

// MARK: pages

@Composable
private fun Page(
    art: @Composable () -> Unit,
    title: String,
    body: String,
    note: @Composable (() -> Unit)? = null,
) {
    Column(
        Modifier.fillMaxSize().padding(horizontal = 34.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center,
    ) {
        art()
        Spacer(Modifier.height(36.dp))
        Text(
            title,
            style = TypeScale.screenTitle,
            color = Palette.ink,
            textAlign = TextAlign.Center,
        )
        Spacer(Modifier.height(14.dp))
        Text(
            body,
            style = TypeScale.body,
            color = Palette.muted,
            textAlign = TextAlign.Center,
        )
        if (note != null) {
            Spacer(Modifier.height(22.dp))
            note()
        }
        Spacer(Modifier.height(40.dp))
    }
}

/**
 * The head unit, running. It was a pair of product shots, which told a first-time
 * rider nothing a photo of a brick wouldn't: the whole pitch of this device is
 * numbers that hold steady in sunlight and a map that moves under you, and neither
 * survives being frozen. So the front unit is live — ride time counting, power and
 * speed working, the ground scrolling on the map — and tapping it switches screens
 * with the e-paper flash the real button produces. The one behind shows the other
 * screen, held still so the eye knows which of the two it is meant to be watching.
 */
@Composable
private fun Welcome(heroFace: DeviceFace, onTapDevice: () -> Unit) {
    Page(
        art = {
            Box(Modifier.height(300.dp).fillMaxWidth(), contentAlignment = Alignment.Center) {
                // Widths are the SCREEN's; the case adds about 11% around it.
                EInkDevice(
                    heroFace.counterpart,
                    width = 110.dp,
                    live = false,
                    modifier = Modifier
                        .offset(x = (-84).dp, y = 12.dp)
                        .rotate(-9f)
                        .alpha(0.9f),
                )
                EInkDevice(
                    heroFace,
                    width = 135.dp,
                    modifier = Modifier
                        .offset(x = 26.dp)
                        .rotate(5f)
                        .clickable(onClick = onTapDevice),
                )
            }
        },
        title = "Welcome to OpenTrailPaper",
        body = "This is the companion for the OpenTrailPaper head unit — a sunlight-readable " +
            "e-paper bike computer you build yourself. The phone does the fiddly bits; the " +
            "device does the riding.",
        note = {
            Row(
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                Icon(Icons.Filled.TouchApp, contentDescription = null, tint = Palette.muted)
                Text(
                    if (heroFace == DeviceFace.DASHBOARD) {
                        "Tap the device to see the map"
                    } else {
                        "Tap the device to see the numbers"
                    },
                    style = TypeScale.bodyStrong,
                    color = Palette.muted,
                )
            }
        },
    )
}

private class Feature(
    val icon: ImageVector,
    val title: String,
    val body: String,
    val face: AppFace,
)

private val FEATURES = listOf(
    Feature(
        Icons.Filled.Map, "Plan routes",
        "Search a destination and send the route to your device as GPX.",
        AppFace.ROUTE,
    ),
    Feature(
        Icons.Filled.SaveAlt, "Build offline maps",
        "Pick an area; the app bakes map tiles onto the device's SD card.",
        AppFace.MAPS,
    ),
    Feature(
        Icons.AutoMirrored.Filled.ViewList, "Review rides",
        "Pull recorded rides off the device and see distance, power and climb.",
        AppFace.RIDES,
    ),
)

/**
 * What the APP is for, demonstrated on a phone rather than listed.
 *
 * Three bullet points describing a route upload, a map build and a ride readback
 * are three sentences a newcomer has no picture for. Each one owns a screen of
 * this app, so selecting it shows that screen doing that job.
 *
 * A phone, not the head unit: all three are things you do here, in your hand, and
 * showing the bike computer doing them put the work on the wrong device.
 */
@Composable
private fun Overview(feature: Int, onPick: (Int) -> Unit) {
    Column(
        Modifier.fillMaxSize().padding(horizontal = 20.dp, vertical = 12.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Text(
            "What you'll do here",
            style = TypeScale.screenTitle,
            color = Palette.ink,
            modifier = Modifier.fillMaxWidth().padding(horizontal = 8.dp),
        )
        Spacer(Modifier.height(8.dp))
        Box(Modifier.weight(1f), contentAlignment = Alignment.Center) {
            PhoneMock(FEATURES[feature].face, width = 118.dp)
        }
        Spacer(Modifier.height(12.dp))
        Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
            FEATURES.forEachIndexed { i, f ->
                FeatureRow(f, selected = i == feature) { onPick(i) }
            }
        }
    }
}

@Composable
private fun FeatureRow(f: Feature, selected: Boolean, onTap: () -> Unit) {
    Row(
        Modifier
            .fillMaxWidth()
            .background(
                if (selected) Palette.accentWash else Color.Transparent,
                RoundedCornerShape(14.dp),
            )
            .border(
                1.dp,
                if (selected) Palette.accent.copy(alpha = 0.35f) else Color.Transparent,
                RoundedCornerShape(14.dp),
            )
            .clickable(onClick = onTap)
            .padding(horizontal = 12.dp, vertical = 10.dp),
        horizontalArrangement = Arrangement.spacedBy(14.dp),
    ) {
        Icon(
            f.icon,
            contentDescription = null,
            tint = if (selected) Palette.accent else Palette.faint,
            modifier = Modifier.size(24.dp),
        )
        Column {
            Text(f.title, style = TypeScale.title, color = Palette.ink)
            // The unselected rows keep their full text rather than collapsing:
            // this is still the screen that says what the app does, and a list
            // that hides two thirds of itself to look tidy has stopped doing
            // that job.
            Text(f.body, style = barlow(13.5.sp), color = Palette.muted)
        }
    }
}

@Composable
private fun PermissionPage(
    glyph: SketchGlyph,
    active: Boolean,
    state: PermissionState,
    title: String,
    body: String,
    granted: String,
    denied: String,
    unavailable: String,
    override: String? = null,
) {
    Page(
        art = {
            SketchIcon(
                glyph,
                tint = if (state.isGranted) Palette.good else Palette.accent,
                active = active,
            )
        },
        title = title,
        body = body,
        note = {
            when {
                override != null -> Chip(override, Icons.Filled.Error, Palette.accent)
                state == PermissionState.GRANTED ->
                    Chip(granted, Icons.Filled.CheckCircle, Palette.good)

                state == PermissionState.DENIED -> Chip(denied, Icons.Filled.Error, Palette.accent)
                state == PermissionState.UNAVAILABLE ->
                    Chip(unavailable, Icons.Filled.Lock, Palette.muted)

                else -> Unit
            }
        },
    )
}

@Composable
private fun Chip(text: String, icon: ImageVector, tint: Color) {
    Row(
        Modifier
            .background(Palette.surface, RoundedCornerShape(14.dp))
            .border(1.dp, Palette.hairline, RoundedCornerShape(14.dp))
            .padding(horizontal = 16.dp, vertical = 12.dp),
        horizontalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        Icon(icon, contentDescription = null, tint = tint)
        Text(text, style = barlow(13.sp), color = Palette.muted)
    }
}

/**
 * Reassures the rider the app pairs with the head unit, and — because the
 * Bluetooth central is live by now — shows it actually finding the device.
 */
@Composable
private fun ConnectPage(ble: BleManager, active: Boolean) {
    val connected = ble.state == BleManager.ConnState.CONNECTED
    Page(
        art = {
            // The glyph swaps the moment the device shows up; drawing the new one
            // is a better "it worked" than a swap would be.
            SketchIcon(
                if (connected) SketchGlyph.CHECK else SketchGlyph.WAVES,
                tint = if (connected) Palette.good else Palette.accent,
                active = active,
            )
        },
        title = "Pair with your device",
        body = "Turn on your OpenTrailPaper and keep it nearby. The app finds it over " +
            "Bluetooth automatically — no pairing codes to type. Once linked, it stays " +
            "paired and reconnects on its own every ride.",
        note = {
            Row(
                Modifier
                    .background(Palette.surface, RoundedCornerShape(50))
                    .border(1.dp, Palette.hairline, RoundedCornerShape(50))
                    .padding(horizontal = 18.dp, vertical = 12.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(10.dp),
            ) {
                when (ble.state) {
                    BleManager.ConnState.CONNECTED -> Box(
                        Modifier.size(10.dp).background(Palette.good, RoundedCornerShape(50)),
                    )

                    BleManager.ConnState.POWERED_OFF -> Box(
                        Modifier.size(10.dp).background(Palette.faint, RoundedCornerShape(50)),
                    )

                    else -> CircularProgressIndicator(
                        Modifier.size(14.dp),
                        color = Palette.accent,
                        strokeWidth = 2.dp,
                    )
                }
                Text(
                    when (ble.state) {
                        BleManager.ConnState.CONNECTED -> "Connected to your device"
                        BleManager.ConnState.CONNECTING -> "Connecting…"
                        BleManager.ConnState.POWERED_OFF -> "Turn on Bluetooth to connect"
                        else -> "Looking for your device…"
                    },
                    style = TypeScale.bodyStrong,
                    color = Palette.ink,
                )
            }
        },
    )
}
