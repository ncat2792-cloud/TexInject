#include <jni.h>
#include <new>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <thread>
#include <unistd.h>
#include <link.h>
#include <android/log.h>

#include "mem/MemRw.h"
#include "game/LocalPlayerHook.h"
#include "game/GameApi.h"
#include "main.h"
#include "mount/ResourceSourceHook.h"
#include "plt/PltHook.h"
// 建筑工具
#include "build_import/BuildProjectionRenderer.h"
#include "build_import/MapAnvilUiCloseBridge.h"
#include "build_import/MapChestUiCloseBridge.h"
#include "tp/BuildPacketReceiveHook.h"
#include "tp/LoopbackPacketSenderCapture.h"
#include "tp/MinecraftUpdateHook.h"
#include "tp/FunctionsAddress.h"
#include "tp/TpModule.h"
#include "addrscan/Resolver.h"
#include "trace/IoTrace.h"
#include "tp/PythonUtils.h"
#include "common/Signatures.h"
#include "scripts/ScriptRuntime.h"

#define LOG_TAG "TexInjectNative"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

std::atomic<uintptr_t> Main::baseAddress{0};

namespace {

constexpr const char* kScriptRoot = "/storage/emulated/0/Android/data/com.netease.x19/files/pack_netease/scripts";

struct FindCtx { const char* name; uintptr_t base; };

int findModuleCb(dl_phdr_info* info, size_t, void* data) {
    auto* c = static_cast<FindCtx*>(data);
    if (!info->dlpi_name || !info->dlpi_name[0]) return 0;
    const char* slash = std::strrchr(info->dlpi_name, '/');
    const char* leaf = slash ? slash + 1 : info->dlpi_name;
    if (std::strcmp(leaf, c->name) != 0) return 0;
    c->base = static_cast<uintptr_t>(info->dlpi_addr);
    return 1;
}

uintptr_t getModuleBase(const char* name) {
    if (std::strcmp(name, "libminecraftpe.so") == 0) {
        const uintptr_t b = FunctionsAddress::moduleBase();
        if (b != 0) return b;
    }
    FindCtx ctx{name, 0};
    dl_iterate_phdr(findModuleCb, &ctx);

    if (ctx.base != 0) return ctx.base;

    FILE* maps = std::fopen("/proc/self/maps", "r");
    if (!maps) return 0;
    char line[512];
    uintptr_t base = 0;
    while (std::fgets(line, sizeof(line), maps)) {
        unsigned long long lo = 0, off = 0;
        char perms[8] = {0};

        if (std::strstr(line, name) != nullptr &&
            std::sscanf(line, "%llx-%*x %7s %llx", &lo, perms, &off) == 3 &&
            off == 0) {
            base = static_cast<uintptr_t>(lo);
            break;
        }
    }
    std::fclose(maps);
    return base;
}

static char gGameVersion[64] = { 0 };

void ensureBase() {
    if (Main::getBaseAddress() == 0) {
        Main::setBaseAddress(getModuleBase("libminecraftpe.so"));
    }
}

// 建筑工具的 hook。
//
// 地址为 0 表示当前游戏版本没有核验过的偏移，直接跳过 —— 建筑页显示
// 不可用，其余功能不受影响。这与「带着 3.9.15 的偏移去 3.10.5 装 hook」
// 是两种完全不同的结果：后者会跳进未映射内存直接闪退。
void installBuildHooks(uintptr_t base) {
    // 3.10.x 的地址不能靠固定偏移（区段内平移量不一致，抄旧值会把 hook
    // 装进别的函数中间）。先按特征解析：只有全部规则在该镜像上唯一命中
    // 才会发布地址，否则一个 hook 都不装。
    //
    // 3.9.15 走 FunctionsAddress 的固定表，这边解析失败也不影响它。
    if (FunctionsAddress::Minecraft_update_Hook == 0) {
        std::string address_error;
        bool retryable = false;
        if (buildtools::addrscan::Initialize(&address_error, &retryable)) {
            LOGI("addrscan: %s", buildtools::addrscan::StatusText().c_str());
        } else {
            LOGE("addrscan failed (%s): %s",
                 retryable ? "retry later" : "terminal for this build",
                 address_error.c_str());
        }
    }
    if (FunctionsAddress::Minecraft_update_Hook == 0) {
        LOGI("build hooks skipped: no verified offsets for this game build");
        return;
    }
    if (!InitLoopbackPacketSenderCapture(base)) {
        LOGE("build: LoopbackPacketSender capture failed");
    }
    if (!BuildPacketReceiveHook::isReceiveHookReady() && !BuildPacketReceiveHook::init(base)) {
        LOGE("build: receive hook failed");
    }
    if (!InitMinecraftUpdateHook(base)) {
        LOGE("build: tick hook failed");
    }
    if (!build_import::InitBuildProjectionHook(base)) {
        LOGE("build: projection render hook failed");
    }
    LOGI("build hooks installed");
}

void waitForMinecraft() {
    for (int attempt = 0; attempt < 100; ++attempt) {
        const uintptr_t base = getModuleBase("libminecraftpe.so");

        if (attempt < 20 || base != 0) {
            LOGI("waitForMinecraft: attempt=%d base=0x%lx", attempt, (unsigned long) base);
        }
        if (base != 0) {
            Main::setBaseAddress(base);

            if (resource_mount::PendingInstallation()) {
                if (resource_mount::Install(0)) LOGI("resource mount: %s", resource_mount::Status().c_str());
                else LOGE("resource mount install failed: %s", resource_mount::Status().c_str());
            }

            {
                FILE* f = std::fopen("/data/local/tmp/texinject/mcp_redirect", "r");
                if (f) {
                    char target[512] = { 0 };
                    if (std::fgets(target, sizeof(target), f)) {
                        size_t n = std::strlen(target);
                        while (n > 0 && (target[n - 1] == '\n' || target[n - 1] == '\r')) target[--n] = 0;
                        if (n > 0 && IoTrace::installRedirect(target)) {
                            LOGI("mcp redirect installed -> %s", target);
                        } else {
                            LOGE("mcp redirect 失败：%s", target);
                        }
                    }
                    std::fclose(f);
                }
            }

            {
                FILE* f = std::fopen("/data/local/tmp/texinject/mirror_stream", "r");
                if (f) {
                    char root[512] = { 0 };
                    if (!std::fgets(root, sizeof(root), f) || root[0] == '\n') {
                        std::snprintf(root, sizeof(root),
                                      "/storage/emulated/0/Android/data/com.netease.x19/files/pack_netease/Download");
                    }
                    size_t n = std::strlen(root);
                    while (n > 0 && (root[n - 1] == '\n' || root[n - 1] == '\r')) root[--n] = 0;
                    if (IoTrace::installMirror(root)) LOGI("stream mirror installed -> %s", root);
                    else LOGE("stream mirror 失败：%s", root);
                    std::fclose(f);
                }
            }
            installBuildHooks(base);
            LOGI("waitForMinecraft: base ready, hook 等面板手动连接");
            return;
        }
        usleep(500000);
    }
    LOGE("libminecraftpe.so not found");
}

void ensureNativeHooks(JNIEnv*, jclass) { ensureBase(); }

jboolean runPythonFile(JNIEnv* env, jclass, jstring jpath) {
    if (!jpath) return JNI_FALSE;
    const char* pathChars = env->GetStringUTFChars(jpath, nullptr);
    if (!pathChars) return JNI_FALSE;
    std::string path(pathChars);
    env->ReleaseStringUTFChars(jpath, pathChars);
    if (path.empty()) return JNI_FALSE;

    if (script_host::EnsureTickObserver()) {
        if (!local_python::SubmitPath(path)) {
            LOGE("runPythonFile: submit rejected（加载器忙或 hook 未就绪）");
            return JNI_FALSE;
        }

        for (int i = 0; i < 600 && local_python::Busy(); ++i) usleep(100 * 1000);
        const bool ok = local_python::LastSucceeded();
        LOGI("runPythonFile: %s -> %s", path.c_str(), ok ? "ok" : "fail");
        return ok ? JNI_TRUE : JNI_FALSE;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) { LOGE("runPythonFile: cannot open %s", path.c_str()); return JNI_FALSE; }
    std::string code((std::istreambuf_iterator<char>(file)),
                     std::istreambuf_iterator<char>());
    file.close();
    if (code.empty()) { LOGE("runPythonFile: empty %s", path.c_str()); return JNI_FALSE; }

    ensureBase();
    if (Main::getBaseAddress() == 0) { LOGE("runPythonFile: base not found"); return JNI_FALSE; }
    const bool ok = PythonUtils::PyExecChecked(code, true);
    LOGI("runPythonFile: %s -> %s", path.c_str(), ok ? "ok" : "fail");
    return ok ? JNI_TRUE : JNI_FALSE;
}

jboolean runPythonSource(JNIEnv* env, jclass, jstring jsource) {
    if (!jsource) return JNI_FALSE;
    const char* chars = env->GetStringUTFChars(jsource, nullptr);
    if (!chars) return JNI_FALSE;
    std::string source(chars);
    env->ReleaseStringUTFChars(jsource, chars);
    if (source.empty()) return JNI_FALSE;

    if (script_host::EnsureTickObserver()) {
        const std::string tmp = std::string(kScriptRoot) + "/_texinject_tmp_run.py";
        FILE* f = std::fopen(tmp.c_str(), "w");
        if (!f) { LOGE("runPythonSource: 写临时脚本失败 %s", tmp.c_str()); return JNI_FALSE; }
        std::fwrite(source.data(), 1, source.size(), f);
        std::fclose(f);
        if (!local_python::SubmitPath(tmp)) { LOGE("runPythonSource: submit rejected"); return JNI_FALSE; }
        for (int i = 0; i < 600 && local_python::Busy(); ++i) usleep(100 * 1000);
        const bool ok = local_python::LastSucceeded();
        LOGI("runPythonSource: -> %s", ok ? "ok" : "fail");
        return ok ? JNI_TRUE : JNI_FALSE;
    }

    ensureBase();
    if (Main::getBaseAddress() == 0) { LOGE("runPythonSource: base not found"); return JNI_FALSE; }
    const bool ok = PythonUtils::PyExecChecked(source, true);
    LOGI("runPythonSource: -> %s", ok ? "ok" : "fail");
    return ok ? JNI_TRUE : JNI_FALSE;
}

void setGameVersion(JNIEnv* env, jclass, jstring jver) {
    if (!jver) return;
    const char* s = env->GetStringUTFChars(jver, nullptr);
    if (!s) return;
    FunctionsAddress::setVersion(s);
    texinject::game::SetGameVersion(s);
    snprintf(gGameVersion, sizeof(gGameVersion), "%s", s);
    env->ReleaseStringUTFChars(jver, s);
}

jint pltInit(JNIEnv*, jclass) {
    ensureBase();
    Plt::init();
    return Plt::importCount();
}

jstring pltLastError(JNIEnv* env, jclass) {
    return env->NewStringUTF(Plt::lastError());
}

jboolean ioTraceStart(JNIEnv*, jclass) {
    ensureBase();
    return IoTrace::start() ? JNI_TRUE : JNI_FALSE;
}

void ioTraceStop(JNIEnv*, jclass) { IoTrace::stop(); }

jboolean ioTraceRunning(JNIEnv*, jclass) {
    return IoTrace::running() ? JNI_TRUE : JNI_FALSE;
}

jint ioTraceCount(JNIEnv*, jclass) {
    return static_cast<jint>(IoTrace::count());
}

jboolean ioTraceDump(JNIEnv* env, jclass, jstring jpath) {
    if (!jpath) return JNI_FALSE;
    const char* chars = env->GetStringUTFChars(jpath, nullptr);
    if (!chars) return JNI_FALSE;
    std::string path(chars);
    env->ReleaseStringUTFChars(jpath, chars);
    if (path.empty()) return JNI_FALSE;
    return IoTrace::dump(path.c_str()) ? JNI_TRUE : JNI_FALSE;
}

jstring ioTraceLastError(JNIEnv* env, jclass) {
    return env->NewStringUTF(IoTrace::lastError());
}

namespace {
std::string toUtf8(JNIEnv* env, jstring s) {
    if (!s) return {};
    const char* chars = env->GetStringUTFChars(s, nullptr);
    if (!chars) return {};
    std::string out(chars);
    env->ReleaseStringUTFChars(s, chars);
    return out;
}
}

jboolean mountConfigure(JNIEnv* env, jclass, jstring jsupplied, jstring jcanonical,
                        jstring jalias, jstring jstorage) {
    try {
        const std::string supplied = toUtf8(env, jsupplied);
        const std::string canonical = toUtf8(env, jcanonical);
        const std::string alias = toUtf8(env, jalias);
        const std::string storage = toUtf8(env, jstorage);
        if (supplied.empty() || canonical.empty() || alias.empty() || storage.empty()) {
            return JNI_FALSE;
        }
        return resource_mount::Configure(supplied, canonical, alias, storage) ? JNI_TRUE
                                                                            : JNI_FALSE;
    } catch (...) {
        return JNI_FALSE;
    }
}

jboolean mountInstall(JNIEnv*, jclass) {
    try {
        return resource_mount::Install(0) ? JNI_TRUE : JNI_FALSE;
    } catch (...) {
        return JNI_FALSE;
    }
}

jstring mountStatus(JNIEnv* env, jclass) {
    try {
        return env->NewStringUTF(resource_mount::Status().c_str());
    } catch (...) {
        return env->NewStringUTF("status_unavailable");
    }
}

jlong memBase(JNIEnv* env, jclass, jstring name) {
    if (!name) return 0;
    const char* raw = env->GetStringUTFChars(name, nullptr);
    uintptr_t base = 0;
    if (raw) {
        base = texinject::mem::ModuleBase(raw);
        env->ReleaseStringUTFChars(name, raw);
    }
    return static_cast<jlong>(base);
}

jint memReadI32(JNIEnv*, jclass, jlong addr) {
    int32_t v = 0;
    texinject::mem::ReadI32(static_cast<uintptr_t>(addr), v);
    return v;
}

jlong memReadI64(JNIEnv*, jclass, jlong addr) {
    int64_t v = 0;
    texinject::mem::ReadI64(static_cast<uintptr_t>(addr), v);
    return v;
}

jfloat memReadF32(JNIEnv*, jclass, jlong addr) {
    float v = 0;
    texinject::mem::ReadF32(static_cast<uintptr_t>(addr), v);
    return v;
}

jdouble memReadF64(JNIEnv*, jclass, jlong addr) {
    double v = 0;
    texinject::mem::ReadF64(static_cast<uintptr_t>(addr), v);
    return v;
}

jbyteArray memReadBytes(JNIEnv* env, jclass, jlong addr, jint len) {
    if (len <= 0 || len > (1 << 20)) return nullptr;
    auto* buf = new (std::nothrow) uint8_t[len];
    if (!buf) return nullptr;
    const bool ok = texinject::mem::ReadRaw(static_cast<uintptr_t>(addr), buf, len);
    jbyteArray out = nullptr;
    if (ok) {
        out = env->NewByteArray(len);
        if (out) env->SetByteArrayRegion(out, 0, len, reinterpret_cast<const jbyte*>(buf));
    }
    delete[] buf;
    return out;
}

jstring memReadCString(JNIEnv* env, jclass, jlong addr, jint max) {
    std::string text;
    if (!texinject::mem::ReadCString(static_cast<uintptr_t>(addr), text,
                                     max > 0 ? static_cast<size_t>(max) : 256)) {
        return nullptr;
    }
    return env->NewStringUTF(text.c_str());
}

jboolean memWriteI32(JNIEnv*, jclass, jlong addr, jint value) {
    return texinject::mem::WriteI32(static_cast<uintptr_t>(addr), value) ? JNI_TRUE : JNI_FALSE;
}

jboolean memWriteI64(JNIEnv*, jclass, jlong addr, jlong value) {
    return texinject::mem::WriteI64(static_cast<uintptr_t>(addr), value) ? JNI_TRUE : JNI_FALSE;
}

jboolean memWriteF32(JNIEnv*, jclass, jlong addr, jfloat value) {
    return texinject::mem::WriteF32(static_cast<uintptr_t>(addr), value) ? JNI_TRUE : JNI_FALSE;
}

jboolean memWriteF64(JNIEnv*, jclass, jlong addr, jdouble value) {
    return texinject::mem::WriteF64(static_cast<uintptr_t>(addr), value) ? JNI_TRUE : JNI_FALSE;
}

jboolean memWriteBytes(JNIEnv* env, jclass, jlong addr, jbyteArray data) {
    if (!data) return JNI_FALSE;
    const jsize len = env->GetArrayLength(data);
    if (len <= 0) return JNI_FALSE;
    jbyte* raw = env->GetByteArrayElements(data, nullptr);
    if (!raw) return JNI_FALSE;
    const bool ok = texinject::mem::WriteRaw(static_cast<uintptr_t>(addr), raw, len);
    env->ReleaseByteArrayElements(data, raw, JNI_ABORT);
    return ok ? JNI_TRUE : JNI_FALSE;
}

static void ensureGameBase() {
    if (Main::getBaseAddress() == 0) ensureBase();
    texinject::game::SetBase(Main::getBaseAddress(), gGameVersion);
}

jboolean ioTraceMirror(JNIEnv* env, jclass, jstring jroot) {
    if (!jroot) return JNI_FALSE;
    const char* root = env->GetStringUTFChars(jroot, nullptr);
    if (!root) return JNI_FALSE;
    const bool ok = IoTrace::installMirror(root);
    env->ReleaseStringUTFChars(jroot, root);
    return ok ? JNI_TRUE : JNI_FALSE;
}

jboolean ioTraceMirrorOn(JNIEnv*, jclass) {
    return IoTrace::mirrorOn() ? JNI_TRUE : JNI_FALSE;
}

jboolean gameFeature(JNIEnv* env, jclass, jstring jid, jboolean on) {
    if (!jid) return JNI_FALSE;
    const char* id = env->GetStringUTFChars(jid, nullptr);
    if (!id) return JNI_FALSE;
    ensureGameBase();
    const bool ok = texinject::game::SetFeature(id, on == JNI_TRUE);
    env->ReleaseStringUTFChars(jid, id);
    return ok ? JNI_TRUE : JNI_FALSE;
}

jboolean gameFeatureOn(JNIEnv* env, jclass, jstring jid) {
    if (!jid) return JNI_FALSE;
    const char* id = env->GetStringUTFChars(jid, nullptr);
    if (!id) return JNI_FALSE;
    const bool on = texinject::game::Feature(id);
    env->ReleaseStringUTFChars(jid, id);
    return on ? JNI_TRUE : JNI_FALSE;
}

jboolean gameAttach(JNIEnv*, jclass) {
    ensureGameBase();
    return texinject::game::Attach() ? JNI_TRUE : JNI_FALSE;
}

jboolean gameInWorld(JNIEnv*, jclass) {
    ensureGameBase();
    return texinject::game::InWorld() ? JNI_TRUE : JNI_FALSE;
}

jint gameHandDamage(JNIEnv*, jclass) {
    ensureGameBase();
    return texinject::game::HandDamage();
}

jboolean gameSetDamage(JNIEnv*, jclass, jint value) {
    ensureGameBase();
    return texinject::game::SetHandDamage(value) ? JNI_TRUE : JNI_FALSE;
}

jstring gameHandInfo(JNIEnv* env, jclass) {
    ensureGameBase();
    return env->NewStringUTF(texinject::game::HandInfo().c_str());
}

jboolean gameApplyNbt(JNIEnv* env, jclass, jstring jops) {    if (!jops) return JNI_FALSE;
    ensureGameBase();
    const char* ops = env->GetStringUTFChars(jops, nullptr);
    if (!ops) return JNI_FALSE;
    const bool ok = texinject::game::ApplyHandNbt(ops);
    env->ReleaseStringUTFChars(jops, ops);
    return ok ? JNI_TRUE : JNI_FALSE;
}

jboolean pyInit(JNIEnv*, jclass) {
    return script_host::EnsureTickObserver() ? JNI_TRUE : JNI_FALSE;
}

jboolean pyReady(JNIEnv*, jclass) {
    return script_host::Operational() ? JNI_TRUE : JNI_FALSE;
}

jboolean pyStart(JNIEnv* env, jclass, jbyteArray jpath) {
    if (!jpath) return JNI_FALSE;
    const jsize n = env->GetArrayLength(jpath);
    if (n < 1 || n > 4096) return JNI_FALSE;
    std::string path(static_cast<size_t>(n), '\0');
    env->GetByteArrayRegion(jpath, 0, n, reinterpret_cast<jbyte*>(path.data()));
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return JNI_FALSE;
    }
    return local_python::SubmitPath(path) ? JNI_TRUE : JNI_FALSE;
}

