package com.raemond.opentrailpaper

import com.raemond.opentrailpaper.data.RideNames
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class RideNamesTest {
    private val file = "2026-10-11_0812.fit"
    private val default = "Oct 11, 08:12"

    @After fun clear() = RideNames.forget(file)

    @Test fun cleanedKeepsOneTrimmedLine() {
        assertEquals("Morning loop", RideNames.cleaned("  Morning   loop \n"))
        assertEquals("Gravel with Sam", RideNames.cleaned("Gravel\nwith\tSam"))
        assertNull(RideNames.cleaned(""))
        assertNull(RideNames.cleaned(" \n\t "))
    }

    @Test fun cleanedCapsLength() {
        val long = "a".repeat(RideNames.MAX_LENGTH + 20)
        assertEquals(RideNames.MAX_LENGTH, RideNames.cleaned(long)!!.length)
        // No trailing space left where the cut lands after a word.
        val words = "word ".repeat(40)
        assertEquals(RideNames.cleaned(words)!!.trimEnd(), RideNames.cleaned(words))
    }

    @Test fun renameIsRememberedAndUploadedUnder() {
        assertEquals(default, RideNames.name(file, default))
        assertEquals("Hill repeats", RideNames.rename(file, " Hill repeats ", default))
        assertEquals("Hill repeats", RideNames.name(file, default))
    }

    @Test fun blankOrDefaultFallsBackAndForgets() {
        RideNames.rename(file, "Hill repeats", default)
        assertEquals(default, RideNames.rename(file, "   ", default))
        assertNull(RideNames.names[file])

        RideNames.rename(file, "Hill repeats", default)
        assertEquals(default, RideNames.rename(file, default, default))
        assertNull(RideNames.names[file])
    }
}
