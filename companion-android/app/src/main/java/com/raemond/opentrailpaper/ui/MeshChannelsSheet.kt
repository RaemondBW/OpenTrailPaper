package com.raemond.opentrailpaper.ui

import android.content.Intent
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Check
import androidx.compose.material.icons.filled.Lock
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.FilterQuality
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalClipboardManager
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.raemond.opentrailpaper.ble.BleManager
import com.raemond.opentrailpaper.ble.MeshChannel
import com.raemond.opentrailpaper.mesh.MeshChannelUrl
import java.util.Locale

/**
 * Private channels: create one, show its QR so someone can join, or scan theirs.
 *
 * A "private channel" here is Meshtastic's own mechanism — a channel with a
 * random 256-bit key that only the people who scanned the code hold. Everyone
 * else on the mesh sees a packet go by and cannot read a byte of it. A two-person
 * channel is therefore a private conversation, and a several-person one is a
 * private group; they are the same thing at different sizes.
 *
 * The QR is the standard meshtastic.org/e/# URL, so the other person does not
 * need this app — the official Meshtastic app will import it too.
 */
@Composable
fun MeshChannelsSheet(
    ble: BleManager,
    selected: Int,
    onSelect: (Int) -> Unit,
    onDismiss: () -> Unit,
) {
    var newName by remember { mutableStateOf("") }
    var sharing by remember { mutableStateOf<MeshChannel?>(null) }
    var scanning by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf<String?>(null) }
    var joined by remember { mutableStateOf<String?>(null) }
    var confirmForget by remember { mutableStateOf<MeshChannel?>(null) }
    var matchRadio by remember { mutableStateOf<RadioMatch?>(null) }

    fun create() {
        val slot = ble.firstFreeMeshChannel ?: return
        val key = MeshChannelUrl.randomKey()
        if (key.size != 32) {
            // Only reachable on a CSPRNG failure. A predictable key would look
            // private and not be, so refuse rather than proceed.
            error = "Could not generate a secure key on this device."
            return
        }
        val name = newName.trim()
        ble.setMeshPrivateChannel(slot, name, key)
        newName = ""
        onSelect(slot)
        // Optimistic: the device stages the change and reports the real entry
        // back a moment later, and waiting for that round trip to show the QR
        // made a freshly created channel look like it had gone nowhere. The
        // invite only needs the name and key, both of which we just chose.
        sharing = MeshChannel(slot, name, hash = 0, psk = key, sharesLocation = false)
    }

    fun join(name: String, psk: ByteArray) {
        val slot = ble.firstFreeMeshChannel ?: return
        ble.setMeshPrivateChannel(slot, name, psk)
        onSelect(slot)
        joined = name
    }

    fun scanned(text: String) {
        scanning = false
        val invite = MeshChannelUrl.parseInvite(text)
        val first = invite.channels.firstOrNull()
        if (first == null) {
            error = "That code is not a Meshtastic channel."
            return
        }
        if (first.psk.isEmpty()) {
            error = "That channel has no encryption key, so it would not be private."
            return
        }
        if (ble.firstFreeMeshChannel == null) {
            error = "All channel slots are in use. Forget one first."
            return
        }
        // Imported as a PRIVATE channel, never as the primary: this device keeps
        // its own primary and adds this one alongside it. What the primary
        // decided — the speed and the frequency — comes from the code's radio
        // settings instead, and only with the rider's say-so, since matching
        // them moves this device off the mesh it is on.
        val name = first.name.ifEmpty { "Shared" }
        val match = invite.radio?.let { radioMatch(ble, it) }
        if (match == null || match.changes.isEmpty()) join(name, first.psk)
        else matchRadio = match.copy(name = name, psk = first.psk)
    }

    FullScreenSheet(title = "Channels", onDismiss = onDismiss) {
        Column(
            Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(14.dp),
        ) {
            for (c in ble.meshChannels) {
                ChannelCard(
                    channel = c,
                    selected = selected == c.index,
                    onRead = { onSelect(c.index); onDismiss() },
                    onShare = { sharing = c },
                    onForget = { confirmForget = c },
                    onShareLocation = { ble.setMeshShareLocation(c.index, it) },
                )
            }

            Card {
                TrackedLabel("New private channel")
                Spacer(Modifier.size(6.dp))
                MeshTextField(newName, { newName = it }, "Name (e.g. Saturday Ride)")
                Text(
                    "Creates a channel with a random 256-bit key. Share its code " +
                        "with the people you want in it — anyone else on the mesh " +
                        "sees the traffic and cannot read it.",
                    style = barlow(13.sp),
                    color = Palette.muted,
                    modifier = Modifier.padding(top = 6.dp),
                )
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    TextButton(
                        onClick = { create() },
                        enabled = newName.isNotBlank() && ble.firstFreeMeshChannel != null,
                    ) {
                        Text(
                            "Create",
                            style = condensed(18.sp, FontWeight.SemiBold),
                            color = if (newName.isBlank() || ble.firstFreeMeshChannel == null) {
                                Palette.faint
                            } else {
                                Palette.accent
                            },
                        )
                    }
                    TextButton(onClick = { scanning = true }) {
                        Text(
                            "Scan someone's code",
                            style = condensed(18.sp, FontWeight.SemiBold),
                            color = Palette.accent,
                        )
                    }
                }
                if (ble.firstFreeMeshChannel == null) {
                    Text(
                        "All channel slots are in use. Forget one to add another.",
                        style = barlow(13.sp),
                        color = Palette.accent,
                    )
                }
            }

            Card {
                TrackedLabel("How this works")
                Spacer(Modifier.size(6.dp))
                Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
                    Text(
                        "Every channel shares one radio setting, so a private " +
                            "channel costs no range and no battery. What separates " +
                            "them is the key.",
                        style = barlow(14.sp),
                        color = Palette.muted,
                    )
                    Text(
                        "Location sharing sends your position about every 10 minutes " +
                            "while you are moving, and only when the device has a GPS " +
                            "fix. It is deliberately not continuous: a position " +
                            "broadcast is spent on everyone's airtime, since it goes " +
                            "out on the shared frequency and other nodes relay it.",
                        style = barlow(14.sp),
                        color = Palette.muted,
                    )
                    Text(
                        "Both people must also be on the same speed and frequency " +
                            "slot. A code carries this device's, and scanning one " +
                            "offers to switch to match. You are on " +
                            "${ble.meshState.channel}.",
                        style = barlow(14.sp),
                        color = Palette.muted,
                    )
                }
            }
            Spacer(Modifier.size(8.dp))
        }
    }

    sharing?.let { c -> MeshShareSheet(c, sharedRadio(ble)) { sharing = null } }

    matchRadio?.let { m ->
        AlertDialog(
            onDismissRequest = { matchRadio = null },
            title = { Text("Switch radio to match?") },
            text = {
                Text(
                    "This channel's code is set up for a different radio:\n\n" +
                        m.changes.joinToString("\n") +
                        "\n\nDevices on a different speed or slot cannot hear each " +
                        "other at all. Switching also moves you off the public mesh " +
                        "you are on now.",
                )
            },
            confirmButton = {
                TextButton(onClick = {
                    matchRadio = null
                    m.apply(ble)
                    join(m.name, m.psk)
                }) { Text("Switch and join") }
            },
            dismissButton = {
                TextButton(onClick = {
                    matchRadio = null
                    join(m.name, m.psk)
                }) { Text("Join only") }
            },
            containerColor = Palette.surface,
        )
    }

    if (scanning) {
        FullScreenSheet(
            title = "Scan a channel",
            onDismiss = { scanning = false },
            confirmLabel = "Cancel",
        ) {
            MeshScannerView(Modifier.fillMaxSize()) { scanned(it) }
        }
    }

    joined?.let { name ->
        AlertDialog(
            onDismissRequest = { joined = null },
            title = { Text("Joined") },
            text = {
                Text(
                    "You are now on $name. It may take a moment to appear while the " +
                        "device stores the key.",
                )
            },
            confirmButton = { TextButton(onClick = { joined = null }) { Text("OK") } },
            containerColor = Palette.surface,
        )
    }

    error?.let { message ->
        AlertDialog(
            onDismissRequest = { error = null },
            title = { Text("Could not join") },
            text = { Text(message) },
            confirmButton = { TextButton(onClick = { error = null }) { Text("OK") } },
            containerColor = Palette.surface,
        )
    }

    confirmForget?.let { c ->
        AlertDialog(
            onDismissRequest = { confirmForget = null },
            title = { Text("Forget this channel?") },
            text = {
                Text(
                    "The key is deleted along with the messages that arrived on it. " +
                        "You will need the code again to rejoin.",
                )
            },
            confirmButton = {
                TextButton(onClick = {
                    ble.forgetMeshChannel(c.index)
                    confirmForget = null
                }) { Text("Forget") }
            },
            dismissButton = {
                TextButton(onClick = { confirmForget = null }) { Text("Cancel") }
            },
            containerColor = Palette.surface,
        )
    }
}

