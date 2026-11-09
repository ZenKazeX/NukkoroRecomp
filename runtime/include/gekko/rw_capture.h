#pragma once

// Observe-only capture of the RenderWare GameCube driver ("RwGC") for the
// native renderer. See docs/native-runtime-2026-09-16/ARCHITECTURE.md.
//
// Every hook installed here returns false: the recovered guest body runs
// unchanged. The hooks only read guest memory (directly from MEM1, never
// through an observing accessor) and append typed commands to the current
// FrameSegment. There is no GX command stream underneath (phase 4,
// docs/native-time-20261005/GX_DATA_PLANE.md): the immediate geometry builder
// (gekko/native_immediate.h) hands each GXBegin primitive's payload to
// on_primitive(), and the lowered copy-trigger sites call on_copy_trigger().
//
// What comes from where:
//   * fixed-function draw state  - argument values of the GX SDK state API the
//                                  driver calls (blend, depth, alpha compare,
//                                  fog, matrices, lights, channel colours, texgen)
//   * combiner program           - RenderWare semantics: pipeline + RwGC
//                                  decision-table case at the render callback
//   * retained geometry          - the RwResEntry instance data, once per generation
//   * immediate vertices         - the payload of the primitive the driver sealed
//                                  with GXBegin (Im2D, Im3D, PTank)
//   * textures                   - the RwRaster behind each TextureBind, once per
//                                  content generation
//   * frame structure            - cameras, clears, camera-texture copies, present

#include <array>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "gekko/gx.h"
#include "gekko/native_frame.h"
#include "gekko/native_immediate.h"
#include "gekko/types.h"

namespace gekko {

class Cpu;
class System;

class RwCapture final : public ImmediateSink {
public:
    explicit RwCapture(System& system);
    ~RwCapture() override;

    RwCapture(const RwCapture&) = delete;
    RwCapture& operator=(const RwCapture&) = delete;

    // Installs every hook and the immediate geometry sink. Call at the end of
    // System::start(), after every other hook, so observers chain in front of
    // whatever handler already owns an address.
    void install();

    // Where closed segments go. Without a queue the segment is recycled in place
    // (counting-only capture).
    void set_queue(native::SegmentQueue* queue) noexcept { queue_ = queue; }

    // Validation: re-hash texture bytes at every bind of an uploaded generation
    // and count content changes that arrived without a lifetime event.
    void set_validate_texture_content(bool enabled) noexcept { validate_textures_ = enabled; }

    // `--measure gx-inputs` (observation only, off by default): an FNV-1a 64
    // digest of every GXBegin-tracked immediate primitive this receives -
    // opcode, vertex count, VAT A/B/C words, stride, payload bytes, in order -
    // the phase-4 equivalence instrument (GX_DATA_PLANE.md section 4).
    void enable_gx_input_digest() noexcept { gx_input_digest_enabled_ = true; }
    bool gx_input_digest_enabled() const noexcept { return gx_input_digest_enabled_; }

    // Submission from a host module that has REPLACED a guest draw rather than
    // observed one - today, the native health bar.
    //
    // Why this is a method here and not raw access to the segment: every state
    // setter goes through a per-(op, index) shadow that deduplicates a repeat
    // of the value the renderer already has. A module that appended state
    // records directly would leave the shadow holding the GUEST's last value
    // while the renderer held its own, so the guest's next identical set would
    // be deduplicated away and the substituted state would persist for the rest
    // of the frame. Going through the same shadow keeps it honest: the shadow
    // ends up holding what the renderer actually has, so the guest's next set
    // differs and is re-emitted.
    //
    // Ordering needs nothing special. A segment is an append log with a
    // monotonic sequence, and this is called on the game thread at the exact
    // instant the replaced guest callback was entered, so the quads take
    // precisely the ordinal the guest's own draws would have taken. Do NOT
    // buffer these and flush at present - that moves them past the 2D HUD.
    //
    // `vertices` are in presentation pixels inside a `width` x `height`
    // rectangle, already final. The records are stamped WorldAnchored so the
    // HUD layout stage leaves them alone: classification travels on the draw,
    // never in a global latch.
    bool submit_native_ui(const shadow::render::RenderVertex* vertices, u32 vertex_count, float width,
                          float height);

