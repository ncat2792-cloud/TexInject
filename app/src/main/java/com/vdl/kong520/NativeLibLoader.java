package com.vdl.kong520;

import android.text.TextUtils;
import android.util.Log;

/**
 * Loads the module-owned native library in an Xposed/LSPosed host process.
 *
 * The module ClassLoader, not the game's ClassLoader, owns the native library
 * path. Keep this deliberately small: it is the same resolution route used by
 * the known-good building-tools release.
 */
public final class NativeLibLoader {
    private static final String TAG = "NativeLibLoader";
    public static final String LIB_NAME = "texinject";
    public static final String FIND_LIB_METHOD = "findLibrary";
    private static volatile Throwable lastLoadError;

    private NativeLibLoader() {}

    public static boolean load(Class<?> moduleClass) {
        if (NativeCore.isNativeReady()) return true;
        lastLoadError = null;
        try {
            // Ask LSPosed's module ClassLoader for the installed module's
            // actual native-library path, then load that exact path.
            ClassLoader moduleLoader = moduleClass.getClassLoader();
            // findLibrary 是 protected：反射调用并 setAccessible。
            // 本模块走 LibXposed API，没有旧 de.robv 依赖。
            java.lang.reflect.Method findLibrary =
                    moduleLoader.getClass().getDeclaredMethod(FIND_LIB_METHOD, String.class);
            findLibrary.setAccessible(true);
            String libPath = (String) findLibrary.invoke(moduleLoader, LIB_NAME);
            if (TextUtils.isEmpty(libPath)) {
                lastLoadError = new UnsatisfiedLinkError(
                        "Module ClassLoader did not find " + System.mapLibraryName(LIB_NAME));
                Log.e(TAG, "Native library not found: " + LIB_NAME);
                return false;
            }
            if (NativeCore.loadNativePath(libPath)) {
                Log.d(TAG, "Native library loaded: " + libPath);
                return true;
            }
            lastLoadError = NativeCore.getNativeLoadError();
            Log.e(TAG, "Native library could not be loaded: " + libPath, lastLoadError);
        } catch (Throwable error) {
            lastLoadError = error;
            Log.e(TAG, "Native library lookup/load failed", error);
        }
        return false;
    }

    public static Throwable getLastLoadError() {
        return lastLoadError;
    }
}
