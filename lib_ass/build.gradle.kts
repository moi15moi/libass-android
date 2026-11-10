import org.jetbrains.kotlin.gradle.dsl.JvmTarget
import com.vanniktech.maven.publish.AndroidSingleVariantLibrary

plugins {
    alias(libs.plugins.android.library)
    alias(libs.plugins.maven.publish)
}

android {
    namespace = "io.github.peerless2012.ass"
    compileSdk = 36
    ndkVersion = libs.versions.ndkVersion.get()

    defaultConfig {
        minSdk = libs.versions.minSdk.get().toInt()
        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
        externalNativeBuild {
            cmake {
                arguments += listOf("-DANDROID_STL=c++_shared")
            }
        }
    }

    buildFeatures {
        prefabPublishing = true
    }

    buildTypes {
        debug {
            externalNativeBuild {
                cmake {
                    cppFlags("-fno-omit-frame-pointer")
                }
            }
        }
        release {
            externalNativeBuild {
                cmake {
                    // AGP defaults to RelWithDebInfo for release variants
                    arguments += listOf("-DCMAKE_BUILD_TYPE=Release")
                }
            }
            isMinifyEnabled = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            // TODO set cmake version to 4.2.0 or higher
        }
    }

    prefab {
        create("ass") {
            headers = "src/main/cpp/build_native_lib/include"
        }
    }
}

// Used by build_libass_android.py to install the NDK (resolving the NDK directory
// makes the Android Gradle Plugin download ndkVersion if it's missing).
tasks.register("installNdk") {
    val ndkDirectory = androidComponents.sdkComponents.ndkDirectory
    doLast {
        println("NDK: ${ndkDirectory.get().asFile}")
    }
}

kotlin {
    compilerOptions {
        jvmTarget = JvmTarget.JVM_11
    }
}

dependencies {
    testImplementation(libs.junit)
    androidTestImplementation(libs.androidx.junit)
    androidTestImplementation(libs.androidx.espresso.core)
}

mavenPublishing {
    publishToMavenCentral()
    // https://vanniktech.github.io/gradle-maven-publish-plugin/what/
    configure(AndroidSingleVariantLibrary())
    signAllPublications()
}