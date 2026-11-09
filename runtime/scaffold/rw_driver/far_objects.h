// SCAFFOLDING (R2): far-object proxies read from the translated layout manager and RenderWare resources - deleted by R6: the scene graph's lift owns the layout and draws far objects itself
#pragma once

// Far-object proxies: the layout objects the game has not spawned yet, drawn by the native renderer from the game's
// own model data, so a stage is seen to any distance while the game's range - what is alive, what thinks, what its
// heap holds - stays the original's (gekko/native_object_range.h). Strictly visual: no game object, no collision, no
// state, no guest allocation. docs/DRAW_DISTANCE.md "Far-object proxies".
//
// Which entries: exactly those the layout manager's spawn loop (0x800CAC08 / 0x800CAA2C) would spawn if a player were
// in range - a slot with a type (+0x10 >= 0), its entry flagged 0x1 and neither 0x2 nor 0x20000, and, while the
// manager's restricted bit (+0x428 bit 0) is set, 0x8000. 0x2 is set while the game's object is alive (measured: set at
// its spawn, cleared when it leaves its range) and 0x1 cleared when it is destroyed or collected, so a proxy never
// doubles a live object nor shows what the game will not bring back.
//
// Which model: resolved from data, never by spawning, per class (kRules in far_objects.cpp): the class's model
// library (a <CLASS>.BIN resource found by name or by id in the resource manager, as the class's constructor finds it)
// and the model entry the constructor builds - by key (0x80162E4C) or, for an ART GADGET, by its gadget record's name.
// An entry's parts are DFF resources of the same type, placed by the part's matrix.
//
// Where: the entry's position and rotation, R = Ry * Rx * Rz in degrees, times the part's matrix and each atomic's
// frame chain below the clump root.
//
// When: inside the view and at least kMinPixels across - radius * focal / distance, the projected size - so a proxy
// begins below a pixel and cannot be seen to appear; the real object replaces it at the original range, in the same
// pose: a spawned entry's proxy is kept until one of its models is drawn by the game (same geometry, same place).
//
// Each class is verified on its own: at every handover the game's own atomic is compared with the proxy's (same
// geometry); only classes whose measured pose matches are drawn by default (`verified`). SHADOW_FAR_VALIDATE=1 draws
// the unverified ones too, for measuring.

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "gekko/types.h"
#include "motion/skeleton_pose.h"
#include "render/frame.h"

namespace gekko {
class Cpu;
}

namespace gekko::scaffold {

class FarObjects {
public:
    static constexpr float kMinPixels = 1.0f;
    static constexpr u32 kHandoverFrames = 3;          // the most a spawned enemy's stand-in outlives it
    // ... a spawned prop's proxy while its entry stays live and the game has not drawn the object yet (measured: some
    // objects first draw about 60 frames after their spawn - docs/DRAW_DISTANCE.md "Handover gaps").
    static constexpr u32 kHandoverHold = 120;
    static constexpr float kHandoverDistance = 20.0f;  // a game atomic of the same geometry this close is the entry's
    static constexpr float kSamePlace = 1.0f;         // ... this close: the game draws the proxy's model itself
    static constexpr std::size_t kMaxRules = 48;

    // On or off, and the farthest a proxy is drawn as a multiple of its entry's authored range (0 = no limit).
    void set_enabled(bool enabled) noexcept { enabled_ = enabled; }
    bool enabled() const noexcept { return enabled_; }
    // Drawing proxies, or measuring (a class trace runs at draw distance 1 too, drawing nothing).
    bool active() const noexcept { return enabled_ || trace_class_ != nullptr || distance_trace_; }
    // Measuring (SHADOW_DISTANCE_TRACE=1): the block manager's mask against the area boxes tested at the camera
    // list's eye (r13+0x2B34 element 0 +8) and at the RwCamera's frame, the land slots, the camera's clip planes and the fog colour, each frame.
    void set_distance_trace(bool on) noexcept { distance_trace_ = on; }
    void set_range_scale(float scale) noexcept { range_scale_ = scale; }
    // Draw the classes not yet verified too (their handovers measure them).
    void set_validate(bool validate) noexcept { validate_ = validate; }
    bool validating() const noexcept { return validate_; }

