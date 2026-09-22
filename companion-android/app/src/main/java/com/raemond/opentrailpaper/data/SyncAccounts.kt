package com.raemond.opentrailpaper.data

import android.content.Context
import android.content.SharedPreferences
import android.net.Uri
import androidx.browser.customtabs.CustomTabsIntent
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateMapOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.security.crypto.EncryptedSharedPreferences
import androidx.security.crypto.MasterKey
import com.google.firebase.FirebaseApp
import com.google.firebase.FirebaseOptions
import com.google.firebase.appcheck.FirebaseAppCheck
import com.google.firebase.appcheck.debug.DebugAppCheckProviderFactory
import com.google.firebase.appcheck.playintegrity.PlayIntegrityAppCheckProviderFactory
import com.raemond.opentrailpaper.BuildConfig
import kotlinx.coroutines.tasks.await
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.io.File
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL
import java.security.SecureRandom
import android.util.Base64
import android.util.Log

/**
 * Strava and RideWithGPS accounts: connect, keep the tokens, upload rides.
 *
 * The phone never holds either provider's client secret or the RideWithGPS API
 * key. Those live in the sync-auth service (cloud/sync-auth), which runs the
 * consent flow: the app opens `<service>/v1/auth/<provider>/start` in a Custom
 * Tab, the provider sends its code to the service, the service swaps it for
 * tokens and bounces back to `opentrailpaper://sync/<provider>?handoff=…&state=…`
 * (an intent filter on MainActivity), and the app trades the short-lived
 * handoff for the tokens over HTTPS. Only this user's tokens ever reach the
 * phone, and they sit in EncryptedSharedPreferences.
 *
 * Uploads: Strava and Intervals.icu take the FIT straight from the phone with
 * the user's bearer token; RideWithGPS goes through the service, because every
 * RWGPS request also needs the API key.
 *
 * Only this app: every call to the service carries a Firebase App Check token
 * (Play Integrity on a real install; a registered debug token on an emulator or
 * debug build), and the sign-in return is an App Link on the service's host
 * that Android hands to this app alone. The service refuses anything else.
 */
object SyncAccounts {
    enum class Provider(val id: String, val title: String) {
        STRAVA("strava", "Strava"),
        INTERVALS("intervals", "Intervals.icu"),
        RIDEWITHGPS("ridewithgps", "RideWithGPS");

        companion object {
            fun byId(id: String?) = entries.firstOrNull { it.id == id }
        }
    }

    data class Tokens(
        val accessToken: String,
        val refreshToken: String?,
        val expiresAt: Long?,          // unix seconds; null = does not expire
        val athleteName: String?,
    ) {
        val expiresSoon get() = expiresAt != null && System.currentTimeMillis() / 1000 > expiresAt - 300
    }

    class SyncException(message: String) : IOException(message)

    /** Public address of the service, from BuildConfig (local.properties `sync.url` or CI). */
    val serviceUrl: String? = BuildConfig.SYNC_SERVICE_URL.trim().takeIf { it.startsWith("http") }?.trimEnd('/')
    val isConfigured get() = serviceUrl != null
    const val CALLBACK_SCHEME = "opentrailpaper"
    const val RETURN_PATH_PREFIX = "/app/sync/"
    val serviceHost: String? get() = serviceUrl?.let { Uri.parse(it).host }
    var attested = false
        private set

    private const val FILE = "opentrailpaper.sync"
    private lateinit var prefs: SharedPreferences

    /** Compose-observable copies of what the store holds. */
    val tokens = mutableStateMapOf<Provider, Tokens>()
    var busy by mutableStateOf<Provider?>(null)
        private set
    private var lastErrorState by mutableStateOf<String?>(null)
    var lastError: String?
        get() = lastErrorState
        set(value) {
            lastErrorState = value
            // Also to logcat: the card's text is the only other place it goes.
            if (value != null) Log.w(TAG, value)
        }
    private const val TAG = "SyncAccounts"

    private val pendingState = mutableMapOf<Provider, String>()

