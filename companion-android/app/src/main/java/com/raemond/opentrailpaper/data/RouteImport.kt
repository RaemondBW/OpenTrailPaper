package com.raemond.opentrailpaper.data

import android.content.Context
import android.net.Uri
import android.provider.OpenableColumns
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import com.raemond.opentrailpaper.routing.Routing
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
 *
 * It owns the route until the rider clears it, rather than handing it to the
 * first screen that draws it: a tab tap destroys everything the Route screen
 * remembers, and a search can be retyped where an import cannot be re-derived.
 * The cues fetched for it are parked here for the same reason.
 */
object RouteImport {

    /** Anything past this isn't a route file, and we won't read it into memory. */
    private const val MAX_BYTES = 32L * 1024 * 1024

    var pending by mutableStateOf<ImportedRoute?>(null)
        private set

    /** Turn cues the rider already paid an OSRM request for, if they asked. */
    var cues by mutableStateOf<Routing.CueSet?>(null)
        private set

    /**
     * True until the Route screen has drawn [pending] once.
     *
     * Lets the screen tell an import arriving from one being redrawn after a
     * tab round-trip: only an arrival invalidates what was last sent to the
     * device.
     */
    var fresh by mutableStateOf(false)
        private set

    var error by mutableStateOf<String?>(null)
        private set

    /**
     * Bumped on every [offer], and what the Route screen keys on.
     *
     * [pending] cannot do that job: ImportedRoute is a data class, so
     * re-importing the same file offers an equal value, the screen's effect
     * never restarts, and [fresh] is left stuck true — which makes the next
     * redraw look like an arrival and wrongly clears the "sent to device"
     * confirmation.
     */
    var arrival by mutableStateOf(0)
        private set

    fun offer(route: ImportedRoute) {
        error = null
        pending = route
        cues = null
        fresh = true
        arrival++
    }

    /**
     * Report a file we could not read, without touching [pending].
     *
     * Clearing here would orphan a route the rider is looking at: the screen
     * keeps drawing it, this object no longer owns it, and the next tab tap
     * loses it with no explanation. A bad second file is not a reason to throw
     * away a good first one.
     */
    fun fail(message: String) {
        error = message
    }

    /** The Route screen has drawn this import; anything after is a redraw. */
    fun markDrawn() { fresh = false }

    fun noteCues(set: Routing.CueSet) { cues = set }

    /** The rider dismissed the route — the only way an import leaves. */
    fun clear() {
        pending = null
        cues = null
        fresh = false
    }

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