    // Log every entry of class `factory` within kTraceRadius of the eye, each frame from `first` to `last`.
    void set_trace(const char* factory, u64 first, u64 last) noexcept {
        trace_class_ = factory;
        trace_first_ = first;
        trace_last_ = last;
    }
    static constexpr float kTraceRadius = 20000.0f;
    // From frame `first` to `last` every `step` frames (0: never), list the layout's classes: entries, candidates (in
    // view), live, nearest candidate, covered.
    void distance_trace(Cpu& c, const shadow::render::Camera& camera);
    bool distance_trace_ = false;
    u64 distance_frames_ = 0, mask_by_list_ = 0, mask_by_camera_ = 0, mask_frames_ = 0;
    u32 last_far_bits_ = 0;
    std::array<u16, 32> last_resident_{};
    std::array<u64, 168> block_in_mask_{}, block_out_of_mask_{};  // frame +1 it entered / left the mask (0: not)
    std::array<bool, 168> block_resident_{};
    void set_census(u64 first, u64 last, u64 step) noexcept {
        census_frame_ = first;
        census_last_ = last < first ? first : last;
        census_step_ = step == 0u ? 1u : step;
    }

    struct Proxy {
        u32 atomic = 0;                 // the library model's atomic
        shadow::render::Float3x4 world;  // where it is drawn (a skinned proxy: its palette poses it)
        u32 palette = 0;                // a skinned proxy's bone palette: `bones` matrices from palette(proxy)
        u32 bones = 0;
        s32 layer = -1;                 // the display layer the game puts this part in (-1: not known, kOpaqueLevel)
        s32 light_group = -1;           // ... and the light group its draw applies (docs/DRAW_DISTANCE.md "Layers")
        bool uva = false;               // its UV animation is a registered, shared one (Atomic::uva)
    };
    // A part the game draws in no layer world but from a task under render level 2 (Opeq): CameraRender 0x801D04D4
    // makes the light manager's own world current ([0x80571DD8 +0x45C]), attaches all its lights to it and sets the
    // level's render states (0x80046D80 with the level's mask, table 0x804D24EC) - the rings' draw 0x80112C70.
    static constexpr s32 kOpaqueLevel = -2;
    // Counts the stage changes (a new set of model libraries): what is cached per stage starts again.
    u64 stage_epoch() const noexcept { return stage_epoch_; }
    const shadow::render::Float3x4* palette(const Proxy& proxy) const noexcept { return palette_.data() + proxy.palette; }
    // The proxies to draw through `camera` at `focal` pixels per unit of view-space slope (render height / 2 / the
    // view window's half height). Reads guest memory only.
    void collect(Cpu& c, const shadow::render::Camera& camera, float focal, std::vector<Proxy>& out);

    // The game drew `geometry` at `world` this frame (the real objects draw before the proxies, in the same camera
    // bracket): it ends a spawned entry's proxy and measures the class's pose.
    void real_drawn(u32 geometry, const shadow::render::Float3x4& world) noexcept;
    // Measuring (a class trace): the game's atomic drawn with one of the traced class's geometries - its words, the
    // first frames it is drawn.
    void trace_atomic(Cpu& c, u32 atomic, u32 geometry);
    // Measuring (a class trace): a draw whose materials are all at alpha 0.
    void real_drawn_transparent(u32 geometry, const shadow::render::Float3x4& world) noexcept {
        if (trace_class_ != nullptr && transparent_this_frame_.size() < 1024u)
            transparent_this_frame_.push_back(Real{geometry, world});
    }

