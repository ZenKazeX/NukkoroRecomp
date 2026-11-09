#pragma once

// RenderWare binary streams: the container format of the game's models, worlds and texture dictionaries.
//
// A stream is a run of chunks; a chunk is a 12-byte little-endian header {type, size, library stamp} followed by
// `size` bytes. Container chunks hold a run of child chunks; the others hold a payload (the "struct" chunk of a
// container carries its fixed fields).

#include <cstdint>
#include <vector>

#include "assets/bytes.h"

namespace shadow::assets {

namespace chunk {
inline constexpr std::uint32_t kStruct = 0x01;
inline constexpr std::uint32_t kString = 0x02;
inline constexpr std::uint32_t kExtension = 0x03;
inline constexpr std::uint32_t kTexture = 0x06;
inline constexpr std::uint32_t kMaterial = 0x07;
inline constexpr std::uint32_t kMaterialList = 0x08;
inline constexpr std::uint32_t kAtomicSector = 0x09;
inline constexpr std::uint32_t kPlaneSector = 0x0A;
inline constexpr std::uint32_t kWorld = 0x0B;
inline constexpr std::uint32_t kFrameList = 0x0E;
inline constexpr std::uint32_t kGeometry = 0x0F;
inline constexpr std::uint32_t kClump = 0x10;
inline constexpr std::uint32_t kAtomic = 0x14;
inline constexpr std::uint32_t kTextureNative = 0x15;
inline constexpr std::uint32_t kTextureDictionary = 0x16;
inline constexpr std::uint32_t kGeometryList = 0x1A;
}  // namespace chunk

struct Chunk {
    std::uint32_t type = 0;
    std::uint32_t size = 0;
    std::uint32_t stamp = 0;
    std::size_t offset = 0;  // of the header, within the stream

    std::size_t body() const noexcept { return offset + 12; }
    std::size_t end() const noexcept { return offset + 12 + size; }
};

// The library version a chunk's stamp encodes (e.g. 0x37002 for 3.7.0.2).
std::uint32_t library_version(std::uint32_t stamp) noexcept;

// A header that fits the bytes and carries a stamp of RenderWare 3.x.
bool valid_chunk_header(ByteView bytes, std::size_t offset) noexcept;

// The run of chunks in [begin, end). False when a chunk overruns `end` or the run does not end exactly at
// `end` (`trailing_allowed` bytes of padding are accepted after the last chunk).
bool read_chunks(ByteView bytes, std::size_t begin, std::size_t end, std::vector<Chunk>* out,
                 std::size_t trailing_allowed = 0);
// The children of a container chunk.
bool children(ByteView bytes, const Chunk& parent, std::vector<Chunk>* out);
const Chunk* find(const std::vector<Chunk>& chunks, std::uint32_t type) noexcept;

// The top-level chunks of a stream, when the bytes are one (a valid first header of a known top-level type and
// a run that covers the bytes up to at most 64 bytes of padding).
bool read_stream(ByteView bytes, std::vector<Chunk>* top);

}  // namespace shadow::assets
