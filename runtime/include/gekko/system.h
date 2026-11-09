#pragma once

// The assembled GameCube machine: CPU, memory, hardware and the boot environment
// the IPL would normally leave behind before jumping into the game DOL.

#include <array>
#include <unordered_map>
#include <atomic>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "gekko/asset_mount.h"
#include "gekko/camera.h"
#include "gekko/cpu.h"
#include "gekko/dynamic_cast_intrinsic.h"
#include "gekko/chara_colli_intrinsic.h"
#include "gekko/matrix_multiply_intrinsic.h"
#include "gekko/gameplay_scheduler.h"
#include "gekko/gx.h"
#include "gekko/gx_sites.h"
#include "gekko/health_bar.h"
#include "gekko/hud_listener.h"
#include "gekko/hw/types.h"
#include "gekko/guest_memory.h"
#include "gekko/native_audio.h"
#include "gekko/native_card.h"
#include "gekko/save_store.h"
#include "gekko/native_file_api.h"
#include "gekko/native_frame_limiter.h"
#include "gekko/native_attached_effects.h"
#include "gekko/native_motion_store.h"
#include "gekko/native_sound_banks.h"
#include "gekko/native_stream_ring.h"
#include "gekko/native_object_range.h"
#include "gekko/native_gx_sync.h"
#if !defined(SHADOW_RENDER_LIFTED)  // G5
#include "gekko/native_immediate.h"
#endif
#include "gekko/native_interrupts.h"
#include "gekko/native_ipl.h"
#include "gekko/lifted.h"
#include "gekko/native_reset.h"
#include "gekko/native_boot.h"
#include "gekko/native_disc_error.h"
#include "gekko/native_link_groups.h"
#include "gekko/native_world_collision.h"
#include "gekko/native_world_line.h"
#include "gekko/prs_ahead.h"
#include "gekko/native_directory_scan.h"
#include "gekko/native_cri_file_device.h"
#include "gekko/native_rtgcnfs.h"
#include "gekko/native_jobs.h"
#include "gekko/native_expand_jobs.h"
#include "gekko/native_one.h"
#include "gekko/native_cri.h"
#include "gekko/game_errata.h"
#include "gekko/native_adx_output.h"
#include "gekko/native_sj.h"
#include "gekko/native_svm.h"
#include "gekko/game_memory.h"
#include "gekko/native_video.h"
#include "gekko/types.h"
#include "native_display.h"
#include "native_audio_mixer.h"
#include "gekko/native_reverb_owner.h"
#include "gekko/native_ax_registration.h"
#include "gekko/native_ax_voice.h"
#include "gekko/native_ax_frame.h"
#include "gekko/native_ax_sync.h"
#include "gekko/ax_pb_image.h"
#include "gekko/native_gx_state.h"
#include "gekko/native_rw_raster.h"
#include "gekko/native_os_settings.h"
#include "gekko/native_os_report.h"
#include "gekko/native_os_heap.h"
#include "gekko/native_os_panic.h"
#include "gekko/native_os_misc.h"
#include "gekko/native_os_machine_check.h"
#include "gekko/native_ax_mix_alloc.h"
#include "gekko/native_math_affine.h"
#include "gekko/native_rw_image.h"
#include "gekko/native_rw_image.h"
#include "gekko/native_rw_pixel.h"
#include "gekko/native_os_threads.h"
#include "gekko/native_os_font.h"
#include "gekko/native_rw_vector.h"
#include "gekko/native_rw_matrix.h"
#include "gekko/native_rw_view_cache.h"
#include "gekko/native_vec_math.h"
#include "gekko/native_mix_entries.h"
#include "gekko/native_sound_tables.h"
#include "gekko/native_gcax_midi.h"
#include "gekko/native_se_manager.h"
#include "gekko/native_options_display.h"
#include "gekko/native_options_menu.h"
#include "gekko/button_glyphs.h"

namespace gekko {

class RwCapture;

struct DolSectionInfo {
    u32 file_offset = 0;
    u32 address = 0;
    u32 size = 0;
    bool executable = false;
};

struct DolProgram {
    std::vector<u8> bytes;
    std::vector<DolSectionInfo> sections;
    u32 bss_address = 0;
    u32 bss_size = 0;
    u32 entry_point = 0;
};

struct AxVoiceAdvanceResult {
    bool valid = false;
    bool running = false;
    bool looped = false;
    u32 next = 0;
};

AxVoiceAdvanceResult advance_ax_voice_position(u32 current, u32 end_address,
                                               u32 loop_address, bool looping,
                                               u64 units) noexcept;

// Host-side ADPCM decode cursor for one AX voice (see System::run_ax_mix).
// The DSP decodes each voice's ADPCM stream once, in order, carrying the
// two-sample predictor history with it; this is the host's copy of that
// state. `nibble` is the next ARAM nibble to decode, `decoded` counts the
// samples produced since the cursor was last (re)established and doubles as
// the write index into the voice's decoded-sample ring. Guest thread only.
struct AxAdpcmVoiceCursor {
    u32 nibble = 0;  // guest nibble address (ARAM); stays 32-bit
    u32 predictor = 0;   // coefficient pair from the current frame header
    u32 scale = 0;       // shift from the current frame header
    s32 yn1 = 0;
    s32 yn2 = 0;
    hw::AudioFrameIndex64 decoded = 0;  // host samples produced; ring write index
    bool valid = false;
};

struct BootConfiguration {
    std::vector<u8> disc_header;  // first 0x20 bytes of boot.bin
    std::vector<u8> bi2;          // 0x2000 bytes of bi2.bin
    bool pal = true;
    u32 console_type = 0x10000006u;
};

class System {
public:
    System();
    ~System();

