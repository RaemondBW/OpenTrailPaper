package com.raemond.opentrailpaper.data

import com.google.gson.JsonParser
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.ByteArrayOutputStream
import java.net.HttpURLConnection
import java.net.URL
import java.security.MessageDigest
import java.util.Base64
import java.util.UUID

/** Providers own authentication and wire formats; screens pass original FIT bytes. */
interface RideUploadService {
    val id: String
    val name: String
    val credentialHelpURL: String
    suspend fun upload(data: ByteArray, credential: String): RideUploadReceipt
}
data class RideUploadReceipt(val activityID: String, val alreadyUploaded: Boolean)
class RideUploadException(message: String) : Exception(message)
fun rideUploadHash(data: ByteArray): String =
    MessageDigest.getInstance("SHA-256").digest(data).joinToString("") { "%02x".format(it) }
data class PreparedRideUpload(val url: URL, val headers: Map<String, String>, val body: ByteArray)

class IntervalsUploadService : RideUploadService {
    override val id = "intervals"
    override val name = "Intervals.icu"
    override val credentialHelpURL = "https://intervals.icu/settings"

    fun request(data: ByteArray, credential: String, boundary: String = UUID.randomUUID().toString()): PreparedRideUpload {
        if (credential.isBlank()) throw RideUploadException("Add your Intervals.icu API key in Upload services first.")
        if (data.size < 12 || !data.copyOfRange(8, 12).contentEquals(".FIT".toByteArray())) {
            throw RideUploadException("This file is not a FIT ride. Download it again from the device.")
        }
        val url = URL("https://intervals.icu/api/v1/athlete/0/activities?device_name=OpenTrailPaper&external_id=otp-${rideUploadHash(data)}")
        val body = ByteArrayOutputStream().apply {
            write(("--$boundary\r\nContent-Disposition: form-data; name=\"file\"; filename=\"ride.fit\"\r\n" +
                "Content-Type: application/octet-stream\r\n\r\n").toByteArray())
            write(data)
            write("\r\n--$boundary--\r\n".toByteArray())
        }.toByteArray()
        return PreparedRideUpload(url, mapOf(
            "Authorization" to "Basic ${Base64.getEncoder().encodeToString("API_KEY:$credential".toByteArray())}",
            "Accept" to "application/json",
            "Content-Type" to "multipart/form-data; boundary=$boundary",
        ), body)
    }

    fun receipt(body: String, status: Int): RideUploadReceipt {
        when (status) {
            200, 201 -> Unit
            401, 403 -> throw RideUploadException("Intervals.icu rejected the API key. Check it in Upload services.")
            429 -> throw RideUploadException("Intervals.icu is limiting uploads. Wait a few minutes and try again.")
            400, 422 -> throw RideUploadException("Intervals.icu could not read this ride. Try downloading the FIT file again.")
            else -> throw RideUploadException("Intervals.icu could not confirm the upload (HTTP $status). You can retry safely.")
        }
        val id = runCatching {
            val json = JsonParser.parseString(body).asJsonObject
            val activity = json.getAsJsonArray("activities")?.firstOrNull()?.asJsonObject
            (activity?.get("id") ?: json.get("id"))?.takeUnless { it.isJsonNull }?.asString
        }.getOrNull()
        if (id.isNullOrBlank()) throw RideUploadException("Intervals.icu did not confirm an activity. You can retry safely.")
        return RideUploadReceipt(id, status == 200)
    }

    override suspend fun upload(data: ByteArray, credential: String): RideUploadReceipt = withContext(Dispatchers.IO) {
        val request = request(data, credential)
        val connection = request.url.openConnection() as HttpURLConnection
        try {
            connection.requestMethod = "POST"
            connection.instanceFollowRedirects = false
            connection.connectTimeout = 20_000
            connection.readTimeout = 90_000
            connection.doOutput = true
            connection.setFixedLengthStreamingMode(request.body.size)
            request.headers.forEach { (key, value) -> connection.setRequestProperty(key, value) }
            connection.outputStream.use { it.write(request.body) }
            val status = connection.responseCode
            val body = if (status == 200 || status == 201) {
                connection.inputStream.bufferedReader().use { it.readText() }
            } else "" // Never expose account details from raw error bodies.
            receipt(body, status)
        } catch (e: RideUploadException) { throw e }
        catch (e: Exception) {
            throw RideUploadException("Upload was not confirmed. Check your connection and retry; duplicate rides are detected automatically.")
        } finally { connection.disconnect() }
    }
}
