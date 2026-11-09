#pragma once

// LEGACY GX/EFB REPLAY RENDERER (not native; NATIVE_AUDIT.md C15, lies list 15): it models the GameCube EFB
// (640x528 colour/depth targets) and replays the captured GX state; it is not the R2 renderer, and G5 keeps it out
// of the lifted (shipping) build. Originally: Direct3D 12 backend of the native renderer (docs/native-runtime-2026-09-16/
// ARCHITECTURE.md section 3). Owned by NativeRenderer and driven only from its
// renderer thread, except for the output handoff accessors, which are locked.
//
// Resource model:
//   * one device; a DIRECT queue for rendering (the presenter borrows the
//     device and uses its own queue)
//   * persistent GPU objects: EFB colour/depth targets, a bindless texture
//     table, a static sampler table, pipeline state cache, per-resource
//     vertex/index buffers and decoded textures
//   * per-frame upload rings (persistently mapped upload heaps) for constants,
//     immediate vertices/indices, texel staging and dynamic skinned vertices
//   * exact fence lifetime: an object released in frame F is destroyed, and its
//     bindless slot reused, only after frame F's fence completes

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include "gekko/display.h"
#include "gekko/native_frame.h"
#include "gekko/camera.h"
#include "gekko/types.h"

namespace gekko {

struct NativeGpuSettings {
    // The PRESENTATION rectangle this renderer produces for. It no longer
    // sizes anything the renderer allocates: the EFB and every native surface
    // derive from `internal_scale` alone. It is kept because `internal_scale`
    // 0 (auto) is chosen from it, because the Hor+ content aspect and the 2D
    // layout are properties of it, and because the propagation report states
    // it. See docs/NEXT_SESSION_PROMPT_2026-09-21_RESOLUTION.md section 1.
    u32 output_width = 640;
    u32 output_height = 480;
    // The one authoritative internal render scale: integer and ISOTROPIC.
    // The EFB is kGuestEfbWidth*S x kGuestEfbHeight*S (640S x 528S) and the
    // raster the guest programs is 640S x 480S. A non-integer or per-axis
    // factor folds the display aspect into the pixel grid, which is what made
    // the two factors disagree (3.0000 x 2.1021) and what stretched everything
    // living in guest 2D space by their ratio. The aspect belongs at present
    // time (display.cpp fit_content_rect), not in the pixel scale.
    // 0 = derive from the output rectangle with recommended_internal_scale(),
    // which is the default: a caller that says nothing gets the scale the
    // output asks for, not a silent pin to the console's 1x.
    u32 internal_scale = 0;
    // Hor+ reference: content aspect the 3D projection is widened to.
    float content_aspect = 4.0f / 3.0f;

    // Widescreen 2D layout (anchors, HUD scale/inset). A 4:3 layout lays out nothing.
    DisplayLayout hud_layout{};
    // Guest-texel footprint reconstruction for resident EFB copies (the glare
    // ladder). Identity at output 640x480; --no-effect-footprint turns it off
    // for A/B capture on one binary.
    bool effect_footprint = true;
    // Raster-border snap for orthographic draws above internal scale 1. The
    // guest authors screen-aligned 2D in pixel-CENTRE coordinates, so its
    // full-screen quads span [0.5, 640.5] x [0.5, 480.5]; at scale 1 that
    // contains every pixel centre and covers the screen, above it the low
    // edges leave floor(S/2) native sub-samples uncovered. Identity at scale 1
    // by construction; --no-logical-pixel-snap is the A/B control and is the
    // same option the legacy raster takes.
    bool logical_pixel_snap = true;
    // Headless verification: write the presented frame `screenshot_frame`
    // (1-based guest present ordinal) as a binary PPM.
    std::filesystem::path screenshot_path;
    u32 screenshot_frame = 0;
    // Also dump every Nth present from screenshot_frame on (0 = only one).
    u32 screenshot_stride = 0;
    u32 screenshot_count = 1;
    // Diagnostic: read the EFB back after every EFB copy (and at the present)
    // of guest present `copy_dump_frame` into copy_dump_directory. Splits and
    // waits the command list per copy; never on in measurement runs.
    std::filesystem::path copy_dump_directory;
    // Where to write the exact bytes of a display list the geometry
    // converter refused. A refused list makes an object invisible for the
    // rest of the session, and the error string alone cannot say where the
    // walk lost sync - the blob can, decoded offline. Empty = off.
    std::filesystem::path geometry_failure_dump;
    u32 copy_dump_frame = 0;
};

class NativeGpu {
public:
    NativeGpu();
    ~NativeGpu();

