#pragma once

// A frame as the game describes it to the renderer (docs/lift-20261006/R2_DRIVER.md 2.2, R_DESIGN.md 2.1-2.3).
//
// The game side builds one immutable FramePacket per game tick with a FrameBuilder: views (a camera and the
// rectangle of the output it fills - one per player in split screen), light sets, passes on those views in the
// game's authored order, and draw records inside the passes. Everything is a value: meshes and textures are named
// by handles the renderer issued, materials and recipes are plain data, matrices are floats. The renderer turns the
// packet into a frame-graph frame on its own; nothing in the packet refers to GPU objects.

#include <cstdint>
#include <mutex>
#include <vector>

#include "render/effect_layers.h"
#include "render/sampler_table.h"
#include "render/view.h"

namespace shadow::render {

// ---- values ----------------------------------------------------------------------------------------------------

struct Float3 {
    float x = 0, y = 0, z = 0;
};
struct Float4 {
    float x = 0, y = 0, z = 0, w = 0;
};
// Rows of a 3x4 matrix in the column-vector convention: p' = M * [p, 1].
struct Float3x4 {
    float m[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
};

// Generational handles: 24-bit slot, 8-bit generation; 0 is "none". 32 bits so the scaffolding can keep one in the
// fields where the console driver kept its own data (R2_DRIVER.md 2.4).
struct TextureHandle {
    std::uint32_t value = 0;
    explicit operator bool() const noexcept { return value != 0; }
};
struct MeshHandle {
    std::uint32_t value = 0;
    explicit operator bool() const noexcept { return value != 0; }
};

enum class Scope : std::uint8_t { Session, Stage, Event, Frame };

// The game's blend factors (its own enumeration's order).
enum class Blend : std::uint8_t {
    Zero, One, SrcColor, InvSrcColor, SrcAlpha, InvSrcAlpha, DestAlpha, InvDestAlpha, DestColor, InvDestColor,
};
enum class Cull : std::uint8_t { None, Back, Front };

// A level's arms and modifiers plus per-draw overrides (RENDERING_ORIGINAL.md 2.2): how a draw composes.
struct Recipe {
    Blend src = Blend::SrcAlpha;
    Blend dst = Blend::InvSrcAlpha;
    bool depth_test = true;
    bool depth_write = true;
    Cull cull = Cull::Back;
    // Fog: linear in eye depth from fog_start (none) to fog_end (all fog colour); every pass of the draw is fogged.
    bool fog = false;
    float fog_start = 0.0f;
    float fog_end = 1.0f;
    Float4 fog_colour{0, 0, 0, 1};
    float alpha_cutoff = 0.0f;  // a texel at or below this alpha is not drawn; 0 = everything is drawn
};

struct Sampler {
    Filter filter = Filter::Anisotropic;
    Address u = Address::Wrap;
    Address v = Address::Wrap;
};

// The two combine forms of the game's material program (r2/SITES_PIPELINES.md 4.3-4.4; shaders/mesh.hlsl):
//   OneStage  C0 x channel x texture - the decision table's cases B and the effects pipeline's base pass;
//   TwoStage  sat(C1 + C0 x channel) [x texture] - cases A and C.
enum class Combine : std::uint8_t { OneStage, TwoStage };

// A second draw of the same mesh (census 6.2): the environment reflection, or a second texture over the first.
struct ExtraPass {
    // BumpEnvironment (MatFX BUMPENVMAP, 0x804A4E00) is not a second draw: the GameCube combines it in the base
    // pass's own TEV (mesh.hlsl PSBase, kBumpEnvironment); `texture` is then the environment map.
    enum class Kind : std::uint8_t { None, Environment, Dual, BumpEnvironment } kind = Kind::None;
    TextureHandle texture;
    Sampler sampler;
    float coefficient = 0.0f;       // environment strength (drawn as min(coefficient, 1))
    Blend src = Blend::One;         // dual: the material's pair (the environment pass adds: ONE / ONE)
    Blend dst = Blend::One;
    bool second_uv_set = false;     // dual: UV1 (geometry with two UV sets)
    float alpha_cutoff = 0.0f;      // dual: the second texture's compare
    // dual: the second layer's own UV transform - MatFX DUALUVTRANSFORM's dual matrix, identity when it has none (or
    // for DUALTEXTURE); never the base layer's (docs/renderer-re-20261007/04_GC_DRIVER.md 3.18).
    float uv_transform[2][3] = {{1, 0, 0}, {0, 1, 0}};
    // bump-environment: the bump texgen's scale, -(coefficient x invBumpWidth) of the bump slot (0x804A4FEC-0x804A5000;
    // the height lookup's offset is this x the view normal), and the environment slot's useFrameBufferAlpha.
    float bump_shift = 0.0f;
    bool framebuffer_alpha = false;
};

// What a mesh's surface looks like. The switches are the material program's inputs (SITES_PIPELINES.md 4.3):
// P the vertex colours are the channel's source, M the material colour enters, L the light set applies, X an
// ambient light is present.
struct Material {
    Float4 colour{1, 1, 1, 1};
    TextureHandle texture;  // none: untextured
    Sampler sampler;
    Combine combine = Combine::OneStage;
    bool vertex_colours = true;  // P
    bool modulate = false;       // M
    bool lit = false;            // L
    bool ambient = false;        // X
    float ka = 1.0f;
    float uv_transform[2][3] = {{1, 0, 0}, {0, 1, 0}};  // u' = r0 . (u, v, 1), v' = r1 . (u, v, 1)
    ExtraPass extra;
    // The fade-in (gekko/appear_fade.h): the share of the draw's pixels still hidden, an ordered dither;
    // 0 = drawn as the game drew it.
    float dissolve = 0.0f;
    // The baked light (render/effect_layers.h): false draws the vertex colours' RGB as white (their alpha stays).
    bool baked_light = true;
};

// The lights a draw sees, in world space (SITES_PIPELINES.md 4.2).
struct LightSet {
    Float3 ambient;  // the clamped sum of the ambient lights
    std::uint32_t directional_count = 0;
    Float3 directional_colour[4];
    Float3 directional_vector[4];  // the direction the light travels
    std::uint32_t point_count = 0;
    struct Point {
        Float3 position;
        float radius = 1.0f;
        Float3 colour;
    } points[8];
};
using LightSetId = std::uint32_t;
inline constexpr LightSetId kNoLights = 0xFFFFFFFFu;

// The game's camera (R_DESIGN.md 2.2): the camera frame in world space - columns right (screen right), up, forward
// (the direction it looks) and position - and the game's view window: the half extents of its view at distance 1.
// What a view displays of it is view.h's displayed_window(): the authored picture (`framing`) fitted to the view.
struct Camera {
    Float3x4 frame;
    float view_window_x = 0.5f;
    float view_window_y = 0.375f;
    float near_plane = 1.0f;
    float far_plane = 10000.0f;
    bool parallel = false;
    Framing framing = Framing::SquarePixels;
};

// The part of the output a view fills, as fractions of the output (split screen: one per player).
struct ViewRect {
    float x = 0, y = 0, width = 1, height = 1;
};

// A view's 2D canvas: the space the game lays its screen-space draws out in (the HUD, menus, prompts, fades), in units
// from the view's top-left corner, y down, and the aspect the canvas was authored to be seen at (its units need not
// be square: a 640 x 528 canvas shown 4:3). Zero size: the view's own pixels; zero aspect: width / height.
struct Canvas {
    float width = 0;
    float height = 0;
    float aspect = 0;
};

enum class PassKind : std::uint8_t {
    Scene,          // the view's scene colour and depth
    GlareSources,   // the view's glare sources: drawn onto black over the scene's depth (tested, not written)
    Glare,          // the glare of the sources drawn since the view's last Glare pass, added to the scene (no draws)
    Snapshot,       // the view's scene colour as it is here, for later draws of the frame to sample (no draws)
    SaveHistory,    // the view's history becomes its scene colour as it is here (no draws)
};

// The game's glare parameters (docs/effects-2026-09-21/BLOOM_SPEC.md 3.1, 6.4), resolved to what the passes use.
// Level A: the sources box-downsampled to (96 W/H, 128) texels times gain_a, then per axis the sum of 1 + taps
// unweighted taps `spread` texels apart (horizontal first). Level B (half of A): gain_b times A's 2 x 2 box (or the
// sources downsampled, `b_from_sources`), summed the same way. The scene gets the sources plus A merged with B (or A
// and B on their own, `separate`).
struct GlareParams {
    bool level_a = true;
    bool level_b = true;
    bool separate = false;
    bool b_from_sources = false;
    float gain_a = 30.0f / 255.0f;
    float gain_b = 48.0f / 255.0f;
    float spread_a = 1.0f;
    float spread_b = 1.0f;
    std::uint32_t taps_a[2] = {2, 2};  // horizontal, vertical
    std::uint32_t taps_b[2] = {4, 4};
};

struct Clear {
    bool colour = false;
    Float4 colour_value{0, 0, 0, 1};
    bool depth = false;
};

using ViewId = std::uint32_t;

// ---- the packet --------------------------------------------------------------------------------------------------

// A skinned draw's palette: `count` matrices of the packet's bone list from `first` (bone b of the mesh's skin is
// entry first + b). The skinned positions are world space, so such a draw's world matrix is the identity.
struct BoneRange {
    std::uint32_t first = 0;
    std::uint32_t count = 0;  // 0: not skinned
};

// A delta-morphed draw's weights: `count` of the packet's morph weights from `first` (target t of the mesh's delta
// targets blends with entry first + t). The renderer blends the targets onto the mesh for this draw - before skinning
// when the draw is also skinned - so every draw carries its own shape and the mesh never changes.
struct MorphRange {
    std::uint32_t first = 0;
    std::uint32_t count = 0;  // 0: the mesh's base shape
};
inline constexpr std::uint32_t kMaxMorphTargets = 8;  // the most delta targets a mesh may carry

// One particle (the game's particle records): a camera-facing quad of `size` (full extents along the batch's axes),
// turned by `rotation` (radians) in the plane of the axes, textured from the uv0 (top-left) to the uv1 (bottom-right)
// corner, coloured by `colour` (RGBA8, R in the lowest byte) when the batch uses particle colours.
struct Particle {
    Float3 position;
    float size[2] = {1, 1};
    std::uint32_t colour = 0xFFFFFFFFu;
    float rotation = 0.0f;
    float uv0[2] = {0, 0};
    float uv1[2] = {1, 1};
};
static_assert(sizeof(Particle) == 44, "a particle is the 44-byte record the renderer streams per instance");

// A particle system's draw (shaders/particles.hlsl): `count` particles of the packet's list from `first`, spanning
// +/- size/2 along `right` and `up` (world space; the quad's first corner is -right -up).
struct ParticleBatch {
    std::uint32_t first = 0;
    std::uint32_t count = 0;
    Float3 right{1, 0, 0};
    Float3 up{0, 1, 0};
    bool particle_colours = true;  // each particle's own colour, else `colour`
    Float4 colour{1, 1, 1, 1};
    TextureHandle texture;         // none: untextured
    Sampler sampler;
};
inline constexpr std::uint32_t kNoParticles = 0xFFFFFFFFu;

// Immediate geometry: vertices the game hands over with the draw (text glyphs, trails, beams, the HUD), coloured per
// vertex and optionally textured, positioned in the world by the draw's world matrix or laid out on the view's canvas.
struct ImmediateVertex {
    Float3 position;
    std::uint32_t colour = 0xFFFFFFFFu;  // RGBA8, R in the lowest byte
    float uv[2] = {0, 0};
};
static_assert(sizeof(ImmediateVertex) == 24, "an immediate vertex is the 24-byte stream the renderer draws");

enum class Topology : std::uint8_t { Lines, LineStrip, Triangles, TriangleStrip };

// Where an immediate draw's positions are: in the world (the draw's world matrix and the pass's camera place them), or
// on the view's canvas (x, y in canvas units, z the depth itself: 0 nearest, 1 farthest).
enum class Space : std::uint8_t { World, Canvas };

// How a canvas draw keeps its place on a view whose aspect is not the canvas's: view.h's Anchor and canvas_fit().

// What an immediate draw samples: a texture the game loaded, a snapshot of its view's scene colour taken earlier in
// the frame (distortion: heat haze, refraction), or its view's history - the scene colour as the view last saved it,
// normally the frame before (feedback: the motion trail). Snapshots and the history cover their view. A canvas draw
// gives their coordinates as canvas points (the point whose picture it samples, in canvas units) and the renderer
// maps them with the draw's anchor exactly as it maps positions, so an undisplaced vertex samples what lies under it
// on any output; other draws give them as fractions of the view, (0, 0) its top-left corner.
enum class ImageSource : std::uint8_t { Texture, Snapshot, History };
using SnapshotId = std::uint32_t;

struct ImmediateDraw {
    Topology topology = Topology::Triangles;
    Space space = Space::World;
    Anchor anchor = Anchor::Center;  // a canvas draw's
    ImageSource source = ImageSource::Texture;
    SnapshotId snapshot = 0;         // ImageSource::Snapshot: which of the frame's snapshots
    std::uint32_t first_vertex = 0;  // into FramePacket::immediate_vertices
    std::uint32_t vertex_count = 0;
    std::uint32_t first_index = 0;   // into FramePacket::immediate_indices (relative to first_vertex)
    std::uint32_t index_count = 0;   // 0: the vertices in order
    TextureHandle texture;           // none: the vertex colours alone
    Sampler sampler;
    // A canvas draw laid out in a region of the view's canvas (a player's viewport in split screen, view.h
    // CanvasRegion): an index into FramePacket::canvas_regions; kNoRegion: the whole canvas.
    std::uint32_t region = 0xFFFFFFFFu;
};
inline constexpr std::uint32_t kNoImmediate = 0xFFFFFFFFu;
inline constexpr std::uint32_t kNoRegion = 0xFFFFFFFFu;

// A draw: a mesh's submesh, a particle batch (`particles` set) or immediate geometry (`immediate` set); the latter
// two take the recipe from the record, immediate geometry also the world matrix.
struct DrawRecord {
    MeshHandle mesh;
    std::uint32_t submesh = 0;
    Material material;
    Float3x4 world;
    Recipe recipe;
    LightSetId lights = kNoLights;
    BoneRange bones;
    std::uint32_t particles = kNoParticles;  // index into FramePacket::particle_batches
    std::uint32_t immediate = kNoImmediate;  // index into FramePacket::immediates
    MorphRange morph;
    EffectLayer layer = EffectLayer::None;   // render/effect_layers.h: the effect the draw belongs to (when tagged)
};

// A pass draws into its view's targets. The game renders parts of one picture with different cameras into the same
// colour and depth (its land classes, the sky, the effects each open their own camera bracket), so a pass may carry
// its own camera; without one it uses the view's.
struct PassRecord {
    ViewId view = 0;
    PassKind kind = PassKind::Scene;
    Clear clear;
    bool has_camera = false;
    Camera camera;
    std::uint32_t first_draw = 0;
    std::uint32_t draw_count = 0;
    GlareParams glare;  // a Glare pass
};

// The output is ONE picture: views are rectangles of it, drawn in the order the game drew them. A view that draws
// over the views before it (`over_views`: the full-screen overlay of split screen - the HUD and the fades the game
// draws over both players' viewports after them) starts from the picture those views made where it covers them,
// instead of the background; the views are composed in order, so it lies on top.
struct ViewRecord {
    Camera camera;
    ViewRect rect;
    Canvas canvas;
    bool over_views = false;
};

struct FramePacket {
    std::uint64_t tick = 0;
    std::uint64_t game_ticks = 0;  // the game clock when the game finished the frame (0: not known)
    std::int64_t pushed_ns = 0;    // steady_clock ns when PacketQueue::push took it (gekko/phase_lock.h)
    std::int64_t anchor_shift_ns = 0;  // the phase lock's applied anchor shift when it was pushed (phase_anchor_shift_ns)
    Float4 background{0, 0, 0, 1};
    // The aspect of the box the views are shown in, centred in the output with black outside it; 0: the whole output.
    // An in-engine cutscene keeps the 4:3 it was composed for.
    float content_aspect = 0.0f;
    std::vector<ViewRecord> views;
    std::vector<LightSet> light_sets;
    std::vector<PassRecord> passes;
    std::vector<DrawRecord> draws;
    std::vector<Float3x4> bones;  // skin palettes, referenced by the draws' bone ranges
    std::vector<float> morph_weights;  // delta-morph weights, referenced by the draws' morph ranges
    std::vector<Particle> particles;
    std::vector<ParticleBatch> particle_batches;  // referenced by the draws' `particles`
    std::vector<ImmediateVertex> immediate_vertices;
    std::vector<std::uint32_t> immediate_indices;
    std::vector<ImmediateDraw> immediates;        // referenced by the draws' `immediate`
    std::vector<CanvasRegion> canvas_regions;     // referenced by the immediates' `region`
};

class FrameBuilder {
public:
    void begin_frame(std::uint64_t tick, const Float4& background);
    ViewId view(const Camera& camera, const ViewRect& rect, const Canvas& canvas = Canvas{}, bool over_views = false);
    // A region of a view's canvas for canvas draws to be laid out in (ImmediateDraw::region).
    std::uint32_t canvas_region(const CanvasRegion& region);
    LightSetId lights(const LightSet& set);
    // `camera`: the pass's own camera (null: the view's).
    void begin_pass(ViewId view, PassKind kind, const Clear& clear, const Camera* camera = nullptr);
    // A skin palette for this frame (bone matrices, world space).
    BoneRange bones(const Float3x4* matrices, std::uint32_t count);
    // A delta-morph weight set for this frame (one weight per target of the mesh it is drawn with).
    MorphRange morph(const float* weights, std::uint32_t count);
    void draw(MeshHandle mesh, std::uint32_t submesh, const Material& material, const Float3x4& world,
              const Recipe& recipe, LightSetId lights = kNoLights, BoneRange bones = {}, MorphRange morph = {});
    // A particle batch drawn at this point of the pass: `count` records (the batch's first / count are set here).
    void particles(const ParticleBatch& batch, const Particle* records, std::uint32_t count, const Recipe& recipe);
    // Immediate geometry drawn at this point of the pass (the draw's ranges are set here; `indices` may be null).
    void immediate(const ImmediateDraw& draw, const ImmediateVertex* vertices, std::uint32_t vertex_count,
                   const std::uint32_t* indices, std::uint32_t index_count, const Float3x4& world,
                   const Recipe& recipe);
    // The view's glare at this point: a Glare pass (it closes the open pass).
    void glare(ViewId view, const GlareParams& params);
    // A snapshot of the view's scene colour at this point (it closes the open pass); draws after it may sample it.
    SnapshotId snapshot(ViewId view);
    // The view's history becomes its scene colour at this point (it closes the open pass).
    void save_history(ViewId view);
    void end_pass();
    // The finished packet (the builder starts empty again).
    FramePacket finish();
    // C3 (HW_REPORT S): a packet that has been rendered (any thread). Its vectors - emptied, their capacity kept -
    // become the next frame's, so building a frame no longer grows a dozen vectors from nothing every frame
    // (FrameBuilder's reallocations: about 0.26 s of the game thread on the Lava replay).
    void recycle(FramePacket&& used);
    // Drops the frame being described (a restored state replaced the timeline); a recycled packet is kept.
    void discard() {
        packet_ = FramePacket{};
        pass_open_ = false;
        snapshots_ = 0;
    }

    std::size_t draws() const noexcept { return packet_.draws.size(); }
    std::size_t passes() const noexcept { return packet_.passes.size(); }
    std::size_t immediates() const noexcept { return packet_.immediates.size(); }
    // Re-places an immediate draw recorded earlier in the frame (the 2D layout learns that a draw belongs to a tiled
    // strip only from the draws after it).
    void set_immediate_anchor(std::size_t immediate, Anchor anchor);
    // The effect layer the next draws belong to (render/effect_layers.h); stamped on every draw until changed.
    void set_layer(EffectLayer layer) noexcept { layer_ = layer; }
    EffectLayer layer() const noexcept { return layer_; }

private:
    EffectLayer layer_ = EffectLayer::None;
    FramePacket packet_;
    std::mutex recycled_mutex_;  // C3: the rendered packet given back, taken by begin_frame
    FramePacket recycled_;
    bool has_recycled_ = false;
    bool pass_open_ = false;
    SnapshotId snapshots_ = 0;
};

// A camera frame looking from `eye` at `target`, with `up` as the world's up direction.
Float3x4 look_at(const Float3& eye, const Float3& target, const Float3& up);

}  // namespace shadow::render
