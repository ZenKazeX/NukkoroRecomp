#pragma once

// Native window, presentation and input for the runtime.
//
// The window shows the guest external framebuffer, converted from the console
// Y'CbCr 4:2:2 layout to host RGB. Input is collected from the keyboard and from
// XInput pads as samples of the game's controls (peripheral/controls.h).

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "gekko/effect_layer_keys.h"
#include "gekko/plume_present.h"
#include "gekko/types.h"
#include "peripheral/controls.h"

namespace gekko {

class HostWindow {
public:
    HostWindow();
    ~HostWindow();

    // Presents through plume (Direct3D 12) instead of GDI when the build has it.
    // Must be set before create(). The pixels presented are identical either
    // way; only the path to the screen differs.
    void set_plume_presentation(bool enabled) noexcept { want_plume_ = enabled; }
    void set_plume_external_context(void* device, void* queue, void* queue_mutex) noexcept {
        plume_device_ = device;
        plume_queue_ = queue;
        plume_queue_mutex_ = queue_mutex;
    }
    bool plume_active() const noexcept;
    // Terminal native fence error; never normalized into a GDI fallback.
    std::string plume_error() const;
    bool plume_failed() const noexcept;
    // Host-only switch. Disabling XInput does not change the recovered PAD/SI
    // path and leaves manual keyboard input available.
    void set_xinput_enabled(bool enabled) noexcept { xinput_enabled_ = enabled; }
    bool xinput_enabled() const noexcept { return xinput_enabled_; }
    // Diagnostic: read the presented back buffer back and compare it with the
    // pixels handed to present(). Must be set before create().
    void set_plume_verify(bool enabled) noexcept { verify_plume_ = enabled; }
    void set_plume_gpu_blit(bool enabled) noexcept { gpu_blit_plume_ = enabled; }
    void set_plume_area_resolve(bool enabled) noexcept { area_resolve_plume_ = enabled; }
    void set_vsync(bool enabled) noexcept { vsync_ = enabled; }
    void set_plume_present_dump(std::string path, int frame = 30) {
        present_dump_ = std::move(path);
        present_dump_frame_ = frame;
    }
    // Switch, default OFF (D06-F8). On, pump() restores and re-raises the
    // window every iteration while it is iconic or hidden - ~120 ShowWindow /
    // SetWindowPos / SetForegroundWindow calls per second, which is what
    // unattended boot validation wants and what a benchmark with the user at
    // the machine emphatically does not: it steals focus back continuously.
    // Off, a minimised window stays minimised; nothing else changes, and the
    // guest never sees the window state either way.
    void set_keep_foreground(bool enabled) noexcept { keep_foreground_ = enabled; }
    u64 foreground_restores() const noexcept { return foreground_restores_; }
    // Presenter switches, applied at create() (D06-F6, D06-F7). Both must be
    // set before create(); setting them afterwards also takes effect, which is
    // how the release hook is installed once the presenter exists.
    void set_plume_narrow_queue_lock(bool enabled) noexcept;
    void set_plume_deferred_pin_release(bool enabled) noexcept;
    void set_plume_native_release_hook(PlumePresenter::NativeReleaseHook hook,
                                       void* context) noexcept;
    // True when the presenter will take ownership of the native XFB pins handed
    // to the next present_native call, so the caller must NOT release them.
    // Query it immediately before the call, on the presenting thread.
    bool plume_pins_adopted() const noexcept;
    // Hands back the pins of every flight slot whose fence has retired. Cheap
    // and non-blocking; the UI loop calls it once per iteration.
    void plume_retire_completed_presents();
    PlumePresenter::VerifyReport plume_verify_report() const;
    PlumePresenter::PresentStats plume_present_stats() const;

    bool create(int width, int height, const std::string& title, bool fullscreen = false);
    void destroy();
    bool available() const noexcept { return handle_ != nullptr; }
    // The window's native handle (HWND), for a renderer that owns its own swap chain on it.
    void* native_handle() const noexcept { return handle_; }
    void client_size(int& width, int& height) const noexcept;
    bool fullscreen() const noexcept { return fullscreen_; }
    // While the window is open (the display settings, gekko/display_settings.h; UI thread): borderless covering the
    // monitor the window is on, or a windowed client of width x height - shaped by the content aspect and fitted
    // into that monitor's work area like create()'s, centred in it. The swap chain follows through the caller's
    // client-size resize. False without a window.
    bool set_mode(bool fullscreen, int width, int height);

    // Processes pending window messages. Returns false once the user closes it.
    bool pump();

    // Presents a host RGB image. content_aspect is the ratio the pixels should
    // occupy (16:9 after Hor+, 4:3 for native). The image is fitted into the
    // client area; unused regions stay black.
    void present(const u32* pixels, int width, int height, float content_aspect = 0.0f);
    // source_pin / previous_pin name the rasterizer native XFB slots the two
    // textures live in. They are only read when plume_pins_adopted() is true.
    bool present_native(void* texture, void* previous_texture, float blend_alpha,
                        int width, int height, float content_aspect = 0.0f,
                        const PlumePresenter::NativePin* source_pin = nullptr,
                        const PlumePresenter::NativePin* previous_pin = nullptr);

    // Player 1's controller now: the foreground-window keyboard and, when
    // enabled, XInput user 0, combined (peripheral/keyboard_layout.h).
    shadow::peripheral::ControllerSample controller() const;

    bool escape_pressed() const noexcept { return escape_pressed_; }

