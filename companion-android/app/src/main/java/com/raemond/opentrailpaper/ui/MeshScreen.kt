package com.raemond.opentrailpaper.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.Send
import androidx.compose.material.icons.filled.Cancel
import androidx.compose.material.icons.filled.CheckCircle
import androidx.compose.material.icons.filled.Check
import androidx.compose.material.icons.filled.Group
import androidx.compose.material.icons.filled.Lock
import androidx.compose.material.icons.filled.PowerSettingsNew
import androidx.compose.material.icons.filled.Schedule
import androidx.compose.material.icons.filled.SettingsInputAntenna
import androidx.compose.material.icons.filled.Tune
import androidx.compose.material.icons.filled.Warning
import androidx.compose.material.icons.filled.WifiTetheringOff
import androidx.compose.material3.Badge
import androidx.compose.material3.BadgedBox
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.Switch
import androidx.compose.material3.SwitchDefaults
import androidx.compose.material3.Text
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
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.raemond.opentrailpaper.ble.BleManager
import com.raemond.opentrailpaper.ble.MeshChannel
import com.raemond.opentrailpaper.ble.MeshMessage
import com.raemond.opentrailpaper.ble.meshNodeId
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * Mesh text messaging over the head unit's LoRa radio.
 *
 * The device is a Meshtastic node: it holds the channel key, does the radio work
 * and keeps the last few dozen messages. This screen is the keyboard and the
 * screen for it — everything shown here was streamed off the device, so a phone
 * that has been away still sees what arrived while it was gone.
 *
 * Deliberately not a per-contact inbox. A mesh channel is one shared
 * conversation, the way a radio net is, and direct messages are the exception —
 * so the channel is the default view and picking a node narrows it.
 */
