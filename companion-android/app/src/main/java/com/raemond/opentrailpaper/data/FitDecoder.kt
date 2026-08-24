package com.raemond.opentrailpaper.data

import kotlin.math.max

/** A parsed ride, ready to preview. */
data class RidePreview(
    val points: List<Point> = emptyList(),
    val startEpochMs: Long? = null,
    val duration: Double = 0.0,      // elapsed wall-clock, seconds
    val movingTime: Double = 0.0,    // device "MOVING TIME" (session timer)
    val distanceKm: Double = 0.0,
    val avgSpeedKmh: Double = 0.0,
    val maxSpeedKmh: Double = 0.0,
    val avgPower: Int? = null,
    val normPower: Int? = null,
    val avgHeartRate: Int? = null,
    val ascentM: Double = 0.0,
) {
    data class Point(
        val coordinate: LatLon,
        val altitude: Double?,
        val speedKmh: Double?,
        val power: Int?,
        val heartRate: Int?,
        val cadence: Int?,
    )

    val coordinates: List<LatLon> get() = points.map { it.coordinate }
}

/**
 * Minimal FIT decoder for the files this device writes (see firmware
 * src/fit_writer.cpp). It reads definition + data messages generically and
 * interprets the record message (global 20) and the session roll-up (global 18).
 *
 * A port of companion-ios/Sources/FITDecoder.swift, field-for-field.
 */
object FitDecoder {
    private const val EPOCH_OFFSET_SEC = 631_065_600.0   // 1989-12-31
    private const val SEMICIRCLE = 180.0 / 2147483648.0

    private data class FieldDef(val num: Int, val size: Int)
    private data class MsgDef(val global: Int, val fields: List<FieldDef>)