    NativeGpu(const NativeGpu&) = delete;
    NativeGpu& operator=(const NativeGpu&) = delete;

    // True when this build carries the Direct3D 12 backend.
    static bool available() noexcept;

    bool create(const NativeGpuSettings& settings, std::string& error);
    void destroy();

    // Renderer thread. Fence failure throws a native retirement error before
    // any in-flight arena/list reuse. The renderer owns its terminal channel.
    void process(const native::FrameSegment& segment);
    // Owning thread after worker join, or renderer thread: waits for every
    // submitted frame; throws on failed retirement, never claims quiescence.
    void finish();

    // Any thread (the window loop): the presented surface changed size. The
    // renderer applies it at the next present, so a frame is never split
    // across two resolutions: the EFB backing, the Hor+ aspect and the 2D
    // layout change together. Repeating the current size is free.
    // `internal_scale` 0 re-derives the scale from the new rectangle; a
    // non-zero value pins it, so an explicit --internal-scale survives every
    // resize.
    void request_output_size(u32 width, u32 height, const DisplayLayout& layout,
                             u32 internal_scale);

    // Any thread (the development link, docs/comparator/README.md): write the
    // first guest present at or after `frame` (the 1-based present ordinal of
    // NativeGpuSettings::screenshot_frame) as a binary PPM to `path`, replacing
    // a pending request. The readback --native-screenshot makes.
    void request_screenshot(const std::filesystem::path& path, u32 frame);
    // The same for the first present at or after game tick `game_tick` (the clock both builds share).
    void request_screenshot_at_tick(const std::filesystem::path& path, u64 game_tick);
    struct ScreenshotResult {
        u64 written = 0;          // requests served by this process
        u32 frame = 0;            // the newest one's present ordinal
        u64 game_ticks = 0;       // ... and its present's game tick
        std::filesystem::path path;
        bool pending = false;     // a request not served yet
    };
    ScreenshotResult screenshot_result() const;

    // Presentation handoff (any thread).
    void* device() const noexcept;
    struct OutputFrame {
        void* texture = nullptr;
        void* ready_semaphore = nullptr;
        u32 width = 0;
        u32 height = 0;
        u64 generation = 0;
        u32 guest_frame = 0;
        u32 slot = 0;
    };
    bool latest_output(OutputFrame& frame) const;
    // Presenter side: a DIRECT queue on the same device used only for presents,
    // the newest published generation, and pinned acquisition of that frame.
    // Every successful acquire_output must be matched by one release_output.
    void* present_queue() const noexcept;
    u64 output_generation() const;
    bool acquire_output(OutputFrame& frame);
    // Pinned acquisition of one specific generation. The two newest
    // generations are always retained, so arrival-order presentation can show
    // the older of two frames that completed inside one display interval.
    bool acquire_output(u64 generation, OutputFrame& frame);
    // When the guest produced a retained generation (steady_clock ns since its
    // epoch). Presentation policy keys on this, not on when the renderer
    // happened to publish the frame.
    bool output_produced_ns(u64 generation, u64& produced_ns) const;
    void release_output(u32 slot, u64 generation);

