#pragma once

// The native renderer: consumes FrameSegments produced by RwCapture on its own
// thread and renders them with persistent Direct3D 12 resources.
// docs/native-runtime-2026-09-16/ARCHITECTURE.md sections 2-3.
//
// Thread ownership:
//   * start()/stop()/report() - the owning (main) thread.
//   * everything else runs on the renderer thread. It reads only segment data;
//     it never touches guest memory or any System object.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include "gekko/native_frame.h"
#include "gekko/native_gpu.h"
#include "gekko/types.h"

namespace gekko {

struct NativeRendererConfig {
    // Frames allowed in flight between the guest and the renderer.
    u32 frames_in_flight = 2;
    // Diagnostic text dump of captured segments.
    std::filesystem::path dump_directory;
    u32 dump_first_frame = 0;
    u32 dump_last_frame = 0;
    // Render through Direct3D 12 (false = capture/count only).
    bool gpu = true;
    NativeGpuSettings gpu_settings;
};

class NativeRenderer {
public:
    NativeRenderer();
    ~NativeRenderer();

    NativeRenderer(const NativeRenderer&) = delete;
    NativeRenderer& operator=(const NativeRenderer&) = delete;

    // Starts the renderer thread. The queue is what RwCapture pushes into.
    bool start(const NativeRendererConfig& config);
    // Locked snapshot; the worker may publish a terminal native error.
    std::string last_error() const;
    // Terminal owner status is retained even if diagnostic text cannot allocate.
    bool failed() const noexcept;
    // The Direct3D 12 backend, or null when rendering is off.
    NativeGpu* gpu() noexcept;
    void stop();
    native::SegmentQueue& queue();

    struct Report {
        u64 segments = 0;
        u64 frames = 0;
        u64 commands = 0;
        u64 payload_bytes = 0;
        u64 dumped_frames = 0;
        u64 process_ns = 0;
        u64 process_ns_max = 0;
        native::SegmentQueue::Stats queue{};
    };
    Report report() const;
    std::string report_text() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace gekko
