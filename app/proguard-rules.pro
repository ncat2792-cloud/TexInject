# TexInject —— R8 规则
#
# 这是 LSPosed 模块，有两个"按字符串找类"的硬依赖，改名即失效：
#   1. META-INF/xposed/java_init.list  -> com.kael.texinject.TexInjectHookInit
#   2. C++ 里 FindClass("com/kael/texinject/nativecore/NativeCore") + GetMethodID
# 所以下面必须 -keep 类名。

# ---------- 1. Xposed 入口（java_init.list 按名字加载） ----------
-keep class com.kael.texinject.TexInjectHookInit { *; }
-keepclassmembers class com.kael.texinject.TexInjectHookInit {
    public void initZygote(...);
    public void handleLoadPackage(...);
}

# ---------- 2. JNI 反射查找的类：类名 + 方法名都不能动 ----------
-keep class com.kael.texinject.nativecore.NativeCore { *; }

# 建筑工具的 JNI 类。JNI_OnLoad 用 FindClass + RegisterNatives 按名字绑定：
#   com/vdl/kong520/NativeCore   地图界面关闭请求等 7 个入口
#   com/vdl/kong520/TpModule     导入/投影/打印/导出 54 个入口
#
# 必须整类保留。只写 native <methods> 不够：这些方法在 Java 侧没有调用者，
# R8 会把它们连同类一起判定为不可达删掉，RegisterNatives 随即报
# "Failed to register native method ... in Anonymous-DexFile"，
# 运行期一调就是 UnsatisfiedLinkError。
-keep class com.vdl.kong520.NativeCore { *; }
-keep class com.vdl.kong520.TpModule { *; }

# 所有 native 方法：保类名、方法名，且不允许被删除。
#
# 原来的 -keepclasseswithmembernames 只保名字，不阻止 R8 把「没有 Java
# 调用者」的 native 方法整个删掉 —— 而 JNI 恰恰只从 native 侧调用它们。
-keepclasseswithmembers,allowshrinking,includedescriptorclasses class * {
    native <methods>;
}
-keepclassmembers,includedescriptorclasses class * {
    native <methods>;
}

# ---------- 3. 本模块类保名（LSPosed / 日志 / 反射字符串引用） ----------
# 只保名称，方法体仍参与压缩与优化，混淆第三方库不受影响
-keepnames class com.kael.texinject.** { *; }
-keepnames interface com.kael.texinject.** { *; }
-keepnames enum com.kael.texinject.** { *; }

# ---------- 4. 属性 ----------
-keepattributes Signature, InnerClasses, EnclosingMethod
-keepattributes *Annotation*, RuntimeVisibleAnnotations
-keepattributes SourceFile, LineNumberTable

# ---------- 5. Compose / Kotlin ----------
-dontwarn org.jetbrains.annotations.**
-dontwarn kotlinx.**
-keep class androidx.compose.runtime.** { *; }
-dontwarn androidx.compose.**

# 开发者模式（DevBounds）反射扫布局树：类名可以混淆，方法名不行。
#   AndroidComposeView.getRoot()      -> 根 LayoutNode
#   LayoutNode.getChildren$ui_release -> 子节点
# 只保成员名、允许压缩删除其它未用方法，别整类 -keep（会拖慢 Compose）。
-keepclassmembers class androidx.compose.ui.platform.AndroidComposeView {
    public <methods>;
}
-keepclassmembers class androidx.compose.ui.node.LayoutNode {
    public <methods>;
}

# ---------- 6. LibXposed（compileOnly，运行时由框架提供） ----------
-dontwarn io.github.libxposed.**
-keep class io.github.libxposed.** { *; }

# ---------- 7. 反射用到的 Android 内部 API ----------
-dontwarn android.view.SurfaceControl
-dontwarn android.view.SurfaceControl$Transaction

# ---------- 8. ViewTree*Owner：ComposeHost 靠反射挂到 PopupWindow 上 ----------
# 这些类用 Class.forName(...) + 方法名 "set" 反射调用，改名即失效。
# 失效后 ComposeView attach 到 PopupWindow 会抛：
#   IllegalStateException: ViewTreeLifecycleOwner not found from PopupWindow$PopupDecorView
-keep class androidx.lifecycle.ViewTreeLifecycleOwner { *; }
-keep class androidx.lifecycle.ViewTreeViewModelStoreOwner { *; }
-keep class androidx.savedstate.ViewTreeSavedStateRegistryOwner { *; }
-keepclassmembers class androidx.lifecycle.** {
    public static void set(android.view.View, **);
}
-keepnames class androidx.lifecycle.**
-keepnames class androidx.savedstate.**
