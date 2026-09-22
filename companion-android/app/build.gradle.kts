import java.util.Properties

plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.android)
    alias(libs.plugins.kotlin.compose)
}

// Release signing, kept OUT of the repository.
//
// keystore.properties (gitignored) holds four values; CI can supply the same
// four as environment variables instead. When neither is present the release
// build still runs and simply comes out unsigned — so a clone with no key can
// verify that a minified build compiles, which is most of what the release
// config is for day to day.
//
//   storeFile=/absolute/path/to/upload-keystore.jks
//   storePassword=…
//   keyAlias=upload
//   keyPassword=…
val keystoreProperties = Properties().apply {
    val f = rootProject.file("keystore.properties")
    if (f.exists()) f.inputStream().use { load(it) }
}

fun signingValue(key: String, env: String): String? =
    keystoreProperties.getProperty(key) ?: System.getenv(env)

val releaseStoreFile = signingValue("storeFile", "OTP_KEYSTORE_FILE")

// CARTO basemaps key, optional. With one the map is CARTO Voyager; without, the
// keyless Esri canvas (see map/MapStyle.kt). local.properties is gitignored,
// so a developer's key stays on their machine; CI supplies OTP_CARTO_KEY.
val cartoKey: String = Properties().apply {
    val f = rootProject.file("local.properties")
    if (f.exists()) f.inputStream().use { load(it) }
}.getProperty("carto.key") ?: System.getenv("OTP_CARTO_KEY") ?: ""
val hasReleaseKey = releaseStoreFile != null && file(releaseStoreFile).exists()

// Public address of the sync-auth service (cloud/sync-auth), the broker for
// Strava / RideWithGPS sign-in. A URL, not a secret; the secrets stay on the
// service. Empty = the Accounts card explains uploads are unavailable.
val localProps = Properties().apply {
    val f = rootProject.file("local.properties")
    if (f.exists()) f.inputStream().use { load(it) }
}
val syncUrl: String = (localProps.getProperty("sync.url") ?: System.getenv("OTP_SYNC_SERVICE_URL")
    ?: "https://sync.opentrailpaper.com").trim().trimEnd('/')
// Firebase App Check identifiers for the Android app (Firebase project
// `opentrailpaper`). Identifiers, not secrets: the API key is restricted in
// Google Cloud to the App Check and Installations APIs and to this package's
// signing certificates, and a token is only minted for an app that passes Play
// Integrity. Override per build with firebase.* in local.properties.
val firebaseAppId = localProps.getProperty("firebase.appId") ?: "1:357305860460:android:8ada00c3af7ac9ca073349"
val firebaseApiKey = localProps.getProperty("firebase.apiKey") ?: "AIzaSyCPFkwR9pn8raJ-wLr8GrCgF9eRCEdgYtI"
val firebaseProjectId = localProps.getProperty("firebase.projectId") ?: "opentrailpaper"
val firebaseSenderId = localProps.getProperty("firebase.senderId") ?: "357305860460"