jbyteArray pyStatus(JNIEnv* env, jclass) {
    std::string text = local_python::StatusJson();
    if (!script_host::Operational()) {
        text = "{\"phase\":6,\"detail\":" + local_python::QuoteStatus(script_host::InitializationStatus()) + "}";
    }
    jbyteArray out = env->NewByteArray(static_cast<jsize>(text.size()));
    if (out) {
        env->SetByteArrayRegion(out, 0, static_cast<jsize>(text.size()),
                                reinterpret_cast<const jbyte*>(text.data()));
    }
    return out;
}

const JNINativeMethod native_methods[] = {
    {"pyInit",            "()Z",     reinterpret_cast<void*>(pyInit)},
    {"pyReady",           "()Z",     reinterpret_cast<void*>(pyReady)},
    {"pyStart",           "([B)Z",   reinterpret_cast<void*>(pyStart)},
    {"pyStatus",          "()[B",    reinterpret_cast<void*>(pyStatus)},
    {"ensureNativeHooks", "()V",     reinterpret_cast<void*>(ensureNativeHooks)},
    {"runPythonFile",     "(Ljava/lang/String;)Z", reinterpret_cast<void*>(runPythonFile)},
    {"runPythonSource",   "(Ljava/lang/String;)Z", reinterpret_cast<void*>(runPythonSource)},
    {"setGameVersion",    "(Ljava/lang/String;)V", reinterpret_cast<void*>(setGameVersion)},
    {"pltInit",           "()I",     reinterpret_cast<void*>(pltInit)},
    {"pltLastError",      "()Ljava/lang/String;", reinterpret_cast<void*>(pltLastError)},
    {"ioTraceStart",      "()Z",     reinterpret_cast<void*>(ioTraceStart)},
    {"ioTraceStop",       "()V",     reinterpret_cast<void*>(ioTraceStop)},
    {"ioTraceRunning",    "()Z",     reinterpret_cast<void*>(ioTraceRunning)},
    {"ioTraceCount",      "()I",     reinterpret_cast<void*>(ioTraceCount)},
    {"ioTraceDump",       "(Ljava/lang/String;)Z", reinterpret_cast<void*>(ioTraceDump)},
    {"ioTraceLastError",  "()Ljava/lang/String;", reinterpret_cast<void*>(ioTraceLastError)},
    {"mountConfigure",    "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Z",
                          reinterpret_cast<void*>(mountConfigure)},
    {"mountInstall",      "()Z",     reinterpret_cast<void*>(mountInstall)},
    {"mountStatus",       "()Ljava/lang/String;", reinterpret_cast<void*>(mountStatus)},
    {"memBase",           "(Ljava/lang/String;)J", reinterpret_cast<void*>(memBase)},
    {"memReadI32",        "(J)I",     reinterpret_cast<void*>(memReadI32)},
    {"memReadI64",        "(J)J",     reinterpret_cast<void*>(memReadI64)},
    {"memReadF32",        "(J)F",     reinterpret_cast<void*>(memReadF32)},
    {"memReadF64",        "(J)D",     reinterpret_cast<void*>(memReadF64)},
    {"memReadBytes",      "(JI)[B",   reinterpret_cast<void*>(memReadBytes)},
    {"memReadCString",    "(JI)Ljava/lang/String;", reinterpret_cast<void*>(memReadCString)},
    {"memWriteI32",       "(JI)Z",    reinterpret_cast<void*>(memWriteI32)},
    {"memWriteI64",       "(JJ)Z",    reinterpret_cast<void*>(memWriteI64)},
    {"memWriteF32",       "(JF)Z",    reinterpret_cast<void*>(memWriteF32)},
    {"memWriteF64",       "(JD)Z",    reinterpret_cast<void*>(memWriteF64)},
    {"memWriteBytes",     "(J[B)Z",   reinterpret_cast<void*>(memWriteBytes)},
    {"ioTraceMirror",     "(Ljava/lang/String;)Z", reinterpret_cast<void*>(ioTraceMirror)},
    {"ioTraceMirrorOn",   "()Z",     reinterpret_cast<void*>(ioTraceMirrorOn)},
    {"gameFeature",       "(Ljava/lang/String;Z)Z", reinterpret_cast<void*>(gameFeature)},
    {"gameFeatureOn",     "(Ljava/lang/String;)Z", reinterpret_cast<void*>(gameFeatureOn)},
    {"gameAttach",        "()Z",      reinterpret_cast<void*>(gameAttach)},
    {"gameInWorld",       "()Z",      reinterpret_cast<void*>(gameInWorld)},
    {"gameHandDamage",    "()I",      reinterpret_cast<void*>(gameHandDamage)},
    {"gameSetDamage",     "(I)Z",     reinterpret_cast<void*>(gameSetDamage)},
    {"gameApplyNbt",      "(Ljava/lang/String;)Z", reinterpret_cast<void*>(gameApplyNbt)},
    {"gameHandInfo",      "()Ljava/lang/String;", reinterpret_cast<void*>(gameHandInfo)},
};

