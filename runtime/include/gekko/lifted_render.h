#pragma once

// The game's rendering in the SHADOW_RENDER_LIFTED build (docs/lift-20261006/R2_DRIVER.md 4.2, 4.3): the native
// renderer (runtime/lift/render), the hand-over of frames from the game thread to the UI thread, and the R2
// scaffolding the lowered RenderWare driver sites call (runtime/scaffold/rw_driver).
//
//   * Game thread: the lowered sites describe each frame; the present's display-copy site pushes the frame packet
//     into a bounded queue that never makes the game wait (older unrendered packets are dropped). Textures and
//     meshes are created on this thread while the game loads; the renderer guards its own tables.
//   * UI thread: owns the window, its messages and the swap chain; takes the newest packet and renders it.
//
// The interface carries no graphics-API type, so the launcher compiles without the renderer's headers.

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace gekko {

class Cpu;
class HudListener;

struct LiftedRenderOptions {
    bool warp = false;         // the software rasterizer (deterministic; headless gates)
    bool debug_layer = false;  // the D3D12 debug layer
    bool vsync = true;
    bool collect_timing = false;  // opt-in CPU-side wall-time observations; no GPU queries
    // Each view's picture framed with the game camera's own horizontal extent (the game's framing) instead of square
    // pixels from its vertical extent (docs/boundary-20261007/DESIGN.md: render/view.h Framing).
    bool game_framing = false;
    // G10 (HW_REPORT N.2): the size the R2 renderer renders at (--render-width / --render-height, display.cfg
    // render_width / render_height); 0: the window's size. Both given: exactly that size - the host makes the
    // game-visible output that size, so the camera, culling and HUD take its aspect (docs/RESOLUTION_AUDIT.md). One
    // given, or an output of another aspect (an input tape's recorded output): aspect-locked to the output - the
    // height is kept and the width follows the output's aspect (a width alone keeps the width). Presented fitted into
    // the window (bars when the shapes differ).
    std::uint32_t render_width = 0;
    std::uint32_t render_height = 0;
};

// Frames written to PNG files: the first frame the game finished at or after `first_frame`, then every `stride`
// frames, `count` in all (a count above 1 numbers the files). Rendered at `width` x `height` whatever the window.
struct LiftedCapture {
    std::string path;
    std::uint64_t first_frame = 0;
    std::uint64_t stride = 0;
    std::uint64_t count = 1;
    std::uint32_t width = 1280;
    std::uint32_t height = 720;
};

class LiftedRender {
public:
    LiftedRender();
    ~LiftedRender();
    LiftedRender(const LiftedRender&) = delete;
    LiftedRender& operator=(const LiftedRender&) = delete;

    // Creates the device and the renderer and installs the scaffolding the lowered sites reach. Before guest code
    // runs.
    bool start(const LiftedRenderOptions& options, std::string* error);
    // After the game thread has stopped: uninstalls the scaffolding and releases the device.
    void stop();
    // The listener for System's 2D identity announcements (the scaffolding lays the 2D out with them); null before
    // start().
    HudListener* hud_listener() noexcept;
    // The fade-in of models that appear inside the view (gekko/appear_fade.h): its length in game ticks, 0 = off.
    // After start().
    void set_fade_in_ticks(std::uint64_t ticks) noexcept;
    // The far-object proxies (docs/DRAW_DISTANCE.md): 1 = the original, none; above 1, to that many times each layout
    // entry's authored range; 8 and up, wherever the projected-size rule allows. After start().
    void set_draw_distance(float range_scale) noexcept;
    // The effect layer switches (docs/EFFECTS_INVENTORY.md "Layer switches"): `off` a comma list of layer names
    // switched off, `solo` one layer drawn alone (empty: none), `isolate` a solo without the world. False and `error`
    // for an unknown name. Any time; the window's keys change them too (effect_layer_keys, UI thread).
    bool set_effect_layers(const std::string& off, const std::string& solo, bool isolate, std::string* error);
    std::string effect_layers() const;
    static std::string effect_layer_names();  // every layer's name, comma separated
    void effect_layer_keys(unsigned keys);
    // The FPS counter over the picture (top right; Num * toggles it too).
    void set_fps_counter(bool on);
    // UI thread: V-Sync from the next present (the swap chain allows tearing whenever the system does, so either mode
    // presents without recreating it).
    void set_vsync(bool on) noexcept;
    std::uint64_t far_proxy_draws() const noexcept;
    // How many models have faded in so far.
    std::uint64_t fade_ins() const noexcept;
    // The lifted build's native owners (extract/native-owners-r2.txt) on `cpu`. After start(), before guest code runs.
    void install_owners(Cpu& cpu);

    // The game-visible output size (HW_REPORT J, F1-F3): the aspect the driver's screen tests, 2D layout and lens
    // flare lay out for - the same output System::display() gives the native frustum. The host sets both together,
    // before the game runs and then only at host services (a window's resize is latched there, a tape's output
    // records are applied there), so the game sees the output change at a game tick a replay reproduces. Neither the
    // window (attach / resize) nor a capture request changes it.
    void set_output_aspect(std::uint32_t width, std::uint32_t height);
    // The physical render size in use (set_output_aspect's result); 0 x 0 when none is asked (the window's size).
    std::uint32_t render_width() const noexcept;
    std::uint32_t render_height() const noexcept;
    // The render size asked (LiftedRenderOptions::render_width / render_height) while the game runs, for the current
    // output; the same thread as set_output_aspect (the host services). 0 x 0: the window's size.
    void set_render_size(std::uint32_t width, std::uint32_t height);

    // Before the game runs. The capture's size is its picture's only; the game's output is set_output_aspect's.
    void request_capture(const LiftedCapture& capture);
    // Without a window: the requested captures are rendered as the game finishes those frames (the exact frames).
    void start_headless();

    // UI thread: the swap chain's size (the picture), not the game-visible output (set_output_aspect).
    bool attach(void* hwnd, std::uint32_t width, std::uint32_t height, std::string* error);
    bool resize(std::uint32_t width, std::uint32_t height, std::string* error);
    // UI-thread observation of an attempted render. A timeout is not a frame.
    // Durations are CPU-side wall time (including waits), never GPU execution.
    struct PresentationSample {
        bool attempted = false, success = false, has_interval = false;
        std::uint64_t index = 0, packet_tick = 0, game_ticks = 0;
        std::uint64_t packet_wait_ns = 0, capture_ns = 0, render_present_ns = 0;
        double interval_ms = 0.0;
        std::chrono::steady_clock::time_point finished_at{};
    };
    // Waits up to `wait` for a frame and renders the newest. False only on a renderer failure.
    bool present_newest(std::chrono::milliseconds wait, std::string* error,
                        PresentationSample* sample = nullptr);

    struct Telemetry {
        std::uint64_t presents = 0, attempts = 0, failures = 0;
        std::uint64_t packet_wait_ns = 0, capture_ns = 0, render_present_ns = 0;
        std::uint64_t packets = 0, dropped = 0, pending = 0;
        std::uint64_t draws_last = 0, textures = 0, meshes = 0;
        std::uint64_t skipped_draws = 0, resource_failures = 0;
    };
    // Any thread: coherent presentation totals; independently synchronized
    // queue and resource observations. Does not take the renderer's GPU lock.
    Telemetry telemetry() const;
    // After stop/join: actual presentation counters and histogram bounds.
    std::string presentation_json() const;

    // Counters of the scaffolding, the hand-over and the renderer, as report lines.
    std::string report_text() const;

    // ---- the development link (docs/comparator/README.md) ----
    // Any thread: write the first frame the game finishes at or after `frame` (a packet tick, next_frame()) to `path`
    // (PNG) at width x height, replacing a pending request. Windowed, the frame is captured just before it is
    // presented and without committing the views' histories, so the present is the frame it would have been.
    void arm_capture(const std::string& path, std::uint64_t frame, std::uint32_t width, std::uint32_t height);
    // The same for the first frame the game finishes at or after game tick `game_tick` (the game clock is the one
    // both builds of a comparison share; their frame counters differ).
    void arm_capture_at_tick(const std::string& path, std::uint64_t game_tick, std::uint32_t width,
                             std::uint32_t height);
    struct CaptureResult {
        std::uint64_t written = 0;   // captures written by this process
        std::uint64_t frame = 0;     // the newest one's packet tick
        std::uint64_t game_ticks = 0;  // ... and the game tick its frame was finished at
        std::string path;            // ... and file
        std::string error;           // the newest failure, if any
        bool pending = false;        // a request not served yet
    };
    CaptureResult capture_result() const;
    // Game thread: the packet tick of the frame the game will finish next.
    std::uint64_t next_frame() const;
    // The lifted renderer's savestate block (gekko/savestate.h RendererStateHooks): the driver's state RenderWare
    // observes; after a load the driver's handles and the views' histories are dropped. Game thread.
    void save_state(std::vector<std::uint8_t>& out) const;
    bool load_state(const std::uint8_t*& cursor, const std::uint8_t* end);
    void on_state_loaded();
    void own_words(std::vector<std::uint32_t>& out) const;
    bool frame_open() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace gekko
