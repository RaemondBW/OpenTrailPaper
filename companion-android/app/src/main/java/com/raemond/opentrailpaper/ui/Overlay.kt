package com.raemond.opentrailpaper.ui

import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.MutableState
import androidx.compose.runtime.compositionLocalOf
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateMapOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember

/**
 * Full-screen covers, rendered at the top of the app's own composition rather
 * than in a separate window.
 *
 * A Compose `Dialog` is the obvious tool and the wrong one here: its window is
 * positioned BELOW the status bar while still being sized to the whole screen,
 * and it reports every window inset as zero — so a full-screen layout inside one
 * runs off the bottom of the display by exactly the status bar's height, with no
 * inset value available to correct it. That put the tutorial's primary button
 * half off the screen.
 *
 * Rendering in-tree instead is also what the iOS app does (`fullScreenCover`),
 * and it means insets, theming and the back gesture all behave the way they do
 * everywhere else in the app.
 */
class OverlayHostState internal constructor() {
    internal val slots = mutableStateMapOf<Long, MutableState<@Composable () -> Unit>>()
    internal val order = mutableStateListOf<Long>()
}

val LocalOverlayHost = compositionLocalOf { OverlayHostState() }

@Composable
fun rememberOverlayHostState(): OverlayHostState = remember { OverlayHostState() }

/**
 * Hoists [content] out of wherever it was written and into the overlay host, so
 * a sheet opened from deep inside a tab still covers the tab bar.
 *
 * The content is stored in a state holder that is refreshed on every
 * recomposition of the caller, so the lambda a screen passes stays current — and
 * because it is INVOKED inside the host, the state it reads invalidates the host
 * and nothing else.
 */
@Composable
fun Overlay(content: @Composable () -> Unit) {
    val host = LocalOverlayHost.current
    val id = remember { nextOverlayId++ }
    val slot = remember { mutableStateOf(content) }
    slot.value = content
    DisposableEffect(host, id) {
        host.slots[id] = slot
        host.order.add(id)
        onDispose {
            host.slots.remove(id)
            host.order.remove(id)
        }
    }
}

/** Draws whatever is currently presented, oldest first so sheets stack. */
@Composable
fun OverlayHost(state: OverlayHostState) {
    // Snapshot the ids: a slot that disposes mid-draw would otherwise mutate the
    // list being iterated.
    for (id in state.order.toList()) {
        state.slots[id]?.value?.invoke()
    }
}

private var nextOverlayId = 1L
