/*
 * Copyright (C) 2026 Devout9527
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * 「建筑」页：建筑文件列表 + 导入 / 投影 / 打印。
 *
 * 视觉沿用面板既有语言：圆角 10dp、0x33FFFFFF 卡片、0xFF0062FF 强调色、
 * 11~13sp 字号。尺寸不写死，跟随 PanelSize 调过的大小区。
 */
package com.kael.texinject.ui.compose

import android.app.Activity
import android.os.Handler
import android.os.Looper
import android.widget.Toast
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.kael.texinject.paths.BuildOptions
import com.kael.texinject.paths.BuildPaths
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.File

private val BuildAccent = Color(0xFF0062FF)
private val BuildCard = Color(0x33FFFFFF)
private val BuildCardActive = Color(0x660062FF)
private val BuildMuted = Color(0xFF8E8E93)
private val BuildGood = Color(0xFF34C759)
private val BuildWarn = Color(0xFFFF9F0A)

/** 建筑页的三种工作模式。 */
private enum class BuildMode(val label: String) {
    Import("导入"),
    Project("投影"),
    Print("打印")
}

/** 建筑文件条目：名字、大小、类型都在扫描时算好，UI 不再做 IO。 */
private data class BuildFile(
    val file: File,
    val name: String,
    val sizeText: String,
    val kindText: String
)

private fun File.describe(): BuildFile {
    val bytes = length()
    val size = when {
        bytes >= 1024L * 1024L -> String.format("%.1f MB", bytes / 1024f / 1024f)
        bytes >= 1024L -> String.format("%.0f KB", bytes / 1024f)
        else -> "$bytes B"
    }
    val kind = when (name.substringAfterLast('.', "").lowercase()) {
        "mcstructure" -> "结构"
        "bdx" -> "BDX"
        "schematic" -> "Schem"
        "litematic" -> "投影"
        "mcworld" -> "世界"
        "infcz" -> "本模块"
        "png" -> "像素画"
        else -> "文件"
    }
    return BuildFile(this, name, size, kind)
}

