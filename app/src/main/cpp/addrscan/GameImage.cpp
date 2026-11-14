#include "GameImage.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <link.h>
#include <unistd.h>

namespace buildtools {
namespace addrscan {
namespace {

bool isGameLibraryPath(const char* path) noexcept {
    if (!path) return false;
    const char* file_name = std::strrchr(path, '/');
    file_name = file_name ? file_name + 1 : path;
    return std::strcmp(file_name, "libminecraftpe.so") == 0 ||
           std::strcmp(file_name, "libminecraftpe.so (deleted)") == 0;
}

void readBuildId(uintptr_t note_address, size_t note_size, Image* image) noexcept {
    size_t cursor = 0;
    while (cursor + 12U <= note_size) {
        uint32_t header[3];
        if (!ReadImageMemory(note_address + cursor, header, sizeof(header))) break;
        cursor += sizeof(header);
        const size_t name_size = (size_t(header[0]) + 3U) & ~size_t(3U);
        const size_t description_size = (size_t(header[1]) + 3U) & ~size_t(3U);
        if (name_size > note_size - cursor || description_size > note_size - cursor - name_size) {
            break;
        }
        char name[4]{};
        if (header[0] == 4U && header[2] == 3U && header[1] <= 64U &&
            ReadImageMemory(note_address + cursor, name, sizeof(name)) &&
            std::memcmp(name, "GNU", 4) == 0) {
            image->build_id.resize(header[1]);
            if (!ReadImageMemory(note_address + cursor + name_size, image->build_id.data(),
                                 image->build_id.size())) {
                image->build_id.clear();
            }
        }
        cursor += name_size + description_size;
    }
}

int gatherSegments(dl_phdr_info* info, size_t, void* opaque) {
    if (!info || !isGameLibraryPath(info->dlpi_name)) {
        return 0;
    }
    auto* image = static_cast<Image*>(opaque);
    image->base = info->dlpi_addr;
    for (size_t i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr)& header = info->dlpi_phdr[i];
        if (header.p_vaddr > UINTPTR_MAX - image->base ||
            header.p_memsz > UINTPTR_MAX - image->base - header.p_vaddr) {
            continue;
        }
        const uintptr_t start = image->base + static_cast<uintptr_t>(header.p_vaddr);
        if (header.p_type == PT_LOAD && (header.p_flags & PF_R)) {
            arm64::CodeRange range{reinterpret_cast<const uint8_t*>(start),
                                   static_cast<size_t>(header.p_memsz), start};
            if (header.p_flags & PF_X) {
                // Only file-backed code is scanned; the zero-filled tail of an
                // executable segment can never be a reviewed instruction.
                range.size = static_cast<size_t>(header.p_filesz);
                image->code.push_back(range);
            } else {
                image->data.push_back(range);
            }
            if (header.p_flags & PF_W) image->writable.push_back(range);
        }
        if (header.p_type == PT_NOTE) {
            readBuildId(start, static_cast<size_t>(header.p_filesz), image);
        }
    }
    return 1;
}

}  // namespace

bool ReadImageMemory(uintptr_t address, void* output, size_t size) noexcept {
    if (!address || !output || !size || size > 4096U || size > UINTPTR_MAX - address) {
        return false;
    }
    struct Channel {
        int fd[2]{-1, -1};
        ~Channel() { reset(); }
        void reset() {
            for (int& handle : fd) {
                if (handle >= 0) close(handle);
                handle = -1;
            }
        }
    };
    // The pipe is the only safe probe: write() validates the user buffer and
    // reports EFAULT for an unmapped page instead of faulting this process.
    static thread_local Channel channel;
    if (channel.fd[0] < 0 && pipe2(channel.fd, O_CLOEXEC | O_NONBLOCK) != 0) return false;
    ssize_t written = 0;
    do {
        written = write(channel.fd[1], reinterpret_cast<const void*>(address), size);
    } while (written < 0 && errno == EINTR);
    if (written != static_cast<ssize_t>(size)) {
        channel.reset();
        return false;
    }
    ssize_t received = 0;
    do {
        received = read(channel.fd[0], output, size);
    } while (received < 0 && errno == EINTR);
    if (received != static_cast<ssize_t>(size)) {
        channel.reset();
        return false;
    }
    return true;
}

bool CaptureGameImage(Image* image) noexcept {
    if (!image) return false;
    image->base = 0;
    image->code.clear();
    image->data.clear();
    image->writable.clear();
    image->build_id.clear();
    dl_iterate_phdr(gatherSegments, image);
    return image->base != 0 && !image->code.empty();
}

bool ContainsRange(const std::vector<arm64::CodeRange>& ranges, uintptr_t address,
                   size_t size) noexcept {
    for (const auto& range : ranges) {
        if (address >= range.address && address - range.address <= range.size &&
            size <= range.size - (address - range.address)) {
            return true;
        }
    }
    return false;
}

const uint8_t* RangeAt(const std::vector<arm64::CodeRange>& ranges, uintptr_t address,
                       size_t size) noexcept {
    for (const auto& range : ranges) {
        if (address >= range.address && address - range.address <= range.size &&
            size <= range.size - (address - range.address)) {
            return range.data + (address - range.address);
        }
    }
    return nullptr;
}

std::string FormatBuildId(const std::vector<uint8_t>& build_id) {
    static const char kHex[] = "0123456789abcdef";
    std::string text;
    text.reserve(build_id.size() * 2U);
    for (uint8_t byte : build_id) {
        text.push_back(kHex[byte >> 4U]);
        text.push_back(kHex[byte & 0x0fU]);
    }
    return text;
}

}  // namespace addrscan
}  // namespace buildtools
