plugins {
    id("com.android.library")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.nway.nsbc.sdk"
    compileSdk = 34

    defaultConfig {
        minSdk = 24
        consumerProguardFiles("consumer-rules.pro")
    }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }

    // 允许访问 WebRTC 的 .so
    packaging {
        jniLibs {
            useLegacyPackaging = true
        }
    }
}

dependencies {
    // SIP over WebSocket（RFC 7118）传输
    api("com.squareup.okhttp3:okhttp:4.12.0")
    // WebRTC 预编译（含 libwebrtc.so，支持 DTLS-SRTP / ICE / AEC）
    api("io.github.webrtc-sdk:android:125.6422.07")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.8.1")
}