    fun init(context: Context) {
        val app = context.applicationContext
        prefs = try {
            val key = MasterKey.Builder(app).setKeyScheme(MasterKey.KeyScheme.AES256_GCM).build()
            EncryptedSharedPreferences.create(
                app, FILE, key,
                EncryptedSharedPreferences.PrefKeyEncryptionScheme.AES256_SIV,
                EncryptedSharedPreferences.PrefValueEncryptionScheme.AES256_GCM,
            )
        } catch (e: Exception) {
            // A keystore that will not open (seen after some OS restores) must
            // not take the whole app down; a plain file without the tokens
            // just means "connect again".
            app.deleteSharedPreferences(FILE)
            app.getSharedPreferences(FILE, Context.MODE_PRIVATE)
        }
        for (p in Provider.entries) read(p)?.let { tokens[p] = it }

        // Firebase, for App Check only. A build without the ids runs unattested,
        // which the service rejects; the Accounts card says so.
        if (BuildConfig.FIREBASE_APP_ID.contains(":") && BuildConfig.FIREBASE_API_KEY.isNotEmpty()) {
            runCatching {
                if (FirebaseApp.getApps(app).isEmpty()) {
                    FirebaseApp.initializeApp(
                        app,
                        FirebaseOptions.Builder()
                            .setApplicationId(BuildConfig.FIREBASE_APP_ID)
                            .setApiKey(BuildConfig.FIREBASE_API_KEY)
                            .setProjectId(BuildConfig.FIREBASE_PROJECT_ID)
                            .setGcmSenderId(BuildConfig.FIREBASE_SENDER_ID)
                            .build(),
                    )
                }
                FirebaseAppCheck.getInstance().installAppCheckProviderFactory(
                    if (BuildConfig.DEBUG || BuildConfig.APP_CHECK_DEBUG) {
                        // Prints a debug token to logcat once; register it under
                        // App Check > Apps > Manage debug tokens.
                        DebugAppCheckProviderFactory.getInstance()
                    } else {
                        PlayIntegrityAppCheckProviderFactory.getInstance()
                    },
                )
                attested = true
                // Debug builds: ask once now so the debug token is printed at
                // launch (the provider only logs it on the first request).
                if (BuildConfig.DEBUG || BuildConfig.APP_CHECK_DEBUG) FirebaseAppCheck.getInstance().getAppCheckToken(false)
            }.onFailure { lastError = "App Check unavailable: ${it.message}" }
        }
    }

    /** Is this an `https://<service host>/app/sync/...` or `opentrailpaper://sync/...` link? */
    fun isReturnLink(uri: Uri?): Boolean {
        uri ?: return false
        if (uri.scheme == CALLBACK_SCHEME && uri.host == "sync") return true
        return uri.scheme == "https" && uri.host == serviceHost && (uri.path ?: "").startsWith(RETURN_PATH_PREFIX)
    }

    fun isConnected(p: Provider) = tokens.containsKey(p)

    // --- connect / disconnect -----------------------------------------------

    suspend fun connect(context: Context, p: Provider) {
        if (serviceUrl == null) { lastError = "This build has no sync service configured."; return }
        val state = randomState()
        pendingState[p] = state
        lastError = null
        busy = p
        try {
            // The consent URL is issued only to an attested app (the ticket in
            // it is what /start accepts), so nothing but this app can start a flow.
            val json = postJson("/v1/auth/${p.id}/begin", JSONObject().put("state", state))
            val url = json.optString("url").takeIf { it.startsWith("http") }
                ?: throw SyncException("Sign-in failed: no url")
            CustomTabsIntent.Builder().setShowTitle(true).build().launchUrl(context, Uri.parse(url))
        } catch (e: Exception) {
            busy = null
            pendingState.remove(p)
            lastError = e.message ?: "Sign-in failed."
        }
    }

    /** True when the intent was ours (the sign-in return, see isReturnLink). */
    suspend fun handleCallback(uri: Uri?): Boolean {
        if (!isReturnLink(uri)) return false
        val p = Provider.byId(uri!!.lastPathSegment) ?: return true
        try {
            val state = uri.getQueryParameter("state")
            if (state == null || state != pendingState[p]) {
                lastError = "Sign-in response did not match this app's request."
                return true
            }
            pendingState.remove(p)
            uri.getQueryParameter("error")?.let { lastError = "Access was not granted ($it)."; return true }
            val handoff = uri.getQueryParameter("handoff") ?: run {
                lastError = "Sign-in response was incomplete."; return true
            }
            val json = postJson("/v1/auth/handoff", JSONObject().put("handoff", handoff))
            val access = json.optString("access_token").takeIf { it.isNotEmpty() }
                ?: throw SyncException("Sign-in failed: no token")
            val athlete = json.optJSONObject("athlete")
            store(
                p,
                Tokens(
                    accessToken = access,
                    refreshToken = json.optString("refresh_token").takeIf { it.isNotEmpty() },
                    expiresAt = if (json.has("expires_at")) json.optLong("expires_at") else null,
                    athleteName = athlete?.optString("name")?.takeIf { it.isNotEmpty() },
                ),
            )
        } catch (e: Exception) {
            lastError = e.message ?: "Sign-in failed."
        } finally {
            busy = null
        }
        return true
    }

