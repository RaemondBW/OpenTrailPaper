package com.raemond.opentrailpaper.map

import com.raemond.opentrailpaper.BuildConfig
import org.osmdroid.tileprovider.tilesource.OnlineTileSourceBase
import org.osmdroid.tileprovider.tilesource.XYTileSource
import org.osmdroid.util.MapTileIndex

/**
 * The base map everything is drawn on.
 *
 * NOT the standard OpenStreetMap style. That style is a general-purpose map of
 * everything — motorway shields, ferry routes, county lines, administrative
 * boundaries, POI icons at every zoom — and this app needs the opposite: a quiet
 * ground for the coverage hexagons to sit on. Which ground you hold only reads at
 * a glance if the map underneath is calm.
 *
 * LABELS ARE A SEPARATE LAYER, and that is the point of splitting them. The iOS
 * hexagons are deliberately drawn below the map's labels so place and street
 * names come back through on top of the coverage tint — without that, a
 * downloaded region is an anonymous patch of colour: you can see the shape of
 * your coverage but not read where it is. Raster tiles bake labels into the
 * image, so the only way to reproduce that is to draw the labels again on top.
 * Both providers below offer such a pair.
 *
 * TWO PROVIDERS, chosen at build time:
 *
 *  - CARTO Voyager, when a CARTO basemaps key is built in (`carto.key` in
 *    local.properties, or OTP_CARTO_KEY in the environment). The closest raster
 *    style to what Apple Maps gives the iOS companion — soft land, white roads,
 *    muted green parks, pale blue water, few labels — as 512 px retina tiles to
 *    zoom 20. CARTO's key is free for non-commercial use (5 M tiles a month) but
 *    without one their servers watermark every tile "API KEY REQUIRED", which is
 *    why it cannot be the default for a build anyone can make.
 *
 *  - Esri's Light Gray canvas otherwise. Keyless, and even calmer: grey land,
 *    white roads, place names on a separate "Reference" layer. Its limits are
 *    256 px tiles, which osmdroid scales up for the screen density, and real
 *    data only to zoom 16 — deep enough for the coverage map and a route
 *    preview, not for reading house numbers.
 *
 * Attribution is required by every provider and is drawn on every map by
 * [com.raemond.opentrailpaper.ui.OsmMap]. Everything that varies between the
 * two lives in [Provider], so nothing else in the app knows which one it got.
 */
object MapStyle {

    /** One basemap: where its tiles come from and how they are shaped. */
    interface Provider {
        /** Stable id; names the on-disk caches, so tiles from one provider are
         *  never mistaken for another's. */
        val id: String
        /** Pixel size of a tile AS SERVED. The map may scale it for the screen;
         *  [MapSnapshotter] draws it as is. */
        val tileSize: Int
        /** Let osmdroid enlarge tiles by the display density. Right for 256 px
         *  tiles on a 3x phone, wrong for tiles that are already retina. */
        val scaleToDpi: Boolean
        val attribution: String
        /** Land, water, parks and roads — no text. */
        val base: OnlineTileSourceBase
        /** Text only, on transparent tiles, drawn back on top of the hexagons. */
        val labels: OnlineTileSourceBase
        /** One flattened tile for the ride thumbnails — a still image has nothing
         *  to layer, so it takes a style with labels already in it where the
         *  provider has one. */
        fun snapshotTileUrl(z: Int, x: Int, y: Int): String
    }

    private class Carto(key: String) : Provider {
        override val id = "carto-voyager"
        override val tileSize = 512
        override val scaleToDpi = false
        override val attribution = "© OpenStreetMap contributors, © CARTO"

        private val subdomains = arrayOf("a", "b", "c", "d")
        private val suffix = "@2x.png?key=$key"
        private fun urls(style: String) =
            subdomains.map { "https://$it.basemaps.cartocdn.com/rastertiles/$style/" }.toTypedArray()

        override val base: OnlineTileSourceBase =
            XYTileSource("CartoVoyagerNoLabels", 2, 20, tileSize, suffix, urls("voyager_nolabels"), attribution)
        override val labels: OnlineTileSourceBase =
            XYTileSource("CartoVoyagerLabels", 2, 20, tileSize, suffix, urls("voyager_only_labels"), attribution)

        override fun snapshotTileUrl(z: Int, x: Int, y: Int): String {
            val s = subdomains[(x + y) umod subdomains.size]
            return "https://$s.basemaps.cartocdn.com/rastertiles/voyager/$z/$x/$y$suffix"
        }
    }

    private object EsriLightGray : Provider {
        override val id = "esri-light-gray"
        override val tileSize = 256
        override val scaleToDpi = true
        // Esri's own copyright line for these services, shortened to fit one row.
        override val attribution = "© Esri, HERE, Garmin, © OpenStreetMap contributors"

        /** Esri addresses tiles z/y/x, not osmdroid's z/x/y, hence the override. */
        private class Source(name: String, service: String, ending: String) : OnlineTileSourceBase(
            name, 2, 16, 256, ending,
            arrayOf("https://server.arcgisonline.com/ArcGIS/rest/services/Canvas/$service/MapServer/tile/"),
            attribution,
        ) {
            override fun getTileURLString(pMapTileIndex: Long): String =
                baseUrl + MapTileIndex.getZoom(pMapTileIndex) + "/" +
                    MapTileIndex.getY(pMapTileIndex) + "/" + MapTileIndex.getX(pMapTileIndex)
        }

        override val base: OnlineTileSourceBase = Source("EsriLightGrayBase", "World_Light_Gray_Base", ".jpg")
        override val labels: OnlineTileSourceBase = Source("EsriLightGrayLabels", "World_Light_Gray_Reference", ".png")

        // Base only: the reference layer is a second fetch per tile for names
        // that are unreadable at thumbnail size anyway.
        override fun snapshotTileUrl(z: Int, x: Int, y: Int): String =
            "https://server.arcgisonline.com/ArcGIS/rest/services/Canvas/World_Light_Gray_Base/MapServer/tile/$z/$y/$x"
    }

    /** The provider this build uses. Decided once; a key is a build-time fact. */
    val active: Provider =
        if (BuildConfig.CARTO_KEY.isNotBlank()) Carto(BuildConfig.CARTO_KEY) else EsriLightGray

    /** Pixel size of a tile as served. Everything that converts between zoom
     *  levels and pixels for the snapshotter reads this rather than assuming 256;
     *  the live map reads osmdroid's own [org.osmdroid.util.TileSystem.getTileSize],
     *  which includes any density scaling. */
    val tileSize: Int get() = active.tileSize

    val attribution: String get() = active.attribution
    val base: OnlineTileSourceBase get() = active.base
    val labels: OnlineTileSourceBase get() = active.labels
    fun snapshotTileUrl(z: Int, x: Int, y: Int): String = active.snapshotTileUrl(z, x, y)

    private infix fun Int.umod(m: Int) = ((this % m) + m) % m
}
