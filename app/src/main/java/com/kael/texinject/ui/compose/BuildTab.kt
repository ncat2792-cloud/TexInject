/*
 * Copyright (C) 2026 Devout9527
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * 「建筑」页。
 *
 * 功能与 kong 版完全一致：导入 / 像素画 / 投影（含打印）/ 导出，四个入口
 * 各自打开一个独立的功能界面。这里只做入口，不放任何功能逻辑 —— 具体
 * 界面在 com.vdl.kong520.ui 下，与 kong 版共用同一份实现。
 */
package com.kael.texinject.ui.compose

import android.app.Activity
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.vdl.kong520.BuildToolsLauncher

private val BuildAccent = Color(0xFF0062FF)
private val BuildCard = Color(0x33FFFFFF)
private val BuildMuted = Color(0xFF8E8E93)

/** 一个入口：标题、说明、对应 launcher 的 tool。 */
private data class BuildEntry(
    val title: String,
    val desc: String,
    val tool: BuildToolsLauncher.Tool
)

private val ENTRIES = listOf(
    BuildEntry(
        "导入建筑",
        "把结构文件铺到当前位置（.mcstructure / .bdx / .schematic / .litematic）",
        BuildToolsLauncher.Tool.IMPORT
    ),
    BuildEntry(
        "像素画导入",
        "把图片按方块逐格搭出来，可选宽度",
        BuildToolsLauncher.Tool.PIXEL_ART
    ),
    BuildEntry(
        "投影 / 打印",
        "加载投影预览，按当前投影自动放置周围的方块",
        BuildToolsLauncher.Tool.PROJECTION
    ),
    BuildEntry(
        "导出建筑",
        "把自己建的建筑导出成文件，保存到 build 目录",
        BuildToolsLauncher.Tool.EXPORT
    )
)

@Composable
fun BuildTab(activity: Activity) {
    Column(
        Modifier
            .fillMaxWidth()
            .verticalScroll(rememberScrollState())
    ) {
        Text(
            "建筑工具",
            fontSize = 13.sp,
            color = Color.White,
            fontWeight = FontWeight.Medium,
            modifier = Modifier.padding(bottom = 2.dp)
        )
        Text(
            "文件放在 pack_netease/build，点下面任意一项开始",
            fontSize = 10.sp,
            color = BuildMuted,
            modifier = Modifier.padding(bottom = 10.dp)
        )

        Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
            ENTRIES.forEach { entry ->
                Row(
                    Modifier
                        .fillMaxWidth()
                        .heightIn(min = 58.dp)
                        .clip(RoundedCornerShape(10.dp))
                        .background(BuildCard)
                        .clickable { BuildToolsLauncher.open(activity, entry.tool) }
                        .padding(horizontal = 12.dp, vertical = 10.dp),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Column(Modifier.weight(1f)) {
                        Text(entry.title, fontSize = 13.sp, color = Color.White)
                        Spacer(Modifier.height(3.dp))
                        Text(entry.desc, fontSize = 10.sp, color = BuildMuted)
                    }
                }
            }
        }

        Spacer(Modifier.height(10.dp))
        Box(
            Modifier
                .fillMaxWidth()
                .clip(RoundedCornerShape(10.dp))
                .background(Color(0x14FFFFFF))
                .padding(10.dp)
        ) {
            Text(
                "提示：打印在「投影 / 打印」里，先加载投影再开启打印。" +
                    "打印只覆盖玩家周围约 6 格，远处的方块要自己走近。",
                fontSize = 10.sp,
                color = BuildMuted
            )
        }
    }
}