    fun decode(data: ByteArray): RidePreview? {
        val b = data
        if (b.size <= 14) return null

        val headerSize = b[0].toInt() and 0xFF
        if (b.size < headerSize) return null
        val dataSize = le32(b, 4)
        var i = headerSize
        // A ride that lost power mid-recording never got its header patched
        // (data_size = 0) or trailer/CRC written. In that case scan to the end of
        // the file — the record messages before the cut are still valid, and the
        // bounds checks below stop cleanly at any trailing partial write.
        var end = headerSize + dataSize
        if (dataSize == 0 || end > b.size || end < headerSize) end = b.size

        val defs = HashMap<Int, MsgDef>()
        val points = ArrayList<RidePreview.Point>()
        var firstTime: Long? = null
        var lastTime = 0L
        var powerSum = 0; var powerN = 0
        var hrSum = 0; var hrN = 0
        var maxSpeed = 0.0
        var ascent = 0.0
        var climbBase: Double? = null   // 3 m-hysteresis ascent, like the device

        // Session roll-up (global 18) — the numbers the device shows on its
        // ride-complete screen. Preferred over the record-derived values below
        // when present (older files without a summary fall back to derived).
        var sMoving: Double? = null
        var sDistKm: Double? = null
        var sAvgSpeed: Double? = null
        var sAvgPower: Int? = null
        var sNormPower: Int? = null
        var sAvgHr: Int? = null
        var sAscent: Double? = null

        while (i < end) {
            val rec = b[i].toInt() and 0xFF
            i += 1
            if (rec and 0x40 != 0) {                          // definition message
                val local = rec and 0x0F
                if (i + 5 > end) break
                // b[i]=reserved, b[i+1]=arch (0=LE), b[i+2..3]=global, b[i+4]=count
                val global = (b[i + 2].toInt() and 0xFF) or ((b[i + 3].toInt() and 0xFF) shl 8)
                val count = b[i + 4].toInt() and 0xFF
                i += 5
                val fields = ArrayList<FieldDef>(count)
                for (f in 0 until count) {
                    if (i + 3 > end) break
                    fields.add(FieldDef(b[i].toInt() and 0xFF, b[i + 1].toInt() and 0xFF))
                    i += 3
                }
                // Developer fields (0x20) — this device never emits them.
                defs[local] = MsgDef(global, fields)
            } else {                                          // data message
                val def = defs[rec and 0x0F] ?: break
                var off = i
                var lat: Int? = null
                var lon: Int? = null
                var alt: Double? = null
                var speed: Double? = null
                var power: Int? = null
                var hr: Int? = null
                var cad: Int? = null
                for (f in def.fields) {
                    if (off + f.size > end) break
                    if (def.global == 20) {                   // record
                        when (f.num) {
                            253 -> {
                                lastTime = le32(b, off).toLong() and 0xFFFFFFFFL
                                if (firstTime == null) firstTime = lastTime
                            }

                            0 -> le32(b, off).let { if (it != 0x7FFFFFFF) lat = it }
                            1 -> le32(b, off).let { if (it != 0x7FFFFFFF) lon = it }
                            2 -> le16(b, off).let { if (it != 0xFFFF) alt = it / 5.0 - 500.0 }
                            6 -> le16(b, off).let { if (it != 0xFFFF) speed = it / 1000.0 * 3.6 }
                            7 -> le16(b, off).let { if (it != 0xFFFF) power = it }
                            3 -> (b[off].toInt() and 0xFF).let { if (it != 0xFF) hr = it }
                            4 -> (b[off].toInt() and 0xFF).let { if (it != 0xFF) cad = it }
                        }
                    } else if (def.global == 18) {            // session roll-up
                        when (f.num) {
                            8 -> sMoving = (le32(b, off).toLong() and 0xFFFFFFFFL) / 1000.0
                            9 -> sDistKm = (le32(b, off).toLong() and 0xFFFFFFFFL) / 100.0 / 1000.0
                            14 -> le16(b, off).let { if (it != 0xFFFF) sAvgSpeed = it / 1000.0 * 3.6 }
                            16 -> (b[off].toInt() and 0xFF).let { if (it != 0xFF) sAvgHr = it }
                            20 -> le16(b, off).let { if (it != 0xFFFF) sAvgPower = it }
                            34 -> le16(b, off).let { if (it != 0xFFFF) sNormPower = it }
                            22 -> le16(b, off).let { if (it != 0xFFFF) sAscent = it.toDouble() }
                        }
                    }
                    off += f.size
                }
                i = off
                val la = lat
                val lo = lon
                if (def.global == 20 && la != null && lo != null) {
                    points.add(
                        RidePreview.Point(
                            coordinate = LatLon(la * SEMICIRCLE, lo * SEMICIRCLE),
                            altitude = alt,
                            speedKmh = speed,
                            power = power,
                            heartRate = hr,
                            cadence = cad,
                        ),
                    )
                    speed?.let { maxSpeed = max(maxSpeed, it) }
                    power?.let { powerSum += it; powerN += 1 }
                    hr?.let { hrSum += it; hrN += 1 }
                    alt?.let { a ->
                        val base = climbBase
                        if (base == null) {
                            climbBase = a
                        } else if (a > base + 3) {
                            ascent += a - base; climbBase = a
                        } else if (a < base - 3) {
                            climbBase = a
                        }
                    }
                }
            }
        }

        if (points.size < 2) return null

        val ft = firstTime
        val startMs = ft?.let { ((EPOCH_OFFSET_SEC + it) * 1000).toLong() }
        val duration = if (ft != null) (lastTime - ft).toDouble() else 0.0
        var distanceKm = trackDistanceKm(points.map { it.coordinate })
        var avgSpeed = if (duration > 0) distanceKm / (duration / 3600) else 0.0
        var avgPower = if (powerN > 0) powerSum / powerN else null
        var avgHr = if (hrN > 0) hrSum / hrN else null

        // Prefer the device's session roll-up so the app matches the
        // ride-complete screen exactly (incl. the map-DEM ascent, which can't be
        // re-derived).
        sDistKm?.let { if (it > 0) distanceKm = it }
        sAvgSpeed?.let { avgSpeed = it }
        sAvgPower?.let { avgPower = it }
        sAvgHr?.let { avgHr = it }

        return RidePreview(
            points = points,
            startEpochMs = startMs,
            duration = duration,
            movingTime = sMoving ?: duration,
            distanceKm = distanceKm,
            avgSpeedKmh = avgSpeed,
            maxSpeedKmh = maxSpeed,
            avgPower = avgPower,
            normPower = sNormPower,
            avgHeartRate = avgHr,
            ascentM = sAscent ?: ascent,
        )
    }

    private fun le16(b: ByteArray, o: Int): Int =
        (b[o].toInt() and 0xFF) or ((b[o + 1].toInt() and 0xFF) shl 8)

    private fun le32(b: ByteArray, o: Int): Int =
        (b[o].toInt() and 0xFF) or ((b[o + 1].toInt() and 0xFF) shl 8) or
            ((b[o + 2].toInt() and 0xFF) shl 16) or ((b[o + 3].toInt() and 0xFF) shl 24)

    private fun trackDistanceKm(c: List<LatLon>): Double {
        if (c.size < 2) return 0.0
        var m = 0.0
        for (i in 1 until c.size) m += c[i - 1].distanceTo(c[i])
        return m / 1000
    }
}
