#include "FunctionsAddress.h"
#include "../main.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <unistd.h>
#include <link.h>
#include <cerrno>
#include <sys/mman.h>
#include <elf.h>
#include <android/log.h>

#define LOG_TAG "TexInjectNative"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static constexpr uintptr_t ADDR_3915[] = {
    0x1351EE70,
    0x12356D88,
    0x122D63F0,
    0x1224ABA0,
    0x123586A4,
    0x12356E20,
};

static constexpr uintptr_t ADDR_310[] = {
    0x00000000,
    0x119C0178,
    0x11940A68,
    0x118BE774,
    0x119C1A94,
    0x119C0210,
};

static constexpr uintptr_t ADDR_3105[] = {
    0x00000000,
    0x119C3150,
    0x11943A40,
    0x118C174C,
    0x119C4A6C,
    0x119C31E8,
};

static const unsigned char BUILD_39[20] = {
    0xc6, 0xd1, 0x7b, 0xd2, 0x3b, 0x89, 0xef, 0xfa, 0x01, 0x30,
    0x94, 0x37, 0x9e, 0x62, 0x39, 0x46, 0x92, 0xfc, 0xc7, 0x83};
static const unsigned char BUILD_310[20] = {
    0xcf, 0xfb, 0xb1, 0x37, 0x66, 0x51, 0x57, 0x11, 0x45, 0x3b,
    0xca, 0x0b, 0x78, 0x72, 0x39, 0x46, 0x05, 0xa3, 0x8d, 0x09};
static const unsigned char BUILD_3105[20] = {
    0xa4, 0x94, 0xde, 0x85, 0x05, 0x9e, 0x92, 0xab, 0xc0, 0x1d,
    0x63, 0x43, 0x03, 0x92, 0x39, 0x46, 0x56, 0x1a, 0x8a, 0x8b};

static constexpr uint64_t FP_3915[5] = {
    0xF9000BF3A9BE7BFDULL, 0xF9000BF5A9BD7BFDULL, 0xF9000BF3A9BE7BFDULL,
    0xA9015FF8A9BC7BFDULL, 0xA90157F6A9BD7BFDULL};
static constexpr uint64_t FP_310[5] = {
    0xF9000BF3A9BE7BFDULL, 0xF9000BF5A9BD7BFDULL, 0xF9000BF3A9BE7BFDULL,
    0xA90B7BFDD10443FFULL, 0xA90157F6A9BD7BFDULL};
static constexpr uint64_t FP_3105[5] = {
    0xF9000BF3A9BE7BFDULL, 0xF9000BF5A9BD7BFDULL, 0xF9000BF3A9BE7BFDULL,
    0xA90B7BFDD10443FFULL, 0xA90157F6A9BD7BFDULL};

uintptr_t FunctionsAddress::PythonUtils_setExecLocked        = 0;
uintptr_t FunctionsAddress::PythonUtils_PyGILState_EnsureMC  = 0;
uintptr_t FunctionsAddress::PythonUtils_PyImport_AddModuleMC = 0;
uintptr_t FunctionsAddress::PythonUtils_PyModule_GetDictMC   = 0;
uintptr_t FunctionsAddress::PythonUtils_PyRun_StringFlagsMC  = 0;
uintptr_t FunctionsAddress::PythonUtils_PyGILState_ReleaseMC = 0;

// ── 建筑工具用到的游戏函数 ──────────────────────────────────────────
uintptr_t FunctionsAddress::Actor_getClientInstance = 0;
uintptr_t FunctionsAddress::Actor_getDimension = 0;
uintptr_t FunctionsAddress::Minecraft_update_Hook = 0;
uintptr_t FunctionsAddress::Level_Render_Hook = 0;
uintptr_t FunctionsAddress::ClientInstance_getCamera = 0;
uintptr_t FunctionsAddress::newReceivePacket_Hook = 0;

