package com.raemond.opentrailpaper.data

// Kotlin side of the device's dashboard layout (src/dash_layout.h).
//
// The wire format IS the config file's text — the same bytes that sit at
// /config/dashboard.cfg on the card. Keeping one representation means there is
// no third encoding to keep in step: what the rider edits here is literally what
// lands in the file, and what a hand-edited file says is exactly what this
// editor shows.
//
// The field ids and size names below MUST match dash_layout.cpp's tables (and
// companion-ios/Sources/DashLayout.swift). They are the contract; the labels are
// cosmetic.

data class DashField(
    val id: String,      // "power3s" — the config token
    val label: String,   // "Power (3s)" — what the rider picks from
    val detail: String,  // where the number comes from
) {
    companion object {
        // Ordered as they appear in the picker. Every one maps to something the
        // device already measures — a paired sensor, the recorder, the GPS, or
        // the elevation grid baked into the map tiles.
        val all = listOf(
            DashField("speed", "Speed", "GPS"),
            DashField("power3s", "Power (3s)", "Power meter, 3s average"),
            DashField("power", "Power", "Power meter, instant"),
            DashField("hr", "Heart rate", "Heart-rate strap"),
            DashField("cadence", "Cadence", "Cadence sensor or power meter"),
            DashField("distance", "Distance", "This ride"),
            DashField("ridetime", "Ride time", "Elapsed"),
            DashField("movingtime", "Moving time", "Elapsed minus stops"),
            DashField("climb", "Climb", "From the map elevation grid"),
            DashField("grade", "Grade", "From the map elevation grid"),
            DashField("altitude", "Altitude", "From the map elevation grid"),
            DashField("battery", "Battery", "Fuel gauge"),
            DashField("sats", "Satellites", "GPS"),
            DashField("clock", "Clock", "Time of day"),
            DashField("routeleft", "Route left", "Distance to the end of the route"),
        )

        fun named(id: String): DashField? = all.firstOrNull { it.id == id }
    }
}

enum class DashSize(val token: String, val label: String, val weight: Int) {
    // `weight` is the height share, matching kWeight in ui_render.cpp — the
    // preview is only honest if it divides the panel the same way the device
    // does.
    SMALL("small", "Small", 2),
    MEDIUM("medium", "Medium", 3),
    LARGE("large", "Large", 4),
    HERO("hero", "Hero", 8);

    companion object {
        fun fromToken(t: String): DashSize? = entries.firstOrNull { it.token == t }
    }
}

data class DashItem(
    val field: String,
    val size: DashSize,
    val half: Boolean,
    /** Stable across edits, so a reorder animates rather than rebuilding rows. */
    val key: Long = nextKey++,
) {
    // `this.` is load-bearing: inside a property accessor the bare name `field`
    // is Kotlin's backing-field keyword, not this class's `field` property, and
    // reading it there would demand a backing field that doesn't exist.
    val fieldLabel: String get() = DashField.named(this.field)?.label ?: this.field

    /**
     * The caption the DEVICE draws above this field — kFields in
     * dash_layout.cpp. Uppercasing the picker label matches every entry except
     * the 3 s power average, whose panel caption uses a middot, and the layout
     * preview has to show what the panel will show.
     */
    val panelLabel: String
        get() = if (this.field == "power3s") "POWER · 3S" else fieldLabel.uppercase()

    companion object {
        private var nextKey = 1L
    }
}

data class DashLayout(val items: List<DashItem>) {

    /**
     * Serialize back to the config file's text, byte-compatible with
     * dashSerialize() so a round trip through the device changes nothing.
     */
    val configText: String
        get() = buildString {
            append("# OpenTrailPaper dashboard layout\n")
            append("# <field> <small|medium|large|hero> [half]\n")
            append("# 'half' shares the row with the next 'half' field.\n")
            for (it in items) {
                append(it.field.padEnd(10))
                append(' ')
                append(it.size.token.padEnd(6))
                if (it.half) append(" half")
                append('\n')
            }
        }