    Cpu& cpu() noexcept { return cpu_; }
    GuestMemory& guest_memory() noexcept { return memory_; }
    // The native platform layers (docs/native-time-20261005/DESIGN.md).
    NativeInterrupts& interrupts() noexcept { return *interrupts_; }
    NativeVideo& video() noexcept { return *video_; }
    const NativeVideo& video() const noexcept { return *video_; }
    // The lifted subsystems (gekko/lifted.h; docs/lift-20261006): native code the translated game
    // reaches through lowered call sites.
    Lifted& lifted() noexcept { return lifted_; }
    shadow::peripheral::ManagerImpPc& peripheral() noexcept { return lifted_.peripheral; }
    const shadow::peripheral::ManagerImpPc& peripheral() const noexcept { return lifted_.peripheral; }
    NativeAudio& audio() noexcept { return *audio_; }
    NativeFrameLimiter& frame_limiter() noexcept { return *frame_limiter_; }
    NativeObjectRange& object_range() noexcept { return *object_range_; }
    NativeMotionStore& motion_store() noexcept { return *motion_store_; }
    NativeSoundBanks& sound_banks() noexcept { return *sound_banks_; }
    NativeStreamRing& stream_ring() noexcept { return *stream_ring_; }
    NativeAttachedEffects& attached_effects() noexcept { return *attached_effects_; }
    NativeGxSync& gx_sync() noexcept { return *gx_sync_; }
    const NativeGxSync& gx_sync() const noexcept { return *gx_sync_; }
    // The memory card and the OS's other external-interface clients (stage 5,
    // docs/native-time-20261005/STAGE5_EXI.md).
    NativeCard& card() noexcept { return *card_; }
    const NativeCard& card() const noexcept { return *card_; }
    // The game's background loaders as native jobs (docs/NATIVE_THREADS.md stage A).
    NativeJobs& native_jobs() noexcept { return *native_jobs_; }
    const NativeExpandJobs* native_expand_jobs() const noexcept { return native_expand_jobs_.get(); }
    NativeExpandJobs& expand_jobs() noexcept { return *native_expand_jobs_; }
    const NativeOne* native_one() const noexcept { return native_one_.get(); }
    const NativeRtGcnFs* native_rtgcnfs() const noexcept { return rtgcnfs_.get(); }
    // CRI's server threads as native calls at their events (docs/NATIVE_THREADS.md stage B).
    NativeCri& native_cri() noexcept { return *native_cri_; }
    NativeSvm& native_svm() noexcept { return *native_svm_; }
    NativeSj& native_sj() noexcept { return *native_sj_; }
    NativeAdxOutput& native_adx_output() noexcept { return *native_adx_output_; }
    const errata::Stats& errata_stats() const noexcept { return errata_stats_; }
    bool errata_installed() const noexcept { return errata_installed_; }
    NativeIpl& ipl() noexcept { return *ipl_; }
    const NativeIpl& ipl() const noexcept { return *ipl_; }
    // The host side of OSReport (gekko/native_os_report.h): the lines the game printed to the debug console.
    os_report::DebugConsole& debug_console() noexcept { return debug_console_; }
    const os_report::DebugConsole& debug_console() const noexcept { return debug_console_; }
    // The console's reset and boot controls (stage 6,
    // docs/native-time-20261005/STAGE6_PI_MI.md).
    NativeReset& reset_controls() noexcept { return *reset_controls_; }
    GameMemory& game_memory() noexcept { return *game_memory_; }
    const NativeReset& reset_controls() const noexcept { return *reset_controls_; }
    NativeBoot& boot() noexcept { return *boot_; }
    const NativeBoot& boot() const noexcept { return *boot_; }
    // GXBegin's primitives from the lowered GX sites (phase 4,
    // docs/native-time-20261005/GX_DATA_PLANE.md).
#if !defined(SHADOW_RENDER_LIFTED)  // G5
    NativeImmediate& immediate() noexcept { return *immediate_; }
    const NativeImmediate& immediate() const noexcept { return *immediate_; }
#else
    // G5 (HW_REPORT T.1): the lifted build has no GX immediate path (its pipe sites are dead and fail closed). Its
    // savestate block (NativeImmediate's, version 19) is the idle builder's - all a lifted build ever has - written
    // as such and read back only as such, so a lifted state is byte-identical.
    static void save_idle_immediate_state(std::vector<u8>& out);
    static bool load_idle_immediate_state(const u8*& cursor, const u8* end);
#endif
    // The generated GX site table (recomp::kGxSites, gekko/gx_sites.h): what
    // the interpreter applies to the listed pipe stores. Install before the
    // guest runs; an executable without generated code has none.
    void install_gx_sites(const GxSiteRecord* sites, std::size_t count) noexcept;
    // The generated platform-site table (recomp::kPlatformSites,
    // gekko/platform_sites.h): what the interpreter applies to the listed
    // lhz / sth words. Install before the guest runs.
    void install_platform_sites(const PlatformSiteRecord* sites, std::size_t count) noexcept {
        cpu_.set_platform_site_table(sites, count);
    }
    // The generated lowered-call-site table (recomp::kLiftSites, gekko/lift_sites.h): what the
    // dispatcher and the interpreter run at a lowered site. Install before the guest runs.
    void install_lift_sites(const LiftSiteRecord* sites, std::size_t count) noexcept {
        cpu_.set_lift_sites(sites, count);
    }
    shadow::display::State& display() noexcept { return display_; }
    // The aspect the game LOGIC's visibility test uses (gekko/gameplay_aspect.h): ElecCoreWall's animation start is the
    // one frustum test whose verdict changes object state. 0 = the display's (live play). Game thread.
    void set_logic_aspect(float aspect) noexcept { logic_aspect_ = aspect > 0.0f ? aspect : 0.0f; }
    float logic_aspect() const noexcept { return logic_aspect_; }

    // Attach a host file as the memory card in slot A (NativeCard) and bind
    // the console settings to it (NativeIpl). A GCI is imported into a
    // separate writable raw card; no progression state is interpreted or
    // patched by the host. The card model is the contract ORACLE of the save store (the original CARD library runs
    // on it in the reference tests); the game itself always saves through the store (the CARD library's bodies are
    // not compiled), so the shipped program never calls this.
    void set_memory_card_path(std::filesystem::path path,
                              std::filesystem::path gci_working_path = {});
    bool memory_card_present() const noexcept;
    const std::filesystem::path& memory_card_backing_path() const noexcept;
    const std::filesystem::path& memory_card_sram_path() const noexcept;

    // The native save store instead of the memory card (gekko/save_store.h, docs/SAVE_REROUTE_PLAN.md): the game's
    // save is the GCI `save` (created by the game when absent), the console settings persist beside it. Before
    // install(). The game always calls it (shadow_main): the store is its save service. A System without it keeps the
    // card model (set_memory_card_path), which is the contract oracle of the reference tests.
    void set_save_file(std::filesystem::path save);
    bool save_store_active() const noexcept { return save_store_active_; }
    SaveStore& save_store() noexcept { return *save_store_; }
    const SaveStore& save_store() const noexcept { return *save_store_; }

    // Mounts the game's assets from the host tree under `files_root`: the
    // native namespace, one AssetId and guest token per file, the token space
    // (gekko/asset_mount.h). No table is read and none is published.
    bool mount_assets(std::unique_ptr<shadow::runtime::v1::Storage> storage, std::vector<u8> identity,
                      std::string& error);
    // ON (default): the mount serves the English save messages in PC words
    // (gekko/native_save_text.h). OFF: the game's own memory-card text, for
    // before / after comparison. Takes effect at mount_assets.
    void set_pc_save_text(bool enabled) noexcept { pc_save_text_ = enabled; }
    bool pc_save_text() const noexcept { return pc_save_text_; }
    assets::AssetSystem& assets() noexcept { return assets_; }
    const assets::AssetSystem& assets() const noexcept { return assets_; }

    // Installs the native file API: every entry point of the file API the game
    // links against (the Nintendo DVD SDK's) is served from the mounted assets
    // - no drive model: no DI registers, no DVD interrupt, no command queue. The DVD SDK's API contract is still
    // emulated at API level (NATIVE_AUDIT.md C3, lie 14; ES comment fix 2026-10-20): DVDFileInfo and command blocks
    // in guest memory, a synthetic 32-bit token per file standing in for the disc offset, the disc ID block at
    // 0x80000000.
    // Must run after mount_assets and before start.
    void enable_file_api();
    NativeFileApi* file_api() noexcept { return file_api_.get(); }
    PrsAhead* prs_ahead() noexcept { return prs_ahead_.get(); }

    // Installs the native gameplay scheduler: the guest's own task traversal
    // (0x8004ECAC) is replaced by the native walk, while each task's execute
    // and destructor stay translated guest code called through the dispatcher.
    // Must run before start; --legacy-scheduler leaves the guest traversal in
    // place for A/B capture.
    void enable_gameplay_scheduler();
    // Stops the run at the next point where the machine can be captured: at
    // once when no gameplay traversal is open, otherwise when the outermost
    // traversal completes (GameplayScheduler::set_boundary_hook). Under the
    // native retrace the clock advances inside traversals, so a stop at the
    // next host service would land mid-traversal and a savestate would refuse.
    void request_stop_between_traversals(StopReason reason, std::string detail);
    GameplayScheduler* gameplay_scheduler() noexcept { return gameplay_scheduler_.get(); }

    void load_dol(const DolProgram& program);
    void apply_boot_configuration(const BootConfiguration& configuration);
    void start();

    // Runs the game until the machine stops: a host stop request, a fault.
    // Nothing counts instructions; the game clock moves only where the game
    // waits (advance_game_time).
    void run();

    // ---- game time (docs/native-time-20261005/DESIGN.md) ---------------------
    //
    // The one place the game clock moves. Called from a wait point - the
    // game's frame limiter (NativeFrameLimiter) and the OS idle loop
    // (Cpu::idle_wait) - it advances the clock towards `target` event by
    // event, delivering each event at its own time. It returns true when the
    // clock is at target, false as soon as the guest has to run first: an
    // interrupt became deliverable, or a stop was requested. The idle wait
    // passes kNoDeadline and so returns after the first event that gives the
    // guest something to do. With nothing scheduled at all the game cannot
    // continue, and the machine stops.
    bool advance_game_time(u64 target);
    u64 game_ticks() const noexcept { return cpu_.game_clock().now(); }
    u64 idle_waits() const noexcept { return idle_waits_; }
    u64 host_services() const noexcept { return host_services_; }

