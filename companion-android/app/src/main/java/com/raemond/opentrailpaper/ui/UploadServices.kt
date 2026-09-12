package com.raemond.opentrailpaper.ui

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Share
import androidx.compose.material.icons.filled.Link
import androidx.compose.material.icons.filled.Lock
import androidx.compose.material.icons.filled.ChevronRight
import androidx.compose.material.icons.filled.ArrowUpward
import androidx.compose.material.icons.filled.OpenInNew
import androidx.compose.material3.Icon
import androidx.compose.material3.OutlinedTextFieldDefaults
import androidx.compose.material3.HorizontalDivider
import androidx.compose.ui.Alignment
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalUriHandler
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.unit.dp
import com.raemond.opentrailpaper.data.RideUploads
import com.raemond.opentrailpaper.data.RideUploadService
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File

@Composable
fun UploadServicesSheet(onDismiss: () -> Unit) {
    val uploads = RideUploads.get(LocalContext.current)
    FullScreenSheet(title = "Upload services", onDismiss = onDismiss) {
        Column(Modifier.verticalScroll(rememberScrollState()).padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp)) {
            Text("Connect your accounts, then choose Share on a saved ride.",
                style = TypeScale.body, color = Palette.muted)
            uploads.services.forEach { service -> UploadServiceSettings(uploads, service) }
        }
    }
}

@Composable
private fun UploadServiceSettings(uploads: RideUploads, service: RideUploadService) {
    val uri = LocalUriHandler.current
    val scope = rememberCoroutineScope()
    var credential by remember { mutableStateOf("") }
    var saved by remember { mutableStateOf(false) }
    var busy by remember { mutableStateOf(false) }
    var message by remember { mutableStateOf("") }
    LaunchedEffect(service.id) {
        try { saved = withContext(Dispatchers.IO) { uploads.credentials.read(service.id).isNotEmpty() } }
        catch (e: Exception) { message = e.message ?: "Could not read the API key." }
    }
    fun save(value: String) {
        busy = true
        scope.launch {
            try {
                withContext(Dispatchers.IO) { uploads.credentials.save(service.id, value) }
                uploads.status.entries.removeAll { it.key.startsWith(service.id + ":") && !it.value.busy }
                credential = ""; saved = value.isNotEmpty()
                message = if (saved) "API key saved securely." else "API key removed."
            } catch (e: Exception) { message = e.message ?: "Could not save the API key." }
            finally { busy = false }
        }
    }
    Card {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            Icon(Icons.Filled.Link, contentDescription = null, tint = Palette.accent)
            Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                Text(service.name, style = TypeScale.title, color = Palette.ink)
                Text(if (saved) "Connected" else "Not connected", style = TypeScale.bodyStrong,
                    color = if (saved) Palette.good else Palette.muted)
            }
        }
        HorizontalDivider(color = Palette.hairline)
        TrackedLabel(if (saved) "Replace API key" else "API key")
        OutlinedTextField(value = credential, onValueChange = { credential = it },
            modifier = Modifier.fillMaxWidth(),
            placeholder = { Text("Paste your API key", style = TypeScale.body) },
            leadingIcon = { Icon(Icons.Filled.Lock, contentDescription = null) },
            textStyle = TypeScale.body,
            shape = RoundedCornerShape(14.dp),
            colors = OutlinedTextFieldDefaults.colors(
                focusedTextColor = Palette.ink, unfocusedTextColor = Palette.ink,
                focusedContainerColor = Palette.paper, unfocusedContainerColor = Palette.paper,
                focusedBorderColor = Palette.accent, unfocusedBorderColor = Palette.hairline,
                cursorColor = Palette.accent, focusedLeadingIconColor = Palette.muted,
                unfocusedLeadingIconColor = Palette.muted,
                focusedPlaceholderColor = Palette.muted, unfocusedPlaceholderColor = Palette.muted),
            visualTransformation = PasswordVisualTransformation(), singleLine = true, enabled = !busy)
        Text("Stored securely on this phone.", style = TypeScale.body, color = Palette.muted)
        PrimaryButton(if (saved) "Update connection" else "Connect", icon = Icons.Filled.Link,
            enabled = !busy && credential.isNotBlank(), onClick = { save(credential.trim()) })
        if (saved) SecondaryButton("Disconnect", enabled = !busy, onClick = { save("") })
        TextButton(modifier = Modifier.fillMaxWidth(), onClick = { uri.openUri(service.credentialHelpURL) }) {
            Text("Get an API key", style = TypeScale.bodyStrong, color = Palette.accent)
            Spacer(Modifier.size(8.dp))
            Icon(Icons.Filled.OpenInNew, contentDescription = null, tint = Palette.accent, modifier = Modifier.size(16.dp))
        }
        if (message.isNotEmpty()) Text(message, style = TypeScale.body, color = Palette.ink)
    }
}

