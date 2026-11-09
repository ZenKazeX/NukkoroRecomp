// SCAFFOLDING (R2): RxPipelineExecute at the atomic and world-sector sites as native draws of RenderWare objects - deleted by R6: the scene graph's lift draws its own meshes
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "assets/geometry_import.h"
#include "assets/light_slots.h"
#include "gekko/cpu.h"
#include "gekko/gc_addr.h"
#include "gekko/uv_anim_time.h"
#include "rw_driver.h"

// r2/SITES_PIPELINES.md: the node bodies (0x80464AAC atomic, 0x80464FF4 sector), their instance / lighting / render
// callbacks (2.2-2.3) and the RenderWare-visible writes they leave behind (3.7). Every pipeline has exactly one node,
// so a lowered site replaces the whole execution and returns the pipeline (success), as RxPipelineExecute does.

namespace gekko::scaffold {

namespace assets = shadow::assets;
namespace render = shadow::render;

namespace {

constexpr u32 kGeometryTriStrip = 0x01;
constexpr u32 kGeometryPositions = 0x02;
constexpr u32 kGeometryTextured = 0x04;
constexpr u32 kGeometryPrelit = 0x08;
constexpr u32 kGeometryNormals = 0x10;
constexpr u32 kGeometryLight = 0x20;
constexpr u32 kGeometryModulate = 0x40;
constexpr u32 kGeometryTextured2 = 0x80;
constexpr u32 kGeometryNative = 0x01000000;
constexpr u32 kMeshHeaderTriStrip = 0x01;

// MatFX effect kinds (rpMatFXMaterialData flags) and the slot fields the passes read (r2/SITES_PIPELINES.md 2.4).
constexpr u32 kEffectEnvironment = 2;
constexpr u32 kEffectBumpEnvironment = 3;
constexpr u32 kEffectDual = 4;
constexpr u32 kEffectUvTransform = 5;
constexpr u32 kEffectDualUvTransform = 6;

float be_float(const u8* p) {
    const u32 bits = (static_cast<u32>(p[0]) << 24) | (static_cast<u32>(p[1]) << 16) |
                     (static_cast<u32>(p[2]) << 8) | p[3];
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// Big-endian floats of a guest array into `out` (count floats); false when it is not mapped.
bool guest_floats(Cpu& c, u32 address, std::size_t count, std::vector<float>& out) {
    const u8* p = address != 0u ? c.guest_memory().peek(GcAddr(address), count * 4u) : nullptr;
    if (p == nullptr)
        return false;
    out.resize(count);
    for (std::size_t i = 0; i < count; ++i)
        out[i] = be_float(p + i * 4u);
    return true;
}

// a after b (column vectors): p' = a * (b * p). RenderWare's row-vector `B x A` is this `compose(A, B)`.
render::Float3x4 compose(const render::Float3x4& a, const render::Float3x4& b) {
    render::Float3x4 r;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 4; ++j) {
            float sum = j == 3 ? a.m[i][3] : 0.0f;
            for (int k = 0; k < 3; ++k)
                sum += a.m[i][k] * b.m[k][j];
            r.m[i][j] = sum;
        }
    }
    return r;
}

}  // namespace

// The per-mesh texture binding of the render callbacks (0x80463964): a textured object's mesh with no texture binds
// the white raster through the Im texture (TEXTURERASTER becomes the white raster, RenderWare can read it back), and
// every mesh sets the alpha-test mode from its texture's alpha.
static void bind_side_effects(Cpu& c, u32 im_texture, u32 texture_ext_offset, u32 white_raster, u32 rw_texture,
                              bool has_alpha, u32* alpha_test_off) {
    if (rw_texture == 0u) {
        if (c.mem_read32(im_texture) != white_raster) {
            c.mem_write32(im_texture, white_raster);
            c.mem_write32(im_texture + texture_ext_offset + layout::kTextureExtState, layout::kTextureStateStale);
        }
        *alpha_test_off = 1u;
        return;
    }
    *alpha_test_off = has_alpha ? 0u : 1u;
}

// A RenderWare matrix (right, up, at, pos rows of 16 bytes) as the renderer's column-vector 3x4.
render::Float3x4 RwDriver::frame_matrix(Cpu& c, u32 matrix) {
    render::Float3x4 m;
    for (int column = 0; column < 4; ++column)
        for (int row = 0; row < 3; ++row)
            m.m[row][column] = read_f32(c, matrix + 16u * static_cast<u32>(column) + 4u * static_cast<u32>(row));
    return m;
}

// The lighting callback's inputs (0x80464478 -> 0x8046521C): the current world's directional list - ambient lights
// summed, directional lights travelling along their frame's `at` - for lights that light atomics. A root light
// frame's matrix is its modelling matrix (read while the frame may still be dirty; RenderWare writes the same LTM at
// its next synchronisation).
// A display layer's lights (docs/DRAW_DISTANCE.md "Layers"). 0x80046D80 attaches the light manager's lights
// ([r13+0x2C30] +4k: wrapper, its RpLight at +8) to a layer's world when the layer's flags (0x8051E328 + 0x18 i +8)
// have bit 0, and detaches them otherwise. Before each part's draw its render callback 0x801645D0 applies the part's
// light group (0x80048308): the group's record at manager +0x20 + 0x34 g sets light 0's colour (+0x00) and light 1's
// (+0x10) through RpLightSetColor (0x80047808), and turns light 1's frame by RwFrameRotate(X, a, replace),
// RwFrameRotate(Y, b, postconcat) with a, b the record's ints at +0x2C and +0x30 in degrees (0x80048184): its at
// vector is (cos a sin b, -sin a, cos a cos b). This builds what light_set would read at that draw.
render::LightSetId RwDriver::layer_lights(Cpu& c, s32 layer, s32 light_group, bool* lit, bool* ambient,
                                          render::LightSet* out) {
    constexpr u32 kLayerTable = 0x8051E328u, kLayerStride = 0x18u, kLayerFlags = 0x08u;
    constexpr u32 kSdaLightManager = 0x2C30u, kManagerLights = 8u, kGroupRecords = 0x20u, kGroupStride = 0x34u;
    constexpr u32 kWrapperLight = 0x08u;
    *lit = *ambient = false;
    // A render level's draws (FarObjects::kOpaqueLevel) see the manager's world with every light attached
    // (CameraRender 0x801D04D4 -> 0x80047E24); a layer's only when the layer is lit.
    if (layer != FarObjects::kOpaqueLevel &&
        (c.mem_read32(kLayerTable + kLayerStride * static_cast<u32>(layer) + kLayerFlags) & 1u) == 0u)
        return render::kNoLights;
    const u32 manager = c.mem_read32(c.gpr[13] + kSdaLightManager);
    if (manager == 0u)
        return render::kNoLights;
    const u32 record = light_group >= 0 ? manager + kGroupRecords + kGroupStride * static_cast<u32>(light_group) : 0u;
    render::LightSet set;
    float ambient_sum[3] = {0, 0, 0};
    for (u32 k = 0; k < kManagerLights; ++k) {
        const u32 wrapper = c.mem_read32(manager + 4u * k);
        const u32 light = wrapper != 0u ? c.mem_read32(wrapper + kWrapperLight) : 0u;
        if (light == 0u || (c.mem_read8(light + layout::kLightFlags) & 1u) == 0u)
            continue;
        // Lights 0 and 1 take the group's colours; any other keeps its own.
        const u32 colour_at = record != 0u && k < 2u ? record + 0x10u * k : light + layout::kLightColour;
        const float colour[3] = {read_f32(c, colour_at), read_f32(c, colour_at + 4u), read_f32(c, colour_at + 8u)};
        const u8 type = c.mem_read8(light + layout::kLightSubType);
        if (type == 2u) {
            for (int i = 0; i < 3; ++i)
                ambient_sum[i] += colour[i];
            *ambient = true;
        } else if (type == 1u && set.directional_count < 4u) {
            render::Float3 at;
            if (record != 0u && k == 1u) {
                constexpr float kRadians = 3.14159265358979f / 180.0f;
                const float a = static_cast<float>(static_cast<s32>(c.mem_read32(record + 0x2Cu))) * kRadians;
                const float b = static_cast<float>(static_cast<s32>(c.mem_read32(record + 0x30u))) * kRadians;
                at = render::Float3{std::cos(a) * std::sin(b), -std::sin(a), std::cos(a) * std::cos(b)};
            } else {
                const u32 frame = c.mem_read32(light + layout::kLightFrame);
                if (frame == 0u)
                    continue;
                const bool root = c.mem_read32(frame + layout::kFrameRoot) == frame;
                const u32 matrix = frame + (root ? layout::kFrameModelling : layout::kFrameLtm);
                at = render::Float3{read_f32(c, matrix + 0x20u), read_f32(c, matrix + 0x24u), read_f32(c, matrix + 0x28u)};
            }
            const std::uint32_t n = set.directional_count++;
            set.directional_colour[n] = render::Float3{colour[0], colour[1], colour[2]};
            set.directional_vector[n] = at;
            *lit = true;
        }
    }
    set.ambient = render::Float3{std::min(ambient_sum[0], 1.0f), std::min(ambient_sum[1], 1.0f),
                                 std::min(ambient_sum[2], 1.0f)};
    if (out != nullptr)
        *out = set;
    return builder_.lights(set);
}

// A display layer's render states. Before each layer's pass 0x800471BC calls 0x80046D80(land 0x80572560, flags,
// world), which sets them from the layer's flags (0x8051E328 + 0x18 i + 8) through RwRenderStateSet ([r13+0x615C]
// +0x20: 6 z test, 8 z write, 9 filter, 0xA / 0xB source / destination blend, 0xE fog, 0x14 cull) and GX's alpha
// compare (0x80498F74) - the first of these bits that is set decides the blend:
//   0x40   source alpha / inverse source alpha, z test on, z write off, compare ALWAYS        (0x80046DD8)
//   0x80   source alpha / one (additive), z test on, z write off, compare ALWAYS              (0x80046E64)
//   0x100  RW blends 10 / 1 (ZERO), z left as it was, compare ALWAYS                         (0x80046EF0)
//   0x800  source alpha / inverse source alpha, z write and test on, compare GREATER 0xC8     (0x80046F4C)
//   0x400  source alpha / inverse source alpha, z test and write off, compare ALWAYS          (0x80046FE0)
//   none   source alpha / inverse source alpha, z test and write on, compare GREATER 1        (0x8004706C)
// then 0x8 the texture filter (2, else 0), 0x10 culling (none, else back), 0x4 fog = the land's byte +0x1809 (off
// when clear). The fog's planes and colour are the in-stage camera's (world_recipe_). FarObjects::kOpaqueLevel: the
// states CameraRender sets for render level 2 through the same function with the level's mask.
render::Recipe RwDriver::layer_recipe(Cpu& c, s32 layer, float* cutoff) const {
    constexpr u32 kLayerTable = 0x8051E328u, kLayerStride = 0x18u, kLayerFlags = 0x08u;
    constexpr u32 kLand = 0x80572560u, kLandFog = 0x1809u;
    const auto blend = [](u32 rw) { return static_cast<render::Blend>(rw - 1u); };  // as recipe(): RW 1..10
    // Render level 2 (Opeq) passes its own mask: the level table 0x804D24EC, 0x14 bytes a level, mask at +8.
    constexpr u32 kRenderLevels = 0x804D24ECu, kRenderLevelStride = 0x14u, kRenderLevelMask = 0x08u, kOpeq = 2u;
    render::Recipe r = world_recipe_;
    r.alpha_cutoff = 0.0f;
    const u32 flags = layer == FarObjects::kOpaqueLevel
                          ? c.mem_read32(kRenderLevels + kRenderLevelStride * kOpeq + kRenderLevelMask)
                          : c.mem_read32(kLayerTable + kLayerStride * static_cast<u32>(layer) + kLayerFlags);
    *cutoff = 0.0f;
    if ((flags & 0x40u) != 0u) {
        r.src = blend(5u), r.dst = blend(6u), r.depth_test = true, r.depth_write = false;
    } else if ((flags & 0x80u) != 0u) {
        r.src = blend(5u), r.dst = blend(2u), r.depth_test = true, r.depth_write = false;
    } else if ((flags & 0x100u) != 0u) {
        r.src = blend(10u), r.dst = blend(1u);
    } else if ((flags & 0x800u) != 0u) {
        r.src = blend(5u), r.dst = blend(6u), r.depth_test = true, r.depth_write = true;
        *cutoff = 200.0f / 255.0f;
    } else if ((flags & 0x400u) != 0u) {
        r.src = blend(5u), r.dst = blend(6u), r.depth_test = false, r.depth_write = false;
    } else {
        r.src = blend(5u), r.dst = blend(6u), r.depth_test = true, r.depth_write = true;
        *cutoff = 1.0f / 255.0f;
    }
    r.cull = (flags & 0x10u) != 0u ? render::Cull::None : render::Cull::Back;
    r.fog = (flags & 0x4u) != 0u && c.mem_read8(kLand + kLandFog) != 0u;  // clear: FOGENABLE 0 (0x80047190)
    // Level 2's draws come after the stage's fog task (TFogSet, created before every object task, children run in
    // creation order), which sets FOGCOLOR (0x8016AC24) and FOGTYPE (0x8016AC3C) each frame. The driver re-arms GX
    // fog with the camera's planes only when either changes (r2/SITES_DEVICE.md 1.2; render_states.cpp rearm_fog) -
    // while the fog colour eases between areas - so the rings draw fogged on exactly those frames.
    if (layer == FarObjects::kOpaqueLevel && level_fog_frame_ == stats_.frames) {
        r.fog = true;
        r.fog_start = level_fog_start_;
        r.fog_end = level_fog_end_;
    }
    return r;
}

