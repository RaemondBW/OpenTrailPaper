package com.raemond.opentrailpaper.ble

import android.app.Application
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.media.AudioManager
import android.media.MediaMetadata
import android.media.session.MediaController
import android.media.session.MediaSessionManager
import android.media.session.PlaybackState
import android.os.Handler
import android.os.Looper
import android.provider.Settings
import android.service.notification.NotificationListenerService
import androidx.core.app.NotificationManagerCompat

// Feeds the device's MUSIC page and answers its transport buttons.
//
// Built on MediaSessionManager, which sees WHATEVER app is playing — Spotify,
// YouTube Music, podcasts — unlike the iOS companion, whose only public hook
// (MPMusicPlayerController) reaches Apple Music alone. The price of that reach
// is a permission: getActiveSessions() only answers once the rider has granted
// this app notification access, which is why MediaListener below exists — the
// grant is bound to a NotificationListenerService, even one with an empty body.
//
// Started only while the device's dashboard config contains a music page — no
// page, no permission involved and no observers.
class MediaRemote(private val app: Application, private val ble: BleManager) {

    private val main = Handler(Looper.getMainLooper())
    private val sessions get() =
        app.getSystemService(Context.MEDIA_SESSION_SERVICE) as MediaSessionManager
    private val listener = ComponentName(app, MediaListener::class.java)

    private var running = false
    private var controller: MediaController? = null
    // The artwork last pushed, so a play/pause blip doesn't re-send ~90 KB.
    private var sentArtKey: String? = null

    /** Has the rider granted notification access (the getActiveSessions gate)? */
    val accessGranted: Boolean
        get() = NotificationManagerCompat.getEnabledListenerPackages(app)
            .contains(app.packageName)