    // Savestate. The capture's per-(op, index) shadow of the last value every GX
    // state setter was given IS the GX state at the save point, so it is saved
    // with the machine and replayed on load: the renderer resumes with exactly
    // the restored state instead of defaults the guest may not re-issue for a
    // long time (viewport, scissor and lights are set a handful of times per
    // session). Resources (textures, geometry) are not saved: guest memory was
    // replaced, so every record restarts and uploads again at its next use.
    void save_state(std::vector<u8>& out) const;
    bool load_state(const u8*& cursor, const u8* end);
    // A load with no capture block (older file, or saved without the native
    // renderer): shadows are unknown, so they are invalidated instead.
    void on_state_loaded();

    // ImmediateSink: one complete GXBegin primitive (NativeImmediate).
    void on_primitive(u8 opcode, u32 vertex_count, const VertexFormat& format, u32 vat_a,
                      u32 vat_b, u32 vat_c, const u8* payload,
                      std::size_t payload_bytes) override;
    // A lowered copy-trigger site (BP 0x52, GXCopyDisp / GXCopyTex).
    void on_copy_trigger(u32 value);
    // The recovered 2D identity hooks (system.cpp) announce layout groups here.
    void on_hud_group(HudGroupKind kind, u32 group_id, float canvas_x, u32 instance);
    void on_hud_scene(u32 node, u32 cast);

    struct Report {
        u64 segments = 0;
        u64 presents = 0;
        u64 commands = 0;
        u64 payload_bytes = 0;
        std::array<u64, static_cast<std::size_t>(native::Op::Count)> op_counts{};
        u64 state_calls = 0;          // GX state API calls observed
        u64 state_deduplicated = 0;   // ... that repeated the value already set
        u64 immediate_draws = 0;
        u64 mesh_draws = 0;
        u64 clear_quads_absorbed = 0;
        // Im3D draws whose program (rwIM3D_VERTEXUV) differs from what the old
        // Im-texture-raster proxy would have chosen. 0 on every measured route.
        u64 im3d_program_proxy_disagreements = 0;
        // PTank draws the guest programs as PASSCLR (flags & 0x84 == 0).
        u64 ptank_untextured = 0;
        u64 untracked_primitives = 0;   // FIFO primitive with no sealed producer: coverage bug
        u32 first_untracked_opcode = 0;
        u64 unknown_begin_sites = 0;    // GXBegin from an unlisted return address
        u32 first_unknown_begin_lr = 0;
        u64 unknown_display_list_sites = 0;
        u32 first_unknown_display_list_lr = 0;
        u64 display_list_without_object = 0;
        u64 display_list_mesh_not_found = 0;
        u64 immediate_indexed_attributes = 0;  // immediate draw with an indexed attribute
        u64 immediate_count_mismatch = 0;
        u64 unsupported_programs = 0;
        u64 texture_binds = 0;
        u64 texture_uploads = 0;
        u64 texture_upload_bytes = 0;
        u64 textures_served_native = 0;  // uploads served by the native asset manager
        u64 textures_native_miss = 0;    // manager lookups that fell back to guest bytes
        u64 texture_null_raster = 0;
        u64 texture_upload_failures = 0;  // unreadable/unsized raster: the GPU shows magenta
        u32 first_failed_texture_format = 0;
        u32 first_failed_texture_width = 0;
        u32 first_failed_texture_height = 0;
        u64 texture_unknown_filter = 0;
        u64 texture_content_changed_without_event = 0;
        u64 texture_releases = 0;
        u64 geometry_uploads = 0;
        u64 geometry_upload_bytes = 0;
        u64 geometry_dynamic_updates = 0;
        u64 geometry_dynamic_bytes = 0;
        u64 geometry_releases = 0;
        u64 geometry_invalid_entries = 0;
        // WHY AN UPLOAD HAPPENED AND WHY A RELEASE HAPPENED (2026-09-28).
        //
        // These replace two redundancy measures that hashed the PE draw-sync
        // token at +0 (re-stamped by every render callback) and the 32-byte
        // arena tail past `size` (the next heap block's header), and so could
        // not see a repeat at all: Death Ruins run-forward reported 7,392
        // "distinct" contents for 41 live objects. The identity excludes both
        // and is the key of the renderer's persistent geometry store.
        u64 geometry_upload_new_entry = 0;      // first upload for this RwResEntry address
        u64 geometry_upload_regenerate = 0;     // same address, serial or size moved
        u64 geometry_upload_retry = 0;          // re-send after a renderer refusal
        u64 geometry_release_notify = 0;        // the guest freed the RwResEntry
        u64 geometry_release_regenerate = 0;    // serial/size moved on a live entry
        u64 geometry_identity_distinct = 0;     // content never uploaded before
        u64 geometry_identity_repeat_live = 0;  // identical content live under another entry
        u64 geometry_identity_resurrected = 0;  // identical content released earlier, none live
        u64 geometry_identity_extent_outside = 0;  // a display list reaches past `size`
        // Who freed the entries: the destroy notify's return address and the
        // two frames above it, from the guest's own back chain (read only).
        struct ReleaseSite {
            u32 ret0 = 0, ret1 = 0, ret2 = 0;
            u64 count = 0;
        };
        static constexpr unsigned kReleaseSites = 16;
        std::array<ReleaseSite, kReleaseSites> release_sites{};
        u32 release_sites_used = 0;
        u64 release_sites_overflow = 0;
        // THE REVERSE CHANNEL. A refusal the renderer handed back, and what
        // this side did about it. `retries_exhausted` is the one that matters:
        // that object is still missing, and these are the only numbers that
        // say which and why.
        u64 rejections_received = 0;
        // Rejections the mailbox had to DROP. Each one is an object that can
        // never recover - exactly the failure the mailbox exists to end - so
        // it is read back from the mailbox and printed rather than left
        // write-only.
        u64 rejections_lost_to_overflow = 0;
        // Folded onto an entry already queued for the same resource. Lossless:
        // two requests to re-send one resource are worth exactly as much as one.
        u64 rejections_coalesced = 0;
        u64 rejections_unmatched = 0;   // arrived for a record that had already moved on
        u64 reuploads_requested = 0;
        u64 retries_exhausted = 0;
        std::array<u64, static_cast<std::size_t>(native::RejectReason::Count)> reject_reasons{};
        u64 geometry_blob_bytes = 0;            // instance blob bytes
        u64 geometry_appended_bytes = 0;        // per-frame arrays appended to it
        u64 copies = 0;
        u64 gx_copy_triggers = 0;
        // --measure gx-inputs: see enable_gx_input_digest.
        u64 gx_input_primitives = 0;
        u64 gx_input_payload_bytes = 0;
        u64 gx_input_digest = 0xCBF29CE484222325ull;
        u64 clears = 0;
        u64 cameras = 0;
        u64 state_loads = 0;
        u64 state_loads_replayed = 0;   // ... that restored the saved GX state
        u64 state_commands_replayed = 0;
        u64 guest_reads_out_of_range = 0;
        u64 queue_wait_ns = 0;
        u64 hook_calls = 0;
        u64 hook_ns = 0;        // wall time inside capture handlers (TSC)
        u64 tap_ns = 0;         // wall time inside on_primitive (TSC)
        // Default-off ownership census reuses the existing hook timestamps.
        // Fixed storage; names point to install()'s static labels.
        struct HookOwnerCost {
            u32 address = 0;
            const char* name = nullptr;
            u64 calls = 0;
            u64 ticks = 0;
        };
        std::array<HookOwnerCost, 64> hook_owner_costs{};
        u32 hook_owner_count = 0;
        u32 hook_owner_overflow = 0;
    };
    Report report() const;
    std::string report_text() const;