render::LightSetId RwDriver::light_set(Cpu& c, bool* lit, bool* ambient, render::LightSet* out) {
    *lit = *ambient = false;
    const u32 globals = c.mem_read32(layout::kRwGlobals);
    const u32 world = c.mem_read32(globals + layout::kGlobalsCurrentWorld);
    if (world == 0u)
        return render::kNoLights;
    render::LightSet set;
    float ambient_sum[3] = {0, 0, 0};
    const u32 head = world + layout::kWorldDirectionalLights;
    u32 link = c.mem_read32(head);
    for (int guard = 0; link != head && link != 0u && guard < 64; ++guard) {
        const u32 light = link - layout::kLightInWorldLink;
        if ((c.mem_read8(light + layout::kLightFlags) & 1u) != 0u) {
            const float colour[3] = {read_f32(c, light + layout::kLightColour),
                                     read_f32(c, light + layout::kLightColour + 4u),
                                     read_f32(c, light + layout::kLightColour + 8u)};
            const u8 type = c.mem_read8(light + layout::kLightSubType);
            if (type == 2u) {
                for (int k = 0; k < 3; ++k)
                    ambient_sum[k] += colour[k];
                *ambient = true;
            } else if (type == 1u && set.directional_count < 4u) {
                const u32 frame = c.mem_read32(light + layout::kLightFrame);
                if (frame != 0u) {
                    const bool root = c.mem_read32(frame + layout::kFrameRoot) == frame;
                    const u32 matrix = frame + (root ? layout::kFrameModelling : layout::kFrameLtm);
                    const std::uint32_t n = set.directional_count++;
                    set.directional_colour[n] = render::Float3{colour[0], colour[1], colour[2]};
                    set.directional_vector[n] = render::Float3{read_f32(c, matrix + 0x20u), read_f32(c, matrix + 0x24u),
                                                               read_f32(c, matrix + 0x28u)};
                    *lit = true;
                }
            }
        }
        link = c.mem_read32(link);
    }
    set.ambient = render::Float3{std::min(ambient_sum[0], 1.0f), std::min(ambient_sum[1], 1.0f),
                                 std::min(ambient_sum[2], 1.0f)};
    if (out != nullptr)
        *out = set;
    return builder_.lights(set);
}

// The deforming-mesh audit's first report per (issue, geometry): stderr, capped.
void RwDriver::deform_report(u32 issue, u32 geometry, const char* text) {
    const u64 key = (u64{issue} << 32) | geometry;
    if (deform_reported_.size() >= 256u || !deform_reported_.insert(key).second)
        return;
    std::fprintf(stderr, "[r2-deform] frame %llu issue %u geometry %08X: %s\n",
                 static_cast<unsigned long long>(stats_.frames), issue, geometry, text);
}

// A model's native mesh from its RpGeometry as RenderWare keeps it in memory: morph target 0's positions and normals,
// the prelit colours, the UV sets, and per material the mesh header's index list (strips or triangles); for a skin,
// its bone indices and weights. Built at the first draw and kept in the geometry's instance slot (RpGeometry+0x58,
// never written by anything else in the lifted build); rebuilt when the mesh header's serial changes or a lock asked
// for it (DMorph's per-frame morph).
render::MeshHandle RwDriver::geometry_mesh(Cpu& c, u32 geometry, u32 flags, u32 mesh_header, u32 mesh_count,
                                           bool locked, u32 skin, u32* dmorph_out) {
    const u32 word = geometry + layout::kGeometryRepEntry;
    const u32 serial = c.mem_read16(mesh_header + 6u);
    const u32 value = c.mem_read32(word);
    u32 dmorph = dmorph_data(c, geometry);
    *dmorph_out = 0u;
    if (value != 0u) {
        for (Owned& owned : meshes_) {
            if (owned.handle != value || owned.word != word)
                continue;
            // A delta-morphed geometry's lock is its morph (the DMorph render callback is the only run-time lock in
            // the game, docs/mesh-re-20261007/M3_INSTANCING_NATIVE.md): the mesh keeps its base shape and targets,
            // and each draw carries the weights - nothing is rebuilt, so no queued frame loses its mesh.
            if (owned.serial == serial && owned.skin == skin && owned.dmorph == dmorph && (!locked || dmorph != 0u)) {
                owned.drawn_frame = stats_.frames;
                *dmorph_out = dmorph;
                return render::MeshHandle{value};
            }
            if (owned.drawn_frame == stats_.frames) {
                // A second lock in one frame (DMorph's per-atomic weights on a shared geometry): the mesh an earlier
                // draw of this frame took is about to be released under it.
                ++stats_.meshes_replaced_in_frame;
                deform_report(1u, geometry, "mesh rebuilt after a draw of the same frame took it");
            }
            renderer_.release(render::MeshHandle{value}, stats_.frames);
            owned.handle = 0u;
            ++stats_.models_rebuilt;
            break;
        }
    }
    const u32 vertices = c.mem_read32(geometry + layout::kGeometryVertexCount);
    const u32 morph = c.mem_read32(geometry + layout::kGeometryMorphTarget);
    if (vertices == 0u || vertices > (1u << 20) || morph == 0u) {
        ++stats_.model_failures;
        return {};
    }
    assets::MeshData mesh;
    mesh.vertex_count = vertices;
    if (!guest_floats(c, c.mem_read32(morph + layout::kMorphTargetVertices), std::size_t{vertices} * 3u, mesh.positions)) {
        ++stats_.model_failures;
        return {};
    }
    if ((flags & kGeometryNormals) != 0u)
        guest_floats(c, c.mem_read32(morph + layout::kMorphTargetNormals), std::size_t{vertices} * 3u, mesh.normals);
    if (dmorph != 0u && !dmorph_targets(c, dmorph, &mesh)) {
        // Unreadable targets: drawn as the game's arrays hold it, rebuilt at each lock (counted).
        ++stats_.dmorph_unsupported;
        deform_report(5u, geometry, "delta-morph targets unreadable: drawn from the morphed arrays");
        mesh.delta_targets.clear();
        dmorph = 0u;
    }
    if ((flags & kGeometryPrelit) != 0u) {
        const u32 prelit = c.mem_read32(geometry + layout::kGeometryPrelit);
        if (const u8* p = prelit != 0u ? c.guest_memory().peek(GcAddr(prelit), std::size_t{vertices} * 4u) : nullptr)
            mesh.colours.assign(p, p + std::size_t{vertices} * 4u);
    }
    const u32 uv_sets = c.mem_read32(geometry + layout::kGeometryUvSets);
    if (uv_sets > 0u)
        guest_floats(c, c.mem_read32(geometry + layout::kGeometryTexCoords), std::size_t{vertices} * 2u, mesh.uv0);
    if (uv_sets > 1u)
        guest_floats(c, c.mem_read32(geometry + layout::kGeometryTexCoords + 4u), std::size_t{vertices} * 2u, mesh.uv1);
    if (skin != 0u) {
        // The console's single-weight skins take the matrix-palette path (instancer 0x80451988, render 0x8044EEC4):
        // each vertex drawn by its first bone alone. The CPU skinners (0x8044E5A4, 0x8044E738, 0x804520D0) blend
        // maxNumWeightsForVertex weights.
        const u32 max_weights = c.mem_read32(skin + layout::kSkinMaxWeights);
        const u32 indices = c.mem_read32(skin + layout::kSkinVertexIndices);
        const u8* p = indices != 0u ? c.guest_memory().peek(GcAddr(indices), std::size_t{vertices} * 4u) : nullptr;
        if (p == nullptr || max_weights == 0u || max_weights > 4u ||
            !guest_floats(c, c.mem_read32(skin + layout::kSkinVertexWeights), std::size_t{vertices} * 4u,
                          mesh.skin_weights)) {
            ++stats_.model_failures;
            return {};
        }
        // Each vertex's indices are one u32, bone k in bits 8k..8k+7 (the skinners take `(word & 0xFF) << 6` as the
        // first bone's matrix offset, 0x8044E684): big-endian in memory, so bone k is the byte at offset 3 - k.
        mesh.skin_indices.resize(std::size_t{vertices} * 4u);
        for (std::size_t i = 0; i < std::size_t{vertices} * 4u; ++i)
            mesh.skin_indices[i] = p[(i & ~std::size_t{3}) + (3u - (i & 3u))];
        float min_sum = 1.0e9f, max_sum = -1.0e9f;
        u32 off = 0;
        for (std::size_t v = 0; v < vertices; ++v) {
            float* w = &mesh.skin_weights[v * 4u];
            for (u32 k = 0; k < 4u; ++k) {
                if (max_weights == 1u)
                    w[k] = k == 0u ? 1.0f : 0.0f;
                else if (k >= max_weights)
                    w[k] = 0.0f;
            }
            const float sum = w[0] + w[1] + w[2] + w[3];
            min_sum = std::min(min_sum, sum);
            max_sum = std::max(max_sum, sum);
            if (sum < 0.98f || sum > 1.02f)
                ++off;
        }
        if (off != 0u) {
            ++stats_.skins_unnormalised;
            stats_.skin_vertices_unnormalised += off;
            if (deform_unreported(2u, geometry)) {
                char text[160];
                std::snprintf(text, sizeof(text), "skin weights not summing to 1: %u of %u vertices, sums %.3f..%.3f, "
                              "max weights %u", off, vertices, min_sum, max_sum, max_weights);
                deform_report(2u, geometry, text);
            }
        }
    }
    const bool strips = (c.mem_read32(mesh_header) & kMeshHeaderTriStrip) != 0u || (flags & kGeometryTriStrip) != 0u;
    std::vector<std::uint32_t> list;
    for (u32 j = 0; j < mesh_count; ++j) {
        const u32 m = mesh_header + layout::kMeshFirst + j * layout::kMeshStride;
        const u32 indices = c.mem_read32(m);
        const u32 count = c.mem_read32(m + 4u);
        assets::MeshData::Submesh sub;
        sub.first_index = static_cast<std::uint32_t>(mesh.indices.size());
        sub.material = j;
        const u8* p = indices != 0u && count != 0u && count < (1u << 22)
                          ? c.guest_memory().peek(GcAddr(indices), std::size_t{count} * 2u)
                          : nullptr;
        if (p != nullptr) {
            list.resize(count);
            for (u32 i = 0; i < count; ++i)
                list[i] = static_cast<std::uint32_t>((p[i * 2u] << 8) | p[i * 2u + 1u]);
            if (strips) {
                assets::append_strip_triangles(list.data(), list.size(), mesh.indices);
            } else {
                for (u32 i = 0; i + 2u < count; i += 3u)
                    mesh.indices.insert(mesh.indices.end(), {list[i], list[i + 1u], list[i + 2u]});
            }
        }
        sub.index_count = static_cast<std::uint32_t>(mesh.indices.size()) - sub.first_index;
        mesh.submeshes.push_back(sub);
    }
    for (std::uint32_t index : mesh.indices) {
        if (index >= vertices) {
            ++stats_.model_failures;
            return {};
        }
    }
    if (mesh.indices.empty()) {
        ++stats_.model_failures;
        return {};
    }
    mesh.bounds = assets::bounding_sphere(mesh.positions);
    std::string error;
    // Made by the renderer's resource worker (vertex streams, GPU buffers, uploads); the handle is valid now and a
    // packet that draws it waits for it (ES 2026-10-19). The mesh is not used here after this.
    const render::MeshHandle handle = renderer_.request_mesh(render::Scope::Session, std::move(mesh), &error);
    if (!handle) {
        ++stats_.model_failures;
        return {};
    }
    c.mem_write32(word, handle.value);
    meshes_.push_back(Owned{handle.value, word, serial, skin, stats_.frames, dmorph});
    ++stats_.models_built;
    if (dmorph != 0u)
        ++stats_.dmorph_meshes;
    *dmorph_out = dmorph;
    return handle;
}

// RpDMorph (docs/mesh-re-20261007/M4_DEFORMATION.md 2.2, 3.1): the game blends a geometry's delta targets into its
// arrays in place at every weight change (base + sum w_t delta_t, the applied weights kept per geometry). The native
// mesh carries the base shape and the targets instead; every draw blends its own weights (the renderer's deformation
// pass). Targets moving prelit colours or UVs (none on the disc) and more targets than the renderer blends are left to
// the arrays (rebuilt at each lock, counted).
u32 RwDriver::dmorph_data(Cpu& c, u32 geometry) {
    const u32 offset = c.mem_read32(layout::kDMorphGeometryOffset);
    if (offset == 0u || offset >= 0x1000u)
        return 0u;
    const u32 data = c.mem_read32(geometry + offset);
    if (data == 0u)
        return 0u;
    const u32 count = c.mem_read32(data + layout::kDMorphTargetCount);
    const u32 targets = c.mem_read32(data + layout::kDMorphTargets);
    if (count == 0u || targets == 0u || c.mem_read32(data + layout::kDMorphCurrentValues) == 0u)
        return 0u;
    bool native = count <= render::kMaxMorphTargets;
    for (u32 t = 0; native && t < count; ++t) {
        const u32 moves = c.mem_read32(targets + t * layout::kDMorphTargetSize + layout::kDMorphTargetFlags);
        native = (moves & ~(kGeometryPositions | kGeometryNormals)) == 0u;
    }
    if (!native) {
        ++stats_.dmorph_unsupported;
        deform_report(5u, geometry, "delta-morph targets the native path does not blend (prelit / UV deltas, or "
                                    "too many targets): drawn from the morphed arrays");
        return 0u;
    }
    return data;
}

bool RwDriver::dmorph_targets(Cpu& c, u32 dmorph, assets::MeshData* mesh) {
    const u32 count = c.mem_read32(dmorph + layout::kDMorphTargetCount);
    const u32 targets = c.mem_read32(dmorph + layout::kDMorphTargets);
    const std::size_t vc = mesh->vertex_count;
    std::vector<float> applied;
    if (!guest_floats(c, c.mem_read32(dmorph + layout::kDMorphCurrentValues), count, applied))
        return false;
    mesh->delta_targets.assign(count, assets::MeshData::DeltaTarget{});
    std::vector<float> data;
    for (u32 t = 0; t < count; ++t) {
        const u32 k = targets + t * layout::kDMorphTargetSize;
        const u32 moves = c.mem_read32(k + layout::kDMorphTargetFlags);
        const u32 codes = c.mem_read32(k + layout::kDMorphTargetRleCodeCount);
        const u32 elements = c.mem_read32(k + layout::kDMorphTargetRleDataCount);
        const u32 code = c.mem_read32(k + layout::kDMorphTargetRleCode);
        const u8* rle = code != 0u && codes != 0u && codes < (1u << 20) ? c.guest_memory().peek(GcAddr(code), codes)
                                                                         : nullptr;
        if (rle == nullptr || elements == 0u || elements > vc)
            return false;
        // The run-length code walks the vertices: bit 7 set = the next (c & 0x7F) vertices take the next offsets,
        // clear = skip (c & 0x7F) vertices; positions and normals walk the same runs.
        const auto expand = [&](u32 source, std::vector<float>& out) {
            if (!guest_floats(c, source, std::size_t{elements} * 3u, data))
                return false;
            out.assign(vc * 3u, 0.0f);
            std::size_t v = 0, e = 0;
            for (u32 i = 0; i < codes; ++i) {
                const u32 n = rle[i] & 0x7Fu;
                if ((rle[i] & 0x80u) == 0u) {
                    v += n;
                    continue;
                }
                for (u32 j = 0; j < n; ++j, ++v, ++e) {
                    if (v >= vc || e >= elements)
                        return false;
                    for (std::size_t axis = 0; axis < 3u; ++axis)
                        out[v * 3u + axis] = data[e * 3u + axis];
                }
            }
            return true;
        };
        assets::MeshData::DeltaTarget& d = mesh->delta_targets[t];
        if ((moves & kGeometryPositions) != 0u && !expand(c.mem_read32(k + layout::kDMorphTargetVertices), d.positions))
            return false;
        if ((moves & kGeometryNormals) != 0u && !mesh->normals.empty() &&
            !expand(c.mem_read32(k + layout::kDMorphTargetNormals), d.normals))
            return false;
    }
    // The arrays hold base + sum applied_t delta_t: the applied weights come back out, leaving the authored base.
    for (u32 t = 0; t < count; ++t) {
        const float w = applied[t];
        if (w == 0.0f)
            continue;
        const assets::MeshData::DeltaTarget& d = mesh->delta_targets[t];
        for (std::size_t i = 0; i < d.positions.size(); ++i)
            mesh->positions[i] -= w * d.positions[i];
        for (std::size_t i = 0; i < d.normals.size(); ++i)
            mesh->normals[i] -= w * d.normals[i];
    }
    return true;
}