    enum class RingUploadOwner : u8 {
        Texture,
        GeometryVertex,
        GeometryIndex,
        DynamicVertex,
        LightConstants,
        DrawConstants,
        ImmediateVertex,
        ImmediateIndex,
        Count
    };
    enum class CpuPhase : u32 {
        StateApply, PrepareDraw, Constants, ProjectionPolicy, PipelineLookup,
        DescriptorLookup, ImmediateDraw, ImmediateLayout, ImmediatePackUpload,
        MeshDraw, GeometryLookup, GeometryConvert, GeometryDynamic, BindVertexIndex,
        TextureUpload, Copy, Present, ListBegin, ListSubmit, FenceWait, ResourceUpload,
        ListClose, TransferSubmit, QueueSubmit,
        SampleConstantsTransformProjection, SampleConstantsTexgen,
        SampleConstantsColorConversion, SampleConstantsFogCoefficients,
        SampleConstantsResolvedTexturesAlpha, SampleConstantsCompareUploadAttribution,
        UploadReserve, VertexBindEmit, IndexBindEmit, DrawEmit, LightUpload,
        Count
    };
    // A fixed deterministic census of actual constant materializations only.
    // Sample timings are observations, not estimates of unsampled work.
    static constexpr u64 kCpuConstantSampleStride = 64u;
    struct CpuPhaseCounter {
        u64 calls = 0;
        u64 ns = 0;
        u64 max_ns = 0;
    };
    struct Stats {
        // Opt-in render-thread CPU timings. Phases are inclusive and overlap.
        bool cpu_profile_enabled = false;
        std::array<CpuPhaseCounter, static_cast<std::size_t>(CpuPhase::Count)> cpu_phases{};
        u64 cpu_profile_clock_pair_ns = 0;
        u64 cpu_profile_clock_pair_calls = 0;
        u64 cpu_projection_key_hits = 0;
        u64 cpu_projection_key_misses = 0;
        u64 cpu_projection_policy_non_neutral = 0;
        // Profile-only census: helper calls are not actual driver emissions.
        // VB/IB attempts and reuse are separate; PSO/viewport/scissor count
        // only native draw preparation emissions, not present/copy setup.
        std::array<u64, 2> cpu_bind_attempts{}, cpu_bind_reuses{};
        std::array<u64, 3> cpu_draw_state_emits{};
        struct QueueCpuCounter {
            u64 calls = 0, ns = 0, max_ns = 0, failures = 0;
            u32 last_failure_hresult = 0;
        };
        // D3D12 default graphics queue: total plus six nested owner stages.
        bool queue_cpu_profile_enabled = false;
        std::array<QueueCpuCounter, 7> queue_cpu_phases{};
        u64 frames = 0;
        u64 draws = 0;
        // Triangles actually submitted, counted from the index count of every
        // TRIANGLE_LIST draw. Lines and points are excluded rather than divided
        // by three, so this is a count and not an estimate; the legacy Flipper
        // reported one of these and the native renderer reported none, which
        // left --telemetry writing a zero that looked like a measurement.
        u64 triangles = 0;
        u64 immediate_draws = 0;
        u64 mesh_draws = 0;
        u64 skipped_draws = 0;
        u64 unsupported_program_draws = 0;
        u64 missing_textures = 0;
        // A DRAW THAT WAS ASKED FOR AND DID NOT HAPPEN. Every one of these is a
        // renderable element that disappeared, so the total is useless on its
        // own - it aggregated five unrelated causes and could not attribute a
        // single one. The breakdown sums to missing_geometry exactly.
        u64 missing_geometry = 0;
        u64 missing_geometry_unknown = 0;       // no entry at that address
        u64 missing_geometry_generation = 0;    // entry exists, generation moved on
        u64 missing_geometry_mesh_index = 0;    // mesh index past the converted set
        u64 missing_geometry_dynamic_stale = 0; // skinned entry not updated this frame
        // A DYNAMIC (CPU-skinned) UPDATE THAT WAS SENT AND DID NOT LAND. Each
        // one leaves its entry unbound for the frame, and then every mesh draw
        // referencing it is dropped as missing_geometry_dynamic_stale. All four
        // were silent returns: the capture's `dynamic` count and the GPU's
        // `dynamic_updates` count diverged with nothing to say why.
        u64 dynamic_drop_unknown = 0;    // no entry / generation moved on
        u64 dynamic_drop_no_cpu = 0;     // entry carries no CPU copy to re-skin
        u64 dynamic_drop_expand = 0;     // rw_expand_dynamic_pos_nrm refused the payload
        u64 dynamic_drop_staging = 0;    // upload ring could not stage it

