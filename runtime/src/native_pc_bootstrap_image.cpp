#include "native_pc_bootstrap_image.h"
#include "native_pc_boot_original_contract.h"
#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>

namespace shadow::pc_bootstrap {
namespace {
std::uint32_t be32(std::span<const std::uint8_t> bytes, std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 4u)
        throw std::runtime_error("bootstrap image: truncated word");
    return (std::uint32_t(bytes[offset]) << 24u) | (std::uint32_t(bytes[offset + 1]) << 16u) |
           (std::uint32_t(bytes[offset + 2]) << 8u) | bytes[offset + 3];
}
using original::kCrtWords;
constexpr std::array<std::array<std::uint32_t, 2>, 3> kZeroRanges{{
    {0x8056FE00u, 0x74700u}, {0x805EF020u, 0x375Cu}, {0x805FC540u, 0xACu}}};
}
std::span<std::uint8_t> InitializedImage::range(std::uint32_t address, std::size_t size) {
    if (address < link_base) throw std::runtime_error("bootstrap image: range below image");
    const auto offset = std::uint64_t(address) - link_base;
    if (offset > storage.size() || size > storage.size() - offset)
        throw std::runtime_error("bootstrap image: range outside image");
    return std::span(storage).subspan(static_cast<std::size_t>(offset), size);
}
std::uint32_t InitializedImage::word(std::uint32_t address) { return be32(range(address, 4), 0); }

InitializedImage load_initialized_image(std::span<const std::uint8_t> dol,
                                       std::uint64_t available_host_bytes) {
    if (dol.size() < 0x100u) throw std::runtime_error("bootstrap image: truncated DOL header");
    struct Section { std::uint32_t file, address, size; };
    std::array<Section, 18> sections{};
    std::uint64_t lo = std::numeric_limits<std::uint32_t>::max(), hi = 0;
    const auto include = [&](std::uint32_t address, std::uint32_t size) {
        const std::uint64_t end = std::uint64_t(address) + size;
        if (end > (1ull << 32u)) throw std::runtime_error("bootstrap image: linked extent wraps");
        lo = std::min(lo, std::uint64_t(address)); hi = std::max(hi, end);
    };
    for (std::size_t i = 0; i < sections.size(); ++i) {
        auto& section = sections[i];
        section = {be32(dol, 4u * i), be32(dol, 0x48u + 4u * i), be32(dol, 0x90u + 4u * i)};
        if (section.size == 0u) continue;
        if (section.file < 0x100u || std::uint64_t(section.file) + section.size > dol.size())
            throw std::runtime_error("bootstrap image: section outside DOL");
        include(section.address, section.size);
    }
    const auto bss_size = be32(dol, 0xDCu);
    if (bss_size != 0u) include(be32(dol, 0xD8u), bss_size);
    if (hi <= lo) throw std::runtime_error("bootstrap image: empty image");
    const std::uint64_t demand = hi - lo;
    if (demand > available_host_bytes || demand > std::numeric_limits<std::size_t>::max())
        throw std::runtime_error("bootstrap image: host budget insufficient");
    InitializedImage image{static_cast<std::uint32_t>(lo), be32(dol, 0xE0u), {}};
    // Clear backing BEFORE initialized sections: DOL BSS overlaps sdata2.
    image.storage.resize(static_cast<std::size_t>(demand), 0u);
    for (const auto& section : sections) {
        if (section.size == 0u) continue;
        const auto destination = image.range(section.address, section.size);
        std::copy_n(dol.begin() + section.file, section.size, destination.begin());
    }
    return image;
}
std::uint64_t apply_verified_crt_initializers(InitializedImage& image) {
    for (std::size_t i = 0; i < kCrtWords.size(); ++i)
        if (image.word(0x80003340u + static_cast<std::uint32_t>(4u * i)) != kCrtWords[i])
            throw std::runtime_error("bootstrap CRT: unknown original instruction body");
    for (std::uint32_t i = 0; i < 10u; ++i) {
        const auto row = 0x80005544u + i * 12u;
        const auto source = image.word(row), destination = image.word(row + 4u), size = image.word(row + 8u);
        if (size == 0u || source != destination)
            throw std::runtime_error("bootstrap CRT: unknown ROM relocation contract");
        (void)image.range(destination, size);
    }
    if (image.word(0x800055C4u) != 0u)
        throw std::runtime_error("bootstrap CRT: copy table has no proven terminator");
    std::array<std::span<std::uint8_t>, 3> zero_views{};
    for (std::size_t i = 0; i < kZeroRanges.size(); ++i) {
        const auto row = 0x800055C8u + static_cast<std::uint32_t>(8u * i);
        if (image.word(row) != kZeroRanges[i][0] || image.word(row + 4u) != kZeroRanges[i][1])
            throw std::runtime_error("bootstrap CRT: unknown zero initialization contract");
        zero_views[i] = image.range(kZeroRanges[i][0], kZeroRanges[i][1]);
    }
    if (image.word(0x800055E4u) != 0u)
        throw std::runtime_error("bootstrap CRT: zero table has no proven terminator");
    std::uint64_t cleared = 0;
    for (const auto view : zero_views) { std::fill(view.begin(), view.end(), 0u); cleared += view.size(); }
    return cleared;
}
}  // namespace shadow::pc_bootstrap
