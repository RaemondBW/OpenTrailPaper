package com.raemond.opentrailpaper

import com.raemond.opentrailpaper.data.*
import org.junit.Assert.*
import org.junit.Test
import java.util.Base64

class RideUploadTest {
    private val service = IntervalsUploadService()
    private val fit = byteArrayOf(14, 32, 0, 0, 0, 0, 0, 0, 46, 70, 73, 84, 0, -1)
    @Test fun `multipart preserves binary FIT bytes and uses authenticated athlete`() {
        val request = service.request(fit, "sample-key", "boundary")
        assertEquals("/api/v1/athlete/0/activities", request.url.path)
        assertEquals("Basic " + Base64.getEncoder().encodeToString("API_KEY:sample-key".toByteArray()), request.headers["Authorization"])
        val expected = "--boundary\r\nContent-Disposition: form-data; name=\"file\"; filename=\"ride.fit\"\r\nContent-Type: application/octet-stream\r\n\r\n".toByteArray() + fit + "\r\n--boundary--\r\n".toByteArray()
        assertArrayEquals(expected, request.body)
        assertTrue(request.url.query.contains("external_id=otp-${rideUploadHash(fit)}"))
        assertEquals(request.url, service.request(fit, "another-account").url)
        assertNotEquals(request.url, service.request(fit + byteArrayOf(1), "sample-key").url)
    }
    @Test fun `created and duplicate receipts follow documented response`() {
        val json = """{"id":"upload1","icu_athlete_id":"i123","activities":[{"id":"i456"}]}"""
        assertEquals(RideUploadReceipt("i456", false), service.receipt(json, 201))
        assertEquals(RideUploadReceipt("i456", true), service.receipt(json, 200))
    }
    @Test fun `failed and unconfirmed uploads never become success`() {
        for (status in listOf(202, 302, 400, 401, 403, 422, 429, 500)) {
            assertThrows(RideUploadException::class.java) { service.receipt("secret account details", status) }
        }
        for (body in listOf("{}", "[]", "<html>login</html>", "{\"activities\":[]}", "{\"id\":\"\"}")) {
            assertThrows(RideUploadException::class.java) { service.receipt(body, 201) }
        }
        assertThrows(RideUploadException::class.java) { service.request(fit, " ") }
        assertThrows(RideUploadException::class.java) { service.request(byteArrayOf(1, 2), "key") }
    }
}
