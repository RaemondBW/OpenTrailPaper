package com.raemond.opentrailpaper.ui

import android.content.Context
import android.content.Intent
import androidx.core.content.FileProvider
import java.io.File

/**
 * Hands a downloaded ride or device log to whatever else is installed — Strava,
 * Files, mail — through the system chooser. The iOS side is a `ShareLink`; here
 * it is a FileProvider URI, because a raw `file://` has been unusable across app
 * boundaries since Android 7.
 */
object Share {
    fun file(context: Context, file: File, mimeType: String) {
        val uri = FileProvider.getUriForFile(
            context,
            "${context.packageName}.fileprovider",
            file,
        )
        val intent = Intent(Intent.ACTION_SEND).apply {
            type = mimeType
            putExtra(Intent.EXTRA_STREAM, uri)
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        }
        context.startActivity(Intent.createChooser(intent, null))
    }

    fun fit(context: Context, file: File) = file(context, file, "application/vnd.ant.fit")

    fun log(context: Context, file: File) = file(context, file, "text/plain")
}