    /** Called when the activity comes back to the front: a closed Custom Tab is a cancel. */
    fun noteResumed() {
        if (busy != null && pendingState.isEmpty()) busy = null
    }

    suspend fun disconnect(p: Provider) {
        val t = tokens[p] ?: run { store(p, null); return }
        store(p, null)
        // Best effort: tell the provider too, so the app disappears from its list.
        when (p) {
            Provider.STRAVA -> runCatching {
                postJson("/v1/auth/strava/revoke", JSONObject().put("access_token", t.accessToken))
            }
            Provider.INTERVALS -> runCatching {
                withContext(Dispatchers.IO) {
                    finish(open("https://intervals.icu/api/v1/disconnect-app", "DELETE", t.accessToken))
                }
            }
            Provider.RIDEWITHGPS -> Unit
        }
    }

    // --- tokens ---------------------------------------------------------------

    private suspend fun accessToken(p: Provider): String {
        var t = tokens[p] ?: throw SyncException("Not connected.")
        if (t.expiresSoon && p == Provider.STRAVA && t.refreshToken != null) {
            val json = postJson("/v1/auth/strava/refresh", JSONObject().put("refresh_token", t.refreshToken))
            val a = json.optString("access_token").takeIf { it.isNotEmpty() }
                ?: throw SyncException("Sign-in expired. Disconnect and connect again.")
            t = t.copy(
                accessToken = a,
                refreshToken = json.optString("refresh_token").takeIf { it.isNotEmpty() } ?: t.refreshToken,
                expiresAt = if (json.has("expires_at")) json.optLong("expires_at") else null,
            )
            store(p, t)
        }
        return t.accessToken
    }

    private fun store(p: Provider, t: Tokens?) {
        prefs.edit().apply {
            if (t == null) remove(p.id) else putString(
                p.id,
                JSONObject()
                    .put("access", t.accessToken)
                    .put("refresh", t.refreshToken)
                    .put("exp", t.expiresAt)
                    .put("name", t.athleteName)
                    .toString(),
            )
        }.apply()
        if (t == null) tokens.remove(p) else tokens[p] = t
    }

    private fun read(p: Provider): Tokens? {
        val s = prefs.getString(p.id, null) ?: return null
        return runCatching {
            val j = JSONObject(s)
            Tokens(
                accessToken = j.getString("access"),
                refreshToken = j.optString("refresh").takeIf { it.isNotEmpty() },
                expiresAt = if (j.isNull("exp")) null else j.optLong("exp"),
                athleteName = j.optString("name").takeIf { it.isNotEmpty() },
            )
        }.getOrNull()
    }

    // --- uploads --------------------------------------------------------------

    /** Uploads a .fit; returns a short status line for the UI. */
    suspend fun upload(file: File, to: Provider, name: String): String = withContext(Dispatchers.IO) {
        val token = accessToken(to)
        when (to) {
            Provider.STRAVA -> {
                val json = multipart(
                    "https://www.strava.com/api/v3/uploads", token,
                    fields = mapOf("data_type" to "fit", "name" to name), file = file,
                )
                json.optString("error").takeIf { it.isNotEmpty() }?.let { throw SyncException(it) }
                val id = json.optLong("id", 0)
                if (id == 0L) return@withContext "Uploaded to Strava."
                // Strava processes asynchronously: poll briefly for the activity id.
                repeat(8) {
                    delay(1500)
                    val st = getJson("https://www.strava.com/api/v3/uploads/$id", token)
                    st.optString("error").takeIf { it.isNotEmpty() }?.let { throw SyncException(it) }
                    val act = st.optLong("activity_id", 0)
                    if (act != 0L) return@withContext "Uploaded: strava.com/activities/$act"
                }
                "Uploaded to Strava (still processing)."
            }
            Provider.INTERVALS -> {
                // Athlete "0" is the bearer's own athlete. device_name is what the
                // activity shows as recorded on; external_id lets Intervals spot a
                // re-upload of the same ride (it also matches by content).
                val url = Uri.parse("https://intervals.icu/api/v1/athlete/0/activities").buildUpon()
                    .appendQueryParameter("name", name)
                    .appendQueryParameter("device_name", "OpenTrailPaper")
                    .appendQueryParameter("external_id", "otp-${file.name}")
                    .build().toString()
                val json = multipart(url, token, fields = emptyMap(), file = file)
                // 201 created / 200 duplicate; either way activities[0].id (or id).
                val first = json.optJSONArray("activities")?.optJSONObject(0) ?: json
                val id = first.optString("id")
                if (id.isNotEmpty()) "Uploaded: intervals.icu/activities/$id" else "Uploaded to Intervals.icu."
            }
            Provider.RIDEWITHGPS -> {
                val base = serviceUrl ?: throw SyncException("This build has no sync service configured.")
                val json = multipart("$base/v1/rwgps/trips", token, fields = mapOf("trip[name]" to name), file = file)
                val id = json.optJSONObject("trip")?.optLong("id", 0) ?: 0
                if (id != 0L) "Uploaded: ridewithgps.com/trips/$id" else "Uploaded to RideWithGPS."
            }
        }
    }