render::Float3x4 RwDriver::frame_ltm(Cpu& c, u32 frame) {
    ++stats_.bone_frames;
    const u32 root = c.mem_read32(frame + layout::kFrameRoot);
    if (root == 0u || (c.mem_read8(root + layout::kFramePrivateFlags) & 1u) == 0u)
        return frame_matrix(c, frame + layout::kFrameLtm);
    // LTM(frame) = modelling(frame) x LTM(parent) in RenderWare's row order; the root's LTM is its modelling matrix.
    ++stats_.dirty_bone_frames;
    render::Float3x4 ltm = frame_matrix(c, frame + layout::kFrameModelling);
    u32 parent = c.mem_read32(frame + layout::kFrameParent);
    for (int guard = 0; parent != 0u && guard < 256; ++guard) {
        ltm = compose(frame_matrix(c, parent + layout::kFrameModelling), ltm);
        parent = c.mem_read32(parent + layout::kFrameParent);
    }
    return ltm;
}

// The bone palette (Bones 0x8044E394, r2/SITES_PIPELINES.md 2.5 and 3.6), world space, for each bone b the skin uses
// (RenderWare's row-vector order):
//   NOMATRICES (the hierarchies the game builds at run time: its animation poses the bones' frames)
//               invBind[b] x LTM(node frame b), the frame from the hierarchy's node info;
//   LOCALSPACE  invBind[b] x matrices[b] x LTM(atomic);
//   otherwise   invBind[b] x matrices[b] (global matrices).
// The console's x invert(LTM(atomic)) (CPU skinning) or x V (matrix palette) and its later x LTM x V cancel to these.
// Bones the skin does not use stay zero.
bool RwDriver::skin_palette(Cpu& c, u32 atomic, u32 skin, render::BoneRange* out) {
    const u32 hierarchy = c.mem_read32(atomic + c.mem_read32(layout::kSkinAtomicOffset));
    const u32 bones = c.mem_read32(skin + layout::kSkinBoneCount);
    const u32 used = c.mem_read32(skin + layout::kSkinUsedBoneCount);
    const u32 used_ids = c.mem_read32(skin + layout::kSkinUsedBones);
    const u32 inverse_bind = c.mem_read32(skin + layout::kSkinInverseBind);
    stats_.last_skin_bones = bones;
    if (hierarchy == 0u) {
        ++stats_.skins_unsupported;
        ++stats_.skins_without_hierarchy;
        return false;
    }
    if (bones == 0u || bones > render::kMaxPaletteBones || used > bones || used_ids == 0u || inverse_bind == 0u) {
        ++stats_.skins_unsupported;
        ++stats_.skins_bad_bones;
        return false;
    }
    const u32 flags = c.mem_read32(hierarchy + layout::kHierarchyFlags);
    const u32 nodes = c.mem_read32(hierarchy + layout::kHierarchyNodeCount);
    const bool node_frames = (flags & layout::kHierarchyNoMatrices) != 0u;
    const u32 source = c.mem_read32(hierarchy + (node_frames ? layout::kHierarchyNodeInfo : layout::kHierarchyMatrices));
    stats_.last_hierarchy_flags = flags;
    stats_.last_hierarchy_nodes = nodes;
    if (source == 0u) {
        ++stats_.skins_unsupported;
        ++stats_.skins_without_matrices;
        return false;
    }
    if (!node_frames) {
        ++stats_.skins_from_matrices;
        if (const u32 geometry = c.mem_read32(atomic + layout::kAtomicGeometry); deform_unreported(4u, geometry)) {
            char text[96];
            std::snprintf(text, sizeof(text), "skin posed from hierarchy matrices (flags %X, hierarchy %08X)", flags,
                          hierarchy);
            deform_report(4u, geometry, text);
        }
    }
    render::Float3x4 base;  // identity: global matrices and node frames are world space
    if (!node_frames && (flags & layout::kHierarchyLocalSpace) != 0u) {
        const u32 frame = c.mem_read32(atomic + layout::kAtomicFrame);
        if (frame != 0u)
            base = frame_matrix(c, frame + layout::kFrameLtm);
    }
    render::Float3x4 zero;
    for (auto& row : zero.m)
        for (float& value : row)
            value = 0.0f;
    std::vector<render::Float3x4> palette(bones, zero);
    for (u32 i = 0; i < used; ++i) {
        const u32 b = c.mem_read8(used_ids + i);
        const u32 frame =
            node_frames && b < nodes ? c.mem_read32(source + layout::kNodeInfoSize * b + layout::kNodeInfoFrame) : 0u;
        if (b >= bones || b >= nodes || (node_frames && frame == 0u)) {
            ++stats_.skins_unsupported;
            ++stats_.skins_bad_bones;
            return false;
        }
        const render::Float3x4 pose = node_frames ? frame_ltm(c, frame) : compose(base, frame_matrix(c, source + 64u * b));
        palette[b] = compose(pose, frame_matrix(c, inverse_bind + 64u * b));
        if (stats_.frames == trace_frame_ && node_frames) {
            const u32 root = c.mem_read32(frame + layout::kFrameRoot);
            std::fprintf(stderr,
                         "[r2-trace]     bone %u frame %08X flags %02X root %08X (flags %02X) ltm pos (%.1f %.1f %.1f) "
                         "modelling pos (%.1f %.1f %.1f) parent %08X\n",
                         b, frame, c.mem_read8(frame + layout::kFramePrivateFlags), root,
                         root != 0u ? c.mem_read8(root + layout::kFramePrivateFlags) : 0u,
                         read_f32(c, frame + layout::kFrameLtm + 0x30u), read_f32(c, frame + layout::kFrameLtm + 0x34u),
                         read_f32(c, frame + layout::kFrameLtm + 0x38u), read_f32(c, frame + layout::kFrameModelling + 0x30u),
                         read_f32(c, frame + layout::kFrameModelling + 0x34u),
                         read_f32(c, frame + layout::kFrameModelling + 0x38u), c.mem_read32(frame + layout::kFrameParent));
        }
    }
    if (skin_hash_) {  // measuring: every bone matrix this frame, in draw order (SHADOW_SKIN_HASH)
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(palette.data());
        u64 own = 0xCBF29CE484222325ull;
        for (std::size_t i = 0; i < palette.size() * sizeof(render::Float3x4); ++i) {
            frame_skin_hash_ = (frame_skin_hash_ ^ bytes[i]) * 0x100000001B3ull;
            own = (own ^ bytes[i]) * 0x100000001B3ull;
        }
        ++frame_skins_;
        if (skin_hash_detail_)  // SHADOW_SKIN_HASH=2: each skin on its own line
            std::fprintf(stderr, "[skinpart] %llu atomic %08X geometry %08X bones %u at %.2f %.2f %.2f hash %016llX\n",
                         static_cast<unsigned long long>(stats_.frames), atomic, c.mem_read32(atomic + 0x18u), bones,
                         palette.empty() ? 0.0 : static_cast<double>(palette[0].m[0][3]),
                         palette.empty() ? 0.0 : static_cast<double>(palette[0].m[1][3]),
                         palette.empty() ? 0.0 : static_cast<double>(palette[0].m[2][3]),
                         static_cast<unsigned long long>(own));
    }
    *out = builder_.bones(palette.data(), bones);
    return true;
}

// RpGeometryDestroy and its inlined copies call RwResourcesFreeResEntry(geometry->repEntry): the instance is the
// native mesh this layer put there (nothing else writes the slot in the lifted build). Released, the slot cleared as
// RwResourcesFreeResEntry clears its owner's reference; returns 1 as it does.
u32 RwDriver::geometry_instance_release(Cpu& c, u32 entry) {
    for (Owned& owned : meshes_) {
        if (owned.handle == entry && owned.handle != 0u) {
            if (c.mem_read32(owned.word) == entry)
                c.mem_write32(owned.word, 0u);
            renderer_.release(render::MeshHandle{entry}, stats_.frames);
            owned.handle = 0u;
            ++stats_.geometry_releases;
            return 1u;
        }
    }
    c.request_stop(StopReason::ExecutionFault,
                   "a geometry instance the R2 scaffolding did not create reached RwResourcesFreeResEntry (" +
                       std::to_string(entry) + "): translated memory cannot be freed from native code");
    return 0u;
}

// RpMatFX's layers of a material (the GameCube render callback 0x804A62D0, shared by the MatFX atomic and sector
// pipelines - renderer-re 04_GC_DRIVER.md 3.18): the environment reflection, the dual texture (with the UV transform's
// dual matrix for DUALUVTRANSFORM), the UV transform of the base layer.
void RwDriver::matfx_layers(Cpu& c, u32 fx, u32 fx_kind, u32 geometry_flags, float cutoff, render::Material* material) {
    render::Material& m = *material;
    const auto uv_matrix = [&](u32 matrix, float (&out)[2][3]) {
        // u' = right.x u + up.x v + pos.x, v' = right.y u + up.y v + pos.y (4.5)
        out[0][0] = read_f32(c, matrix + 0x00u);
        out[0][1] = read_f32(c, matrix + 0x10u);
        out[0][2] = read_f32(c, matrix + 0x30u);
        out[1][0] = read_f32(c, matrix + 0x04u);
        out[1][1] = read_f32(c, matrix + 0x14u);
        out[1][2] = read_f32(c, matrix + 0x34u);
    };
    if (fx_kind == kEffectEnvironment) {
        bool env_alpha = false;
        const u32 env_texture = c.mem_read32(fx + 0x04u);
        m.extra.kind = render::ExtraPass::Kind::Environment;
        m.extra.texture = texture_of(c, env_texture, &env_alpha);
        m.extra.sampler = env_texture != 0u ? sampler_of(c.mem_read32(env_texture + layout::kTextureFilterAddressing))
                                            : render::Sampler{};
        m.extra.coefficient = std::min(read_f32(c, fx + 0x08u), 1.0f);
    } else if (fx_kind == kEffectBumpEnvironment) {
        // BUMPENVMAP (0x804A4E00, r7 = 1; docs/effects/lighting_materials.md 2): one pass, four TEV stages (mesh.hlsl
        // bump_environment). Slot 0 is the bump {frame +0, bumped texture +4, bump texture +8, coefficient +0xC,
        // invBumpWidth +0x10}; slot 1 (+0x18) the environment {frame, texture, coefficient, useFrameBufferAlpha}.
        // Map 0 is the bumped texture (0x804A4FDC), not the material's; the bump texgen's scale is
        // -(invBumpWidth x coefficient) (0x804A4FEC-0x804A5000). The frames are not read: the env frame is ignored as
        // for ENVMAP (Q3), and both texgens then use the camera's view matrix.
        const auto base_texture = m.texture.value;
        if (const u32 bumped = c.mem_read32(fx + 0x04u)) {
            bool bumped_alpha = false;
            m.texture = texture_of(c, bumped, &bumped_alpha);
            m.sampler = sampler_of(c.mem_read32(bumped + layout::kTextureFilterAddressing));
        }
        const u32 env_texture = c.mem_read32(fx + 0x1Cu);
        bool env_alpha = false;
        m.extra.kind = render::ExtraPass::Kind::BumpEnvironment;
        m.extra.texture = texture_of(c, env_texture, &env_alpha);
        m.extra.sampler = env_texture != 0u ? sampler_of(c.mem_read32(env_texture + layout::kTextureFilterAddressing))
                                            : render::Sampler{};
        m.extra.coefficient = std::min(read_f32(c, fx + 0x20u), 1.0f);
        m.extra.framebuffer_alpha = c.mem_read32(fx + 0x24u) != 0u;
        m.extra.bump_shift = -read_f32(c, fx + 0x10u) * read_f32(c, fx + 0x0Cu);
        if (bump_reported_.insert(fx).second)
            std::fprintf(stderr,
                         "[matfx] bump-env fx %08X bumped %08X bump %08X coef %g invWidth %g env %08X coef %g fbAlpha %u"
                         " frames %08X/%08X material texture handle %u -> %u\n",
                         fx, c.mem_read32(fx + 0x04u), c.mem_read32(fx + 0x08u), read_f32(c, fx + 0x0Cu),
                         read_f32(c, fx + 0x10u), env_texture, read_f32(c, fx + 0x20u), c.mem_read32(fx + 0x24u),
                         c.mem_read32(fx), c.mem_read32(fx + 0x18u), static_cast<unsigned>(base_texture),
                         static_cast<unsigned>(m.texture.value));
    } else if (fx_kind == kEffectDual || fx_kind == kEffectDualUvTransform) {
        const u32 slot = fx_kind == kEffectDual ? fx : fx + 0x18u;
        const u32 dual_texture = c.mem_read32(slot);
        bool dual_alpha = false;
        m.extra.kind = render::ExtraPass::Kind::Dual;
        m.extra.texture = texture_of(c, dual_texture, &dual_alpha);
        m.extra.sampler = dual_texture != 0u ? sampler_of(c.mem_read32(dual_texture + layout::kTextureFilterAddressing))
                                             : render::Sampler{};
        const u32 src = c.mem_read32(slot + 0x04u), dst = c.mem_read32(slot + 0x08u);
        m.extra.src = src >= 1u && src <= 10u ? static_cast<render::Blend>(src - 1u) : render::Blend::One;
        m.extra.dst = dst >= 1u && dst <= 10u ? static_cast<render::Blend>(dst - 1u) : render::Blend::One;
        m.extra.second_uv_set = (geometry_flags & kGeometryTextured2) != 0u;
        m.extra.alpha_cutoff = dual_alpha ? cutoff : 0.0f;
        // DUALUVTRANSFORM (0x804A4358): UV slot {base, dual} - the base layer takes the base matrix, the second layer
        // the DUAL matrix, identity when it has none (04_GC_DRIVER.md 3.18) - never the base.
        if (fx_kind == kEffectDualUvTransform) {
            if (const u32 base = c.mem_read32(fx))
                uv_matrix(base, m.uv_transform);
            if (const u32 dual = c.mem_read32(fx + 0x04u))
                uv_matrix(dual, m.extra.uv_transform);
        }
    } else if (fx_kind == kEffectUvTransform) {
        if (const u32 base = c.mem_read32(fx))
            uv_matrix(base, m.uv_transform);
    }
}

