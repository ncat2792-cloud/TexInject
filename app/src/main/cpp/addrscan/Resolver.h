#ifndef INFINITE_BUILDTOOL_ADDRESS_RESOLVER_H
#define INFINITE_BUILDTOOL_ADDRESS_RESOLVER_H

#include <cstddef>
#include <cstdint>
#include <string>

#include "GameImage.h"

namespace buildtools {
namespace addrscan {

struct Binding;

// Resolves every published game address from reviewed signatures in the loaded
// libminecraftpe.so. This is the only writer of FunctionsAddress: no fixed RVA
// and no guessed fallback exists anywhere in the resolution path.
//
// Returns true once the whole rule set matched exactly once and every binding
// was published. On failure nothing is published, so a caller that installs a
// Dobby hook afterwards can never patch an unverified address.
bool Initialize(std::string* error, bool* retryable);

// Same transaction against an already captured image. The host test feeds it an
// ELF read from disk so the publication path is exercised without a device.
bool ResolveImage(const Image& image, std::string* error);

bool Ready() noexcept;

// Size of the reviewed catalog: zero rules means the build is registered but
// no address may be published yet.
size_t RuleCount() noexcept;
size_t BindingCount() noexcept;

// The reviewed binding table. Host tests walk it instead of naming fields, so
// a renamed or added binding cannot silently escape the checks.
const Binding* Bindings() noexcept;

uintptr_t Base() noexcept;

// Re-validates a published offset against the resolved code ranges before it
// is handed to a hook installer.
bool CodeOffset(uintptr_t base, uintptr_t offset, size_t size, uintptr_t* address) noexcept;

// Reads bytes from the captured image after checking that the whole range lies
// inside a resolved segment. The Python ABI verifier uses it to inspect call
// sites and the bytecode dispatch table before any of them is called.
bool ReadImage(uintptr_t address, void* output, size_t size) noexcept;

// True when the offset belongs to the resolved rule set. Used to audit the
// remaining fixed offsets that still live outside the catalog.
bool IsResolvedOffset(uintptr_t offset) noexcept;

size_t ResolvedCount() noexcept;

// Human-readable state for the in-game status surface and logcat.
std::string StatusText();

}  // namespace addrscan
}  // namespace buildtools

#endif  // INFINITE_BUILDTOOL_ADDRESS_RESOLVER_H