    // ---- STEP-0 MEASUREMENT (audit R4 / COHERE / HASH-TOKEN) - DEFAULT OFF ----
    //
    // A coherence probe for the skip-upload branch of prepare_geometry: at every
    // use of a geometry whose generation is unchanged it re-hashes the CURRENT
    // guest bytes of the blob (PE token and serial excluded, embedded pointers
    // normalised) and compares them with the hash taken when that generation was
    // uploaded. A mismatch means the host copy is older than the guest's - the
    // in-place re-emit (DMorph software path) the capture cannot see.
    //
    // Observation only. It reads MEM1 through its own unobserved accessor, never
    // RwCapture::guest() (which counts into report_), writes no guest state, no
    // production record and no report_ field. With `flush_overlap` it adds two
    // chained, declining observers at DCFlushRange/DCStoreRange (the guest's own
    // "data changed" signal); without it, zero HLE entries. Never enabled unless
    // enable_coherence_probe() is called. See rw_capture.cpp "CoherenceProbe".
    struct CoherenceProbeConfig {
        u32 max_events = 300;              // change events, non-skin pipelines
        u32 max_skin_events = 50;          // change events, Skin/SkinMatFx
        u32 max_events_per_record = 8;
        u32 max_record_summaries = 2048;
        u64 snapshot_budget_bytes = 64ull << 20;
        u32 selftest_uploads = 64;         // in-blob hash self-test on host copies
        bool flush_overlap = true;         // DCFlushRange/DCStoreRange observers
    };
    // Call before System::start() (the observers then install inside install(),
    // after every production hook) or after it (they install immediately).
    // A second call is a no-op.
    void enable_coherence_probe(const CoherenceProbeConfig& config);
    bool coherence_probe_enabled() const noexcept { return coherence_ != nullptr; }
    // ONE JSON object; {"enabled":false} when the probe is off.
    std::string coherence_json() const;
    // A few human lines; empty when the probe is off.
    std::string coherence_summary() const;

private:
    // STEP-0 measurement, defined in rw_capture.cpp. Null unless enabled.
    class CoherenceProbe;
    struct Hook;
    void observe(u32 address, void (RwCapture::*handler)(Cpu&), const char* name);

