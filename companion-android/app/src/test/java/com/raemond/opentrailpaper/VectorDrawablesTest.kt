package com.raemond.opentrailpaper

import androidx.compose.ui.graphics.vector.PathParser
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

class VectorDrawablesTest {

    // Compose's painterResource throws on a pathData it can't parse, and only
    // when the screen draws it: an SVG layer id ("Layer_1") left in a logo
    // crashed the Settings tab.
    @Test
    fun everyVectorPathParses() {
        val res = listOf(File("src/main/res"), File("app/src/main/res")).first { it.isDirectory }
        val paths = res.listFiles { f -> f.name.startsWith("drawable") }.orEmpty()
            .flatMap { it.listFiles { f -> f.extension == "xml" }.orEmpty().toList() }
            .flatMap { f -> Regex("pathData=\"([^\"]*)\"").findAll(f.readText()).map { f.name to it.groupValues[1] } }
        assertTrue("no vector paths found under $res", paths.isNotEmpty())
        for ((file, data) in paths) {
            try {
                PathParser().parsePathString(data)
            } catch (e: IllegalArgumentException) {
                throw AssertionError("$file: unparseable pathData \"${data.take(40)}\": ${e.message}")
            }
        }
    }
}
