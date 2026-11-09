#pragma once

// Byte views and endian-explicit loads for the asset importers. Asset files mix byte orders: archive tables and
// RenderWare chunk headers are little-endian, the payloads the game's platform wrote are big-endian.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace shadow::assets {

struct ByteView {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;

    bool contains(std::size_t offset, std::size_t length) const noexcept {
        return offset <= size && length <= size - offset;
    }
    ByteView slice(std::size_t offset, std::size_t length) const noexcept {
        return contains(offset, length) ? ByteView{data + offset, length} : ByteView{};
    }
};

inline std::uint16_t load_be16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}
inline std::uint32_t load_be32(const std::uint8_t* p) noexcept {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
}
inline std::uint16_t load_le16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}
inline std::uint32_t load_le32(const std::uint8_t* p) noexcept {
    return p[0] | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}
inline float load_le_float(const std::uint8_t* p) noexcept {
    const std::uint32_t bits = load_le32(p);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}
inline float load_be_float(const std::uint8_t* p) noexcept {
    const std::uint32_t bits = load_be32(p);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

}  // namespace shadow::assets
