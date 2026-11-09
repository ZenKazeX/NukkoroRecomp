#include "assets/material_import.h"

#include <algorithm>

namespace shadow::assets {

namespace {

bool fail(std::string* error, const std::string& what) {
    if (error != nullptr)
        *error = what;
    return false;
}

constexpr std::uint32_t kChunkUnicodeString = 0x13;
constexpr std::uint32_t kChunkEffects = 0x120;
constexpr std::uint32_t kChunkUvAnimation = 0x135;
constexpr std::uint32_t kChunkPlatformMaterial = 0x129;

std::string read_string(ByteView bytes, const Chunk& c) {
    if (c.type == kChunkUnicodeString) {
        std::string out;
        for (std::size_t p = c.body(); p + 1 < c.end(); p += 2) {
            const std::uint16_t unit = load_le16(bytes.data + p);
            if (unit == 0)
                break;
            out.push_back(unit < 0x80 ? static_cast<char>(unit) : '?');
        }
        return out;
    }
    const auto* begin = reinterpret_cast<const char*>(bytes.data + c.body());
    return std::string(begin, std::find(begin, begin + c.size, '\0'));
}

bool read_texture(ByteView bytes, const Chunk& texture, TextureReference* out, std::string* error) {
    std::vector<Chunk> parts;
    if (texture.type != chunk::kTexture || !children(bytes, texture, &parts))
        return fail(error, "expected a texture chunk");
    out->present = true;
    if (const Chunk* body = find(parts, chunk::kStruct); body != nullptr && body->size >= 4)
        out->filter_address = load_le32(bytes.data + body->body());
    int strings = 0;
    for (const Chunk& part : parts) {
        if (part.type != chunk::kString && part.type != kChunkUnicodeString)
            continue;
        (strings++ == 0 ? out->name : out->mask_name) = read_string(bytes, part);
    }
    return true;
}

// An effect slot's optional texture: a 32-bit flag, then the texture chunk when it is set.
bool read_slot_texture(ByteView bytes, std::size_t& p, std::size_t end, TextureReference* out, std::string* error) {
    if (end - p < 4)
        return fail(error, "effect slot texture flag past the chunk");
    const bool present = load_le32(bytes.data + p) != 0;
    p += 4;
    if (!present)
        return true;
    if (!valid_chunk_header(ByteView{bytes.data, end}, p))
        return fail(error, "effect slot texture header invalid");
    Chunk c;
    c.type = load_le32(bytes.data + p);
    c.size = load_le32(bytes.data + p + 4);
    c.stamp = load_le32(bytes.data + p + 8);
    c.offset = p;
    if (!read_texture(bytes, c, out, error))
        return false;
    p = c.end();
    return true;
}

bool read_effects(ByteView bytes, const Chunk& c, MaterialRecord* m, std::string* error) {
    std::size_t p = c.body();
    const std::size_t end = c.end();
    if (end - p < 4)
        return fail(error, "effect chunk too short");
    m->effect = static_cast<EffectKind>(load_le32(bytes.data + p));
    p += 4;
    for (int s = 0; s < 2 && end - p >= 4; ++s) {
        EffectSlot& slot = m->slots[s];
        slot.type = load_le32(bytes.data + p);
        p += 4;
        switch (slot.type) {
        case 1:  // bump: strength, bumped texture, bump texture
            if (end - p < 4)
                return fail(error, "bump slot past the chunk");
            slot.coefficient = load_le_float(bytes.data + p);
            p += 4;
            if (!read_slot_texture(bytes, p, end, &slot.texture, error) ||
                !read_slot_texture(bytes, p, end, &slot.texture2, error))
                return false;
            break;
        case 2:  // environment: strength, framebuffer-alpha flag, texture
            if (end - p < 8)
                return fail(error, "environment slot past the chunk");
            slot.coefficient = load_le_float(bytes.data + p);
            slot.framebuffer_alpha = load_le32(bytes.data + p + 4) != 0;
            p += 8;
            if (!read_slot_texture(bytes, p, end, &slot.texture, error))
                return false;
            break;
        case 4:  // dual: blend pair, texture
            if (end - p < 8)
                return fail(error, "dual slot past the chunk");
            slot.src_blend = load_le32(bytes.data + p);
            slot.dst_blend = load_le32(bytes.data + p + 4);
            p += 8;
            if (!read_slot_texture(bytes, p, end, &slot.texture, error))
                return false;
            break;
        case 0:
        case 5:
            break;
        default:
            return fail(error, "effect slot of unknown type " + std::to_string(slot.type));
        }
        ++m->slot_count;
    }
    if (p != end)
        return fail(error, "effect chunk does not end where its data does");
    return true;
}

bool read_material(ByteView bytes, const Chunk& c, MaterialRecord* m, std::string* error) {
    std::vector<Chunk> parts;
    if (c.type != chunk::kMaterial || !children(bytes, c, &parts))
        return fail(error, "expected a material chunk");
    const Chunk* body = find(parts, chunk::kStruct);
    if (body == nullptr || body->size < 16)
        return fail(error, "material without its struct");
    const std::uint8_t* s = bytes.data + body->body();
    m->flags = load_le32(s);
    std::copy(s + 4, s + 8, m->colour);
    m->textured = load_le32(s + 12) != 0;
    if (body->size >= 28) {
        m->ambient = load_le_float(s + 16);
        m->specular = load_le_float(s + 20);
        m->diffuse = load_le_float(s + 24);
    }
    if (const Chunk* texture = find(parts, chunk::kTexture)) {
        if (!read_texture(bytes, *texture, &m->texture, error))
            return false;
    }
    if (m->textured && !m->texture.present)
        return fail(error, "material marked textured without a texture");
    if (const Chunk* extension = find(parts, chunk::kExtension)) {
        std::vector<Chunk> plugins;
        if (!children(bytes, *extension, &plugins))
            return fail(error, "material extension with a broken child run");
        for (const Chunk& plugin : plugins) {
            if (plugin.type == kChunkEffects) {
                if (!read_effects(bytes, plugin, m, error))
                    return false;
            } else if (plugin.type == kChunkUvAnimation) {
                std::vector<Chunk> inner;
                const Chunk* st = nullptr;
                if (read_chunks(bytes, plugin.body(), plugin.end(), &inner))
                    st = find(inner, chunk::kStruct);
                if (st == nullptr || st->size < 4)
                    return fail(error, "UV animation plugin without its struct");
                const std::uint32_t mask = load_le32(bytes.data + st->body());
                std::size_t p = st->body() + 4;
                for (int bit = 0; bit < 32; ++bit) {
                    if ((mask & (1u << bit)) == 0)
                        continue;
                    if (st->end() - p < 32)
                        return fail(error, "UV animation names past the struct");
                    const auto* name = reinterpret_cast<const char*>(bytes.data + p);
                    m->uv_animations.emplace_back(name, std::find(name, name + 32, '\0'));
                    p += 32;
                }
            } else if (plugin.type == kChunkPlatformMaterial) {
                m->platform_material = true;
            }
        }
    }
    return true;
}

}  // namespace

bool import_material_list(ByteView bytes, const Chunk& list, std::vector<MaterialRecord>* out, std::string* error) {
    out->clear();
    std::vector<Chunk> parts;
    if (list.type != chunk::kMaterialList || !children(bytes, list, &parts))
        return fail(error, "not a material list");
    const Chunk* header = find(parts, chunk::kStruct);
    if (header == nullptr || header->size < 4)
        return fail(error, "material list without its struct");
    const auto count = static_cast<std::int32_t>(load_le32(bytes.data + header->body()));
    if (count < 0 || header->size < 4 + static_cast<std::size_t>(count) * 4)
        return fail(error, "material list count does not fit its struct");
    std::vector<const Chunk*> materials;
    for (const Chunk& part : parts) {
        if (part.type == chunk::kMaterial)
            materials.push_back(&part);
    }
    std::size_t next = 0;
    for (std::int32_t i = 0; i < count; ++i) {
        const auto instance = static_cast<std::int32_t>(load_le32(bytes.data + header->body() + 4 + i * 4));
        if (instance < 0) {
            if (next >= materials.size())
                return fail(error, "material list names more materials than it holds");
            MaterialRecord m;
            if (!read_material(bytes, *materials[next++], &m, error))
                return false;
            out->push_back(std::move(m));
        } else {
            if (instance >= i)
                return fail(error, "material list instance refers forward");
            out->push_back((*out)[static_cast<std::size_t>(instance)]);
        }
    }
    return true;
}

}  // namespace shadow::assets