    // Development savestate hotkeys (gekko/savestate.h). N saves, M loads;
    // neither letter is used by the pad mapping. They are active only while the
    // game window is foreground and edge-triggered inside pump(), so holding the
    // key performs exactly one action. Consumed by take_*() so the caller cannot
    // process the same press twice.
    bool take_save_state_request() noexcept {
        const bool requested = save_state_requested_;
        save_state_requested_ = false;
        return requested;
    }
    bool take_load_state_request() noexcept {
        const bool requested = load_state_requested_;
        load_state_requested_ = false;
        return requested;
    }
    // The marker key (default F8, --marker-key): the user marks "I just heard/saw the problem" - during a tape
    // replay a marker of the replay (docs/tools/REPLAY_MARKERS.md). Same edge and focus rules as N/M. Returns the
    // presses since the last call. A Windows virtual key; 0 disables it.
    void set_marker_key(int virtual_key) noexcept { marker_key_ = virtual_key; }
    unsigned take_user_marks() noexcept {
        const unsigned marks = user_marks_requested_;
        user_marks_requested_ = 0;
        return marks;
    }
    // Pointing at the bug: with click marks on (a tape replay), a left click in the window is a marker too. The last
    // marker's point - the click, or the mouse over the window as the marker key went down - as a fraction of the
    // client area (0..1, from the top left); false when the mouse was elsewhere.
    void set_click_marks(bool enabled) noexcept { click_marks_ = enabled; }
    bool mark_point(double& x, double& y) const noexcept {
        x = mark_x_;
        y = mark_y_;
        return mark_point_valid_;
    }
    // The range key (default F9, --range-key): its presses alternately open and close a marked range of the replay
    // (docs/tools/REPLAY_MARKERS.md "Ranges"). Same edge and focus rules; 0 disables it. Returns the presses since the
    // last call.
    void set_range_key(int virtual_key) noexcept { range_key_ = virtual_key; }
    unsigned take_range_marks() noexcept {
        const unsigned marks = range_marks_requested_;
        range_marks_requested_ = 0;
        return marks;
    }
    // The comparison snapshot key (docs/comparator/README.md "One-button
    // snapshots"; default F5, --snapshot-key): "snapshot this exact moment".
    // Same edge and focus rules as N/M. A Windows virtual key; 0 disables it.
    void set_snapshot_key(int virtual_key) noexcept { snapshot_key_ = virtual_key; }
    // An exact render size's aspect (docs/RESOLUTION_AUDIT.md): set before create, a windowed client is created in
    // that shape (inside the asked size and the work area) and keeps it while resized, so the picture has no bars.
    // 0: any shape.
    static void set_content_aspect(double aspect) noexcept;
    int snapshot_key() const noexcept { return snapshot_key_; }
    // The effect layer keys (gekko/effect_layer_keys.h): fresh presses since the last call, as EffectLayerKey bits.
    unsigned take_effect_keys() noexcept {
        const unsigned keys = effect_keys_;
        effect_keys_ = 0;
        return keys;
    }
    // Returns the presses since the last call.
    unsigned take_snapshot_requests() noexcept {
        const unsigned requests = snapshot_requests_;
        snapshot_requests_ = 0;
        return requests;
    }

private:
    void remember_plume_failure() noexcept;
    void* handle_ = nullptr;
    std::vector<u32> scratch_;
    bool escape_pressed_ = false;
    bool save_state_key_down_ = false;
    bool load_state_key_down_ = false;
    bool save_state_requested_ = false;
    bool load_state_requested_ = false;
    bool user_mark_key_down_ = false;
    int marker_key_ = 0x77;  // VK_F8
    unsigned user_marks_requested_ = 0;
    bool click_marks_ = false;
    bool mark_point_valid_ = false;
    double mark_x_ = 0.0, mark_y_ = 0.0;
    int range_key_ = 0x78;  // VK_F9
    unsigned range_marks_requested_ = 0;
    int snapshot_key_ = 0x74;  // VK_F5
    bool snapshot_key_down_ = false;
    unsigned snapshot_requests_ = 0;
    unsigned effect_keys_ = 0;
    bool want_plume_ = false;
    bool plume_failed_ = false;
    std::string plume_error_;
    PlumePresenter::VerifyReport stopped_plume_verify_report_;
    PlumePresenter::PresentStats stopped_plume_present_stats_;
    void* plume_device_ = nullptr;
    void* plume_queue_ = nullptr;
    void* plume_queue_mutex_ = nullptr;
    bool verify_plume_ = false;
    bool gpu_blit_plume_ = false;
    bool area_resolve_plume_ = true;
    bool vsync_ = true;
    std::string present_dump_;
    int present_dump_frame_ = 30;
    bool xinput_enabled_ = true;
    bool fullscreen_ = false;
    bool keep_foreground_ = false;
    u64 foreground_restores_ = 0;
    bool narrow_present_lock_ = true;
    bool deferred_pin_release_ = true;
    PlumePresenter::NativeReleaseHook native_release_hook_ = nullptr;
    void* native_release_context_ = nullptr;
    std::unique_ptr<PlumePresenter> presenter_;
};

// Converts one console external framebuffer scanline set into host RGB.
// The guest layout is Y'CbCr 4:2:2, two pixels per four bytes.
void decode_xfb(const u8* xfb, u32 width, u32 height, u32 stride, std::vector<u32>& out);

}  // namespace gekko
