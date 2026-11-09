#include "gekko/native_frame.h"
#include "native_memory.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <random>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

using gekko::native::FrameSegment;
using gekko::native::SegmentQueue;

// Frame f's payload: its index up front and, when `grow`, every fifth frame large
// enough to grow a segment's 1 MiB arena, so arena growth is part of what must repeat.
void fill(FrameSegment& segment, gekko::u64 frame, bool grow) {
    gekko::u8* bytes = nullptr;
    segment.reserve_payload(grow && frame % 5u == 3u ? (3u << 20) : 64u, bytes);
    std::memcpy(bytes, &frame, sizeof(frame));
}

struct RingRun {
    std::vector<const FrameSegment*> acquired;  // in acquire order
    gekko::u64 segments_allocated = 0;
    gekko::u64 renderer_allocations = 0;        // Renderer-domain allocations during the run
};

gekko::u64 renderer_allocations() {
    return shadow::memory::system().snapshot().domains[static_cast<std::size_t>(shadow::memory::Domain::Renderer)].allocations;
}

// Single-threaded schedules at the two extremes push() allows: the consumer
// recycles every segment at once, or it keeps the queue full and holds one.
RingRun run_schedule(bool lagging, gekko::u64 frames) {
    const gekko::u64 before = renderer_allocations();
    RingRun run;
    {
        SegmentQueue queue(2);
        std::unique_ptr<FrameSegment> rendering;
        auto segment = queue.acquire();
        run.acquired.push_back(segment.get());
        for (gekko::u64 frame = 0; frame < frames; ++frame) {
            fill(*segment, frame, true);
            if (lagging && queue.depth() == 2u) {
                if (rendering) queue.recycle(std::move(rendering));
                rendering = queue.pop(0);
            }
            queue.push(std::move(segment));
            if (!lagging) queue.recycle(queue.pop(0));
            segment = queue.acquire();
            run.acquired.push_back(segment.get());
        }
        run.segments_allocated = queue.stats().segments_allocated;
        require(queue.stats().producer_waits == 0u, "single-threaded ring schedule blocked");
    }
    run.renderer_allocations = renderer_allocations() - before;
    return run;
}

// Positions, not addresses: slot i is the i-th distinct segment handed out.
std::vector<std::size_t> slots(const RingRun& run) {
    std::vector<const FrameSegment*> seen;
    std::vector<std::size_t> out;
    for (const FrameSegment* segment : run.acquired) {
        std::size_t slot = 0;
        while (slot < seen.size() && seen[slot] != segment) ++slot;
        if (slot == seen.size()) seen.push_back(segment);
        out.push_back(slot);
    }
    return out;
}

void segment_ring_is_timing_independent() {
    constexpr gekko::u64 kFrames = 64;
    const RingRun eager = run_schedule(false, kFrames);
    const RingRun lagging = run_schedule(true, kFrames);
    require(eager.segments_allocated == 4u && lagging.segments_allocated == 4u,
            "segment ring is not exactly capacity + 2 segments");
    const auto order = slots(eager);
    require(order == slots(lagging), "which segment carries a frame depends on the consumer's timing");
    for (std::size_t i = 0; i < order.size(); ++i) {
        require(order[i] == i % 4u, "segment ring does not rotate strictly");
    }
    require(eager.renderer_allocations == lagging.renderer_allocations,
            "renderer-host allocations depend on the consumer's timing");

    // Real threads with jittered pacing on both sides: the ring still never
    // grows, rotates strictly, and delivers every frame intact and in order.
    SegmentQueue queue(2);
    constexpr gekko::u64 kThreadedFrames = 4000;
    std::atomic<bool> intact{true};
    std::thread consumer([&] {
        std::mt19937 jitter(7u);
        for (gekko::u64 expected = 0; expected < kThreadedFrames;) {
            auto segment = queue.pop(50);
            if (!segment) continue;
            gekko::u64 frame = ~0ull;
            std::memcpy(&frame, segment->payload().data(), sizeof(frame));
            if (frame != expected++) intact = false;
            for (unsigned spin = jitter() % 64u; spin != 0u; --spin) std::this_thread::yield();
            queue.recycle(std::move(segment));
        }
    });
    std::mt19937 jitter(11u);
    std::vector<const FrameSegment*> acquired;
    auto segment = queue.acquire();
    acquired.push_back(segment.get());
    for (gekko::u64 frame = 0; frame < kThreadedFrames; ++frame) {
        fill(*segment, frame, false);
        for (unsigned spin = jitter() % 64u; spin != 0u; --spin) std::this_thread::yield();
        queue.push(std::move(segment));
        segment = queue.acquire();
        acquired.push_back(segment.get());
    }
    consumer.join();
    require(intact.load(), "threaded ring delivered a frame out of order");
    require(queue.stats().segments_allocated == 4u, "threaded ring grew");
    for (std::size_t i = 4; i < acquired.size(); ++i) {
        require(acquired[i] == acquired[i - 4u], "threaded ring does not rotate strictly");
    }
}
}  // namespace

int main() {
    try {
        static_assert(sizeof(gekko::native::PayloadRef::offset) == 8);
        static_assert(sizeof(gekko::native::PayloadRef::size) == 8);
        auto& memory = shadow::memory::system();
        const auto baseline = memory.snapshot();
        shadow::memory::Mapping retained;
        {
            auto frame = std::make_unique<gekko::native::FrameSegment>();
            gekko::u8* bytes = nullptr;
            const auto ref = frame->reserve_payload(32u << 20, bytes);
            require(ref.size == (32u << 20), "renderer payload retained a console-size ceiling");
            bytes[ref.size - 1u] = 0x73u;
            require(frame->payload_at(ref)[ref.size - 1u] == 0x73u, "renderer payload does not use native backing");
            require(frame->payload_at({4ull << 30, 1u}) == nullptr, "renderer payload truncated a 64-bit offset");
            require(frame->payload_at({ref.size, 1u}) == nullptr, "renderer accepted out-of-range payload");
            bool rejected = false;
            try { frame->reserve_payload(std::numeric_limits<std::size_t>::max(), bytes); }
            catch (const std::length_error&) { rejected = true; }
            require(rejected, "renderer payload size overflow accepted");
            const auto handle = memory.handle_of(frame->payload().data());
            require(bool(handle), "renderer payload bypassed native ownership");
            retained = memory.map(handle, ref.size - 1u, 1u);
            require(bool(retained), "cannot retain renderer payload allocation");
            require(memory.snapshot().domains[static_cast<std::size_t>(shadow::memory::Domain::Renderer)].active >= (32u << 20),
                    "renderer memory diagnostics omitted payload");
        }
        require(retained.data()[0] == std::byte{0x73}, "native mapping did not outlive packet owner");
        retained = {};
        require(memory.snapshot().total.active == baseline.total.active &&
                memory.snapshot().total.committed == baseline.total.committed,
                "renderer native allocations were not reclaimed");
        std::puts("PASS native renderer payload ownership, 64-bit bounds, and streaming size types");
        segment_ring_is_timing_independent();
        std::puts("PASS segment ring: capacity + 2 segments, strict rotation, allocations independent of thread timing");
        return 0;
    } catch (const std::exception& e) {
        std::printf("FAIL %s\n", e.what());
        return 1;
    }
}
