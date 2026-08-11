package com.raemond.opentrailpaper

import android.app.Application
import com.raemond.opentrailpaper.ble.BleManager
import com.raemond.opentrailpaper.data.Prefs
import com.raemond.opentrailpaper.map.TileCache
import org.osmdroid.config.Configuration

/**
 * Process-wide singletons.
 *
 * The BLE link outlives any one screen — a firmware update keeps running while
 * the rider moves between tabs, and the device reconnects on its own long after
 * the screen that started the scan is gone — so it hangs off the Application
 * rather than a ViewModel, exactly as the iOS `@StateObject` on the App does.
 */
class OpenTrailPaperApp : Application() {

    lateinit var ble: BleManager
        private set

    override fun onCreate() {
        super.onCreate()
        instance = this
        Prefs.init(this)
        TileCache.init(this)

        // osmdroid keeps its tile cache in app-private storage (no storage
        // permission) and identifies itself politely to the OSM tile servers,
        // which their usage policy requires of every client.
        Configuration.getInstance().apply {
            userAgentValue = "OpenTrailPaper-Android/${BuildConfig.VERSION_NAME}"
            osmdroidBasePath = cacheDir.resolve("osmdroid").apply { mkdirs() }
            osmdroidTileCache = osmdroidBasePath.resolve("tiles").apply { mkdirs() }
        }

        ble = BleManager(this)
    }

    companion object {
        lateinit var instance: OpenTrailPaperApp
            private set
    }
}
