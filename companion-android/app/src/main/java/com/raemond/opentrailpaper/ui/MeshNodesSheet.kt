package com.raemond.opentrailpaper.ui

import android.location.Location
import android.text.format.DateUtils
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Check
import androidx.compose.material.icons.filled.LocationOn
import androidx.compose.material.icons.filled.Map
import androidx.compose.material.icons.outlined.LocationOn
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.raemond.opentrailpaper.ble.BleManager
import com.raemond.opentrailpaper.ble.MeshNode
import java.util.Locale

/**
 * Who else is out there. Tapping a node narrows the thread to a direct
 * conversation with it; direct messages are acknowledged, broadcasts are not.
 */
@Composable
fun MeshNodesSheet(
    ble: BleManager,
    selected: Int?,
    onSelect: (Int?) -> Unit,
    onMap: () -> Unit,
    onDismiss: () -> Unit,
) {
    val positioned = ble.meshNodes.filter { it.position != null }

    FullScreenSheet(title = "Nodes", onDismiss = onDismiss) {
        Column(
            Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            Card {
                TrackedLabel("This device")
                Text(
                    ble.meshState.longName.ifEmpty { ble.meshState.nodeId },
                    style = TypeScale.title,
                    color = Palette.ink,
                )
                Text(
                    "${ble.meshState.nodeId} · ${ble.meshState.shortName}",
                    style = barlow(15.sp),
                    color = Palette.muted,
                )
            }

            Card(modifier = Modifier.clickable { onSelect(null) }) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Column(Modifier.weight(1f)) {
                        Text(
                            "Everyone on ${ble.meshState.channel}",
                            style = TypeScale.title,
                            color = Palette.ink,
                        )
                        Text(
                            "Broadcast to the whole channel",
                            style = barlow(13.sp),
                            color = Palette.muted,
                        )
                    }
                    if (selected == null) {
                        Icon(Icons.Filled.Check, contentDescription = null, tint = Palette.accent)
                    }
                }
            }

            Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                TrackedLabel("Heard recently", Modifier.weight(1f))
                // Only offered when there is something to plot: a map of nothing
                // is worse than no map button.
                if (positioned.isNotEmpty()) {
                    Row(
                        Modifier.clickable(onClick = onMap),
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(4.dp),
                    ) {
                        Icon(
                            Icons.Filled.Map,
                            contentDescription = null,
                            tint = Palette.accent,
                            modifier = Modifier.size(16.dp),
                        )
                        Text(
                            "${positioned.size} on map",
                            style = condensed(15.sp, FontWeight.SemiBold),
                            color = Palette.accent,
                        )
                    }
                }
            }

            if (ble.meshNodes.isEmpty()) {
                Text(
                    "No neighbours yet. Nodes appear as they transmit — it can take " +
                        "a few minutes on a quiet mesh.",
                    style = barlow(14.sp),
                    color = Palette.muted,
                )
            }

            for (n in ble.meshNodes) {
                Card(modifier = Modifier.clickable { onSelect(n.num) }) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Column(Modifier.weight(1f)) {
                            Row(
                                verticalAlignment = Alignment.CenterVertically,
                                horizontalArrangement = Arrangement.spacedBy(5.dp),
                            ) {
                                Text(n.displayName, style = TypeScale.title, color = Palette.ink)
                                // The whole point of the glyph: which of these has
                                // told us where it is.
                                n.position?.let { p ->
                                    Icon(
                                        if (p.isImprecise) {
                                            Icons.Outlined.LocationOn
                                        } else {
                                            Icons.Filled.LocationOn
                                        },
                                        contentDescription = null,
                                        tint = if (p.isImprecise) Palette.faint else Palette.good,
                                        modifier = Modifier.size(14.dp),
                                    )
                                }
                            }
                            Text(nodeDetail(n), style = barlow(15.sp), color = Palette.muted)
                            meshPositionLine(n, ble)?.let {
                                Text(it, style = barlow(14.sp), color = Palette.good)
                            }
                        }
                        if (selected == n.num) {
                            Icon(Icons.Filled.Check, contentDescription = null, tint = Palette.accent)
                        }
                    }
                }
            }
            Spacer(Modifier.size(8.dp))
        }
    }
}

private fun nodeDetail(n: MeshNode): String {
    val parts = mutableListOf(n.nodeId)
    if (n.hops == 0) {
        parts.add("direct · ${n.rssi} dBm")
    } else {
        parts.add("${n.hops} hop${if (n.hops == 1) "" else "s"} away")
    }
    parts.add(
        DateUtils.getRelativeTimeSpanString(
            n.lastHeardMs, System.currentTimeMillis(), DateUtils.MINUTE_IN_MILLIS,
        ).toString(),
    )
    return parts.joinToString(" · ")
}

/**
 * Where a node says it is, as a line of text. Separate from the map pin's callout
 * so the list and the map say the same thing.
 */
fun meshPositionLine(n: MeshNode, ble: BleManager): String? {
    val p = n.position ?: return null
    val parts = mutableListOf<String>()
    val here = ble.lastLocation
    if (here != null) {
        val out = FloatArray(1)
        Location.distanceBetween(here.latitude, here.longitude, p.latitude, p.longitude, out)
        val d = out[0]
        parts.add(
            if (d < 1000) String.format(Locale.US, "%.0f m away", d)
            else String.format(Locale.US, "%.1f km away", d / 1000),
        )
    } else {
        parts.add(p.shortText)
    }
    p.uncertaintyM?.let { u ->
        // Say how coarse it is rather than implying the coordinate is exact.
        parts.add(
            if (u < 1000) String.format(Locale.US, "±%.0f m", u)
            else String.format(Locale.US, "±%.0f km", u / 1000),
        )
    }
    if (p.satsInView > 0) parts.add("${p.satsInView} sats")
    return parts.joinToString(" · ")
}
