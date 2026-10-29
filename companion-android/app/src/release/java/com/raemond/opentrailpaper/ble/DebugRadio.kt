package com.raemond.opentrailpaper.ble

import android.app.Application

/** Release builds always use Bluetooth; the simulator radio is debug-only (src/debug). */
object DebugRadio {
    @Suppress("UNUSED_PARAMETER")
    fun create(app: Application): Radio? = null
}