namespace {

// 建筑侧地址表。只有 3.9.15 有核验过的值；其余版本留 0，建筑功能整体
// 关闭而不是带着旧偏移跳进未映射内存。
struct BuildOffsets {
    uintptr_t actor_get_client_instance;
    uintptr_t actor_get_dimension;
    uintptr_t minecraft_update_hook;
    uintptr_t level_render_hook;
    uintptr_t client_instance_get_camera;
    uintptr_t new_receive_packet_hook;
};

constexpr BuildOffsets kBuildOffsets3915{
    0x74C673C,   // Actor::getClientInstance
    0xD8E7554,   // Actor::getDimension
    0x74C79FC,   // Actor::normalTick
    0x7B73A54,   // Level::_render
    0x7463BF4,   // ClientInstance::getCamera
    0xA7905A4,   // newReceivePacket
};

// 3.10.5.301327（build-id a494de85…）。
//
// 不是靠平移算出来的：这版同一区段内的平移量已经互不相同，抄旧偏移会把
// hook 装进别的函数中间。这六个值来自 tools/hooks.json 里每条规则的特征
// 窗口，对该镜像全文扫描后唯一命中；其中三个在已有记录里能核对
// （getClientInstance / getDimension / _render 完全一致），
// Minecraft_update_Hook 也正好落在 LocalPlayer vtable 第 29 槽。
constexpr BuildOffsets kBuildOffsets3105{
    0x759A408,   // Actor::getClientInstance
    0xCE8EBA0,   // Actor::getDimension
    0x759B6C8,   // Actor::normalTick
    0x7CFCB90,   // Level::_render
    0x7548774,   // ClientInstance::getCamera
    0xA3A6464,   // newReceivePacket
};

void applyBuildOffsets(const BuildOffsets& o) {
    FunctionsAddress::Actor_getClientInstance  = o.actor_get_client_instance;
    FunctionsAddress::Actor_getDimension       = o.actor_get_dimension;
    FunctionsAddress::Minecraft_update_Hook    = o.minecraft_update_hook;
    FunctionsAddress::Level_Render_Hook        = o.level_render_hook;
    FunctionsAddress::ClientInstance_getCamera = o.client_instance_get_camera;
    FunctionsAddress::newReceivePacket_Hook    = o.new_receive_packet_hook;
}

void clearBuildOffsets() {
    FunctionsAddress::Actor_getClientInstance  = 0;
    FunctionsAddress::Actor_getDimension       = 0;
    FunctionsAddress::Minecraft_update_Hook    = 0;
    FunctionsAddress::Level_Render_Hook        = 0;
    FunctionsAddress::ClientInstance_getCamera = 0;
    FunctionsAddress::newReceivePacket_Hook    = 0;
}

}  // namespace

static std::mutex g_detectMutex;
static bool g_detected = false;

