#include "Resolver.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

#include "GameImage.h"
#include "SignatureCatalog.h"
#include "tp/FunctionsAddress.h"

#if defined(__ANDROID__)
#include <android/log.h>
#define ADDRSCAN_LOG(...) __android_log_print(ANDROID_LOG_INFO, "BuildToolsAddr", __VA_ARGS__)
#else
#define ADDRSCAN_LOG(...) ((void)0)
#endif

namespace buildtools {
namespace addrscan {

namespace catalog {
#include "SignatureCatalog.inc"
}  // namespace catalog

namespace {

using arm64::ScanStatus;
using arm64::Signature;

std::mutex g_mutex;
std::atomic<uintptr_t> g_published{0};
std::vector<uintptr_t> g_members;
std::vector<arm64::CodeRange> g_code_ranges;
std::vector<arm64::CodeRange> g_readable;
std::string g_status = "等待 libminecraftpe.so 加载";
std::atomic<bool> g_failed_permanently{false};

const uint8_t* at(const std::vector<arm64::CodeRange>& ranges, uintptr_t address,
                  size_t size) {
    return RangeAt(ranges, address, size);
}

const uint8_t* readable(const Image& image, uintptr_t address, size_t size) {
    if (const uint8_t* code = at(image.code, address, size)) return code;
    return at(image.data, address, size);
}

bool matches(const Image& image, uintptr_t address, const Signature& signature) {
    if (!signature.count) return true;
    const uint8_t* code = at(image.code, address, signature.count * sizeof(uint32_t));
    if (!code) return false;
    for (size_t index = 0; index < signature.count; ++index) {
        uint32_t word = 0;
        std::memcpy(&word, code + index * sizeof(uint32_t), sizeof(word));
        // Only the fixed bits participate: the reviewed value must be masked
        // too, otherwise a relative immediate that the mask deliberately drops
        // makes every window miss.
        if (((word ^ signature.instructions[index].value) &
             signature.instructions[index].mask) != 0U) {
            return false;
        }
    }
    return true;
}

// Decodes the ADRP + consumer pair at owner + word*4 and publishes the page
// address it references.
bool reference(const Image& image, uintptr_t owner, size_t adrp_word, size_t consumer_word,
               uintptr_t* result) {
    const uint8_t* first = at(image.code, owner + adrp_word * 4U, sizeof(uint32_t));
    const uint8_t* second = at(image.code, owner + consumer_word * 4U, sizeof(uint32_t));
    if (!first || !second) return false;
    uint32_t adrp = 0;
    uint32_t consumer = 0;
    std::memcpy(&adrp, first, sizeof(adrp));
    std::memcpy(&consumer, second, sizeof(consumer));
    return arm64::DecodeAdrpReference(owner + adrp_word * 4U, adrp, consumer, result);
}

bool buildAccepted(const std::vector<uint8_t>& build_id, std::string* label,
                   std::string* error) {
    for (const AllowedBuild& build : catalog::kAllowedBuilds) {
        if (build.size == build_id.size() && build.bytes &&
            std::memcmp(build.bytes, build_id.data(), build.size) == 0) {
            if (label) *label = build.label ? build.label : "";
            return true;
        }
    }
    if (error) {
        *error = "当前游戏构建（build-id " + FormatBuildId(build_id) +
                 "）未登记，拒绝在未复核的镜像上解析地址";
    }
    return false;
}

bool fail(const std::string& message, bool retryable, std::string* error) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_status = message;
    g_failed_permanently.store(!retryable, std::memory_order_release);
    if (error) *error = message;
    ADDRSCAN_LOG("address resolution failed: %s", message.c_str());
    return false;
}

}  // namespace

bool Initialize(std::string* error, bool* retryable) {
    if (retryable) *retryable = true;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_published.load(std::memory_order_acquire) != 0) return true;
        if (g_failed_permanently.load(std::memory_order_acquire)) {
            if (error) *error = g_status;
            if (retryable) *retryable = false;
            return false;
        }
    }

    Image image;
    if (!CaptureGameImage(&image)) {
        return fail("等待 libminecraftpe.so 加载", true, error);
    }
    const bool resolved = ResolveImage(image, error);
    if (retryable) *retryable = !g_failed_permanently.load(std::memory_order_acquire);
    return resolved;
}

