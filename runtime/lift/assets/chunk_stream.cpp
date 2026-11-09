#include "assets/chunk_stream.h"

namespace shadow::assets {

std::uint32_t library_version(std::uint32_t stamp) noexcept {
    if ((stamp & 0xFFFF0000u) != 0)
        return (((stamp >> 14) & 0x3FF00u) + 0x30000u) | ((stamp >> 16) & 0x3Fu);
    return stamp << 8;
}

namespace {

bool known_top_level(std::uint32_t type) noexcept {
    switch (type) {
    case chunk::kClump: case chunk::kWorld: case chunk::kTextureDictionary: case chunk::kGeometry:
    case 0x2B: case 0x1B: case 0x29: case 0x2A: case chunk::kAtomic: case chunk::kMaterial:
    case chunk::kMaterialList: case chunk::kTexture: case chunk::kTextureNative: case 0x12: case 0x05:
    case chunk::kFrameList: case chunk::kGeometryList: case 0x1E:
        return true;
    default:
        return false;
    }
}

}  // namespace

bool valid_chunk_header(ByteView bytes, std::size_t offset) noexcept {
    if (!bytes.contains(offset, 12))
        return false;
    const std::uint32_t size = load_le32(bytes.data + offset + 4);
    const std::uint32_t stamp = load_le32(bytes.data + offset + 8);
    if (size > bytes.size - offset - 12)
        return false;
    const std::uint32_t version = library_version(stamp);
    return stamp == 0x1C020037u || stamp == 0x1C020020u || (version >= 0x30000u && version <= 0x3FFFFu);
}

bool read_chunks(ByteView bytes, std::size_t begin, std::size_t end, std::vector<Chunk>* out,
                 std::size_t trailing_allowed) {
    out->clear();
    if (end > bytes.size || begin > end)
        return false;
    std::size_t at = begin;
    while (at + 12 <= end) {
        Chunk c;
        c.type = load_le32(bytes.data + at);
        c.size = load_le32(bytes.data + at + 4);
        c.stamp = load_le32(bytes.data + at + 8);
        c.offset = at;
        if (c.size > end - at - 12)
            return false;
        out->push_back(c);
        at = c.end();
    }
    return end - at <= trailing_allowed;
}

bool children(ByteView bytes, const Chunk& parent, std::vector<Chunk>* out) {
    return read_chunks(bytes, parent.body(), parent.end(), out);
}

const Chunk* find(const std::vector<Chunk>& chunks, std::uint32_t type) noexcept {
    for (const Chunk& c : chunks) {
        if (c.type == type)
            return &c;
    }
    return nullptr;
}

bool read_stream(ByteView bytes, std::vector<Chunk>* top) {
    top->clear();
    if (!valid_chunk_header(bytes, 0) || !known_top_level(load_le32(bytes.data)))
        return false;
    return read_chunks(bytes, 0, bytes.size, top, 64) && !top->empty();
}

}  // namespace shadow::assets