    // Host work at every GameEvent::Frame, kHostServiceTicks of game time
    // apart: real-time pacing, input, the stop and savestate checks. It runs
    // on the game thread with all guest state committed and must not touch
    // architected state. The argument is the game time of the event.
    using HostService = std::function<void(u64 game_ticks)>;
    void set_host_service(HostService service) { host_service_ = std::move(service); }
    static constexpr u64 kHostServiceTicks = kGameTicksPerSecond / 200u;  // 5 ms
    // Real-time pacing ticks (docs/FRAME_PACING.md "Pacing on the frame grid"): 1/300 s divides the 1/60 s and the
    // 1/50 s frame, so every frame boundary is one. GameEvent::Frame is also delivered at each of them; one that is
    // not a host-service tick runs only the pacing hook (no input latch, stop or savestate check: the host service's
    // ticks, and so the input tapes, are unchanged).
    static constexpr u32 kPacingHz = 300u;
    static constexpr u64 kPacingTicks = kGameTicksPerSecond / kPacingHz;
    static_assert(kGameTicksPerSecond % kPacingHz == 0u && (kGameTicksPerSecond / 60u) % kPacingTicks == 0u &&
                      (kGameTicksPerSecond / 50u) % kPacingTicks == 0u,
                  "a pacing tick divides every frame period");
    void set_host_pacing(HostService pacing) { host_pacing_ = std::move(pacing); }
    // The next GameEvent::Frame after `now`: the next host-service or pacing tick, both on their absolute grids.
    static constexpr u64 next_frame_event(u64 now) noexcept {
        const u64 service = (now / kHostServiceTicks + 1u) * kHostServiceTicks;
        const u64 pacing = (now / kPacingTicks + 1u) * kPacingTicks;
        return service < pacing ? service : pacing;
    }

    // Wall-clock watchdog on the wait points (diagnostic; off unless set).
    // When the guest has executed for `wall_ms` of wall time without reaching
    // a wait point, the machine stops with the guest location. Under the
    // native time model that is a wait the model does not know about - a
    // guest spin on something only time can change - or a hang. 0 = off.
    void set_wait_watchdog(u32 wall_ms);

    void add_symbol(u32 address, std::string name);
    std::string describe_address(u32 address) const;
    const std::map<u32, std::string>& symbols() const noexcept { return symbols_; }

    // Report of the last stop, including a symbolised call trace.
    std::string stop_report() const;

    // Where the machine has actually got to: hardware activity that only
    // happens once the corresponding subsystem has initialised.
    std::string progress_report() const;

    // ---- Sofdec movie pipeline diagnostics (development only) -------------
    //
    // The recovered CRI Sofdec player and the game's movie module around it
    // are entirely guest code, so the only way to see what they do is to
    // observe them at their own call boundaries. Every handler installed by
    // enable_movie_trace() returns false and writes no guest state, so a
    // traced run executes exactly the same guest code as an untraced one and
    // stays byte-identical. Nothing is installed unless the CLI asks for it.
    struct MovieTraceEvent {
        u64 game_ticks = 0;
        u32 vi_frame = 0;
        u32 site = 0;   // guest address of the observed entry point
        u32 a = 0;
        u32 b = 0;
        u32 c = 0;
        u32 d = 0;
    };
    struct MovieTraceCounts {
        u64 open = 0;         // fn_80036d68  movie object created
        u64 tick = 0;         // fn_80036b0c  per-frame movie update
        u64 get_frame = 0;    // 0x80403e3c   CRI get-frame
        u64 frame_ready = 0;  // fn_800367a8  entered only when a frame exists
        u64 convert = 0;      // 0x804096d0   picture -> RwRaster conversion
        u64 release = 0;      // 0x80403a20   CRI release-frame
        u64 blit = 0;         // fn_80036570  the Im2D blit of the movie quad
        u64 blit_skipped = 0; // ... of which returned early on a null latch
        u64 get_stat = 0;     // 0x804086d8   mwPlyGetStat
        // The nine-module pipeline underneath. 0x803fede8 polls modules 0..8
        // with method 0; module 2 is the MPEG video decoder (its method 11,
        // fn_803f4aa8, is what finally hands a picture back), and fn_803f9c80
        // is the picture-queue head it reads.
        u64 module_poll = 0;    // 0x803fede8
        u64 video_tick = 0;     // fn_803f9398  module 2, method 0
        u64 queue_head = 0;     // fn_803f9c80  picture-queue head fetch
        u64 demux_tick = 0;     // fn_803f449c  module 1, method 0
        std::array<u64, 16> poll_method{};  // 0x803fede8's method index
        // The four functions that pump the module chain, and the method each
        // one passes: fn_803fa858 -> 4, fn_803fadac -> 4, fn_803fb014 -> 3,
        // fn_803fc0e0 -> 2. Method 4 is the one that never appears, so which
        // of these two never runs is the whole question.
        u64 pump_a858 = 0;
        u64 pump_adac = 0;
        u64 pump_b014 = 0;
        u64 pump_c0e0 = 0;
        // fn_80036e88 is the movie SERVICE. extract/functions.csv splits it at
        // the bctrl at 0x80036ebc, so "fn_80036ec0" is not a function at all -
        // it is the same function's continuation, and it is what reaches
        // fn_8040534c, the CRI player service that pumps method 4 seven times.
        u64 service = 0;        // fn_80036e88
        u64 cri_service = 0;    // fn_8040534c  mwPlyCreateSofdec
        u64 cri_thread_prio24 = 0;  // 0x803c5330  the thread that decodes
        u64 idle_thrd_proc = 0;     // 0x80408c44  MWSFSVR_IdleThrdProc
        u64 vsync_thrd_proc = 0;    // 0x80408cec  MWSFSVR_VsyncThrdProc
        u64 adxgc_init = 0;         // 0x803c5150  creates the four CRI threads
        u64 os_resume_thread = 0;   // 0x80378ecc  OSResumeThread
        // Inside the idle server: MWSFSVR_IdleThrdProc -> mwPlyExecSvrHndl ->
        // mwlSfdExecSvr (all 8 handles, then SFD_ExecServer) and
        // mwlSfdExecDecSvr, the per-handle decode state machine keyed on the
        // MwePlyStat word at handle+4.
        u64 exec_svr_hndl = 0;  // 0x80408a68  mwPlyExecSvrHndl
        u64 sfd_exec_svr = 0;   // 0x80408b30  mwlSfdExecSvr
        u64 sfd_exec_dec = 0;   // 0x80408d80  mwlSfdExecDecSvr
        u64 sfd_exec_server = 0;  // 0x803fc394  SFD_ExecServer
        // Method 2 is the per-service tick the chain is actually pumped with.
        // 0x803fede8 stops at the first module that returns non-zero, so these
        // three say how far down the chain each service pass gets.
        u64 m2_module0 = 0;  // fn_803f2898
        u64 m2_module1 = 0;  // fn_803f4458
        u64 m2_module2 = 0;  // fn_803f92e0  the MPEG video decoder
    };
    // Must be called before start(); the hooks are installed there.
    void enable_movie_trace() noexcept { movie_trace_enabled_ = true; }
    // Read-only observer on the guest's own frustum culling
    // (RwCameraTestSphere, 0x80486718). Answers the one question the host
    // cannot see by itself: how much world content the GUEST rejected that
    // the host's widened projection would have shown. The handler returns
    // false, so the recovered body runs untouched and no guest state moves.
    void enable_visibility_probe() noexcept { visibility_probe_enabled_ = true; }