u32 RwDriver::pipeline_atomic(Cpu& c, u32 pipeline, u32 atomic) {
    ++stats_.atomics;
    tag_draws(c);
    const u32 geometry = c.mem_read32(atomic + layout::kAtomicGeometry);
    if (geometry == 0u)
        return pipeline;
    const u32 flags = c.mem_read32(geometry + layout::kGeometryFlags);
    const u32 mesh_header = c.mem_read32(geometry + layout::kGeometryMesh);
    const u32 mesh_count = mesh_header != 0u ? c.mem_read16(mesh_header + layout::kMeshHeaderCount) : 0u;
    if ((flags & kGeometryNative) == 0u &&
        (c.mem_read32(geometry + layout::kGeometryVertexCount) == 0u || mesh_count == 0u))
        return pipeline;  // the node body returns before any callback
    // (Re)instancing consumed the lock flags.
    const bool locked = c.mem_read16(geometry + layout::kGeometryLocked) != 0u;
    if (const u32 targets = c.mem_read32(geometry + layout::kGeometryMorphTargets); targets > 1u) {
        ++stats_.morph_target_draws;
        if (deform_unreported(3u, geometry)) {
            const u32 interp = atomic + layout::kAtomicInterpolatorFlags;
            char text[160];
            std::snprintf(text, sizeof(text), "%u morph targets drawn as target 0 (interpolator start %d end %d "
                          "time %.3f position %.3f, atomic %08X)", targets, static_cast<s16>(c.mem_read16(interp + 4u)),
                          static_cast<s16>(c.mem_read16(interp + 6u)), read_f32(c, interp + 8u),
                          read_f32(c, interp + 16u), atomic);
            deform_report(3u, geometry, text);
        }
    }
    if (stats_.frames == trace_frame_) {
        const u32 targets = c.mem_read32(geometry + layout::kGeometryMorphTargets);
        const u32 interp = atomic + layout::kAtomicInterpolatorFlags;
        std::fprintf(stderr,
                     "[r2-trace]   geometry %08X lock %04X morph targets %u interpolator flags %08X start %d end %d "
                     "time %.3f recip %.3f position %.3f\n",
                     geometry, c.mem_read16(geometry + layout::kGeometryLocked), targets, c.mem_read32(interp),
                     static_cast<s16>(c.mem_read16(interp + 4u)), static_cast<s16>(c.mem_read16(interp + 6u)),
                     read_f32(c, interp + 8u), read_f32(c, interp + 12u), read_f32(c, interp + 16u));
    }
    if (locked)
        c.mem_write16(geometry + layout::kGeometryLocked, 0u);
    if (c.mem_read32(geometry + layout::kGeometryMorphTargets) != 1u) {
        const u32 interpolator = atomic + layout::kAtomicInterpolatorFlags;
        const u32 value = c.mem_read32(interpolator);
        if ((value & 1u) != 0u)
            c.mem_write32(interpolator, value & ~1u);
    }
    // The lighting callback's frame stamp (0x80464478): lit geometry in a world advances RwGlobals->lightFrame. The
    // PTank pipeline's callback (0x80449D00) runs it only for geometry with normals.
    const u32 globals = c.mem_read32(layout::kRwGlobals);
    const bool ptank = pipeline == c.mem_read32(layout::kPtankPipeline);
    const bool in_world = c.mem_read32(globals + layout::kGlobalsCurrentWorld) != 0u;
    const bool lighting = (!ptank || (flags & kGeometryNormals) != 0u) && (flags & kGeometryLight) != 0u && in_world;
    if (lighting) {
        const u32 frame = globals + layout::kGlobalsLightFrame;
        c.mem_write16(frame, static_cast<u16>(c.mem_read16(frame) + 1u));
    }
    const bool textured = (flags & (kGeometryTextured | kGeometryTextured2)) != 0u;
    const u32 im = im_texture(c);
    const u32 texture_ext_offset = c.mem_read32(layout::kTextureExtOffset);
    const u32 white = c.mem_read32(layout::kWhiteRaster);
    const bool skinned = pipeline == c.mem_read32(layout::kSkinPipeline) ||
                         pipeline == c.mem_read32(layout::kSkinMatFxPipeline);
    const bool effects = pipeline == c.mem_read32(layout::kMatFxAtomicPipeline) ||
                         pipeline == c.mem_read32(layout::kSkinMatFxPipeline);
    // A particle system has no mesh: its one material draws its particles as a batch (below).
    const u32 skin = skinned ? c.mem_read32(geometry + c.mem_read32(layout::kSkinGeometryOffset)) : 0u;
    render::MeshHandle mesh;
    u32 dmorph = 0u;
    if (skinned && skin == 0u) {
        ++stats_.skins_unsupported;
        ++stats_.skins_without_skin;
    } else if (!ptank) {
        mesh = geometry_mesh(c, geometry, flags, mesh_header, mesh_count, locked, skin, &dmorph);
    }
    // A skinned mesh is posed by its palette into world space; any other by its frame.
    render::Float3x4 world;
    render::BoneRange bones;
    if (mesh && skinned) {
        if (!ensure_pass() || !skin_palette(c, atomic, skin, &bones))
            mesh = {};
    } else if (mesh) {
        const u32 frame = c.mem_read32(atomic + layout::kAtomicFrame);
        if (frame != 0u)
            world = frame_ltm(c, frame);
    }
    if (stats_.frames == trace_frame_) {
        const char* kind = ptank ? "ptank" : skinned ? (effects ? "skin+fx" : "skin") : effects ? "fx" : "default";
        const u32 hierarchy = skin != 0u ? c.mem_read32(atomic + c.mem_read32(layout::kSkinAtomicOffset)) : 0u;
        std::fprintf(stderr,
                     "[r2-trace] atomic %08X %-7s%s pipe %08X cb %08X geom %08X flags %08X verts %u meshes %u skin %08X "
                     "(bones %u maxw %u) hier %08X (flags %X nodes %u) mesh %08X palette %u world (%.1f %.1f %.1f) "
                     "sphere (%.1f %.1f %.1f r %.1f)\n",
                     atomic, kind, glare_window_ ? " [glare source]" : "", pipeline, c.mem_read32(atomic + 0x48u),
                     geometry, flags,
                     c.mem_read32(geometry + layout::kGeometryVertexCount), mesh_count, skin,
                     skin != 0u ? c.mem_read32(skin + layout::kSkinBoneCount) : 0u,
                     skin != 0u ? c.mem_read32(skin + layout::kSkinMaxWeights) : 0u, hierarchy,
                     hierarchy != 0u ? c.mem_read32(hierarchy + layout::kHierarchyFlags) : 0u,
                     hierarchy != 0u ? c.mem_read32(hierarchy + layout::kHierarchyNodeCount) : 0u, mesh.value,
                     bones.count, world.m[0][3], world.m[1][3], world.m[2][3], read_f32(c, atomic + 0x2Cu),
                     read_f32(c, atomic + 0x30u), read_f32(c, atomic + 0x34u), read_f32(c, atomic + 0x38u));
    }
    // A delta-morphed mesh's weights: the ones the DMorph render callback applied just before this draw (kept per
    // geometry; equal to this atomic's own weights at this point - every draw blends its own shape).
    render::MorphRange morph;
    if (mesh && dmorph != 0u && ensure_pass()) {
        const u32 count = std::min<u32>(c.mem_read32(dmorph + layout::kDMorphTargetCount), render::kMaxMorphTargets);
        const u32 values = c.mem_read32(dmorph + layout::kDMorphCurrentValues);
        float weights[render::kMaxMorphTargets] = {};
        for (u32 t = 0; t < count; ++t)
            weights[t] = read_f32(c, values + 4u * t);
        morph = builder_.morph(weights, count);
        ++stats_.dmorph_draws;
        if (stats_.frames == trace_frame_)
            std::fprintf(stderr, "[r2-trace]   delta morph %08X: %u targets, weights %.4f %.4f %.4f %.4f\n", dmorph,
                         count, weights[0], weights[1], weights[2], weights[3]);
    }
    bool lit = false, ambient = false;
    render::LightSetId lights = render::kNoLights;
    if (mesh && lighting && ensure_pass()) {
        if (far_.validating()) {  // measuring: the lights of this world under the light group applied now
            const u32 manager = c.mem_read32(c.gpr[13] + 0x2C30u);
            const u32 group = manager != 0u ? (c.mem_read8(manager + 0x454u) & 0x1Fu) : 0x1Fu;
            const u32 current_world = c.mem_read32(c.mem_read32(layout::kRwGlobals) + layout::kGlobalsCurrentWorld);
            GameLights& seen = game_lights_[current_world | group];
            lights = light_set(c, &seen.lit, &seen.ambient, &seen.set);
            lit = seen.lit;
            ambient = seen.ambient;
            if (skinned)
                game_skin_lights_[geometry] = SkinLights{seen, current_world, group};
        } else {
            lights = light_set(c, &lit, &ambient);
        }
    }
    const render::Recipe level = recipe();
    const float cutoff = alpha_cutoff();
    // Measuring: a draw inside one of the display layers' worlds has the states layer_recipe gives that layer's
    // proxies (the in-stage camera's draws only; fog compared as on / off).
    if (far_.validating() && mesh && current_camera_ != 0u &&
        current_camera_ == c.mem_read32(layout::kInStageCameraHolder)) {
        const u32 current_world = c.mem_read32(c.mem_read32(layout::kRwGlobals) + layout::kGlobalsCurrentWorld);
        // The layers' worlds, then (i = 15) the light manager's own, current under every CameraRender level: compared
        // for the geometries the level-2 proxies draw (the rings) only.
        const u32 manager_world = c.mem_read32(c.mem_read32(c.gpr[13] + 0x2C30u) + 0x45Cu);
        for (u32 i = 0; i < 16u && current_world != 0u; ++i) {
            if ((i < 15u ? c.mem_read32(0x8051E328u + 0x18u * i + 0x10u) : manager_world) != current_world)
                continue;
            if (i == 15u && opaque_level_geometries_.count(geometry) == 0u)
                break;
            float want_cutoff = 0.0f;
            const render::Recipe want =
                layer_recipe(c, i < 15u ? static_cast<s32>(i) : FarObjects::kOpaqueLevel, &want_cutoff);
            ++layer_state_checks_;
            const bool same = want.src == level.src && want.dst == level.dst && want.depth_test == level.depth_test &&
                              want.depth_write == level.depth_write && want.cull == level.cull &&
                              want.fog == level.fog && std::fabs(want_cutoff - cutoff) < 1e-6f;
            if (!same && ++layer_state_mismatches_ <= 16u)
                std::fprintf(stderr,
                             "[far-states] layer %u geometry 0x%08X: game blend %d/%d z %d/%d cull %d fog %d cutoff %.4f; "
                             "layer_recipe blend %d/%d z %d/%d cull %d fog %d cutoff %.4f\n",
                             i, geometry, static_cast<int>(level.src), static_cast<int>(level.dst),
                             level.depth_test ? 1 : 0, level.depth_write ? 1 : 0, static_cast<int>(level.cull),
                             level.fog ? 1 : 0, static_cast<double>(cutoff), static_cast<int>(want.src),
                             static_cast<int>(want.dst), want.depth_test ? 1 : 0, want.depth_write ? 1 : 0,
                             static_cast<int>(want.cull), want.fog ? 1 : 0, static_cast<double>(want_cutoff));
            break;
        }
    }
    const u32 effects_offset = c.mem_read32(layout::kMaterialEffectsOffset);
    // The fade-in: a model that starts being drawn inside the view dissolves in (gekko/appear_fade.h). Its position
    // is its frame's (a skinned mesh's world matrix is the palette's, not the model's).
    float dissolve = 0.0f;
    if (mesh) {
        const u32 frame = c.mem_read32(atomic + layout::kAtomicFrame);
        const render::Float3x4 at = frame != 0u ? frame_ltm(c, frame) : render::Float3x4{};
        const float position[3] = {at.m[0][3], at.m[1][3], at.m[2][3]};
        dissolve = appear_.draw(atomic, position, c.game_clock().now());
        // A model the game draws fully transparent (a holder's fade at alpha 0) is not seen: it hides no proxy. The
        // material's alpha counts only while the geometry modulates by the material colour (mesh.hlsl: c0 = modulate ?
        // colour : white): instances share their library's materials, and a fading instance's render callback
        // (0x801645D0) leaves its alpha in them; the others clear rpGEOMETRYMODULATEMATERIALCOLOR and draw opaque.
        bool seen = (flags & kGeometryModulate) == 0u;
        for (u32 j = 0; j < mesh_count && !seen; ++j) {
            const u32 material =
                c.mem_read32(mesh_header + layout::kMeshFirst + j * layout::kMeshStride + layout::kMeshMaterial);
            seen = material == 0u || (c.mem_read32(material + layout::kMaterialColour) & 0xFFu) != 0u;
        }
        if (seen)  // a skinned model at its frame (the object's): what an enemy's stand-in hands over to
            far_.real_drawn(geometry, at);
        else
            far_.real_drawn_transparent(geometry, at);  // measuring: a class trace lists these too
        far_.trace_atomic(c, atomic, geometry);         // measuring: a class trace's game atomics
    }
    // The proxies draw like the game's plain world models: remember this one's states.
    if (mesh && !skinned && !ptank && !effects && current_camera_ != 0u &&
        current_camera_ == c.mem_read32(layout::kInStageCameraHolder)) {
        world_recipe_ = level;
        world_cutoff_ = cutoff;
        world_recipe_valid_ = true;
    }
    if (dissolve > 0.0f)
        ++stats_.dissolving_draws;
    for (u32 j = 0; j < mesh_count; ++j) {
        const u32 material = c.mem_read32(mesh_header + layout::kMeshFirst + j * layout::kMeshStride + layout::kMeshMaterial);
        const u32 rw_texture = textured && material != 0u ? c.mem_read32(material + layout::kMaterialTexture) : 0u;
        bool has_alpha = false;
        render::Material m;
        if (rw_texture != 0u) {
            m.texture = texture_of(c, rw_texture, &has_alpha);
            m.sampler = sampler_of(c.mem_read32(rw_texture + layout::kTextureFilterAddressing));
            if (!m.texture)
                ++stats_.draws_without_texture;
        }
        const u32 fx = effects && material != 0u ? c.mem_read32(material + effects_offset) : 0u;
        const u32 fx_kind = fx != 0u ? c.mem_read32(fx + layout::kMatFxFlags) : 0u;
        if (textured)
            bind_side_effects(c, im, texture_ext_offset, white, rw_texture, has_alpha, &states_.alpha_test_off);
        else
            states_.alpha_test_off = 1u;
        if (fx_kind == kEffectEnvironment || fx_kind == kEffectBumpEnvironment)
            states_.alpha_test_off = 1u;  // the environment / bump base pass forces the test off (0x804987C8)
        if (ptank) {
            if (j == 0u && ensure_pass()) {
                render::Recipe r = level;
                r.alpha_cutoff = has_alpha ? cutoff : 0.0f;
                particle_batch(c, atomic, m, r);
            }
            continue;
        }
        if (!mesh || !ensure_pass())
            continue;
        if (material != 0u) {
            const u32 rgba = c.mem_read32(material + layout::kMaterialColour);
            m.colour = render::Float4{static_cast<float>((rgba >> 24) & 0xFFu) / 255.0f,
                                      static_cast<float>((rgba >> 16) & 0xFFu) / 255.0f,
                                      static_cast<float>((rgba >> 8) & 0xFFu) / 255.0f,
                                      static_cast<float>(rgba & 0xFFu) / 255.0f};
            m.ka = read_f32(c, material + layout::kMaterialAmbient);
        }
        // The decision table 0x80466624 (r2/SITES_PIPELINES.md 4.3) and the MatFX base pass (4.4).
        m.vertex_colours = (flags & kGeometryPrelit) != 0u;
        m.modulate = (flags & kGeometryModulate) != 0u;
        m.lit = lit;
        m.ambient = ambient;
        m.combine = effects ? render::Combine::OneStage
                    : (!textured || (m.vertex_colours && m.ambient)) ? render::Combine::TwoStage
                                                                    : render::Combine::OneStage;
        render::Recipe r = level;
        r.alpha_cutoff = has_alpha && fx_kind != kEffectEnvironment && fx_kind != kEffectBumpEnvironment ? cutoff : 0.0f;
        matfx_layers(c, fx, fx_kind, flags, cutoff, &m);
        // A registered (shared) UV animation: drawn at the shared native clock, as its far proxies are.
        if (uv_shared_enabled_ && far_.enabled() && material != 0u && (fx_kind == kEffectUvTransform || fx_kind == kEffectDualUvTransform) &&
            uv_shared_geometry(c, geometry))
            shared_uv(c, material, geometry, fx_kind, &m, false);
        if (m.extra.kind != render::ExtraPass::Kind::None)
            ++stats_.extra_passes;
        if (stats_.frames == trace_frame_) {
            std::fprintf(stderr,
                         "[r2-trace]   mesh %u material %08X colour %08X texture %08X (alpha %d) fx %u (%08X) blend %u/%u "
                         "z %d/%d cull %u fog %d cutoff %.3f combine %u P%d M%d L%d X%d ka %.2f extra %u coef %.2f\n",
                         j, material, material != 0u ? c.mem_read32(material + layout::kMaterialColour) : 0u,
                         m.texture.value, has_alpha ? 1 : 0, fx_kind, fx, static_cast<unsigned>(r.src),
                         static_cast<unsigned>(r.dst), r.depth_test ? 1 : 0, r.depth_write ? 1 : 0,
                         static_cast<unsigned>(r.cull), r.fog ? 1 : 0, r.alpha_cutoff,
                         static_cast<unsigned>(m.combine), m.vertex_colours ? 1 : 0, m.modulate ? 1 : 0, m.lit ? 1 : 0,
                         m.ambient ? 1 : 0, m.ka, static_cast<unsigned>(m.extra.kind), m.extra.coefficient);
        }
        m.dissolve = dissolve;
        builder_.draw(mesh, j, m, world, r, lights, bones, morph);
        ++stats_.model_draws;
        if (bones.count != 0u)
            ++stats_.skinned_draws;
    }
    return pipeline;
}