    // ---- guest memory (direct MEM1, unobserved) ----
    const u8* guest(u32 address, u32 size) const noexcept;
    u32 read32(u32 address) const noexcept;
    u16 read16(u32 address) const noexcept;
    u8 read8(u32 address) const noexcept;
    float read_f32(u32 address) const noexcept;

    // ---- segment ----
    native::FrameSegment& segment();
    template <typename Record>
    void emit(native::Op op, const Record& record);
    // Emits unless the same (op, index) slot already holds these exact bytes.
    template <typename Record>
    void emit_state(native::Op op, u32 index, const Record& record);
    void close_segment(bool present);

    // ---- hooks: GX state API ----
    void gx_set_viewport(Cpu& cpu);
    void gx_set_viewport_jitter(Cpu& cpu);
    void gx_set_scissor(Cpu& cpu);
    void gx_set_scissor_offset(Cpu& cpu);
    void glare_pre(Cpu& cpu);
    void glare_post(Cpu& cpu);
    void glare_window(Cpu& cpu, u32 phase);
    void gx_set_projection(Cpu& cpu);
    void gx_set_projection_v(Cpu& cpu);
    void gx_load_position_matrix(Cpu& cpu);
    void gx_load_normal_matrix(Cpu& cpu);
    void gx_set_current_matrix(Cpu& cpu);
    void gx_load_texture_matrix(Cpu& cpu);
    void gx_set_blend_mode(Cpu& cpu);
    void gx_set_z_mode(Cpu& cpu);
    void gx_set_z_comp_loc(Cpu& cpu);
    void gx_set_alpha_compare(Cpu& cpu);
    void gx_set_cull_mode(Cpu& cpu);
    void gx_set_fog(Cpu& cpu);
    void gx_set_color_update(Cpu& cpu);
    void gx_set_alpha_update(Cpu& cpu);
    void gx_set_dst_alpha(Cpu& cpu);
    void gx_set_num_chans(Cpu& cpu);
    void gx_set_chan_ctrl(Cpu& cpu);
    void gx_set_chan_mat_color(Cpu& cpu);
    void gx_set_chan_amb_color(Cpu& cpu);
    void gx_load_light(Cpu& cpu);
    void gx_set_num_tex_gens(Cpu& cpu);
    void gx_set_tex_coord_gen(Cpu& cpu);
    void gx_set_tev_color(Cpu& cpu);
    void gx_set_tev_konst_color(Cpu& cpu);
    // ---- hooks: draw seals ----
    void gx_begin(Cpu& cpu);
    void gx_call_display_list(Cpu& cpu);
    // ---- hooks: RwGC semantics ----
    void rw_render_default(Cpu& cpu);
    void rw_render_matfx(Cpu& cpu);
    void rw_render_skin(Cpu& cpu);
    void rw_render_skin_matfx(Cpu& cpu);
    void rw_render_ptank(Cpu& cpu);
    void rw_texture_bind(Cpu& cpu);
    void rw_camera_begin(Cpu& cpu);
    void rw_camera_end(Cpu& cpu);
    void rw_clear_quad(Cpu& cpu);
    void rw_camera_texture_copy(Cpu& cpu);
    void rw_show_raster(Cpu& cpu);
    void rw_raster_create(Cpu& cpu);
    void rw_raster_destroy(Cpu& cpu);
    void rw_raster_unlock(Cpu& cpu);
    void rw_raster_unlock_palette(Cpu& cpu);
    void rw_geometry_notify(Cpu& cpu);

    enum class Pipeline : u8 { None, Default, MatFx, Skin, SkinMatFx, PTank };
    void begin_object(Cpu& cpu, Pipeline pipeline);
    void color_state(Cpu& cpu, native::Op op);
    void invalidate_channel_aliases(native::Op op, u32 channel);