@Composable
fun BuildTab(activity: Activity) {
    val main = remember { Handler(Looper.getMainLooper()) }
    fun toast(msg: String) = main.post {
        Toast.makeText(activity, msg, Toast.LENGTH_SHORT).show()
    }

    var mode by remember { mutableStateOf(BuildMode.Import) }
    var files by remember { mutableStateOf<List<BuildFile>>(emptyList()) }
    var selected by remember { mutableStateOf<BuildFile?>(null) }
    var scanTick by remember { mutableIntStateOf(0) }
    var printerRate by remember { mutableIntStateOf(BuildOptions.printerRate(activity)) }

    // 目录扫描放后台：列表可能上百个文件，压在主线程会卡住和游戏共用的 UI 线程。
    LaunchedEffect(scanTick) {
        files = withContext(Dispatchers.IO) {
            val extensions = BuildPaths.BUILD_EXTENSIONS
            BuildPaths.buildDir(activity).listFiles()
                ?.filter { it.isFile && extensions.any { e -> it.name.lowercase().endsWith(e) } }
                ?.sortedBy { it.name.lowercase() }
                ?.map { it.describe() }
                ?: emptyList()
        }
    }

    Column(Modifier.fillMaxSize()) {
        // ---- 模式切换 -------------------------------------------------
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            BuildMode.entries.forEach { m ->
                Box(
                    Modifier
                        .weight(1f)
                        .clip(RoundedCornerShape(8.dp))
                        .background(if (mode == m) BuildCardActive else BuildCard)
                        .clickable { mode = m }
                        .padding(vertical = 7.dp),
                    contentAlignment = Alignment.Center
                ) {
                    Text(
                        m.label,
                        fontSize = 12.sp,
                        color = Color.White,
                        fontWeight = if (mode == m) FontWeight.Medium else FontWeight.Normal
                    )
                }
            }
        }

        Spacer(Modifier.height(8.dp))

        // ---- 文件列表 -------------------------------------------------
        Row(
            Modifier.fillMaxWidth().padding(bottom = 4.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Text(
                "建筑文件 · ${files.size}",
                fontSize = 11.sp,
                color = BuildMuted,
                modifier = Modifier.weight(1f)
            )
            Text(
                "刷新",
                fontSize = 11.sp,
                color = Color(0xFF7FB2FF),
                modifier = Modifier.clickable { scanTick++ }.padding(horizontal = 6.dp)
            )
        }

        Box(
            Modifier
                .fillMaxWidth()
                .heightIn(min = 96.dp, max = 190.dp)
                .clip(RoundedCornerShape(10.dp))
                .background(Color(0x14FFFFFF))
        ) {
            if (files.isEmpty()) {
                Column(
                    Modifier.fillMaxSize().padding(12.dp),
                    verticalArrangement = Arrangement.Center,
                    horizontalAlignment = Alignment.CenterHorizontally
                ) {
                    Text("还没有建筑文件", fontSize = 12.sp, color = BuildMuted)
                    Spacer(Modifier.height(4.dp))
                    Text("放进 build/ 目录后点刷新", fontSize = 11.sp, color = BuildMuted)
                }
            } else {
                LazyColumn(Modifier.fillMaxSize().padding(4.dp)) {
                    items(files, key = { it.file.absolutePath }) { f ->
                        val active = selected?.file?.absolutePath == f.file.absolutePath
                        Row(
                            Modifier
                                .fillMaxWidth()
                                .padding(vertical = 2.dp)
                                .clip(RoundedCornerShape(8.dp))
                                .background(if (active) BuildCardActive else Color.Transparent)
                                .clickable { selected = f }
                                .padding(horizontal = 8.dp, vertical = 7.dp),
                            verticalAlignment = Alignment.CenterVertically
                        ) {
                            Box(
                                Modifier
                                    .clip(RoundedCornerShape(4.dp))
                                    .background(Color(0x330062FF))
                                    .padding(horizontal = 5.dp, vertical = 1.dp)
                            ) {
                                Text(f.kindText, fontSize = 9.sp, color = Color(0xFFB8D4FF))
                            }
                            Spacer(Modifier.width(7.dp))
                            Text(
                                f.name,
                                fontSize = 12.sp,
                                color = Color.White,
                                maxLines = 1,
                                modifier = Modifier.weight(1f)
                            )
                            Spacer(Modifier.width(6.dp))
                            Text(f.sizeText, fontSize = 10.sp, color = BuildMuted)
                        }
                    }
                }
            }
        }

        Spacer(Modifier.height(8.dp))

        // ---- 打印选项（只在打印模式出现） -------------------------------
        if (mode == BuildMode.Print) {
            Column(
                Modifier
                    .fillMaxWidth()
                    .clip(RoundedCornerShape(10.dp))
                    .background(BuildCard)
                    .padding(horizontal = 10.dp, vertical = 8.dp),
                verticalArrangement = Arrangement.spacedBy(6.dp)
            ) {
                BuildRateRow(rate = printerRate, onChange = {
                    printerRate = it
                    BuildOptions.setPrinterRate(activity, it)
                })
            }
            Spacer(Modifier.height(8.dp))
        }

        // ---- 主按钮 ---------------------------------------------------
        val isPrint = mode == BuildMode.Print
        val ready = isPrint || selected != null
        val label = when {
            isPrint -> "开始打印"
            selected == null -> "先选择建筑文件"
            mode == BuildMode.Import -> "导入 ${selected!!.name}"
            else -> "投影 ${selected!!.name}"
        }
        Box(
            Modifier
                .fillMaxWidth()
                .clip(RoundedCornerShape(10.dp))
                .background(if (ready) BuildAccent else Color(0x22FFFFFF))
                .clickable {
                    val target = selected
                    if (!isPrint && target == null) {
                        toast("先选择建筑文件")
                        return@clickable
                    }
                    // 功能层调用会做文件 IO 和 native 初始化，不能压在主线程
                    // —— 游戏 UI 线程和它共用一个，卡住就是 ANR。
                    Thread({
                        val msg = runCatching {
                            when (mode) {
                                BuildMode.Import -> BuildActions.startImport(activity, target!!.file)
                                BuildMode.Project -> BuildActions.startProjection(activity, target!!.file)
                                BuildMode.Print -> BuildActions.startPrinter(activity, printerRate)
                            }
                        }.getOrElse { it.message ?: "操作失败" }
                        main.post { toast(msg) }
                    }, "BuildAction").start()
                }
                .padding(vertical = 11.dp),
            contentAlignment = Alignment.Center
        ) {
            Text(label, fontSize = 13.sp, color = Color.White, fontWeight = FontWeight.Medium)
        }

        Spacer(Modifier.height(6.dp))
        Text("目录：pack_netease/build", fontSize = 10.sp, color = BuildMuted)
    }
}

/** 一行开关：标题 + 说明 + 右侧胶囊。 */
@Composable
private fun BuildToggle(title: String, desc: String, checked: Boolean, onChange: (Boolean) -> Unit) {
    Row(
        Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(8.dp))
            .clickable { onChange(!checked) }
            .padding(vertical = 3.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Column(Modifier.weight(1f)) {
            Text(title, fontSize = 12.sp, color = Color.White)
            Text(desc, fontSize = 10.sp, color = BuildMuted)
        }
        Spacer(Modifier.width(8.dp))
        Box(
            Modifier
                .size(width = 34.dp, height = 19.dp)
                .clip(RoundedCornerShape(10.dp))
                .background(if (checked) BuildGood else Color(0x33FFFFFF)),
            contentAlignment = if (checked) Alignment.CenterEnd else Alignment.CenterStart
        ) {
            Box(
                Modifier
                    .padding(horizontal = 2.dp)
                    .size(15.dp)
                    .clip(RoundedCornerShape(8.dp))
                    .background(Color.White)
            )
        }
    }
}

/** 速率选择：用等宽按钮而不是滑块，游戏里更好点。 */
@Composable
private fun BuildRateRow(rate: Int, onChange: (Int) -> Unit) {
    Column {
        Text("打印速度 · $rate 块/秒", fontSize = 12.sp, color = Color.White)
        Spacer(Modifier.height(4.dp))
        Row(horizontalArrangement = Arrangement.spacedBy(5.dp)) {
            listOf(2, 4, 6, 10, 20).forEach { v ->
                Box(
                    Modifier
                        .weight(1f)
                        .clip(RoundedCornerShape(6.dp))
                        .background(if (rate == v) BuildCardActive else BuildCard)
                        .clickable { onChange(v) }
                        .padding(vertical = 5.dp),
                    contentAlignment = Alignment.Center
                ) {
                    Text("$v", fontSize = 11.sp, color = Color.White)
                }
            }
        }
    }
}
