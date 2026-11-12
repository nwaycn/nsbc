pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}

dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        // WebRTC 预编译包（io.github.webrtc-sdk 发布在 Maven Central）
        mavenCentral()
    }
}

rootProject.name = "NsbcAndroid"
include(":nsbc-sdk")
include(":app")
