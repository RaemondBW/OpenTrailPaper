plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.android)
    alias(libs.plugins.kotlin.compose)
}

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

        externalNativeBuild {
            cmake {
                // H3 is third-party C; don't fail the build on its warnings.
                arguments += listOf("-DANDROID_STL=none")
                cFlags += listOf("-w", "-O2")
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
    debugImplementation(libs.androidx.ui.tooling)
    // The byte formats shared with the firmware are pure Kotlin, so they are
    // testable on the JVM with no device and no network.
    testImplementation("junit:junit:4.13.2")
}
