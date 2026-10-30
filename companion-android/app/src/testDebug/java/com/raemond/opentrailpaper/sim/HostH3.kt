package com.raemond.opentrailpaper.sim

import java.io.File

/**
 * The app's H3 JNI library (src/main/cpp: the vendored H3 C, h3shim.c and
 * h3jni.c — the code the APK carries) built for the test JVM, so code that
 * asks H3Tiles which cell a point is in (the Overpass POI pass) runs on the
 * JVM as on a phone. Built with the host's cc into sim/build/h3jni, which
 * app/build.gradle.kts puts on the unit tests' java.library.path: H3Native's
 * own System.loadLibrary("h3shim") loads it.
 *
 * A JVM loads a library into one class loader only, so one test class at
 * most (Robolectric's sandbox) may use it.
 */
object HostH3 {
    private val cpp: File? by lazy {
        TileCdn.root?.let { File(it, "companion-android/app/src/main/cpp") }?.takeIf { File(it, "h3jni.c").isFile }
    }

    /** Builds the library when it is missing or older than its sources; false
     *  when it cannot be (no JDK headers) or is not on the library path. */
    @Synchronized
    fun build(): Boolean {
        val src = cpp ?: return false
        val out = File(TileCdn.root, "sim/build/h3jni").apply { mkdirs() }
        val onPath = (System.getProperty("java.library.path") ?: "").split(File.pathSeparator)
            .any { runCatching { File(it).canonicalFile == out.canonicalFile }.getOrDefault(false) }
        if (!onPath) return false
        val lib = File(out, System.mapLibraryName("h3shim"))
        val sources = listOf(File(src, "h3shim.c"), File(src, "h3jni.c")) +
            (File(src, "h3/lib").listFiles { f -> f.name.endsWith(".c") }?.sorted() ?: emptyList())
        if (lib.isFile && sources.all { it.lastModified() <= lib.lastModified() }) return true
        val home = File(System.getProperty("java.home"))
        val include = listOf(File(home, "include"), File(home.parentFile, "include")).firstOrNull { File(it, "jni.h").isFile }
            ?: return false
        val platform = include.listFiles()?.firstOrNull { File(it, "jni_md.h").isFile } ?: return false
        val tmp = File(out, lib.name + ".tmp")
        val cmd = listOf(
            System.getenv("CC") ?: "cc", "-shared", "-fPIC", "-O2", "-w", "-std=c11", "-D_DEFAULT_SOURCE",
            "-I", src.path, "-I", File(src, "h3/include").path, "-I", include.path, "-I", platform.path,
        ) + sources.map { it.path } + listOf("-lm", "-o", tmp.path)
        val p = ProcessBuilder(cmd).redirectErrorStream(true).start()
        val log = p.inputStream.bufferedReader().readText()
        check(p.waitFor() == 0) { "building the host H3 library failed:\n$log" }
        check(tmp.renameTo(lib)) { "can't write $lib" }
        return true
    }
}