        // The first few drops individually. A total says something disappeared;
        // this says WHICH object, in WHICH frame, and what the two generations
        // were - the difference between a defect that can be chased and a
        // number that can only be stared at.
        struct GeometryDrop {
            const char* cause = nullptr;  // static literal, never owned
            u32 address = 0;
            u32 wanted = 0;   // generation the draw or update asked for
            u32 held = 0;     // generation the entry actually holds
            u32 detail = 0;   // mesh index, vertex count or byte size, per cause
            u32 frame = 0;
        };
        static constexpr unsigned kGeometryDropRecords = 24;
        GeometryDrop geometry_drops[kGeometryDropRecords]{};
        u64 geometry_drops_seen = 0;

        // THE PREFIX RING ABOVE ANSWERS THE WRONG QUESTION. It records what
        // went wrong FIRST, which cannot distinguish "17 objects lost forever"
        // from "1,538 objects lost for one frame each" - and those two demand
        // completely different fixes. The 2026-09 Gun Fortress investigation
        // had to reach that answer by arithmetic on two unrelated totals
        // because no structure in the renderer held it.
        //
        // This is that structure: one entry per DISTINCT object address, with
        // the frame span it was dropped over. A permanent loss is a handful of
        // addresses whose last_frame is the final frame. A transient is many
        // addresses with small counts. Bounded, so a pathological run cannot
        // grow it without limit; `geometry_drop_objects_overflow` says when
        // the bound bit, because a silently truncated table is exactly the
        // failure mode this replaces.
        struct GeometryDropObject {
            u32 address = 0;
            const char* first_cause = nullptr;  // static literal, never owned
            const char* last_cause = nullptr;
            u32 first_frame = 0;
            u32 last_frame = 0;
            u32 wanted = 0;  // generation most recently asked for
            u32 held = 0;    // generation the entry most recently held
            u64 count = 0;
        };
        static constexpr unsigned kGeometryDropObjects = 256;
        GeometryDropObject geometry_drop_objects[kGeometryDropObjects]{};
        u32 geometry_drop_objects_used = 0;
        u64 geometry_drop_objects_overflow = 0;

        // A DRAW THAT VANISHED WITH NO COUNTER AT ALL. Each of these was a
        // bare `return` on a path that had already decided a renderable
        // element existed. They are listed in the order they appear along the
        // draw path so the report reads as a funnel.
        //
        // The immediate lane is the one the directive names explicitly (HUD,
        // text, particles, effects) and it carried ~450k draws in a single
        // reported session with not one attributable outcome.
        u64 immediate_drop_zero_count = 0;     // reached the renderer with no vertices
        u64 immediate_drop_degenerate = 0;     // non-TRIANGLES batch whose index expansion produced nothing
        u64 immediate_drop_no_pipeline = 0;    // prepare_draw refused (PSO creation failed)
        u64 immediate_drop_vertex_ring = 0;    // upload ring could not stage the vertices
        // WORSE THAN A DROP: bind_vertices has already run, so the command
        // list is left holding this draw's vertex buffer against the previous
        // draw's index buffer. Counted separately because the recovery is not
        // the same one.
        u64 immediate_drop_index_ring = 0;
        // A mesh whose display list converted to zero triangles. This was the
        // last uninstrumented exit in draw_mesh, sitting between five
        // attributed causes above it and one below.
        u64 mesh_drop_zero_indices = 0;
        u64 mesh_drop_no_pipeline = 0;

