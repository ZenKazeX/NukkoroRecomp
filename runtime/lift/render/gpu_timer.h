#pragma once

// GPU frame timing (HW 2026-10-20, roadmap G1; findings-20261019-HW/HW_REPORT.md section I). MEASUREMENT ONLY.
//
// Enabled by the environment variable GEKKO_GPU_TRACE=<csv path>; off by default. When off the renderer never creates
// a GpuTimer: no query heap, no readback buffer, and not one extra call in any command list. When on, every frame gets
// D3D12 timestamps at its beginning, around each kept frame-graph pass and at its end, resolved into the frame slot's
// region of a readback buffer. The results are read when that slot comes round again, right after the fence wait the
// frame loop already makes for it (FrameLoop::begin), so tracing adds no CPU wait. GPU times are converted to
// std::chrono::steady_clock nanoseconds (QueryPerformanceCounter on Windows) through the direct queue's clock
// calibration, re-taken every second, so the rows line up with the presentation trace (GEKKO_PRESENT_TRACE). The rows
// are kept in memory and written once, when the renderer is destroyed (finish).
//
// Not measured here (deferred, HW_REPORT.md I): pipeline-state binds, and the copy queue's uploads.
//
// Per-draw detail (2026-10-30): GEKKO_GPU_TRACE_DRAWS=<csv path>, together with GEKKO_GPU_TRACE, adds a timestamp
// before and after every draw of the draw passes and writes one row per draw for every frame whose GPU time reached
// GEKKO_GPU_TRACE_DRAWS_MIN_MS (default 8). Consecutive draws overlap on the GPU, so a draw's span is an attribution,
// not an isolated cost; the spans of a pass add up to it. Without the variable nothing changes.

#include <d3d12.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "render/frame_graph.h"
#include "render/gpu_device.h"
#include "render/resources.h"

namespace shadow::render {

class GpuTimer final : public PassMarks {
public:
    // The trace file the environment asks for; empty when tracing is off.
    static std::string requested_path();
    static std::int64_t now_ns();  // steady_clock

    bool create(Device& device, std::uint32_t slots, const std::string& path, std::string* error);
    void destroy(Device& device);

    // What the renderer knows about a frame besides its GPU times (read after recording, before the submit).
    struct Counters {
        std::uint64_t draws = 0, immediates = 0, particles = 0;
        std::uint64_t upload_bytes = 0, upload_overflows = 0;
        std::uint64_t transient_descriptors = 0, persistent_descriptors = 0;
        std::uint64_t vram_usage = 0, vram_budget = 0;
    };

    // Right after FrameLoop::begin returned the slot: the slot's previous frame is complete (the frame loop waited
    // for it), so its row is taken first; then the frame-begin mark of the new frame.
    void begin_frame(ID3D12GraphicsCommandList* list, std::uint32_t slot, std::uint64_t frame_number,
                     std::uint64_t packet_tick, std::uint64_t game_ticks, bool to_image);
    // PassMarks: around each kept frame-graph pass (its barriers and clears included).
    void pass_begin(ID3D12GraphicsCommandList* list, const std::string& name) override;
    void pass_end(ID3D12GraphicsCommandList* list) override;
    void note_barriers(std::uint64_t count);
    // Per-draw detail (GEKKO_GPU_TRACE_DRAWS): what a draw was. kind 0 = mesh (a = mesh, b = indices, c = recipe
    // bits), 1 = particle batch (a = particles, b = texture), 2 = immediate (a = vertices, b = indices).
    struct DrawNote {
        std::uint32_t draw = 0;
        std::uint8_t kind = 0;
        std::uint32_t a = 0, b = 0, c = 0;
    };
    bool draw_detail() const noexcept { return open_ && !draws_path_.empty(); }
    void draw_begin(ID3D12GraphicsCommandList* list);
    void draw_end(ID3D12GraphicsCommandList* list, const DrawNote& note);
    // Before the list is closed: the frame-end mark and the resolve into the slot's readback region.
    void end_frame(ID3D12GraphicsCommandList* list, const Counters& counters);
    void on_submitted();
    void on_presented();
    // The GPU is idle (the renderer waited): every frame still pending is taken, then the rows are written (once).
    void finish();

private:
    static constexpr std::uint32_t kPassMarksPerSlot = 256;
    static constexpr std::uint32_t kDrawMarksPerSlot = 16384;
    static constexpr std::uint32_t kNoMark = 0xFFFFFFFFu;
    struct PassSpan {
        std::uint32_t name;  // index into names_
        std::uint32_t begin = kNoMark, end = kNoMark;
    };
    struct DrawSpan {
        std::uint32_t pass;  // index into Pending::passes
        std::uint32_t begin = kNoMark, end = kNoMark;
        DrawNote note;
    };
    struct Pending {
        bool valid = false;
        bool to_image = false;
        std::uint64_t frame = 0, tick = 0, game_ticks = 0;
        std::int64_t cpu_begin = 0, cpu_submit = 0, cpu_present = 0;
        std::uint32_t begin_mark = kNoMark, end_mark = kNoMark;
        std::uint32_t marks = 0;  // timestamps written into the slot's region
        bool overflow = false;    // a mark the region had no room for (dropped)
        std::uint64_t barriers = 0;
        std::vector<PassSpan> passes;
        std::vector<DrawSpan> draws;
        Counters counters;
    };
    struct DrawRow {
        std::uint64_t frame, tick;
        std::uint32_t pass_name;
        DrawNote note;
        std::uint64_t ticks;
    };
    struct Row {
        std::uint64_t frame, tick, game_ticks;
        bool to_image;
        std::uint32_t slot;
        std::int64_t cpu_begin, cpu_submit, cpu_present;
        std::int64_t gpu_begin, gpu_end;  // steady_clock ns; 0 when the mark is missing
        std::uint64_t gpu_ticks;          // end - begin in GPU timestamp ticks
        std::uint64_t passes, barriers;
        bool overflow;
        Counters counters;
        std::vector<std::pair<std::uint32_t, std::uint64_t>> pass_ticks;  // (name, ticks)
    };
    std::uint32_t mark(ID3D12GraphicsCommandList* list);
    void collect(std::uint32_t slot);
    void calibrate();
    std::int64_t gpu_to_ns(std::uint64_t gpu) const;

    Com<ID3D12QueryHeap> heap_;
    GpuBuffer readback_;
    ID3D12CommandQueue* queue_ = nullptr;
    std::uint64_t gpu_frequency_ = 0;
    std::int64_t qpc_frequency_ = 0;
    std::uint64_t calib_gpu_ = 0;
    std::int64_t calib_ns_ = 0;
    std::int64_t last_calibration_ns_ = 0;
    std::vector<Pending> pending_;
    std::uint32_t slot_ = 0;
    bool open_ = false;
    std::map<std::string, std::uint32_t> name_ids_;
    std::vector<std::string> names_;
    std::string path_;
    std::vector<Row> rows_;
    std::uint32_t marks_per_slot_ = kPassMarksPerSlot;
    std::string draws_path_;
    double draws_min_ms_ = 8.0;
    std::vector<DrawRow> draw_rows_;
    bool finished_ = false;
};

}  // namespace shadow::render