// ── 建筑工具：地图界面关闭请求 ───────────────────────────────────────
//
// 服务器结算铁砧/箱子结果后客户端要主动关窗口，否则会一直停在「处理中」。
// 这几个只把请求取回来，真正的界面操作在 Kotlin 层。
jlong takePendingMapAnvilUiCloseRequest(JNIEnv*, jclass) {
    return static_cast<jlong>(build_import::TakePendingMapAnvilUiCloseRequest());
}
jboolean isMapAnvilUiCloseStillSafe(JNIEnv*, jclass, jlong ticket) {
    return build_import::IsMapAnvilUiCloseStillSafe(static_cast<uint64_t>(ticket))
        ? JNI_TRUE : JNI_FALSE;
}
void markMapAnvilUiCloseDispatched(JNIEnv*, jclass, jlong ticket) {
    build_import::MarkMapAnvilUiCloseDispatched(static_cast<uint64_t>(ticket));
}
jlong takePendingMapChestUiCloseRequest(JNIEnv*, jclass) {
    return static_cast<jlong>(build_import::TakePendingMapChestUiCloseRequest());
}
jboolean isMapChestUiCloseStillSafe(JNIEnv*, jclass, jlong ticket) {
    return build_import::IsMapChestUiCloseStillSafe(static_cast<uint64_t>(ticket))
        ? JNI_TRUE : JNI_FALSE;
}
void markMapChestUiCloseDispatched(JNIEnv*, jclass, jlong ticket) {
    build_import::MarkMapChestUiCloseDispatched(static_cast<uint64_t>(ticket));
}

