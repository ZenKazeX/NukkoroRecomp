#pragma once

// Presentation of the guest external framebuffer through plume (Direct3D 12).
//
// This is the first step of moving rendering off the CPU. It changes *nothing*
// about what the guest executes or what the software rasteriser produces: the
// pixels handed to present() are the same ones GDI receives today, decoded from
// the same XFB by the same decode_xfb. Only the path from those pixels to the
// screen differs.
//
// It exists because GPU rasterisation needs a device, a queue and a swap chain,
// and standing those up against unchanged, already-verified pixel data is the
// only step where the output can be compared byte for byte with the existing
// path. Nothing here interprets guest state.
//
// The frame is copied 1:1 into the back buffer and centred; the surrounding area
// is black. GDI stretches to the client area instead, so the two paths match
// exactly when the client area is the frame size, which is how the window is
// created. Restoring arbitrary scaling needs a sampled blit and is deliberately
// not done here.
//
// Resources are triple-buffered so a present does not wait for the GPU except
// to recycle a slot from two frames ago. The swap chain waits on display
// refresh for each completed XFB so the GPU can idle between PAL fields.
// That wait is UI-thread-only and is not the guest clock: VI still paces
// recovered time. Earlier vsync-on blocked Present on every host upload,
// including empty retraces, and stalled the recompiled CPU thread.
//
// When the build has no plume, every method is a no-op and available() is false,
// so HostWindow falls back to the GDI path.

#include <memory>
#include <string>

#include "gekko/types.h"

namespace gekko {

class PlumePresenter {
public:
    PlumePresenter();
    ~PlumePresenter();

    PlumePresenter(const PlumePresenter&) = delete;
    PlumePresenter& operator=(const PlumePresenter&) = delete;

    // Binds to a native window. Returns false when plume is not compiled in or
    // no Direct3D 12 device could be created; the caller keeps using GDI.
    bool create(void* native_window);
    bool create(void* native_window, void* external_device, void* external_queue,
                void* external_queue_mutex);
    // Bounded retirement before disposal. Ambiguous in-flight ownership is a
    // fatal teardown failure; confirmed device removal permits safe disposal.
    void destroy();
    bool available() const noexcept;

    // Uploads one host RGB frame and presents it. Same pixel format GDI takes:
    // 0xAARRGGBB per pixel, top-down. content_aspect describes how those
    // pixels should be fitted into the swap chain (Hor+ 16:9 content still
    // arrives as a 640x480 XFB).
    void present(const u32* pixels, int width, int height, float content_aspect = 0.0f);

    // Native XFB pin hand-back (D06-F7). The caller pins a rasterizer native
    // XFB slot for as long as the presenter reads it. Without a hook the
    // caller releases the pin as soon as present_native returns - before the
    // blit has executed, so a slot recreation could in principle race a
    // queued read. With a hook installed and deferred release on, the
    // presenter takes over every pin it is handed, whichever way the call
    // ends: pins of a present that never reaches the queue are handed
    // straight back, pins of a queued present ride on its flight slot and
    // come back once that slot's fence has retired. The hook runs on the
    // presenting thread only, from present_native, retire_completed_presents
    // and destroy. Install it once, before the first present.
    struct NativePin {
        u32 slot = 0;
        u64 generation = 0;
        bool valid = false;
        // Flipper-signaled ready semaphore for this generation. Present waits
        // on it on the present queue so the blit does not share the Flipper
        // DIRECT queue. Null on the CPU-upload path.
        void* ready_semaphore = nullptr;
    };
    using NativeReleaseHook = void (*)(void* context, u32 slot, u64 generation);
    void set_native_release_hook(NativeReleaseHook hook, void* context) noexcept;
    // Default ON since the NativeXFB ring is 8 slots (2026-09-03). The pin is
    // held until the blit fence retires, which is the correct order against a
    // queued read. Off restores immediate unpin for bisection. A starved
    // reserve is counted (native_xfb_reserve_misses); do not turn this off
    // to hide a slot-count bug.
    void set_deferred_pin_release(bool enabled) noexcept;
    // True when the presenter will take ownership of the pins handed to the
    // NEXT present_native call. Query it immediately before that call, on the
    // presenting thread: the answer cannot change underneath a single caller,
    // and present_native latches the same predicate on entry, so every pin is
    // released exactly once - by the presenter when this is true, by the
    // caller when it is false.
    bool deferred_pin_release_active() const noexcept;
    // Non-blocking: hands back the pins of every flight slot whose fence has
    // already retired. The UI thread calls it once per loop iteration so a
    // pin is held for about one present interval, not for the three presents
    // it takes for the flight slot to be recycled. NativeXFB is 8 slots.
    void retire_completed_presents();

