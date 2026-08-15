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
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Check
import androidx.compose.material.icons.filled.Lock
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Icon
import androidx.compose.material3.Switch
import androidx.compose.material3.SwitchDefaults
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

    fun scanned(text: String) {
        scanning = false
        val first = MeshChannelUrl.parse(text).firstOrNull()
        if (first == null) {
            error = "That code is not a Meshtastic channel."
            return
        }
        if (first.psk.isEmpty()) {
            error = "That channel has no encryption key, so it would not be private."
            return
        }
        val slot = ble.firstFreeMeshChannel
        if (slot == null) {
            error = "All channel slots are in use. Forget one first."
            return
        }
        // Imported as a PRIVATE channel, never as the primary: the primary sets
        // the frequency, and taking someone else's would move this device off the
        // mesh it is on. A shared channel works because both ends keep their own
        // primary and add this one alongside it.
        val name = first.name.ifEmpty { "Shared" }
        ble.setMeshPrivateChannel(slot, name, first.psk)
        onSelect(slot)
        joined = name
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
                        "Both people must also be on the same public channel — that " +
                            "is what sets the frequency you are both listening on. If " +
                            "a code does not work, check you are both on " +
                            "${ble.meshState.channel}.",
                        style = barlow(14.sp),
                        color = Palette.muted,
                    )
                }
            }
            Spacer(Modifier.size(8.dp))
        }
    }

    sharing?.let { c -> MeshShareSheet(c) { sharing = null } }

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
            Switch(
                checked = channel.sharesLocation,
                onCheckedChange = onShareLocation,
                colors = SwitchDefaults.colors(
                    checkedThumbColor = Palette.accentInk,
                    checkedTrackColor = Palette.accent,
                ),
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
private fun MeshShareSheet(channel: MeshChannel, onDismiss: () -> Unit) {
    val context = LocalContext.current
    val clipboard = LocalClipboardManager.current
    val shareUrl = remember(channel) {
        MeshChannelUrl.url(channel.name, channel.psk)
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
                        .background(Color.White, RoundedCornerShape(18.dp))
                        .padding(16.dp),
                )
            }
            Text(channel.displayName, style = TypeScale.title, color = Palette.ink)
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
