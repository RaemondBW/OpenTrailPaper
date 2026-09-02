package com.raemond.opentrailpaper.data

import android.content.Context
import android.net.Uri
import android.provider.OpenableColumns
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

/**
 * A route waiting to be previewed.
 *
 * An import can arrive before the screen that shows it exists — an
 * ACTION_VIEW from Files lands in the Activity while the rider is on the Ride
 * tab — so the result has to sit somewhere above both. An `object` rather than
 * something hung off BleManager: a file import is not Bluetooth, and that class
 * is past 2,000 lines already. Same idiom as [Prefs] and EInkTileStore.
 */
object RouteImport {

    /** Anything past this isn't a route file, and we won't read it into memory. */
    private const val MAX_BYTES = 32L * 1024 * 1024

    var pending by mutableStateOf<ImportedRoute?>(null)
        private set

    var error by mutableStateOf<String?>(null)
        private set

    fun offer(route: ImportedRoute) {
        error = null
        pending = route
    }

    fun fail(message: String) {
        pending = null
        error = message
    }

    /** Taken by the Route screen once it has drawn it. */
    fun consume(): ImportedRoute? = pending?.also { pending = null }

    fun clearError() { error = null }

    /**
     * Read and parse a GPX the rider picked or opened.
     *
     * The file is recognised by its contents, never by the MIME type the
     * provider claims — GPX arrives as `application/gpx+xml`,
     * `application/octet-stream`, `text/xml` or nothing at all depending on
     * where it came from.
     */
    suspend fun read(context: Context, uri: Uri) = withContext(Dispatchers.IO) {
        val resolver = context.contentResolver
        val display = runCatching {
            resolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)
                ?.use { c -> if (c.moveToFirst()) c.getString(0) else null }
        }.getOrNull()
        val fallback = display?.substringBeforeLast('.')?.takeIf { it.isNotBlank() } ?: "Imported route"

        val size = runCatching {
            resolver.openAssetFileDescriptor(uri, "r")?.use { it.length }
        }.getOrNull()
        if (size != null && size > MAX_BYTES) {
            fail("That file is too big to be a route")
            return@withContext
        }

        val result = runCatching {
            resolver.openInputStream(uri)?.use { GpxImporter.parse(it, fallback) }
        }.getOrNull()

        when (result) {
            is ImportResult.Ok -> offer(result.route)
            ImportResult.NoPoints -> fail("That file has no route in it")
            ImportResult.NotGpx -> fail("That file isn't a GPX route")
            null -> fail("Couldn't open that file")
        }
    }
}