/** 建筑工具状态，供面板判断能否打开建筑页。 */
jstring addressResolutionStatus(JNIEnv* env, jclass) {
    if (!env) return nullptr;
    // 面板用 startsWith("已就绪") 判断可用性。
    std::string text = "已就绪：建筑工具";
    if (Main::getBaseAddress() == 0) {
        text = "游戏原生库尚未加载";
    } else if (FunctionsAddress::Minecraft_update_Hook == 0) {
        // 这是当前唯一会让建筑功能整体不可用的条件：版本没核验过。
        std::string reason = FunctionsAddress::detectReason();
        text = reason.empty() ? "当前游戏版本不支持建筑功能"
                              : std::string("当前游戏版本不支持建筑功能：") + reason;
    }
    return env->NewStringUTF(text.c_str());
}

const JNINativeMethod build_native_methods[] = {
    {"ensureBuildToolsHooks", "()V", reinterpret_cast<void*>(ensureNativeHooks)},
    {"addressResolutionStatus", "()Ljava/lang/String;",
     reinterpret_cast<void*>(addressResolutionStatus)},
    {"takePendingMapAnvilUiCloseRequest", "()J",
     reinterpret_cast<void*>(takePendingMapAnvilUiCloseRequest)},
    {"isMapAnvilUiCloseStillSafe", "(J)Z",
     reinterpret_cast<void*>(isMapAnvilUiCloseStillSafe)},
    {"markMapAnvilUiCloseDispatched", "(J)V",
     reinterpret_cast<void*>(markMapAnvilUiCloseDispatched)},
    {"takePendingMapChestUiCloseRequest", "()J",
     reinterpret_cast<void*>(takePendingMapChestUiCloseRequest)},
    {"isMapChestUiCloseStillSafe", "(J)Z",
     reinterpret_cast<void*>(isMapChestUiCloseStillSafe)},
    {"markMapChestUiCloseDispatched", "(J)V",
     reinterpret_cast<void*>(markMapChestUiCloseDispatched)},
};

}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
    jclass clazz = env->FindClass("com/kael/texinject/nativecore/NativeCore");
    if (!clazz) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return JNI_ERR;
    }

    const size_t total = sizeof(native_methods) / sizeof(native_methods[0]);
    size_t failed = 0;
    for (const JNINativeMethod& method : native_methods) {
        if (env->RegisterNatives(clazz, &method, 1) == JNI_OK) continue;
        if (env->ExceptionCheck()) env->ExceptionClear();
        LOGE("RegisterNatives 失败: %s %s", method.name, method.signature);
        ++failed;
    }
    env->DeleteLocalRef(clazz);
    if (failed == total) {
        LOGE("native 方法全部没接上，检查 NativeCore 的 native 声明");
        return JNI_ERR;
    }
    if (failed != 0) LOGE("%zu/%zu 个 native 方法没接上", failed, total);

    // 建筑工具：注册到它自己的两个类上。
    //
    // 单独注册、失败只记日志不影响 TexInject 本体 —— 建筑功能不可用是
    // 可接受的降级，整个模块全废不是。
    {
        jclass build_core = env->FindClass("com/vdl/kong520/NativeCore");
        if (build_core) {
            for (const JNINativeMethod& method : build_native_methods) {
                if (env->RegisterNatives(build_core, &method, 1) == JNI_OK) continue;
                if (env->ExceptionCheck()) env->ExceptionClear();
                LOGE("建筑: RegisterNatives 失败 %s", method.name);
            }
            env->DeleteLocalRef(build_core);
        } else {
            if (env->ExceptionCheck()) env->ExceptionClear();
            LOGE("建筑: com/vdl/kong520/NativeCore 未找到，建筑功能不可用");
        }
        if (!RegisterTpModuleNatives(env)) {
            LOGE("TpModule natives 注册失败，建筑功能不可用");
        }
    }

    {
        FILE* f = fopen("/proc/self/cmdline", "r");
        char cmd[256] = {0};
        bool isTest = false;
        if (f) {
            size_t n = fread(cmd, 1, sizeof(cmd) - 1, f);
            fclose(f);
            if (n > 0 && strstr(cmd, "mctest") != nullptr) isTest = true;
        }
        if (isTest) {
            LOGE("test build detected: native hooks disabled (offsets incomplete)");
            return JNI_VERSION_1_6;
        }
    }

    std::thread(waitForMinecraft).detach();
    return JNI_VERSION_1_6;
}

