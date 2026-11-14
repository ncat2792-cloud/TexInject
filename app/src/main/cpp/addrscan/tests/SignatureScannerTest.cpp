#include "../Arm64SignatureScanner.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

// Host-side proof for the scanner's two promises: a pattern that is not unique
// never yields an address, and the PC-relative masking keeps a signature valid
// when only the surrounding code moved.
namespace {

using namespace buildtools::arm64;

int failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::printf("FAIL %s\n", what);
        ++failures;
    }
}

uint32_t adrp(int reg, int64_t delta) {
    const int64_t pages = delta >> 12;
    const uint32_t imm = static_cast<uint32_t>(pages) & 0x1fffffU;
    return 0x90000000U | ((imm & 3U) << 29U) | (((imm >> 2U) & 0x7ffffU) << 5U) |
           static_cast<uint32_t>(reg);
}

uint32_t addImmediate(int destination, int source, uint32_t immediate) {
    return 0x91000000U | ((immediate & 0xfffU) << 10U) |
           (static_cast<uint32_t>(source) << 5U) | static_cast<uint32_t>(destination);
}

uint32_t branchLink(int64_t delta) {
    return 0x94000000U | (static_cast<uint32_t>(delta >> 2) & 0x03ffffffU);
}

std::vector<uint8_t> bytes(const std::vector<uint32_t>& words) {
    std::vector<uint8_t> out(words.size() * sizeof(uint32_t));
    std::memcpy(out.data(), words.data(), out.size());
    return out;
}

Signature signature(const std::vector<uint32_t>& words) {
    static std::vector<std::vector<Instruction>> storage;
    storage.emplace_back();
    for (uint32_t word : words) {
        storage.back().push_back({word, InstructionMask(word)});
    }
    return {storage.back().data(), storage.back().size(), 0};
}

const std::vector<uint32_t> kBody{0xd10143ffU, 0xa9027bfdU, 0xa90357f6U, 0xa9044ff4U};

}  // namespace