    // NATIVE VISIBILITY: the frustum test is OUR implementation, not the
    // original's.
    //
    // RwCameraFrustumTestSphere (0x80486718) decides, per atomic, whether the
    // original emits any GX draw at all. It tests against the six planes the
    // original built from ITS viewWindow, which at any aspect wider than 4:3 is
    // a narrower cone than the one the host displays. Measured on Final Haunt
    // at 16:9: 3,644 props rejected while still inside the displayed frustum,
    // every one of them on a horizontal side plane, the worst with 215 world
    // units of room to spare.
    //
    // This does not widen the original's planes, add a margin, or expand a
    // bound. It replaces the decision: the handler returns true, so the
    // recovered body never runs, and the answer comes from
    // gekko::camera::classify_sphere against the frustum derived from the
    // DISPLAYED camera's extents. Coverage and culling become the same volume
    // because they are the same camera.
    //
    // The contract, read out of the original at 0x80486718: r3 = 0 OUTSIDE
    // (early return on the first plane with dot(n,c) - d > radius), 1 BOUNDARY,
    // 2 INSIDE (the default, made sticky at 1 by any plane within -radius).
    void enable_native_visibility() noexcept { native_visibility_ = true; }

    // The native health bar. Default ON: it IS the health-bar layer, and
    // `--legacy-health-bar` runs the guest's own implementation instead, for
    // A/B only. See docs/health-bar-2026-09-21/.
    void enable_native_health_bar(bool enabled) noexcept { native_health_bar_ = enabled; }
    bool native_health_bar() const noexcept { return native_health_bar_; }
    const health_bar::Layer::Stats& health_bar_stats() const noexcept {
        return health_bar_layer_.stats();
    }
    u64 health_bar_camera_declines() const noexcept { return health_bar_camera_declines_; }
    bool native_visibility() const noexcept { return native_visibility_; }

    // The PC Options menu (gekko/native_options_menu.h): six categories on the game's Options screen.
    const options_menu::Menu& options_display() const noexcept { return options_display_; }
    // The prompts' controller buttons (gekko/button_glyphs.h): configure before mount_assets, which attaches them.
    button_glyphs::ButtonGlyphs& button_glyphs() noexcept { return button_glyphs_; }

    struct NativeVisibilityStats {
        u64 decided = 0;        // answered by the native implementation
        u64 deferred_unity = 0; // widening exactly 1.0: the original's own arithmetic
        u64 deferred_layout = 0;// frustum not recognised; the original decides
        // Widening read exactly 1.0 because the renderer had not been told its
        // output size yet - a first-frame pop, not a 4:3 frame. Counted apart
        // so it can never hide inside deferred_unity again.
        u64 deferred_unconfigured = 0;
        u64 outside = 0;
        u64 boundary = 0;
        u64 inside = 0;
        u64 admitted = 0;       // the original would have rejected; we keep it
        u64 rebuilds = 0;       // camera changes seen
    };
    const NativeVisibilityStats& native_visibility_stats() const noexcept {
        return native_visibility_stats_;
    }
    // RenderWare's camera frustum built from the displayed picture (native_frustum_build): builds made, and the
    // builds left to RenderWare's own code - each reason apart (none is expected with the native renderer).
    struct FrustumBuildStats {
        u64 built = 0;
        u64 declined_camera = 0;  // no canonical camera (pointers, window, near / far, basis)
        u64 declined_offset = 0;  // a view offset (none in this game): the canonical camera has no shear
    };
    const FrustumBuildStats& frustum_build_stats() const noexcept { return frustum_build_stats_; }

    // Read-only observer on the camera FOV chain. fn_801C28A8 is the leaf
    // getter `addi r3,r3,28; blr` that hands fn_80271F9C the camera-mode
    // object's (fovX, fovY) degree pair; fn_802731A4 computes the scale
    // pair applied to it. Between them they decide the live FOV, and the
    // port's horizontal half disagrees with real hardware.
    void enable_fov_probe() noexcept { fov_probe_enabled_ = true; }
    struct FovProbe {
        u64 pair_reads = 0;      // fn_801C28A8 calls from fn_80271F9C
        u64 scale_calls = 0;     // fn_802731A4 calls
        double fov_x = 0.0;      // camera-mode object +28, degrees
        double fov_y = 0.0;      // camera-mode object +32, degrees
        double fov_x_min = 1e9, fov_x_max = -1e9;
        u32 mode_object = 0;     // r3 at the getter
        int scale_index = -9999; // r4 handed to fn_802731A4
        u64 scale_index_negative = 0;  // r4 == -1 path
        u64 scale_index_zero = 0;      // r4 == 0 path
        u64 scale_index_other = 0;
    };
    const FovProbe& fov_probe() const noexcept { return fov_probe_; }
    struct VisibilityProbe {
        u64 tests = 0;             // sphere tests observed
        u64 layout_ok = 0;         // apex recovered, side planes consistent
        u64 layout_bad = 0;        // frustum did not look like 4 side planes
        u64 rejected_guest = 0;    // outside the guest's own frustum
        u64 rejected_widened = 0;  // outside the widened frustum too
        u64 lost = 0;              // guest rejected, widened would show: THE DEFECT
        double lost_max_halfangle_deg = 0.0;
        double guest_half_hfov_deg = 0.0;
        double guest_half_vfov_deg = 0.0;
        // Read straight out of RwCamera+0x68/+0x6C rather than derived
        // from the plane normals, so the reported FOV does not depend on
        // the probe's own geometry.
        double view_window_x = 0.0;
        double view_window_y = 0.0;
        double view_window_x_min = 1e9;
        double view_window_x_max = 0.0;
        u32 camera_address = 0;
        double widened_half_hfov_deg = 0.0;

        // --- production culler vs independent clip-space reference ----------
        // The production culler is the ORIGINAL's own plane test, reproduced
        // exactly from the planes it is about to use. The reference transforms
        // the sphere's conservative world-space AABB into the DISPLAYED
        // camera's view space and rejects only when all eight corners are
        // outside the same clip plane. A disagreement in the direction
        // "production OUTSIDE, reference VISIBLE" is a prop that pops out while
        // still on screen.
        u64 reference_visible_production_rejected = 0;
        u64 reference_rejected_production_visible = 0;  // must stay 0
        double near_plane = 0.0;
        double far_plane = 0.0;
        // Which plane of the PRODUCTION frustum did the rejecting, for the
        // disagreements only. Index matches gekko::camera::FrustumPlane.
        std::array<u64, 6> disagree_by_plane{};
        // The worst case seen: how far the prop still was from the displayed
        // frustum edge when production threw it away, in world units.
        double worst_disagreement_margin = 0.0;
        u32 worst_disagreement_sphere = 0;
    };
    struct VisibilityDisagreement {
        u32 sphere_address = 0;
        float centre[3] = {0.0f, 0.0f, 0.0f};
        float radius = 0.0f;
        u32 rejecting_plane = 0;
        float production_margin = 0.0f;   // how far outside the production plane
        float reference_slack = 0.0f;     // how far INSIDE the displayed frustum
        float view_x = 0.0f, view_y = 0.0f, view_z = 0.0f;
    };
    const std::vector<VisibilityDisagreement>& visibility_disagreements() const noexcept {
        return visibility_disagreements_;
    }
    const VisibilityProbe& visibility_probe() const noexcept {
        return visibility_probe_;
    }
    // The PAL 50/60 Hz selection screen. It is reached from RsRwInitialize
    // through fn_8030717c -> fn_80306b48, gated only on VIGetTvFormat() == 1
    // (VI_PAL), and builds its texture-dictionary path with sprintf from
    // "palSel/PalSel%s.txd" at 0x8050c9c8 (materialised as literal-pool base
    // 0x8050c888 + 0x140, which is why a naive lis/addi scan cannot see it).
    // Observation only, like the movie hooks.
    void enable_ui_probe() noexcept { ui_probe_enabled_ = true; }
    // Write the decoded movie picture out as a PPM, taken at RwRasterUnlock -
    // after mwPlyFxCnvFrm has converted the CRI picture into the locked raster
    // and while those pixels are still LINEAR. Renderer::note_texture only
    // dumps a texture at its first GX bind, which is why the archived dumps
    // bound the content at one instant and could not say whether the decoder
    // produces a real picture. Requires enable_movie_trace().
    void set_movie_frame_dump(std::string path) { movie_frame_dump_ = std::move(path); }

