package com.raemond.opentrailpaper.ui

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
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Icon
import androidx.compose.material3.Switch
import androidx.compose.material3.SwitchDefaults
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TextField
import androidx.compose.material3.TextFieldDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.raemond.opentrailpaper.ble.BleManager
import com.raemond.opentrailpaper.ble.MeshPreset
import java.util.Locale

/**
 * Radio and identity.
 *
 * Region is absent on purpose: which band the radio may use is set at build time
 * in the firmware, because it is a certification question rather than a
 * preference.
 */
@Composable
fun MeshSettingsSheet(ble: BleManager, onDismiss: () -> Unit) {
    val state = ble.meshState
    var longName by remember { mutableStateOf("") }
    var shortName by remember { mutableStateOf("") }
    var channel by remember { mutableStateOf("") }
    var channelKey by remember { mutableStateOf(1) }
    var confirmChannel by remember { mutableStateOf(false) }
    /**
     * Set while the "switch modem?" dialog is up. Confirmed rather than applied on
     * tap because it retunes the radio, and on a bandwidth change it moves the
     * frequency too — not something to do on a mis-tap.
     */
    var pendingPreset by remember { mutableStateOf<MeshPreset?>(null) }

    val activePreset = ble.meshPresets.firstOrNull { it.index == state.presetIndex }

    LaunchedEffect(Unit) {
        longName = state.longName
        shortName = state.shortName
        // Left blank when the name is the modem's, so the field shows the state
        // rather than looking like a pinned custom channel.
        channel = if (state.channelFollowsPreset) "" else state.channel
        channelKey = state.channelKey
        ble.requestMeshStats()
    }

    FullScreenSheet(title = "Mesh", onDismiss = onDismiss) {
        Column(
            Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(14.dp),
        ) {
            Card {
                TrackedLabel("Radio")
                Spacer(Modifier.size(6.dp))
                Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                    Text("LoRa radio", style = barlow(16.sp), color = Palette.ink,
                        modifier = Modifier.weight(1f))
                    Switch(
                        checked = state.enabled,
                        onCheckedChange = { ble.setMeshEnabled(it) },
                        colors = SwitchDefaults.colors(
                            checkedThumbColor = Palette.accentInk,
                            checkedTrackColor = Palette.accent,
                        ),
                    )
                }
                InfoRow("Node", state.nodeId)
                InfoRow("Channel", "${state.channel} · key ${state.channelKey}")
                if (state.channelFollowsPreset) {
                    Text(
                        "The channel name comes from the modem, the way an " +
                            "unconfigured Meshtastic node works — so changing the " +
                            "modem moves the channel and the frequency with it.",
                        style = barlow(13.sp),
                        color = Palette.muted,
                    )
                }
                InfoRow("Modem", activePreset?.let { "${it.name} · SF${it.sf}" } ?: "—")
                InfoRow(
                    "Frequency",
                    String.format(Locale.US, "%.3f MHz", state.frequencyMHz),
                )
                InfoRow("Status", if (state.radioOk) "up" else "not found")
            }

            Card {
                TrackedLabel("Modem · how fast")
                Spacer(Modifier.size(6.dp))
                // Listed from the device, not from a table in the app: the
                // firmware decides which modems exist.
                if (ble.meshPresets.isEmpty()) {
                    Text(
                        "Ask the device for its modem list by reconnecting.",
                        style = barlow(15.sp),
                        color = Palette.muted,
                    )
                }
                for (p in ble.meshPresets) {
                    Row(
                        Modifier
                            .fillMaxWidth()
                            .clickable { pendingPreset = p }
                            .padding(vertical = 3.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Column(Modifier.weight(1f)) {
                            Text(
                                p.name,
                                style = condensed(19.sp, FontWeight.SemiBold),
                                color = Palette.ink,
                            )
                            Text(p.detail, style = barlow(14.sp), color = Palette.muted)
                        }
                        if (p.index == state.presetIndex) {
                            Icon(Icons.Filled.Check, contentDescription = null, tint = Palette.accent)
                        }
                    }
                }
                Text(
                    "How fast the radio talks, not where. Lower spreading factors " +
                        "are quicker and cheaper on battery but carry less far. Every " +
                        "node you want to reach must use the same modem — a mismatch " +
                        "is silent, not just slow.",
                    style = barlow(14.sp),
                    color = Palette.muted,
                )
            }

            Card {
                TrackedLabel("This node's name")
                Spacer(Modifier.size(6.dp))
                MeshTextField(longName, { longName = it }, "Long name")
                Spacer(Modifier.size(8.dp))
                MeshTextField(
                    shortName,
                    { shortName = it.take(4) },
                    "Short (4 characters)",
                )
                Text(
                    "How other people's Meshtastic apps will label your messages.",
                    style = barlow(13.sp),
                    color = Palette.muted,
                    modifier = Modifier.padding(top = 6.dp),
                )
                TextButton(onClick = { ble.setMeshNames(longName, shortName) }) {
                    Text(
                        "Save name",
                        style = condensed(18.sp, FontWeight.SemiBold),
                        color = Palette.accent,
                    )
                }
            }

            Card {
                TrackedLabel("Channel · where")
                Spacer(Modifier.size(6.dp))
                MeshTextField(
                    channel,
                    { channel = it },
                    if (state.channelFollowsPreset) {
                        "Same as the modem (${state.channel})"
                    } else {
                        "Channel name"
                    },
                )
                Spacer(Modifier.size(8.dp))
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text("Key $channelKey", style = barlow(16.sp), color = Palette.ink,
                        modifier = Modifier.weight(1f))
                    TextButton(
                        onClick = { channelKey = (channelKey - 1).coerceAtLeast(1) },
                        enabled = channelKey > 1,
                    ) { Text("−", style = condensed(22.sp, FontWeight.Bold)) }
                    TextButton(
                        onClick = { channelKey = (channelKey + 1).coerceAtMost(10) },
                        enabled = channelKey < 10,
                    ) { Text("+", style = condensed(22.sp, FontWeight.Bold)) }
                }
                Text(
                    "Everyone you want to talk to must use the same name and key. " +
                        "The name is what sets the frequency, which is why changing " +
                        "it retunes the radio. Leave it empty to follow the modem, " +
                        "which is what a stock Meshtastic node does and the setting " +
                        "most likely to reach other people.",
                    style = barlow(13.sp),
                    color = Palette.muted,
                )
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    TextButton(
                        onClick = { confirmChannel = true },
                        enabled = channel.isNotBlank(),
                    ) {
                        Text(
                            "Switch channel",
                            style = condensed(18.sp, FontWeight.SemiBold),
                            color = if (channel.isBlank()) Palette.faint else Palette.accent,
                        )
                    }
                    if (!state.channelFollowsPreset) {
                        TextButton(onClick = { channel = ""; confirmChannel = true }) {
                            Text(
                                "Follow the modem",
                                style = condensed(18.sp, FontWeight.SemiBold),
                                color = Palette.accent,
                            )
                        }
                    }
                }
            }

            Card {
                TrackedLabel("Packets")
                Spacer(Modifier.size(6.dp))
                InfoRow("Received", "${ble.meshStats.rx}")
                InfoRow("Other channels", "${ble.meshStats.rxOtherChannel}")
                InfoRow("Duplicates", "${ble.meshStats.rxDuplicate}")
                InfoRow("Dropped", "${ble.meshStats.rxDropped}")
                InfoRow("Sent", "${ble.meshStats.tx}")
                InfoRow("Send failures", "${ble.meshStats.txFailed}")
                InfoRow("Acknowledged", "${ble.meshStats.acksRx}")
            }
            Spacer(Modifier.size(8.dp))
        }
    }

    if (confirmChannel) {
        AlertDialog(
            onDismissRequest = { confirmChannel = false },
            title = { Text("Switch channel?") },
            text = {
                Text(
                    if (channel.isBlank()) {
                        "The channel name will follow the modem preset again. The " +
                            "device retunes its radio and clears the messages and " +
                            "neighbours it learned on the old channel."
                    } else {
                        "The device retunes its radio and clears the messages and " +
                            "neighbours it learned on the old channel."
                    },
                )
            },
            confirmButton = {
                TextButton(onClick = {
                    confirmChannel = false
                    ble.setMeshChannel(channel, channelKey)
                }) { Text("Switch") }
            },
            dismissButton = {
                TextButton(onClick = { confirmChannel = false }) { Text("Cancel") }
            },
            containerColor = Palette.surface,
        )
    }

    pendingPreset?.let { p ->
        AlertDialog(
            onDismissRequest = { pendingPreset = null },
            title = { Text("Switch modem?") },
            text = {
                Text(
                    "Switch to ${p.name} (${p.detail})? Every node you want to talk " +
                        "to has to use the same modem, and a mismatch means hearing " +
                        "nothing at all rather than hearing it slowly.",
                )
            },
            confirmButton = {
                TextButton(onClick = {
                    ble.setMeshPreset(p.index)
                    pendingPreset = null
                }) { Text("Switch") }
            },
            dismissButton = {
                TextButton(onClick = { pendingPreset = null }) { Text("Cancel") }
            },
            containerColor = Palette.surface,
        )
    }
}

@Composable
private fun InfoRow(label: String, value: String) {
    Row(Modifier.fillMaxWidth().padding(vertical = 2.dp)) {
        Text(label, style = barlow(15.sp), color = Palette.muted, modifier = Modifier.weight(1f))
        Text(value, style = condensed(17.sp, FontWeight.SemiBold), color = Palette.ink)
    }
}

/** The app's text field, in the one shape every mesh form uses. */
@Composable
internal fun MeshTextField(
    value: String,
    onValueChange: (String) -> Unit,
    placeholder: String,
    modifier: Modifier = Modifier,
) {
    TextField(
        value = value,
        onValueChange = onValueChange,
        modifier = modifier.fillMaxWidth(),
        placeholder = { Text(placeholder, style = barlow(16.sp), color = Palette.faint) },
        textStyle = barlow(16.sp),
        singleLine = true,
        colors = TextFieldDefaults.colors(
            focusedContainerColor = Palette.paper,
            unfocusedContainerColor = Palette.paper,
            focusedTextColor = Palette.ink,
            unfocusedTextColor = Palette.ink,
            cursorColor = Palette.accent,
            focusedIndicatorColor = Palette.accent,
            unfocusedIndicatorColor = Palette.hairline,
        ),
    )
}
