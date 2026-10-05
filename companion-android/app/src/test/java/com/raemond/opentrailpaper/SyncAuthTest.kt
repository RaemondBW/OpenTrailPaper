package com.raemond.opentrailpaper

import com.raemond.opentrailpaper.data.SyncErrors
import com.raemond.opentrailpaper.data.SyncHttpException
import com.raemond.opentrailpaper.data.SyncSignedOutException
import com.raemond.opentrailpaper.data.SyncTokens
import com.raemond.opentrailpaper.data.TokenKeeper
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitAll
import kotlinx.coroutines.delay
import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test

/**
 * The account-token rules: Strava access tokens last 6 h, a refresh may rotate
 * the refresh token, and the rotated one is the only one that works afterwards.
 */
class SyncAuthTest {

    private val now = 1_800_000_000L

    /** A fake account store and a fake Strava-through-the-service refresh. */
    private inner class Fixture(start: SyncTokens?, var rejectRefresh: Boolean = false) {
        var stored: SyncTokens? = start
        val refreshedWith = mutableListOf<String>()
        var n = 0
        val keeper = TokenKeeper(
            title = "Strava",
            current = { stored },
            save = { stored = it },
            refresher = { rt ->
                refreshedWith += rt
                delay(10)   // let a concurrent caller pile up behind the lock
                if (rejectRefresh) throw SyncHttpException(401, "rejected", "refresh_rejected")
                n++
                SyncTokens("a$n", "r$n", now + 21_600, null)
            },
            nowS = { now },
        )
    }

    @Test
    fun `a token far from expiry is used as is`() = runBlocking {
        val f = Fixture(SyncTokens("a0", "r0", now + 3_600, "Ada"))
        assertEquals("a0", f.keeper.accessToken())
        assertTrue(f.refreshedWith.isEmpty())
    }

    @Test
    fun `a token within five minutes of expiry is refreshed and the rotated refresh token saved`() = runBlocking {
        val f = Fixture(SyncTokens("a0", "r0", now + 200, "Ada"))
        assertEquals("a1", f.keeper.accessToken())
        assertEquals(listOf("r0"), f.refreshedWith)
        assertEquals(SyncTokens("a1", "r1", now + 21_600, "Ada"), f.stored)
        // The next refresh must spend the rotated token, not the original.
        f.stored = f.stored!!.copy(expiresAt = now - 1)
        f.keeper.accessToken()
        assertEquals(listOf("r0", "r1"), f.refreshedWith)
    }

    @Test
    fun `expiry is in seconds, not milliseconds`() {
        val t = SyncTokens("a", "r", now + 299, null)
        assertTrue(t.expiresWithin(300, now))
        assertFalse(t.copy(expiresAt = now + 301).expiresWithin(300, now))
        assertFalse(t.copy(expiresAt = null).expiresWithin(300, now))
    }

    @Test
    fun `two uploads at once spend the refresh token only once`() = runBlocking {
        val f = Fixture(SyncTokens("a0", "r0", now - 10, null))
        val got = (1..3).map { async { f.keeper.accessToken() } }.awaitAll()
        assertEquals(listOf("a1", "a1", "a1"), got)
        assertEquals(listOf("r0"), f.refreshedWith)
    }

    @Test
    fun `a 401 refreshes once and retries with the new token`() = runBlocking {
        val f = Fixture(SyncTokens("a0", "r0", now + 3_600, null))   // looks valid, but was revoked
        val used = mutableListOf<String>()
        val out = f.keeper.authorized { token ->
            used += token
            if (token == "a0") throw SyncHttpException(401, "nope")
            "ok"
        }
        assertEquals("ok", out)
        assertEquals(listOf("a0", "a1"), used)
        assertEquals("r1", f.stored!!.refreshToken)
    }

    @Test
    fun `a second 401 is reported as signed out, not retried forever`() = runBlocking {
        val f = Fixture(SyncTokens("a0", "r0", now + 3_600, null))
        var calls = 0
        try {
            f.keeper.authorized<Unit> { calls++; throw SyncHttpException(401, "nope") }
            fail()
        } catch (e: SyncSignedOutException) {
            assertTrue(e.message!!.contains("connect Strava again"))
        }
        assertEquals(2, calls)
    }

    @Test
    fun `a refresh token the provider rejects means connect again`() = runBlocking {
        val f = Fixture(SyncTokens("a0", "r0", now - 1, null), rejectRefresh = true)
        try {
            f.keeper.accessToken()
            fail()
        } catch (e: SyncSignedOutException) {
            assertTrue(e.message!!.contains("Disconnect and connect Strava again"))
        }
    }

    @Test
    fun `other failures are passed through untouched`() = runBlocking {
        val f = Fixture(SyncTokens("a0", "r0", now + 3_600, null))
        try {
            f.keeper.authorized<Unit> { throw SyncHttpException(500, "boom") }
            fail()
        } catch (e: SyncHttpException) {
            assertEquals(500, e.code)
        }
        assertTrue(f.refreshedWith.isEmpty())
    }

    @Test
    fun `a provider with no refresh token is signed out on 401`() = runBlocking {
        val k = TokenKeeper("Intervals.icu", { SyncTokens("i", null, null, null) }, {}, null, { now })
        try {
            k.authorized<Unit> { throw SyncHttpException(401, "nope") }
            fail()
        } catch (e: SyncSignedOutException) {
            assertTrue(e.message!!.contains("Intervals.icu"))
        }
    }

    // --- error wording ---

    private val cloudflare502 = """{"type":"https://developers.cloudflare.com/support/troubleshooting/http-status-codes/cloudflare-5xx-errors/error-502/","title":"Error 502: Bad gateway","status":502,"detail":"The origin returned an invalid response"}"""

    @Test
    fun `a Cloudflare error document is never shown raw`() {
        val msg = SyncErrors.describe(502, cloudflare502, SyncErrors.SERVICE)
        assertEquals("The sync service could not be reached (HTTP 502). Try again in a few minutes.", msg)
        assertFalse(msg.contains("cloudflare"))
        val html = SyncErrors.describe(502, "<html><body>Bad gateway</body></html>", "Strava")
        assertEquals("Strava could not be reached (HTTP 502). Try again in a few minutes.", html)
    }

    @Test
    fun `the service's own error text is shown as is`() {
        val body = """{"error":"Strava is having trouble right now (HTTP 500). Try again in a few minutes."}"""
        assertEquals("Strava is having trouble right now (HTTP 500). Try again in a few minutes.",
            SyncErrors.describe(503, body, SyncErrors.SERVICE))
        assertEquals("The sync service could not verify this app (app check token expired).",
            SyncErrors.describe(401, """{"error":"app check token expired"}""", SyncErrors.SERVICE))
    }

    @Test
    fun `provider errors are named and short`() {
        assertEquals("Strava: Bad Request (HTTP 400)",
            SyncErrors.describe(400, """{"message":"Bad Request","errors":[]}""", "Strava"))
        assertEquals("Strava refused the sign-in (HTTP 401).",
            SyncErrors.describe(401, """{"message":"Authorization Error"}""", "Strava"))
        assertEquals("Strava is limiting requests right now. Try again in 15 minutes.",
            SyncErrors.describe(429, "", "Strava"))
    }
}