// The far-object proxies (far_objects.h): drawn at the end of the in-stage camera's bracket, once a frame, through
// that camera's pass - its fog, its lights, the recipe of the game's last plain world model.
void RwDriver::draw_far_proxies(Cpu& c) {
    if (far_drawn_this_frame_ || !world_recipe_valid_ || !far_.active() || !ensure_pass())
        return;
    far_drawn_this_frame_ = true;
    builder_.set_layer(render::EffectLayer::None);  // the world's far objects
    const render::Camera camera = camera_of(c, current_camera_);
    if (camera.parallel || !(camera.view_window_y > 0.0f))
        return;
    far_.collect(c, camera, 0.5f * far_pixel_height_ / camera.view_window_y, far_proxies_);
    if (far_proxies_.empty())
        return;
    bool lit = false, ambient = false;
    const render::LightSetId lights = light_set(c, &lit, &ambient);
    // A part whose display layer is known is lit as the game lights it there (layer_lights); one set per layer and
    // light group a frame.
    struct LayerLights {
        s32 layer, light_group;
        render::LightSetId lights;
        bool lit, ambient;
    };
    std::array<LayerLights, 16> layer_sets;
    std::size_t layer_set_count = 0u;
    // Each part draws with its layer's render states (layer_recipe); a part with no layer like the game's last plain
    // world model. The parts that write depth draw first, then the blended ones (the game draws its alpha, additive
    // and multiply layers after the plain ones), so a blended part never hides what stands behind it.
    struct LayerState {
        render::Recipe recipe;
        float cutoff = 0.0f;
        bool made = false;
    };
    std::array<LayerState, 16> layer_states;
    const LayerState plain{world_recipe_, world_cutoff_, true};
    const auto state_of = [&](s32 layer) -> const LayerState& {
        if (layer != FarObjects::kOpaqueLevel && (layer < 0 || layer >= 15))
            return plain;
        LayerState& s = layer_states[layer == FarObjects::kOpaqueLevel ? 15u : static_cast<std::size_t>(layer)];
        if (!s.made) {
            float cutoff = 0.0f;
            s.recipe = layer_recipe(c, layer, &cutoff);
            s.cutoff = cutoff;
            s.made = true;
        }
        return s;
    };
    for (int pass = 0; pass < 2; ++pass)
    for (const FarObjects::Proxy& proxy : far_proxies_) {
        const LayerState& state = state_of(proxy.layer);
        if (state.recipe.depth_write != (pass == 0))
            continue;
        if (far_.validating() && proxy.layer == FarObjects::kOpaqueLevel)
            opaque_level_geometries_.insert(c.mem_read32(proxy.atomic + layout::kAtomicGeometry));
        if (far_.validating() && proxy.bones != 0u) {  // measuring: an enemy stand-in against the game's lights
            const u32 geometry = c.mem_read32(proxy.atomic + layout::kAtomicGeometry);
            const auto seen = game_skin_lights_.find(geometry);
            if (seen != game_skin_lights_.end()) {
                bool l = false, a = false;
                render::LightSet values;
                u32 world = 0u;
                if (proxy.layer < 0) {
                    light_set(c, &l, &a, &values);
                    world = c.mem_read32(c.mem_read32(layout::kRwGlobals) + layout::kGlobalsCurrentWorld);
                } else {
                    layer_lights(c, proxy.layer, proxy.light_group, &l, &a, &values);
                    world = c.mem_read32(0x8051E328u + 0x18u * static_cast<u32>(proxy.layer) + 0x10u);
                }
                const GameLights& g = seen->second.lights;
                const auto same_light = [](const render::Float3& x, const render::Float3& y) {
                    return std::fabs(x.x - y.x) < 1e-3f && std::fabs(x.y - y.y) < 1e-3f && std::fabs(x.z - y.z) < 1e-3f;
                };
                bool same = l == g.lit && a == g.ambient && values.directional_count == g.set.directional_count &&
                            same_light(values.ambient, g.set.ambient);
                for (std::uint32_t k = 0; same && k < values.directional_count; ++k)
                    same = same_light(values.directional_colour[k], g.set.directional_colour[k]) &&
                           same_light(values.directional_vector[k], g.set.directional_vector[k]);
                same = same && world == seen->second.world;
                ++skin_light_checks_;
                if (!same && ++skin_light_mismatches_ <= 16u)
                    std::fprintf(stderr,
                                 "[far-lights] enemy geometry 0x%08X: stand-in world 0x%08X lit %d ambient %d (%.3f %.3f "
                                 "%.3f) dirs %u; game world 0x%08X group %u lit %d ambient %d (%.3f %.3f %.3f) dirs %u\n",
                                 geometry, world, l ? 1 : 0, a ? 1 : 0, values.ambient.x, values.ambient.y,
                                 values.ambient.z, values.directional_count, seen->second.world, seen->second.group,
                                 g.lit ? 1 : 0, g.ambient ? 1 : 0, g.set.ambient.x, g.set.ambient.y, g.set.ambient.z,
                                 g.set.directional_count);
            }
        }
        if (proxy.layer == -1) {
            draw_proxy_atomic(c, proxy.atomic, proxy.world, lights, lit, ambient, state.recipe, state.cutoff,
                              proxy.bones != 0u ? far_.palette(proxy) : nullptr, proxy.bones, proxy.uva);
            continue;
        }
        const LayerLights* set = nullptr;
        for (std::size_t i = 0; i < layer_set_count && set == nullptr; ++i)
            if (layer_sets[i].layer == proxy.layer && layer_sets[i].light_group == proxy.light_group)
                set = &layer_sets[i];
        LayerLights made{proxy.layer, proxy.light_group, render::kNoLights, false, false};
        render::LightSet values;
        if (far_.validating()) {  // measuring: the lights the game gave this world and group this frame, if it drew
            bool l = false, a = false;
            layer_lights(c, proxy.layer, proxy.light_group, &l, &a, &values);
            const u32 geometry = c.mem_read32(proxy.atomic + layout::kAtomicGeometry);
            // A render level's draws: the light manager's own world (+0x45C).
            const u32 world = proxy.layer == FarObjects::kOpaqueLevel
                                  ? c.mem_read32(c.mem_read32(c.gpr[13] + 0x2C30u) + 0x45Cu)
                                  : c.mem_read32(0x8051E328u + 0x18u * static_cast<u32>(proxy.layer) + 0x10u);
            const auto seen = game_lights_.find(world | (static_cast<u32>(proxy.light_group) & 0x1Fu));
            if (seen != game_lights_.end()) {
                ++light_checks_;
                if (seen->second.lit && light_checks_lit_++ < 4u)  // a sample of what is compared
                    std::fprintf(stderr,
                                 "[far-lights] sample: geometry 0x%08X layer %d group %d ambient %.3f %.3f %.3f / game "
                                 "%.3f %.3f %.3f; light colour %.3f %.3f %.3f / %.3f %.3f %.3f, at %.4f %.4f %.4f / "
                                 "%.4f %.4f %.4f\n",
                                 geometry, proxy.layer, proxy.light_group, values.ambient.x, values.ambient.y,
                                 values.ambient.z, seen->second.set.ambient.x, seen->second.set.ambient.y,
                                 seen->second.set.ambient.z, values.directional_colour[0].x,
                                 values.directional_colour[0].y, values.directional_colour[0].z,
                                 seen->second.set.directional_colour[0].x, seen->second.set.directional_colour[0].y,
                                 seen->second.set.directional_colour[0].z, values.directional_vector[0].x,
                                 values.directional_vector[0].y, values.directional_vector[0].z,
                                 seen->second.set.directional_vector[0].x, seen->second.set.directional_vector[0].y,
                                 seen->second.set.directional_vector[0].z);
                bool same = l == seen->second.lit && a == seen->second.ambient &&
                            values.directional_count == seen->second.set.directional_count;
                const auto same_light = [](const render::Float3& x, const render::Float3& y) {
                    return std::fabs(x.x - y.x) < 1e-3f && std::fabs(x.y - y.y) < 1e-3f && std::fabs(x.z - y.z) < 1e-3f;
                };
                same = same && same_light(values.ambient, seen->second.set.ambient);
                for (std::uint32_t k = 0; same && k < values.directional_count; ++k)
                    same = same_light(values.directional_colour[k], seen->second.set.directional_colour[k]) &&
                           same_light(values.directional_vector[k], seen->second.set.directional_vector[k]);
                if (!same && ++light_mismatches_ <= 16u)
                    std::fprintf(stderr,
                                 "[far-lights] geometry 0x%08X layer %d group %d: proxy lit %d ambient %d (%.3f %.3f %.3f) "
                                 "dirs %u, game lit %d ambient %d (%.3f %.3f %.3f) dirs %u\n",
                                 geometry, proxy.layer, proxy.light_group, l ? 1 : 0, a ? 1 : 0, values.ambient.x,
                                 values.ambient.y, values.ambient.z, values.directional_count,
                                 seen->second.lit ? 1 : 0, seen->second.ambient ? 1 : 0, seen->second.set.ambient.x,
                                 seen->second.set.ambient.y, seen->second.set.ambient.z,
                                 seen->second.set.directional_count);
            }
        }
        if (set == nullptr) {
            made.lights = layer_lights(c, proxy.layer, proxy.light_group, &made.lit, &made.ambient);
            if (layer_set_count < layer_sets.size())
                layer_sets[layer_set_count++] = made;
            set = &made;
        }
        draw_proxy_atomic(c, proxy.atomic, proxy.world, set->lights, set->lit, set->ambient, state.recipe,
                          state.cutoff, proxy.bones != 0u ? far_.palette(proxy) : nullptr, proxy.bones, proxy.uva);
    }
    if (far_.validating() && stats_.frames % 600u == 0u) {
        std::fprintf(stderr, "[far-lights] frame %llu: %llu enemy stand-in draws compared with the game's lights, %llu differ\n",
                     static_cast<unsigned long long>(stats_.frames), static_cast<unsigned long long>(skin_light_checks_),
                     static_cast<unsigned long long>(skin_light_mismatches_));
        std::fprintf(stderr, "[far-states] frame %llu: %llu game draws in layer worlds compared with layer_recipe, %llu differ\n",
                     static_cast<unsigned long long>(stats_.frames), static_cast<unsigned long long>(layer_state_checks_),
                     static_cast<unsigned long long>(layer_state_mismatches_));
        std::fprintf(stderr, "[far-lights] frame %llu: %llu proxy parts compared with the game's lights (%llu lit), %llu differ\n",
                     static_cast<unsigned long long>(stats_.frames), static_cast<unsigned long long>(light_checks_),
                     static_cast<unsigned long long>(light_checks_lit_),
                     static_cast<unsigned long long>(light_mismatches_));
        std::fprintf(stderr,
                     "[far-uv] frame %llu: %llu shared-clock evaluations (%.0f ns each); %llu evaluations at the game's "
                     "time compared with the game's matrix, %llu differ; %llu registered steps, the offset to the game's "
                     "time broke %llu times; %llu paused frames; %llu repeat draws of an animation in one frame, %llu at "
                     "another time; %zu clocks, %zu geometries; %llu stage archives; manager time %.3f s (%llu steps)\n",
                     static_cast<unsigned long long>(stats_.frames), static_cast<unsigned long long>(uv_evaluations_),
                     uv_evaluations_ != 0u ? static_cast<double>(uv_eval_ns_) / static_cast<double>(uv_evaluations_) : 0.0,
                     static_cast<unsigned long long>(uv_checks_), static_cast<unsigned long long>(uv_mismatches_),
                     static_cast<unsigned long long>(uv_registered_steps_),
                     static_cast<unsigned long long>(uv_offset_breaks_), static_cast<unsigned long long>(uv_paused_frames_),
                     static_cast<unsigned long long>(uv_both_frames_), static_cast<unsigned long long>(uv_split_frames_),
                     uv_clocks_.size(), uv_shared_geometries_.size(),
                     static_cast<unsigned long long>(uv_stage_resets_),
                     gekko::uv_anim_manager_time.seconds.load(std::memory_order_relaxed),
                     static_cast<unsigned long long>(gekko::uv_anim_manager_time.steps.load(std::memory_order_relaxed)));
    }
}