    /** The system screen where that grant lives, for the editor's prompt. */
    fun accessSettingsIntent(): Intent =
        Intent(Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS)
            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)

    fun start() {
        if (running) return
        running = true
        main.post { beginObserving() }
    }

    fun stop() {
        if (!running) return
        running = false
        main.post {
            detachController()
            runCatching { sessions.removeOnActiveSessionsChangedListener(sessionsChanged) }
            sentArtKey = null
            ble.sendMediaClear()
        }
    }

    /** Re-check after the rider returns from the settings screen. */
    fun refresh() {
        if (!running) return
        main.post { beginObserving() }
    }

    private val sessionsChanged =
        MediaSessionManager.OnActiveSessionsChangedListener { list ->
            adoptController(list ?: emptyList())
        }

    private fun beginObserving() {
        if (!running || !accessGranted) return
        runCatching {
            sessions.addOnActiveSessionsChangedListener(sessionsChanged, listener)
            adoptController(sessions.getActiveSessions(listener))
        }
    }

    /**
     * Follow the session that actually has something to show. The list is
     * priority-ordered by the system (the active/most-recent session first),
     * so the first one carrying metadata is the one the rider thinks of as
     * "what's playing".
     */
    private fun adoptController(list: List<MediaController>) {
        val next = list.firstOrNull { it.metadata != null } ?: list.firstOrNull()
        if (next?.sessionToken == controller?.sessionToken) { pushNow(); return }
        detachController()
        controller = next
        next?.registerCallback(controllerCallback, main)
        pushNow()
    }

    private fun detachController() {
        controller?.unregisterCallback(controllerCallback)
        controller = null
    }

    private val controllerCallback = object : MediaController.Callback() {
        override fun onMetadataChanged(metadata: MediaMetadata?) = pushNow()
        override fun onPlaybackStateChanged(state: PlaybackState?) = pushNow()
        override fun onSessionDestroyed() {
            detachController()
            main.post { beginObserving() }   // fall through to the next session
        }
    }

    /** Transport command from the device (media_state.h MediaCmd). */
    fun handleCommand(cmd: Int) {
        main.post {
            val c = controller
            when (cmd) {
                1 -> if (c?.playbackState?.state == PlaybackState.STATE_PLAYING) {
                    c.transportControls.pause()
                } else {
                    c?.transportControls?.play()
                }
                2 -> c?.transportControls?.skipToNext()
                3 -> c?.transportControls?.skipToPrevious()
                // System volume, so it works whatever app is playing.
                4 -> nudgeVolume(AudioManager.ADJUST_RAISE)
                5 -> nudgeVolume(AudioManager.ADJUST_LOWER)
            }
            // A state callback follows, but not always promptly — reflect the
            // new state now so the panel settles instead of flip-flopping.
            pushNow()
        }
    }

    private fun nudgeVolume(direction: Int) {
        val audio = app.getSystemService(Context.AUDIO_SERVICE) as AudioManager
        audio.adjustStreamVolume(AudioManager.STREAM_MUSIC, direction, 0)
    }

    private fun pushNow() {
        if (!running) return
        val c = controller
        val meta = c?.metadata
        if (c == null || meta == null) {
            sentArtKey = null
            ble.sendMediaClear()
            return
        }
        val state = c.playbackState
        val playing = state?.state == PlaybackState.STATE_PLAYING
        val pos = ((state?.position ?: 0L) / 1000L).coerceIn(0, 65535).toInt()
        val dur = (meta.getLong(MediaMetadata.METADATA_KEY_DURATION) / 1000L)
            .coerceIn(0, 65535).toInt()
        val title = meta.getString(MediaMetadata.METADATA_KEY_TITLE) ?: ""
        val artist = meta.getString(MediaMetadata.METADATA_KEY_ARTIST) ?: ""
        val album = meta.getString(MediaMetadata.METADATA_KEY_ALBUM) ?: ""
        ble.sendMediaMeta(playing, pos, dur, title, artist, album)

        // Only a track whose art actually went out counts as sent. Spotify
        // publishes a new track's metadata dozens of times before the bitmap
        // lands; marking it sent on the first, art-less update meant the art
        // that followed was never pushed at all.
        val artKey = "$title\u0000$artist\u0000$album"
        if (sentArtKey != artKey) {
            val art = artOf(meta) ?: return
            sentArtKey = artKey
            // 300 px fits the device's 324 px frame; grayscale 8-bit is what
            // its ditherer wants (BleManager may dither it here instead). No
            // art on the item -> nothing sent; the device draws its vinyl
            // placeholder.
            val side = 300
            ble.sendMediaArt(grayscaleBytes(art, side), side, side)
        }
    }

    private fun artOf(meta: MediaMetadata): Bitmap? =
        meta.getBitmap(MediaMetadata.METADATA_KEY_ALBUM_ART)
            ?: meta.getBitmap(MediaMetadata.METADATA_KEY_ART)

    private fun grayscaleBytes(src: Bitmap, side: Int): ByteArray {
        // White ground first: art with alpha must land on paper, not black.
        val scaled = Bitmap.createBitmap(side, side, Bitmap.Config.ARGB_8888)
        Canvas(scaled).apply {
            drawColor(Color.WHITE)
            drawBitmap(
                src, null,
                android.graphics.Rect(0, 0, side, side),
                android.graphics.Paint(android.graphics.Paint.FILTER_BITMAP_FLAG),
            )
        }
        val px = IntArray(side * side)
        scaled.getPixels(px, 0, side, 0, 0, side, side)
        scaled.recycle()
        return ByteArray(px.size) { i ->
            val p = px[i]
            // Rec. 601 luma — the same weights the device's own tools use.
            val y = (Color.red(p) * 299 + Color.green(p) * 587 + Color.blue(p) * 114) / 1000
            LIFT[y]
        }
    }

    private companion object {
        /**
         * Tone curve applied before the art is sent (and so before either
         * ditherer sees it). The panel's greys are darker than a screen's and
         * most covers sit in the shadows, so straight luma came out murky.
         * Gamma 0.75 lifts the midtones (128 -> 152) and keeps pure black and
         * white where they are. One table, so the device's own dither of 8-bit
         * art and ArtDither's tone art stay identical.
         */
        private const val GAMMA = 0.75
        val LIFT = ByteArray(256) { i ->
            Math.round(255.0 * Math.pow(i / 255.0, GAMMA)).toInt().toByte()
        }
    }
}

/**
 * Exists so Android will let the app see media sessions: getActiveSessions()
 * is gated on the rider granting notification access, and the grant is bound
 * to a NotificationListenerService — even one that never reads a notification.
 */
class MediaListener : NotificationListenerService()
