// SCAFFOLDING (R2): RwRenderStateSet / Get, the alpha-test entries and the texture LOD entries of the GameCube driver, natively - deleted by R5: the render tree's lift sets draw state on the renderer directly
#include <cstring>

#include "gekko/cpu.h"
#include "rw_driver.h"

// Every case transcribes the driver's own code (RwRenderStateSet 0x80498954 with its table 0x8056FAD4,
// RwRenderStateGet 0x804984C4 with 0x8056FA58, alpha-test mode 0x804987C8, full compare 0x80498F74, LOD 0x8049B568 /
// 0x8049B734; disassembly in scratch/lift-20261006/r2/rs_*.dis.txt, semantics in r2/SITES_DEVICE.md 1.2-1.3, 6):
// the stored values, the return values and the writes RenderWare sees. What the driver sent to GX is not sent
// anywhere: the draw records take their recipe from these values (rw_driver.h `recipe`).

namespace gekko::scaffold {

namespace {

enum State : u32 {
    kTextureRaster = 1,
    kTextureAddress = 2,
    kTextureAddressU = 3,
    kTextureAddressV = 4,
    kTexturePerspective = 5,
    kZTestEnable = 6,
    kShadeMode = 7,
    kZWriteEnable = 8,
    kTextureFilter = 9,
    kSrcBlend = 10,
    kDestBlend = 11,
    kFogEnable = 14,
    kFogColour = 15,
    kFogType = 16,
    kCullMode = 20,
    kAlphaTestFunction = 29,
    kAlphaTestFunctionRef = 30,
};

constexpr u32 kAddressBorder = 4;

}  // namespace

u32 RwDriver::render_state_set(Cpu& c, u32 state, u32 value) {
    ++stats_.state_sets;
    switch (state) {
    case kTextureRaster: {
        // TextureSetRaster(imTex, value) 0x8049819C when it changes: the raster, and the GX object marked stale.
        const u32 tex = im_texture(c);
        if (value != c.mem_read32(tex + layout::kTextureRaster)) {
            c.mem_write32(tex + layout::kTextureRaster, value);
            c.mem_write32(texture_ext(c, tex) + layout::kTextureExtState, layout::kTextureStateStale);
        }
        return 1u;
    }
    case kTextureAddress: {
        if (value == kAddressBorder)
            return 0u;
        const u32 at = im_texture(c) + layout::kTextureFilterAddressing;
        const u32 word = c.mem_read32(at);
        c.mem_write32(at, (word & 0xFFFF00FFu) | ((value << 12) & 0xF000u) | ((value << 8) & 0x0F00u));
        return 1u;
    }
    case kTextureAddressU: {
        if (value == kAddressBorder)
            return 0u;
        const u32 at = im_texture(c) + layout::kTextureFilterAddressing;
        c.mem_write32(at, (c.mem_read32(at) & ~0x0F00u) | ((value << 8) & 0x0F00u));
        return 1u;
    }
    case kTextureAddressV: {
        if (value == kAddressBorder)
            return 0u;
        const u32 at = im_texture(c) + layout::kTextureFilterAddressing;
        c.mem_write32(at, (c.mem_read32(at) & ~0xF000u) | ((value << 12) & 0xF000u));
        return 1u;
    }
    case kTexturePerspective:
        return value;
    case kZTestEnable:
        states_.z_test = value != 0u ? 1u : 0u;
        return 1u;
    case kShadeMode:
        return value == 2u ? 1u : 0u;
    case kZWriteEnable:
        states_.z_write = value != 0u ? 1u : 0u;
        return 1u;
    case kTextureFilter: {
        const u32 at = im_texture(c) + layout::kTextureFilterAddressing;
        c.mem_write32(at, (c.mem_read32(at) & 0xFFFFFF00u) | (value & 0xFFu));
        return 1u;
    }
    case kSrcBlend: {
        if (value == states_.src_blend)
            return 1u;
        // ZERO, ONE, SRCALPHA .. INVDESTCOLOR; SRCCOLOR, INVSRCCOLOR, SRCALPHASAT and 0 are refused, not stored.
        const auto signed_value = static_cast<s32>(value);
        if (signed_value < 1 || signed_value == 3 || signed_value == 4 || signed_value >= 11)
            return 0u;
        states_.src_blend = value;
        return 1u;
    }
    case kDestBlend: {
        if (value == states_.dst_blend)
            return 1u;
        const auto signed_value = static_cast<s32>(value);
        if (signed_value < 1 || signed_value >= 9)  // DESTCOLOR, INVDESTCOLOR refused
            return 0u;
        states_.dst_blend = value;
        return 1u;
    }
    case kFogEnable: {
        if (value != 0u) {
            if (states_.fog_enable == 0u) {
                // The planes of RenderWare's current camera, at this moment (RwGlobals->curCamera).
                const u32 camera = c.mem_read32(c.mem_read32(layout::kRwGlobals) + layout::kGlobalsCurrentCamera);
                if (camera != 0u) {
                    states_.fog_end = read_f32(c, camera + layout::kCameraFar);  // fogEndPinned is never set
                    states_.fog_start = read_f32(c, camera + layout::kCameraFogPlane);
                    states_.fog_near = read_f32(c, camera + layout::kCameraNear);
                    states_.fog_far = read_f32(c, camera + layout::kCameraFar);
                }
                states_.gx_fog = true;
                states_.gx_fog_start = states_.fog_start;
                states_.gx_fog_end = states_.fog_end;
                states_.fog_enable = 1u;
            }
        } else if (states_.fog_enable != 0u) {
            states_.gx_fog = false;
            states_.fog_enable = 0u;
        }
        return 1u;
    }
    case kFogColour:
        if (value != states_.fog_colour) {
            // Re-arms GX fog with the current camera's planes even while RenderWare fog is off (the driver
            // dereferences the camera without a test).
            rearm_fog(c);
            states_.fog_colour = value;
        }
        return 1u;
    case kFogType:
        if (static_cast<s32>(value) == static_cast<s32>(states_.fog_type))
            return 1u;
        if (value != 1u)
            return 0u;
        rearm_fog(c);
        states_.fog_type = value;
        return 1u;
    case kCullMode:
        states_.cull = value;
        return 1u;
    case kAlphaTestFunction:
        if (states_.full_compare) {
            states_.cmp0 = value - 1u;
            states_.alpha_op = 0u;
            states_.cmp1 = 7u;
            states_.ref1 = 0u;
        } else if (value - 1u != states_.cmp0) {
            states_.cmp0 = value - 1u;
        }
        return 1u;
    case kAlphaTestFunctionRef:
        if (value != states_.ref0)
            states_.ref0 = static_cast<u8>(value);
        return 1u;
    default:
        // 0, VERTEXALPHAENABLE (12), BORDERCOLOR (13), FOGDENSITY (17), 18, 19, the stencil states, > 30.
        return 0u;
    }
}

u32 RwDriver::render_state_get(Cpu& c, u32 state, u32 out) {
    ++stats_.state_gets;
    const auto put = [&](u32 value) {
        c.mem_write32(out, value);
        return 1u;
    };
    switch (state) {
    case kTextureRaster:
        return put(c.mem_read32(im_texture(c) + layout::kTextureRaster));
    case kTextureAddress: {
        const u32 word = c.mem_read32(im_texture(c) + layout::kTextureFilterAddressing);
        const u32 u = (word >> 8) & 0xFu;
        const u32 v = (word >> 12) & 0xFu;
        return u == v ? put(u) : 0u;  // nothing written when U and V differ
    }
    case kTextureAddressU:
        return put((c.mem_read32(im_texture(c) + layout::kTextureFilterAddressing) >> 8) & 0xFu);
    case kTextureAddressV:
        return put((c.mem_read32(im_texture(c) + layout::kTextureFilterAddressing) >> 12) & 0xFu);
    case kTexturePerspective:
        return put(1u);
    case kZTestEnable:
        return put(states_.z_test);
    case kShadeMode:
        return put(2u);
    case kZWriteEnable:
        return put(states_.z_write);
    case kTextureFilter:
        return put(c.mem_read32(im_texture(c) + layout::kTextureFilterAddressing) & 0xFFu);
    case kSrcBlend:
        return put(states_.src_blend);
    case kDestBlend:
        return put(states_.dst_blend);
    case kFogEnable:
        return put(states_.fog_enable);
    case kFogColour:
        return put(states_.fog_colour);
    case kFogType:
        return put(states_.fog_type);
    case kCullMode:
        return put(states_.cull);
    case kAlphaTestFunction:
        return put(states_.alpha_op == 0u && states_.cmp1 == 7u ? states_.cmp0 + 1u : 0u);
    case kAlphaTestFunctionRef:
        return put(states_.ref0);
    default:
        // Nothing written: 0, VERTEXALPHAENABLE (12; its 18 readers keep their stack slot), 13, 17-19, 21-28, > 30.
        return 0u;
    }
}

// FINALIZESTART's reset (0x804981E8) has just stored the driver's defaults in its cache: the same defaults here.
void RwDriver::render_state_reset() {
    states_ = RenderStates{};
}

// FOGCOLOR / FOGTYPE program GX fog with the current camera's fog and far planes at this moment, whether RenderWare
// fog is on or not (the cache's FOGENABLE planes are left as they were).
void RwDriver::rearm_fog(Cpu& c) {
    const u32 camera = c.mem_read32(c.mem_read32(layout::kRwGlobals) + layout::kGlobalsCurrentCamera);
    if (camera != 0u) {
        states_.gx_fog_start = read_f32(c, camera + layout::kCameraFogPlane);
        states_.gx_fog_end = read_f32(c, camera + layout::kCameraFar);
    }
    states_.gx_fog = true;
    // Inside a CameraRender level (the light manager's world current): the stage's fog task in level 2 re-arms fog
    // for the level's later draws - the rings - on the frames its colour changes (layer_recipe, kOpaqueLevel).
    const u32 world = c.mem_read32(c.mem_read32(layout::kRwGlobals) + layout::kGlobalsCurrentWorld);
    const u32 manager = c.mem_read32(c.gpr[13] + 0x2C30u);
    if (world != 0u && manager != 0u && world == c.mem_read32(manager + 0x45Cu)) {
        level_fog_frame_ = stats_.frames;
        level_fog_start_ = states_.gx_fog_start;
        level_fog_end_ = states_.gx_fog_end;
    }
}

// 0x804987C8: the compare is only re-programmed on a change of mode; the mode is stored as given.
void RwDriver::alpha_test_mode(u32 off) {
    states_.alpha_test_off = off;
}

// 0x80498F74: the new compare is cached (and becomes GX's at the next mode change: the one-call lag of
// r2/SITES_DEVICE.md 6.2, listed as an intended difference - draws take the cached compare as the level's intent).
void RwDriver::alpha_compare(u32 cmp0, u32 ref0, u32 op, u32 cmp1, u32 ref1) {
    states_.cmp0 = cmp0;
    states_.ref0 = static_cast<u8>(ref0);
    states_.alpha_op = op;
    states_.cmp1 = cmp1;
    states_.ref1 = static_cast<u8>(ref1);
    states_.full_compare = true;
}

// 0x8049B568: the LOD values become the texture's (natively: its sampler), and the texture's state word records the
// filter / address word it was set with, the stale bit cleared.
void RwDriver::texture_set_lod(Cpu& c, u32 texture, u32 bias_clamp, u32 edge_lod, u32 max_aniso, double lod_bias) {
    const u32 ext = texture_ext(c, texture);
    write_f32(c, ext + layout::kTextureExtLodBias, static_cast<float>(lod_bias));
    c.mem_write32(ext + layout::kTextureExtBiasClamp, bias_clamp & 0xFFu);
    c.mem_write32(ext + layout::kTextureExtEdgeLod, edge_lod & 0xFFu);
    c.mem_write32(ext + layout::kTextureExtMaxAniso, max_aniso);
    const u32 state = c.mem_read32(ext + layout::kTextureExtState);
    const u32 filter_addressing = c.mem_read32(texture + layout::kTextureFilterAddressing);
    c.mem_write32(ext + layout::kTextureExtState,
                  ((state & 0xFFFF0000u) | (filter_addressing & 0x0000FFFFu)) & ~layout::kTextureStateStale);
}

// 0x8049B734: the values set, or (0.0, 1, 1, 0) while the texture's object is stale.
void RwDriver::texture_get_lod(Cpu& c, u32 texture, u32 bias, u32 bias_clamp, u32 edge_lod, u32 max_aniso) {
    const u32 ext = texture_ext(c, texture);
    if ((c.mem_read32(ext + layout::kTextureExtState) & layout::kTextureStateStale) != 0u) {
        write_f32(c, bias, 0.0f);
        c.mem_write32(bias_clamp, 1u);
        c.mem_write32(edge_lod, 1u);
        c.mem_write32(max_aniso, 0u);
        return;
    }
    c.mem_write32(bias, c.mem_read32(ext + layout::kTextureExtLodBias));
    c.mem_write32(bias_clamp, c.mem_read32(ext + layout::kTextureExtBiasClamp));
    c.mem_write32(edge_lod, c.mem_read32(ext + layout::kTextureExtEdgeLod));
    c.mem_write32(max_aniso, c.mem_read32(ext + layout::kTextureExtMaxAniso));
}

// ---- what a draw takes from the states ----------------------------------------------------------------------------

shadow::render::Recipe RwDriver::recipe() const {
    shadow::render::Recipe r;
    // RenderWare's blend enumeration (1 ZERO .. 10 INVDESTCOLOR) is the renderer's, shifted by one; the driver's
    // GX mapping kept those meanings (destination colour in the source slot, source colour in the destination one).
    const auto blend = [](u32 rw, shadow::render::Blend fallback) {
        return rw >= 1u && rw <= 10u ? static_cast<shadow::render::Blend>(rw - 1u) : fallback;
    };
    r.src = blend(states_.src_blend, shadow::render::Blend::SrcAlpha);
    r.dst = blend(states_.dst_blend, shadow::render::Blend::InvSrcAlpha);
    // "Test off" is GX compare ALWAYS with writes still governed by ZWRITE.
    r.depth_test = states_.z_test != 0u;
    r.depth_write = states_.z_write != 0u;
    // RenderWare's NONE / BACK / FRONT; the renderer's winding convention was measured on the game's own worlds
    // (R2_DRIVER.md 2.5), so the game's BACK is the renderer's Back.
    r.cull = states_.cull == 3u   ? shadow::render::Cull::Front
             : states_.cull == 2u ? shadow::render::Cull::Back
                                  : shadow::render::Cull::None;
    r.fog = states_.gx_fog;
    r.fog_start = states_.gx_fog_start;
    r.fog_end = states_.gx_fog_end;
    const u32 fog = states_.fog_colour;  // 0xAARRGGBB
    r.fog_colour = shadow::render::Float4{static_cast<float>((fog >> 16) & 0xFFu) / 255.0f,
                                          static_cast<float>((fog >> 8) & 0xFFu) / 255.0f,
                                          static_cast<float>(fog & 0xFFu) / 255.0f,
                                          static_cast<float>(fog >> 24) / 255.0f};
    r.alpha_cutoff = 0.0f;
    return r;
}

// The cached compare as a cut-off (a texel at or below it is not drawn): the levels use GREATER 1 (opaque) and
// GREATER 200 (punch-through) or ALWAYS (r2/SITES_DEVICE.md 6.2).
float RwDriver::alpha_cutoff() const {
    if (states_.alpha_op != 0u || states_.cmp1 != 7u)
        return 0.0f;
    switch (states_.cmp0) {
    case 4u:  // GREATER ref
        return static_cast<float>(states_.ref0) / 255.0f;
    case 6u:  // GEQUAL ref
        return states_.ref0 == 0u ? 0.0f : (static_cast<float>(states_.ref0) - 1.0f) / 255.0f;
    default:
        return 0.0f;
    }
}

}  // namespace gekko::scaffold