// ── 建筑工具：可执行段地址解析 ──────────────────────────────────────
//
// 用 /proc/self/maps 而不是 dl_iterate_phdr。
//
// dl_iterate_phdr 回调里的 dlpi_name 在回调返回后可能失效；据此算出的基址
// 会带垃圾高字节，于是「基址 + RVA」得到的地址可能根本不存在。maps 是内核
// 给的文本快照，不依赖任何回调期指针。
bool ResolveMinecraftExecutableOffset(uintptr_t base_address, uintptr_t offset,
                                      size_t required_size, uintptr_t* address) {
    if (!address) return false;
    *address = 0;
    if (!base_address || !offset || !required_size ||
        base_address > std::numeric_limits<uintptr_t>::max() - offset) {
        return false;
    }
    const uintptr_t target = base_address + offset;
    if ((target & 0x3U) != 0) return false;

    FILE* maps = std::fopen("/proc/self/maps", "r");
    if (!maps) return false;
    char line[1024];
    bool ok = false;
    while (std::fgets(line, sizeof(line), maps)) {
        unsigned long long start = 0, end = 0, file_off = 0;
        char perms[8] = {0};
        if (std::sscanf(line, "%llx-%llx %7s %llx", &start, &end, perms, &file_off) != 4) continue;
        if (perms[2] != 'x') continue;  // 第三位是 x 才是可执行
        if (target < start) continue;
        if (target + required_size > end) continue;
        ok = true;
        break;
    }
    std::fclose(maps);
    if (!ok) return false;
    *address = target;
    return true;
}

// 投影所需的 tick + 渲染 hook。面板在打开投影前会单独请求一次，
// 所以这里不依赖 installBuildHooks 是否已经跑过。
bool EnsureBuildProjectionHooksReady() {
    const uintptr_t base_address = Main::getBaseAddress();
    if (base_address == 0) return false;
    if (FunctionsAddress::Minecraft_update_Hook == 0) {
        // 当前游戏版本没有核验过的偏移，明确失败而不是硬装。
        return false;
    }
    return InitMinecraftUpdateHook(base_address) &&
           build_import::InitBuildProjectionHook(base_address);
}
