#pragma once
#include <cstdint>

struct FunctionsAddress {
    static uintptr_t PythonUtils_setExecLocked;
    static uintptr_t PythonUtils_PyGILState_EnsureMC;
    static uintptr_t PythonUtils_PyImport_AddModuleMC;
    static uintptr_t PythonUtils_PyModule_GetDictMC;
    static uintptr_t PythonUtils_PyRun_StringFlagsMC;
    static uintptr_t PythonUtils_PyGILState_ReleaseMC;

    // ── 建筑工具用到的游戏函数 ──────────────────────────────────────
    //
    // 由 detect() 按 build-id 填充。0 表示该版本没有可用值：建筑侧看到 0
    // 会直接判定「功能不可用」并拒绝安装 hook，不会带着旧偏移跳过去。
    static uintptr_t Actor_getClientInstance;
    static uintptr_t Actor_getDimension;
    static uintptr_t Minecraft_update_Hook;
    static uintptr_t Level_Render_Hook;
    static uintptr_t ClientInstance_getCamera;
    static uintptr_t newReceivePacket_Hook;

    static void detect();

    static void setVersion(const char* v);

    static const char* detectReason();

    static uintptr_t moduleBase();

    static bool pyTrace();
};
