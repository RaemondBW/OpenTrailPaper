package com.raemond.opentrailpaper.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
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
            Text("Upload saved rides directly from their ride details. Uploads only happen when you tap Upload.",
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
        Text(service.name, style = TypeScale.title, color = Palette.ink)
        Text(if (saved) "API key saved on this phone" else "No API key saved", style = TypeScale.body)
        TextButton(onClick = { uri.openUri(service.credentialHelpURL) }) {
            Text("Get an API key in ${service.name} Settings → Developer Settings")
        }
        OutlinedTextField(value = credential, onValueChange = { credential = it },
            label = { Text(if (saved) "Replace API key" else "API key") },
            visualTransformation = PasswordVisualTransformation(), singleLine = true, enabled = !busy)
        TextButton(enabled = !busy && credential.isNotBlank(), onClick = { save(credential.trim()) }) {
            Text("Save API key")
        }
        if (saved) TextButton(enabled = !busy, onClick = { save("") }) { Text("Disconnect") }
        if (message.isNotEmpty()) Text(message, style = TypeScale.body)
    }
}

@Composable
fun RideUploadButtons(file: File) {
    val uploads = RideUploads.get(LocalContext.current)
    var showServices by remember { mutableStateOf(false) }
    Card {
        TrackedLabel("Upload ride")
        uploads.services.forEach { service ->
            val state = uploads.status[uploads.actionID(service.id, file)]
            TextButton(enabled = state?.busy != true, onClick = { uploads.upload(service, file) }) {
                if (state?.busy == true) CircularProgressIndicator(Modifier.size(20.dp))
                Text(if (state?.busy == true) "Uploading…" else "Upload to ${service.name}")
            }
            state?.message?.takeIf { it.isNotEmpty() }?.let { Text(it, style = TypeScale.body) }
        }
        TextButton(onClick = { showServices = true }) { Text("Upload services") }
    }
    if (showServices) UploadServicesSheet { showServices = false }
}