bool ResolveImage(const Image& image, std::string* error) {
    if (catalog::kAllowedBuildCount == 0) {
        return fail("特征目录没有登记任何已复核构建", false, error);
    }
    std::string label;
    if (!buildAccepted(image.build_id, &label, error)) {
        return fail(error ? *error : std::string("构建未登记"), false, error);
    }
    if (catalog::kRuleCount == 0 || catalog::kBindingCount == 0) {
        return fail("构建 " + label + " 的特征集尚未生成，地址保持未发布", false, error);
    }

    std::vector<Signature> patterns;
    patterns.reserve(catalog::kRuleCount * 3U);
    for (const Rule& rule : catalog::kRules) {
        if (rule.first.count) patterns.push_back(rule.first);
        if (rule.second.count) patterns.push_back(rule.second);
        if (rule.body.count) patterns.push_back(rule.body);
    }

    arm64::ScanIndex index;
    const bool indexed = index.Build(image.code.data(), image.code.size(), patterns.data(),
                                     patterns.size());

    std::vector<std::pair<uint64_t, uintptr_t>> resolved;
    resolved.reserve(catalog::kRuleCount);
    for (const Rule& rule : catalog::kRules) {
        const arm64::ScanResult hit = arm64::Scan(image.code.data(), image.code.size(),
                                                  rule.first, indexed ? &index : nullptr);
        uintptr_t value = hit.address;
        bool valid = hit.status == ScanStatus::Unique;

        if (valid && rule.kind == RuleKind::Branch) {
            const uint8_t* call = at(image.code, value + rule.branch_word * 4U, sizeof(uint32_t));
            uint32_t word = 0;
            if (call) std::memcpy(&word, call, sizeof(word));
            const int64_t delta = int64_t(static_cast<int32_t>(word << 6U) >> 6) * 4;
            valid = call && (word & 0xfc000000U) == 0x94000000U;
            value = uintptr_t(int64_t(value + rule.branch_word * 4U) + delta);
        }

        if (valid && (rule.kind == RuleKind::Data || rule.kind == RuleKind::VerifiedReference)) {
            valid = reference(image, hit.address, rule.first_adrp_word, rule.first_consumer_word,
                              &value);
            if (valid && rule.second.count) {
                const arm64::ScanResult other = arm64::Scan(
                    image.code.data(), image.code.size(), rule.second,
                    indexed ? &index : nullptr);
                uintptr_t second = 0;
                valid = other.status == ScanStatus::Unique &&
                        reference(image, other.address, rule.second_adrp_word,
                                  rule.second_consumer_word, &second) &&
                        second == value;
            }
            const size_t alignment = rule.target_alignment ? rule.target_alignment : 1U;
            valid = valid && value != 0 && (value & (alignment - 1U)) == 0 &&
                    (rule.writable ? ContainsRange(image.writable, value, rule.target_size)
                                   : readable(image, value, rule.target_size) != nullptr);
            if (valid && rule.target_byte_count) {
                const uint8_t* bytes = readable(image, value, rule.target_byte_count);
                valid = bytes && std::memcmp(bytes, rule.target_bytes, rule.target_byte_count) == 0;
            }
        } else {
            valid = valid && (value & 3U) == 0U && at(image.code, value, sizeof(uint32_t)) &&
                    matches(image, value, rule.body);
        }

        if (!valid) {
            return fail("特征识别失败，key=" + std::to_string(rule.key), false, error);
        }
        resolved.emplace_back(rule.key, value);
    }

    std::vector<uintptr_t> accepted;
    accepted.reserve(catalog::kBindingCount);
    for (const Binding& binding : catalog::kBindings) {
        const auto found = std::find_if(resolved.begin(), resolved.end(),
                                        [&](const auto& item) { return item.first == binding.key; });
        if (found == resolved.end() || !binding.field) {
            if (!binding.required) {
                ADDRSCAN_LOG("optional binding unavailable: key=%llu",
                             static_cast<unsigned long long>(binding.key));
                continue;
            }
            return fail("绑定缺失，key=" + std::to_string(binding.key), false, error);
        }
        *binding.field = found->second - image.base;
        accepted.push_back(found->second);
    }
    std::sort(accepted.begin(), accepted.end());

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_members = std::move(accepted);
        g_code_ranges = image.code;
        g_readable = image.code;
        g_readable.insert(g_readable.end(), image.data.begin(), image.data.end());
        g_status = "已就绪：" + label + " / " + std::to_string(catalog::kRuleCount) + " 项特征";
        g_published.store(image.base, std::memory_order_release);
        ADDRSCAN_LOG("address resolution ready: %s", g_status.c_str());
    }
    if (error) error->clear();
    return true;
}

bool Ready() noexcept { return g_published.load(std::memory_order_acquire) != 0; }

size_t RuleCount() noexcept { return catalog::kRuleCount; }

size_t BindingCount() noexcept { return catalog::kBindingCount; }

const Binding* Bindings() noexcept { return catalog::kBindings.data(); }

uintptr_t Base() noexcept { return g_published.load(std::memory_order_acquire); }

bool ReadImage(uintptr_t address, void* output, size_t size) noexcept {
    if (!address || !size || !output) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!ContainsRange(g_readable, address, size)) return false;
    std::memcpy(output, reinterpret_cast<const void*>(address), size);
    return true;
}

bool CodeOffset(uintptr_t base, uintptr_t offset, size_t size, uintptr_t* address) noexcept {
    if (!address) return false;
    *address = 0;
    if (!base || base != Base() || offset > UINTPTR_MAX - base) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!ContainsRange(g_code_ranges, base + offset, size)) return false;
    *address = base + offset;
    return true;
}

bool IsResolvedOffset(uintptr_t offset) noexcept {
    std::lock_guard<std::mutex> lock(g_mutex);
    return std::binary_search(g_members.begin(), g_members.end(), Base() + offset);
}

size_t ResolvedCount() noexcept {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_members.size();
}

std::string StatusText() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_status;
}

}  // namespace addrscan
}  // namespace buildtools
