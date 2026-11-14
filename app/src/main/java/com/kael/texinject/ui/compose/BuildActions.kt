/*
 * Copyright (C) 2026 Devout9527
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * 建筑功能的调用层。
 *
 * 面板是 Compose，功能入口是 TpModule 的 native 方法；这里把两者接起来，
 * 并把 UI 侧的「一个文件」翻译成 native 需要的完整参数。
 *
 * 所有方法都在后台线程调用（见 BuildTab），返回值是可直接 toast 的中文结果。
 * 任何一步失败都给出具体原因，不静默吞掉 —— 点了按钮没反应是最糟的体验。
 */
package com.kael.texinject.ui.compose

import android.app.Activity
import com.kael.texinject.paths.BuildPaths
import com.vdl.kong520.NativeCore
import com.vdl.kong520.NativeLibLoader
import com.vdl.kong520.TpModule
import java.io.File
import java.util.UUID

object BuildActions {

    /**
     * 确保 native 库已加载、Python 桥就绪。
     *
     * 面板可能在游戏刚起来时就被打开，此时 libminecraftpe.so 还没映射完，
     * ensureBuildToolsHooks 会失败；这里等一小会儿再试。
     */
    private fun ensureReady(activity: Activity) {
        NativeLibLoader.load(NativeCore::class.java)
        if (!NativeCore.isNativeReady()) {
            throw IllegalStateException("建筑工具原生库未加载")
        }
        // 地址解析随 so 映射完成而成功，失败就带原因退出。
        var last = ""
        repeat(20) {
            runCatching { NativeCore.ensureBuildToolsHooks() }
            last = runCatching { NativeCore.addressResolutionStatus() }.getOrDefault("")
            if (last.startsWith("已就绪")) return
            Thread.sleep(250)
        }
        throw IllegalStateException(if (last.isBlank()) "建筑工具未就绪" else "建筑工具未就绪：$last")
    }

    private fun playerBlockPosition(): IntArray? =
        runCatching { TpModule.getBuildImportPlayerBlockPosition() }
            .getOrNull()
            ?.takeIf { it.size >= 3 }

    private fun worldId(): String? =
        runCatching { TpModule.getWorldId() }.getOrNull()?.takeIf { it.isNotBlank() }

    /** 导入：把结构文件铺到玩家当前位置。 */
    fun startImport(activity: Activity, file: File): String {
        ensureReady(activity)
        if (!file.isFile) return "文件不存在：${file.name}"

        val pos = playerBlockPosition() ?: return "读不到玩家位置，请先进入世界"
        val world = worldId() ?: return "读不到当前世界，请先进入世界"

        val jobId = UUID.randomUUID().toString()
        val spool = File(activity.filesDir, "build_import/jobs/$jobId")
        if (!spool.mkdirs() && !spool.isDirectory) return "无法创建导入工作目录"

        val accepted = TpModule.startBuildImport(
            file.absolutePath,
            spool.absolutePath,
            jobId,
            pos[0], pos[1], pos[2],
            /* blocksPerSecond = */ 4,
            /* clearExisting = */ false,
            /* placeDenyLayer = */ false,
            /* verifyAfterImport = */ true,
            /* verificationPrecision = */ 1,
            /* simulationChunkRange = */ 4,
            world,
            /* dimensionId = */ 0,
            /* suppressCommandFeedback = */ true
        )
        if (!accepted) {
            val status = runCatching { TpModule.getBuildImportStatus() }.getOrNull()
            return if (status.isNullOrBlank()) "导入未被接受" else "导入未被接受：$status"
        }
        return "已开始导入 ${file.name}"
    }

    /** 投影：加载成半透明预览，不实际放置方块。 */
    fun startProjection(activity: Activity, file: File): String {
        ensureReady(activity)
        if (!file.isFile) return "文件不存在：${file.name}"

        val pos = playerBlockPosition() ?: return "读不到玩家位置，请先进入世界"
        val work = BuildPaths.buildDir(activity)

        val loaded = TpModule.loadBuildProjection(
            file.absolutePath,
            work.absolutePath,
            pos[0], pos[1], pos[2],
            /* rotationDegrees = */ 0,
            /* pixelArtWidth = */ 0
        )
        if (!loaded) {
            val status = runCatching { TpModule.getBuildProjectionStatus() }.getOrNull()
            return if (status.isNullOrBlank()) "投影加载失败" else "投影加载失败：$status"
        }
        // 加载成功不等于显示，投影层要单独打开。
        runCatching { TpModule.setBuildProjectionEnabled(true) }
        return "投影已加载：${file.name}"
    }

    /** 打印：按当前投影自动放置周围能够到的方块。 */
    fun startPrinter(activity: Activity, rate: Int): String {
        ensureReady(activity)

        val status = runCatching { TpModule.getBuildProjectionStatus() }.getOrNull()
        if (status.isNullOrBlank()) return "没有可打印的投影，请先加载建筑文件"

        runCatching { TpModule.setBuildProjectionPrinterRate(rate) }
        runCatching { TpModule.setBuildProjectionPrinterEnabled(true) }

        val enabled = runCatching { TpModule.isBuildProjectionPrinterEnabled() }.getOrDefault(false)
        if (!enabled) {
            val printerStatus = runCatching { TpModule.getBuildProjectionPrinterStatus() }.getOrNull()
            return if (printerStatus.isNullOrBlank()) "打印机未能启动" else "打印机未能启动：$printerStatus"
        }
        return "打印已开始（$rate 块/秒）"
    }
}
