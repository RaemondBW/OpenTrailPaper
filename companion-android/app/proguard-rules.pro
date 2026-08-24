# R8 rules for the release build.
#
# build.gradle.kts has always pointed at this file; it did not exist, so every
# release build logged "Supplied proguard configuration does not exist" and
# minified on the defaults alone. Nothing had ever run a minified build, so
# nothing had ever found out what that cost.
#
# The defaults cover most of it — AndroidX, Compose, coroutines, CameraX and
# Gson all ship their own consumer rules inside their AARs, and R8 keeps every
# component named in AndroidManifest.xml. What follows is the part that is ours.

# ---------------------------------------------------------------------------
# H3, via JNI. The one rule this app genuinely cannot ship without.
# ---------------------------------------------------------------------------
#
# app/src/main/cpp/h3jni.c exports its entry points as
#   Java_com_raemond_opentrailpaper_map_H3Native_<name>
# because a JNI symbol IS the fully-qualified Java name. Renaming the class to
# `a.a.a` leaves the .so exporting symbols nothing looks for any more, and the
# first call to it dies with UnsatisfiedLinkError.
#
# The default config's -keepclasseswithmembernames covers this today, but this
# is not a thing to inherit silently: it is load-bearing, it is invisible until
# a map is drawn, and it would come back as "release build crashes on the Maps
# screen" long after whatever changed the defaults.
-keepclasseswithmembernames,includedescriptorclasses class * {
    native <methods>;
}
-keep class com.raemond.opentrailpaper.map.H3Native { *; }

# ---------------------------------------------------------------------------
# osmdroid
# ---------------------------------------------------------------------------
#
# It reads and writes its own preferences by field name (DefaultConfigurationProvider
# walks its fields to build the osmdroid preference keys), so its configuration
# classes cannot be renamed. Keeping the whole library costs ~200 KB of an app
# that is already several megabytes of vendored H3 and fonts, and buys not
# having to discover which corner of it reflects.
-keep class org.osmdroid.** { *; }
-dontwarn org.osmdroid.**

# ---------------------------------------------------------------------------
# The rest is quieting warnings for optional dependencies that are not present
# at runtime, which R8 reports as missing classes and would otherwise fail on.
# ---------------------------------------------------------------------------

# Gson pulls in java.sql and sun.misc for types this app never uses — it is here
# only for its streaming JsonReader (see build.gradle.kts).
-dontwarn java.sql.**
-dontwarn sun.misc.**

# ZXing's core is shared with a Java SE build that references AWT and javax.
# The QR encoder and the QR reader used here touch none of it.
-dontwarn java.awt.**
-dontwarn javax.imageio.**

# Keep the line numbers in a crash report meaningful. Without this a tester's
# stack trace is a list of obfuscated frames with no line numbers, which is the
# opposite of what a closed test is for.
-keepattributes SourceFile,LineNumberTable
-renamesourcefileattribute SourceFile