android {
    namespace = "com.raemond.opentrailpaper"
    compileSdk = 35
    ndkVersion = "26.3.11579264"

    defaultConfig {
        applicationId = "com.raemond.opentrailpaper"
        minSdk = 26
        targetSdk = 35
        // Kept in step with companion-ios/project.yml (MARKETING_VERSION /
        // CURRENT_PROJECT_VERSION), so the two companions read as one release.
        versionCode = 10
        versionName = "0.3"

        buildConfigField("String", "CARTO_KEY", "\"${cartoKey.trim()}\"")
        buildConfigField("String", "SYNC_SERVICE_URL", "\"$syncUrl\"")
        buildConfigField("String", "FIREBASE_APP_ID", "\"$firebaseAppId\"")
        buildConfigField("String", "FIREBASE_API_KEY", "\"$firebaseApiKey\"")
        buildConfigField("String", "FIREBASE_PROJECT_ID", "\"$firebaseProjectId\"")
        buildConfigField("String", "FIREBASE_SENDER_ID", "\"$firebaseSenderId\"")
        // App Check's debug provider in a RELEASE build, for a sideloaded test
        // install on a developer's own phone: Play Integrity only vouches for
        // apps Google Play installed, so a sideloaded release is refused by the
        // sync service. Build with `-Pappcheck.debug=true`, then register the
        // token the phone prints. Never for a published APK: the property
        // defaults to false and CI does not set it.
        buildConfigField("boolean", "APP_CHECK_DEBUG",
            (project.findProperty("appcheck.debug")?.toString() == "true").toString())
        // The App Link host for the sign-in return (AndroidManifest.xml).
        manifestPlaceholders["syncHost"] = syncUrl.removePrefix("https://").removePrefix("http://")

        externalNativeBuild {
            cmake {
                // H3 is third-party C; don't fail the build on its warnings.
                arguments += listOf("-DANDROID_STL=none")
                cFlags += listOf("-w", "-O2")
            }
        }
    }

    signingConfigs {
        if (hasReleaseKey) {
            create("release") {
                storeFile = file(releaseStoreFile!!)
                storePassword = signingValue("storePassword", "OTP_KEYSTORE_PASSWORD")
                keyAlias = signingValue("keyAlias", "OTP_KEY_ALIAS")
                keyPassword = signingValue("keyPassword", "OTP_KEY_PASSWORD")
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = true
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro",
            )
            // Absent on a clone with no key: the bundle builds unsigned rather
            // than the build failing, and Play rejects it at upload — which is a
            // far better place to find out than a tester's phone.
            if (hasReleaseKey) signingConfig = signingConfigs.getByName("release")
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }
    buildFeatures {
        compose = true
        // The app's own version string is the User-Agent it introduces itself
        // with to the OSM tile, routing and geocoding servers.
        buildConfig = true
    }
    packaging {
        resources {
            excludes += "/META-INF/{AL2.0,LGPL2.1}"
        }
    }
}

dependencies {
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.lifecycle.runtime.ktx)
    implementation(libs.androidx.lifecycle.runtime.compose)
    implementation(libs.androidx.lifecycle.viewmodel.compose)
    implementation(libs.androidx.activity.compose)
    implementation(platform(libs.androidx.compose.bom))
    implementation(libs.androidx.ui)
    implementation(libs.androidx.ui.graphics)
    implementation(libs.androidx.ui.tooling.preview)
    implementation(libs.androidx.material3)
    implementation(libs.androidx.material.icons.extended)
    implementation(libs.kotlinx.coroutines.android)
    implementation(libs.osmdroid.android)
    // Gson only for its streaming JsonReader — android.util's copy of the same
    // parser is a stub on the JVM, which would put the Overpass decoder (the
    // one piece where a mistake produces a silently wrong map tile) out of
    // reach of unit tests.
    implementation(libs.gson)
    // Mesh channel invites. ZXing's core is pure Java — it both draws the QR and
    // reads one out of a camera frame, so joining a channel needs no Play
    // Services and no key, the same reason the map is osmdroid and not Google's.
    implementation(libs.zxing.core)
    implementation(libs.androidx.camera.core)
    implementation(libs.androidx.camera.camera2)
    implementation(libs.androidx.camera.lifecycle)
    implementation(libs.androidx.camera.view)
    // Strava / RideWithGPS sign-in: the consent page opens in a Custom Tab and
    // the user's tokens live in EncryptedSharedPreferences (data/SyncAccounts.kt).
    implementation(libs.androidx.browser)
    implementation(libs.androidx.security.crypto)
    // Firebase App Check: proves to the sync service that a call comes from
    // this app (Play Integrity on a real install, a registered debug token on
    // an emulator / debug build). Only App Check is linked, no google-services
    // plugin: Firebase is configured in code from BuildConfig.
    implementation(platform(libs.firebase.bom))
    implementation(libs.firebase.appcheck.playintegrity)
    // Linked in every variant (a release build must still compile the code
    // that names it); it is only *installed* when BuildConfig.DEBUG is true.
    implementation(libs.firebase.appcheck.debug)
    implementation(libs.kotlinx.coroutines.play.services)
    // Not used directly. camera-view drags in appcompat 1.1.0 and with it
    // androidx.fragment 1.1.0, and androidx.activity's release lint (fatal
    // since the App Check / Custom Tabs artifacts raised it) refuses
    // registerForActivityResult next to a fragment older than 1.3.0.
    implementation(libs.androidx.fragment)
    debugImplementation(libs.androidx.ui.tooling)
    // The byte formats shared with the firmware are pure Kotlin, so they are
    // testable on the JVM with no device and no network.
    testImplementation("junit:junit:4.13.2")
}
