package com.raemond.opentrailpaper.map

import org.osmdroid.tileprovider.tilesource.OnlineTileSourceBase
import org.osmdroid.tileprovider.tilesource.XYTileSource

/**
 * The base map everything is drawn on.
 *
 * NOT the standard OpenStreetMap style. That style is a general-purpose map of
 * everything — motorway shields, ferry routes, county lines, administrative
 * boundaries, POI icons at every zoom — and this app needs the opposite: a quiet
 * ground for the coverage hexagons to sit on. Which ground you hold only reads at
 * a glance if the map underneath is calm.
 *
 * CARTO's Voyager is the closest keyless OSM raster style to what Apple Maps
 * gives the iOS companion: soft land, white roads with light casing, muted green
 * parks and pale blue water, and far fewer labels. Retina tiles, because a 256 px
 * tile stretched over a 3x display is the other half of why the default looked
 * muddy.
 *
 * LABELS ARE A SEPARATE LAYER, and that is the point of splitting them. The iOS
 * hexagons are deliberately drawn below the map's labels so place and street
 * names come back through on top of the coverage tint — without that, a
 * downloaded region is an anonymous patch of colour: you can see the shape of
 * your coverage but not read where it is. Raster tiles bake labels into the
 * image, so the only way to reproduce that is to draw the labels again on top.
 *
 * Attribution is required by both OpenStreetMap and CARTO and is drawn on every
 * map by [com.raemond.opentrailpaper.ui.OsmMap]. CARTO's basemaps are free to use
 * with attribution for non-commercial projects; swapping this object's URLs is
 * the whole of what a different provider would take.
 */
object MapStyle {

    /** Retina tiles are 512 px. Everything that converts between zoom levels and
     *  pixels reads this rather than assuming the usual 256. */
    const val TILE_SIZE = 512

    private const val MIN_ZOOM = 2
    private const val MAX_ZOOM = 20

    private val SUBDOMAINS = arrayOf("a", "b", "c", "d")

    private const val ATTRIBUTION = "© OpenStreetMap contributors, © CARTO"

    /** What the map must display, wherever it is drawn. */
    const val attribution = ATTRIBUTION

    private fun urls(style: String) =
        SUBDOMAINS.map { "https://$it.basemaps.cartocdn.com/rastertiles/$style/" }.toTypedArray()

    /** Land, water, parks and roads — no text. */
    val base: OnlineTileSourceBase = XYTileSource(
        "CartoVoyagerNoLabels",
        MIN_ZOOM,
        MAX_ZOOM,
        TILE_SIZE,
        "@2x.png",
        urls("voyager_nolabels"),
        ATTRIBUTION,
    )

    /** Text only, on transparent tiles, so it can be drawn back on top of the
     *  coverage hexagons. */
    val labels: OnlineTileSourceBase = XYTileSource(
        "CartoVoyagerLabels",
        MIN_ZOOM,
        MAX_ZOOM,
        TILE_SIZE,
        "@2x.png",
        urls("voyager_only_labels"),
        ATTRIBUTION,
    )

    /** One flattened tile, for the ride thumbnails — a still image has nothing to
     *  layer, so it takes the style with its labels already in it. */
    fun snapshotTileUrl(z: Int, x: Int, y: Int): String {
        val subdomain = SUBDOMAINS[(x + y) umod SUBDOMAINS.size]
        return "https://$subdomain.basemaps.cartocdn.com/rastertiles/voyager/$z/$x/$y@2x.png"
    }

    private infix fun Int.umod(m: Int) = ((this % m) + m) % m
}
