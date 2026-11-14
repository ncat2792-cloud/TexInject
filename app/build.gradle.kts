import java.util.Properties

plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.android)
    alias(libs.plugins.compose.compiler)
}

// 正式签名是否可用：keystore 文件存在且密码不为空。
// 任一项缺失就走 debug 签名，避免整包构建失败。
fun canSignRelease(): Boolean {
    val store = rootProject.file(System.getenv("KEYSTORE_PATH") ?: "keystore/release.jks")
    val pwd = System.getenv("KEYSTORE_PASSWORD") ?: ""
    // 必须同时满足：文件存在、且不是空文件、密码非空。
    //
    // 只判 isFile 不够：CI 上用 `base64 -d` 解未配置的 secret 会留下一个
    // 0 字节的 release.jks，于是这里误判为「有正式签名」，一路走到 package
    // 阶段才炸在 KeytoolException: Tag number over 30 is not supported。
    return store.isFile && store.length() > 0L && pwd.isNotEmpty()
}

android {
    namespace = "com.kael.texinject"
    compileSdk = 36
    ndkVersion = "27.0.12077973"

    defaultConfig {
        applicationId = "com.kael.texinject"
        minSdk = 33
        targetSdk = 36
        versionCode = 160
        versionName = "1.6.0"

        ndk {
            abiFilters += "arm64-v8a"
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    val localProps = Properties()
    val localPropsFile = rootProject.file("local.properties")
    if (localPropsFile.exists()) {
        localPropsFile.inputStream().use { stream -> localProps.load(stream) }
    }
    fun secret(k: String, d: String): String {
        val e = System.getenv(k)
        if (e != null && e.isNotEmpty()) return e
        val p = localProps.getProperty(k)
        if (p != null && p.isNotEmpty()) return p
        return d
    }

    signingConfigs {
        create("release") {
            storeFile = rootProject.file(secret("KEYSTORE_PATH", "keystore/release.jks"))
            storePassword = secret("KEYSTORE_PASSWORD", "")
            keyAlias = secret("KEY_ALIAS", "texinject")
            keyPassword = secret("KEY_PASSWORD", "")
        }
    }

    buildTypes {
        release {

            isMinifyEnabled = true
            isShrinkResources = true
            // 有正式密钥就用正式签名；没有就退回 debug 签名。
            //
            // 缺密钥时 Release 会在 package 阶段直接失败（keystore 读不了），
            // 整个包都拿不到。debug 签名照样能装、能测，只是不能覆盖已安装
            // 的正式版。CI 上没配 secrets 时走这条路。
            signingConfig = if (canSignRelease()) {
                signingConfigs.getByName("release")
            } else {
                signingConfigs.getByName("debug")
            }
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
        }
    }

    lint {
        checkReleaseBuilds = false
        abortOnError = false
        checkDependencies = false
    }

    buildFeatures {
        compose = true

        buildConfig = true
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }
    kotlinOptions {
        jvmTarget = "11"
    }
}

dependencies {

    compileOnly(files("libs/libxposed-api-102.jar"))

    implementation(platform(libs.androidx.compose.bom))
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.activity.compose)
    implementation(libs.androidx.compose.ui)
    implementation(libs.androidx.compose.ui.graphics)
    implementation(libs.androidx.compose.foundation)
    implementation(libs.androidx.compose.material3)
    implementation(libs.androidx.compose.ui.tooling.preview)
    implementation(libs.androidx.lifecycle.runtime.ktx)
    implementation(libs.androidx.lifecycle.viewmodel.ktx)
    implementation(libs.androidx.lifecycle.runtime.compose)
    implementation(libs.androidx.savedstate.ktx)
    debugImplementation("androidx.compose.ui:ui-tooling")
}