@Composable
fun MeshScreen(ble: BleManager) {
    var draft by remember { mutableStateOf("") }
    /**
     * Who we are talking to, as a node NUMBER rather than a MeshNode — the node
     * list is rebuilt from the device on every refresh, so a stored object goes
     * stale (its name and signal are a snapshot) while the number does not.
     */
    var recipientNum by remember { mutableStateOf<Int?>(null) }
    var showNodes by remember { mutableStateOf(false) }
    var showSettings by remember { mutableStateOf(false) }
    var showNodeMap by remember { mutableStateOf(false) }
    var showChannels by remember { mutableStateOf(false) }
    /** Which channel's thread is on screen. 0 is the public/primary one. */
    var channel by remember { mutableStateOf(0) }

    val attached = ble.meshAttached
    val state = ble.meshState

    val recipient = recipientNum?.let { n -> ble.meshNodes.firstOrNull { it.num == n } }
    val recipientName = recipientNum?.let { recipient?.displayName ?: meshNodeId(it) } ?: ""
    val currentChannel = ble.meshChannels.firstOrNull { it.index == channel }

    // Messages on the channel being viewed. Channels are separated first and
    // hard: a private channel exists precisely so its traffic is not mixed in
    // with the public one.
    val messages = remember(ble.meshMessages, channel, recipientNum) {
        val onChannel = ble.meshMessages.filter { it.channel == channel }
        val n = recipientNum
        if (n == null) onChannel else onChannel.filter { it.from == n || it.to == n }
    }

    LaunchedEffect(attached) {
        if (attached) {
            ble.refreshMesh()
            ble.markMeshRead()
        }
    }

    // Forgetting the channel you were reading should not leave a thread on screen
    // that no longer exists.
    LaunchedEffect(ble.meshChannels) {
        if (ble.meshChannels.isNotEmpty() && ble.meshChannels.none { it.index == channel }) {
            channel = 0
        }
    }

    Column(Modifier.fillMaxSize().background(Palette.paper)) {
        MeshHeader(
            subtitle = meshSubtitle(ble, attached, recipientNum, recipientName, currentChannel),
            attached = attached,
            enabled = state.enabled,
            nodeCount = state.nodeCount,
            onToggleRadio = { ble.setMeshEnabled(it) },
            onNodes = { showNodes = true },
            onSettings = { showSettings = true },
        )

        when {
            !attached -> MeshEmpty(
                Icons.Filled.WifiTetheringOff,
                "Not connected",
                if (ble.state == BleManager.ConnState.CONNECTED) {
                    "This firmware has no mesh radio support. Update the device to use Mesh."
                } else {
                    "Connect to your OpenTrailPaper to read and send mesh messages."
                },
            )

            !state.enabled -> Column(
                Modifier.fillMaxSize(),
                horizontalAlignment = Alignment.CenterHorizontally,
            ) {
                MeshEmpty(
                    Icons.Filled.PowerSettingsNew,
                    "Mesh radio is off",
                    "Turn it on with the switch above to join the mesh and send " +
                        "messages. The radio listens continuously, so it draws a " +
                        "little power even when nothing is being said.",
                    modifier = Modifier.weight(1f),
                )
                PrimaryButton(
                    title = "Turn on the radio",
                    modifier = Modifier.padding(horizontal = 32.dp, vertical = 28.dp),
                ) { ble.setMeshEnabled(true) }
            }

            !state.radioOk -> MeshEmpty(
                Icons.Filled.Warning,
                "No radio found",
                "The device could not talk to its SX1262. On the Lite board there " +
                    "is no LoRa module fitted; otherwise check the device log.",
            )

            else -> {
                ChannelBar(
                    channels = ble.meshChannels,
                    selected = channel,
                    onSelect = { channel = it },
                    onManage = { showChannels = true },
                )
                Thread(
                    messages = messages,
                    isPrivate = currentChannel?.isPrivate == true,
                    direct = recipientNum != null,
                    nameFor = { num -> meshDisplayName(ble, num) },
                    modifier = Modifier.weight(1f),
                )
                Composer(
                    draft = draft,
                    onDraft = { draft = it },
                    rejected = ble.meshSendRejected,
                    recipientName = if (recipientNum == null) null else recipientName,
                    onClearRecipient = { recipientNum = null },
                    onSend = {
                        ble.sendMeshText(draft, to = recipientNum, channel = channel)
                        draft = ""
                    },
                )
            }
        }
    }

    if (showNodes) {
        MeshNodesSheet(
            ble = ble,
            selected = recipientNum,
            onSelect = { recipientNum = it; showNodes = false },
            onMap = { showNodes = false; showNodeMap = true },
            onDismiss = { showNodes = false },
        )
    }
    if (showSettings) MeshSettingsSheet(ble) { showSettings = false }
    if (showNodeMap) MeshMapSheet(ble) { showNodeMap = false }
    if (showChannels) {
        MeshChannelsSheet(
            ble = ble,
            selected = channel,
            onSelect = { channel = it },
            onDismiss = { showChannels = false },
        )
    }
}

/** The name to show for a node number, live if we know it. */
internal fun meshDisplayName(ble: BleManager, num: Int): String {
    if (num == ble.meshState.nodeNum) {
        return ble.meshState.longName.ifEmpty { ble.meshState.nodeId }
    }
    ble.meshNodes.firstOrNull { it.num == num }?.let { return it.displayName }
    // Heard before its NodeInfo arrived: the id is the honest answer.
    return meshNodeId(num)
}

private fun meshSubtitle(
    ble: BleManager,
    attached: Boolean,
    recipientNum: Int?,
    recipientName: String,
    currentChannel: MeshChannel?,
): String {
    val s = ble.meshState
    if (!attached) return "not connected"
    if (!s.enabled) return "radio off"
    if (recipientNum != null) return "direct to $recipientName"
    if (currentChannel?.isPrivate == true) return "${currentChannel.displayName} · private"
    val preset = ble.meshPresets.firstOrNull { it.index == s.presetIndex }
    // Channel, modem, frequency: the three things that have to match another
    // node, in one line.
    return if (preset != null) {
        String.format(Locale.US, "%s · SF%d · %.3f MHz", s.channel, preset.sf, s.frequencyMHz)
    } else {
        String.format(Locale.US, "%s · %.3f MHz", s.channel, s.frequencyMHz)
    }
}

