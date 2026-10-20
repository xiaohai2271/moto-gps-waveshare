plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    id("org.jetbrains.kotlin.plugin.compose")
}

import java.util.Properties

// Release signing material lives outside the repository. Provide
// platforms/android/signing.properties (gitignored) or these env vars:
// MOTO_GPS_KEYSTORE, MOTO_GPS_KEYSTORE_PASSWORD, MOTO_GPS_KEY_ALIAS,
// MOTO_GPS_KEY_PASSWORD.
val signingPropertiesFile = rootProject.file("signing.properties")
val signingProperties = Properties().apply {
    if (signingPropertiesFile.exists()) {
        signingPropertiesFile.inputStream().use { load(it) }
    }
}
fun signingValue(key: String, env: String): String? =
    signingProperties.getProperty(key) ?: System.getenv(env)

android {
    namespace = "dev.glimpse.motogps"
    compileSdk = 36
    buildToolsVersion = "36.0.0"

    defaultConfig {
        applicationId = "dev.glimpse.motogps"
        minSdk = 26
        targetSdk = 36
        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
        versionCode = 7
        versionName = "0.1.0-alpha7"

        ndkVersion = "27.2.12479018"
        ndk {
            // armeabi-v7a users may need to build locally; release targets device families actually available.
            abiFilters += listOf("arm64-v8a", "x86_64")
        }
        externalNativeBuild {
            cmake {
                arguments += listOf("-DANDROID_STL=c++_shared")
                cppFlags += "-std=c++17"
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
            val keystorePath = signingValue("storeFile", "MOTO_GPS_KEYSTORE")
            if (keystorePath != null) {
                signingConfig = signingConfigs.create("release") {
                    storeFile = file(keystorePath)
                    storePassword = signingValue("storePassword", "MOTO_GPS_KEYSTORE_PASSWORD")
                    keyAlias = signingValue("keyAlias", "MOTO_GPS_KEY_ALIAS")
                    keyPassword = signingValue("keyPassword", "MOTO_GPS_KEY_PASSWORD")
                }
            }
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
        buildConfig = true
    }
    packaging {
        resources.excludes += "META-INF/*"
    }
    testOptions {
        unitTests.isIncludeAndroidResources = false
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.lifecycle:lifecycle-runtime-ktx:2.8.7")
    implementation("androidx.lifecycle:lifecycle-viewmodel-compose:2.8.7")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.9.0")
    implementation("androidx.activity:activity-compose:1.9.3")
    implementation(platform("androidx.compose:compose-bom:2024.10.01"))
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.material3:material3")
    testImplementation("junit:junit:4.13.2")
    testImplementation("org.json:json:20240303")
    androidTestImplementation("androidx.test.ext:junit:1.2.1")
    androidTestImplementation("androidx.test:runner:1.6.2")
}