    // ---- the AX microcode's contract (run_ax_mix) ----------------------------
    //
    // The native DSP (NativeAudio) runs every command list the game hands it
    // through run_ax_mix: it advances the voice positions the way the AX
    // microcode would - the three quantities the guest reads back from the
    // parameter blocks, state, currentVolume and currentAddress, and nothing
    // else (CRI's Sofdec clock is derived from them) - and mixes the frame
    // into the output buffer the list names. It always runs: the voices move
    // whether or not a host device plays the result.
    //
    // The two pure 64-bit divide helpers of the Metrowerks runtime (0x803A1F54
    // unsigned, 0x803A2040 signed) served by one host 64-bit division. ON by
    // default: they reproduce the recovered body's complete register and stack
    // state. --no-divide-intrinsics runs the recovered loops for A/B.
    void set_divide_intrinsics(bool enabled) noexcept { divide_intrinsics_ = enabled; }
    bool divide_intrinsics() const noexcept { return divide_intrinsics_; }
    // The Metrowerks __dynamic_cast (0x803A1AFC) served natively (gekko/dynamic_cast_intrinsic.h; docs/CPU_FINAL_HAUNT.md).
    // ON by default; --no-dynamic-cast-intrinsic runs the guest body for A/B.
    void set_dynamic_cast_intrinsic(bool enabled) noexcept { dynamic_cast_intrinsic_ = enabled; }
    // RenderWare's RwMatrixMultiply (0x8047FC08) served natively (gekko/matrix_multiply_intrinsic.h).
    // ON by default; --no-matrix-multiply-intrinsic runs the guest body for A/B.
    void set_matrix_multiply_intrinsic(bool enabled) noexcept { matrix_multiply_intrinsic_ = enabled; }
    // --verify-matrix-multiply: each served call is also run by the guest body and compared.
    void set_verify_matrix_multiply(bool enabled) noexcept { matrix_multiply_.set_verify(enabled); }
    // The CharaColli broad phase's bounding tests served natively where they reject (gekko/chara_colli_intrinsic.h).
    void set_chara_colli_intrinsic(bool enabled) noexcept { chara_colli_intrinsic_ = enabled; }
    void set_verify_chara_colli(bool enabled) noexcept { chara_colli_.set_verify(enabled); }
    void set_chara_colli_loops(bool enabled) noexcept { chara_colli_loops_ = enabled; }
    void set_chara_colli_manage(bool enabled) noexcept { chara_colli_manage_ = enabled; }
    void set_chara_colli_pass(bool enabled) noexcept { chara_colli_pass_ = enabled; }
    void set_trace_stale_contacts(bool enabled) noexcept { trace_stale_contacts_ = enabled; }
    void set_chara_colli_bone_native(bool enabled) noexcept { chara_colli_bone_native_ = enabled; }
    void set_chara_colli_bone_memo(bool enabled) noexcept { chara_colli_bone_memo_ = enabled; }
    u64 divide_intrinsic_declines() const noexcept { return divide_intrinsic_declines_; }
    // 2026-09 ADPCM mixer switches (D07-F2/F3/F4). Both default to the fixed
    // behaviour; the integrator wires --no-... flags so a windowed audio dump
    // can be bisected on one binary. Neither has any guest-visible effect:
    // they change only the samples written into the AI output buffer, which
    // the guest never reads back (only the AI DMA engine does).
    //   sample_resampling: ADPCM voices are resampled in SOURCE-SAMPLE units.
    //     OFF = the 2026-08 nibble-unit arithmetic (each 16-nibble frame's
    //     first sample played three times, ~12.5% of every span skipped).
    //   adpcm_cursor: one continuous decode cursor per voice with its own
    //     predictor history and decoded-sample ring, instead of re-decoding
    //     from the frame boundary every 5 ms with re-primed history. Only
    //     takes effect when sample_resampling is ON (the cursor ring is
    //     indexed in sample units).
    void set_ax_sample_resampling(bool enabled) noexcept { ax_sample_resampling_ = enabled; }
    void set_ax_adpcm_cursor(bool enabled) noexcept { ax_adpcm_cursor_ = enabled; }
    bool ax_sample_resampling() const noexcept { return ax_sample_resampling_; }
    bool ax_adpcm_cursor() const noexcept { return ax_adpcm_cursor_; }
    struct AxMixerReport {
        bool enabled = false;
        u64 frames = 0;        // AX 5 ms frames serviced
        u64 voice_advances = 0;
        u64 loops = 0;         // voices that wrapped at endAddress
        u64 finished = 0;      // one-shots that reached endAddress
        u64 skipped_insane = 0;  // voices whose address window made no sense
        u64 lists = 0;           // AX command lists executed
        u64 lists_unparsed = 0;  // ... that did not yield an output buffer
        u64 mixed_frames = 0;    // output buffers written
        u64 mixed_voices = 0;    // voice-frames that contributed samples
        u64 adpcm_voices = 0;    // ADPCM voice-frames decoded
        // 2026-09 (D07-F2/F3/F4). The first two prove the switches fired;
        // the rest size the continuous decoder's work.
        u64 adpcm_sample_unit_voices = 0;   // ADPCM voice-frames resampled in sample units
        u64 adpcm_cursor_voices = 0;        // ... of which were served from the decode cursor
        u64 adpcm_cursor_reloads = 0;       // cursor (re)established: voice start, seek, restart
        u64 adpcm_samples_decoded = 0;      // ADPCM samples decoded by the cursor path
        u64 muted_adpcm_voices = 0;         // source history maintained with zero main gains/envelope
        u64 muted_adpcm_samples_decoded = 0; // actual cursor samples decoded for those voice-frames
        u64 adpcm_loop_context_reloads = 0; // loop wraps that reloaded pb+180/182/184
        u64 adpcm_span_clamped = 0;         // voice-frames whose ratio exceeded the ring
        u64 source_window_clamped = 0;      // voice-frames whose reach exceeded the mixer's source window
        u64 ramped_voices = 0;              // voice-frames whose mixer control asked for a volume ramp
        u64 enveloped_voices = 0;           // voice-frames with a non-zero envelope delta
        u64 dolby_frames = 0;               // frames mixed in the mode-2 (Dolby matrix) program
        u64 compressor_triggers = 0;        // frames whose main L/R reached the compressor threshold
        u64 compressor_frames = 0;          // frames the compressor applied a gain row to
        s32 peak = 0;            // loudest absolute sample written
        // Wall time spent on the GAME thread doing audio work. Both of these
        // run inline in guest execution - run_ax_frame from an HLE hook and
        // run_ax_mix from the DSP command-list sink - so whatever they cost is
        // taken directly out of the frame budget. Recorded so the question
        // "should audio move to a worker?" can be answered with a number.
        u64 advance_ns = 0;      // voice position advance
        u64 mix_ns = 0;          // ADPCM decode + resample + mix
        // The room effect (gekko/native_reverb_owner.h): blocks whose list scheduled its send/return, and what each
        // call of the game's callback did; effect_ns is the callback's wall time (it runs on the game thread too).
        u64 effect_scheduled = 0;
        u64 effect_processed = 0;
        u64 effect_bypassed = 0;
        u64 effect_idle = 0;
        u64 effect_ns = 0;
    };
    const AxMixerReport& ax_mixer_report() const noexcept { return ax_report_; }
    const sound_tables::Report& sound_table_report() const noexcept { return sound_table_report_; }
    const gcax_midi::Report& midi_report() const noexcept { return midi_report_; }
    const se_manager::Report& se_manager_report() const noexcept { return se_manager_report_; }
    void reset_host_audio_state() noexcept;
    void import_ax_pb_image_from_guest();
    void import_ax_lists_from_guest();
    // Mixes one frame list (a command-list slot address) into the output pass: what the AX frame does after its sync; public for the tests.
    void mix_ax_list(u32 list) { run_ax_mix(list); }
    // Checkpoint continuation at an audio-job boundary; transient source windows
    // and diagnostics are rebuilt, while decoder and mixer history must survive.
    void save_host_audio_state(std::vector<u8>& out) const;
    bool load_host_audio_state(const u8*& cursor, const u8* end, u32 version);
    // The forced MWS_PLY_STAT_PLAYEND heuristic. On by default because the
    // event script cannot advance without it; turning it off shows what the
    // recovered player does on its own.
    void set_sofdec_playend_stub(bool enabled) noexcept { sofdec_playend_stub_ = enabled; }
    // Developer navigation (--boot-stage). NOT a game feature and never active
    // unless requested: on a cold boot it performs, once, exactly the hand-off
    // the game's own Select Mode confirm makes (0x802C4894 + its exit
    // 0x802C4D48) so StageSequence::Manager -> PlayOnce -> StageManager(id)
    // load the stage through recovered code. See docs/tools/BOOT_STAGE.md.
    struct BootStageReport {
        bool requested = false;
        // The StageSequence kind requested: 0 PlayOnce (--boot-stage, id = a
        // stage), 1 PlayStory (--boot-story, a new game), 2 PlayEvent
        // (--boot-event, id = an event-stage record), 7 PlayFlashback
        // (--boot-flashback).
        u32 kind = 0;
        u32 id = 0;
        // --boot-cutscene: the story cutscene (eventNNNN) PlayStory is handed as its first entry; 0 = none.
        u32 cutscene = 0;
        // --boot-story-stage: the stage (plain decimal id) PlayStory is handed as its first entry; 0 = none.
        u32 story_stage = 0;
        // The menus' own play-record reset fn_80206680(0x8057E82C) before the hand-off (2P, Last Story, Expert).
        bool reset_play_record = false;
        u32 story_mode = 0;           // PlayStory's start mode 0x8058578C: 0 new game, 2 Last Story
        u32 stages[3] = {0, 0, 0};    // Play2POnce: [0]; Play2PThree: all three
        u32 characters[2] = {14, 14}; // the 2P character words 0x8057E89C / 0x8057E8A0 (14 + cursor, 14..19)
        u32 expert_index = 0;         // PlayHard: the save block's run position (0x80577A18 + 0x9C), stage 0..22
        bool cutscene_handed = false;
        bool fired = false;
        u64 fired_at_ticks = 0;  // game time
        u32 hz_choice = 0;  // 0 = no PAL screen seen, 60 = PAL60 accepted
    };
    void set_boot_stage(u32 id) noexcept {
        boot_stage_.requested = true;
        boot_stage_.kind = 0u;
        boot_stage_.id = id;
    }
    // --boot-event: the same hand-off into StageSequence::PlayEvent, which plays
    // one in-engine event (files/event/eventNNNN*) through recovered code.
    void set_boot_event(u32 id) noexcept {
        boot_stage_.requested = true;
        boot_stage_.kind = 2u;
        boot_stage_.id = id;
    }
    // --boot-story: the same hand-off into StageSequence::PlayStory, new game.
    void set_boot_story() noexcept {
        boot_stage_.requested = true;
        boot_stage_.kind = 1u;
        boot_stage_.id = 0u;
    }
    // --boot-cutscene: the --boot-story hand-off, and PlayStory's first story entry is the cutscene `id`
    // (files/event/eventNNNN.scr, or the movie ENNNN.sfd when it has no script), played by the game's own event step.
    void set_boot_cutscene(u32 id) noexcept {
        set_boot_story();
        boot_stage_.cutscene = id;
    }
    // --boot-story-stage: the --boot-story hand-off, and PlayStory's first story entry is the stage `id`: the story
    // loop builds StageManager(id, 1) as for any story stage (the stages no Select Mode grid lists: 0700, 0710, ...).
    void set_boot_story_stage(u32 id) noexcept {
        set_boot_story();
        boot_stage_.story_stage = id;
    }
    // --boot-2p / --boot-2p-three: the 2P battle select's hand-off (0x802C1514 / 0x802C148C): StageSequence kind 3
    // (Play2POnce, one arena) or 4 (Play2PThree, stg0800 / 0801 / 0802), the two characters, the play-record reset.
    // 2P itself follows from the stage record's flag 0x4 (only 0800 - 0802), which the StageManager reads.
    void set_boot_2p(u32 stage, u32 character1, u32 character2) noexcept {
        boot_stage_.requested = true;
        boot_stage_.kind = 3u;
        boot_stage_.id = stage;
        boot_stage_.stages[0] = stage;
        boot_stage_.characters[0] = character1;
        boot_stage_.characters[1] = character2;
        boot_stage_.reset_play_record = true;
    }
    void set_boot_2p_three(u32 character1, u32 character2) noexcept {
        set_boot_2p(800u, character1, character2);
        boot_stage_.kind = 4u;
        boot_stage_.stages[1] = 801u;
        boot_stage_.stages[2] = 802u;
    }
    // --boot-last-story: PlayStory in start mode 2 (the story menu's Last Story, 0x802BB878): its own queue plays the
    // Last Story events, then StageManager(700, 0).
    void set_boot_last_story() noexcept {
        set_boot_story();
        boot_stage_.story_mode = 2u;
        boot_stage_.id = 2u;  // reported as the story mode
        boot_stage_.reset_play_record = true;
    }
    // --boot-expert: PlayHard (Expert mode, kind 6) continuing a run at stage index 0..22 of its table 0x80560708
    // (0x805857A8 = 1 continue; the save block's run position 0x80577A18 + 0x9C).
    void set_boot_expert(u32 index) noexcept {
        boot_stage_.requested = true;
        boot_stage_.kind = 6u;
        boot_stage_.id = 0u;
        boot_stage_.expert_index = index;
        boot_stage_.reset_play_record = true;
    }
    // --boot-flashback: the same hand-off into StageSequence::PlayFlashback.
    void set_boot_flashback() noexcept {
        boot_stage_.requested = true;
        boot_stage_.kind = 7u;
        boot_stage_.id = 0u;
    }
    const BootStageReport& boot_stage_report() const noexcept { return boot_stage_; }
    // ---- native renderer capture (docs/native-runtime-2026-09-16) -----------
    //
    // An observer runs before whatever handler already owns the address and
    // never serves the call: the recovered body (or the owning handler) runs
    // exactly as without it. Observers installed later chain in front of
    // earlier ones.
    void add_hle_observer(u32 address, std::function<void(Cpu&)> observer, std::string name);
    // Creates the RenderWare capture. Must be called before start(); its hooks
    // are installed last in start() so they chain in front of every other hook.
#if !defined(SHADOW_RENDER_LIFTED)  // G5
    RwCapture& enable_native_capture();
    RwCapture* native_capture() noexcept { return native_capture_.get(); }
#endif
    // The SHADOW_RENDER_LIFTED build's listener for the 2D identity hooks' announcements (before start()).
    void set_hud_listener(HudListener* listener) noexcept { hud_listener_ = listener; }