        // skipped_draws MERGED TWO OPPOSITE MEANINGS. GX_CULL_ALL is the game
        // intentionally drawing nothing and must stay at whatever it is; a
        // null pipeline is a draw the renderer lost. These two sum to
        // skipped_draws.
        u64 skipped_cull_all = 0;
        u64 skipped_null_pipeline = 0;
        // Pipelines the device refused (the factory returned null). Every draw
        // of such a key lands in skipped_null_pipeline.
        u64 pipeline_create_failures = 0;

        // ring_bound_failures was the only trace of four unrelated refusals
        // across texture staging, immediate draws, dynamic skinning and
        // geometry. These sum to it.
        u64 ring_fail_grow = 0;
        u64 ring_fail_allocate_oversize = 0;
        u64 ring_fail_reserve_oversize = 0;

        // A geometry whose buffers the device refused, and one whose fallback
        // staging ran out. The second is not a clean miss: index_bytes is
        // assigned after that return, so the entry keeps the PREVIOUS
        // geometry's index extent against NEW buffers.
        u64 geometry_drop_buffer_create = 0;
        u64 geometry_drop_vertex_staging = 0;
        u64 geometry_drop_index_staging = 0;

        // texture_decode_failures merged "nothing existed, magenta is honest"
        // with "a working resident texture was thrown away".
        u64 texture_pinned_missing_new = 0;    // first content for this raster failed
        u64 texture_pinned_missing_reuse = 0;  // a live texture was replaced by magenta
        // A camera-target bind that found no resident target and fell back to
        // the OPAQUE WHITE sentinel. Under the guest's SUBTRACT glare blend
        // that is not a neutral substitute - it blacks the scene out.
        u64 camera_target_miss_white = 0;

        // Resources the device refused on the present path. Unlike create_efb
        // and on_texture_content, these had no usable() check at all, so the
        // failure was a null dereference or a frame that silently never
        // appeared.
        u64 present_output_texture_failures = 0;
        u64 present_output_semaphore_failures = 0;
        u64 efb_resize_refused = 0;

        // Ops the capture emits and apply_state's `default:` arm swallows.
        // Op::DestinationAlpha and Op::TevKonstColor are both emitted by
        // rw_capture and appear nowhere in this file; Op::TevRegisterColor by
        // contrast IS consumed, so this is two specific channels rather than a
        // whole family. Counted here rather than fixed, because dest-alpha and
        // TEV konstants are appearance and the replacement is a decision, not
        // a mechanical port.
        u64 state_ops_unconsumed = 0;
        u32 state_op_unconsumed_first = 0;  // first op id seen, for attribution

        // Slot fences found already retired when a slot was reused (the fast
        // path of wait_slot; `fence_waits` counts the ones that blocked).
        u64 slot_fence_already_complete = 0;

        // THE TRANSFER QUEUE (render::TransferQueue): uploads into fresh
        // resources on the COPY queue, through the RHI. `graphics_waits_on_copy`
        // is COPY semaphores attached to GRAPHICS submissions.
        u64 copy_queue_submits = 0;
        u64 copy_queue_uploads = 0;
        u64 copy_queue_generated_uploads = 0;
        u64 copy_queue_generated_bytes = 0;
        u64 copy_queue_bytes = 0;
        u64 copy_queue_staging_bytes = 0;     // including row and placement padding
        u64 copy_queue_retained_bytes = 0;    // mapped staging retained by all batches
        u64 copy_queue_peak_batch_bytes = 0;  // largest live prefix in one batch
        u64 copy_queue_largest_request_bytes = 0;
        u64 copy_queue_growths = 0;
        u64 copy_queue_allocation_failures = 0;
        u64 copy_queue_current_bytes = 0;
        u64 copy_queue_resets = 0;
        u64 copy_queue_chunk_switches = 0;
        u64 copy_queue_owner_uploads[4]{};
        u64 copy_queue_owner_bytes[4]{};
        u64 copy_queue_owner_staging_bytes[4]{};
        u64 copy_queue_early_submits = 0;     // a batch filled mid-frame
        u64 copy_queue_batch_waits = 0;       // reuses of a submitted batch (each waits once)
        u64 copy_queue_batch_wait_ns = 0;     // time in those waits: non-zero = real blocking
        u64 copy_queue_refused_oversize = 0;  // staged on the GRAPHICS list instead
        u64 copy_queue_refused_arguments = 0; // zero-byte (e.g. empty index list)
        u64 copy_queue_semaphores = 0;
        u64 copy_queue_semaphore_failures = 0;
        u64 graphics_waits_on_copy = 0;

