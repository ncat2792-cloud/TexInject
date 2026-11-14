#ifndef INFINITE_BUILDTOOL_SIGNATURE_CATALOG_H
#define INFINITE_BUILDTOOL_SIGNATURE_CATALOG_H

#include <cstddef>
#include <cstdint>

#include "Arm64SignatureScanner.h"

namespace buildtools {
namespace addrscan {

// How a rule turns one reviewed instruction pattern into a published address.
enum class RuleKind : uint8_t {
    // The match is the target entry point itself.
    Direct,
    // The match is a call site; the target is the decoded BL callee.
    Branch,
    // The match references a global through ADRP + consumer.
    Data,
    // Data plus a second, independently matched reference to the same global.
    VerifiedReference,
};

struct Rule {
    uint64_t key = 0;
    RuleKind kind = RuleKind::Direct;
    arm64::Signature first{};
    arm64::Signature second{};
    arm64::Signature body{};
    size_t branch_word = 0;
    size_t first_adrp_word = 0;
    size_t first_consumer_word = 0;
    size_t second_adrp_word = 0;
    size_t second_consumer_word = 0;
    size_t target_size = 4;
    size_t target_alignment = 4;
    bool writable = false;
    const uint8_t* target_bytes = nullptr;
    size_t target_byte_count = 0;
};

// A rule key publishes exactly one FunctionsAddress field. The field keeps the
// image-relative value, so every existing base + offset call site stays valid.
struct Binding {
    uint64_t key = 0;
    uintptr_t* field = nullptr;
    // A required binding must resolve; an optional one only disables the
    // feature that reads it, so a build where one auxiliary global moved can
    // still run every hook whose own address was verified.
    bool required = true;
};

// A build is accepted only when its GNU build-id was reviewed together with
// the rule set. A matching build-id never grants an address on its own: every
// rule still has to match exactly once.
struct AllowedBuild {
    const char* label = "";
    const uint8_t* bytes = nullptr;
    size_t size = 0;
};

}  // namespace addrscan
}  // namespace buildtools

#endif  // INFINITE_BUILDTOOL_SIGNATURE_CATALOG_H