namespace {

struct Img {
    uintptr_t base = 0;
    unsigned char id[20] = {0};
    bool hasId = false;
    struct Seg { uintptr_t lo, hi; bool exec; } seg[24];
    int n = 0;
};

struct MapLine {
    uintptr_t lo, hi;
    unsigned long long off;
    bool r, x;
    char path[256];
};

bool looksLikeElf(uintptr_t p) {
    if (p == 0) return false;
    return *reinterpret_cast<const uint32_t*>(p) == 0x464c457f;
}

bool readBuildIdMem(uintptr_t base, unsigned char out[20]) {
    if (base == 0 || !looksLikeElf(base)) return false;
    const auto* eh = reinterpret_cast<const Elf64_Ehdr*>(base);
    if (eh->e_ident[EI_CLASS] != ELFCLASS64) return false;
    if (eh->e_phnum == 0 || eh->e_phnum > 128) return false;
    const auto* ph = reinterpret_cast<const Elf64_Phdr*>(base + eh->e_phoff);
    for (int i = 0; i < eh->e_phnum; ++i) {
        if (ph[i].p_type != PT_NOTE) continue;
        const char* p = reinterpret_cast<const char*>(base + ph[i].p_vaddr);
        const char* end = p + ph[i].p_memsz;
        if (end <= p) continue;
        while (p + sizeof(Elf64_Nhdr) <= end) {
            const auto* nh = reinterpret_cast<const Elf64_Nhdr*>(p);
            const char* name = p + sizeof(Elf64_Nhdr);
            const size_t nameSz = (nh->n_namesz + 3u) & ~size_t(3u);
            const char* desc = name + nameSz;
            if (desc + nh->n_descsz > end) break;
            if (nh->n_type == NT_GNU_BUILD_ID && nh->n_namesz == 4 &&
                memcmp(name, "GNU", 3) == 0 && nh->n_descsz == 20) {
                memcpy(out, desc, 20);
                return true;
            }
            p = desc + ((nh->n_descsz + 3u) & ~size_t(3u));
        }
    }
    return false;
}

struct FileSeg { uint64_t off, vaddr, memsz; uint32_t flags; };

int readFileLoads(FILE* f, FileSeg* out, int max) {
    Elf64_Ehdr eh;
    if (fseek(f, 0, SEEK_SET) != 0 || fread(&eh, sizeof(eh), 1, f) != 1) return 0;
    if (memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 || eh.e_ident[EI_CLASS] != ELFCLASS64) return 0;
    if (eh.e_phnum == 0 || eh.e_phnum > 128) return 0;
    int n = 0;
    for (int i = 0; i < eh.e_phnum && n < max; ++i) {
        Elf64_Phdr ph;
        if (fseek(f, (long)(eh.e_phoff + (uint64_t)i * sizeof(Elf64_Phdr)), SEEK_SET) != 0) break;
        if (fread(&ph, sizeof(ph), 1, f) != 1) break;
        if (ph.p_type != PT_LOAD) continue;
        out[n].off = ph.p_offset;
        out[n].vaddr = ph.p_vaddr;
        out[n].memsz = ph.p_memsz;
        out[n].flags = ph.p_flags;
        ++n;
    }
    return n;
}

bool readBuildIdFile(FILE* f, unsigned char out[20]) {
    Elf64_Ehdr eh;
    if (fseek(f, 0, SEEK_SET) != 0 || fread(&eh, sizeof(eh), 1, f) != 1) return false;
    if (memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 || eh.e_ident[EI_CLASS] != ELFCLASS64) return false;
    if (eh.e_phnum == 0 || eh.e_phnum > 128) return false;
    unsigned char buf[1024];
    for (int i = 0; i < eh.e_phnum; ++i) {
        Elf64_Phdr ph;
        if (fseek(f, (long)(eh.e_phoff + (uint64_t)i * sizeof(Elf64_Phdr)), SEEK_SET) != 0) return false;
        if (fread(&ph, sizeof(ph), 1, f) != 1) return false;
        if (ph.p_type != PT_NOTE || ph.p_filesz == 0) continue;
        const size_t want = ph.p_filesz < sizeof(buf) ? (size_t)ph.p_filesz : sizeof(buf);
        if (fseek(f, (long)ph.p_offset, SEEK_SET) != 0) continue;
        if (fread(buf, 1, want, f) != want) continue;
        const char* p = reinterpret_cast<const char*>(buf);
        const char* end = p + want;
        while (p + sizeof(Elf64_Nhdr) <= end) {
            const auto* nh = reinterpret_cast<const Elf64_Nhdr*>(p);
            const char* name = p + sizeof(Elf64_Nhdr);
            const size_t nameSz = (nh->n_namesz + 3u) & ~size_t(3u);
            const char* desc = name + nameSz;
            if (desc + nh->n_descsz > end) break;
            if (nh->n_type == NT_GNU_BUILD_ID && nh->n_namesz == 4 &&
                memcmp(name, "GNU", 3) == 0 && nh->n_descsz == 20) {
                memcpy(out, desc, 20);
                return true;
            }
            p = desc + ((nh->n_descsz + 3u) & ~size_t(3u));
        }
    }
    return false;
}

int readMapLines(MapLine* out, int max) {
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    int n = 0;
    char line[600];
    while (n < max && fgets(line, sizeof(line), f)) {
        unsigned long long lo = 0, hi = 0, off = 0;
        char perms[8] = {0};
        if (sscanf(line, "%llx-%llx %7s %llx", &lo, &hi, perms, &off) != 4) continue;
        MapLine& m = out[n];
        m.lo = (uintptr_t)lo;
        m.hi = (uintptr_t)hi;
        m.off = off;
        m.r = perms[0] == 'r';
        m.x = perms[2] == 'x';
        m.path[0] = 0;
        const char* slash = strchr(line, '/');
        if (slash) {
            size_t len = strlen(slash);
            while (len && (slash[len - 1] == '\n' || slash[len - 1] == '\r')) --len;
            if (len >= sizeof(m.path)) len = sizeof(m.path) - 1;
            memcpy(m.path, slash, len);
            m.path[len] = 0;
        }
        ++n;
    }
    fclose(f);
    return n;
}

struct ProbeCtx { Img* img; bool found; };

int probeCb(dl_phdr_info* info, size_t, void* data) {
    auto* c = static_cast<ProbeCtx*>(data);
    if (!info->dlpi_name || !info->dlpi_name[0]) return 0;
    const char* slash = strrchr(info->dlpi_name, '/');
    const char* leaf = slash ? slash + 1 : info->dlpi_name;
    if (strcmp(leaf, "libminecraftpe.so") != 0) return 0;
    Img* img = c->img;
    img->base = static_cast<uintptr_t>(info->dlpi_addr);
    img->n = 0;
    for (int i = 0; i < info->dlpi_phnum && img->n < 24; ++i) {
        const ElfW(Phdr)& p = info->dlpi_phdr[i];
        if (p.p_type != PT_LOAD) continue;
        const uintptr_t lo = img->base + p.p_vaddr;
        img->seg[img->n].lo = lo;
        img->seg[img->n].hi = lo + p.p_memsz;
        img->seg[img->n].exec = (p.p_flags & PF_X) != 0;
        img->n++;
    }
    img->hasId = readBuildIdMem(img->base, img->id);
    c->found = true;
    return 1;
}

static MapLine g_maps[256];
static int g_mapsN = 0;

Img probeLib() {
    Img img;
    ProbeCtx ctx{&img, false};
    dl_iterate_phdr(probeCb, &ctx);
    if (ctx.found && img.base != 0 && img.n > 0) return img;

    g_mapsN = readMapLines(g_maps, 256);
    int idx[64], cnt = 0;
    for (int i = 0; i < g_mapsN && cnt < 64; ++i) {
        if (strstr(g_maps[i].path, "libminecraftpe.so") != nullptr) idx[cnt++] = i;
    }
    if (cnt == 0) {
        LOGE("probe: /proc/self/maps 里没有 libminecraftpe.so");
        return Img{};
    }

    for (int k = 0; k < cnt; ++k) {
        const MapLine& m = g_maps[idx[k]];
        if (m.off != 0 || !m.r || (m.hi - m.lo) < 0x40) continue;
        Img t;
        t.base = m.lo;
        for (int k2 = 0; k2 < cnt && t.n < 24; ++k2) {
            const MapLine& s = g_maps[idx[k2]];
            t.seg[t.n].lo = s.lo;
            t.seg[t.n].hi = s.hi;
            t.seg[t.n].exec = s.x;
            t.n++;
        }
        t.hasId = readBuildIdMem(t.base, t.id);
        LOGI("probe: base=0x%lx 来自内存 ELF 头", (unsigned long)t.base);
        return t;
    }

    for (int k = 0; k < cnt; ++k) {
        const MapLine& m = g_maps[idx[k]];
        if (m.path[0] != '/' || strstr(m.path, ".apk") != nullptr) continue;
        FILE* f = fopen(m.path, "rb");
        if (!f) continue;
        FileSeg fs[12];
        const int sn = readFileLoads(f, fs, 12);
        unsigned char id[20];
        const bool hasId = sn > 0 && readBuildIdFile(f, id);
        fclose(f);
        if (sn == 0) continue;
        for (int s = 0; s < sn; ++s) {
            if (fs[s].off != m.off || m.lo < fs[s].vaddr) continue;
            Img t;
            t.base = m.lo - fs[s].vaddr;
            t.n = 0;
            for (int s2 = 0; s2 < sn && t.n < 24; ++s2) {
                t.seg[t.n].lo = t.base + fs[s2].vaddr;
                t.seg[t.n].hi = t.base + fs[s2].vaddr + fs[s2].memsz;
                t.seg[t.n].exec = (fs[s2].flags & PF_X) != 0;
                t.n++;
            }
            t.hasId = hasId;
            if (hasId) memcpy(t.id, id, 20);
            LOGI("probe: base=0x%lx 由文件段反推（maps off=0x%llx path=%s）",
                 (unsigned long)t.base, m.off, m.path);
            return t;
        }
    }

    static bool dumped = false;
    if (dumped) return Img{};
    dumped = true;
    LOGE("probe: 无法确定 base，maps 里的 libminecraftpe.so 行：");
    for (int k = 0; k < cnt && k < 10; ++k) {
        const MapLine& m = g_maps[idx[k]];
        LOGE("  %lx-%lx %c%c%c off=%llx %s", (unsigned long)m.lo, (unsigned long)m.hi,
             m.r ? 'r' : '-', '-', m.x ? 'x' : '-', m.off, m.path);
    }
    return Img{};
}

bool inExec(const Img& img, uintptr_t abs, size_t len = 4) {
    for (int i = 0; i < img.n; ++i) {
        if (img.seg[i].exec && abs >= img.seg[i].lo && abs + len <= img.seg[i].hi) return true;
    }
    return false;
}

int fpScore(const Img& img, const uintptr_t* t, const uint64_t* fp) {
    int s = 0;
    for (int i = 1; i <= 5; ++i) {
        const uintptr_t abs = img.base + t[i];
        if (!inExec(img, abs, 8)) continue;
        uint64_t v = 0;
        memcpy(&v, reinterpret_cast<const void*>(abs), 8);
        if (v == fp[i - 1]) s++;
    }
    return s;
}

void zeroAll() {
    FunctionsAddress::PythonUtils_setExecLocked        = 0;
    FunctionsAddress::PythonUtils_PyGILState_EnsureMC  = 0;
    FunctionsAddress::PythonUtils_PyImport_AddModuleMC = 0;
    FunctionsAddress::PythonUtils_PyModule_GetDictMC   = 0;
    FunctionsAddress::PythonUtils_PyRun_StringFlagsMC  = 0;
    FunctionsAddress::PythonUtils_PyGILState_ReleaseMC = 0;
    clearBuildOffsets();
}

void dumpSegs(const Img& img, const uintptr_t* table, const char* name) {
    LOGE("dump: base=0x%lx segs=%d", (unsigned long)img.base, img.n);
    for (int i = 0; i < img.n && i < 8; ++i) {
        LOGE("dump: seg%d 0x%lx-0x%lx %c", i,
             (unsigned long)img.seg[i].lo, (unsigned long)img.seg[i].hi,
             img.seg[i].exec ? 'x' : '-');
    }
    if (!table) return;
    for (int i = 1; i <= 5; ++i) {
        const uintptr_t abs = img.base + table[i];
        LOGE("dump: %s[%d] off=0x%lx abs=0x%lx exec=%d",
             name, i, (unsigned long)table[i], (unsigned long)abs, inExec(img, abs) ? 1 : 0);
    }
}

const char* buildName(const unsigned char* id) {
    if (memcmp(id, BUILD_39, 20) == 0) return "3.9.15";
    if (memcmp(id, BUILD_310, 20) == 0) return "3.10.0";
    if (memcmp(id, BUILD_3105, 20) == 0) return "3.10.5";
    return "unknown";
}

struct VerTable {
    const char* name;
    const char* verPrefix;
    const uintptr_t* addr;
    const uint64_t* fp;
    const unsigned char* bid;
};

const VerTable kTables[] = {
    {"3.9.15", "3.9", ADDR_3915, FP_3915, BUILD_39},
    {"3.10.0", "3.10.0", ADDR_310, FP_310, BUILD_310},
    {"3.10.5", "3.10.5", ADDR_3105, FP_3105, BUILD_3105},
};
constexpr int kTableCount = int(sizeof(kTables) / sizeof(kTables[0]));

}

