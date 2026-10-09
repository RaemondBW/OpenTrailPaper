package com.raemond.opentrailpaper.map

import com.google.gson.stream.JsonReader
import com.google.gson.stream.JsonToken
import java.io.InputStream
import java.io.InputStreamReader

/**
 * An Overpass response, parsed ONCE into the few things the encoders need.
 *
 * The iOS builder decodes the same JSON four times over — for roads, water,
 * parks and the coastline — which is affordable there and is not here: an
 * Overpass region for one batch of hexes runs to tens of megabytes, and holding
 * a decoded object graph of it four times is how a phone gets killed mid-download.
 * So the payload is streamed straight off the socket into primitive arrays and
 * every later pass reads those.
 *
 * Node coordinates live in parallel [DoubleArray]s with an id→index map rather
 * than a map of objects: a dense region is ~10^5 nodes, and one boxed pair each
 * is tens of megabytes of garbage for no benefit.
 */
class OsmData(
    private val nodeIndex: HashMap<Long, Int>,
    private val nodeLat: DoubleArray,
    private val nodeLon: DoubleArray,
    val ways: List<Way>,
    /** way id -> highest bike-route network level (1 local … 3 national). */
    val routeLevels: Map<Long, Int> = emptyMap(),
    /** Cycling POIs, in the order docs/mapgen.js collectPois produces. */
    val pois: List<Cycling.Poi> = emptyList(),
) {
    /**
     * One way, reduced to the questions the encoders ask of it. Tags are dropped
     * after classification — nothing downstream needs them, and keeping them is
     * the single biggest cost of holding a region in memory.
     */
    class Way(
        val nodes: LongArray,
        /** Road render class 0–5, or -1 when this way is not a road. */
        val roadClass: Int,
        val isWater: Boolean,
        val isCoastline: Boolean,
        val isPark: Boolean,
        val id: Long = 0,
        /** Cycleway / bike-lane way flags from the way's own tags (no route level). */
        val baseFlags: Int = 0,
        /** Class to draw it as if it turns out to be a bike-route member, or -1. */
        val memberClass: Int = -1,
    ) {
        /** Render class once route membership is known: a route member the
         *  highway filter does not classify (service road …) still draws. */
        fun classWith(level: Int): Int = if (roadClass < 0 && level > 0) memberClass else roadClass
    }

    /** Resolves a way's node ids to coordinates, dropping any the response omitted. */
    fun coords(way: Way): List<DoubleArray> = coords(way.nodes)

    /** As above, for a node-id list assembled from several ways (coastline chains). */
    fun coords(nodes: LongArray): List<DoubleArray> {
        val out = ArrayList<DoubleArray>(nodes.size)
        for (id in nodes) {
            val i = nodeIndex[id] ?: continue
            out.add(doubleArrayOf(nodeLat[i], nodeLon[i]))
        }
        return out
    }

    companion object {
        // Road tiers (device render classes). primary/secondary/tertiary are
        // their own tiers so each can be styled + shed independently per zoom;
        // only motorway/trunk (arterial) survive at the widest zooms.
        private val ARTERIAL = setOf("motorway", "trunk")
        private val PRIMARY = setOf("primary")
        private val SECONDARY = setOf("secondary")
        private val TERTIARY = setOf("tertiary")
        private val MINOR = setOf("residential", "unclassified", "living_street", "pedestrian")
        private val PATH = setOf("cycleway", "footway", "path", "track", "steps")

        // Parks / green areas. Must agree with mapgen.js / build_map.py and the
        // Overpass query in MapBuilder.
        private val PARK_LANDUSE =
            setOf("grass", "forest", "meadow", "recreation_ground", "cemetery", "village_green")
        private val PARK_NATURAL = setOf("wood", "scrub", "grassland", "heath")

        /**
         * Rail/transit is dropped; natural=water is handled separately (WTR2), so
         * neither yields a road class here. Numbering must agree with
         * build_map.py / mapgen.js and the firmware MapFeatureClass enum.
         */
        fun classify(highway: String?, footway: String?): Int {
            val hw = highway ?: return -1
            if (hw == "footway" && (footway == "sidewalk" || footway == "crossing")) return -1
            val base = hw.substringBefore("_link")
            return when (base) {
                in ARTERIAL -> 0
                in PRIMARY -> 1
                in SECONDARY -> 2
                in TERTIARY -> 3
                in MINOR -> 4
                in PATH -> 5
                else -> -1
            }
        }

        private fun isPark(leisure: String?, landuse: String?, natural: String?): Boolean =
            leisure == "park" || landuse in PARK_LANDUSE || natural in PARK_NATURAL

        /** Stream-parses an Overpass `[out:json]` body. */
        fun parse(stream: InputStream): OsmData {
            val nodeIndex = HashMap<Long, Int>(1 shl 16)
            var lat = DoubleArray(1 shl 14)
            var lon = DoubleArray(1 shl 14)
            var nodeCount = 0
            val ways = ArrayList<Way>()
            val seenWays = HashSet<Long>()
            val routeLevels = HashMap<Long, Int>()
            // POIs: key "n<id>"/"w<id>"/"r<id>" -> entry, in insertion order
            // (LinkedHashMap keeps a re-put key in place, like a JS Map).
            val poiByKey = LinkedHashMap<String, Cycling.Poi>()
            val poiOutlines = ArrayList<Triple<String, Pair<Int, Int>, LongArray>>()

            JsonReader(InputStreamReader(stream, Charsets.UTF_8)).use { reader ->
                reader.beginObject()
                while (reader.hasNext()) {
                    if (reader.nextName() != "elements") {
                        reader.skipValue()
                        continue
                    }
                    reader.beginArray()
                    while (reader.hasNext()) {
                        var type = ""
                        var id = 0L
                        var elLat = Double.NaN
                        var elLon = Double.NaN
                        var nodes: LongArray? = null
                        var tags: HashMap<String, String>? = null
                        var centerLat = Double.NaN
                        var centerLon = Double.NaN

                        reader.beginObject()
                        while (reader.hasNext()) {
                            when (reader.nextName()) {
                                "type" -> type = reader.nextString()
                                "id" -> id = reader.nextLong()
                                "lat" -> elLat = reader.nextDouble()
                                "lon" -> elLon = reader.nextDouble()
                                "nodes" -> {
                                    val acc = ArrayList<Long>(32)
                                    reader.beginArray()
                                    while (reader.hasNext()) acc.add(reader.nextLong())
                                    reader.endArray()
                                    nodes = acc.toLongArray()
                                }

                                "tags" -> {
                                    // Every tag, briefly: the cycling flags and POIs
                                    // read a dozen keys. Dropped once the element is
                                    // classified, so the region never holds them.
                                    val t = HashMap<String, String>()
                                    reader.beginObject()
                                    while (reader.hasNext()) {
                                        val key = reader.nextName()
                                        if (reader.peek() != JsonToken.STRING) {
                                            reader.skipValue()
                                            continue
                                        }
                                        t[key] = reader.nextString()
                                    }
                                    reader.endObject()
                                    tags = t
                                }

                                "center" -> {             // `out center` (POI query)
                                    reader.beginObject()
                                    while (reader.hasNext()) {
                                        when (reader.nextName()) {
                                            "lat" -> centerLat = reader.nextDouble()
                                            "lon" -> centerLon = reader.nextDouble()
                                            else -> reader.skipValue()
                                        }
                                    }
                                    reader.endObject()
                                }

                                else -> reader.skipValue()
                            }
                        }
                        reader.endObject()

                        if (type == "node" && !elLat.isNaN() && !elLon.isNaN()) {
                            if (nodeCount == lat.size) {
                                lat = lat.copyOf(lat.size * 2)
                                lon = lon.copyOf(lon.size * 2)
                            }
                            lat[nodeCount] = elLat
                            lon[nodeCount] = elLon
                            nodeIndex[id] = nodeCount
                            nodeCount += 1
                            val poi = tags?.let { Cycling.poiOf(it) }
                            if (poi != null) {
                                poiByKey["n$id"] = Cycling.Poi(poi.first, poi.second, elLat, elLon)
                            }
                        } else if ((type == "way" || type == "relation") && tags != null &&
                            Cycling.poiOf(tags) != null
                        ) {
                            // A POI outline: never a map feature (mapgen.js skips
                            // it the same way), placed at the server's centre or
                            // else its vertex centroid once all nodes are known.
                            if (type == "way") seenWays.add(id)
                            val poi = Cycling.poiOf(tags)!!
                            val key = (if (type == "way") "w" else "r") + id
                            if (!centerLat.isNaN() && !centerLon.isNaN()) {
                                poiByKey[key] = Cycling.Poi(poi.first, poi.second, centerLat, centerLon)
                            } else if (type == "way" && nodes != null) {
                                poiOutlines.add(Triple(key, poi, nodes))
                            }
                        } else if (type == "way" && nodes != null) {
                            if (!seenWays.add(id)) continue   // arrived twice (route member)
                            val t = tags ?: emptyMap()
                            val natural = t["natural"]
                            val roadClass = classify(t["highway"], t["footway"])
                            val water = natural == "water"
                            val coast = natural == "coastline"
                            val park = isPark(t["leisure"], t["landuse"], natural)
                            val memberClass = Cycling.classifyRouteMember(t)
                            // A way that is none of these is every building,
                            // barrier and boundary Overpass hands back with the
                            // recursion — dropping them here is most of the
                            // memory saving. Route-member candidates stay until
                            // the `bikeroute` elements (sent last) say which are.
                            if (roadClass >= 0 || water || coast || park || memberClass >= 0) {
                                ways.add(
                                    Way(
                                        nodes, roadClass, water, coast, park,
                                        id = id,
                                        baseFlags = Cycling.wayFlags(t, 0),
                                        memberClass = memberClass,
                                    ),
                                )
                            }
                        } else if (type == "bikeroute") {
                            // Derived element: one per network level, ways = "id;id;…".
                            val t = tags
                            val lvl = (t?.get("level")?.toIntOrNull() ?: 0) and 3
                            val list = t?.get("ways")
                            if (!list.isNullOrEmpty()) {
                                for (part in list.split(';')) {
                                    val k = part.toLongOrNull() ?: continue
                                    if (k == 0L) continue
                                    if ((routeLevels[k] ?: 0) < lvl) routeLevels[k] = lvl
                                }
                            }
                        }
                    }
                    reader.endArray()
                }
                reader.endObject()
            }

            for ((key, poi, nids) in poiOutlines) {
                var la = 0.0
                var lo = 0.0
                var k = 0
                val ring = if (nids.size > 1 && nids.first() == nids.last()) {
                    nids.copyOf(nids.size - 1)
                } else nids
                for (nid in ring) {
                    val i = nodeIndex[nid] ?: continue
                    la += lat[i]; lo += lon[i]; k++
                }
                if (k > 0) poiByKey[key] = Cycling.Poi(poi.first, poi.second, la / k, lo / k)
            }
            return OsmData(nodeIndex, lat, lon, ways, routeLevels, poiByKey.values.toList())
        }
    }
}