    bool present_native(void* texture, void* previous_texture, float blend_alpha,
                        int width, int height, float content_aspect = 0.0f,
                        const NativePin* source_pin = nullptr,
                        const NativePin* previous_pin = nullptr);

    // Reads the presented back buffer back off the GPU and compares it, pixel
    // for pixel, with what was handed to present(). This is the same kind of
    // differential check the recompiler is held to: the Direct3D 12 path is only
    // known to be correct if its output can be shown to equal the output of the
    // path it replaces. Costs a full surface readback per frame, so it is a
    // diagnostic.
    void set_verify(bool enabled) noexcept;
    // Opt in to the GPU presentation blit (see plume_present.cpp). Must be
    // called before create().
    void set_gpu_blit(bool enabled) noexcept;
    void set_area_resolve(bool enabled) noexcept;
    // Controls DXGI synchronization. Enabled by default; disabling it requests
    // immediate presentation and tearing where the host supports it. Must be
    // called before create().
    void set_vsync(bool enabled) noexcept;
    // Acceptance capture: write the presented back buffer to a PPM once.
    // `frame` is the presented-frame ordinal to capture, 1-based. The old
    // hardcoded 30 skipped the whole speed burst on a --load-state route,
    // so every presented capture landed on a frame with no blur at all.
    void set_present_dump(const char* path, int frame);
    // Switch (default on): hold the shared presentation queue mutex only
    // around executeCommandLists, as the CPU-upload path always has, instead
    // of across the whole native recording (D06-F6). Off restores the wide
    // scope for bisection.
    void set_narrow_queue_lock(bool enabled) noexcept;
    struct VerifyReport {
        u64 frames = 0;
        u64 cpu_upload_frames = 0;
        u64 native_xfb_frames = 0;
        // Presents the oracle could not check because the staging was not 1:1.
        // Non-zero means the run is UNVERIFIED, however clean `frames` looks.
        u64 skipped = 0;
        u64 mismatched_frames = 0;
        u64 mismatched_pixels = 0;
        u32 first_bad_x = 0;
        u32 first_bad_y = 0;
        u32 first_bad_expected = 0;
        u32 first_bad_actual = 0;
    };
    VerifyReport verify_report() const;

    // Present-side instrumentation (D06-F5). Written and read on the
    // presenting thread; every field is a lifetime total.
    struct PresentStats {
        u64 presents = 0;                    // swap-chain Present() calls, both paths
        u64 present_ns = 0;                  // time inside Present(), total
        u64 present_ns_max = 0;
        u64 slot_waits = 0;                  // wait_slot calls that found the slot in flight
        u64 slot_waits_blocked = 0;          // ...of which the fence had not retired on entry
        u64 slot_wait_ns = 0;                // time inside wait_slot's fence loop, total
        u64 slot_wait_ns_max = 0;
        u64 slots_retired_early = 0;         // flight slots retired by the non-blocking poll
        u64 queue_lock_misses = 0;           // try_lock on the shared queue mutex failed
        u64 queue_lock_wait_ns = 0;          // time spent blocking after such a miss
        u64 pins_adopted = 0;                // native pins the presenter took ownership of
        u64 pins_released_deferred = 0;      // handed back after the flight fence retired
        u64 pins_released_immediate = 0;     // handed back because the present was not queued
        u64 presents_skipped_minimised = 0;  // client area 0x0: nothing to present, not a failure
        u64 present_failures = 0;            // swap-chain Present() returned false
        u64 present_failures_consecutive = 0;
        u64 xfb_ready_waits = 0;             // blits that GPU-waited on a Flipper XFB semaphore
    };
    PresentStats present_stats() const;

    // Set when a present failed and the caller should fall back permanently.
    bool failed() const noexcept;
    // Presenting-thread snapshot. Nonempty on terminal fence ownership failure.
    std::string last_error() const;
    bool fence_failed() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace gekko