    const MovieTraceCounts& movie_trace_counts() const noexcept { return movie_counts_; }
    const std::vector<MovieTraceEvent>& movie_trace_events() const noexcept {
        return movie_events_;
    }
    std::string movie_trace_report() const;

private:
    shadow::display::State display_;
    float logic_aspect_ = 0.0f;  // set_logic_aspect
    GuestMemory memory_;
    Cpu cpu_;
    // The lifted subsystems; Cpu::lifted() points here.
    Lifted lifted_;
    std::unique_ptr<NativeInterrupts> interrupts_;
    std::unique_ptr<NativeVideo> video_;
    std::unique_ptr<NativeAudio> audio_;
    std::unique_ptr<NativeFrameLimiter> frame_limiter_;
    std::unique_ptr<NativeObjectRange> object_range_;
    std::unique_ptr<NativeMotionStore> motion_store_;
    std::unique_ptr<NativeSoundBanks> sound_banks_;
    std::unique_ptr<NativeStreamRing> stream_ring_;
    std::unique_ptr<NativeAttachedEffects> attached_effects_;
    std::unique_ptr<NativeGxSync> gx_sync_;
#if !defined(SHADOW_RENDER_LIFTED)  // G5
    std::unique_ptr<NativeImmediate> immediate_;
#endif
    std::unique_ptr<NativeCard> card_;
    std::unique_ptr<SaveStore> save_store_;
    bool save_store_active_ = false;  // set_save_file: the game's save service; without it the card model (the oracle)
    std::unique_ptr<NativeIpl> ipl_;
    os_report::DebugConsole debug_console_;
    std::unique_ptr<NativeReset> reset_controls_;
    std::unique_ptr<NativeBoot> boot_;  // the program's start-up (native __start, OSInit)
    std::unique_ptr<NativeDiscError> disc_error_;
    std::unique_ptr<NativeLinkGroups> link_groups_;
    std::unique_ptr<NativeWorldLine> world_line_;
    std::unique_ptr<NativeWorldCollision> world_collision_;
    std::unique_ptr<NativeDirectoryScan> directory_scan_;
    // CRI's file device over the file API (created with it, DISC_SYSTEM_MAP.md cut 3).
    std::unique_ptr<NativeCriFileDevice> cri_file_device_;
    // RenderWare's GameCube file system over the file API (DISC_SYSTEM_MAP.md cut 4).
    std::unique_ptr<NativeRtGcnFs> rtgcnfs_;
    std::unique_ptr<NativeJobs> native_jobs_;
    std::unique_ptr<NativeExpandJobs> native_expand_jobs_;
    std::unique_ptr<NativeOne> native_one_;
    std::unique_ptr<NativeCri> native_cri_;
    std::unique_ptr<NativeSvm> native_svm_;  // CRI's server manager (docs/CRI_SERVERS.md section 1)
    std::unique_ptr<NativeSj> native_sj_;    // CRI's stream joints (docs/CRI_SERVERS.md section 4b)
    std::unique_ptr<NativeAdxOutput> native_adx_output_;  // CRI's ADX output to the voices (section 4c)
    errata::Stats errata_stats_;  // deliberate original-bug fixes (gekko/game_errata.h)
    bool errata_installed_ = false;
    std::unique_ptr<GameMemory> game_memory_;
    // The lowered GX sites' calls (gekko/gx_sites.h), routed to the owners.
    class GxSiteRouter;
    std::unique_ptr<GxSiteRouter> gx_site_router_;
    // The recovered 2D identity hooks' announcements, for the capture or the
    // HUD listener (with neither installed nothing listens).
    void mark_hud_group(HudGroupKind kind, u32 group_id = 0, float canvas_x = 0.0f, u32 instance = 0);
    void mark_hud_scene(u32 node, u32 cast);
    HudListener* hud_listener_ = nullptr;
    assets::AssetSystem assets_;
    std::unique_ptr<NativeFileApi> file_api_;
    // PRS members decoded ahead of their expand calls (prs_ahead.h). After assets_: destroyed first.
    std::unique_ptr<PrsAhead> prs_ahead_;
    std::unique_ptr<GameplayScheduler> gameplay_scheduler_;
    bool boundary_stop_pending_ = false;
    StopReason boundary_stop_reason_ = StopReason::HostRequest;
    std::string boundary_stop_detail_;
    // The arena limit the boot sets (0x80000034): below BI2 by the size of
    // the disc's directory table, the boundary the game was mastered with.
    u32 arena_high_ = 0;
    std::map<u32, std::string> symbols_;