    struct ClassStats {
        const char* name = nullptr;
        bool verified = false;
        u64 candidates = 0;     // candidate-frames of this class
        u64 resolved = 0;       // ... with a model resolved from data
        u64 drawn_entries = 0;  // ... drawn (inside the view, at least kMinPixels)
        u64 compared = 0;       // handovers compared with the game's own atomic
        u64 exact = 0;          // ... equal to it (every matrix element within 1e-3)
        u64 doubled = 0;        // proxies not drawn: the game drew the same geometry there that frame
        u64 faded = 0;          // entry-frames a proxy stood in for its live object fading at the range's edge
        u64 hidden = 0;         // candidate-frames with no stand-in: the game hides it (until near, or by its switch)
        float max_error = 0.0f; // the largest matrix element difference seen
        u32 reported = 0;       // differences printed (SHADOW_FAR_VALIDATE)
    };
    struct Stats {
        u64 frames = 0;
        u64 candidates = 0;     // entries the spawn loop would spawn if in range (summed over frames)
        u64 resolved = 0;
        u64 drawn_entries = 0;
        u64 drawn_atomics = 0;
        u64 handovers = 0;        // proxies kept after their entry spawned, until its real model drew
        u64 handover_frames = 0;  // frames such a proxy was still drawn
        u64 handover_timeouts = 0;
        std::array<ClassStats, kMaxRules> classes{};
        std::size_t class_count = 0;
    };
    const Stats& stats() const noexcept { return stats_; }

private:
    struct Atomic {
        u32 atomic = 0;
        u32 geometry = 0;
        shadow::render::Float3x4 pose;  // under the model's origin
        u32 part = 0;                   // its part record in the model library (0x40 bytes; 0: a bare clump's)
        s32 layer = -1;                 // its display layer (layer_of), -1 when it has no part record
        s32 light_group = -1;           // its light group (0x804C397C[record +0x34])
        bool uva = false;               // its record names a .UVA (+0x08): its UV animation is the shared, registered kind
    };
    struct Model {
        std::vector<Atomic> atomics;
        float radius = 0.0f;  // bounds the whole model about its origin
    };
    struct Shown {
        shadow::render::Float3x4 frame;
        const Model* model = nullptr;
        std::size_t rule = 0;
        u32 frames_left = 0;  // a handover's remaining frames
        u64 key = 0;          // measuring: the entry (and ring), its slot's state object and first state word
        u32 state = 0;
        u32 bits = 0;
        u32 partial = 0;      // frames since the game first drew some of its parts
    };
    struct Real {
        u32 geometry = 0;
        shadow::render::Float3x4 world;
    };
    // An enemy kind's stand-in (docs/DRAW_DISTANCE.md "Enemies and characters"): its skeleton and clip as the game
    // loaded them, posed once a frame by the game's evaluator, and its clump's skinned atomics.
    struct Skin {
        u32 atomic = 0;
        u32 inverse_bind = 0;             // RwMatrix[bones]
        u32 bones = 0;
        std::vector<int> node_bone;       // skin bone -> skeleton bone (by the hierarchy node's id; -1 none)
        int frame_bone = -1;              // the bone of the node whose frame the atomic hangs on (-1 none)
    };
    struct Enemy {
        bool ok = false;
        u64 retry = 0;                    // a failed lookup is tried again from this frame (banks still loading)
        const char* why = nullptr;        // the last failure, reported once a kind
        shadow::motion::Skeleton skeleton;
        shadow::motion::Clip clip;
        shadow::motion::SkeletonPose pose;
        std::vector<shadow::motion::Mtx34> world;   // the pose, model space, by skeleton bone
        std::vector<shadow::motion::Mtx34> spawn;   // the clip's first frame: the game restarts it at the spawn
        u64 posed_frame = ~u64{0};
        std::vector<Skin> skins;
        Model model;                      // its atomics at the origin (handover and doubles by geometry)
        s32 layer = -1, light_group = -1; // where the game's enemy draws, and lit by (0x801A4654)
    };
    Enemy* enemy_for(Cpu& c, std::size_t rule);
    // Pose the enemy for this frame, then its proxies at `frame` into `out` (palettes into palette_).
    // `spawned`: the entry's object exists (a handover): its stand-in shows the clip's first frame, as the game does.
    void push_enemy(Cpu& c, Enemy& enemy, const shadow::render::Float3x4& frame, std::vector<Proxy>& out,
                    bool spawned = false);

