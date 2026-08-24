package com.raemond.opentrailpaper.data

import android.content.Context
import java.io.File

/**
 * Workouts saved on the phone (filesDir/workouts). The phone is the library;
 * the device holds copies. Everything created or imported lands here FIRST,
 * so nothing depends on the BLE link being up. Port of LocalWorkouts in
 * companion-ios/Sources/WorkoutsView.swift.
 */
object LocalWorkouts {
    private fun dir(context: Context): File =
        File(context.filesDir, "workouts").apply { mkdirs() }

    fun list(context: Context): List<String> =
        (dir(context).list() ?: emptyArray())
            .filter {
                it.lowercase().endsWith(".mrc") || it.lowercase().endsWith(".erg")
            }
            .sorted()

    fun read(context: Context, name: String): String? =
        runCatching { File(dir(context), name).readText() }.getOrNull()

    fun save(context: Context, name: String, text: String) {
        runCatching { File(dir(context), name).writeText(text) }
    }

    fun delete(context: Context, name: String) {
        runCatching { File(dir(context), name).delete() }
    }
}