// RpUVAnim on a far proxy's material (docs/DISTANT_RENDERING.md 6b). The plugin (material + [0x805F2544], 0x28 bytes):
// +0 / +4 the uv[0] / uv[1] matrices, +8.. eight RtAnimInterpolators. An interpolator: +0 its animation, +4 the
// current time, +0x2C its node count. The animation: +0 interpolator info (+0 type 0x1C0 linear / 0x1C1 parameterised,
// +8 key size 0x20), +4 key count, +0xC duration, +0x10 keys {prev, time, six floats}, +0x14 custom data (+0x20 each
// node's channel, 0 / 1). Interpolation (0x804543A0 linear, 0x80454704 parameterised): each float lerped between the
// node's keys around the time, theta's difference wrapped into (-pi, pi]. The matrix (ApplyUpdate 0x8045567C): linear -
// right (k0, k1), up (k2, k3), pos (k4, k5); parameterised - right (s0, skew), up (0, s1), pos (x, y), then translated
// by (-0.5, -0.5), turned about z by theta (degrees), translated by (0.5, 0.5); each node's matrix pre-multiplies its
// channel's (from identity).
namespace {
struct Uv2D {  // a RenderWare matrix's 2D part, row vectors: right, up, pos
    float r[2] = {1, 0}, u[2] = {0, 1}, p[2] = {0, 0};
};
Uv2D uv_then(const Uv2D& a, const Uv2D& b) {  // a then b (RwMatrixTransform(a, b, postconcat))
    Uv2D o;
    const auto dir = [&](const float* v, float* out) {
        out[0] = v[0] * b.r[0] + v[1] * b.u[0];
        out[1] = v[0] * b.r[1] + v[1] * b.u[1];
    };
    dir(a.r, o.r);
    dir(a.u, o.u);
    dir(a.p, o.p);
    o.p[0] += b.p[0];
    o.p[1] += b.p[1];
    return o;
}
}  // namespace

bool RwDriver::uv_animation(Cpu& c, u32 material, u32 geometry, UvTime when, float (&base)[2][3], float (&dual)[2][3],
                            bool* has_dual) {
    constexpr u32 kUvAnimOffset = 0x805F2544u, kInterpolators = 8u;
    const u32 plugin = material + c.mem_read32(kUvAnimOffset);
    Uv2D channel[2];
    bool any = false;
    *has_dual = false;
    const u64 now = c.game_clock().now();
    for (u32 k = 0; k < kInterpolators; ++k) {
        const u32 interp = c.mem_read32(plugin + 8u + 4u * k);
        if (interp == 0u)
            continue;
        const u32 anim = c.mem_read32(interp);
        const u32 info = anim != 0u ? c.mem_read32(anim) : 0u;
        const u32 type = info != 0u ? c.mem_read32(info) : 0u;
        if (type != 0x1C0u && type != 0x1C1u)
            return false;  // another interpolator kind: leave the game's matrix
        const u32 key_size = c.mem_read32(info + 8u);
        const s32 key_count = static_cast<s32>(c.mem_read32(anim + 4u));
        const float duration = read_f32(c, anim + 0xCu);
        const u32 keys = c.mem_read32(anim + 0x10u);
        const u32 nodes = c.mem_read32(interp + 0x2Cu);
        const u32 custom = c.mem_read32(anim + 0x14u);
        if (key_size != 0x20u || key_count <= 0 || key_count > 4096 || nodes == 0u || nodes > 64u || keys == 0u ||
            custom == 0u || !(duration > 0.0f))
            return false;
        const float game_time = read_f32(c, interp + 4u);
        float t = game_time;
        if (when == UvTime::Shared) {
            // The animation's shared clock: created at the interpolator's time the first time it is drawn, stepped
            // once a frame by advance_uv_clocks.
            UvClock& clock = uv_clocks_[(u64{anim} << 32) | interp];
            if (clock.last_ticks == 0u) {
                clock.interpolator = interp;
                clock.geometry = geometry;
                clock.duration = duration;
                clock.time = game_time;
                clock.last_game = game_time;
                clock.last_ticks = now;
                clock.last_manager = gekko::uv_anim_manager_time.seconds.load(std::memory_order_relaxed);
            }
            t = static_cast<float>(clock.time);
            if (far_.validating()) {
                // Within one frame every draw of the animation - proxies' and the game's - gets the same time.
                if (uv_frame_ != stats_.frames) {
                    uv_frame_ = stats_.frames;
                    uv_frame_time_.clear();
                }
                const auto [seen, inserted] = uv_frame_time_.emplace(interp, t);
                if (!inserted) {
                    ++uv_both_frames_;
                    if (seen->second != t)
                        ++uv_split_frames_;
                }
            }
        }
        // Each key's node: the first `nodes` keys are nodes 0.., any later key follows its previous one.
        std::vector<s32> node_of(static_cast<std::size_t>(key_count), -1);
        for (s32 i = 0; i < key_count; ++i) {
            if (static_cast<u32>(i) < nodes) {
                node_of[static_cast<std::size_t>(i)] = i;
                continue;
            }
            const u32 prev = c.mem_read32(keys + 0x20u * static_cast<u32>(i));
            const s32 j = prev >= keys ? static_cast<s32>((prev - keys) / 0x20u) : -1;
            node_of[static_cast<std::size_t>(i)] = j >= 0 && j < i ? node_of[static_cast<std::size_t>(j)] : -1;
        }
        for (u32 n = 0; n < nodes; ++n) {
            // The node's keys around t (keys are in time order per node).
            s32 k1 = -1, k2 = -1;
            for (s32 i = 0; i < key_count; ++i) {
                if (node_of[static_cast<std::size_t>(i)] != static_cast<s32>(n))
                    continue;
                const float time = read_f32(c, keys + 0x20u * static_cast<u32>(i) + 4u);
                if (time <= t || k1 < 0) {
                    k1 = i;
                    k2 = -1;
                } else if (k2 < 0) {
                    k2 = i;
                }
            }
            if (k1 < 0)
                continue;
            if (k2 < 0)
                k2 = k1;
            const u32 a = keys + 0x20u * static_cast<u32>(k1), b = keys + 0x20u * static_cast<u32>(k2);
            const float ta = read_f32(c, a + 4u), tb = read_f32(c, b + 4u);
            const float f = tb > ta ? (t - ta) / (tb - ta) : 0.0f;
            float v[6];
            for (u32 i = 0; i < 6u; ++i) {
                const float x = read_f32(c, a + 8u + 4u * i), y = read_f32(c, b + 8u + 4u * i);
                float d = y - x;
                if (type == 0x1C1u && i == 0u) {  // theta: the shorter way round (r2+0x19E0 / 0x19E4 / 0x19E8)
                    if (d < read_f32(c, 0x805FC164u))
                        d += read_f32(c, 0x805FC168u);
                    else if (d > read_f32(c, 0x805FC160u))
                        d -= read_f32(c, 0x805FC168u);
                }
                v[i] = f * d + x;
            }
            Uv2D m;
            if (type == 0x1C0u) {
                m.r[0] = v[0], m.r[1] = v[1], m.u[0] = v[2], m.u[1] = v[3], m.p[0] = v[4], m.p[1] = v[5];
            } else {
                m.r[0] = v[1], m.r[1] = v[3], m.u[0] = 0.0f, m.u[1] = v[2], m.p[0] = v[4], m.p[1] = v[5];
                Uv2D to, rot, back;
                to.p[0] = to.p[1] = -0.5f;
                back.p[0] = back.p[1] = 0.5f;
                const float radians = v[0];  // the game passes 180 theta / pi degrees to RwMatrixRotate
                rot.r[0] = std::cos(radians), rot.r[1] = std::sin(radians);
                rot.u[0] = -std::sin(radians), rot.u[1] = std::cos(radians);
                m = uv_then(uv_then(uv_then(m, to), rot), back);
            }
            const u32 ch = c.mem_read32(custom + 0x20u + 4u * n);
            if (ch < 2u) {
                channel[ch] = uv_then(m, channel[ch]);  // RwMatrixTransform(uv, m, preconcat): m, then uv
                *has_dual = *has_dual || ch == 1u;
            }
        }
        any = true;
    }
    if (!any)
        return false;
    const auto put = [](const Uv2D& m, float (&out)[2][3]) {
        out[0][0] = m.r[0], out[0][1] = m.u[0], out[0][2] = m.p[0];
        out[1][0] = m.r[1], out[1][1] = m.u[1], out[1][2] = m.p[1];
    };
    put(channel[0], base);
    put(channel[1], dual);
    return true;
}

// TextureUVAnimManager (0x80580B08, a std::map<name, {clump +0x18, count +0x1C}>; begin [+0xC], end +4, nodes {left,
// right, parent | colour}): the library clumps whose UV animations the game advances while an instance exists. A
// geometry of such a clump is shared for the rest of the stage. Positive answers are kept (the map changes as objects
// come and go); a miss walks the map again, so a spawn that registers mid-frame is seen at its first draw.
void RwDriver::uv_registered_now(Cpu& c, std::set<u32>* geometries) {
    constexpr u32 kManager = 0x80580B08u;
    const u32 end = kManager + 4u;
    u32 node = c.mem_read32(kManager + 0xCu);
    for (int guard = 0; node != 0u && node != end && guard < 1024; ++guard) {
        const u32 clump = c.mem_read32(node + 0x18u);
        const u32 head = clump + 0x08u;
        u32 link = clump != 0u ? c.mem_read32(head) : 0u;
        for (int a = 0; link != 0u && link != head && a < 64; ++a) {
            geometries->insert(c.mem_read32(link - 0x40u + layout::kAtomicGeometry));
            link = c.mem_read32(link);
        }
        // The in-order successor (0x80012E90): the right subtree's leftmost, else up until coming from a left child.
        if (const u32 right = c.mem_read32(node + 4u)) {
            node = right;
            while (const u32 left = c.mem_read32(node))
                node = left;
        } else {
            u32 parent = c.mem_read32(node + 8u) & ~1u;
            while (parent != 0u && c.mem_read32(parent) != node) {
                node = parent;
                parent = c.mem_read32(node + 8u) & ~1u;
            }
            node = parent;
        }
    }
}

void RwDriver::uv_stage_check(Cpu& c) {
    // The stage's archive (stgNNNN_dat.one, the one ONE the game keeps open, at 0x805F03C0 until the StageManager's
    // teardown): a new one - or none - is another stage's libraries.
    const u32 archive = c.mem_read32(0x805F03C0u);
    if (archive == uv_stage_archive_)
        return;
    if (far_.validating())
        std::fprintf(stderr, "[far-uv] frame %llu: stage archive 0x%08X -> 0x%08X, %zu clocks dropped\n",
                     static_cast<unsigned long long>(stats_.frames), uv_stage_archive_, archive, uv_clocks_.size());
    uv_stage_archive_ = archive;
    uv_shared_geometries_.clear();
    uv_clocks_.clear();
    ++uv_stage_resets_;
}

bool RwDriver::uv_shared_geometry(Cpu& c, u32 geometry) {
    if (uv_epoch_ != far_.stage_epoch()) {
        uv_epoch_ = far_.stage_epoch();
        uv_shared_geometries_.clear();
        uv_clocks_.clear();
    }
    if (uv_shared_geometries_.count(geometry) != 0u)
        return true;
    uv_registered_now(c, &uv_shared_geometries_);
    return uv_shared_geometries_.count(geometry) != 0u;
}