int main() {
    // 1. A unique pattern publishes its own address.
    {
        auto text = bytes(kBody);
        CodeRange range{text.data(), text.size(), 0x1000};
        const auto pattern = signature(kBody);
        const auto hit = Scan(&range, 1, pattern);
        check(hit.status == ScanStatus::Unique, "unique pattern resolved");
        check(hit.address == 0x1000, "unique pattern address");
    }

    // 2. The same body twice is ambiguous and publishes nothing.
    {
        auto twice = kBody;
        twice.insert(twice.end(), {0xd65f03c0U, 0xd65f03c0U, 0xd65f03c0U, 0xd65f03c0U});
        twice.insert(twice.end(), kBody.begin(), kBody.end());
        auto text = bytes(twice);
        CodeRange range{text.data(), text.size(), 0x2000};
        const auto hit = Scan(&range, 1, signature(kBody));
        check(hit.status == ScanStatus::Ambiguous, "duplicate body is ambiguous");
        check(hit.address == 0, "ambiguous match publishes nothing");
        check(hit.matches == 2, "ambiguous match count capped at two");
    }

    // 3. A missing pattern never resolves.
    {
        std::vector<uint32_t> other{0xd65f03c0U, 0xd65f03c0U, 0xd65f03c0U, 0xd65f03c0U};
        auto text = bytes(other);
        CodeRange range{text.data(), text.size(), 0x3000};
        const auto hit = Scan(&range, 1, signature(kBody));
        check(hit.status == ScanStatus::NotFound, "missing pattern not found");
    }

    // 4. A BL whose destination moved is still part of the same signature.
    {
        auto site = kBody;
        site.push_back(branchLink(0x400));
        auto moved = site;
        moved[4] = branchLink(0x800);
        auto first = bytes(site);
        auto second = bytes(moved);
        CodeRange ranges[2]{{first.data(), first.size(), 0x4000}, {second.data(), second.size(), 0x5000}};
        const auto hit = Scan(ranges, 2, signature(site));
        check(hit.status == ScanStatus::Ambiguous, "relocated call site is ambiguous");
        // Scanning only the relocated copy still matches: the immediate is masked.
        const auto only = Scan(&ranges[1], 1, signature(site));
        check(only.status == ScanStatus::Unique, "masked branch matches after relocation");
        check(only.address == 0x5000, "masked branch address");
    }

    // 5. A call site selects the callee that has the reviewed body.
    {
        auto call = kBody;
        call.push_back(branchLink(8));  // BL at +0x10 calls the body at +0x18
        call.push_back(0xd65f03c0U);
        std::vector<uint32_t> text = call;
        text.insert(text.end(), kBody.begin(), kBody.end());
        auto memory = bytes(text);
        CodeRange range{memory.data(), memory.size(), 0x6000};
        const auto hit = ScanWithCallIdentity(&range, 1, signature(call), 4, signature(kBody));
        check(hit.status == ScanStatus::Unique, "call identity resolved");
        check(hit.address == 0x6000, "call identity selects the owner, not the callee");
        const auto wrong = ScanWithCallAddress(&range, 1, signature(call), 4, 0x7000);
        check(wrong.status == ScanStatus::NotFound, "foreign call target rejected");
    }

    // 6. ADRP + ADD and ADRP + STRB both decode to the same global.
    {
        const uintptr_t pc = 0x10000;
        const uintptr_t target = 0x24040;
        const uint32_t page = adrp(9, int64_t(target) - int64_t(pc & ~uintptr_t{0xfff}));
        uintptr_t decoded = 0;
        check(DecodeAdrpReference(pc, page, addImmediate(9, 9, 0x40), &decoded), "adrp+add decodes");
        check(decoded == target, "adrp+add target");
        // STRB Wd, [Xn, #imm]
        const uint32_t store = 0x39000000U | (0x40U << 10U) | (9U << 5U) | 0U;
        check(DecodeAdrpReference(pc, page, store, &decoded), "adrp+strb decodes");
        check(decoded == target, "adrp+strb target");
        // A consumer that uses another base register must be refused.
        check(!DecodeAdrpReference(pc, page, addImmediate(9, 8, 0), &decoded),
              "mismatched consumer register refused");
    }

    // 7. entry_delta publishes the function entry from an interior anchor.
    {
        std::vector<uint32_t> text(4U, 0xd503201fU);
        text.insert(text.end(), kBody.begin(), kBody.end());
        text.insert(text.end(), 4U, 0xd65f03c0U);
        auto memory = bytes(text);
        CodeRange range{memory.data(), memory.size(), 0x9000};
        auto pattern = signature(kBody);
        pattern.entry_delta = 0;
        const auto entry = Scan(&range, 1, pattern);
        check(entry.address == 0x9010, "entry anchor");
        auto interior = signature(kBody);
        interior.entry_delta = -4;
        const auto swapped = Scan(&range, 1, interior);
        check(swapped.address == 0x900c, "entry_delta rewinds the anchor");
    }

    // 8. A range whose address is not word aligned is refused outright.
    {
        auto text = bytes(kBody);
        CodeRange range{text.data(), text.size(), 0x1001};
        const auto hit = Scan(&range, 1, signature(kBody));
        check(hit.status == ScanStatus::InvalidInput, "unaligned range refused");
    }

    // 9. The accelerated index returns exactly what the linear scan returns.
    {
        std::vector<uint32_t> text = kBody;
        text.insert(text.end(), 4U, 0xd65f03c0U);
        text.insert(text.end(), kBody.begin(), kBody.end());
        auto memory = bytes(text);
        CodeRange range{memory.data(), memory.size(), 0xa000};
        std::vector<Signature> patterns{signature(kBody)};
        ScanIndex index;
        check(index.Build(&range, 1, patterns.data(), patterns.size()), "index built");
        const auto linear = Scan(&range, 1, patterns[0], nullptr);
        const auto accelerated = Scan(&range, 1, patterns[0], &index);
        check(linear.status == accelerated.status, "index status matches linear scan");
        check(linear.address == accelerated.address, "index address matches linear scan");
        check(accelerated.status == ScanStatus::Ambiguous, "index sees both copies");
    }

    if (failures) {
        std::printf("FAIL %d scanner assertions\n", failures);
        return 1;
    }
    std::printf("PASS scanner: uniqueness, relocation masking, call identity, adrp decode, "
                "entry delta, range validation, index parity\n");
    return 0;
}
