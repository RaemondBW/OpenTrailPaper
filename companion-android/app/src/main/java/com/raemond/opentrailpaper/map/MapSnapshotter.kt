package com.raemond.opentrailpaper.map

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Path
import android.util.LruCache
import androidx.compose.ui.graphics.toArgb
import com.raemond.opentrailpaper.BuildConfig
import com.raemond.opentrailpaper.data.BoundingBox
import com.raemond.opentrailpaper.data.LatLon
import com.raemond.opentrailpaper.ui.Palette
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.osmdroid.config.Configuration
import java.io.File
import java.net.HttpURLConnection
import java.net.URL
import kotlin.math.floor
import kotlin.math.max

/**
 * A still map with a ride's track drawn on it, for the Rides list.
 *
 * iOS gets this from MKMapSnapshotter. There is no such thing on Android outside
 * a live map view, and putting a live MapView inside every row of a scrolling
 * list is exactly the wrong shape — so the handful of tiles the track needs are
 * fetched, composited once, and cached by ride name. A card renders a bitmap,
 * not a map engine.
 *
 * Tiles come from the standard OSM tile servers under the same User-Agent and
 * on-disk cache osmdroid uses, and a thumbnail is drawn once per ride, so this
 * stays well inside their usage policy.
 */
object MapSnapshotter {

    private val TILE = MapStyle.TILE_SIZE
    private const val MAX_TILES = 24        // a thumbnail is never worth more

    /** ~12 MB of thumbnails; a row is ~340×170 at 3x. */
    private val memory = object : LruCache<String, Bitmap>(12 * 1024 * 1024) {
        override fun sizeOf(key: String, value: Bitmap) = value.byteCount
    }

    private val tileDir: File by lazy {
        // Versioned by style: a cache of the old basemap would leave half the ride
        // list in one design and half in another.
        File(Configuration.getInstance().osmdroidBasePath, "snapshots-v2").apply { mkdirs() }
    }

    /**
     * Renders [coords] over map tiles at [widthPx]×[heightPx], or null if the
     * track has no extent or no tile could be fetched.
     */
    suspend fun snapshot(
        key: String,
        coords: List<LatLon>,
        widthPx: Int,
        heightPx: Int,
    ): Bitmap? {
        if (coords.size < 2 || widthPx < 8 || heightPx < 8) return null
        val cacheKey = "$key@${widthPx}x$heightPx"
        memory.get(cacheKey)?.let { return it }

        val bitmap = withContext(Dispatchers.IO) {
            render(coords, widthPx, heightPx)
        } ?: return null
        memory.put(cacheKey, bitmap)
        return bitmap
    }

    private fun render(coords: List<LatLon>, w: Int, h: Int): Bitmap? {
        val box = BoundingBox.around(coords) ?: return null

        // ~18% padding around the track, and a floor on the span so a lap of the
        // block doesn't render at doorway zoom.
        val padLat = max((box.north - box.south) * 0.18, 0.0015)
        val padLon = max((box.east - box.west) * 0.18, 0.0015)
        val south = box.south - padLat
        val north = box.north + padLat
        val west = box.west - padLon
        val east = box.east + padLon

        // Largest zoom whose world still fits the padded track in the box, capped
        // so a short ride doesn't demand a hundred tiles.
        var zoom = 17
        while (zoom > 1) {
            val size = TILE * (1 shl zoom).toDouble()
            val dx = (MercatorWorld.x(east) - MercatorWorld.x(west)) / MercatorWorld.WORLD * size
            val dy = (MercatorWorld.y(south) - MercatorWorld.y(north)) / MercatorWorld.WORLD * size
            val tilesWide = dx / TILE + 1
            val tilesHigh = dy / TILE + 1
            if (dx <= w && dy <= h && tilesWide * tilesHigh <= MAX_TILES) break
            zoom -= 1
        }

        val mapSize = TILE * (1 shl zoom).toDouble()
        fun px(c: LatLon) = doubleArrayOf(
            MercatorWorld.x(c.lon) / MercatorWorld.WORLD * mapSize,
            MercatorWorld.y(c.lat) / MercatorWorld.WORLD * mapSize,
        )

        val centre = px(LatLon((south + north) / 2, (west + east) / 2))
        val left = centre[0] - w / 2.0
        val top = centre[1] - h / 2.0

        val out = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888)
        val canvas = Canvas(out)
        canvas.drawColor(Palette.paper.toArgb())

        val tx0 = floor(left / TILE).toInt()
        val ty0 = floor(top / TILE).toInt()
        val tx1 = floor((left + w) / TILE).toInt()
        val ty1 = floor((top + h) / TILE).toInt()
        val maxTile = (1 shl zoom) - 1
        var drewAny = false
        for (ty in ty0..ty1) {
            if (ty < 0 || ty > maxTile) continue
            for (tx in tx0..tx1) {
                val wrapped = ((tx % (maxTile + 1)) + maxTile + 1) % (maxTile + 1)
                val tile = tileBitmap(zoom, wrapped, ty) ?: continue
                canvas.drawBitmap(
                    tile,
                    (tx * TILE - left).toFloat(),
                    (ty * TILE - top).toFloat(),
                    null,
                )
                drewAny = true
            }
        }
        if (!drewAny) {
            out.recycle()
            return null   // offline: let the caller fall back to the track glyph
        }

        val line = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            style = Paint.Style.STROKE
            strokeWidth = 3.5f * (w / 340f).coerceIn(1f, 3f)
            strokeCap = Paint.Cap.ROUND
            strokeJoin = Paint.Join.ROUND
            color = Palette.accent.toArgb()
        }
        val path = Path()
        for ((i, c) in coords.withIndex()) {
            val p = px(c)
            val x = (p[0] - left).toFloat()
            val y = (p[1] - top).toFloat()
            if (i == 0) path.moveTo(x, y) else path.lineTo(x, y)
        }
        canvas.drawPath(path, line)

        // Start marker.
        val start = px(coords.first())
        canvas.drawCircle(
            (start[0] - left).toFloat(),
            (start[1] - top).toFloat(),
            line.strokeWidth * 1.2f,
            Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Palette.good.toArgb() },
        )
        return out
    }

    private fun tileBitmap(z: Int, x: Int, y: Int): Bitmap? {
        val file = File(tileDir, "$z-$x-$y.png")
        if (file.exists()) {
            BitmapFactory.decodeFile(file.path)?.let { return it }
            file.delete()
        }
        var conn: HttpURLConnection? = null
        return try {
            conn = (URL(MapStyle.snapshotTileUrl(z, x, y)).openConnection()
                as HttpURLConnection).apply {
                setRequestProperty(
                    "User-Agent",
                    Configuration.getInstance().userAgentValue
                        ?: "OpenTrailPaper-Android/${BuildConfig.VERSION_NAME}",
                )
                connectTimeout = 15_000
                readTimeout = 15_000
            }
            if (conn.responseCode != 200) return null
            val bytes = conn.inputStream.readBytes()
            file.writeBytes(bytes)
            BitmapFactory.decodeByteArray(bytes, 0, bytes.size)
        } catch (_: Exception) {
            null
        } finally {
            conn?.disconnect()
        }
    }
}
