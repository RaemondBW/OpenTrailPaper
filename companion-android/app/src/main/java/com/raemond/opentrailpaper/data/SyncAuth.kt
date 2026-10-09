package com.raemond.opentrailpaper.data

import com.google.gson.JsonParser
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import java.io.IOException

/*
 * The token-keeping and error-wording half of SyncAccounts, kept free of
 * android.* so it runs in JVM unit tests (SyncAuthTest).
 */

/** One account's tokens. [expiresAt] is unix seconds; null = does not expire. */
data class SyncTokens(
    val accessToken: String,
    val refreshToken: String?,
    val expiresAt: Long?,
    val athleteName: String?,
) {
    fun expiresWithin(marginS: Long, nowS: Long) = expiresAt != null && nowS >= expiresAt - marginS
}

open class SyncException(message: String) : IOException(message)

/** A non-2xx answer. [message] is already worded for the user (see [SyncErrors]). */
class SyncHttpException(val code: Int, message: String, val reason: String? = null) : SyncException(message)

/** The provider no longer accepts the saved sign-in: only connecting again helps. */
class SyncSignedOutException(message: String) : SyncException(message)

/**
 * Hands out a bearer token that works, for one account.
 *
 * Strava access tokens last six hours, and every refresh may hand back a new
 * refresh token that replaces the old one, so: refresh a few minutes before
 * expiry, save whatever comes back before using it, never run two refreshes at
 * once (the second would spend a refresh token the first just replaced), and if
 * the provider still says 401, refresh once and try again.
 */
class TokenKeeper(
    private val title: String,
    private val current: () -> SyncTokens?,
    private val save: (SyncTokens) -> Unit,
    /** Trades a refresh token for new tokens (the service's /refresh); null = this provider cannot. */
    private val refresher: (suspend (refreshToken: String) -> SyncTokens)?,
    private val nowS: () -> Long = { System.currentTimeMillis() / 1000 },
    private val marginS: Long = REFRESH_MARGIN_S,
) {
    private val mutex = Mutex()

    /** A token good for at least [marginS] seconds, refreshing first if need be. */
    suspend fun accessToken(): String = mutex.withLock {
        val t = current() ?: throw SyncException("Not connected to $title.")
        if (canRefresh(t) && t.expiresWithin(marginS, nowS())) refreshLocked(t).accessToken else t.accessToken
    }

    /**
     * Runs [call] with a token. If the provider answers 401 (a token revoked or
     * expired early, or a clock that is off), refreshes once and retries; a
     * second 401 means the sign-in is gone.
     */
    suspend fun <R> authorized(call: suspend (token: String) -> R): R {
        val first = accessToken()
        try {
            return call(first)
        } catch (e: SyncHttpException) {
            if (e.code != 401) throw e
        }
        val second = mutex.withLock {
            val t = current() ?: throw SyncException("Not connected to $title.")
            when {
                t.accessToken != first -> t.accessToken          // another call refreshed meanwhile
                canRefresh(t) -> refreshLocked(t).accessToken
                else -> throw signedOut()
            }
        }
        try {
            return call(second)
        } catch (e: SyncHttpException) {
            throw if (e.code == 401) signedOut() else e
        }
    }

    private fun canRefresh(t: SyncTokens) = refresher != null && t.refreshToken != null

    private suspend fun refreshLocked(t: SyncTokens): SyncTokens {
        val fresh = try {
            refresher!!(t.refreshToken!!)
        } catch (e: SyncHttpException) {
            // The service's 401 is either "the provider refused the refresh
            // token" (reason refresh_rejected) or an App Check failure; only the
            // first is fixed by connecting again.
            throw if (e.code == 401 && e.reason == "refresh_rejected") signedOut() else e
        }
        val merged = t.copy(
            accessToken = fresh.accessToken,
            // Rotated: the old one is dead from now on, so this must be saved.
            refreshToken = fresh.refreshToken ?: t.refreshToken,
            expiresAt = fresh.expiresAt,
        )
        save(merged)
        return merged
    }

    private fun signedOut() =
        SyncSignedOutException("$title no longer accepts this phone's sign-in. Disconnect and connect $title again.")

    companion object {
        const val REFRESH_MARGIN_S = 300L
    }
}

/** Words a non-2xx answer for the user, without dumping bodies at them. */
object SyncErrors {
    /**
     * @param who "Strava", "Intervals.icu", ... or [SERVICE] for our own sync service,
     *   whose `{error}` texts are already written for people and shown as they are.
     */
    fun describe(code: Int, body: String, who: String): String {
        val detail = detail(body)
        if (who == SERVICE && detail != null) {
            return if (code == 401 && detail.startsWith("app check", ignoreCase = true)) {
                "The sync service could not verify this app ($detail)."
            } else detail
        }
        val from = if (who == SERVICE) "The sync service" else who
        return when {
            code == 401 -> "$from refused the sign-in (HTTP 401)."
            code == 429 -> "$from is limiting requests right now. Try again in 15 minutes."
            code in 502..504 || code in 520..530 ->
                "$from could not be reached (HTTP $code). Try again in a few minutes."
            code >= 500 -> "$from had a problem (HTTP $code). Try again later."
            detail != null -> "$from: $detail (HTTP $code)"
            else -> "$from refused the request (HTTP $code)."
        }
    }

    const val SERVICE = "service"

    /** The human part of an error body, or null: never HTML, never a CDN's error document. */
    internal fun detail(body: String): String? {
        val trimmed = body.trim()
        if (!trimmed.startsWith("{")) return null
        val obj = runCatching { JsonParser.parseString(trimmed).asJsonObject }.getOrNull() ?: return null
        // A problem+json "type" pointing at a CDN's docs (Cloudflare answers
        // with one when the origin fails) says nothing about what went wrong.
        val type = obj.get("type")?.takeIf { it.isJsonPrimitive }?.asString
        if (type != null && type.contains("cloudflare", ignoreCase = true)) return null
        for (k in listOf("error", "message", "detail", "title")) {
            val v = obj.get(k)?.takeIf { it.isJsonPrimitive }?.asString?.trim().orEmpty()
            if (v.isNotEmpty() && !v.startsWith("http")) return v.lineSequence().first().take(160)
        }
        return null
    }
}