@Composable
private fun MeshHeader(
    subtitle: String,
    attached: Boolean,
    enabled: Boolean,
    nodeCount: Int,
    onToggleRadio: (Boolean) -> Unit,
    onNodes: () -> Unit,
    onSettings: () -> Unit,
) {
    Row(
        Modifier.fillMaxWidth().padding(start = 16.dp, end = 8.dp, top = 8.dp, bottom = 12.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(Modifier.weight(1f)) {
            Text("Mesh", style = TypeScale.screenTitle, color = Palette.ink)
            TrackedLabel(subtitle)
        }
        if (attached) {
            Switch(
                checked = enabled,
                onCheckedChange = onToggleRadio,
                colors = SwitchDefaults.colors(
                    checkedThumbColor = Palette.accentInk,
                    checkedTrackColor = Palette.accent,
                ),
            )
        }
        IconButton(onClick = onNodes, enabled = enabled) {
            BadgedBox(badge = {
                if (nodeCount > 0) {
                    Badge(containerColor = Palette.accent, contentColor = Palette.accentInk) {
                        Text("$nodeCount", style = condensed(11.sp, FontWeight.Bold))
                    }
                }
            }) {
                Icon(
                    Icons.Filled.Group,
                    contentDescription = "Nodes",
                    tint = if (enabled) Palette.ink else Palette.faint,
                )
            }
        }
        IconButton(onClick = onSettings) {
            Icon(Icons.Filled.SettingsInputAntenna, contentDescription = "Radio", tint = Palette.ink)
        }
    }
}

/**
 * One chip per channel, plus the way in to creating or joining one. Tapping the
 * channel you are already reading opens its details, so a chip is also the way
 * back to the QR.
 */
@Composable
private fun ChannelBar(
    channels: List<MeshChannel>,
    selected: Int,
    onSelect: (Int) -> Unit,
    onManage: () -> Unit,
) {
    if (channels.isEmpty()) return
    Row(
        Modifier.fillMaxWidth().padding(bottom = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Row(
            Modifier.weight(1f).horizontalScroll(rememberScrollState()).padding(start = 16.dp),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            for (c in channels) {
                val on = c.index == selected
                Row(
                    Modifier
                        .background(if (on) Palette.accent else Palette.surface, CircleShape)
                        .border(
                            1.dp,
                            if (on) Color.Transparent else Palette.hairline,
                            CircleShape,
                        )
                        .clickable { if (on) onManage() else onSelect(c.index) }
                        .padding(horizontal = 12.dp, vertical = 6.dp),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(5.dp),
                ) {
                    if (c.isPrivate) {
                        Icon(
                            Icons.Filled.Lock,
                            contentDescription = null,
                            tint = if (on) Palette.accentInk else Palette.ink,
                            modifier = Modifier.size(11.dp),
                        )
                    }
                    Text(
                        if (c.isPrimary) "Public" else c.displayName,
                        style = condensed(16.sp, FontWeight.SemiBold),
                        color = if (on) Palette.accentInk else Palette.ink,
                    )
                }
            }
        }
        // Pinned, not part of the scroll: with a few channels the trailing chip
        // is exactly what slides out of reach.
        Box(
            Modifier
                .padding(start = 8.dp, end = 16.dp)
                .background(Palette.surface, CircleShape)
                .border(1.dp, Palette.hairline, CircleShape)
                .clickable(onClick = onManage)
                .padding(horizontal = 12.dp, vertical = 8.dp),
        ) {
            Icon(
                Icons.Filled.Tune,
                contentDescription = "Channels",
                tint = Palette.accent,
                modifier = Modifier.size(15.dp),
            )
        }
    }
}

@Composable
private fun MeshEmpty(
    icon: ImageVector,
    title: String,
    body: String,
    modifier: Modifier = Modifier,
) {
    Column(
        modifier.fillMaxWidth().fillMaxSize().padding(horizontal = 34.dp),
        verticalArrangement = Arrangement.Center,
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Icon(icon, contentDescription = null, tint = Palette.faint, modifier = Modifier.size(34.dp))
        Spacer(Modifier.size(10.dp))
        Text(title, style = TypeScale.title, color = Palette.ink)
        Spacer(Modifier.size(4.dp))
        Text(body, style = barlow(15.sp), color = Palette.muted, textAlign = TextAlign.Center)
    }
}

@Composable
private fun Thread(
    messages: List<MeshMessage>,
    isPrivate: Boolean,
    direct: Boolean,
    nameFor: (Int) -> String,
    modifier: Modifier = Modifier,
) {
    val listState = rememberLazyListState()
    // Follow the conversation, the way any chat does.
    LaunchedEffect(messages.size) {
        if (messages.isNotEmpty()) listState.animateScrollToItem(messages.lastIndex)
    }
    LazyColumn(
        modifier = modifier.fillMaxWidth(),
        state = listState,
        contentPadding = androidx.compose.foundation.layout.PaddingValues(
            start = 16.dp, end = 16.dp, bottom = 8.dp,
        ),
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        if (messages.isEmpty()) {
            item {
                Text(
                    when {
                        direct -> "No messages with this node yet."
                        isPrivate -> "Nothing here yet. Only people you have shared " +
                            "this channel's code with can read it."
                        else -> "Nothing heard yet. Messages from anyone on this " +
                            "channel show up here."
                    },
                    style = barlow(15.sp),
                    color = Palette.muted,
                    textAlign = TextAlign.Center,
                    modifier = Modifier.fillMaxWidth().padding(horizontal = 30.dp, vertical = 40.dp),
                )
            }
        }
        items(messages, key = { it.id }) { m ->
            MeshBubble(m, nameFor(m.from))
        }
    }
}

/**
 * One message. Ours on the right, everyone else's on the left — the convention
 * every messaging app shares, and the fastest way to read a thread at a glance.
 */
@Composable
private fun MeshBubble(message: MeshMessage, senderName: String) {
    val time = remember(message.timeMs) {
        SimpleDateFormat("HH:mm", Locale.getDefault()).format(Date(message.timeMs))
    }
    // A 40 dp gutter on the far side rather than a percentage cap: it is the one
    // thing that has to hold, so a bubble is never edge-to-edge and the side it
    // sits on stays readable at a glance.
    Row(
        Modifier
            .fillMaxWidth()
            .padding(start = if (message.outgoing) 40.dp else 0.dp,
                     end = if (message.outgoing) 0.dp else 40.dp),
        horizontalArrangement = if (message.outgoing) Arrangement.End else Arrangement.Start,
    ) {
        Column(
            Modifier
                .weight(1f, fill = false)
                .background(
                    if (message.outgoing) Palette.accent else Palette.surface,
                    RoundedCornerShape(18.dp),
                )
                .border(
                    1.dp,
                    if (message.outgoing) Color.Transparent else Palette.hairline,
                    RoundedCornerShape(18.dp),
                )
                .padding(horizontal = 14.dp, vertical = 10.dp),
            horizontalAlignment = if (message.outgoing) Alignment.End else Alignment.Start,
            verticalArrangement = Arrangement.spacedBy(3.dp),
        ) {
            if (!message.outgoing) {
                Text(senderName, style = condensed(16.sp, FontWeight.SemiBold), color = Palette.accent)
            }
            Text(
                message.text,
                style = barlow(16.sp),
                color = if (message.outgoing) Palette.accentInk else Palette.ink,
            )
            Row(
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(6.dp),
            ) {
                val meta = Palette.accentWash.takeIf { message.outgoing } ?: Palette.faint
                Text(time, style = condensed(15.sp, FontWeight.Medium), color = meta)
                if (!message.outgoing) {
                    Text(
                        if (message.hops > 0) {
                            "· ${message.hops} hop${if (message.hops == 1) "" else "s"}"
                        } else {
                            "· ${message.rssi} dBm"
                        },
                        style = condensed(15.sp, FontWeight.Medium),
                        color = meta,
                    )
                }
                if (message.outgoing) StatusMark(message.status, meta)
            }
        }
    }
}

/**
 * Queued / on the air / acknowledged / gave up. A broadcast stops at "sent":
 * there is nobody in particular to acknowledge it, and showing a permanently
 * unacknowledged tick would read as a failure.
 */
@Composable
private fun StatusMark(status: MeshMessage.Status, tint: Color) {
    val icon = when (status) {
        MeshMessage.Status.PENDING -> Icons.Filled.Schedule
        MeshMessage.Status.SENT -> Icons.Filled.Check
        MeshMessage.Status.ACKED -> Icons.Filled.CheckCircle
        MeshMessage.Status.FAILED -> Icons.Filled.Warning
    }
    Icon(icon, contentDescription = status.name, tint = tint, modifier = Modifier.size(13.dp))
}

@Composable
private fun Composer(
    draft: String,
    onDraft: (String) -> Unit,
    rejected: Boolean,
    recipientName: String?,
    onClearRecipient: () -> Unit,
    onSend: () -> Unit,
) {
    val bytes = remember(draft) { draft.toByteArray(Charsets.UTF_8).size }
    val canSend = draft.isNotBlank()
    Column {
        HorizontalDivider(color = Palette.hairline)
        Column(
            Modifier
                .background(Palette.paper)
                .imePadding()
                .padding(horizontal = 16.dp, vertical = 8.dp),
            verticalArrangement = Arrangement.spacedBy(6.dp),
        ) {
            if (rejected) {
                Text(
                    "The device could not queue that — its outbox is full. " +
                        "Try again in a moment.",
                    style = barlow(13.sp),
                    color = Palette.accent,
                )
            }
            Row(
                verticalAlignment = Alignment.Bottom,
                horizontalArrangement = Arrangement.spacedBy(10.dp),
            ) {
                if (recipientName != null) {
                    IconButton(onClick = onClearRecipient) {
                        Icon(Icons.Filled.Cancel, "Message the channel instead", tint = Palette.faint)
                    }
                }
                TextField(
                    value = draft,
                    onValueChange = onDraft,
                    modifier = Modifier.weight(1f),
                    placeholder = {
                        Text(
                            recipientName?.let { "Message $it" } ?: "Message the channel",
                            style = barlow(16.sp),
                            color = Palette.faint,
                        )
                    },
                    textStyle = barlow(16.sp),
                    maxLines = 4,
                    shape = RoundedCornerShape(20.dp),
                    keyboardOptions = KeyboardOptions(imeAction = androidx.compose.ui.text.input.ImeAction.Send),
                    keyboardActions = KeyboardActions(onSend = { if (canSend) onSend() }),
                    colors = TextFieldDefaults.colors(
                        focusedContainerColor = Palette.surface,
                        unfocusedContainerColor = Palette.surface,
                        focusedTextColor = Palette.ink,
                        unfocusedTextColor = Palette.ink,
                        cursorColor = Palette.accent,
                        focusedIndicatorColor = Color.Transparent,
                        unfocusedIndicatorColor = Color.Transparent,
                    ),
                )
                Box(
                    Modifier
                        .size(42.dp)
                        .background(if (canSend) Palette.accent else Palette.faint, CircleShape)
                        .clickable(enabled = canSend, onClick = onSend),
                    contentAlignment = Alignment.Center,
                ) {
                    Icon(
                        Icons.AutoMirrored.Filled.Send,
                        contentDescription = "Send",
                        tint = Palette.accentInk,
                        modifier = Modifier.size(18.dp),
                    )
                }
            }
            // Air time is the real cost on a mesh, so the budget is visible
            // rather than enforced by a silent truncation.
            if (bytes > 140) {
                Text(
                    "$bytes/200 bytes",
                    style = condensed(14.sp, FontWeight.Medium),
                    color = if (bytes > 200) Palette.accent else Palette.faint,
                    modifier = Modifier.fillMaxWidth(),
                    textAlign = TextAlign.End,
                )
            }
        }
    }
}
