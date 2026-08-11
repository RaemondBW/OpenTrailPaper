package com.raemond.opentrailpaper.ui

import androidx.activity.compose.BackHandler
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.navigationBars
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.statusBars
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

/**
 * A full-screen modal with a Done button, standing in for the iOS `.sheet`.
 *
 * Presented through [Overlay] rather than a `Dialog` — see that file for why a
 * dialog window cannot lay out a full-screen page correctly. Being in-tree also
 * means the back gesture has to be handled explicitly, which is no bad thing:
 * dismissing is then the same action however it is invoked.
 */
@Composable
fun FullScreenSheet(
    title: String,
    onDismiss: () -> Unit,
    confirmLabel: String = "Done",
    leading: @Composable RowScope.() -> Unit = {},
    trailing: @Composable RowScope.() -> Unit = {},
    content: @Composable ColumnScope.() -> Unit,
) {
    Overlay {
        BackHandler(onBack = onDismiss)
        Surface(
            Modifier
                .fillMaxSize()
                // Swallows taps, so nothing behind the sheet can be operated
                // through it — the one thing a dialog window gave for free.
                .pointerInput(Unit) {},
            color = Palette.paper,
        ) {
            Column(
                Modifier
                    .fillMaxSize()
                    .windowInsetsPadding(WindowInsets.statusBars),
            ) {
                Row(
                    Modifier
                        .fillMaxWidth()
                        .background(Palette.paper)
                        .padding(horizontal = 12.dp, vertical = 10.dp),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(4.dp),
                ) {
                    leading()
                    Text(
                        title,
                        style = condensed(22.sp, FontWeight.Bold),
                        color = Palette.ink,
                        modifier = Modifier.weight(1f).padding(start = 4.dp),
                    )
                    trailing()
                    TextButton(onClick = onDismiss) {
                        Text(confirmLabel, style = TypeScale.bodyStrong, color = Palette.accent)
                    }
                }
                HorizontalDivider(color = Palette.hairline)
                Box(
                    Modifier
                        .weight(1f)
                        .windowInsetsPadding(WindowInsets.navigationBars),
                ) {
                    Column(Modifier.fillMaxSize(), content = content)
                }
            }
        }
    }
}

/**
 * A full-screen cover with no chrome of its own — for a page that IS a map and
 * floats its own controls over it, the way the iOS Maps screen does.
 *
 * Sheets whose content is an Android view (the map) cannot use the bar above:
 * the platform view is handed to the system to draw and ends up over any Compose
 * content composed before it, so the bar simply disappears behind the map. A page
 * that composes its chrome AFTER the map has no such problem.
 */
@Composable
fun FullScreenCover(onDismiss: () -> Unit, content: @Composable () -> Unit) {
    Overlay {
        BackHandler(onBack = onDismiss)
        Surface(
            Modifier.fillMaxSize().pointerInput(Unit) {},
            color = Palette.paper,
        ) {
            content()
        }
    }
}