@Composable
fun RideShareButton(file: File) {
    var showShare by remember { mutableStateOf(false) }
    PrimaryButton("Share", icon = Icons.Filled.Share) { showShare = true }
    if (showShare) RideShareSheet(file) { showShare = false }
}

@Composable
private fun RideShareSheet(file: File, onDismiss: () -> Unit) {
    val uploads = RideUploads.get(LocalContext.current)
    var showServices by remember { mutableStateOf(false) }
    var connectedIDs by remember { mutableStateOf(setOf<String>()) }
    var connectionErrors by remember { mutableStateOf(listOf<String>()) }
    var loading by remember { mutableStateOf(true) }
    // Refresh after managing connections, including newly connected services.
    LaunchedEffect(showServices) {
        if (showServices) return@LaunchedEffect
        loading = true
        val (connected, errors) = withContext(Dispatchers.IO) {
            val ids = mutableSetOf<String>()
            val failures = mutableListOf<String>()
            uploads.services.forEach { service ->
                try {
                    if (uploads.credentials.read(service.id).isNotEmpty()) ids.add(service.id)
                } catch (e: Exception) {
                    failures.add("${service.name}: ${e.message ?: "Could not read the saved connection."}")
                }
            }
            ids.toSet() to failures.toList()
        }
        connectedIDs = connected
        connectionErrors = errors
        loading = false
    }
    FullScreenSheet(title = "Share ride", onDismiss = onDismiss) {
        Column(Modifier.verticalScroll(rememberScrollState()).padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp)) {
            Text("Choose a service to upload this ride to.", style = TypeScale.body, color = Palette.muted)
            if (loading) {
                CircularProgressIndicator()
            } else {
                if (connectedIDs.isEmpty()) {
                    Card {
                        Text("No connected services", style = TypeScale.title, color = Palette.ink)
                        Text("Connect a service to share your ride.", style = TypeScale.body, color = Palette.muted)
                    }
                }
                uploads.services.filter { it.id in connectedIDs }.forEach { service ->
                    val state = uploads.status[uploads.actionID(service.id, file)]
                    Card(Modifier.clickable(enabled = state?.busy != true) { uploads.upload(service, file) }) {
                        Row(verticalAlignment = Alignment.CenterVertically,
                            horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                            Icon(Icons.Filled.ArrowUpward, contentDescription = null, tint = Palette.accent)
                            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                                Text(service.name, style = TypeScale.title, color = Palette.ink)
                                Text(if (state?.busy == true) "Uploading ride…" else "Upload this ride",
                                    style = TypeScale.body, color = Palette.muted)
                            }
                            if (state?.busy == true) CircularProgressIndicator(Modifier.size(20.dp), color = Palette.accent)
                            else Icon(Icons.Filled.ChevronRight, contentDescription = null, tint = Palette.muted)
                        }
                    }
                    state?.message?.takeIf { it.isNotEmpty() }?.let { Text(it, style = TypeScale.body, color = Palette.ink) }
                }
                connectionErrors.forEach { Text(it, style = TypeScale.body) }
            }
            UploadNavigationCard("Manage services", "Connect or update your accounts") { showServices = true }
        }
    }
    if (showServices) UploadServicesSheet { showServices = false }
}

@Composable
fun UploadNavigationCard(title: String, summary: String, onClick: () -> Unit) {
    Card(Modifier.clickable(onClick = onClick)) {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            Icon(Icons.Filled.Link, contentDescription = null, tint = Palette.accent)
            Column(Modifier.weight(1f)) {
                TrackedLabel(title)
                Text(summary, style = TypeScale.bodyStrong, color = Palette.ink)
            }
            Icon(Icons.Filled.ChevronRight, contentDescription = null, tint = Palette.muted)
        }
    }
}
