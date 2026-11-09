#pragma once

// GPU memory trace (HW 2026-10-20, HW_REPORT L.4). MEASUREMENT ONLY.
//
// Enabled by the environment variable GEKKO_GPU_MEMORY=<csv path>; off by default. When off nothing is created,
// nothing is named, nothing is written. When on:
//   * <path>              one row per second of wall time (render thread, after a frame's submit) and one at the
//                         renderer's destroy: D3D12MA's statistics per heap type (DEFAULT / UPLOAD / READBACK /
//                         CUSTOM / GPU_UPLOAD) and per segment group, the budget (QueryVideoMemoryInfo usage, which
//                         also counts what the runtime and the driver hold outside D3D12MA: command allocators,
//                         descriptor heaps, pipelines), the CPU-mapped bytes, and the renderer's counters (copy
//                         command allocators created / spare / in flight, upload ring waits, dedicated uploads,
//                         pending released objects, persistent descriptors);
//   * <path>.maps.csv     every persistent CPU mapping of a GPU buffer as it is made and released, with its CPU
//                         address range, so a process memory census (ES resmon) can label each write-combined
//                         allocation base as the renderer's or not;
//   * <path>.<n>.json     D3D12MA's detailed map (every block, every allocation by name) at the first row, at every
//                         rise of the non-local usage by 32 MiB or more since the last map, and at destroy (at most
//                         32 maps).
// Allocations are named (D3D12MA SetName) only while the trace is on.

#include <cstdint>
#include <memory>
#include <string>

namespace shadow::render {

class Device;

namespace gpu_memory {
// The trace file the environment asks for (read once); empty when off.
const std::string& path();
inline bool enabled() { return !path().empty(); }
// A buffer's persistent CPU mapping and its end (any thread; no-ops when off).
void note_map(const wchar_t* name, const void* cpu, std::uint64_t bytes);
void note_unmap(const void* cpu);
}  // namespace gpu_memory

class GpuMemoryTrace {
public:
    struct Counters {
        std::uint64_t frame = 0;
        std::uint64_t copy_allocators_created = 0;
        std::uint64_t copy_allocators_spare = 0;
        std::uint64_t copy_submissions_in_flight = 0;
        std::uint64_t upload_submissions = 0;
        std::uint64_t upload_staged_bytes = 0;
        std::uint64_t upload_ring_waits = 0;
        std::uint64_t upload_dedicated = 0;
        std::uint64_t retired_pending = 0;
        std::uint64_t persistent_descriptors = 0;
    };

    GpuMemoryTrace();
    ~GpuMemoryTrace();
    // Render thread, after a frame's submit: a row once a second.
    void tick(Device& device, const Counters& counters);
    // The renderer's destroy (GPU idle): the last row and the last map.
    void finish(Device& device, const Counters& counters);

private:
    struct State;
    void row(Device& device, const Counters& counters, bool force_map, const char* why);
    std::unique_ptr<State> state_;
};

}  // namespace shadow::render
