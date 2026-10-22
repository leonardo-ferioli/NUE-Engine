// NUE engine for Android: the AAR.  Contents
//   * Java: org.nue.engine.* (NueActivity & co.) plus SDL3's own Java classes (taken from the SDL3 source tree)
//   * native: libSDL3.so and libnue_android.so for each ABI, 16 KB page aligned
//   * prefab packages "SDL3" and "nue_android" so a game module can find_package() them from its own CMake
plugins {
    id("com.android.library")
}

val sdl3Dir: String = providers.gradleProperty("nue.sdl3.dir").get()
val sdl3Root: File = rootProject.file(sdl3Dir).canonicalFile
val abis: List<String> = providers.gradleProperty("nue.abis").get().split(",").map { it.trim() }.filter { it.isNotEmpty() }

android {
    namespace = "org.nue.engine"
    compileSdk = 36
    ndkVersion = "28.2.13676358"

    defaultConfig {
        // Android 10: first release with the thermal API, multi-resume, scoped storage everywhere and Vulkan 1.1 on
        // virtually every GPU that ships with it; see docs/PLATFORM_ANDROID.md for the reasoning.
        minSdk = 29
        consumerProguardFiles("consumer-rules.pro")
        ndk {
            abiFilters += abis
        }
        externalNativeBuild {
            cmake {
                arguments += listOf(
                    "-DANDROID_STL=c++_static",
                    "-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON", // 16 KB page alignment on every NDK that knows the flag
                    "-DNUE_SDL3_SOURCE_DIR=${sdl3Root.absolutePath.replace('\\', '/')}"
                )
                cppFlags += listOf("-std=c++17")
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    sourceSets {
        getByName("main") {
            // SDL3's Java side (SDLActivity, SDLSurface, audio / HID / controller managers).
            java.srcDir(File(sdl3Root, "android-project/app/src/main/java"))
        }
    }

    buildFeatures {
        prefabPublishing = true
    }
    prefab {
        create("SDL3") {
            headers = File(sdl3Root, "include").absolutePath
            libraryName = "libSDL3"
        }
        create("nue_android") {
            headers = file("../../src/platform/android/include").absolutePath
            libraryName = "libnue_android"
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    packaging {
        jniLibs {
            useLegacyPackaging = false // libraries stay uncompressed and page aligned inside the APK
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false // the AAR is not shrunk; the app shrinks with consumer-rules.pro
        }
    }
}