void RwDriver::advance_uv_clocks(Cpu& c) {
    if (uv_advanced_frame_ == stats_.frames)
        return;
    uv_advanced_frame_ = stats_.frames;
    uv_registered_.clear();
    uv_registered_now(c, &uv_registered_);
    uv_shared_geometries_.insert(uv_registered_.begin(), uv_registered_.end());
    const u64 now = c.game_clock().now();
    // The game's own UV animation time: what TextureUVAnimManagerTask was given (gekko/uv_anim_time.h).
    const bool manager_fed = gekko::uv_anim_manager_time.steps.load(std::memory_order_relaxed) != 0u;
    const double manager = gekko::uv_anim_manager_time.seconds.load(std::memory_order_relaxed);
    // Did the manager step this frame? Any registered animation whose time moved says so; registered ones that all
    // stood mean the game paused them (unregistered clocks stand too then).
    bool any_registered = false, any_moved = false;
    for (const auto& [key, clock] : uv_clocks_) {
        if (uv_registered_.count(clock.geometry) == 0u)
            continue;
        any_registered = true;
        any_moved = any_moved || read_f32(c, clock.interpolator + 4u) != clock.last_game;
    }
    const bool paused = manager_fed ? manager == uv_manager_seen_ : any_registered && !any_moved;
    uv_manager_seen_ = manager;
    if (paused)
        ++uv_paused_frames_;
    for (auto& [key, clock] : uv_clocks_) {
        const float game_time = read_f32(c, clock.interpolator + 4u);
        double step = 0.0;
        if (uv_registered_.count(clock.geometry) != 0u) {
            // The game's own step (RtAnimInterpolatorAddAnimTime 0x80478F24 loops by the duration): its speed, and a
            // time the game sets itself (a re-created object's animation starting over) carried over too. Only the
            // phase is drawn, so whole periods a long gap wrapped away change nothing.
            if (game_time != clock.last_game) {
                step = static_cast<double>(game_time) - static_cast<double>(clock.last_game);
                if (step < 0.0)
                    step += static_cast<double>(clock.duration);
            }
        } else if (manager_fed) {
            // Not registered (only proxies show it): the time TextureUVAnimManagerTask was given since this clock's
            // last step - the game's speed, standing whenever the game stops that task, however many frames went
            // undrawn.
            step = manager - clock.last_manager;
        } else if (!paused) {
            step = static_cast<double>(now - clock.last_ticks) / static_cast<double>(gekko::kGameTicksPerSecond);
        }
        clock.time = std::fmod(clock.time + step, static_cast<double>(clock.duration));
        if (far_.validating()) {
            // While registered the shared time keeps one offset from the game's: same speed, no restart.
            if (uv_registered_.count(clock.geometry) != 0u) {
                ++uv_registered_steps_;
                double offset = std::fmod(clock.time - static_cast<double>(game_time), static_cast<double>(clock.duration));
                if (offset < 0.0)
                    offset += static_cast<double>(clock.duration);
                if (clock.offset_known) {
                    double d = std::fabs(offset - clock.offset);
                    d = std::min(d, static_cast<double>(clock.duration) - d);
                    if (d > 1e-4)
                        ++uv_offset_breaks_;
                }
                clock.offset = offset;
                clock.offset_known = true;
            } else {
                clock.offset_known = false;
            }
        }
        clock.last_game = game_time;
        clock.last_ticks = now;
        clock.last_manager = manager;
    }
}

void RwDriver::shared_uv(Cpu& c, u32 material, u32 geometry, u32 fx_kind, render::Material* m, bool proxy) {
    float base[2][3], dual[2][3];
    bool has_dual = false;
    const auto started = far_.validating() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    advance_uv_clocks(c);
    if (!uv_animation(c, material, geometry, UvTime::Shared, base, dual, &has_dual))
        return;
    std::memcpy(m->uv_transform, base, sizeof base);
    if (fx_kind == kEffectDualUvTransform && has_dual)
        std::memcpy(m->extra.uv_transform, dual, sizeof dual);
    if (!far_.validating())
        return;
    ++uv_evaluations_;
    uv_eval_ns_ += static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count());
    // The evaluation itself: at the interpolator's own time it must give the game's own matrix (both change together
    // in the manager's step, or stand together), except before the first ApplyUpdate (no matrix in MatFX yet).
    const u32 plugin = material + c.mem_read32(0x805F2544u);
    const u32 game_base = c.mem_read32(plugin);
    const u32 fx = c.mem_read32(material + c.mem_read32(layout::kMaterialEffectsOffset));
    if (game_base == 0u || fx == 0u || c.mem_read32(fx) == 0u)
        return;
    float at_game[2][3], unused[2][3];
    bool unused_dual = false;
    if (!uv_animation(c, material, geometry, UvTime::Game, at_game, unused, &unused_dual))
        return;
    ++uv_checks_;
    const float g[2][3] = {{read_f32(c, game_base), read_f32(c, game_base + 0x10u), read_f32(c, game_base + 0x30u)},
                           {read_f32(c, game_base + 4u), read_f32(c, game_base + 0x14u), read_f32(c, game_base + 0x34u)}};
    float e = 0.0f;
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 3; ++j)
            e = std::max(e, std::fabs(g[i][j] - at_game[i][j]));
    if (e > 1e-4f && ++uv_mismatches_ <= 8u)
        std::fprintf(stderr,
                     "[far-uv] %s material 0x%08X: evaluation at the game's time differs by %g (pos %.4f %.4f, game "
                     "%.4f %.4f)\n",
                     proxy ? "proxy" : "game", material, static_cast<double>(e), static_cast<double>(at_game[0][2]),
                     static_cast<double>(at_game[1][2]), static_cast<double>(g[0][2]), static_cast<double>(g[1][2]));
}

// One library atomic at `world`: pipeline_atomic's mesh and material decisions, reading the game's data only (no lock
// or interpolator flags cleared, no light frame advanced, no texture binding written back).
void RwDriver::draw_proxy_atomic(Cpu& c, u32 atomic, const render::Float3x4& world, render::LightSetId lights,
                                 bool world_lit, bool world_ambient, const render::Recipe& recipe, float cutoff,
                                 const render::Float3x4* palette, u32 bone_count, bool uva) {
    const u32 geometry = c.mem_read32(atomic + layout::kAtomicGeometry);
    if (geometry == 0u)
        return;
    const u32 pipeline = c.mem_read32(atomic + layout::kAtomicPipeline);
    const bool skinned = pipeline != 0u && (pipeline == c.mem_read32(layout::kSkinPipeline) ||
                                            pipeline == c.mem_read32(layout::kSkinMatFxPipeline));
    if (pipeline != 0u && pipeline == c.mem_read32(layout::kPtankPipeline))
        return;  // particles are not models
    if (skinned != (bone_count != 0u && palette != nullptr))
        return;  // a skin is posed only by the palette its stand-in brought
    const u32 flags = c.mem_read32(geometry + layout::kGeometryFlags);
    const u32 mesh_header = c.mem_read32(geometry + layout::kGeometryMesh);
    const u32 mesh_count = mesh_header != 0u ? c.mem_read16(mesh_header + layout::kMeshHeaderCount) : 0u;
    if ((flags & kGeometryNative) != 0u || mesh_count == 0u || c.mem_read32(geometry + layout::kGeometryVertexCount) == 0u)
        return;
    const u32 skin = skinned ? c.mem_read32(geometry + c.mem_read32(layout::kSkinGeometryOffset)) : 0u;
    if (skinned && skin == 0u)
        return;
    u32 dmorph = 0u;
    const render::MeshHandle mesh = geometry_mesh(c, geometry, flags, mesh_header, mesh_count, false, skin, &dmorph);
    if (!mesh || dmorph != 0u)
        return;
    // A skinned stand-in's palette is in world space already: its mesh draws with no world of its own.
    const render::BoneRange bones = skinned ? builder_.bones(palette, bone_count) : render::BoneRange{};
    const bool effects = pipeline != 0u && (pipeline == c.mem_read32(layout::kMatFxAtomicPipeline) ||
                                            pipeline == c.mem_read32(layout::kSkinMatFxPipeline));
    const bool textured = (flags & (kGeometryTextured | kGeometryTextured2)) != 0u;
    const bool lit = world_lit && (flags & kGeometryLight) != 0u;
    const u32 effects_offset = c.mem_read32(layout::kMaterialEffectsOffset);
    for (u32 j = 0; j < mesh_count; ++j) {
        const u32 material = c.mem_read32(mesh_header + layout::kMeshFirst + j * layout::kMeshStride + layout::kMeshMaterial);
        const u32 rw_texture = textured && material != 0u ? c.mem_read32(material + layout::kMaterialTexture) : 0u;
        bool has_alpha = false;
        render::Material m;
        if (rw_texture != 0u) {
            m.texture = texture_of(c, rw_texture, &has_alpha);
            m.sampler = sampler_of(c.mem_read32(rw_texture + layout::kTextureFilterAddressing));
        }
        const u32 fx = effects && material != 0u ? c.mem_read32(material + effects_offset) : 0u;
        const u32 fx_kind = fx != 0u ? c.mem_read32(fx + layout::kMatFxFlags) : 0u;
        if (material != 0u) {
            const u32 rgba = c.mem_read32(material + layout::kMaterialColour);
            m.colour = render::Float4{static_cast<float>((rgba >> 24) & 0xFFu) / 255.0f,
                                      static_cast<float>((rgba >> 16) & 0xFFu) / 255.0f,
                                      static_cast<float>((rgba >> 8) & 0xFFu) / 255.0f,
                                      static_cast<float>(rgba & 0xFFu) / 255.0f};
            m.ka = read_f32(c, material + layout::kMaterialAmbient);
        }
        m.vertex_colours = (flags & kGeometryPrelit) != 0u;
        m.modulate = (flags & kGeometryModulate) != 0u;
        m.lit = lit;
        m.ambient = lit && world_ambient;
        m.combine = effects ? render::Combine::OneStage
                    : (!textured || (m.vertex_colours && m.ambient)) ? render::Combine::TwoStage
                                                                    : render::Combine::OneStage;
        render::Recipe r = recipe;
        r.alpha_cutoff = has_alpha && fx_kind != kEffectEnvironment && fx_kind != kEffectBumpEnvironment ? cutoff : 0.0f;
        matfx_layers(c, fx, fx_kind, flags, cutoff, &m);
        // A registered (shared) UV animation runs on the shared native clock, the same one the game's own draws get.
        if (uv_shared_enabled_ && far_.enabled() && material != 0u && (fx_kind == kEffectUvTransform || fx_kind == kEffectDualUvTransform) &&
            (uva || uv_shared_geometry(c, geometry))) {
            if (uva && uv_epoch_ == far_.stage_epoch())
                uv_shared_geometries_.insert(geometry);
            shared_uv(c, material, geometry, fx_kind, &m, true);
        }
        if (stats_.frames == trace_frame_)
            std::fprintf(stderr,
                         "[r2-trace] proxy atomic %08X pipe %08X geom %08X flags %08X mesh %u material %08X colour "
                         "%08X texture %08X (alpha %d) fx %u combine %u P%d M%d L%d X%d ka %.2f lights %u world_lit %d "
                         "world (%.1f %.1f %.1f)\n",
                         atomic, pipeline, geometry, flags, j, material,
                         material != 0u ? c.mem_read32(material + layout::kMaterialColour) : 0u, m.texture.value,
                         has_alpha ? 1 : 0, fx_kind, static_cast<unsigned>(m.combine), m.vertex_colours ? 1 : 0,
                         m.modulate ? 1 : 0, m.lit ? 1 : 0, m.ambient ? 1 : 0, m.ka, static_cast<unsigned>(lights),
                         world_lit ? 1 : 0, world.m[0][3], world.m[1][3], world.m[2][3]);
        builder_.draw(mesh, j, m, skinned ? render::Float3x4{} : world, r, lit ? lights : render::kNoLights, bones, {});
        ++stats_.far_proxy_draws;
    }
}

// A PTank execution (r2/SITES_PIPELINES.md 2.7 and 4.6, R2_DRIVER.md 4.5): the records the game's effect filled,
// copied as values, drawn as one batch along the axes the render callback (0x80449A74) handed its emitter through
// the extension's userData - the camera's right and its up turned over. The configuration the game creates only
// (position, size, rotation, two texture corners; per-particle or constant colour); any other is counted, not drawn.
void RwDriver::particle_batch(Cpu& c, u32 atomic, const render::Material& material, const render::Recipe& recipe) {
    const u32 ext = c.mem_read32(atomic + c.mem_read32(layout::kPtankExtOffset));
    const u32 count = ext != 0u ? c.mem_read32(ext + layout::kPtankActiveCount) : 0u;
    if (count == 0u)
        return;
    const u32 data = c.mem_read32(ext + layout::kPtankDataFlags);
    const u32 plat = c.mem_read32(ext + layout::kPtankPlatFlags);
    const u32 axes = c.mem_read32(ext + layout::kPtankAxes);
    const u32 required = layout::kPtankPosition | layout::kPtankSize | layout::kPtankRotation | layout::kPtankUv2;
    const u32 refused = layout::kPtankMatrix | layout::kPtankNormal | layout::kPtankVertexColours |
                        layout::kPtankUv4 | layout::kPtankUseCenter;
    const bool particle_colours = (data & layout::kPtankColour) != 0u;
    // The render's vertex count is 4 x (count & 0x3FFF): a larger count is not a batch the console drew.
    if ((data & required) != required || (data & refused) != 0u || axes == 0u || count > 0x3FFFu ||
        (!particle_colours && (plat & layout::kPtankPlatConstantColour) == 0u)) {
        ++stats_.particles_unsupported;
        ++stats_.particles_skipped;
        return;
    }
    struct Cluster {
        const u8* data = nullptr;
        u32 stride = 0;
    };
    const auto cluster = [&](u32 slot, u32 bytes) {
        Cluster out;
        const u32 address = c.mem_read32(ext + slot);
        out.stride = c.mem_read32(ext + slot + 4u);
        if (address != 0u)
            out.data = c.guest_memory().peek(GcAddr(address), std::size_t{out.stride} * (count - 1u) + bytes);
        return out;
    };
    const Cluster positions = cluster(layout::kPtankPositions, 12u);
    const Cluster sizes = cluster(layout::kPtankSizes, 8u);
    const Cluster rotations = cluster(layout::kPtankRotations, 4u);
    const Cluster uvs = cluster(layout::kPtankUvs, 16u);
    const Cluster colours = particle_colours ? cluster(layout::kPtankColours, 4u) : Cluster{};
    if (positions.data == nullptr || sizes.data == nullptr || rotations.data == nullptr || uvs.data == nullptr ||
        (particle_colours && colours.data == nullptr)) {
        ++stats_.particles_unsupported;
        ++stats_.particles_skipped;
        return;
    }
    particle_records_.resize(count);
    for (u32 i = 0; i < count; ++i) {
        render::Particle& p = particle_records_[i];
        const u8* position = positions.data + std::size_t{positions.stride} * i;
        p.position = render::Float3{be_float(position), be_float(position + 4), be_float(position + 8)};
        const u8* size = sizes.data + std::size_t{sizes.stride} * i;
        p.size[0] = be_float(size);
        p.size[1] = be_float(size + 4);
        p.rotation = be_float(rotations.data + std::size_t{rotations.stride} * i);
        const u8* uv = uvs.data + std::size_t{uvs.stride} * i;
        p.uv0[0] = be_float(uv);
        p.uv0[1] = be_float(uv + 4);
        p.uv1[0] = be_float(uv + 8);
        p.uv1[1] = be_float(uv + 12);
        if (particle_colours) {
            const u8* rgba = colours.data + std::size_t{colours.stride} * i;  // RwRGBA: r, g, b, a
            p.colour = static_cast<std::uint32_t>(rgba[0]) | (static_cast<std::uint32_t>(rgba[1]) << 8) |
                       (static_cast<std::uint32_t>(rgba[2]) << 16) | (static_cast<std::uint32_t>(rgba[3]) << 24);
        }
    }
    render::ParticleBatch batch;
    batch.right = render::Float3{read_f32(c, axes), read_f32(c, axes + 4u), read_f32(c, axes + 8u)};
    batch.up = render::Float3{read_f32(c, axes + 12u), read_f32(c, axes + 16u), read_f32(c, axes + 20u)};
    batch.particle_colours = particle_colours;
    if (!particle_colours) {
        const u32 rgba = c.mem_read32(ext + layout::kPtankConstantColour);
        batch.colour = render::Float4{static_cast<float>(rgba >> 24) / 255.0f,
                                      static_cast<float>((rgba >> 16) & 0xFFu) / 255.0f,
                                      static_cast<float>((rgba >> 8) & 0xFFu) / 255.0f,
                                      static_cast<float>(rgba & 0xFFu) / 255.0f};
    }
    batch.texture = material.texture;
    batch.sampler = material.sampler;
    if (stats_.frames == trace_frame_) {
        const render::Particle& p = particle_records_[0];
        std::fprintf(stderr,
                     "[r2-trace]   particles %u (data %X plat %X) first at (%.1f %.1f %.1f) size %.2f x %.2f colour %08X "
                     "rot %.2f uv (%.2f %.2f)-(%.2f %.2f); axes R (%.2f %.2f %.2f) U (%.2f %.2f %.2f); texture %08X; "
                     "blend %u/%u z %d/%d cull %u fog %d (%.1f..%.1f) cutoff %.3f; pass camera at (%.1f %.1f %.1f) "
                     "near %.2f far %.1f\n",
                     count, data, plat, p.position.x, p.position.y, p.position.z, p.size[0], p.size[1], p.colour,
                     p.rotation, p.uv0[0], p.uv0[1], p.uv1[0], p.uv1[1], batch.right.x, batch.right.y, batch.right.z,
                     batch.up.x, batch.up.y, batch.up.z, batch.texture.value, static_cast<unsigned>(recipe.src),
                     static_cast<unsigned>(recipe.dst), recipe.depth_test ? 1 : 0, recipe.depth_write ? 1 : 0,
                     static_cast<unsigned>(recipe.cull), recipe.fog ? 1 : 0, recipe.fog_start, recipe.fog_end,
                     recipe.alpha_cutoff, pass_camera_.frame.m[0][3], pass_camera_.frame.m[1][3],
                     pass_camera_.frame.m[2][3], pass_camera_.near_plane, pass_camera_.far_plane);
    }
    builder_.particles(batch, particle_records_.data(), count, recipe);
    ++stats_.particle_batches;
    stats_.particles += count;
}