    // A bare clump's model; its atomics draw in `layer` lit by `light_group` (-1: as a model part, set by the caller).
    const Model* clump_model(Cpu& c, u32 clump, s32 layer = -1, s32 light_group = -1);
    const Model* entry_model(Cpu& c, s32 type, u32 library, u32 model_entry);
    const Model* model_for(Cpu& c, std::size_t rule, u32 library, u32 entry);
    int rule_of(Cpu& c, u32 factory);
    // The nearest real draw matching `shown` this frame (same geometry, within kHandoverDistance), measured into its
    // class.
    bool match_real(const Shown& shown);
    // The game drew `model` at `frame` this frame (an atomic of the same geometry within kSamePlace).
    bool trace_entry(Cpu& c, u32 factories, s32 type, u32 entry, const float eye[3]);
    void census(Cpu& c, u32 factories, u32 slots, bool restricted, const shadow::render::Camera& camera);
    bool drawn_by_game(const shadow::render::Float3x4& frame, const Model& model) const;
    std::vector<Real> transparent_this_frame_;
    std::vector<u32> traced_geometries_;  // measuring: the traced class's part geometries
    std::vector<u32> traced_atomics_;     // ... and the game atomics seen drawing them
    // The game drew this one atomic's geometry this frame within `reach` of `world`.
    bool atomic_drawn(const shadow::render::Float3x4& world, u32 geometry, float reach) const;
    // ... every atomic of `model` at `frame` (an object's parts may first draw on different frames).
    bool all_drawn_by_game(const shadow::render::Float3x4& frame, const Model& model, float reach) const;

    bool enabled_ = true;
    bool validate_ = false;
    u64 stage_epoch_ = 0;

    const char* trace_class_ = nullptr;
    u64 trace_first_ = 0;
    u64 trace_last_ = 0;
    u64 census_frame_ = 0;
    u64 census_last_ = 0;
    u64 census_step_ = 1;
    float range_scale_ = 0.0f;
    std::array<u32, kMaxRules> libraries_{};          // each rule's library, as last resolved
    std::unordered_map<u32, Model> clumps_;           // clump -> its atomics
    std::unordered_map<u64, Model> entries_;          // (library, model entry) -> its parts' atomics, placed
    std::unordered_map<u32, int> factory_rules_;      // factory -> rule index (-1: no proxy)
    // Keyed by entry, a group's ring by (ring index + 1) << 32 | entry.
    std::unordered_map<u64, Shown> shown_;            // entry -> its proxy, drawn last frame
    std::unordered_map<u64, Shown> shown_now_;
    std::unordered_map<u64, Shown> handover_;         // spawned entries whose real model has not drawn yet
    std::unordered_map<u64, Shown> late_;             // measuring: timed-out handovers, until the game draws them
    // Measuring (validation): blinks - an entry on screen drawn (by its proxy or the game) one frame and by neither
    // the next. Entry -> {last frame drawn, first frame of the open gap (0: none), flags then}.
    struct Gap {
        u64 drawn = 0, open = 0;
        u32 flags = 0;
    };
    std::unordered_map<u32, Gap> gaps_;
    u64 gap_count_ = 0, gap_frames_ = 0;
    void find_gaps(Cpu& c, u32 factories, u32 slots, const shadow::render::Camera& camera,
                   const std::vector<Proxy>& out);
    std::vector<Real> real_this_frame_;               // the game's own models drawn so far this frame
    std::unordered_map<u32, Enemy> enemies_;          // enemy kind -> its stand-in
    std::vector<shadow::render::Float3x4> palette_;   // this frame's skinned proxies' palettes
    Stats stats_{};
};

}  // namespace gekko::scaffold