    // `needs_reupload` is a SEPARATE flag and not a reset of
    // `uploaded_generation`, deliberately. `Op::ReleaseGeometry` is emitted
    // only when `uploaded_generation != 0`, so clearing that word to force a
    // re-send would also suppress the release and leak the GPU resource. The
    // latch keeps meaning "what we last sent"; this means "the renderer did not
    // take it".
    //
    // `retries` bounds the re-send. A display list that cannot be converted
    // fails identically every time, and re-sending it every frame forever is
    // the other way to be wrong here. When the budget is spent the object stays
    // missing - but ATTRIBUTABLY missing, with a reason and a count, which is
    // the whole point. A generation change resets the budget, because new
    // content deserves a fresh attempt.
    //
    // `last_reject_present` makes the budget bound CONSECUTIVE failures rather
    // than lifetime ones. Without it, three refusals spread anywhere across a
    // whole stage would permanently disable retry for an object that had in
    // fact recovered from each one. The budget cannot simply be reset when a
    // re-send happens, because "sent" is not "accepted" - that would reset it
    // on every retry and restore the endless loop. Elapsed presents since the
    // last refusal is the available evidence that the object recovered.
    struct TextureRecord {
        u32 generation = 0;
        u32 uploaded_generation = 0;
        u64 content_hash = 0;
        bool needs_reupload = false;
        u8 retries = 0;
        native::RejectReason last_reject = native::RejectReason::Unknown;
        u64 last_reject_present = 0;
    };
    struct GeometryRecord {
        u32 generation = 0;
        u32 uploaded_generation = 0;
        u16 serial = 0;
        u32 size = 0;
        bool needs_reupload = false;
        u8 retries = 0;
        native::RejectReason last_reject = native::RejectReason::Unknown;
        u64 last_reject_present = 0;
        // Content identity of the current upload (GeometryContent::identity).
        u64 identity = 0;
    };
    u32 next_generation() noexcept { return ++generation_counter_; }
    TextureRecord& texture_record(u32 parent);
    void bump_texture(u32 raster, bool release);
    bool upload_texture(u32 parent, TextureRecord& record, u32 flags);
    u64 hash_texture(u32 parent) const noexcept;

    struct ObjectContext {
        bool valid = false;
        Pipeline pipeline = Pipeline::None;
        u32 object = 0;
        u32 entry = 0;
        u32 header = 0;
        u32 geometry_flags = 0;
        u32 light_mask = 0;
        u32 has_ambient = 0;
        u32 header_flags = 0;
        u32 mesh_count = 0;
        native::ResourceKey geometry{};
    };
    bool prepare_geometry(Cpu& cpu, ObjectContext& context);

    // Takes everything the renderer refused since the last call and clears the
    // corresponding latches so the content is sent again. Called from the
    // guest thread at the top of object preparation - the one place that is
    // both frequent enough to recover quickly and cheap enough that a mutex
    // does not matter, because it only does work when something failed.
    void drain_rejections();
    std::vector<native::ResourceRejection> rejection_scratch_;

    struct PendingBegin {
        bool valid = false;
        u32 return_address = 0;
        native::DrawProducer producer = native::DrawProducer::Unknown;
        native::MaterialProgram program = native::MaterialProgram::PassColor;
        bool clear_quad = false;
        u8 opcode = 0;
        u32 count = 0;
    };

    struct ShadowSlot {
        bool valid = false;
        u8 size = 0;
        std::array<u8, 72> bytes{};
    };
    static constexpr std::size_t kShadowSlots = 64;

    System& system_;
    native::SegmentQueue* queue_ = nullptr;
    std::unique_ptr<native::FrameSegment> segment_;
    std::vector<std::array<ShadowSlot, kShadowSlots>> shadow_;
    std::unordered_map<u32, TextureRecord> textures_;
    std::unordered_map<u32, GeometryRecord> geometry_;
    // identity -> number of live records carrying it (0 = released, still known).
    std::unordered_map<u64, u32> geometry_identity_live_;
    void release_identity(GeometryRecord& record);
    ObjectContext object_{};
    PendingBegin pending_{};
    std::vector<shadow::render::RenderVertex> vertex_scratch_;
    u32 generation_counter_ = 0;
    u32 present_ordinal_ = 0;
    bool validate_textures_ = false;
    bool gx_input_digest_enabled_ = false;
    bool installed_ = false;
    bool cpu_owner_profile_ = false;
    mutable Report report_{};
    // STEP-0 measurement (R4 / COHERE). Null = disabled = one pointer test per
    // prepare_geometry / present / notify / state load.
    std::unique_ptr<CoherenceProbe> coherence_;
};

}  // namespace gekko