        // Refusals handed back to the capture so it can re-send. Every one of
        // these used to be an object silently deleted for the rest of the
        // session.
        u64 rejections_posted = 0;
        u64 rejections_geometry = 0;
        u64 rejections_texture = 0;
        // A refusal with nowhere to go: no mailbox wired. Not recoverable.
        u64 rejections_undeliverable = 0;
        u64 clears = 0;
        u64 copies = 0;
        // GXSetTexCopyDst's mipmap argument (bit 9 of the copy-execute word):
        // the console applies a 2x box on the way out. Never observed on any
        // measured route; it is counted AND reported with the guest address
        // rather than dropped quietly, because a copy that does nothing is
        // indistinguishable from a copy that worked.
        u64 half_resolution_copies_unsupported = 0;
        // EFB copies whose rectangle did not survive clamping, and copies into
        // a target the device refused to allocate. Both used to return with no
        // trace at all.
        u64 copies_rejected_geometry = 0;
        u64 copies_missing_target = 0;
        // GXSetScissorBoxOffset with a non-zero origin. GX subtracts it from
        // every vertex before the scissor test, which is how a virtual frame
        // buffer larger than the EFB is rendered in tiles. This game programs
        // (0, 0) on every measured route, so the op is identity - but it was
        // being dropped by a default case, which makes an unhandled op
        // indistinguishable from a handled one.
        u64 scissor_offsets_unsupported = 0;
        // Bloom ("glare") pass boundaries seen, and how many of them the guest's
        // own gate opened. The parameter block is read live at every boundary
        // (it has a temporal channel), so `glare_parameter_changes` counts how
        // often it actually moved - on every shipped stage it should be 1, the
        // first observation, because the values are executable constants.
        u64 glare_windows = 0;
        u64 glare_windows_gated_on = 0;
        u64 glare_parameter_changes = 0;
        // The most recent parameter block, for the exit report.
        native::GlareWindow glare_parameters{};
        u64 pipelines = 0;
        u64 pipeline_create_ns = 0;
        u64 textures_live = 0;
        u64 textures_created = 0;
        u64 texture_decode_failures = 0;
        u64 texture_bytes = 0;
        u64 camera_targets = 0;
        // Guest instance-cache entries (RwResEntry) currently bound to geometry.
        u64 geometry_live = 0;
        // PERSISTENT GEOMETRY STORE. `geometry_created` counts ASSETS - one
        // conversion, one buffer allocation, one upload each. An instance whose
        // content is already resident binds it instead (`geometry_asset_hits`):
        // no conversion, no allocation, no upload, no release. Identity is
        // proven by comparing every byte the conversion consumed, so a hash
        // match with different bytes is a new asset, counted as a conflict.
        u64 geometry_created = 0;
        u64 geometry_asset_hits = 0;
        u64 geometry_identity_conflicts = 0;
        u64 geometry_assets_resident = 0;
        u64 geometry_assets_unreferenced = 0;
        u64 geometry_asset_bytes = 0;       // GPU bytes of resident assets
        u64 geometry_source_bytes = 0;      // CPU bytes kept to prove identity
        u64 geometry_assets_evicted = 0;    // unreferenced assets released (budget)
        u64 geometry_convert_failures = 0;
        u64 geometry_foreign_commands = 0;
        u64 dynamic_updates = 0;
        u64 constant_uploads = 0;
        u64 constant_reuses = 0;
        u64 constant_identity_reuses = 0;  // avoided materialization through the semantic key
        u64 constant_rebuild_reuses = 0;   // materialized, then found byte-identical
        u64 constant_materializations = 0;
        u64 constant_restored_reuses = 0;
        u64 constant_restore_checks = 0;
        u64 constant_restore_mismatches = 0;
        std::array<u64, static_cast<std::size_t>(native::Op::Count)>
            constant_rebuild_dirty_ops{};
        u64 constant_rebuild_no_state = 0;
        // Fixed-size generation attribution. Seen opcodes above are annotations,
        // not evidence that they invalidated DrawConstants.
        std::array<u64, static_cast<std::size_t>(native::Op::Count)>
            constant_generation_changes{};
        std::array<u64, static_cast<std::size_t>(native::Op::Count)>
            constant_rebuild_changed_ops{};
        std::array<u64, static_cast<std::size_t>(native::Op::Count)>
            constant_rebuild_single_changed_ops{};
        u64 constant_rebuild_no_changed_state = 0;
        u64 constant_rebuild_non_state_changed = 0;
        u64 constant_rebuild_projection_source_restored = 0;
        u64 constant_rebuild_projection_source_distinct = 0;
        u64 constant_rebuild_fog_source_restored = 0;
        u64 constant_rebuild_fog_source_distinct = 0;
        u64 upload_bytes = 0;
        u64 cpu_staging_copy_bytes_avoided = 0;
        u64 cpu_dynamic_scratch_bytes_avoided = 0;
        u64 cpu_static_geometry_copy_bytes_avoided = 0;
        u64 cpu_index_staging_copy_bytes_avoided = 0;
        // Real native-linear arrivals and the intermediate CPU scratch copy.
        // Keep demand separate from executed copies so a bypass can be measured
        // without reporting arrival bytes as work that still takes place.
        u64 native_linear_texture_payloads = 0;
        u64 native_linear_texture_bytes = 0;
        u64 native_linear_texture_scratch_copies = 0;
        u64 native_linear_texture_scratch_bytes = 0;
        u64 native_linear_texture_direct_payloads = 0;
        u64 native_linear_texture_direct_bytes = 0;
        // Fixed-size topology census for choosing persistent immediate-index
        // residency from live demand without allocating a diagnostic map.
        std::array<u64, 8> immediate_primitive_draws{};
        std::array<u64, 8> immediate_indexed_draws{};
        std::array<u64, 8> immediate_index_bytes{};
        std::array<u64, 8> immediate_max_vertices{};
        u64 immediate_index_pattern_hits = 0;
        u64 immediate_index_pattern_growths = 0;
        u64 immediate_index_pattern_bytes = 0;
        u64 immediate_index_pattern_create_failures = 0;
        u64 immediate_index_pattern_upload_failures = 0;
        u64 immediate_index_upload_bytes_avoided = 0;
        u64 immediate_index_generation_bytes_avoided = 0;
        u64 ring_retained_bytes = 0;  // mapped per-slot upload capacity
        u64 ring_peak_bytes = 0;      // largest submitted prefix of one slot
        u64 ring_largest_request_bytes = 0;
        u64 ring_growths = 0;       // retired-slot capacity changes, including first allocation
        u64 ring_allocation_failures = 0;
        u64 ring_current_bytes = 0;
        u64 ring_resets = 0;
        u64 ring_chunk_switches = 0;
        u64 ring_owner_allocations[static_cast<std::size_t>(RingUploadOwner::Count)]{};
        u64 ring_owner_bytes[static_cast<std::size_t>(RingUploadOwner::Count)]{};
        u64 ring_flushes = 0;
        u64 ring_bound_failures = 0;
        u64 deferred_releases = 0;
        u64 fence_waits = 0;
        u64 fence_wait_ns = 0;
        u64 fence_already_complete = 0;
        // Waits that ended without the fence retiring: a lost device or a
        // submission that never came back. The slot stays in flight.
        u64 fence_wait_failures = 0;
        // Render targets the device refused. Non-zero means the renderer is
        // running degraded, not that a frame merely looked wrong.
        u64 target_allocation_failures = 0;
        // Draws that took the guest-texel footprint reconstruction. Zero at
        // internal scale 1 by construction; non-zero above it means the glare
        // ladder is being reconstructed instead of point-sampled into a comb.
        u64 footprint_draws = 0;
        // Orthographic draws whose vertices were snapped to the raster border,
        // and draws where the snap refused because the guest's own position
        // matrix could not carry the correction without moving the draw in
        // eye space. Both are zero at internal scale 1.
        u64 border_snap_draws = 0;
        u64 border_snap_refused = 0;
        // Transitions the renderer asked for, and the ones the RHI backend
        // actually recorded (it elides against the state it tracks per
        // resource, so only it knows the second number).
        u64 barrier_requests = 0;
        u64 barriers = 0;
        u64 pso_hits = 0;
        u64 pso_misses = 0;
        u64 record_ns = 0;
        u64 record_ns_max = 0;
        u64 gpu_frame_ns = 0;   // timestamp-query GPU time, summed
        u64 gpu_frame_ns_max = 0;
        u64 gpu_frames_timed = 0;
        u64 screenshots = 0;
        u64 vram_usage_bytes = 0;
        u64 vram_budget_bytes = 0;
        u64 bindless_high_water = 0;
        u64 line_draws = 0;
        u64 point_draws = 0;
        u64 depth_before_texture_with_alpha_test = 0;
        u64 logic_op_draws = 0;
        // Widescreen 2D layout of orthographic immediate draws.
        u64 hud_laid_out = 0;
        u64 hud_skip_world_anchored = 0;             // classified into an anchor
        u64 hud_remapped = 0;             // ... whose affine was not the identity
        u64 hud_off_canvas = 0;           // parked outside the canvas, left in place
        u64 hud_skip_before_copy = 0;     // drawn into a camera texture, not the picture
        u64 hud_skip_effect_texture = 0;  // samples a camera texture (blur, glare)
        u64 hud_skip_small_scissor = 0;   // effect downsample region
        u64 hud_groups = 0;
        u64 hud_scenes = 0;
        // Output rectangles accepted (the Hor+ aspect and the 2D layout
        // followed one) and, separately, the EFB rebuilds those caused. They
        // are no longer the same event: the EFB is a property of the integer
        // internal scale, so most window drags change the first and not the
        // second.
        u64 output_resizes = 0;
        u64 efb_rebuilds = 0;
        u64 output_slots = 0;
        u64 output_slots_created = 0;
        u64 output_pins = 0;
        u64 output_pins_peak = 0;
        u64 output_pool_bytes = 0;
        u64 state_resets = 0;
        // Guest present -> output published (render thread recorded and submitted it).
        u64 publish_latency_ns = 0;
        u64 publish_latency_ns_max = 0;
        u64 publish_latency_samples = 0;
        std::array<u64, 64> publish_latency_histogram{};  // 0.5 ms buckets
        u32 output_width = 0;
        u32 output_height = 0;
        // The internal scale actually in force and the EFB it produced.
        u32 internal_scale = 1;
        u32 efb_width = 0;
        u32 efb_height = 0;
        std::string last_error;
    };
    Stats stats() const;

    // The reverse half of the resource contract. Without it this renderer can
    // refuse a geometry or a texture and the capture will never learn, never
    // re-send, and the object is gone for the session. Set once at start; may
    // be null, in which case refusals are still counted but not recoverable.
    void set_rejections(native::RejectionMailbox* mailbox);

    // Every render target this renderer owns, with the guest dimensions it
    // was derived from and the native dimensions it actually got. This is
    // the measurement the resolution architecture has to be designed from:
    // which surfaces scale with the output, which are pinned, and by what
    // factor each one was scaled. Empty on a build without plume.
    std::string target_inventory() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace gekko