    // --- http -----------------------------------------------------------------

    private suspend fun postJson(path: String, body: JSONObject): JSONObject = withContext(Dispatchers.IO) {
        val base = serviceUrl ?: throw SyncException("This build has no sync service configured.")
        val c = open("$base$path", "POST", null)
        c.setRequestProperty("Content-Type", "application/json")
        c.doOutput = true
        c.outputStream.use { it.write(body.toString().toByteArray()) }
        finish(c)
    }

    private suspend fun getJson(url: String, token: String): JSONObject = finish(open(url, "GET", token))

    private suspend fun multipart(url: String, token: String, fields: Map<String, String>, file: File): JSONObject {
        val boundary = "otp-" + System.nanoTime()
        val c = open(url, "POST", token)
        c.setRequestProperty("Content-Type", "multipart/form-data; boundary=$boundary")
        c.doOutput = true
        c.setChunkedStreamingMode(0)
        c.outputStream.buffered().use { out ->
            fun line(s: String) = out.write("$s\r\n".toByteArray())
            for ((k, v) in fields) {
                line("--$boundary")
                line("Content-Disposition: form-data; name=\"$k\"")
                line("")
                line(v)
            }
            line("--$boundary")
            line("Content-Disposition: form-data; name=\"file\"; filename=\"${file.name}\"")
            line("Content-Type: application/octet-stream")
            line("")
            file.inputStream().use { it.copyTo(out) }
            line("")
            line("--$boundary--")
        }
        return finish(c)
    }

    private suspend fun open(url: String, method: String, token: String?): HttpURLConnection {
        val c = URL(url).openConnection() as HttpURLConnection
        c.requestMethod = method
        c.connectTimeout = 15_000
        c.readTimeout = 60_000
        c.setRequestProperty("Accept", "application/json")
        c.setRequestProperty("User-Agent", "OpenTrailPaper/${BuildConfig.VERSION_NAME} (Android)")
        if (token != null) c.setRequestProperty("Authorization", "Bearer $token")
        // Prove this is the app: only calls to our own service get the token.
        if (attested && URL(url).host == serviceHost) {
            val t = runCatching { FirebaseAppCheck.getInstance().getAppCheckToken(false).await().token }
                .getOrElse {
                    Log.w(TAG, "App Check token", it)
                    throw SyncException("App Check failed: ${it.message}")
                }
            c.setRequestProperty("X-Firebase-AppCheck", t)
        }
        return c
    }

    private fun finish(c: HttpURLConnection): JSONObject {
        val code = c.responseCode
        val text = (if (code in 200..299) c.inputStream else c.errorStream)
            ?.bufferedReader()?.use { it.readText() } ?: ""
        c.disconnect()
        val json = runCatching { JSONObject(text) }.getOrElse { JSONObject() }
        if (code !in 200..299) {
            if (code == 401) throw SyncException("Signed out by the provider. Disconnect and connect again.")
            val msg = json.optString("error").ifEmpty { json.optString("message").ifEmpty { text.take(120) } }
            throw SyncException("Server error $code: $msg")
        }
        return json
    }

    private fun randomState(): String {
        val b = ByteArray(24).also { SecureRandom().nextBytes(it) }
        return Base64.encodeToString(b, Base64.URL_SAFE or Base64.NO_PADDING or Base64.NO_WRAP)
    }
}
