// Sample APK: window + audio tone + multi-touch + gamepad + capability probe (logcat tag "NUE").
plugins {
    id("com.android.application")
}

val abis: List<String> = providers.gradleProperty("nue.abis").get().split(",").map { it.trim() }.filter { it.isNotEmpty() }

// Optional signing config for sideloading: scripts/make_keystore.ps1 writes keystore.properties (gitignored).
// Without it the release build is signed with the debug key, which is fine for installing with adb.
val keystoreProps = java.util.Properties().apply {
    val f = rootProject.file("keystore.properties")
    if (f.isFile) f.inputStream().use { load(it) }
}

android {
    namespace = "org.nue.sample"
    compileSdk = 36
    ndkVersion = "28.2.13676358"

    defaultConfig {
        applicationId = "org.nue.sample"
        // minSdk 29 (Android 10) - see docs/PLATFORM_ANDROID.md.  targetSdk 35 (Android 15): edge-to-edge is
        // enforced (the engine handles insets), 16 KB page size aware; raise to 36 when the engine was tested there.
        minSdk = 29
        targetSdk = 35
        versionCode = 1
        versionName = "0.1.0"
        ndk {
            abiFilters += abis
        }
        externalNativeBuild {
            cmake {
                val sdlInclude = rootProject.file(providers.gradleProperty("nue.sdl3.dir").get()).canonicalFile.resolve("include")
                arguments += listOf(
                    "-DANDROID_STL=c++_static",
                    "-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON",
                    "-DNUE_SDL3_INCLUDE=" + sdlInclude.absolutePath.replace("\\", "/")
                )
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    buildFeatures {
        prefab = true
    }

    signingConfigs {
        if (keystoreProps.containsKey("storeFile")) {
            create("sideload") {
                storeFile = rootProject.file(keystoreProps.getProperty("storeFile"))
                storePassword = keystoreProps.getProperty("storePassword")
                keyAlias = keystoreProps.getProperty("keyAlias")
                keyPassword = keystoreProps.getProperty("keyPassword")
            }
        }
    }

    buildTypes {
        debug {
            isDebuggable = true
        }
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
            signingConfig = signingConfigs.findByName("sideload") ?: signingConfigs.getByName("debug")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    packaging {
        jniLibs {
            useLegacyPackaging = false
        }
    }
}

dependencies {
    implementation(project(":nue-engine"))
}
