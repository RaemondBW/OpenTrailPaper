package com.raemond.opentrailpaper.data

import java.text.Normalizer
import java.util.Locale

/**
 * Fitting strings into the firmware's fixed `char[]` buffers.
 *
 * Both budgets here are byte budgets, not character budgets, and that is the
 * whole point. The device copies cue text with `snprintf` into a `char[48]`
 * (routes.h MANEUVER_TEXT) and route names into a `char[32]` (MAX_NAME); both
 * cut at a byte count with no idea that "Ł" takes two of them. Truncating on
 * this side, on codepoint boundaries, is what stops a half-character reaching a
 * panel that then draws a replacement glyph — or nothing.
 */
object DeviceText {

    /** routes.h MANEUVER_TEXT is 48; one byte belongs to the NUL. */
    const val MANEUVER_BUDGET = 47

    /** routes.h MAX_NAME is 32, less the NUL, less ".gpx". */
    private const val NAME_BUDGET = 31 - 4

    /**
     * A route filename the device can hold and an SD card can store.
     *
     * Accents are folded rather than stripped: `Char.isLetterOrDigit()` is
     * Unicode-aware, so filtering alone would keep "ś" and quietly spend two of
     * the 31 bytes on it — and leave a FAT filesystem holding a name it may not
     * round-trip.
     */
    fun routeFileName(title: String?): String {
        val folded = Normalizer.normalize(title ?: "", Normalizer.Form.NFD)
            .replace(Regex("\\p{Mn}+"), "")
            .lowercase(Locale.US)
        val safe = buildString {
            for (c in folded) {
                if (c in 'a'..'z' || c in '0'..'9') append(c)
                else if (isNotEmpty() && last() != '_') append('_')
            }
        }.trim('_')
        val base = safe.take(NAME_BUDGET).trim('_').ifEmpty { "route" }
        return "$base.gpx"
    }

    /** A turn cue trimmed to what the device's maneuver buffer will hold. */
    fun maneuverText(text: String): String = fitUtf8(text, MANEUVER_BUDGET)

    /**
     * The longest prefix of [text] that encodes to at most [maxBytes] of UTF-8,
     * cut at a space where one is close to the end so a road name is shortened
     * rather than a word left in pieces.
     */
    fun fitUtf8(text: String, maxBytes: Int): String {
        if (text.toByteArray(Charsets.UTF_8).size <= maxBytes) return text

        var end = 0
        var used = 0
        while (end < text.length) {
            val cp = text.codePointAt(end)
            val width = String(Character.toChars(cp)).toByteArray(Charsets.UTF_8).size
            if (used + width > maxBytes) break
            used += width
            end += Character.charCount(cp)
        }
        val cut = text.substring(0, end)
        // Only honour a word boundary that isn't throwing most of the cue away.
        val space = cut.lastIndexOf(' ')
        return (if (space > cut.length / 2) cut.substring(0, space) else cut).trimEnd()
    }
}
