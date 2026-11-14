#ifndef MINECRAFT_MAIN_H
#define MINECRAFT_MAIN_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <android/log.h>

struct Vec3 {
    float x, y, z;
    Vec3() : x(0), y(0), z(0) {}
    Vec3(float _x, float _y, float _z) : x(_x), y(_y), z(_z) {}
};

class Main {
public:
    static std::atomic<uintptr_t> baseAddress;

    static uintptr_t getBaseAddress() noexcept {
        return baseAddress.load(std::memory_order_acquire);
    }

    static void setBaseAddress(uintptr_t value) noexcept {
        baseAddress.store(value, std::memory_order_release);
    }
};

// 只在整段 [target, target+required_size) 落在 libminecraftpe.so 的某个
// 可执行 PT_LOAD 段里时返回 true。建筑侧的每个偏移在使用前都过这里，
// 版本对不上时是一个明确的失败，而不是跳到未映射内存。
bool ResolveMinecraftExecutableOffset(uintptr_t base_address, uintptr_t offset,
                                      size_t required_size, uintptr_t* address);

// 建筑投影需要 tick + 渲染两条 hook；面板打开投影时单独调用，
// 不依赖启动期是否已经装过。
bool EnsureBuildProjectionHooksReady();

#endif