    // Host-side Sofdec completion. The original CRI MPEG decoder is in the
    // DOL and runs, but it never produces a finished frame on this runtime, so
    // mwPlyGetStat stays PREP/PLAYING and the event script never loads the
    // first stage. After an .sfd has been streaming for a short time the host
    // writes the PLAYEND status the decoder would have stored.
    std::string sofdec_current_sfd_;
    u32 sofdec_start_frame_ = 0;
    std::string sofdec_completed_sfd_;
    bool sofdec_playend_stub_ = true;
    bool pc_save_text_ = true;
    bool movie_trace_enabled_ = false;
    bool visibility_probe_enabled_ = false;
    bool native_visibility_ = false;
    // The canonical camera and its OWN cache key. Deliberately separate from
    // the frustum-test cache below: sharing them shipped a defect where the
    // visibility path validated the shared key without ever writing the
    // camera, so a cache hit here returned a default-constructed camera and
    // silently turned every world-anchored depth into -world_z.
    health_bar::Layer health_bar_layer_{};
    health_bar::Composition health_bar_composition_ = health_bar::Composition::authored();
    bool native_health_bar_ = true;
    u64 health_bar_camera_declines_ = 0;

    camera::Camera native_camera_{};
    std::array<u32, 18> native_camera_key_{};
    bool native_camera_valid_ = false;
    NativeVisibilityStats native_visibility_stats_{};
    FrustumBuildStats frustum_build_stats_{};
    // Cached displayed frustum, rebuilt only when the original's planes
    // or the host widening actually move.
    std::array<u32, 24> native_frustum_signature_{};
    float native_frustum_scale_ = 0.0f;
    bool native_frustum_valid_ = false;
    std::array<float, 24> native_frustum_planes_{};
    std::vector<VisibilityDisagreement> visibility_disagreements_;
    bool fov_probe_enabled_ = false;
    FovProbe fov_probe_{};
    VisibilityProbe visibility_probe_{};
    void observe_frustum_test(Cpu& cpu);
    // Same-call capture: runs at 0x800091D0 with lr == 0x800BFA18, i.e.
    // immediately after 0x800BF9D8 projected the anchor and before it
    // applies any of the six predicates.
    // Returns true when it answered (r3 written); false to let the
    // recovered body decide.
    bool native_frustum_test(Cpu& cpu);
    // RenderWare's perspective frustum build (0x80485318) from the displayed picture: true when it built the camera's
    // corners and planes (r3 = the camera); false leaves the build to RenderWare's code.
    bool native_frustum_build(Cpu& cpu);
    // Recover the canonical native camera from the guest's own frustum, cached
    // on the plane signature and the coverage. Shared by world visibility and
    // world-anchor projection so they cannot use different cameras.
    bool canonical_camera(Cpu& cpu, camera::Camera& out);
    // `aspect` > 0: that picture aspect instead of the display's (the logic aspect for a gameplay caller).
    bool canonical_camera(Cpu& cpu, u32 camera_ptr, camera::Camera& out, float aspect = 0.0f);
    bool native_health_bar_draw(Cpu& cpu);
    // Replaces the guest world-to-screen projection for world-anchored UI.
    bool movie_module_vtables_logged_ = false;
    bool ui_probe_enabled_ = false;
    void install_ui_probe_hooks();
    // The listed platform sites (extract/platform-sites.txt), routed to the
    // native subsystem whose site table lists them.
    u16 platform_read16(Cpu& cpu, u32 site, u32 effective);
    void platform_write16(Cpu& cpu, u32 site, u32 effective, u16 value);
    u32 platform_value(Cpu& cpu, u32 site);
    BootStageReport boot_stage_{};
    int boot_stage_hz_step_ = 0;
    void install_boot_stage_hooks();
    // Installs a hook that first runs any handler already bound to the address
    // (a --watch, --ui-probe or trace hook), so neither silently replaces the
    // other.
    void install_chained_hle(u32 address, Cpu::HleHandler handler, std::string name);
#if !defined(SHADOW_RENDER_LIFTED)  // G5
    std::unique_ptr<RwCapture> native_capture_;
#endif
    std::string movie_frame_dump_;
    u32 movie_convert_dest_ = 0;
    u32 movie_frame_width_ = 0;
    u32 movie_frame_height_ = 0;
    u32 movie_frames_dumped_ = 0;
    u32 movie_frames_seen_ = 0;
    void dump_movie_frame(Cpu& cpu);
    bool divide_intrinsics_ = true;
    bool dynamic_cast_intrinsic_ = true;
    DynamicCastIntrinsic dynamic_cast_;
    bool matrix_multiply_intrinsic_ = true;
    MatrixMultiplyIntrinsic matrix_multiply_;
    bool chara_colli_intrinsic_ = true;
    bool chara_colli_loops_ = true;
    bool chara_colli_manage_ = true;
    bool trace_stale_contacts_ = false;
    bool chara_colli_pass_ = false;  // not exact yet: docs/CPU_FINAL_HAUNT.md, the pair pass
    bool chara_colli_bone_native_ = true;
    bool chara_colli_bone_memo_ = true;
    CharaColliIntrinsic chara_colli_;
    AxMixerReport ax_report_;
    // DSP-published PB list, not a game-link-address-derived array. Decoder
    // slots are keyed by PB address so a reordered chain retains voice history.
    std::array<u32, 64> ax_voice_pb_{};
    std::array<u32, 64> ax_pb_order_{};
    u32 ax_pb_count_ = 0;
    // The DSP side's lagging copy of the voice parameter blocks and update rings (ax_pb_image.h): written by the native sync, read by the mixer.
    AxPbImage ax_pb_image_;
    AxLists ax_lists_;  // the next frame mix's settings (native_ax_frame.h)
    ax_frame::Engine ax_engine_;  // the AX frame sequencing (native_ax_frame.h)
    void ax_pb_write16(u32 pb, u32 offset, u16 value);
    u32 ax_pb_peek32(u32 pb, u32 offset) const;
    bool parse_ax_list(u32 list, u32& head, u32& output, shadow::audio::FrameSettings& settings, bool quiet = false);
    // The guest address of the compressor table the list names, once its 21 x 160 gains have been checked equal to
    // the native copy (shadow::audio::compressor_gain_table); 0 = not yet.
    u32 ax_compressor_table_verified_ = 0;
    bool select_ax_parameter_blocks(u32 head);
    void reset_ax_voice(u32 voice) noexcept;
    // src.currentAddressFrac is write-only from the CPU's side, so the
    // sub-unit remainder is kept here rather than in the guest's block, where
    // the guest would overwrite it whenever it rewrote the SRC fields.
    std::array<u32, 64> ax_position_fraction_{};
    std::array<bool, 64> ax_voice_was_running_{};
    // Set when a voice (re)starts, consumed by its next mix: the native mixer then takes the SRC history the SDK
    // primed in the PB (words 86..89) instead of continuing its own.
    std::array<bool, 64> ax_voice_restarted_{};
    // The span each voice covered in the frame just advanced, so the mixer can
    // generate the samples the DSP would have produced over exactly that span.
    std::array<u32, 64> ax_span_start_{};
    std::array<u32, 64> ax_span_frac_{};  // 16.16 fraction at span_start (host mix)
    std::array<bool, 64> ax_span_active_{};
    // ADPCM is sequential: each sample depends on the two before it. The DSP
    // keeps that history per voice, so the host has to as well, along with the
    // position it belongs to - a jump means the history is stale and has to be
    // reloaded from the parameter block the guest primed.
    std::array<s32, 64> ax_adpcm_yn1_{};
    std::array<s32, 64> ax_adpcm_yn2_{};
    std::array<u32, 64> ax_adpcm_position_{};
    // 2026-09 continuous ADPCM decoder (D07-F3/F4). One cursor per voice and
    // a ring of the samples it produced, so a frame decodes forward from
    // where the previous frame stopped instead of restarting at the frame
    // boundary with re-primed history. The ring must hold one frame's worth
    // of source samples at the largest ratio the guest programs (4.0 -> 637)
    // plus the small backward tolerance the resampler is allowed to reuse.
    // Allocated once, by the constructor (System lives on the main thread's
    // stack, so the 128 KiB ring is not an inline member). Guest thread only.
    // One AX 5 ms frame at the SDK's 4.0 ratio is 637 source samples plus a
    // small backward window. 1024 is that working set, not a stall bound: do
    // not enlarge it to hide a wrap. The index type is 64-bit so decoded
    // (cumulative host samples) can index the ring without a 32-bit mask.
    static constexpr hw::AudioFrameIndex64 kAxAdpcmRingSamples = 1024;
    struct AxAdpcmRingStore {
        std::array<std::array<s16, static_cast<std::size_t>(kAxAdpcmRingSamples)>, 64> voice{};
    };
    bool ax_sample_resampling_ = true;
    bool ax_adpcm_cursor_ = true;
    std::array<AxAdpcmVoiceCursor, 64> ax_adpcm_cursor_state_{};
    std::unique_ptr<AxAdpcmRingStore> ax_adpcm_ring_;
    // SCAFFOLDING (R2): deleted by the Audio + CRI lift. The guest-side source fetch hands the native mixer each
    // voice's source samples for the frame as plain values: one window per submitted voice, sized for the largest
    // ratio the SDK programs (4.0: 638 samples) with headroom; reads past a window are silence (counted).
    static constexpr u32 kAxSourceWindowSamples = 4096u;
    struct AxSourceWindowStore {
        std::array<std::array<s16, kAxSourceWindowSamples>, 64> voice{};
    };
    std::unique_ptr<AxSourceWindowStore> ax_source_windows_;
    // The native mixer (runtime/include/native_audio_mixer.h): kept by the native audio engine.
    std::unique_ptr<shadow::audio::Mixer> audio_mixer_;
    // The game's room reverb: the context and parameters the native effect belongs to (gekko/native_reverb_owner.h).
    reverb::Binding reverb_binding_;
    void install_reverb_owner();
    // The game's sound-effect table lookup and its data errata (gekko/native_sound_tables.h).
    sound_tables::Report sound_table_report_;
    // GCAX's MIDI input and the SE manager (gekko/native_gcax_midi.h, gekko/native_se_manager.h).
    gcax_midi::Report midi_report_;
    se_manager::Report se_manager_report_;
    se_manager::Services se_services_{se_manager_report_, midi_report_, sound_table_report_};
    options_menu::Menu options_display_{};
    bool voice_archive_ready_ = false;  // the switchable dialogue-voice archive is served (gekko/voice_language.h)
    button_glyphs::ButtonGlyphs button_glyphs_{};
    static void play_menu_sound(void* system, Cpu& cpu, u32 id);
    void advance_ax_accepted_voices();
    void run_ax_mix(u32 command_list);
    MovieTraceCounts movie_counts_;
    std::vector<MovieTraceEvent> movie_events_;
    void note_movie_event(Cpu& cpu, u32 site, u32 a = 0, u32 b = 0, u32 c = 0, u32 d = 0);
    void install_movie_trace_hooks();
    HostService host_service_;
    HostService host_pacing_;

