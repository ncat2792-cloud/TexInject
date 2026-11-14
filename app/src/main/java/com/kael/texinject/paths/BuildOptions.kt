/*
 * Copyright (C) 2026 Devout9527
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * 建筑打印的持久化选项，与面板 UI 分离。
 *
 * 面板是 PopupWindow，关掉就整棵 composition 销毁，状态不能只放在
 * remember 里；读写都走 SharedPreferences，下次打开还是上次的设置。
 */
package com.kael.texinject.paths

import android.content.Context

object BuildOptions {

    private const val PREF = "texinject_build"
    private const val KEY_RATE = "printer_rate"

    const val RATE_MIN = 1
    const val RATE_MAX = 20
    const val RATE_DEF = 4

    private fun p(c: Context) = c.getSharedPreferences(PREF, Context.MODE_PRIVATE)

    fun printerRate(c: Context): Int =
        p(c).getInt(KEY_RATE, RATE_DEF).coerceIn(RATE_MIN, RATE_MAX)

    fun setPrinterRate(c: Context, value: Int) {
        p(c).edit().putInt(KEY_RATE, value.coerceIn(RATE_MIN, RATE_MAX)).apply()
    }
}