@Composable
private fun ChannelCard(
    channel: MeshChannel,
    selected: Boolean,
    onRead: () -> Unit,
    onShare: () -> Unit,
    onForget: () -> Unit,
    onShareLocation: (Boolean) -> Unit,
) {
    Card {
        Row(verticalAlignment = Alignment.CenterVertically) {
            if (channel.isPrivate) {
                Icon(
                    Icons.Filled.Lock,
                    contentDescription = null,
                    tint = Palette.good,
                    modifier = Modifier.size(13.dp).padding(end = 1.dp),
                )
                Spacer(Modifier.size(5.dp))
            }
            Text(
                if (channel.isPrimary) "Public · ${channel.displayName}" else channel.displayName,
                style = TypeScale.title,
                color = Palette.ink,
                modifier = Modifier.weight(1f),
            )
            if (selected) {
                Icon(Icons.Filled.Check, contentDescription = null, tint = Palette.accent)
            }
        }
        Text(
            if (channel.isPrimary) {
                "Sets the frequency for every channel · ${channel.keyDescription}"
            } else {
                "${channel.keyDescription} · channel byte 0x" +
                    String.format(Locale.US, "%02x", channel.hash)
            },
            style = barlow(14.sp),
            color = Palette.muted,
        )
        Spacer(Modifier.size(8.dp))
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                Text("Share my location here", style = barlow(15.sp), color = Palette.ink)
                Text(
                    if (channel.isPrimary) {
                        "This channel is public — everyone in radio range could read it"
                    } else {
                        "Only people holding this channel's key can read it"
                    },
                    style = barlow(12.sp),
                    color = if (channel.isPrimary) Palette.accent else Palette.muted,
                )
            }
            EinkSwitch(
                checked = channel.sharesLocation,
                onCheckedChange = onShareLocation,
            )
        }
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            TextButton(onClick = onRead) {
                Text("Read this one", style = condensed(17.sp, FontWeight.SemiBold),
                    color = Palette.accent)
            }
            if (channel.isPrivate) {
                TextButton(onClick = onShare) {
                    Text("Share", style = condensed(17.sp, FontWeight.SemiBold),
                        color = Palette.accent)
                }
            }
            if (!channel.isPrimary) {
                TextButton(onClick = onForget) {
                    Text("Forget", style = condensed(17.sp, FontWeight.SemiBold),
                        color = Palette.accent)
                }
            }
        }
    }
}

