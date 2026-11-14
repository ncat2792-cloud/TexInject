#ifndef INFINITE_BUILDTOOL_GAME_IMAGE_H
#define INFINITE_BUILDTOOL_GAME_IMAGE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "Arm64SignatureScanner.h"

namespace buildtools {
namespace addrscan {

// The loaded Minecraft image as the linker mapped it. Ranges keep the image
// virtual address so a signature hit can be compared with a data reference
// decoded from an ADRP pair without any relocation arithmetic.
struct Image {
    uintptr_t base = 0;
    std::vector<arm64::CodeRange> code;
    std::vector<arm64::CodeRange> data;
    std::vector<arm64::CodeRange> writable;
    std::vector<uint8_t> build_id;
};

// Walks the loaded modules and fills every readable segment of
// libminecraftpe.so. Returns false while the game library is not mapped yet,
// which is a retryable state, not an error.
bool CaptureGameImage(Image* image) noexcept;

// Reads game memory through a pipe so an invalid address reports EFAULT
// instead of raising SIGSEGV in the host process. Never dereferences a
// pointer directly.
bool ReadImageMemory(uintptr_t address, void* output, size_t size) noexcept;

bool ContainsRange(const std::vector<arm64::CodeRange>& ranges, uintptr_t address,
                   size_t size) noexcept;

const uint8_t* RangeAt(const std::vector<arm64::CodeRange>& ranges, uintptr_t address,
                       size_t size) noexcept;

std::string FormatBuildId(const std::vector<uint8_t>& build_id);

}  // namespace addrscan
}  // namespace buildtools

#endif  // INFINITE_BUILDTOOL_GAME_IMAGE_H