uintptr_t FunctionsAddress::moduleBase() {
    return probeLib().base;
}

static char g_reason[256] = "detect not run";
static char g_version[64] = {0};

void FunctionsAddress::setVersion(const char* v) {
    if (!v) return;
    snprintf(g_version, sizeof(g_version), "%s", v);
}

bool FunctionsAddress::pyTrace() {
    static const bool on = (access("/data/local/tmp/texinject_py_trace", F_OK) == 0);
    return on;
}

// ── 3.10.5 的源码编译拦截 ────────────────────────────────────────────
//
// 3.10.5 的 PyRun_StringFlags 在编译成功之后，无条件再调一次拦截函数
// （相对 PyRun_StringFlags +0xCC，实际地址 0x119314D4）：它打一行
// "[SCRIPT_SEC] runtime compile blocked"、设 RuntimeError("exec failed")
// 并返回 NULL。于是外面看到的永远是 exec failed，代码一次都没执行。
//
// 那条 bl 换成 nop 就绕开了：x0 里还是刚编译好的 code object，紧跟的
// cbz 不跳，下一条就是真正执行。改之前先比对原指令，对不上（版本不同 /
// 已经改过）就原样放过，不动别人的内存。
namespace {

// 拦截函数被 6 个入口调用，不只 PyRun_StringFlags。
//
// 只打 PyRun_StringFlags 内那一处（+0xCC）不够：日志里 SCRIPT_SEC 仍会
// 打出来，建筑的所有 Python 调用全部 fail。这 6 个位置的 BL 目标都是
// 同一个拦截函数，逐一改掉才不会漏。
//
// 偏移来自对 3.10.5.301327 镜像（build-id a494de85）的全文扫描：
// 找出所有 BL 到 0x119314D4 的指令。
constexpr uintptr_t kBlockCalls[] = {
    0x1190AC40,  // 0x1190AA34 + 0x20C
    0x11946C18,  // 0x119469B4 + 0x264
    0x11980778,  // 0x119806F0 + 0x88
    0x119C49FC,  // PyRun_File 一带
    0x119C4B38,  // PyRun_StringFlags + 0xCC
    0x119C599C,  // 0x119C5938 + 0x64
};
constexpr uint32_t kNop = 0xD503201FU;

// 只对确实是 BL 的位置动手；不是 BL 说明版本不同或已打过，原样放过。
bool patchScriptCompileBlock(uintptr_t base, uintptr_t /*run_flags_rva*/) {
    if (base == 0) return false;
    const long page = sysconf(_SC_PAGESIZE);
    int patched = 0;
    for (uintptr_t site_rva : kBlockCalls) {
        auto* site = reinterpret_cast<uint32_t*>(base + site_rva);
        const uint32_t original = *site;
        if ((original & 0xFC000000U) != 0x94000000U) {
            LOGI("script block: 0x%lx is not a BL (0x%08x), skipped",
                 (unsigned long)site_rva, original);
            continue;
        }
        void* page_start = reinterpret_cast<void*>(
            reinterpret_cast<uintptr_t>(site) & ~static_cast<uintptr_t>(page - 1));
        if (mprotect(page_start, static_cast<size_t>(page),
                     PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
            LOGE("script block: mprotect failed at 0x%lx: %s",
                 (unsigned long)site_rva, std::strerror(errno));
            continue;
        }
        *site = kNop;
        __builtin___clear_cache(reinterpret_cast<char*>(site),
                                reinterpret_cast<char*>(site) + 4);
        mprotect(page_start, static_cast<size_t>(page), PROT_READ | PROT_EXEC);
        ++patched;
    }
    LOGI("script block: patched %d/%zu call sites", patched,
         sizeof(kBlockCalls) / sizeof(kBlockCalls[0]));
    return patched > 0;
}

}  // namespace

void FunctionsAddress::detect() {
    std::lock_guard<std::mutex> lock(g_detectMutex);
    if (g_detected) return;
    g_detected = true;

    if (access("/data/local/tmp/texinject_no_python", F_OK) == 0) {
        zeroAll();
        snprintf(g_reason, sizeof(g_reason), "disabled by /data/local/tmp/texinject_no_python");
        LOGE("detect: /data/local/tmp/texinject_no_python 存在，禁用 native Python");
        return;
    }

    const Img img = probeLib();
    if (img.base == 0) {
        zeroAll();
        snprintf(g_reason, sizeof(g_reason), "libminecraftpe base not found");
        LOGE("detect: 拿不到 base，禁用 native Python");
        return;
    }

    if (img.hasId) {
        LOGI("detect: base=0x%lx build-id=%02x%02x%02x%02x%02x%02x%02x%02x (%s) execSeg=%d",
             (unsigned long)img.base, img.id[0], img.id[1], img.id[2], img.id[3],
             img.id[4], img.id[5], img.id[6], img.id[7], buildName(img.id), img.n);
    } else {
        LOGE("detect: base=0x%lx 但读不到 build-id，execSeg=%d", (unsigned long)img.base, img.n);
    }

    const VerTable* sel = nullptr;

    if (g_version[0]) {
        for (int i = 0; i < kTableCount; ++i) {
            const size_t n = strlen(kTables[i].verPrefix);
            if (strncmp(g_version, kTables[i].verPrefix, n) == 0) {
                sel = &kTables[i];
                break;
            }
        }
        LOGI("detect: 版本号 %s -> %s", g_version, sel ? sel->name : "未收录");
    }

    if (!sel && img.hasId) {
        for (int i = 0; i < kTableCount; ++i) {
            if (memcmp(img.id, kTables[i].bid, 20) == 0) {
                sel = &kTables[i];
                break;
            }
        }
    }

    if (!sel) {
        int hit = -1, ties = 0;
        for (int i = 0; i < kTableCount; ++i) {
            if (fpScore(img, kTables[i].addr, kTables[i].fp) == 5) {
                if (hit < 0) hit = i;
                else ++ties;
            }
        }
        if (hit >= 0 && ties == 0) sel = &kTables[hit];
        LOGE("detect: 未知构建, 指纹命中=%d 并列=%d", hit, ties);
    }

    const int score = sel ? fpScore(img, sel->addr, sel->fp) : -1;
    if (!sel || score != 5) {
        zeroAll();
        dumpSegs(img, ADDR_3915, "3.9.15");
        dumpSegs(img, ADDR_310, "3.10.0");
        dumpSegs(img, ADDR_3105, "3.10.5");
        snprintf(g_reason, sizeof(g_reason), "table %s untrusted, fpScore=%d/5, build=%s, execSeg=%d",
                 sel ? sel->name : "none", score,
                 img.hasId ? buildName(img.id) : "no-build-id", img.n);
        LOGE("detect: 地址表不可信（%s），禁用 native Python", sel ? sel->name : "none");
        return;
    }

    const uintptr_t* table = sel->addr;
    PythonUtils_setExecLocked        = table[0];
    PythonUtils_PyGILState_EnsureMC  = table[1];
    PythonUtils_PyImport_AddModuleMC = table[2];
    PythonUtils_PyModule_GetDictMC   = table[3];
    PythonUtils_PyRun_StringFlagsMC  = table[4];
    PythonUtils_PyGILState_ReleaseMC = table[5];

    // 建筑侧：沿用同一张表的版本归属。
    if (sel->addr == ADDR_3915) {
        applyBuildOffsets(kBuildOffsets3915);
    } else if (sel->addr == ADDR_3105) {
        applyBuildOffsets(kBuildOffsets3105);
    } else {
        // 3.10.0 尚无核验值，建筑功能整体关闭而不是硬装。
        clearBuildOffsets();
    }
    // 3.10.x 之后 PyRun_StringFlags 编译完会无条件被拦截，必须打掉那条 bl。
    // 3.9.15 走的是 setExecLocked 全局开关，不需要改代码。
    if (sel->addr == ADDR_3105 || sel->addr == ADDR_310) {
        // 用 detect 自己探测到的基址，不用 Main::getBaseAddress()。
        //
        // 两者可能不同：detect 走 probeLib() 校验过的映像起点，而 Main 里
        // 存的是 /proc/self/maps 匹配到的第一行。用错的基址算出来的补丁地址
        // 会落在别的映射上 —— 写进去也不报错，但拦截一点没改，表现就是
        // 「patched 6/6」而 SCRIPT_SEC 照旧打印。
        const uintptr_t patch_base = img.base != 0 ? img.base : Main::getBaseAddress();
        patchScriptCompileBlock(patch_base, PythonUtils_PyRun_StringFlagsMC);
        LOGI("script block: patch base=0x%lx (detect=%d, main=0x%lx)",
             (unsigned long)patch_base, img.base != 0 ? 1 : 0,
             (unsigned long)Main::getBaseAddress());
    }

    snprintf(g_reason, sizeof(g_reason), "ok %s", sel->name);

    LOGI("detect: 使用 %s 地址表 GIL=0x%lx addmod=0x%lx getdict=0x%lx runstr=0x%lx rel=0x%lx",
         sel->name, (unsigned long)PythonUtils_PyGILState_EnsureMC,
         (unsigned long)PythonUtils_PyImport_AddModuleMC,
         (unsigned long)PythonUtils_PyModule_GetDictMC,
         (unsigned long)PythonUtils_PyRun_StringFlagsMC,
         (unsigned long)PythonUtils_PyGILState_ReleaseMC);
}

const char* FunctionsAddress::detectReason() {
    return g_reason;
}