/**
 * The QR someone else scans to join. Also shows the URL, because pasting a link
 * into a message is often easier than getting two phones in front of each other.
 */
@Composable
private fun MeshShareSheet(channel: MeshChannel, radio: MeshChannelUrl.Radio?, onDismiss: () -> Unit) {
    val context = LocalContext.current
    val clipboard = LocalClipboardManager.current
    val shareUrl = remember(channel, radio) {
        MeshChannelUrl.url(channel.name, channel.psk, radio)
    }
    val qr = remember(shareUrl) { MeshChannelUrl.qrBitmap(shareUrl) }

    FullScreenSheet(title = "Invite", onDismiss = onDismiss) {
        Column(
            Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            qr?.let { bitmap ->
                Image(
                    bitmap = bitmap.asImageBitmap(),
                    contentDescription = "Channel QR code",
                    // None, so the modules stay crisp squares rather than being
                    // smoothed into something a scanner has to work at.
                    filterQuality = FilterQuality.None,
                    contentScale = ContentScale.Fit,
                    modifier = Modifier
                        .widthIn(max = 280.dp)
                        .fillMaxWidth()
                        .aspectRatio(1f)
                        .background(Color.White)
                        .padding(16.dp),
                )
            }
            Text(channel.displayName, style = TypeScale.title, color = Palette.ink)
            radio?.let { r ->
                Text(
                    listOfNotNull(r.preset, r.region, r.slot.takeIf { it > 0 }?.let { "slot $it" })
                        .joinToString(" · "),
                    style = barlow(14.sp, FontWeight.SemiBold),
                    color = Palette.ink,
                )
            }
            Text(
                "Anyone who scans this joins the channel and can read its messages. " +
                    "It works in the official Meshtastic app too.",
                style = barlow(14.sp),
                color = Palette.muted,
                textAlign = TextAlign.Center,
                modifier = Modifier.padding(horizontal = 24.dp),
            )

            Card {
                TrackedLabel("Or send this link")
                Spacer(Modifier.size(6.dp))
                Text(shareUrl, style = barlow(12.sp), color = Palette.muted)
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    TextButton(onClick = { clipboard.setText(AnnotatedString(shareUrl)) }) {
                        Text("Copy", style = condensed(17.sp, FontWeight.SemiBold),
                            color = Palette.accent)
                    }
                }
            }
            PrimaryButton(title = "Share link") {
                val send = Intent(Intent.ACTION_SEND).apply {
                    type = "text/plain"
                    putExtra(Intent.EXTRA_TEXT, shareUrl)
                }
                context.startActivity(Intent.createChooser(send, "Share channel"))
            }
            Text(
                "Treat it like a password: anyone who gets the code can read " +
                    "everything on the channel, including messages sent before they " +
                    "joined.",
                style = barlow(13.sp),
                color = Palette.faint,
                textAlign = TextAlign.Center,
                modifier = Modifier.padding(horizontal = 20.dp),
            )
            Spacer(Modifier.size(8.dp))
        }
    }
}

