package com.raemond.opentrailpaper.ui

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.raemond.opentrailpaper.data.CloudRoute
import com.raemond.opentrailpaper.data.CloudRoutes
import com.raemond.opentrailpaper.data.RouteImport
import com.raemond.opentrailpaper.data.SyncAccounts
import com.raemond.opentrailpaper.data.Units
import kotlinx.coroutines.launch
import java.util.Locale

/**
 * The rider's saved routes on Strava or RideWithGPS. Picking one downloads its
 * GPX and hands it to [RouteImport], so from there on it is a file import:
 * same parse, same preview, same upload to the device.
 */
@Composable
fun CloudRoutesSheet(provider: SyncAccounts.Provider, useMiles: Boolean, onDismiss: () -> Unit) {
    val scope = rememberCoroutineScope()
    val routes = remember { mutableStateListOf<CloudRoute>() }
    var nextPage by remember { mutableStateOf<Int?>(1) }
    var loading by remember { mutableStateOf(false) }
    var loadedOnce by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf<String?>(null) }
    var importing by remember { mutableStateOf<String?>(null) }

    fun loadMore() {
        val page = nextPage ?: return
        if (loading) return
        loading = true
        error = null
        scope.launch {
            try {
                val got = SyncAccounts.routes(provider, page)
                routes.addAll(got.routes.filter { r -> routes.none { it.id == r.id } })
                nextPage = got.nextPage
            } catch (e: Exception) {
                error = e.message ?: "Couldn't load your ${provider.title} routes"
            } finally {
                loading = false
                loadedOnce = true
            }
        }
    }

    LaunchedEffect(provider) { loadMore() }

    FullScreenSheet(title = "From ${provider.title}", onDismiss = onDismiss) {
        LazyColumn(Modifier.fillMaxSize()) {
            items(routes, key = { it.id }) { r ->
                Row(
                    Modifier
                        .fillMaxWidth()
                        .clickable(enabled = importing == null) {
                            importing = r.id
                            error = null
                            scope.launch {
                                try {
                                    val gpx = SyncAccounts.routeGpx(provider, r)
                                    // A GPX that will not parse is reported by
                                    // RouteImport on the Route screen, like a bad file.
                                    RouteImport.importGpx(gpx, r.name)
                                    onDismiss()
                                } catch (e: Exception) {
                                    error = e.message ?: "Couldn't download that route"
                                } finally {
                                    importing = null
                                }
                            }
                        }
                        .padding(horizontal = 20.dp, vertical = 14.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Column(Modifier.weight(1f)) {
                        Text(r.name, style = TypeScale.body, color = Palette.ink, maxLines = 1)
                        summary(r, useMiles)?.let {
                            Text(it, style = barlow(12.sp), color = Palette.muted)
                        }
                    }
                    if (importing == r.id) {
                        CircularProgressIndicator(Modifier.size(18.dp), color = Palette.accent, strokeWidth = 2.dp)
                    }
                }
                HorizontalDivider(color = Palette.hairline)
            }

            item {
                Column(
                    Modifier.fillMaxWidth().padding(20.dp),
                    horizontalAlignment = Alignment.CenterHorizontally,
                    verticalArrangement = Arrangement.spacedBy(10.dp),
                ) {
                    when {
                        loading -> CircularProgressIndicator(color = Palette.accent)
                        nextPage != null && loadedOnce -> TextButton(onClick = { loadMore() }) {
                            Text(
                                if (error != null && routes.isEmpty()) "Try again" else "Load more",
                                style = barlow(14.sp, FontWeight.SemiBold),
                                color = Palette.accent,
                            )
                        }
                        loadedOnce && routes.isEmpty() && error == null -> Text(
                            "No saved routes on ${provider.title} yet.",
                            style = TypeScale.body, color = Palette.muted, textAlign = TextAlign.Center,
                        )
                    }
                    error?.let {
                        Text(it, style = barlow(13.sp), color = Palette.accent, textAlign = TextAlign.Center)
                    }
                    if (provider == SyncAccounts.Provider.STRAVA &&
                        !CloudRoutes.stravaSeesPrivateRoutes(SyncAccounts.tokens[provider]?.scope)
                    ) {
                        Text(
                            "Only your public Strava routes are listed. To include private ones, " +
                                "disconnect and connect Strava again in Settings > Accounts.",
                            style = barlow(12.sp), color = Palette.faint, textAlign = TextAlign.Center,
                        )
                    }
                }
            }
        }
    }
}

/** "52.3 km · 812 m up", from whatever the provider reported. */
private fun summary(r: CloudRoute, useMiles: Boolean): String? {
    val parts = buildList {
        r.distanceMeters?.let {
            add(String.format(Locale.US, "%.1f %s", Units.distance(it / 1000, useMiles), Units.distLabel(useMiles)))
        }
        r.elevationGainMeters?.let {
            add(String.format(Locale.US, "%.0f %s up", Units.elevation(it, useMiles), Units.elevLabel(useMiles)))
        }
    }
    return parts.takeIf { it.isNotEmpty() }?.joinToString(" · ")
}