    // ---- game time -----------------------------------------------------------
    // Moves the clock to `when` (never past a pending deadline) and ages the
    // native card's persistence debounce over the elapsed ticks.
    void move_clock_to(u64 when);
    void deliver_event(GameEvent event);
    // GameEvent::Devices: when the native card's deferred host persist is due
    // (a host-policy moment: nothing the guest sees changes then).
    void schedule_devices();
    // A wait point's wait (Cpu::idle_wait): advance the clock to the next
    // event and deliver every event due then.
    void idle_wait();
    // The same advance, for the frame limiter's zero-time frames; returns
    // what advance_game_time returns.
    bool advance_to_next_event();
    u64 idle_waits_ = 0;
    u64 host_services_ = 0;
    // set_wait_watchdog: steady-clock ns at which the guest thread last
    // started executing guest code (run(), or a return from
    // advance_game_time); 0 while it is not executing guest code.
    std::atomic<s64> guest_running_since_ns_{0};
    std::atomic<bool> watchdog_quit_{false};
    std::thread watchdog_;
    u32 watchdog_wall_ms_ = 0;
    void note_guest_running(bool running) noexcept;

    u64 unsigned_divide_intrinsic_calls_ = 0;
    u64 signed_divide_intrinsic_calls_ = 0;
    u64 divide_intrinsic_declines_ = 0;
};

}  // namespace gekko
