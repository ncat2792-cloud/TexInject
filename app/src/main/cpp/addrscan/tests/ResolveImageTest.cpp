#include "../GameImage.h"
#include "../Resolver.h"
#include "../SignatureCatalog.h"
#include "../../tp/FunctionsAddress.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

// Feeds the resolver an ELF read from disk, so the publication path can be
// checked on a host before an APK ever runs on a device.
//
//   ResolveImageTest <libminecraftpe.so>
//
// With an empty catalog the test reports SKIP: there is nothing to publish yet,
// and the module is expected to refuse every hook in that state.
namespace {

using buildtools::addrscan::Image;

struct ProgramHeader {
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t vaddr;
    uint64_t paddr;
    uint64_t filesz;
    uint64_t memsz;
    uint64_t align;
};

template <typename T>
T readAt(const std::vector<uint8_t>& bytes, size_t offset) {
    T value;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

bool loadImage(const char* path, std::vector<std::vector<uint8_t>>* memory, Image* image) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return false;
    const size_t size = static_cast<size_t>(file.tellg());
    std::vector<uint8_t> bytes(size);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    if (bytes.size() < 64 || std::memcmp(bytes.data(), "\x7f" "ELF\x02\x01", 6) != 0) {
        return false;
    }
    image->base = 0x7100000000ULL;
    const uint64_t phoff = readAt<uint64_t>(bytes, 32);
    const uint16_t phnum = readAt<uint16_t>(bytes, 56);
    memory->reserve(phnum);
    for (uint16_t index = 0; index < phnum; ++index) {
        const ProgramHeader header =
            readAt<ProgramHeader>(bytes, static_cast<size_t>(phoff + index * sizeof(ProgramHeader)));
        if (header.type == 1 && (header.flags & 4)) {
            memory->emplace_back(static_cast<size_t>(header.memsz), 0);
            std::memcpy(memory->back().data(), bytes.data() + header.offset,
                        static_cast<size_t>(header.filesz));
            buildtools::arm64::CodeRange range{memory->back().data(),
                                               static_cast<size_t>(header.memsz),
                                               image->base + header.vaddr};
            if (header.flags & 1) {
                range.size = static_cast<size_t>(header.filesz);
                image->code.push_back(range);
            } else {
                image->data.push_back(range);
            }
            if (header.flags & 2) image->writable.push_back(range);
        }
        if (header.type == 4) {
            size_t at = static_cast<size_t>(header.offset);
            const size_t end = at + static_cast<size_t>(header.filesz);
            while (at + 12 <= end) {
                const uint32_t namesz = readAt<uint32_t>(bytes, at);
                const uint32_t descsz = readAt<uint32_t>(bytes, at + 4);
                const uint32_t type = readAt<uint32_t>(bytes, at + 8);
                at += 12;
                const size_t aligned_name = (namesz + 3U) & ~size_t(3U);
                const size_t aligned_desc = (descsz + 3U) & ~size_t(3U);
                if (namesz == 4 && type == 3 && std::memcmp(bytes.data() + at, "GNU", 4) == 0) {
                    image->build_id.assign(bytes.begin() + at + aligned_name,
                                           bytes.begin() + at + aligned_name + descsz);
                }
                at += aligned_name + aligned_desc;
            }
        }
    }
    return image->base != 0 && !image->code.empty();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("SKIP no ELF argument\n");
        return 0;
    }
    if (buildtools::addrscan::RuleCount() == 0) {
        std::printf("SKIP catalog is empty; the module refuses every hook by design\n");
        return 0;
    }
    std::vector<std::vector<uint8_t>> memory;
    Image image;
    if (!loadImage(argv[1], &memory, &image)) {
        std::printf("FAIL cannot load %s\n", argv[1]);
        return 1;
    }
    std::string error;
    if (!buildtools::addrscan::ResolveImage(image, &error)) {
        std::printf("FAIL %s\n", error.c_str());
        return 1;
    }
    const size_t published = buildtools::addrscan::ResolvedCount();
    if (!buildtools::addrscan::Ready() || published != buildtools::addrscan::BindingCount()) {
        std::printf("FAIL published=%zu expected=%zu\n", published,
                    buildtools::addrscan::BindingCount());
        return 1;
    }
    // Walk the catalog itself: every binding it publishes has to point inside
    // the image it was resolved from, and no binding may still be zero. Naming
    // fields here would let a renamed or newly added binding escape the check.
    const buildtools::addrscan::Binding* bindings = buildtools::addrscan::Bindings();
    size_t live = 0;
    for (size_t index = 0; index < buildtools::addrscan::BindingCount(); ++index) {
        const uintptr_t offset = *bindings[index].field;
        if (!offset) {
            if (bindings[index].required) {
                std::printf("FAIL required binding key=%llu stayed zero\n",
                            static_cast<unsigned long long>(bindings[index].key));
                return 1;
            }
            continue;
        }
        ++live;
        if (!buildtools::addrscan::IsResolvedOffset(offset)) {
            std::printf("FAIL an offset was written that the catalog never resolved\n");
            return 1;
        }
        const uintptr_t address = buildtools::addrscan::Base() + offset;
        bool inside = false;
        for (const auto& range : image.code) {
            if (address >= range.address && address + 4 <= range.address + range.size) inside = true;
        }
        for (const auto& range : image.data) {
            if (address >= range.address && address + 4 <= range.address + range.size) inside = true;
        }
        if (!inside) {
            std::printf("FAIL binding key=%llu points outside the image\n",
                        static_cast<unsigned long long>(bindings[index].key));
            return 1;
        }
    }
    if (!live) {
        std::printf("FAIL the catalog resolved bindings but no field was written\n");
        return 1;
    }
    // A second transaction on the same build must be a no-op success.
    if (!buildtools::addrscan::ResolveImage(image, &error)) {
        std::printf("FAIL second resolve: %s\n", error.c_str());
        return 1;
    }
    // A build-id that is off by one bit must be refused, with nothing published
    // for that image.
    Image foreign = image;
    foreign.build_id[0] ^= 1;
    if (buildtools::addrscan::ResolveImage(foreign, &error)) {
        std::printf("FAIL an unregistered build was accepted\n");
        return 1;
    }
    std::printf("PASS image resolution: %zu bindings published, foreign build refused\n", published);
    return 0;
}