    /**
     * Group items into rows the way ui_render.cpp packs them: a `half` item
     * pairs with the NEXT item only if that one is also `half`, otherwise it
     * spans. Duplicated here so the preview shows what the panel will actually
     * do — including the surprise that a lone `half` takes the full width.
     */
    val rows: List<List<DashItem>>
        get() {
            val out = mutableListOf<List<DashItem>>()
            var i = 0
            while (i < items.size) {
                if (items[i].half && i + 1 < items.size && items[i + 1].half) {
                    out.add(listOf(items[i], items[i + 1]))
                    i += 2
                } else {
                    out.add(listOf(items[i]))
                    i += 1
                }
            }
            return out
        }

    /** Two layouts are the same when the bytes the device would store are. */
    override fun equals(other: Any?): Boolean =
        other is DashLayout && configText == other.configText

    override fun hashCode(): Int = configText.hashCode()

    companion object {
        /**
         * At most 12, matching DASH_MAX_ITEMS — the device silently stops parsing
         * past that, so the editor must not let a rider build a layout whose tail
         * would vanish without explanation.
         */
        const val MAX_ITEMS = 12

        /**
         * Parse the device's config text. Deliberately as forgiving as the
         * firmware's dashParse: unknown tokens are skipped, not fatal, so a
         * hand-edited file with one typo still opens in the editor.
         */
        fun parse(text: String): DashLayout {
            val out = mutableListOf<DashItem>()
            for (rawLine in text.split("\n")) {
                val line = rawLine.substringBefore('#')
                val tok = line.split(' ', '\t', '\r').filter { it.isNotEmpty() }
                val first = tok.firstOrNull() ?: continue
                if (DashField.named(first) == null) continue
                val size = tok.getOrNull(1)?.let { DashSize.fromToken(it) } ?: DashSize.MEDIUM
                val half = tok.drop(1).any { it == "half" }
                out.add(DashItem(first, size, half))
                if (out.size >= MAX_ITEMS) break
            }
            return DashLayout(out)
        }

        /** The device's built-in default, for the "reset" action. */
        val deviceDefault: DashLayout
            get() = DashLayout(
                listOf(
                    DashItem("power3s", DashSize.HERO, false),
                    DashItem("hr", DashSize.MEDIUM, true),
                    DashItem("cadence", DashSize.MEDIUM, true),
                    DashItem("ridetime", DashSize.MEDIUM, true),
                    DashItem("distance", DashSize.MEDIUM, true),
                ),
            )

        /**
         * The layout the tutorial's device arrives with — power up top with its
         * zone bar, then the four numbers the product shots show. It is the
         * device's own default with speed in place of cadence, because speed is
         * the field a newcomer looks for first and the one whose movement is
         * obvious without knowing what a good number is.
         */
        val tutorial: DashLayout
            get() = DashLayout(
                listOf(
                    DashItem("power3s", DashSize.HERO, false),
                    DashItem("hr", DashSize.MEDIUM, true),
                    DashItem("speed", DashSize.MEDIUM, true),
                    DashItem("ridetime", DashSize.MEDIUM, true),
                    DashItem("distance", DashSize.MEDIUM, true),
                ),
            )
    }
}

// MARK: - Pages

/**
 * The whole config: an ordered carousel of pages the device's Home key steps
 * through. A page is either a field layout, the MUSIC page (phone media
 * controls — its content comes over BLE, so it carries no items), or the map.
 * Mirrors DashPages in src/dash_layout.h and DashConfig in
 * companion-ios/Sources/DashLayout.swift; the `page` / `page music` separator
 * lines are the wire format, and text with no separators is exactly the old
 * one-page config.
 */