// The sector's native mesh: built from the prepared data the translated reader put in its RG1 block, at the first
// draw; the handle replaces the block header's flags word (read only by the lowered render callback).
render::MeshHandle RwDriver::sector_mesh(Cpu& c, u32 sector) {
    const u32 entry = c.mem_read32(sector + layout::kSectorRepEntry);
    if (entry == 0u)
        return {};
    const u32 header = entry + layout::kResEntryHeader;
    const u32 word = header + layout::kPreparedFlags;
    const u32 value = c.mem_read32(word);
    if ((value & 0xFF000000u) != 0u) {
        for (const Owned& owned : meshes_) {
            if (owned.handle == value && owned.word == word)
                return render::MeshHandle{value};
        }
    }
    const u32 mesh_header = c.mem_read32(sector + layout::kSectorMesh);
    const u32 mesh_count = mesh_header != 0u ? c.mem_read16(mesh_header + layout::kMeshHeaderCount) : 0u;
    const u32 attribute_count = c.mem_read32(header + layout::kPreparedAttributeCount);
    if (attribute_count == 0u || attribute_count > 4u || mesh_count == 0u) {
        ++stats_.sector_import_failures;
        return {};
    }
    // The reader relocated every offset by the data block's guest address: the block starts 32-byte aligned after
    // the header, and the resource entry kept the struct chunk's size (12 + header + data).
    const u32 header_size = 12u + 8u * attribute_count + 8u * mesh_count;
    const u32 data_base = (header + header_size + 31u) & ~31u;
    const u32 chunk_size = c.mem_read32(entry + layout::kResEntrySize);
    if (chunk_size < 12u + header_size) {
        ++stats_.sector_import_failures;
        return {};
    }
    const u32 data_size = chunk_size - 12u - header_size;
    assets::PreparedArrays arrays;
    for (u32 i = 0; i < attribute_count; ++i) {
        const u32 a = header + layout::kPreparedAttributes + 8u * i;
        assets::PreparedArrays::Attribute attribute;
        attribute.offset = c.mem_read32(a) - data_base;
        attribute.kind = c.mem_read8(a + 4u);
        attribute.stride = c.mem_read8(a + 5u);
        const u8 index_type = c.mem_read8(a + 6u);
        attribute.index_bytes = index_type == 2u ? 1u : index_type == 3u ? 2u : 0u;
        arrays.attributes.push_back(attribute);
    }
    for (u32 j = 0; j < mesh_count; ++j) {
        const u32 l = header + layout::kPreparedAttributes + 8u * attribute_count + 8u * j;
        arrays.lists.push_back({c.mem_read32(l) - data_base, c.mem_read32(l + 4u)});
    }
    const u8* data = c.guest_memory().peek(GcAddr(data_base), data_size);
    if (data == nullptr) {
        ++stats_.sector_import_failures;
        return {};
    }
    arrays.data = assets::ByteView{data, data_size};
    std::vector<std::uint32_t> materials(mesh_count);
    for (u32 j = 0; j < mesh_count; ++j)
        materials[j] = j;
    assets::WorldSector imported;
    std::string error;
    if (!assets::import_prepared_mesh(arrays, materials, &imported, &error) || imported.mesh.vertex_count == 0u ||
        imported.mesh.indices.empty()) {
        ++stats_.sector_import_failures;
        return {};
    }
    // The corridor lights onto their painted slots (assets/light_slots.h, a deliberate deviation): the submeshes whose
    // texture is a frame of the blinking-light pattern (*_zn055.N - the pattern swaps the frames, so any of them).
    {
        std::vector<bool> glow(imported.mesh.submeshes.size(), false);
        bool any = false;
        for (std::size_t s = 0; s < imported.mesh.submeshes.size(); ++s) {
            const u32 j = imported.mesh.submeshes[s].material;
            const u32 material =
                j < mesh_count ? c.mem_read32(mesh_header + layout::kMeshFirst + j * layout::kMeshStride + layout::kMeshMaterial)
                               : 0u;
            const u32 texture = material != 0u ? c.mem_read32(material + layout::kMaterialTexture) : 0u;
            if (texture == 0u)
                continue;
            char name[33]{};
            for (u32 i = 0; i < 32u; ++i) {
                name[i] = static_cast<char>(c.mem_read8(texture + 0x10u + i));
                if (name[i] == '\0')
                    break;
            }
            glow[s] = std::strstr(name, "_zn055.") != nullptr;
            any = any || glow[s];
        }
        if (any) {
            const assets::LightSlotResult fixed = assets::fix_light_slots(imported.mesh, glow);
            stats_.light_slot_vertices_moved += fixed.moved_vertices;
            stats_.light_slots_added += fixed.added_lights;
        }
    }
    // Diagnostics: SHADOW_R2_DUMP_SECTORS=<dir> writes each sector's imported mesh as <dir>/<sector>.obj (one group
    // per material mesh), to measure how the level's surfaces lie on each other.
    if (const char* dump = std::getenv("SHADOW_R2_DUMP_SECTORS"); dump != nullptr && dump[0] != '\0') {
        const std::string path = std::string(dump) + "/" + std::to_string(sector) + ".obj";
        if (std::FILE* f = std::fopen(path.c_str(), "w")) {
            const std::vector<float>& p = imported.mesh.positions;
            for (std::size_t i = 0; i + 2 < p.size(); i += 3)
                std::fprintf(f, "v %.9g %.9g %.9g\n", p[i], p[i + 1], p[i + 2]);
            const std::vector<float>& uv = imported.mesh.uv0;
            for (std::size_t i = 0; i + 1 < uv.size(); i += 2)
                std::fprintf(f, "vt %.9g %.9g\n", uv[i], uv[i + 1]);
            for (const auto& s : imported.mesh.submeshes) {
                std::fprintf(f, "g mesh%u\n", s.material);
                for (std::uint32_t k = s.first_index; k + 2 < s.first_index + s.index_count; k += 3)
                    std::fprintf(f, "f %u %u %u\n", imported.mesh.indices[k] + 1, imported.mesh.indices[k + 1] + 1,
                                 imported.mesh.indices[k + 2] + 1);
            }
            std::fclose(f);
        }
    }
    const render::MeshHandle handle =
        renderer_.request_mesh(render::Scope::Session, std::move(imported.mesh), &error);
    if (!handle) {
        ++stats_.sector_import_failures;
        return {};
    }
    c.mem_write32(word, handle.value);
    meshes_.push_back(Owned{handle.value, word});
    ++stats_.sector_meshes_built;
    return handle;
}

// The default and MatFX sector pipelines (0x80464FF4 with 0x80463964 / 0x804A62D0): sectors are unlit (the level
// decoder clears the world's light flags) and prelit; each mesh draws its material with the level's recipe
// (r2/SITES_PIPELINES.md 2.3, 3.5; R2_MATERIAL_CENSUS.md 4.4). The MatFX sector pipeline ([0x805F2768]) shares the
// atomics' render callback 0x804A62D0, so its meshes take the same MatFX layers (matfx_layers; renderer-re
// 04_GC_DRIVER.md 3.18 - 421 sector meshes with a dual second pass on the disc).
u32 RwDriver::pipeline_sector(Cpu& c, u32 pipeline, u32 sector) {
    ++stats_.sectors;
    tag_draws(c, render::EffectLayer::None, true);  // a land class's sector (layers.cpp), else the world
    if (c.mem_read16(sector + layout::kSectorVertexCount) == 0u)
        return pipeline;
    const render::MeshHandle mesh = sector_mesh(c, sector);
    if (!mesh) {
        ++stats_.sectors_skipped;
        return pipeline;
    }
    const u32 globals = c.mem_read32(layout::kRwGlobals);
    const u32 world = c.mem_read32(globals + layout::kGlobalsCurrentWorld);
    const u32 world_flags = world != 0u ? c.mem_read32(world + layout::kGeometryFlags) : 0u;
    const u32 mesh_header = c.mem_read32(sector + layout::kSectorMesh);
    const u32 mesh_count = c.mem_read16(mesh_header + layout::kMeshHeaderCount);
    const bool textured = (world_flags & (kGeometryTextured | kGeometryTextured2)) != 0u;
    const u32 im = im_texture(c);
    const u32 texture_ext_offset = c.mem_read32(layout::kTextureExtOffset);
    const u32 white = c.mem_read32(layout::kWhiteRaster);
    const render::Recipe level = recipe();
    const float cutoff = alpha_cutoff();
    const bool effects = pipeline == c.mem_read32(layout::kMatFxSectorPipeline);
    const u32 effects_offset = c.mem_read32(layout::kMaterialEffectsOffset);
    for (u32 j = 0; j < mesh_count; ++j) {
        const u32 material = c.mem_read32(mesh_header + layout::kMeshFirst + j * layout::kMeshStride + layout::kMeshMaterial);
        const u32 rw_texture = textured && material != 0u ? c.mem_read32(material + layout::kMaterialTexture) : 0u;
        const u32 fx = effects && material != 0u ? c.mem_read32(material + effects_offset) : 0u;
        const u32 fx_kind = fx != 0u ? c.mem_read32(fx + layout::kMatFxFlags) : 0u;
        bool has_alpha = false;
        render::Material m;
        m.combine = render::Combine::OneStage;
        m.vertex_colours = (world_flags & kGeometryPrelit) != 0u;
        m.modulate = (world_flags & kGeometryModulate) != 0u;
        if (material != 0u) {
            const u32 rgba = c.mem_read32(material + layout::kMaterialColour);
            m.colour = render::Float4{static_cast<float>((rgba >> 24) & 0xFFu) / 255.0f,
                                      static_cast<float>((rgba >> 16) & 0xFFu) / 255.0f,
                                      static_cast<float>((rgba >> 8) & 0xFFu) / 255.0f,
                                      static_cast<float>(rgba & 0xFFu) / 255.0f};
            m.ka = read_f32(c, material + layout::kMaterialAmbient);
        }
        if (rw_texture != 0u) {
            m.texture = texture_of(c, rw_texture, &has_alpha);
            if (!m.texture)
                ++stats_.draws_without_texture;
            m.sampler = sampler_of(c.mem_read32(rw_texture + layout::kTextureFilterAddressing));
        }
        if (textured)
            bind_side_effects(c, im, texture_ext_offset, white, rw_texture, has_alpha, &states_.alpha_test_off);
        else
            states_.alpha_test_off = 1u;
        if (fx_kind == kEffectEnvironment || fx_kind == kEffectBumpEnvironment)
            states_.alpha_test_off = 1u;  // the environment / bump base pass forces the test off (0x804987C8)
        if (!ensure_pass()) {
            ++stats_.draws_without_view;
            continue;
        }
        render::Recipe r = level;
        r.alpha_cutoff = has_alpha && fx_kind != kEffectEnvironment && fx_kind != kEffectBumpEnvironment ? cutoff : 0.0f;
        if (fx != 0u) {
            matfx_layers(c, fx, fx_kind, world_flags, cutoff, &m);
            if (m.extra.kind != render::ExtraPass::Kind::None)
                ++stats_.extra_passes;
        }
        if (stats_.frames == trace_frame_) {
            std::fprintf(stderr,
                         "[r2-trace] sector %08X [%s] mesh %u texture %08X colour %.2f %.2f %.2f %.2f blend %u/%u "
                         "z %d/%d cull %u cutoff %.3f\n",
                         sector, glare_window_ ? "glare source" : "scene", j, m.texture.value, m.colour.x, m.colour.y,
                         m.colour.z, m.colour.w,
                         static_cast<unsigned>(r.src), static_cast<unsigned>(r.dst), r.depth_test ? 1 : 0,
                         r.depth_write ? 1 : 0, static_cast<unsigned>(r.cull), r.alpha_cutoff);
        }
        builder_.draw(mesh, j, m, render::Float3x4{}, r);
        ++stats_.sector_draws;
    }
    return pipeline;
}

}  // namespace gekko::scaffold
