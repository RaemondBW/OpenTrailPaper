package com.raemond.opentrailpaper.data

import com.google.gson.JsonElement
import com.google.gson.JsonObject
import com.google.gson.JsonParser

/*
 * The saved-route lists of Strava and RideWithGPS, as the route picker shows
 * them. Parsing only, kept free of android.* so it runs in JVM unit tests
 * (CloudRoutesTest); the requests are SyncAccounts.routes / routeGpx.
 */

/** One route in a provider's list. [id] is a string: Strava's route ids pass 2^53. */
data class CloudRoute(
    val id: String,
    val name: String,
    val distanceMeters: Double?,
    val elevationGainMeters: Double?,
)

/** One page of a list; [nextPage] is null when that was the last. */
data class CloudRoutePage(val routes: List<CloudRoute>, val nextPage: Int?)

object CloudRoutes {
    /** Strava's own default; a full page is the only sign there may be another. */
    const val STRAVA_PAGE_SIZE = 30

    /** GET /api/v3/athletes/{id}/routes?page=[page]&per_page=[STRAVA_PAGE_SIZE]: a bare array. */
    fun parseStrava(text: String, page: Int): CloudRoutePage {
        val arr = runCatching { JsonParser.parseString(text).asJsonArray }.getOrNull()
            ?: return CloudRoutePage(emptyList(), null)
        val routes = arr.mapNotNull { e ->
            val o = e.takeIf { it.isJsonObject }?.asJsonObject ?: return@mapNotNull null
            // id_str first: the numeric id is past what a double holds exactly.
            val id = o.str("id_str") ?: o.str("id") ?: return@mapNotNull null
            CloudRoute(id, o.str("name") ?: "Strava route", o.num("distance"), o.num("elevation_gain"))
        }
        return CloudRoutePage(routes, if (arr.size() >= STRAVA_PAGE_SIZE) page + 1 else null)
    }

    /** The sync service's GET /v1/rwgps/routes: {routes: [...], next_page}. */
    fun parseRwgps(text: String): CloudRoutePage {
        val o = runCatching { JsonParser.parseString(text).asJsonObject }.getOrNull()
            ?: return CloudRoutePage(emptyList(), null)
        val routes = o.getAsJsonArray("routes")?.mapNotNull { e ->
            val r = e.takeIf { it.isJsonObject }?.asJsonObject ?: return@mapNotNull null
            val id = r.str("id") ?: return@mapNotNull null
            CloudRoute(id, r.str("name") ?: "RideWithGPS route", r.num("distance"), r.num("elevation_gain"))
        }.orEmpty()
        val next = o.get("next_page")?.takeIf { it.isJsonPrimitive }?.asInt
        return CloudRoutePage(routes, next)
    }

    /** Strava reports what the rider granted; anything without read_all lists public routes only. */
    fun stravaSeesPrivateRoutes(scope: String?) =
        scope?.split(',')?.any { it.trim() == "read_all" } == true

    private fun JsonObject.prim(k: String): JsonElement? = get(k)?.takeIf { it.isJsonPrimitive }
    private fun JsonObject.str(k: String) = prim(k)?.asString?.trim()?.takeIf { it.isNotEmpty() }
    private fun JsonObject.num(k: String) = prim(k)?.takeIf { it.asJsonPrimitive.isNumber }?.asDouble
}
