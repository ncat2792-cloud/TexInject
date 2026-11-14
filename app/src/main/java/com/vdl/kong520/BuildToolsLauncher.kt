package com.vdl.kong520

import android.app.Activity
import android.widget.Toast
import com.vdl.kong520.NativeCore
import com.vdl.kong520.NativeLibLoader
import com.vdl.kong520.ui.BuildExportUi
import com.vdl.kong520.ui.BuildImportUi
import com.vdl.kong520.ui.BuildProjectionUi
import com.vdl.kong520.ui.PixelArtImportUi

/** Entry point for the building tools in the injected game process. */
object BuildToolsLauncher {
    enum class Tool { IMPORT, PIXEL_ART, PROJECTION, EXPORT }

    fun open(activity: Activity, tool: Tool) {
        openWhenReady(activity) { showTool(activity, tool) }
    }

    private fun openWhenReady(activity: Activity, show: () -> Unit) {
        if (activity.isFinishing || activity.isDestroyed) return
        Thread({
            var reason = "建筑工具原生运行库未能加载"
            val ready = try {
                val loaded = NativeLibLoader.load(NativeCore::class.java)
                if (!loaded) {
                    reason = "建筑工具原生运行库未能加载"
                    false
                } else {
                    // 地址解析要等 so 完全映射；失败时把原因带出来，比一句
                    // 「加载失败」有用得多。
                    var status = ""
                    var ok = false
                    for (attempt in 0 until 20) {
                        runCatching { NativeCore.ensureBuildToolsHooks() }
                        status = runCatching { NativeCore.addressResolutionStatus() }.getOrDefault("")
                        if (status.startsWith("已就绪")) { ok = true; break }
                        Thread.sleep(250)
                    }
                    if (!ok) reason = if (status.isBlank()) "建筑工具未就绪" else "建筑工具未就绪：$status"
                    ok
                }
            } catch (error: Throwable) {
                reason = error.message ?: "建筑工具初始化失败"
                false
            }
            activity.runOnUiThread {
                if (activity.isFinishing || activity.isDestroyed) return@runOnUiThread
                if (ready) show() else Toast.makeText(
                    activity.applicationContext, reason, Toast.LENGTH_LONG
                ).show()
            }
        }, "build-tools-open").start()
    }

    private fun showTool(activity: Activity, tool: Tool) {
        when (tool) {
            Tool.IMPORT -> BuildImportUi.show(activity, activity.applicationContext)
            Tool.PIXEL_ART -> PixelArtImportUi.show(activity, activity.applicationContext)
            Tool.PROJECTION -> BuildProjectionUi.show(activity, activity.applicationContext)
            Tool.EXPORT -> BuildExportUi.show(activity, activity.applicationContext)
        }
    }
}
