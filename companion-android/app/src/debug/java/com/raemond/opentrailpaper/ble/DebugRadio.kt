package com.raemond.opentrailpaper.ble

import android.app.Application
import android.content.Context
import android.util.Log
import java.io.File

/**
 * Debug builds: run the app against the firmware simulator instead of
 * Bluetooth when files/otp-sim exists (sim/ANDROID.md):
 *
 *     adb shell run-as com.raemond.opentrailpaper sh -c 'echo 10.0.2.2:7878 > files/otp-sim'
 *
 * (10.0.2.2 is the host from an emulator.) A second word `forget` clears the
 * sim phone's bond store first — a phone that forgot the device. Delete the
 * file to go back to Bluetooth. Release builds have a stub that returns null.
 */
object DebugRadio {
    fun create(app: Application): Radio? {
        val f = File(app.filesDir, "otp-sim")
        if (!f.exists()) return null
        val words = f.readText().trim().split(Regex("\\s+"))
        val hostPort = words.firstOrNull()?.takeIf { it.isNotEmpty() } ?: return null
        val host = hostPort.substringBeforeLast(':', "10.0.2.2")
        val port = hostPort.substringAfterLast(':').toIntOrNull() ?: 7878
        val store = app.getSharedPreferences("otp_sim_radio", Context.MODE_PRIVATE)
        val radio = SimRadio(
            host, port, store,
            SimRadio.Options(autoConnectFailMs = 10_000, log = { Log.i("SimRadio", it) }),
        )
        if ("forget" in words.drop(1)) radio.forgetBonds()
        Log.w("SimRadio", "using the firmware sim at $host:$port instead of Bluetooth")
        return radio
    }
}