data class DashConfig(
    val pages: List<Page>,
    /** The map screen's 3-cell data strip (`map <f> <f> <f>` in the config). */
    val mapFields: List<String> = listOf("speed", "distance", "ridetime"),
) {
    enum class PageKind { FIELDS, MUSIC, MAP }

    data class Page(
        val kind: PageKind,
        val layout: DashLayout,
        /**
         * Stable identity across edits, so a carousel reorder animates the
         * SAME card rather than rebuilding neighbours — index-keyed cards made
         * a deletion reuse the wrong views on iOS, and Compose keys have the
         * identical failure mode.
         */
        val key: Long = nextKey++,
    ) {
        val isMusic: Boolean get() = kind == PageKind.MUSIC
        val isMap: Boolean get() = kind == PageKind.MAP

        companion object {
            private var nextKey = 1L
            fun fields(layout: DashLayout = DashLayout(emptyList())) =
                Page(PageKind.FIELDS, layout)
            fun music() = Page(PageKind.MUSIC, DashLayout(emptyList()))
            fun map() = Page(PageKind.MAP, DashLayout(emptyList()))
        }
    }

    /** Configurable (non-map) pages the device accepts; the map page rides
     * along on top of these (DASH_MAX_PAGES = 5 on the device). */
    val contentPages: Int get() = pages.count { !it.isMap }

    /** The first data page — what thumbnails show. */
    val firstFields: DashLayout? get() = pages.firstOrNull { it.kind == PageKind.FIELDS }?.layout

    val hasMusicPage: Boolean get() = pages.any { it.isMusic }

    /**
     * Serialize, byte-compatible with dashSerializePages(): the first field
     * page carries no `page` line, so a one-page config round-trips to the
     * pre-pages format. The header lines are byte-identical with
     * dash_layout.cpp's kHeader — `==` between the app's config and the
     * device's echo is a string comparison.
     */
    val configText: String
        get() = buildString {
            append("# OpenTrailPaper dashboard layout\n")
            append("# <field> <small|medium|large|hero> [half]; 'page' or 'page music' starts a new page\n")
            append("map ${mapFields[0]} ${mapFields[1]} ${mapFields[2]}\n")
            pages.forEachIndexed { i, page ->
                when {
                    page.isMusic -> append("page music\n")
                    page.isMap -> append("page map\n")
                    i > 0 -> append("page\n")
                }
                if (page.kind == PageKind.FIELDS) {
                    for (it in page.layout.items) {
                        append(it.field.padEnd(10))
                        append(' ')
                        append(it.size.token.padEnd(6))
                        if (it.half) append(" half")
                        append('\n')
                    }
                }
            }
        }

    /** Two configs are the same when the bytes the device would store are. */
    override fun equals(other: Any?): Boolean =
        other is DashConfig && configText == other.configText

    override fun hashCode(): Int = configText.hashCode()

    companion object {
        const val MAX_PAGES = 4

        /**
         * Parse the device's text: split into per-page chunks on `page` lines,
         * reusing DashLayout's forgiving field parsing for each chunk.
         */
        fun parse(text: String): DashConfig {
            val out = mutableListOf<Page>()
            var chunk = StringBuilder()
            var kind = PageKind.FIELDS
            var sawMap = false
            val strip = mutableListOf("speed", "distance", "ridetime")

            fun commit() {
                val layout = if (kind == PageKind.FIELDS) DashLayout.parse(chunk.toString())
                             else DashLayout(emptyList())
                var keep = kind != PageKind.FIELDS || layout.items.isNotEmpty()
                if (kind == PageKind.MAP) {
                    if (sawMap) keep = false else sawMap = true
                }
                if (keep) out.add(Page(kind, layout))
                chunk = StringBuilder()
                kind = PageKind.FIELDS
            }

            for (rawLine in text.split("\n")) {
                val line = rawLine.substringBefore('#')
                val tok = line.split(' ', '\t', '\r').filter { it.isNotEmpty() }
                when {
                    tok.firstOrNull() == "page" -> {
                        commit()
                        when (tok.getOrNull(1)) {
                            "music" -> kind = PageKind.MUSIC
                            "map" -> kind = PageKind.MAP
                        }
                    }
                    tok.firstOrNull() == "map" -> {
                        for (i in 0 until 3) {
                            val f = tok.getOrNull(i + 1) ?: continue
                            if (DashField.named(f) != null) strip[i] = f
                        }
                    }
                    else -> chunk.append(rawLine).append('\n')
                }
            }
            commit()
            // A pre-map-page config: the map belongs at the end, where the old
            // fixed cycle put it.
            if (!sawMap) out.add(Page.map())
            var pages = out.take(MAX_PAGES + 1)
            if (pages.none { it.isMap }) pages = pages + Page.map()
            return DashConfig(pages, strip)
        }

        val deviceDefault: DashConfig
            get() = DashConfig(listOf(Page.fields(DashLayout.deviceDefault), Page.map()))
    }
}
