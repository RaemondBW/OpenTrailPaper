package com.raemond.opentrailpaper.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.navigationBars
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.selection.selectable
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

/** One tab: its CAPS label, its icon and the unread count on it (0 = none). */
class EinkTab(val label: String, val icon: ImageVector, val badge: Int = 0)

/**
 * The app's tab bar, the device's way: a 3dp rule along the top, CAPS labels,
 * and the selected tab as a solid black cell. The cells run down under the
 * gesture bar to the screen's edge; icons and labels stay above it. Port of
 * EinkTabBar in companion-ios/Sources/BikeGPSCompanionApp.swift.
 */
@Composable
fun EinkTabBar(tabs: List<EinkTab>, selected: Int, onSelect: (Int) -> Unit) {
    Column(Modifier.fillMaxWidth().background(Palette.paper)) {
        Box(Modifier.fillMaxWidth().height(Rule.title).background(Palette.ink))
        Row(Modifier.fillMaxWidth()) {
            tabs.forEachIndexed { i, tab ->
                val on = i == selected
                Box(
                    Modifier
                        .weight(1f)
                        .background(if (on) Palette.ink else Palette.paper)
                        .selectable(selected = on, role = Role.Tab) { onSelect(i) }
                        .semantics {
                            contentDescription =
                                if (tab.badge > 0) "${tab.label}, ${tab.badge} unread" else tab.label
                        }
                        .windowInsetsPadding(WindowInsets.navigationBars),
                    contentAlignment = Alignment.Center,
                ) {
                    Column(
                        Modifier.height(56.dp),
                        horizontalAlignment = Alignment.CenterHorizontally,
                        verticalArrangement = Arrangement.spacedBy(4.dp, Alignment.CenterVertically),
                    ) {
                        val fg = if (on) Palette.paper else Palette.ink
                        Box {
                            Icon(tab.icon, contentDescription = null, tint = fg, modifier = Modifier.size(22.dp))
                            // Unread mesh messages arrive while another tab is
                            // up, so the count is visible from anywhere.
                            if (tab.badge > 0) {
                                Text(
                                    if (tab.badge > 99) "99+" else "${tab.badge}",
                                    style = barlow(10.sp, FontWeight.Bold),
                                    color = if (on) Palette.ink else Palette.paper,
                                    modifier = Modifier
                                        .align(Alignment.TopEnd)
                                        .offset(x = 12.dp, y = (-6).dp)
                                        .background(if (on) Palette.paper else Palette.ink)
                                        .border(1.dp, if (on) Palette.ink else Palette.paper)
                                        .padding(horizontal = 4.dp, vertical = 1.dp),
                                )
                            }
                        }
                        Text(
                            tab.label.uppercase(),
                            style = caps(11.sp, 1.5.sp),
                            color = fg,
                            maxLines = 1,
                        )
                    }
                }
            }
        }
    }
}
