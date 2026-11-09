// SCAFFOLDING (R2): native textures for the rasters RenderWare's translated texture reader fills - deleted by R6: the texture dictionaries load through the importer once RenderWare's object factories are lifted
#include <string>
#include <utility>
#include <vector>

#include "assets/texel_decode.h"
#include "assets/texture_dictionary.h"
#include "assets/texture_import.h"
#include "gekko/cpu.h"
#include "gekko/gc_addr.h"
#include "gekko/texture_overrides.h"
#include "rw_driver.h"

namespace gekko::scaffold {

namespace assets = shadow::assets;
namespace render = shadow::render;

// The native texture read (0x8049A8DC) stays translated: it creates the RwRaster and RwTexture through RenderWare,
// allocates the texels and reads them from the stream into the raster. Its last step, the LOD call lowered here,
// comes after all of that, so the raster holds the stream's texels exactly: the importer decodes them (the same
// decoder that walks the disc) and the native texture's handle goes into the raster extension.
void RwDriver::native_texture_loaded(Cpu& c, u32 texture, u32 bias_clamp, u32 edge_lod, u32 max_aniso,
                                     double lod_bias) {
    texture_set_lod(c, texture, bias_clamp, edge_lod, max_aniso, lod_bias);
    const u32 raster = c.mem_read32(texture + layout::kTextureRaster);
    if (raster == 0u)
        return;
    render::TextureHandle handle;
    build_texture(c, raster, raster_ext(c, raster), &handle);
}

bool RwDriver::build_texture(Cpu& c, u32 raster, u32 ext, render::TextureHandle* out, bool mips) {
    assets::TextureRecord record;
    record.width = static_cast<std::uint16_t>(c.mem_read32(raster + layout::kRasterWidth));
    record.height = static_cast<std::uint16_t>(c.mem_read32(raster + layout::kRasterHeight));
    record.depth = static_cast<std::uint8_t>(c.mem_read32(raster + layout::kRasterDepth));
    record.encoding = static_cast<assets::TexelEncoding>(c.mem_read32(ext + layout::kRasterExtGxFormat) & 0xFFu);
    const u32 tlut = c.mem_read32(ext + layout::kRasterExtTlutFormat);
    record.palette_encoding = static_cast<assets::PaletteEncoding>(tlut <= 2u ? tlut : 0u);
    record.has_alpha = c.mem_read32(ext + layout::kRasterExtFlags) & 1u;
    const u32 pixels = c.mem_read32(ext + layout::kRasterExtPixels);
    const std::size_t level0 = assets::level_bytes(record.encoding, record.width, record.height);
    if (pixels == 0u || level0 == 0u) {
        ++stats_.texture_failures;
        return false;
    }
    const u8* texels = c.guest_memory().peek(GcAddr(pixels), level0);
    if (texels == nullptr) {
        ++stats_.texture_failures;
        return false;
    }
    record.texels = assets::ByteView{texels, level0};
    if (assets::is_indexed(record.encoding)) {
        const u32 palette = c.mem_read32(ext + layout::kRasterExtPalette);
        const u32 entries = 1u << (record.depth <= 8u ? record.depth : 8u);
        const u8* bytes = palette != 0u ? c.guest_memory().peek(GcAddr(palette), entries * 2u) : nullptr;
        if (bytes == nullptr) {
            ++stats_.texture_failures;
            return false;
        }
        record.palette.resize(entries);
        for (u32 i = 0; i < entries; ++i)
            record.palette[i] = static_cast<std::uint16_t>((bytes[i * 2] << 8) | bytes[i * 2 + 1]);
    }
    // The decode and the mip chain are the renderer's resource worker's job (ES 2026-10-19: 18-37 % of the measured
    // stage-load stalls were this import on the game thread). What the game hands over is copied here, while the
    // raster still holds it: the raster's memory is the game's again when this returns. The import can no longer fail
    // past the checks above (level_bytes is 0 for an empty size or an unknown encoding, and the decoder fails only on
    // a level shorter than level_bytes), so a texture refused here is refused exactly where it was before.
    render::Renderer::TextureSource source;
    // A texture registered to be drawn in place of this content (the sharp button sheets: full colour, larger than
    // the raster; sampling is normalised, so the game's quads map it the same way).
    std::shared_ptr<const texture_overrides::Override> replacement;
    if (assets::is_indexed(record.encoding) && !texture_overrides::Registry::instance().empty()) {
        std::vector<u8> palette_bytes(record.palette.size() * 2u);
        for (std::size_t i = 0; i < record.palette.size(); ++i) {
            palette_bytes[i * 2u] = static_cast<u8>(record.palette[i] >> 8);
            palette_bytes[i * 2u + 1u] = static_cast<u8>(record.palette[i] & 0xFFu);
        }
        replacement = texture_overrides::Registry::instance().find(
            texture_overrides::content_key(texels, level0, palette_bytes.data(), palette_bytes.size()));
    }
    if (replacement) {
        record.encoding = assets::TexelEncoding::RGBA8;
        record.width = static_cast<std::uint16_t>(replacement->width);
        record.height = static_cast<std::uint16_t>(replacement->height);
        record.depth = 32u;
        record.palette.clear();
        source.texels = replacement->gx_rgba8;
        ++stats_.textures_overridden;
    } else {
        source.texels.assign(texels, texels + level0);
    }
    source.record = std::move(record);
    source.options.generate_mips = mips;
    std::string error;
    const render::TextureHandle handle =
        renderer_.request_texture(render::Scope::Session, std::move(source), &error);
    if (!handle) {
        ++stats_.texture_failures;
        return false;
    }
    // A raster that already had one (a texture read into a reused raster) gives it back first.
    const u32 word = ext + layout::kRasterExtTexture;
    for (Owned& owned : textures_) {
        if (owned.word == word && owned.handle != 0u) {
            renderer_.release(render::TextureHandle{owned.handle}, stats_.frames);
            owned.handle = 0u;
            ++stats_.textures_released;
        }
    }
    c.mem_write32(word, handle.value);
    textures_.push_back(Owned{handle.value, word});
    texture_index_dirty_ = true;
    ++stats_.textures_built;
    *out = handle;
    return true;
}

// The native texture a material's RwTexture draws with: its raster's top-level parent holds the handle (the
// driver's bind read `texture->raster->parent`). A raster the scaffold has not seen yet (one the game filled itself:
// the white raster, a movie frame) is built from its texels on first use.
render::TextureHandle RwDriver::texture_of(Cpu& c, u32 rw_texture, bool* has_alpha) {
    const render::TextureHandle handle = texture_lookup(c, rw_texture, has_alpha);
    if (handle && material_probe_.enabled() && !material_probe_.has_name(handle.value)) {
        char name[33]{};
        for (u32 i = 0; i < 32u; ++i) {
            name[i] = static_cast<char>(c.mem_read8(rw_texture + 0x10u + i));
            if (name[i] == '\0')
                break;
        }
        material_probe_.set_name(handle.value, name);
    }
    return handle;
}

render::TextureHandle RwDriver::texture_lookup(Cpu& c, u32 rw_texture, bool* has_alpha) {
    *has_alpha = false;
    if (rw_texture == 0u)
        return {};
    const u32 raster = c.mem_read32(rw_texture + layout::kTextureRaster);
    if (raster == 0u)
        return {};
    u32 parent = c.mem_read32(raster + layout::kRasterParent);
    if (parent == 0u)
        parent = raster;
    const u32 ext = raster_ext(c, parent);
    *has_alpha = (c.mem_read32(ext + layout::kRasterExtFlags) & 1u) != 0u;
    const u32 value = c.mem_read32(ext + layout::kRasterExtTexture);
    const u32 word = ext + layout::kRasterExtTexture;
    if (value != 0u) {
        if (texture_index_dirty_) {
            // At most one live entry per word (build_texture gives the old one back first).
            texture_index_.clear();
            for (const Owned& owned : textures_) {
                if (owned.handle != 0u)
                    texture_index_[owned.word] = owned.handle;
            }
            texture_index_dirty_ = false;
        }
        const auto found = texture_index_.find(word);
        if (found != texture_index_.end() && found->second == value)
            return render::TextureHandle{value};
    }
    // Unknown here (never built, or a word restored by a savestate from another session): build it now.
    render::TextureHandle handle;
    if (build_texture(c, parent, ext, &handle))
        return handle;
    return {};
}

// The texture's own filter / address word (RwTexture+0x50). The game's 3D textures are filtered smoothly at native
// resolution with the renderer's anisotropic sampler (R_DESIGN.md 2.6: generated mips); nearest stays nearest.
render::Sampler RwDriver::sampler_of(u32 filter_addressing) const {
    render::Sampler s;
    const u32 filter = filter_addressing & 0xFFu;
    s.filter = filter == 1u || filter == 3u ? render::Filter::Point : render::Filter::Anisotropic;
    const auto address = [](u32 rw) {
        // RenderWare: 1 wrap, 2 mirror, 3 clamp (4 border is refused by the driver).
        return rw == 2u ? render::Address::Mirror : rw == 3u ? render::Address::Clamp : render::Address::Wrap;
    };
    s.u = address((filter_addressing >> 8) & 0xFu);
    s.v = address((filter_addressing >> 12) & 0xFu);
    return s;
}

}  // namespace gekko::scaffold