/**
 * This device's radio, as an invite carries it: the preset and region by name
 * and the slot it is actually on. Null until the presets have streamed in, and
 * then the invite goes out with the channel alone, as it always used to.
 */
private fun sharedRadio(ble: BleManager): MeshChannelUrl.Radio? {
    val st = ble.meshState
    val preset = ble.meshPresets.firstOrNull { it.index == st.presetIndex } ?: return null
    val region = st.regionIndex?.let { i -> ble.meshRegions.firstOrNull { it.index == i } }
    return MeshChannelUrl.Radio(preset.name, region?.name, st.activeSlot ?: 0)
}

/** What scanning a code would change on this device's radio, and how to do it. */
private data class RadioMatch(
    val changes: List<String>,
    val preset: Int?,
    val region: Int?,
    val slot: Int?,
    val name: String = "",
    val psk: ByteArray = ByteArray(0),
) {
    fun apply(ble: BleManager) {
        // Preset and region first: they decide the band the slot is cut from.
        preset?.let(ble::setMeshPreset)
        region?.let(ble::setMeshRegion)
        slot?.let(ble::setMeshFreqSlot)
    }
}

private fun radioMatch(ble: BleManager, want: MeshChannelUrl.Radio): RadioMatch {
    val st = ble.meshState
    val changes = ArrayList<String>()
    val curPreset = ble.meshPresets.firstOrNull { it.index == st.presetIndex }
    val curRegion = st.regionIndex?.let { i -> ble.meshRegions.firstOrNull { it.index == i } }

    var preset: Int? = null
    var newPreset = curPreset
    if (want.preset != null && curPreset != null && !want.preset.equals(curPreset.name, true)) {
        val p = ble.meshPresets.firstOrNull { it.name.equals(want.preset, true) }
        if (p == null) {
            changes += "Speed: ${want.preset} (this device can't use it — staying on ${curPreset.name})"
        } else {
            changes += "Speed: ${curPreset.name} → ${p.name}"
            preset = p.index
            newPreset = p
        }
    }

    var region: Int? = null
    var newRegion = curRegion
    if (want.region != null && curRegion != null && !want.region.equals(curRegion.name, true)) {
        val r = ble.meshRegions.firstOrNull { it.name.equals(want.region, true) }
        if (r == null) {
            changes += "Region: ${want.region} (this device can't use it — staying on ${curRegion.name})"
        } else {
            changes += "Region: ${curRegion.name} → ${r.name}"
            region = r.index
            newRegion = r
        }
    }

    // The slot this device will be on once preset and region match, left
    // automatic where that already is the code's slot, so it keeps following
    // the preset as a stock node does.
    var slot: Int? = null
    if (want.slot > 0 && newPreset != null && newRegion != null) {
        val primary = if (st.channelFollowsPreset) newPreset.name else st.channel
        val auto = MeshChannelUrl.automaticSlot(
            primary, newPreset.bandwidthKhz,
            newRegion.startHz / 1e6, newRegion.endHz / 1e6, newRegion.spacingHz / 1e6,
        )
        val pinned = st.slotOverride ?: 0
        val target = if (want.slot == auto) 0 else want.slot
        // An older firmware can't pin a slot: it gets the preset's automatic one.
        if (st.slotOverride != null && target != pinned) slot = target
        val now = st.activeSlot ?: (if (pinned == 0) auto else pinned)
        if (now != want.slot) changes += "Slot: $now → ${want.slot}"
    }
    return RadioMatch(changes, preset, region, slot)
}
